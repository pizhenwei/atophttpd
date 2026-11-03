/*
 * Unit tests for connection.c (connection-type registry + listen helper)
 * and socket.c (the plain TCP connection implementation).
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "connection.h"
#include "test_framework.h"

/*
 * write()/writev() interposers (linked with -Wl,--wrap=write,--wrap=writev).
 * They normally pass straight through to the real syscalls; a test can arm
 * g_short_armed on a specific fd to force exactly one short write (a single
 * byte) and prove the send path recovers from it instead of truncating the
 * response. Only the main thread touches these globals, so they are TSan-safe.
 */
extern ssize_t __real_write(int fd, const void *buf, size_t count);
extern ssize_t __real_writev(int fd, const struct iovec *iov, int iovcnt);

static int g_short_fd = -1;
static int g_short_armed = 0;

ssize_t __wrap_write(int fd, const void *buf, size_t count)
{
    if (fd == g_short_fd && g_short_armed && count > 1) {
        g_short_armed = 0;
        return __real_write(fd, buf, 1); /* deliver only the first byte */
    }
    return __real_write(fd, buf, count);
}

ssize_t __wrap_writev(int fd, const struct iovec *iov, int iovcnt)
{
    if (fd == g_short_fd && g_short_armed && iovcnt > 0 && iov[0].iov_len > 1) {
        g_short_armed = 0;
        /* deliver only the first byte of the first segment */
        struct iovec one = {.iov_base = iov[0].iov_base, .iov_len = 1};
        return __real_writev(fd, &one, 1);
    }
    return __real_writev(fd, iov, iovcnt);
}

/* Minimal fake connection types used only to probe the registry below. */
static const char *probe1_get_type(connection *c)
{
    (void)c;
    return "probe-type-1";
}
static const char *probe2_get_type(connection *c)
{
    (void)c;
    return "probe-type-2";
}
static connection_type CT_Probe1 = {.get_type = probe1_get_type};
static connection_type CT_Probe2 = {.get_type = probe2_get_type};

static void test_registry(void)
{
    conntype_initialize();

    /* tcp is always available; lookup is case-insensitive */
    CHECK(get_conntype_by_name(CONN_TYPE_SOCKET) != NULL);
    CHECK(get_conntype_by_name("TCP") != NULL);
    CHECK(get_conntype_by_name("does-not-exist") == NULL);

    CHECK(get_conntype_index_by_name(CONN_TYPE_SOCKET) >= 0);
    CHECK_EQ_INT(get_conntype_index_by_name("does-not-exist"), -1);

    /*
     * A rejected duplicate registration must consume no slot. Checking the
     * index of tcp itself cannot see this (the lookup returns the first match,
     * which never moves), so probe it differentially: register a fresh type,
     * attempt to register tcp again, then register a second fresh type. If the
     * duplicate were wrongly accepted it would take a slot and push the second
     * probe's index one further, breaking the +1 invariant below.
     */
    conntype_register(&CT_Probe1);
    int p1 = get_conntype_index_by_name("probe-type-1");
    CHECK(p1 >= 0);
    register_conntype_socket(); /* duplicate attempt, must be rejected */
    conntype_register(&CT_Probe2);
    int p2 = get_conntype_index_by_name("probe-type-2");
    CHECK_EQ_INT(p2, p1 + 1);
}

static void test_listen_ipv4(void)
{
    int fd = listen_to_port(0, "127.0.0.1", AF_INET);
    CHECK(fd >= 0);
    if (fd >= 0)
        close(fd);
}

static void test_listen_ipv4_wildcard(void)
{
    int fd = listen_to_port(0, "*", AF_INET);
    CHECK(fd >= 0);
    if (fd >= 0)
        close(fd);
}

static void test_listen_ipv6(void)
{
    int fd = listen_to_port(0, "::1", AF_INET6);
    /* some CI kernels disable IPv6; accept either success or clean failure */
    CHECK(fd >= 0 || fd == -1);
    if (fd >= 0)
        close(fd);
}

static void test_listen_ipv6_wildcard(void)
{
    int fd = listen_to_port(0, "::*", AF_INET6);
    CHECK(fd >= 0 || fd == -1);
    if (fd >= 0)
        close(fd);
}

