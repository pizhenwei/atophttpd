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

#include "http_args.h"
#include "httpd.h"
#include "log.h"
#include "output.h"

#include "version.h"

#define DEFAULT_PORT 2867
#define DEFAULT_TLS_PORT 2868
#define DEFAULT_LOG_PATH "/var/log/atop"

#define DEFAULT_CERT_FILE "/etc/pki/atophttpd/server.crt"
#define DEFAULT_KEY_FILE "/etc/pki/atophttpd/server.key"
#define DEFAULT_CA_FILE "/etc/pki/CA/ca.crt"

static void http_show_samp_done(struct output *op, connection *conn);

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
static volatile sig_atomic_t httpd_stop = 0;

#define INBUF_SIZE 4096
#define URL_LEN 1024
/* HTTP codes */
static char *http_200 = "HTTP/1.1 200 OK\r\n";
static char *http_404 = "HTTP/1.1 404 Not Found\r\n";

/* HTTP content types */
static char *http_content_type_none = "";
static char *http_content_type_deflate = "Content-Encoding: deflate\r\n";

/* HTTP generic header */
static char *http_generic = "Server: atop\r\n"
                            "%s" /* for http_content_type_XXX */
                            "Content-Type: %s; charset=utf-8\r\n"
                            "Content-Length: %zu\r\n\r\n";

/* HTTP content types */
static char *http_content_type_html = "text/html";
static char *http_content_type_css = "text/css";
static char *http_content_type_javascript = "application/javascript";

/* ------------------------------------------------------------------------
 * Event loop: every connection (and listener) is an epoll node. The main
 * loop below drives each connection through a small state machine:
 *
 *   CLI_HANDSHAKE (TLS only) -> CLI_READ -> CLI_WRITE -> close
 *
 * Everything runs non-blocking on one thread, so a client that stalls (a
 * silent TLS handshake, a never-finished request head, an unreadable
 * response) only ever stalls itself; every other connection keeps being
 * served. Each state carries a deadline after which the connection is
 * dropped (see the sweep in httpd_routine()).
 * ------------------------------------------------------------------------ */

/* Budget (ms) to read one request head, kept from the previous
 * implementation: a client that connects but never finishes sending its
 * request is dropped after this. */
#define REQUEST_TIMEOUT_MS 100
/* Budget (ms) for completing one TLS handshake. */
#define HANDSHAKE_TIMEOUT_MS 10000
/* Budget (ms) for flushing one response to a slow client. */
#define RESPONSE_TIMEOUT_MS 10000
#define MAX_EVENTS 64
/* Largest single conn_write() call, so no one write can run long. */
#define WRITE_CHUNK (64 * 1024)
/* Bytes flushed to one client per event-loop wakeup, so a fast reader of a
 * large response cannot starve the deadline sweep or the other connections. */
#define WRITE_BUDGET (256 * 1024)

enum cli_state {
    CLI_LISTENER,  /* epoll node is a listener: accept on EPOLLIN */
    CLI_HANDSHAKE, /* TLS handshake in progress */
    CLI_READ,      /* accumulating the request head */
    CLI_WRITE,     /* flushing the response */
};

struct client {
    connection *conn; /* owns fd (+ssl); conn->priv points back to this struct */
    enum cli_state state;
    int wait_events;    /* epoll interest currently armed */
    long long deadline; /* ms (monotonic) to leave the current state */

    size_t inbytes;
    char inbuf[INBUF_SIZE];

    char *out; /* response head+body, contiguous, malloc'd */
    size_t outlen;
    size_t outsent;
    size_t outpending; /* length of a write that returned -EAGAIN, 0 if none */

    struct client *prev, *next;
};

static struct client *clients;
static int epollfd = -1;

static void client_insert(struct client *cli)
{
    cli->prev = NULL;
    cli->next = clients;
    if (clients)
        clients->prev = cli;
    clients = cli;
}

static void client_remove(struct client *cli)
{
    if (cli->prev)
        cli->prev->next = cli->next;
    else
        clients = cli->next;
    if (cli->next)
        cli->next->prev = cli->prev;
    cli->prev = cli->next = NULL;
}

static void client_drop(struct client *cli)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, cli->conn->fd, NULL);
    client_remove(cli);
    conn_close(cli->conn);
    free(cli->conn);
    free(cli->out);
    free(cli);
}

