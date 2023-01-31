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
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#include "httpd.h"
#include "output.h"

#include "version.h"

static void http_show_samp_done(struct output *op);

struct output defop = {
	.output_type = OUTPUT_BUF,
	.done = http_show_samp_done
};

unsigned int pagesize;
unsigned short hertz;
struct utsname utsname;

#define INBUF_SIZE	4096
#define URL_LEN		1024

static int clifd = -1;

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

static void http_prepare_response()
{
	/* try to send response data in a timeout */
	int onoff = 1;
	setsockopt(clifd, IPPROTO_TCP, TCP_NODELAY, &onoff, sizeof(onoff));

	fcntl(clifd, F_SETFL, fcntl(clifd, F_GETFL) & ~O_NONBLOCK);

	struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
	setsockopt(clifd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

}

static void http_response_200(char *buf, size_t len, char *encoding, char* content_type)
{
	struct iovec iovs[3], *iov;

	http_prepare_response();

	/* 1, http code */
	iov = &iovs[0];
	iov->iov_base = http_200;
	iov->iov_len = strlen(http_200);

	/* 2, http generic content */
	iov = &iovs[1];
	char content[128] = {0};
	int content_length = 0;

	content_length = sprintf(content, http_generic, encoding, content_type, len);
	iov->iov_base = content;
	iov->iov_len = content_length;

	/* 3, sample data record */
	iov = &iovs[2];
	iov->iov_base = buf;
	iov->iov_len = len;

	writev(clifd, iovs, sizeof(iovs) / sizeof(iovs[0]));

	close(clifd);
}

static void http_show_samp_done(struct output *op)
{
	if (op->encoding == http_content_type_none) {
		http_response_200(op->ob.buf, op->ob.offset, op->encoding, http_content_type_html);
		return;
	}

	/* compress data for encoding deflate */
	char *compbuf = malloc(op->ob.offset);
	unsigned long complen = op->ob.offset;

	if (compress((Bytef *)compbuf, &complen, (Bytef *)op->ob.buf, op->ob.offset) != Z_OK){
		http_prepare_response();
		write(clifd, http_404, strlen(http_404));
	}

	http_response_200(compbuf, complen, http_content_type_deflate, http_content_type_html);
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

static void http_showsamp(char *req)
{
	time_t timestamp = 0;
	char lables[1024];
	char encoding[16];

	if (http_arg_long(req, "timestamp", &timestamp) < 0) {
		char *err = "missing timestamp\r\n";
		http_response_200(err, strlen(err), defop.encoding, http_content_type_html);
		return;
	}

	if (http_arg_str(req, "lables", lables, sizeof(lables)) < 0) {
		char *err = "missing lables\r\n";
		http_response_200(err, strlen(err), http_content_type_none, http_content_type_html);
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
			http_response_200(err, strlen(err), http_content_type_none, http_content_type_html);
			return;
		}
	}

	if (rawlog_get_record(timestamp, lables) < 0) {
		char *err = "missing sample\r\n";
		http_response_200(err, strlen(err), http_content_type_none, http_content_type_html);
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

/* Build favicon.ico into atop binary */
IMPORT_BIN(".rodata", "http/favicon.ico", favicon);
extern char favicon[], favicon_end[];

static void http_favicon()
{
	http_response_200(favicon, favicon_end - favicon, http_content_type_none, http_content_type_html);
}

/* Build index.html into atop binary */
IMPORT_BIN(".rodata", "http/index.html", http_index_html);
extern char http_index_html[], http_index_html_end[];

static void http_index()
{
	http_response_200(http_index_html, http_index_html_end - http_index_html, http_content_type_none, http_content_type_html);
}

/* Build atop.js into atop binary */
IMPORT_BIN(".rodata", "http/js/atop.js", http_js);
extern char http_js[], http_js_end[];

static void http_get_js()
{
	http_response_200(http_js, http_js_end - http_js, http_content_type_none, http_content_type_javascript);
}

/* Build atop.css into atop binary */
IMPORT_BIN(".rodata", "http/css/atop.css", http_css);
extern char http_css[], http_css_end[];

static void http_get_css()
{
	http_response_200(http_css, http_css_end - http_css, http_content_type_none, http_content_type_css);
}

/* Build template.html into atop binary */
IMPORT_BIN(".rodata", "http/template/html/generic.html", generic_html_template);
extern char generic_html_template[], generic_html_template_end[];

IMPORT_BIN(".rodata", "http/template/html/memory.html", memory_html_template);
extern char memory_html_template[], memory_html_template_end[];

IMPORT_BIN(".rodata", "http/template/html/disk.html", disk_html_template);
extern char disk_html_template[], disk_html_template_end[];

IMPORT_BIN(".rodata", "http/template/html/command_line.html", command_line_html_template);
extern char command_line_html_template[], command_line_html_template_end[];

static void http_get_template(char *req)
{
	char template_type[256];
	if (http_arg_str(req, "type", template_type, sizeof(template_type)) < 0)
		return;

	if (!strcmp(template_type, "generic")) {
		http_response_200(generic_html_template, generic_html_template_end - generic_html_template, http_content_type_none, http_content_type_html);
	} else if (!strcmp(template_type, "memory")) {
		http_response_200(memory_html_template, memory_html_template_end - memory_html_template, http_content_type_none, http_content_type_html);
	} else if (!strcmp(template_type, "disk")) {
		http_response_200(disk_html_template, disk_html_template_end - disk_html_template, http_content_type_none, http_content_type_html);
	} else if (!strcmp(template_type, "command_line")) {
		http_response_200(command_line_html_template, command_line_html_template_end - command_line_html_template, http_content_type_none, http_content_type_html);
	} else {
		http_prepare_response();
		write(clifd, http_404, strlen(http_404));
	}
}

static void http_ping()
{
	char *pong = "pong\r\n";

	http_response_200(pong, strlen(pong), http_content_type_none, http_content_type_html);
}

static void http_process_request(char *req)
{
	char location[URL_LEN] = {0};
	char *c;

	c = strchr(req, '?');
	if (c)
		memcpy(location, req, c - req);
	else
		memcpy(location, req, strlen(req));

	if (strlen(location) == 0) {
		http_index();
		return;
	}

	if (!strcmp(location, "ping"))
		http_ping();
	else if (!strcmp(location, "favicon.ico"))
		http_favicon();
	else if (!strcmp(location, "showsamp"))
		http_showsamp(req);
	else if (!strcmp(location, "index.html"))
		http_index();
	else if (!strcmp(location, "js/atop.js"))
		http_get_js();
	else if (!strcmp(location, "css/atop.css"))
		http_get_css();
	else if (!strcmp(location, "template"))
		http_get_template(req);
	else {
		http_prepare_response();
		write(clifd, http_404, strlen(http_404));
	}
}

static time_t httpd_now_ms()
{
	struct timeval now;

	gettimeofday(&now, NULL);

	return now.tv_sec * 1000 + now.tv_usec / 1000;
}

static void httpd_handle_request()
{
	char inbuf[INBUF_SIZE] = {0};
	int inbytes = 0;
	char httpreq[URL_LEN] = {0};
	time_t timeout = httpd_now_ms() + 100;

	fcntl(clifd, F_SETFL, fcntl(clifd, F_GETFL) | O_NONBLOCK);

	for ( ; ; ) {
		time_t now = httpd_now_ms();
		if (now >= timeout)
			goto closefd;

		struct pollfd pfd = {.fd = clifd, .events = POLLIN, .revents = 0};
		poll(&pfd, 1, timeout - now);

		int ret = read(clifd, inbuf + inbytes, sizeof(inbuf) - inbytes);
		if (ret < 0)
		{
			if (errno != EAGAIN)
				goto closefd;
			continue;
		}

		inbytes += ret;
		if ((inbytes >= 4) && strstr(inbuf, "\r\n\r\n"))
			break;

		/* buf is full, but we can not search the ent of HTTP header */
		if (inbytes == sizeof(inbuf))
			goto closefd;
	}

	/* support GET request only */
	if (strncmp("GET ", inbuf, 4))
		goto closefd;

	/* support HTTP 1.1 request only */
	char *httpver = strstr(inbuf, "HTTP/1.1");
	if (!httpver)
		goto closefd;

	/* Ex, GET /hello HTTP/1.1 */
	if ((httpver - inbuf > URL_LEN + 6) || (httpver - inbuf < 6))
		goto closefd;

	memcpy(httpreq, inbuf + 5, httpver - inbuf - 6);
	http_process_request(httpreq);

closefd:
	close(clifd);
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

static void *httpd_routine(int listenfd, char  *log_path)
{
	struct sockaddr_in cliaddr;
	socklen_t addrlen = sizeof(cliaddr);
	struct pollfd pfd = { .fd = listenfd, .events = POLLIN, .revents = 0 };
	int ret;

	while (1) {
		ret = poll(&pfd, 1, 1000);
		if (ret > 0) {
			clifd = accept(listenfd, (struct sockaddr *)&cliaddr, &addrlen);
			if (clifd < 0)
				continue;

			httpd_handle_request(clifd);
		}

		httpd_update_cache(log_path);
	}

	return NULL;
}

static int httpd(int httpport, char *log_path)
{
	struct sockaddr_in listenaddr;
	int listenfd = -1;

	signal(SIGPIPE, SIG_IGN);

	listenfd = socket(AF_INET, SOCK_STREAM, 0);
	if (listenfd == -1)
		return errno;

	int reuse = 1;
	setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
	setsockopt(listenfd, SOL_SOCKET, SO_REUSEPORT, (const char*)&reuse, sizeof(reuse));
	listenaddr.sin_family = AF_INET;
	listenaddr.sin_addr.s_addr = htonl(INADDR_ANY);
	listenaddr.sin_port = htons(httpport);
	if (bind(listenfd, (struct sockaddr *)&listenaddr, sizeof(listenaddr)))
		return errno;

	if (listen(listenfd, 128))
		return errno;

	httpd_routine(listenfd, log_path);

	return 0;
}

int __debug = 0;
static int default_port = 2867;
static char *default_log_path = "/var/log/atop";

static char *short_opts = "dDhp:P:V";
static struct option long_opts[] = {
	{"daemon",	no_argument,		0,	'd'},
	{"debug",	no_argument,		0,	'D'},
	{"port",	required_argument,	0,	'p'},
	{"path",	required_argument,	0,	'P'},
	{"help",	no_argument,		0,	'h'},
	{"version",	no_argument,		0,	'V'},
	{0,		0,			0,	0  }
};

static void httpd_showhelp(void)
{
	printf("Usage:\n");
	printf("  -d/--daemon   : run in daemon mode\n");
	printf("  -D/--debug    : run with debug message\n");
	printf("  -p/--port PORT: listen to PORT, default %d\n", default_port);
	printf("  -P/--path PATH: atop log path, default %s\n", default_log_path);
	printf("  -h/--help     : show help\n\n");
	printf("  maintained by : zhenwei pi<pizhenwei@bytedance.com> (HTTP backend)\n");
	printf("                  enhua zhou<zhouenhua@bytedance.com> (HTTP frontend)\n");
	exit(0);
}

static void httpd_showversion(void)
{
	printf("Version: %s\n", ATOPVERS);
	exit(0);
}

int main(int argc, char *argv[])
{
	int port = 0, daemonmode = 0;
	char *log_path = NULL;
	int args;
	char ch;

	while ((ch = getopt_long(argc, argv, short_opts, long_opts, &args)) >= 0) {
		switch (ch) {
			case 'd':
				daemonmode = 1;
				break;
			case 'D':
				__debug = 1;
				break;
			case 'p':
				port = atoi(optarg);
				break;
			case 'P':
				log_path = optarg;
				break;
			case 'V':
				httpd_showversion();
			case 'h':
			default:
				httpd_showhelp();
		}
	}

	if (daemonmode)
		daemon(0, 0);

	pagesize = sysconf(_SC_PAGESIZE);
	hertz = sysconf(_SC_CLK_TCK);
	uname(&utsname);

	if (!log_path)
		log_path = default_log_path;

	if (rawlog_parse_all(log_path)) {
		printf("%s: rawlog parse failed\n", __func__);
		return -1;
	}

	if (!port)
		port = default_port;

	log_debug("%s runs with log path(%s), port(%d)\n", argv[0], log_path, port);
	httpd(port, log_path);

	return 0;
}