static void test_listen_bad_addr(void)
{
    /* getaddrinfo must fail for a bogus address -> -1 */
    int fd = listen_to_port(0, "999.999.999.999", AF_INET);
    CHECK_EQ_INT(fd, -1);
}

/* Full socket round-trip through the connection_type vtable. */
static void test_socket_roundtrip(void)
{
    connection_type *ct = get_conntype_by_name(CONN_TYPE_SOCKET);
    CHECK(ct != NULL);
    if (!ct)
        return;

    connection *listener = conn_create(ct, 0, "127.0.0.1");
    CHECK(listener != NULL);
    if (listener == NULL)
        return;
    CHECK_EQ_INT(conn_listen(listener), 0);

    /* discover the ephemeral port we actually bound to */
    struct sockaddr_in sa = {0};
    socklen_t slen = sizeof(sa);
    CHECK_EQ_INT(getsockname(listener->fd, (struct sockaddr *)&sa, &slen), 0);

    int cli = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(cli >= 0);
    struct sockaddr_in dst = {0};
    dst.sin_family = AF_INET;
    dst.sin_port = sa.sin_port;
    dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK_EQ_INT(connect(cli, (struct sockaddr *)&dst, sizeof(dst)), 0);

    connection *srv = conn_create(ct, -1, NULL);
    CHECK_EQ_INT(conn_accept(listener, srv), 0);

    /* server -> client write / writev, client reads */
    CHECK(conn_write(srv, "hi", 2) == 2);

    struct iovec iov[2];
    iov[0].iov_base = "AB";
    iov[0].iov_len = 2;
    iov[1].iov_base = "CD";
    iov[1].iov_len = 2;
    CHECK(conn_writev(srv, iov, 2) == 4);

    char rb[16] = {0};
    int n = read(cli, rb, sizeof(rb));
    CHECK(n == 6);
    CHECK(rb[0] == 'h' && rb[2] == 'A' && rb[4] == 'C');

    /* client -> server, server reads via conn_read */
    CHECK(write(cli, "xyz", 3) == 3);
    char sb[16] = {0};
    int m = conn_read(srv, sb, sizeof(sb));
    CHECK(m == 3);
    CHECK(sb[0] == 'x');

    conn_shutdown(srv);
    conn_close(srv);
    /* closing twice is safe */
    conn_close(srv);

    close(cli);
    conn_close(listener);
    free(srv);
    free(listener);
}

static void test_socket_error_paths(void)
{
    connection_type *ct = get_conntype_by_name(CONN_TYPE_SOCKET);
    connection *c = conn_create(ct, -1, NULL);
    c->fd = -1;

    /* operations on a closed connection return -EINVAL */
    CHECK_EQ_INT(conn_write(c, "x", 1), -EINVAL);
    struct iovec iov = {.iov_base = "x", .iov_len = 1};
    CHECK_EQ_INT(conn_writev(c, &iov, 1), -EINVAL);
    CHECK_EQ_INT(conn_read(c, (char[4]) {0}, 4), -EINVAL);

    /* listen with port == -1 is rejected */
    c->port = -1;
    c->bindaddr = "127.0.0.1";
    CHECK_EQ_INT(conn_listen(c), -EINVAL);

    /* shutdown/close are no-ops on an already-closed connection */
    conn_shutdown(c);
    conn_close(c);

    free(c);
}

/* IPv6 bind address selects the AF_INET6 listen path in conn_socket_listen. */
static void test_socket_listen_ipv6_path(void)
{
    connection_type *ct = get_conntype_by_name(CONN_TYPE_SOCKET);
    connection *c = conn_create(ct, 0, "::1");
    int ret = conn_listen(c);
    CHECK(ret == 0 || ret == -EINVAL); /* depends on IPv6 availability */
    if (ret == 0)
        conn_close(c);
    free(c);
}

/* A bind address that getaddrinfo cannot resolve makes conn_listen fail. */
static void test_socket_listen_bad_addr(void)
{
    connection_type *ct = get_conntype_by_name(CONN_TYPE_SOCKET);
    connection *c = conn_create(ct, 0, "999.999.999.999");
    CHECK_EQ_INT(conn_listen(c), -EINVAL);
    free(c);
}