/* (Re-)arm the epoll interest of a connection; a no-op when unchanged. The
 * first arm (wait_events == 0) registers the fd, later ones just modify it. */
static void client_arm(struct client *cli, int events)
{
    struct epoll_event ev = {.events = events, .data.ptr = cli};
    int op = cli->wait_events ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;

    if (cli->wait_events == events)
        return;

    if (epoll_ctl(epollfd, op, cli->conn->fd, &ev) < 0)
        log_warn("epoll_ctl(%s) failed: %m\n", op == EPOLL_CTL_ADD ? "ADD" : "MOD");
    else
        cli->wait_events = events;
}

/* Attach a response buffer (head+body, contiguous) to the client; the event
 * loop flushes it and closes the connection once done. On allocation failure
 * out stays NULL and the caller drops the connection. */
static void client_set_out(struct client *cli, const void *buf, size_t len)
{
    char *out = malloc(len);

    if (!out) {
        log_warn("response allocation failed, dropping request\n");
        return;
    }
    memcpy(out, buf, len);
    cli->out = out;
    cli->outlen = len;
    cli->outsent = 0;
}

/* A bare single-line response (e.g. the 404 status line). */
static void client_respond_line(struct client *cli, const char *line)
{
    client_set_out(cli, line, strlen(line));
}

/* status line + generic headers + body in one contiguous buffer. */
static void client_respond(struct client *cli, const char *status, const char *body, size_t len,
                           const char *encoding, const char *content_type)
{
    char header[256];
    int hlen = snprintf(header, sizeof(header), "%s", status);

    if (hlen < 0 || hlen >= (int)sizeof(header))
        return;
    hlen +=
        snprintf(header + hlen, sizeof(header) - hlen, http_generic, encoding, content_type, len);

    if (hlen < 0 || hlen >= (int)sizeof(header)) {
        log_err("response header too large, dropping request\n");
        return;
    }

    char *out = malloc((size_t)hlen + len);
    if (!out) {
        log_warn("response allocation failed, dropping request\n");
        return;
    }
    memcpy(out, header, (size_t)hlen);
    memcpy(out + hlen, body, len);

    cli->out = out;
    cli->outlen = (size_t)hlen + len;
    cli->outsent = 0;
}

/* The handlers keep their original (connection *) signatures; the event loop's
 * per-connection state hangs off conn->priv. */
static void http_response_200(connection *conn, char *buf, size_t len, char *encoding,
                              char *content_type)
{
    client_respond(conn->priv, http_200, buf, len, encoding, content_type);
}

static void http_response_404(connection *conn)
{
    client_respond_line(conn->priv, http_404);
}

static void http_show_samp_done(struct output *op, connection *conn)
{
    if (op->encoding == http_content_type_none) {
        http_response_200(conn, op->ob.buf, op->ob.offset, op->encoding, http_content_type_html);
        return;
    }

    /* compress data for encoding deflate */
    unsigned long complen = compressBound(op->ob.offset);
    char *compbuf = malloc(complen);

    if (!compbuf ||
        compress((Bytef *)compbuf, &complen, (Bytef *)op->ob.buf, op->ob.offset) != Z_OK) {
        http_response_404(conn);
        free(compbuf);
        return;
    }

    http_response_200(conn, compbuf, complen, http_content_type_deflate, http_content_type_html);
    free(compbuf);
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

    if (timestamp <= 0) {
        char *err = "invalid timestamp\r\n";
        http_response_200(conn, err, strlen(err), http_content_type_none, http_content_type_html);
        return;
    }

    if (http_arg_str(req, "lables", lables, sizeof(lables)) < 0) {
        char *err = "missing lables\r\n";
        http_response_200(conn, err, strlen(err), http_content_type_none, http_content_type_html);
        return;
    }

    struct output op = {
        .output_type = OUTPUT_BUF,
        .done = http_show_samp_done,
        .encoding = http_content_type_deflate,
    };
    if (http_arg_str(req, "encoding", encoding, sizeof(encoding)) == 0) {
        if (!strcmp(encoding, "none")) {
            op.encoding = http_content_type_none;
        } else if (!strcmp(encoding, "deflate")) {
            op.encoding = http_content_type_deflate;
        } else {
            char *err = "encoding supports none/deflate only\r\n";
            http_response_200(conn, err, strlen(err), http_content_type_none,
                              http_content_type_html);
            return; /* op.ob.buf is NULL here, nothing to free */
        }
    }

    if (rawlog_get_record(timestamp, lables, conn, &op) < 0) {
        char *err = "missing sample\r\n";
        http_response_200(conn, err, strlen(err), http_content_type_none, http_content_type_html);
    }

    free(op.ob.buf);
}

