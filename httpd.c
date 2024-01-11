/*
 * Copyright 2022 zhenwei pi
 *
 * Authors:
 *   zhenwei pi <pizhenwei@bytedance.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/tcp.h>
#include <linux/types.h>
#include <netdb.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#include "httpd.h"
#include "output.h"

#include "version.h"

#define DEFAULT_PORT		2867
#define DEFAULT_TLS_PORT	2868
#define DEFAULT_LOG_PATH	"/var/log/atop"

#define DEFAULT_CERT_FILE	"/etc/pki/atophttpd/server.crt"
#define DEFAULT_KEY_FILE	"/etc/pki/atophttpd/server.key"
#define DEFAULT_CA_FILE		"/etc/pki/CA/ca.crt"

static void http_show_samp_done(struct output *op, connection *conn);

struct output defop = {
	.output_type = OUTPUT_BUF,
	.done = http_show_samp_done
};

static struct atophttd_context config = {
	.port = DEFAULT_PORT,
        .daemonmode = 0,
	.log_path = DEFAULT_LOG_PATH,
	.addr = "127.0.0.1",

	.tls_ctx_config.tls_port = -1,
	.tls_ctx_config.tls_addr = "::*",
	.tls_ctx_config.ca_cert_file = DEFAULT_CA_FILE,
	.tls_ctx_config.cert_file = DEFAULT_CERT_FILE,
	.tls_ctx_config.key_file = DEFAULT_KEY_FILE,
};


unsigned int pagesize;
unsigned short hertz;
struct utsname utsname;
int hidecmdline = 0;

#define INBUF_SIZE	4096
#define URL_LEN		1024
/* HTTP codes */
static char *http_200 = "HTTP/1.1 200 OK\r\n";
static char *http_404 = "HTTP/1.1 404 Not Found\r\n";

/* HTTP content types */
static char *http_content_type_none = "";
static char *http_content_type_deflate = "Content-Encoding: deflate\r\n";

/* HTTP generic header */
static char *http_generic = "Server: atop\r\n"
"%s"	/* for http_content_type_XXX */
"Content-Type: %s; charset=utf-8\r\n"
"Content-Length: %d\r\n\r\n";

/* HTTP content types */
static char *http_content_type_html = "text/html";
static char *http_content_type_css = "text/css";
static char *http_content_type_javascript = "application/javascript";

