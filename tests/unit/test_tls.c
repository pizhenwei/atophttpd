/*
 * Unit tests for the non-blocking TLS backend (tls.c: conn_tls_handshake,
 * conn_write_tls, conn_read_tls).
 *
 * The backend functions are static, so this file #includes tls.c (-DUSE_TLS) and
 * interposes on the OpenSSL calls -- SSL_accept()/SSL_read()/SSL_write()/
 * SSL_get_error() -- via the linker's --wrap. No OpenSSL session or TCP is
 * needed: the tests drive the wrappers through every outcome (success, WANT_READ,
 * WANT_WRITE, clean close, fatal) and check how the backend maps each one onto
 * the connection contract (see connection.h): data/0 on success, -EAGAIN plus
 * the epoll interest to re-arm (conn->wait_events), or a negative errno.
 *
 * This is what lets the event loop stay single-threaded and non-blocking: a
 * stalled peer must surface as -EAGAIN here, never block inside SSL_*.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>

#include <openssl/ssl.h>

#include "connection.h"
#include "test_framework.h"

/* What the next SSL_* call "returns" and the error SSL_get_error() reports. */
static int g_ret;
static int g_err;
static int g_sys_errno;   /* errno to expose with SSL_ERROR_SYSCALL */
static size_t g_last_num; /* length SSL_write/SSL_read was asked for        */
static int g_calls;       /* total SSL_* calls (a blocking loop would add up) */

static void wrap_set(int ret, int err)
{
    g_ret = ret;
    g_err = err;
    g_sys_errno = 0;
    g_last_num = 0;
    g_calls = 0;
}

int __wrap_SSL_accept(SSL *ssl)
{
    (void)ssl;
    g_calls++;
    return g_ret;
}

int __wrap_SSL_write(SSL *ssl, const void *buf, int num)
{
    (void)ssl;
    (void)buf;
    g_calls++;
    g_last_num = (size_t)num;
    return g_ret < 0 ? g_ret : (g_ret > num ? num : g_ret);
}

int __wrap_SSL_read(SSL *ssl, void *buf, int num)
{
    (void)ssl;
    (void)buf;
    g_calls++;
    g_last_num = (size_t)num;
    if (g_ret > 0 && g_sys_errno == 0)
        return g_ret > num ? num : g_ret;
    if (g_sys_errno)
        errno = g_sys_errno;
    return g_ret;
}

int __wrap_SSL_get_error(const SSL *ssl, int ret)
{
    (void)ssl;
    (void)ret;
    return g_err;
}

/* Pull in the static functions under test (and the rest of the TLS backend). */
#include "../../tls.c"

/* A non-NULL opaque handle; the wrappers never dereference it. */
static int ssl_dummy;

static connection *new_conn(void)
{
    connection *c = conn_create(&CT_TLS, -1, NULL);
    ((tls_connection *)c)->ssl = (SSL *)&ssl_dummy;
    return c;
}

/* The wrapped-out SSL handle must not be freed by conn_close_tls. */
static void free_conn(connection *c)
{
    ((tls_connection *)c)->ssl = NULL;
    free(c);
}

static void test_handshake_ok(void)
{
    connection *c = new_conn();

    wrap_set(1, SSL_ERROR_NONE);
    CHECK_EQ_INT(conn_tls_handshake(c), 0);
    CHECK_EQ_INT(g_calls, 1); /* exactly one step, no internal retry loop */

    free_conn(c);
}

static void test_handshake_want_read(void)
{
    connection *c = new_conn();

    /* silent client: no ClientHello yet. Must return at once, asking to be
     * woken when the socket becomes readable -- this is issue #12 */
    wrap_set(-1, SSL_ERROR_WANT_READ);
    CHECK_EQ_INT(conn_tls_handshake(c), -EAGAIN);
    CHECK_EQ_INT(c->wait_events, EPOLLIN);
    CHECK_EQ_INT(g_calls, 1);

    free_conn(c);
}

static void test_handshake_want_write(void)
{
    connection *c = new_conn();

    wrap_set(-1, SSL_ERROR_WANT_WRITE);
    CHECK_EQ_INT(conn_tls_handshake(c), -EAGAIN);
    CHECK_EQ_INT(c->wait_events, EPOLLOUT);

    free_conn(c);
}

static void test_handshake_fatal(void)
{
    connection *c = new_conn();

    wrap_set(-1, SSL_ERROR_SSL);
    CHECK_EQ_INT(conn_tls_handshake(c), -EIO);

    free_conn(c);
}

static void test_handshake_no_ssl(void)
{
    connection *c = conn_create(&CT_TLS, -1, NULL);

    CHECK_EQ_INT(conn_tls_handshake(c), -EINVAL);

    free(c);
}

/* The whole buffer is handed to SSL_write in one call: no per-record chunking
 * or retry loop here any more, the event loop resumes from the returned count. */