/* Import a binary file */
#define IMPORT_BIN(sect, file, sym)                                                                \
    asm(".section " #sect "\n"                                                                     \
        ".global " #sym "\n"                                                                       \
        ".global " #sym "_end\n" #sym ":\n"                                                        \
        ".incbin \"" file "\"\n" #sym "_end:\n"                                                    \
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
    http_response_200(conn, favicon, favicon_end - favicon, http_content_type_none,
                      http_content_type_html);
}

/* Build index.html into atop binary */
IMPORT_BIN(".rodata", "http/index.html", http_index_html);
extern char http_index_html[], http_index_html_end[];

static void http_index(connection *conn)
{
    http_response_200(conn, http_index_html, http_index_html_end - http_index_html,
                      http_content_type_none, http_content_type_html);
}

/* Build atop.js into atop binary */
IMPORT_BIN(".rodata", "http/js/atop.js", atop_js);
extern char atop_js[], atop_js_end[];

static void http_get_atop_js(connection *conn)
{
    http_response_200(conn, atop_js, atop_js_end - atop_js, http_content_type_none,
                      http_content_type_javascript);
}

/* Build atop_parse.js into atop binary */
IMPORT_BIN(".rodata", "http/js/atop_parse.js", atop_parse_js);
extern char atop_parse_js[], atop_parse_js_end[];

static void http_get_atop_parse_js(connection *conn)
{
    http_response_200(conn, atop_parse_js, atop_parse_js_end - atop_parse_js,
                      http_content_type_none, http_content_type_javascript);
}

/* Build atop_compare_fc.js into atop binary */
IMPORT_BIN(".rodata", "http/js/atop_compare_fc.js", atop_compare_fc_js);
extern char atop_compare_fc_js[], atop_compare_fc_js_end[];

static void http_get_atop_compare_fc_js(connection *conn)
{
    http_response_200(conn, atop_compare_fc_js, atop_compare_fc_js_end - atop_compare_fc_js,
                      http_content_type_none, http_content_type_javascript);
}

/* Build atop.css into atop binary */
IMPORT_BIN(".rodata", "http/css/atop.css", http_css);
extern char http_css[], http_css_end[];

static void http_get_css(connection *conn)
{
    http_response_200(conn, http_css, http_css_end - http_css, http_content_type_none,
                      http_content_type_css);
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
    http_response_200(conn, header_html_template, header_html_template_end - header_html_template,
                      http_content_type_none, http_content_type_html);
}