static int http_prepare_response(connection *conn)
{
	/* try to send response data in a timeout */
	int onoff = 1;
	int ret;
	ret = setsockopt(conn->fd, IPPROTO_TCP, TCP_NODELAY, &onoff, sizeof(onoff));
	if (ret) {
		printf("failed to set socket to TCP_NODELAY\n");
		return ret;
	}

	ret = fcntl(conn->fd, F_SETFL, fcntl(conn->fd, F_GETFL) & ~O_NONBLOCK);
	if (ret) {
		printf("failed to set conn to blocking\n");
		return ret;
	}

	struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
	ret = setsockopt(conn->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	if (ret) {
		printf("failed to set socket to SNDTIMEO\n");
		return ret;
	}

	return 0;
}

static void http_response_200(connection *conn, char *buf, size_t len, char *encoding, char* content_type)
{
	struct iovec iovs[3], *iov;
	int ret;
	char content[128] = {0};
	int content_length = 0;

	ret = http_prepare_response(conn);
	if (ret) {
		goto closeconn;
	}

	/* 1, http code */
	iov = &iovs[0];
	iov->iov_base = http_200;
	iov->iov_len = strlen(http_200);

	/* 2, http generic content */
	iov = &iovs[1];

	content_length = sprintf(content, http_generic, encoding, content_type, len);
	iov->iov_base = content;
	iov->iov_len = content_length;

	/* 3, sample data record */
	iov = &iovs[2];
	iov->iov_base = buf;
	iov->iov_len = len;

	conn_writev(conn, iovs, sizeof(iovs) / sizeof(iovs[0]));

closeconn:
	conn_close(conn);
}

static void http_show_samp_done(struct output *op, connection *conn)
{
	if (op->encoding == http_content_type_none) {
		http_response_200(conn, op->ob.buf, op->ob.offset, op->encoding, http_content_type_html);
		return;
	}

	/* compress data for encoding deflate */
	char *compbuf = malloc(op->ob.offset);
	unsigned long complen = op->ob.offset;

	if (compress((Bytef *)compbuf, &complen, (Bytef *)op->ob.buf, op->ob.offset) != Z_OK){
		http_prepare_response(conn);
		conn_write(conn, http_404, strlen(http_404));
	}

	http_response_200(conn, compbuf, complen, http_content_type_deflate, http_content_type_html);
	free(compbuf);
}

static int http_arg_long(char *req, char *needle, long *l)
{
	char *s = strstr(req, needle);
	if (!s)
		return -1;

	s += strlen(needle);
	if (*s != '=')
		return -1;

	*l = atol(++s);

	return 0;
}

static int http_arg_str(char *req, char *needle, char *p, int len)
{
        char *s = strstr(req, needle);
        if (!s)
                return -1;

        s += strlen(needle);
        if (*s++ != '=')
                return -1;

        char *e = strchr(s, '&');
        if (!e)
                e = s + strlen(s);

	if (e - s > len)
		return -1;

        memcpy(p, s, e - s);
        p[e - s] = '\0';
	while ((p = strstr(p, "%2C")) != NULL)
	{
		*(p++) = ',';
		memmove(p, p + 2, strlen(p));
	}

        return 0;
}

static void http_showsamp(char *req, connection *conn)
{
	time_t timestamp = 0;
	char lables[1024];
	char encoding[16];

	if (http_arg_long(req, "timestamp", &timestamp) < 0) {
		char *err = "missing timestamp\r\n";
		http_response_200(conn, err, strlen(err), http_content_type_none, http_content_type_html);
		return;
	}

	if (http_arg_str(req, "lables", lables, sizeof(lables)) < 0) {
		char *err = "missing lables\r\n";
		http_response_200(conn, err, strlen(err), http_content_type_none, http_content_type_html);
		return;
	}

	defop.encoding = http_content_type_deflate;
	if (http_arg_str(req, "encoding", encoding, sizeof(encoding)) == 0) {
		if (!strcmp(encoding, "none")) {
			defop.encoding = http_content_type_none;
		} else if (!strcmp(encoding, "deflate")) {
			defop.encoding = http_content_type_deflate;
		} else {
			char *err = "encoding supports none/deflate only\r\n";
			http_response_200(conn, err, strlen(err), http_content_type_none, http_content_type_html);
			return;
		}
	}

	if (rawlog_get_record(timestamp, lables, conn) < 0) {
		char *err = "missing sample\r\n";
		http_response_200(conn, err, strlen(err), http_content_type_none, http_content_type_html);
		return;
	}
}

/* Import a binary file */
#define IMPORT_BIN(sect, file, sym) asm (	\
	".section " #sect "\n"			\
	".global " #sym "\n"			\
	".global " #sym "_end\n"		\
	#sym ":\n"				\
	".incbin \"" file "\"\n"		\
	#sym "_end:\n"				\
	".section \".text\"\n")

/* Build help.html into atop binary */
IMPORT_BIN(".rodata", "http/help.html", help);
extern char help[], help_end[];

static void http_help(connection *conn)
{
	http_response_200(conn, help, help_end - help, http_content_type_none, http_content_type_html);
}

/* Build favicon.ico into atop binary */
IMPORT_BIN(".rodata", "http/favicon.ico", favicon);
extern char favicon[], favicon_end[];

static void http_favicon(connection *conn)
{
	http_response_200(conn, favicon, favicon_end - favicon, http_content_type_none, http_content_type_html);
}