/* accept() on a non-listening descriptor returns a negative errno. */
static void test_socket_accept_fail(void)
{
    connection_type *ct = get_conntype_by_name(CONN_TYPE_SOCKET);
    connection *listener = conn_create(ct, -1, NULL);
    connection *conn = conn_create(ct, -1, NULL);

    /* a plain (non-socket) fd makes accept() fail with ENOTSOCK */
    listener->fd = open("/dev/null", 0);
    CHECK(listener->fd >= 0);
    CHECK(conn_accept(listener, conn) < 0);

    conn_close(listener);
    free(listener);
    free(conn);
}

/* conn_read maps EAGAIN and hard errors to the expected negative returns. */
static void test_socket_read_nonblock(void)
{
    connection_type *ct = get_conntype_by_name(CONN_TYPE_SOCKET);
    int sv[2];
    CHECK_EQ_INT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

    connection *c = conn_create(ct, -1, NULL);
    c->fd = sv[0];

    /* non-blocking with no pending data -> -EAGAIN */
    int flags = fcntl(sv[0], F_GETFL, 0);
    fcntl(sv[0], F_SETFL, flags | O_NONBLOCK);
    char buf[8];
    CHECK_EQ_INT(conn_read(c, buf, sizeof(buf)), -EAGAIN);

    /* peer closed -> read() returns 0 (EOF); with errno cleared this maps to a
     * deterministic 0 (not -EAGAIN, not a spurious error) via conn_read */
    close(sv[1]);
    errno = 0;
    int ret = conn_read(c, buf, sizeof(buf));
    CHECK_EQ_INT(ret, 0);

    conn_close(c);
    free(c);
}

/* Reader thread: drain `want` bytes, tracking the count and an additive
 * checksum so we can prove the send path delivered the whole payload intact
 * (no truncation, no reordering) across the send-all loop. */
struct drainer {
    int fd;
    size_t want;
    size_t got;
    unsigned long long sum;
};

static void *drain_fn(void *arg)
{
    struct drainer *d = arg;
    unsigned char buf[65536];

    while (d->got < d->want) {
        ssize_t n = read(d->fd, buf, sizeof(buf));
        if (n <= 0)
            break;
        for (ssize_t i = 0; i < n; i++)
            d->sum += buf[i];
        d->got += (size_t)n;
    }

    return NULL;
}

/*
 * A payload larger than the socket buffers forces conn_write/conn_writev to
 * loop; verify the whole thing arrives (byte count + checksum) so a short
 * write can never silently truncate the response.
 */
static void test_socket_send_all_large(void)
{
    connection_type *ct = get_conntype_by_name(CONN_TYPE_SOCKET);
    connection *listener = conn_create(ct, 0, "127.0.0.1");
    CHECK_EQ_INT(conn_listen(listener), 0);

    struct sockaddr_in sa = {0};
    socklen_t slen = sizeof(sa);
    CHECK_EQ_INT(getsockname(listener->fd, (struct sockaddr *)&sa, &slen), 0);

    int cli = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(cli >= 0);
    struct sockaddr_in dst = {0};
    dst.sin_family = AF_INET;
    dst.sin_port = sa.sin_port;
    dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK_EQ_INT(connect(cli, (struct sockaddr *)&dst, sizeof(dst)), 0);

    connection *srv = conn_create(ct, -1, NULL);
    CHECK_EQ_INT(conn_accept(listener, srv), 0);

    size_t n = 1u << 20; /* 1 MiB */
    unsigned char *payload = malloc(n);
    CHECK(payload != NULL);
    unsigned long long expect = 0;
    for (size_t i = 0; i < n; i++) {
        payload[i] = (unsigned char)(i & 0xff);
        expect += payload[i];
    }

    /* contiguous send */
    struct drainer d1 = {.fd = cli, .want = n};
    pthread_t t1;
    CHECK_EQ_INT(pthread_create(&t1, NULL, drain_fn, &d1), 0);
    CHECK_EQ_INT(conn_write(srv, payload, n), (int)n);
    pthread_join(t1, NULL);
    CHECK(d1.got == n);
    CHECK(d1.sum == expect);

    /* scatter-gather send across three segments */
    size_t a = n / 2, b = n / 4, c = n - a - b;
    struct iovec iov[3] = {
        {.iov_base = payload, .iov_len = a},
        {.iov_base = payload + a, .iov_len = b},
        {.iov_base = payload + a + b, .iov_len = c},
    };
    struct drainer d2 = {.fd = cli, .want = n};
    pthread_t t2;
    CHECK_EQ_INT(pthread_create(&t2, NULL, drain_fn, &d2), 0);
    CHECK_EQ_INT(conn_writev(srv, iov, 3), (int)n);
    pthread_join(t2, NULL);
    CHECK(d2.got == n);
    CHECK(d2.sum == expect);

    free(payload);
    conn_close(srv);
    close(cli);
    conn_close(listener);
    free(srv);
    free(listener);
}