static void http_get_template(char *req, connection *conn)
{
    char template_type[256];
    if (http_arg_str(req, "type", template_type, sizeof(template_type)) < 0)
        return;

    if (!strcmp(template_type, "generic")) {
        http_response_200(conn, generic_html_template,
                          generic_html_template_end - generic_html_template, http_content_type_none,
                          http_content_type_html);
    } else if (!strcmp(template_type, "memory")) {
        http_response_200(conn, memory_html_template,
                          memory_html_template_end - memory_html_template, http_content_type_none,
                          http_content_type_html);
    } else if (!strcmp(template_type, "disk")) {
        http_response_200(conn, disk_html_template, disk_html_template_end - disk_html_template,
                          http_content_type_none, http_content_type_html);
    } else if (!strcmp(template_type, "command_line")) {
        http_response_200(conn, command_line_html_template,
                          command_line_html_template_end - command_line_html_template,
                          http_content_type_none, http_content_type_html);
    } else {
        http_response_404(conn);
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

    if (strlen(req) >= URL_LEN) {
        http_response_404(conn);
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
        http_response_404(conn);
    }
}

/*
 * Per-state drivers. Each one makes non-blocking progress and either advances
 * the state machine, re-arms the epoll interest, or drops the connection.
 */
static void client_drop(struct client *cli);

static void client_handshake(struct client *cli)
{
    int ret = conn_handshake(cli->conn);

    if (ret == 0) { /* handshake done: wait for the request head */
        cli->state = CLI_READ;
        cli->deadline = conn_now_ms() + REQUEST_TIMEOUT_MS;
        client_arm(cli, EPOLLIN);
        return;
    }

    if (ret == -EAGAIN) {
        client_arm(cli, cli->conn->wait_events);
        return;
    }

    client_drop(cli);
}

static void client_write(struct client *cli)
{
    connection *conn = cli->conn;
    size_t budget = WRITE_BUDGET;

    while (cli->outsent < cli->outlen) {
        if (conn_now_ms() >= cli->deadline) { /* too slow a reader */
            client_drop(cli);
            return;
        }
        if (!budget) { /* yield; epoll fires again at once if still writable */
            client_arm(cli, EPOLLOUT);
            return;
        }

        /* After -EAGAIN the write must be retried with the very same arguments
         * (an OpenSSL requirement), so replay the remembered length. */
        size_t len = cli->outpending;

        if (!len) {
            len = cli->outlen - cli->outsent;
            if (len > WRITE_CHUNK)
                len = WRITE_CHUNK;
            if (len > budget)
                len = budget;
        }

        int ret = conn_write(conn, cli->out + cli->outsent, len);

        if (ret == -EAGAIN) {
            cli->outpending = len;
            client_arm(cli, conn->wait_events);
            return;
        }
        cli->outpending = 0;
        if (ret < 0) { /* hard error: give up on this client */
            client_drop(cli);
            return;
        }
        cli->outsent += (size_t)ret;
        budget = (size_t)ret >= budget ? 0 : budget - (size_t)ret;
    }

    /* response fully flushed; one request per connection, close now */
    client_drop(cli);
}

static void client_parse_request(struct client *cli);

static void client_read(struct client *cli)
{
    connection *conn = cli->conn;

    for (;;) {
        int ret = conn_read(conn, cli->inbuf + cli->inbytes, sizeof(cli->inbuf) - 1 - cli->inbytes);

        if (ret == 0) /* EOF before a complete request */
            goto drop;
        if (ret == -EAGAIN) {
            client_arm(cli, conn->wait_events ? conn->wait_events : EPOLLIN);
            return;
        }
        if (ret < 0)
            goto drop;

        cli->inbytes += (size_t)ret;
        /* strstr() relies on inbuf staying NUL-terminated: the struct is
         * calloc'd and the byte at inbytes is never written before the parse. */
        if (cli->inbytes >= 4 && strstr(cli->inbuf, "\r\n\r\n"))
            break;

        /* buf is full, but the HTTP header end is nowhere in sight */
        if (cli->inbytes == sizeof(cli->inbuf) - 1)
            goto drop;
    }

    client_parse_request(cli);
    return;

drop:
    client_drop(cli);
}

/*
 * The request head is complete: parse and route it. Handlers build the response
 * into cli->out; a malformed request or a handler that builds nothing just
 * drops the connection, like the previous implementation did.
 */
static void client_parse_request(struct client *cli)
{
    char *inbuf = cli->inbuf;
    char httpreq[URL_LEN] = {0};

    /*
     * Parse the request line "GET <uri> HTTP/1.1". Bound every lookup to the
     * first CRLF so nothing reads into the body, and locate the version via
     * the space just before it (a query string that itself contains the token
     * "HTTP/1.1" must not fool the parser).
     */
    char *eol = strstr(inbuf, "\r\n");
    if (!eol)
        goto drop;

    /* method: GET only */
    if (strncmp("GET ", inbuf, 4))
        goto drop;

    char *uri = inbuf + 4;                  /* just after "GET " */
    char *sp = memchr(uri, ' ', eol - uri); /* space before the version */
    if (!sp)
        goto drop;

    /* version: HTTP/1.1 only */
    if (strncmp(sp + 1, "HTTP/1.1", 8))
        goto drop;

    /*
     * The URI must be absolute; drop the leading '/' so it matches the
     * routing table, which compares against bare names ("ping", "help", ...).
     */
    if (*uri != '/')
        goto drop;
    uri++;

    size_t urilen = (size_t)(sp - uri);
    if (urilen >= URL_LEN)
        goto drop;

    memcpy(httpreq, uri, urilen);
    httpreq[urilen] = '\0';

    http_process_request(httpreq, cli->conn);

    if (!cli->out) /* malformed, unrouted, or allocation failure */
        goto drop;

    /* flush the built response */
    cli->state = CLI_WRITE;
    cli->deadline = conn_now_ms() + RESPONSE_TIMEOUT_MS;
    client_write(cli);
    return;

drop:
    client_drop(cli);
}

/* Accept one connection from a listener and enter the state machine. */
static void client_new(struct client *listener_cli)
{
    connection *listener = listener_cli->conn;
    connection *conn = conn_create(listener->type, -1, NULL);
    struct client *cli = calloc(1, sizeof(struct client));
    int onoff = 1;

    if (!conn || !cli) {
        log_warn("accept: out of memory\n");
        goto fail;
    }

    if (conn_accept(listener, conn) < 0)
        goto fail;

    /* every client fd runs non-blocking inside the event loop */
    fcntl(conn->fd, F_SETFL, fcntl(conn->fd, F_GETFL) | O_NONBLOCK);
    setsockopt(conn->fd, IPPROTO_TCP, TCP_NODELAY, &onoff, sizeof(onoff));

    conn->priv = cli;
    cli->conn = conn;
    cli->state = listener->type->handshake ? CLI_HANDSHAKE : CLI_READ;
    cli->deadline =
        conn_now_ms() + (cli->state == CLI_HANDSHAKE ? HANDSHAKE_TIMEOUT_MS : REQUEST_TIMEOUT_MS);
    client_insert(cli);

    if (cli->state == CLI_HANDSHAKE)
        client_handshake(cli); /* first step; returns -EAGAIN until ClientHello */
    else
        client_arm(cli, EPOLLIN);
    return;

fail:
    if (conn) {
        conn_close(conn);
        free(conn);
    }
    free(cli);
}

static void httpd_update_cache(char *log_path)
{
    static time_t update;
    time_t now = time(NULL);

    if (now - update < 3)
        return;

    if (rawlog_parse_all(log_path)) {
        log_warn("%s: rawlog parse failed\n", __func__);
    }

    update = now;
}

/*
 * The main (and only) serving thread: one epoll loop drives every listener
 * and every accepted connection through the state machine above. Nothing
 * blocks, so one stalled client -- e.g. a silent TLS handshake -- can never
 * stop the server from accepting or serving other connections (issue #12).
 */
static void *httpd_routine(connection **listeners, char *log_path)
{
    struct epoll_event events[MAX_EVENTS];
    struct client *cli, *next;
    int nr_listener = 0;

    epollfd = epoll_create1(0);
    if (epollfd < 0) {
        log_err("Failed create epollfd\n");
        exit(1);
    }

    for (int i = 0; i < CONN_TYPE_MAX; i++) {
        connection *listener = listeners[i];
        struct client *lc;

        if (listener == NULL)
            continue;

        if (conn_listen(listener) < 0) {
            log_err("ConnectionType %s failed listening on port %u, aborting.\n",
                    listener->type->get_type(NULL), listener->port);
            exit(1);
        }

        /* a client that resets between epoll_wait() and accept() must not
         * leave accept() blocking the whole loop */
        fcntl(listener->fd, F_SETFL, fcntl(listener->fd, F_GETFL) | O_NONBLOCK);

        lc = calloc(1, sizeof(struct client));
        if (!lc)
            exit(1);
        lc->conn = listener;
        lc->state = CLI_LISTENER;
        client_insert(lc);
        client_arm(lc, EPOLLIN);
        nr_listener++;
    }

    if (nr_listener == 0) {
        log_err("Listen Nothing, Exit.\n");
        exit(1);
    }

    log_info("Ready to serve\n");

    while (!httpd_stop) {
        /* never sleep past the nearest per-state deadline */
        int timeout = 1000;
        long long now = conn_now_ms();

        for (cli = clients; cli; cli = cli->next) {
            if (cli->state == CLI_LISTENER)
                continue;
            if (cli->deadline - now < (long long)timeout)
                timeout = (int)(cli->deadline - now);
        }
        if (timeout < 0)
            timeout = 0;

        int n = epoll_wait(epollfd, events, MAX_EVENTS, timeout);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            log_err("Error in epoll_wait %m\n");
            exit(1);
        }

        for (int i = 0; i < n; i++) {
            struct client *c = events[i].data.ptr;
            uint32_t ev = events[i].events;

            if (ev & (EPOLLERR | EPOLLHUP)) {
                if (c->state != CLI_LISTENER)
                    client_drop(c);
                continue;
            }

            switch (c->state) {
            case CLI_LISTENER:
                client_new(c);
                break;
            case CLI_HANDSHAKE:
                client_handshake(c);
                break;
            case CLI_READ:
                client_read(c);
                break;
            case CLI_WRITE:
                client_write(c);
                break;
            }
        }

        /* drop every client whose state deadline has expired: silent or
         * slow clients never hold the loop, they simply go away */
        now = conn_now_ms();
        for (cli = clients; cli; cli = next) {
            next = cli->next;
            if (cli->state != CLI_LISTENER && now >= cli->deadline)
                client_drop(cli);
        }

        /* refresh the cache periodically (throttled to every few seconds)
         * so it stays current even when no request is in flight */
        httpd_update_cache(log_path);
    }

    /* shutdown: close every client and listener */
    for (cli = clients; cli; cli = next) {
        next = cli->next;
        if (cli->state == CLI_LISTENER) {
            client_remove(cli);
            conn_close(cli->conn); /* the ctx owns the listener object */
            free(cli);
        } else {
            client_drop(cli);
        }
    }

    close(epollfd);
    return NULL;
}