/* Build index.html into atop binary */
IMPORT_BIN(".rodata", "http/index.html", http_index_html);
extern char http_index_html[], http_index_html_end[];

static void http_index(connection *conn)
{
	http_response_200(conn, http_index_html, http_index_html_end - http_index_html, http_content_type_none, http_content_type_html);
}

/* Build atop.js into atop binary */
IMPORT_BIN(".rodata", "http/js/atop.js", atop_js);
extern char atop_js[], atop_js_end[];

static void http_get_atop_js(connection *conn)
{
	http_response_200(conn, atop_js, atop_js_end - atop_js, http_content_type_none, http_content_type_javascript);
}

/* Build atop_parse.js into atop binary */
IMPORT_BIN(".rodata", "http/js/atop_parse.js", atop_parse_js);
extern char atop_parse_js[], atop_parse_js_end[];

static void http_get_atop_parse_js(connection *conn)
{
	http_response_200(conn, atop_parse_js, atop_parse_js_end - atop_parse_js, http_content_type_none, http_content_type_javascript);
}

/* Build atop_compare_fc.js into atop binary */
IMPORT_BIN(".rodata", "http/js/atop_compare_fc.js", atop_compare_fc_js);
extern char atop_compare_fc_js[], atop_compare_fc_js_end[];

static void http_get_atop_compare_fc_js(connection *conn)
{
	http_response_200(conn, atop_compare_fc_js, atop_compare_fc_js_end - atop_compare_fc_js, http_content_type_none, http_content_type_javascript);
}

/* Build atop.css into atop binary */
IMPORT_BIN(".rodata", "http/css/atop.css", http_css);
extern char http_css[], http_css_end[];

static void http_get_css(connection *conn)
{
	http_response_200(conn, http_css, http_css_end - http_css, http_content_type_none, http_content_type_css);
}

/* Build template.html into atop binary */
IMPORT_BIN(".rodata", "http/template/html/header.html", header_html_template);
extern char header_html_template[], header_html_template_end[];

IMPORT_BIN(".rodata", "http/template/html/generic.html", generic_html_template);
extern char generic_html_template[], generic_html_template_end[];

IMPORT_BIN(".rodata", "http/template/html/memory.html", memory_html_template);
extern char memory_html_template[], memory_html_template_end[];

IMPORT_BIN(".rodata", "http/template/html/disk.html", disk_html_template);
extern char disk_html_template[], disk_html_template_end[];

IMPORT_BIN(".rodata", "http/template/html/command_line.html", command_line_html_template);
extern char command_line_html_template[], command_line_html_template_end[];

static void http_get_template_header(connection *conn)
{
	http_response_200(conn, header_html_template, header_html_template_end - header_html_template, http_content_type_none, http_content_type_html);
}

static void http_get_template(char *req, connection *conn)
{
	char template_type[256];
	if (http_arg_str(req, "type", template_type, sizeof(template_type)) < 0)
		return;

	if (!strcmp(template_type, "generic")) {
		http_response_200(conn, generic_html_template, generic_html_template_end - generic_html_template, http_content_type_none, http_content_type_html);
	} else if (!strcmp(template_type, "memory")) {
		http_response_200(conn, memory_html_template, memory_html_template_end - memory_html_template, http_content_type_none, http_content_type_html);
	} else if (!strcmp(template_type, "disk")) {
		http_response_200(conn, disk_html_template, disk_html_template_end - disk_html_template, http_content_type_none, http_content_type_html);
	} else if (!strcmp(template_type, "command_line")) {
		http_response_200(conn, command_line_html_template, command_line_html_template_end - command_line_html_template, http_content_type_none, http_content_type_html);
	} else {
		http_prepare_response(conn);
		conn_write(conn, http_404, strlen(http_404));
	}
}

static void http_ping(connection *conn)
{
	char *pong = "pong\r\n";

	http_response_200(conn, pong, strlen(pong), http_content_type_none, http_content_type_html);
}