/*
 * A non-blocking socket with a tiny send buffer and no reader forces the send
 * path to stop mid-payload: conn_write/conn_writev must then report the partial
 * byte count (never the full length, never a bogus success) and, for writev,
 * correctly resume in the middle of an iovec segment on the next call.
 */
static void test_socket_send_all_partial(void)
{
    connection_type *ct = get_conntype_by_name(CONN_TYPE_SOCKET);

    int sv[2];
    CHECK_EQ_INT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

    int small = 8192;
    setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof(small));
    setsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &small, sizeof(small));
    int flags = fcntl(sv[0], F_GETFL, 0);
    CHECK_EQ_INT(fcntl(sv[0], F_SETFL, flags | O_NONBLOCK), 0);

    connection *c = conn_create(ct, -1, NULL);
    c->fd = sv[0];

    size_t n = 1u << 20; /* 1 MiB, far larger than the send buffer */
    unsigned char *payload = calloc(1, n);
    CHECK(payload != NULL);

    /* contiguous: fills the buffer, then EAGAIN -> partial, not truncated-as-ok */
    int w = conn_write(c, payload, n);
    CHECK(w > 0 && (size_t)w < n);

    /* fully drain the peer so the sender's buffer space is reclaimed (a partial
     * read would not free the head skb), then the next writev can make progress
     * but still cannot finish */
    int rflags = fcntl(sv[1], F_GETFL, 0);
    fcntl(sv[1], F_SETFL, rflags | O_NONBLOCK);
    unsigned char tmp[4096];
    while (read(sv[1], tmp, sizeof(tmp)) > 0) {
    }

    /* gather across three segments; the first partial writev fills the buffer
     * mid-segment, exercising the resume-inside-an-iovec path */
    size_t seg = n / 3;
    struct iovec iov[3] = {
        {.iov_base = payload, .iov_len = seg},
        {.iov_base = payload, .iov_len = seg},
        {.iov_base = payload, .iov_len = seg},
    };
    int wv = conn_writev(c, iov, 3);
    CHECK(wv > 0 && (size_t)wv < n);

    free(payload);
    conn_close(c);
    close(sv[1]);
    free(c);
}

/* When the peer is gone the send path surfaces an error instead of looping, and
 * an empty gather list is a no-op. */
static void test_socket_send_all_errors(void)
{
    signal(SIGPIPE, SIG_IGN);
    connection_type *ct = get_conntype_by_name(CONN_TYPE_SOCKET);

    int sv[2];
    CHECK_EQ_INT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    connection *c = conn_create(ct, -1, NULL);
    c->fd = sv[0];
    close(sv[1]); /* peer gone -> writes fail with EPIPE */

    char buf[256] = {0};
    CHECK(conn_write(c, buf, sizeof(buf)) < 0);

    struct iovec iov[2] = {
        {.iov_base = buf, .iov_len = sizeof(buf)},
        {.iov_base = buf, .iov_len = sizeof(buf)},
    };
    CHECK(conn_writev(c, iov, 2) < 0);

    /* empty gather list writes nothing */
    CHECK_EQ_INT(conn_writev(c, NULL, 0), 0);

    conn_close(c);
    free(c);
}