static void httpd_on_terminate(int sig)
{
    /* request a clean shutdown so the main loop can return normally */
    (void)sig;
    httpd_stop = 1;
}

static int httpd(struct atophttd_context ctx)
{
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, httpd_on_terminate);
    signal(SIGINT, httpd_on_terminate);
    connection *listener;
    int conn_index, ret;

    if (ctx.port > 0) {
        conn_index = get_conntype_index_by_name(CONN_TYPE_SOCKET);
        if (conn_index < 0) {
            log_err("Failed finding connection listener of %s\n", CONN_TYPE_SOCKET);
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
        listener = conn_create(get_conntype_by_name(CONN_TYPE_TLS), ctx.tls_ctx_config.tls_port,
                               ctx.tls_ctx_config.tls_addr);

        ret = conn_configure(get_conntype_by_name(CONN_TYPE_TLS), &ctx.tls_ctx_config, 1);
        if (ret < 0) {
            log_err("Failed to configure TLS\n");
            exit(1);
        }

        ctx.listeners[conn_index] = listener;
    }

    if (ctx.daemonmode && daemon(0, 0)) {
        log_err("failed to daemonize: %m\n");
        exit(1);
    }

    httpd_routine(ctx.listeners, ctx.log_path);
    return 0;
}

int __debug = 0;
static char *short_opts = "dDhHp:a:P:t::A:C:c:k:V";