static void http_process_request(char *req, connection *conn)
{
	char location[URL_LEN] = {0};
	char *c;

	if (strlen(req) > URL_LEN) {
		http_prepare_response(conn);
		conn_write(conn, http_404, strlen(http_404));
		return;
	}

	c = strchr(req, '?');
	if (c)
		memcpy(location, req, c - req);
	else
		memcpy(location, req, strlen(req));

	if (strlen(location) == 0) {
		http_index(conn);
		return;
	}

	if (!strcmp(location, "ping"))
		http_ping(conn);
	else if (!strcmp(location, "help"))
		http_help(conn);
	else if (!strcmp(location, "favicon.ico"))
		http_favicon(conn);
	else if (!strcmp(location, "showsamp"))
		http_showsamp(req, conn);
	else if (!strcmp(location, "index.html"))
		http_index(conn);
	else if (!strcmp(location, "js/atop.js"))
		http_get_atop_js(conn);
	else if (!strcmp(location, "js/atop_parse.js"))
		http_get_atop_parse_js(conn);
	else if (!strcmp(location, "js/atop_compare_fc.js"))
		http_get_atop_compare_fc_js(conn);
	else if (!strcmp(location, "css/atop.css"))
		http_get_css(conn);
	else if (!strcmp(location, "template_header"))
		http_get_template_header(conn);
	else if (!strcmp(location, "template"))
		http_get_template(req, conn);
	else {
		http_prepare_response(conn);
		conn_write(conn, http_404, strlen(http_404));
	}
}

static time_t httpd_now_ms()
{
	struct timeval now;

	gettimeofday(&now, NULL);

	return now.tv_sec * 1000 + now.tv_usec / 1000;
}

static void httpd_handle_request(connection *conn) {
	char inbuf[INBUF_SIZE] = {0};
	int inbytes = 0;
	char httpreq[URL_LEN] = {0};
	time_t timeout = httpd_now_ms() + 100;
	int ret;
	char *httpver;

	ret = fcntl(conn->fd, F_SETFL, fcntl(conn->fd, F_GETFL) | O_NONBLOCK);
	if (ret) {
		printf("failed to set conn to non_blocking");
		goto close_fd;
	}

	for ( ; ; ) {
		time_t now = httpd_now_ms();
		if (now >= timeout)
			goto close_fd;

		struct pollfd pfd = {.fd = conn->fd, .events = POLLIN, .revents = 0};
		ret = poll(&pfd, 1, timeout - now);
		if (ret <= 0)
		{
			if (ret != 0) {
				goto close_fd;
			}
			continue;
		}

		ret = conn_read(conn, inbuf + inbytes, sizeof(inbuf) - inbytes);
		if (ret < 0)
		{
			if (ret != -EAGAIN) {
				goto close_fd;
			}
			continue;
		}

		inbytes += ret;
		if ((inbytes >= 4) && strstr(inbuf, "\r\n\r\n"))
			break;

		/* buf is full, but we can not search the end of HTTP header */
		if (inbytes == sizeof(inbuf))
			goto close_fd;
	}

	/* support GET request only */
	if (strncmp("GET ", inbuf, 4))
		goto close_fd;

	/* support HTTP 1.1 request only */
	httpver = strstr(inbuf, "HTTP/1.1");
	if (!httpver)
		goto close_fd;

	/* Ex, GET /hello HTTP/1.1 */
	if ((httpver - inbuf > URL_LEN + 6) || (httpver - inbuf < 6))
		goto close_fd;

	memcpy(httpreq, inbuf + 5, httpver - inbuf - 6);
	http_process_request(httpreq, conn);

close_fd:
	conn_close(conn);
	return;
}

static void httpd_update_cache(char *log_path)
{
	static time_t update;
	time_t now = time(NULL);

	if (now - update < 3)
		return;

	if (rawlog_parse_all(log_path)) {
		printf("%s: rawlog parse failed\n", __func__);
	}

	update = now;
}