/*
 * Regression guard for the send-all loop. A forced one-byte short write (via
 * the __wrap_write/__wrap_writev interposers) must NOT make conn_write or
 * conn_writev report a truncated length: the loop has to resume and deliver the
 * whole payload. The reader thread drains everything and checksums it, so we
 * also prove the bytes arrive intact and in order after the resume. With the
 * pre-fix single write()/writev() this test fails (returns 1, not n).
 */
static void test_socket_send_all_short_write(void)
{
    connection_type *ct = get_conntype_by_name(CONN_TYPE_SOCKET);

    size_t n = 1u << 16; /* 64 KiB: forces a second write, stays quick */
    unsigned char *payload = malloc(n);
    CHECK(payload != NULL);
    if (!payload)
        return;
    unsigned long long expect = 0;
    for (size_t i = 0; i < n; i++) {
        payload[i] = (unsigned char)(i * 131u + 7u);
        expect += payload[i];
    }

    /* --- contiguous send: force one short write() mid-payload --- */
    {
        int sv[2];
        CHECK_EQ_INT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
        connection *c = conn_create(ct, -1, NULL);
        c->fd = sv[0];

        struct drainer d = {.fd = sv[1], .want = n};
        pthread_t t;
        CHECK_EQ_INT(pthread_create(&t, NULL, drain_fn, &d), 0);

        g_short_fd = c->fd;
        g_short_armed = 1;
        int w = conn_write(c, payload, n);
        g_short_armed = 0;
        g_short_fd = -1;

        conn_shutdown(c); /* EOF so the drainer stops even on a short send */
        pthread_join(t, NULL);

        CHECK_EQ_INT(w, (int)n); /* the regression: never truncate */
        CHECK(d.got == (size_t)n);
        CHECK(d.sum == expect);

        conn_close(c);
        close(sv[1]);
        free(c);
    }

    /* --- scatter-gather send: force one short writev() inside the first
     * segment, exercising the resume-mid-iovec path --- */
    {
        int sv[2];
        CHECK_EQ_INT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
        connection *c = conn_create(ct, -1, NULL);
        c->fd = sv[0];

        struct drainer d = {.fd = sv[1], .want = n};
        pthread_t t;
        CHECK_EQ_INT(pthread_create(&t, NULL, drain_fn, &d), 0);

        size_t a = n / 2, b = n / 4, rest = n - a - b;
        struct iovec iov[3] = {
            {.iov_base = payload, .iov_len = a},
            {.iov_base = payload + a, .iov_len = b},
            {.iov_base = payload + a + b, .iov_len = rest},
        };

        g_short_fd = c->fd;
        g_short_armed = 1;
        int wv = conn_writev(c, iov, 3);
        g_short_armed = 0;
        g_short_fd = -1;

        conn_shutdown(c);
        pthread_join(t, NULL);

        CHECK_EQ_INT(wv, (int)n);
        CHECK(d.got == (size_t)n);
        CHECK(d.sum == expect);

        conn_close(c);
        close(sv[1]);
        free(c);
    }

    free(payload);
}

int main(void)
{
    TEST_SUITE_BEGIN("connection+socket");
    RUN_TEST(test_registry);
    RUN_TEST(test_listen_ipv4);
    RUN_TEST(test_listen_ipv4_wildcard);
    RUN_TEST(test_listen_ipv6);
    RUN_TEST(test_listen_ipv6_wildcard);
    RUN_TEST(test_listen_bad_addr);
    RUN_TEST(test_socket_roundtrip);
    RUN_TEST(test_socket_error_paths);
    RUN_TEST(test_socket_listen_ipv6_path);
    RUN_TEST(test_socket_listen_bad_addr);
    RUN_TEST(test_socket_accept_fail);
    RUN_TEST(test_socket_read_nonblock);
    RUN_TEST(test_socket_send_all_large);
    RUN_TEST(test_socket_send_all_partial);
    RUN_TEST(test_socket_send_all_errors);
    RUN_TEST(test_socket_send_all_short_write);
    TEST_SUITE_END();
}