static void test_write_ok_and_partial(void)
{
    connection *c = new_conn();
    char buf[64 * 1024];

    memset(buf, 'x', sizeof(buf));

    wrap_set((int)sizeof(buf), SSL_ERROR_NONE);
    CHECK_EQ_INT(conn_write_tls(c, buf, sizeof(buf)), (long long)sizeof(buf));
    CHECK_EQ_INT(g_last_num, (long long)sizeof(buf));
    CHECK_EQ_INT(g_calls, 1);

    /* partial write mode: only part of the buffer accepted */
    wrap_set(16384, SSL_ERROR_NONE);
    CHECK_EQ_INT(conn_write_tls(c, buf, sizeof(buf)), 16384);
    CHECK_EQ_INT(g_calls, 1);

    free_conn(c);
}

static void test_write_would_block(void)
{
    connection *c = new_conn();
    char buf[1024] = {0};

    wrap_set(-1, SSL_ERROR_WANT_WRITE);
    CHECK_EQ_INT(conn_write_tls(c, buf, sizeof(buf)), -EAGAIN);
    CHECK_EQ_INT(c->wait_events, EPOLLOUT);
    CHECK_EQ_INT(g_calls, 1); /* no spinning on a stalled peer */

    /* a renegotiation may need to read before the write can proceed */
    wrap_set(-1, SSL_ERROR_WANT_READ);
    CHECK_EQ_INT(conn_write_tls(c, buf, sizeof(buf)), -EAGAIN);
    CHECK_EQ_INT(c->wait_events, EPOLLIN);

    free_conn(c);
}

static void test_write_fatal(void)
{
    connection *c = new_conn();
    char buf[1024] = {0};

    wrap_set(-1, SSL_ERROR_SSL);
    CHECK_EQ_INT(conn_write_tls(c, buf, sizeof(buf)), -EIO);

    free_conn(c);
}

static void test_writev_gathers_into_one_write(void)
{
    connection *c = new_conn();
    struct iovec iov[3] = {
        {.iov_base = "HTTP/1.1 200 OK\r\n", .iov_len = 17},
        {.iov_base = "Server: atop\r\n\r\n", .iov_len = 16},
        {.iov_base = "pong\r\n", .iov_len = 6},
    };

    wrap_set(17 + 16 + 6, SSL_ERROR_NONE);
    CHECK_EQ_INT(conn_writev_tls(c, iov, 3), 39);
    CHECK_EQ_INT(g_last_num, 39); /* every segment counted, none dropped */

    free_conn(c);
}

static void test_read_paths(void)
{
    connection *c = new_conn();
    char buf[256];

    wrap_set(42, SSL_ERROR_NONE);
    CHECK_EQ_INT(conn_read_tls(c, buf, sizeof(buf)), 42);

    /* peer sent close_notify: clean EOF is a deterministic 0 */
    wrap_set(0, SSL_ERROR_ZERO_RETURN);
    CHECK_EQ_INT(conn_read_tls(c, buf, sizeof(buf)), 0);

    wrap_set(-1, SSL_ERROR_WANT_READ);
    CHECK_EQ_INT(conn_read_tls(c, buf, sizeof(buf)), -EAGAIN);
    CHECK_EQ_INT(c->wait_events, EPOLLIN);

    wrap_set(-1, SSL_ERROR_WANT_WRITE);
    CHECK_EQ_INT(conn_read_tls(c, buf, sizeof(buf)), -EAGAIN);
    CHECK_EQ_INT(c->wait_events, EPOLLOUT);

    /* raw non-blocking read below the TLS layer surfaces as SYSCALL+EAGAIN */
    wrap_set(-1, SSL_ERROR_SYSCALL);
    g_sys_errno = EAGAIN;
    CHECK_EQ_INT(conn_read_tls(c, buf, sizeof(buf)), -EAGAIN);
    CHECK_EQ_INT(c->wait_events, EPOLLIN);

    /* abrupt TCP reset: a hard error, not a retry */
    wrap_set(-1, SSL_ERROR_SYSCALL);
    g_sys_errno = ECONNRESET;
    CHECK_EQ_INT(conn_read_tls(c, buf, sizeof(buf)), -EIO);

    wrap_set(-1, SSL_ERROR_SSL);
    CHECK_EQ_INT(conn_read_tls(c, buf, sizeof(buf)), -EIO);

    free_conn(c);
}

static void test_no_ssl_is_einval(void)
{
    connection *c = conn_create(&CT_TLS, -1, NULL);
    char buf[8] = {0};

    CHECK_EQ_INT(conn_write_tls(c, buf, sizeof(buf)), -EINVAL);
    CHECK_EQ_INT(conn_read_tls(c, buf, sizeof(buf)), -EINVAL);

    free(c);
}

int main(void)
{
    TEST_SUITE_BEGIN("tls non-blocking backend");
    RUN_TEST(test_handshake_ok);
    RUN_TEST(test_handshake_want_read);
    RUN_TEST(test_handshake_want_write);
    RUN_TEST(test_handshake_fatal);
    RUN_TEST(test_handshake_no_ssl);
    RUN_TEST(test_write_ok_and_partial);
    RUN_TEST(test_write_would_block);
    RUN_TEST(test_write_fatal);
    RUN_TEST(test_writev_gathers_into_one_write);
    RUN_TEST(test_read_paths);
    RUN_TEST(test_no_ssl_is_einval);
    TEST_SUITE_END();
}