static void *httpd_routine(connection **listeners, char *log_path)
{
	int epollfd;
	int ret = 0;
	int nr_listener = 0;
	connection *listener;
	struct epoll_event event;

	epollfd = epoll_create1(0);
	if (epollfd < 0) {
		printf("Failed create epollfd\n");
		exit(1);
	}

	for (int i = 0; i < CONN_TYPE_MAX; i++) {
		listener = listeners[i];
		if (listener == NULL) {
			continue;
		}

		ret = conn_listen(listener);
		if (ret < 0) {
			printf("ConnectionType %s failed listening on port %u, aborting.\n", listener->type->get_type(NULL), listener->port);
			exit(1);
		}

		event.events = EPOLLIN;
		event.data.ptr = listener;
		if (epoll_ctl(epollfd, EPOLL_CTL_ADD, listener->fd, &event)) {
			printf("Add listener into epoll failed\n");
			exit(1);
		}
		nr_listener++;
	}

	if (nr_listener == 0) {
		printf("Listen Nothing, Exit.\n");
		exit(1);
	}

	printf("Ready to serve\n");
	while (1) {
		ret = epoll_wait(epollfd, &event, 1, 1000);
		if (!ret) {
			continue;
		}

		if (ret < 0) {
			if (errno == EINTR) {
				continue;
			} else {
				printf("Error in epoll_wait %m\n");
				exit(1);
			}
		}

		listener = event.data.ptr;
		connection *conn;
		conn = conn_create(listener->type, -1, NULL);
		ret = conn_accept(listener, conn);
		if (ret < 0) {
			conn_close(conn);
			free(conn);
			continue;
		}

		httpd_handle_request(conn);
		httpd_update_cache(log_path);
		free(conn);
	}

	return NULL;
}

static int httpd(struct atophttd_context ctx)
{
	signal(SIGPIPE, SIG_IGN);
	connection *listener;
	int conn_index, ret;

	if (ctx.port > 0) {
		conn_index = get_conntype_index_by_name(CONN_TYPE_SOCKET);
		if (conn_index < 0) {
			printf("Failed finding connectin listener of %s\n", CONN_TYPE_SOCKET);
			exit(1);
		}
		listener = conn_create(get_conntype_by_name(CONN_TYPE_SOCKET), ctx.port, ctx.addr);
		ctx.listeners[conn_index] = listener;
	}

	if (ctx.tls_ctx_config.tls_port > 0) {
		conn_index = get_conntype_index_by_name(CONN_TYPE_TLS);
		if (conn_index < 0) {
			exit(1);
		}
		listener = conn_create(get_conntype_by_name(CONN_TYPE_TLS), ctx.tls_ctx_config.tls_port, ctx.tls_ctx_config.tls_addr);

		ret = conn_configure(get_conntype_by_name(CONN_TYPE_TLS), &ctx.tls_ctx_config, 1);
		if (ret < 0) {
			printf("Failed to configure TLS\n");
			exit(1);
		}

		ctx.listeners[conn_index] = listener;
	}

        if (ctx.daemonmode)
                daemon(0, 0);

	httpd_routine(ctx.listeners, ctx.log_path);
	return 0;
}

int __debug = 0;
static char *short_opts = "dDhHp:a:P:t::A:C:c:k:V";

static struct option long_opts[] = {
	{ "daemon",		no_argument,		0,	'd' },
	{ "debug",		no_argument,		0,	'D' },
	{ "port",		required_argument,	0,	'p' },
	{ "addr",		required_argument,	0,	'a' },
	{ "path",		required_argument,	0,	'P' },
	{ "tls-port",		optional_argument,	0,	't'},
	{ "tls-addr",		required_argument,	0,	'A'},
	{ "ca-cert-file",	required_argument,	0,	'C' },
	{ "cert-file",		required_argument,	0,	'c' },
	{ "key-file",		required_argument,	0,	'k' },
	{ "help",		no_argument,		0,	'h' },
	{ "hide-cmdline",	no_argument,		0,	'H' },
	{ "version",		no_argument,		0,	'V' },
	{ 0,			0,			0,	0   }
};