static struct option long_opts[] = {{"daemon", no_argument, 0, 'd'},
                                    {"debug", no_argument, 0, 'D'},
                                    {"port", required_argument, 0, 'p'},
                                    {"addr", required_argument, 0, 'a'},
                                    {"path", required_argument, 0, 'P'},
                                    {"tls-port", optional_argument, 0, 't'},
                                    {"tls-addr", required_argument, 0, 'A'},
                                    {"ca-cert-file", required_argument, 0, 'C'},
                                    {"cert-file", required_argument, 0, 'c'},
                                    {"key-file", required_argument, 0, 'k'},
                                    {"help", no_argument, 0, 'h'},
                                    {"hide-cmdline", no_argument, 0, 'H'},
                                    {"version", no_argument, 0, 'V'},
                                    {0, 0, 0, 0}};

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
    printf(
        "  -C/--ca-cert-file PATH\n    Path to the server TLS trusted CA cert file, default %s\n",
        DEFAULT_CA_FILE);
    printf("  -c/--cert-file PATH \n    Path to the server TLS cert file, default %s\n",
           DEFAULT_CERT_FILE);
    printf("  -k/--key-file PATH  \n    Path to the server TLS key file, default %s\n",
           DEFAULT_KEY_FILE);
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
    int args;
    int ch;
    int rc;

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
            break;
        case 'h':
        default:
            httpd_showhelp();
        }
    }

    log_open(config.daemonmode);

    pagesize = sysconf(_SC_PAGESIZE);
    hertz = sysconf(_SC_CLK_TCK);
    uname(&utsname);

    if (config.log_path == NULL) {
        log_err("%s: log path is nil\n", __func__);
        return -1;
    }

    if (rawlog_parse_all(config.log_path)) {
        log_err("%s: rawlog parse failed\n", __func__);
        return -1;
    }

    log_info("%s runs with log path(%s), port(%d)\n", argv[0], config.log_path, config.port);
    conntype_initialize();
    rc = httpd(config);

    return rc;
}