static void httpd_showhelp(void)
{
	printf("Usage:\n");
	printf("  -d/--daemon         \n    run in daemon mode\n");
	printf("  -D/--debug          \n    run with debug message\n");
	printf("  -p/--port PORT      \n    listen to PORT, default %d\n", DEFAULT_PORT);
	printf("  -a/--addr ADDR      \n    bind to ADDR, default bind local host\n");
	printf("  -P/--path PATH      \n    atop log path, default %s\n", DEFAULT_LOG_PATH);
	printf("  -t/--tls-port PORT  \n    listen to TLS PORT, default %d\n", DEFAULT_TLS_PORT);
	printf("  -A/--tls-addr ADDR  \n    bind to TLS ADDR, default bind * (all addresses)\n");
	printf("  -C/--ca-cert-file PATH\n    Path to the server TLS trusted CA cert file, default %s\n", DEFAULT_CA_FILE);
	printf("  -c/--cert-file PATH \n    Path to the server TLS cert file, default %s\n", DEFAULT_CERT_FILE);
	printf("  -k/--key-file PATH  \n    Path to the server TLS key file, default %s\n", DEFAULT_KEY_FILE);
	printf("  -H/--hide-cmdline   \n    hide cmdline for security protection\n");
	printf("  -h/--help           \n    show help\n\n");
	printf("  maintained by       \n    zhenwei pi<pizhenwei@bytedance.com> (HTTP backend)\n");
	printf("                            enhua zhou<zhouenhua@bytedance.com> (HTTP frontend)\n");
	exit(0);
}

static void httpd_showversion(void)
{
	printf("Version: %s\n", ATOPVERS);
	exit(0);
}

int main(int argc, char *argv[])
{
	int args, errno;
	int ch;

	while (1) {
		ch = getopt_long(argc, argv, short_opts, long_opts, &args);
		if (ch == -1) {
			break;
		}
		switch (ch) {
			case 0:
				break;
			case 'd':
				config.daemonmode = 1;
				break;
			case 'D':
				__debug = 1;
				break;
			case 'p':
				config.port = atoi(optarg);
				break;
			case 'a':
				config.addr = optarg;
				break;
			case 'P':
				config.log_path = optarg;
				break;
			case 't':
				if (optarg)
					config.tls_ctx_config.tls_port = atoi(optarg);
				else
					config.tls_ctx_config.tls_port = DEFAULT_TLS_PORT;
				break;
			case 'A':
				config.tls_ctx_config.tls_addr = optarg;
				break;
			case 'C':
				config.tls_ctx_config.ca_cert_file = optarg;
				break;
			case 'c':
				config.tls_ctx_config.cert_file = optarg;
				break;
			case 'k':
				config.tls_ctx_config.key_file = optarg;
				break;
			case 'H':
				hidecmdline = 1;
				break;
			case 'V':
				httpd_showversion();
			case 'h':
			default:
				httpd_showhelp();
		}
	}

	pagesize = sysconf(_SC_PAGESIZE);
	hertz = sysconf(_SC_CLK_TCK);
	uname(&utsname);

	if (config.log_path == NULL) {
		printf("%s: log path is nil\n", __func__);
		return -1;
	}

	if (rawlog_parse_all(config.log_path)) {
		printf("%s: rawlog parse failed\n", __func__);
		return -1;
	}

	log_debug("%s runs with log path(%s), port(%d)\n", argv[0], config.log_path, config.port);

	printf("%s runs with log path(%s), port(%d)\n", argv[0], config.log_path, config.port);
	conntype_initialize();
	errno = httpd(config);

	return errno;
}
