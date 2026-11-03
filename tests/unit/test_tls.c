/*
 * Unit tests for the TLS send path (tls.c: ssl_write_all).
 *
 * ssl_write_all is static, so this file #includes tls.c (-DUSE_TLS) and
 * interposes on its OpenSSL calls -- SSL_write()/SSL_get_error()/SSL_get_fd() --
 * via the linker's --wrap. The fd it hands back is one end of a real socketpair,
 * so the fcntl()/poll() non-blocking machinery runs for real (no OpenSSL session
 * or TCP needed): the deadline test fills that socket's send buffer so poll()
 * genuinely blocks until the budget expires.
 *
 * This exercises the deadline *logic* deterministically and fast. A client-side
 * end-to-end "slow reader over real TLS" test was tried but dropped: the
 * difference between the non-blocking path and the old blocking one is below one
 * TLS record and masked by kernel/TLS buffering, so it could not reliably tell
 * the regression apart without a multi-minute, flaky reader. test_uses_nonblocking_io
 * below is the deterministic guard instead: it fails if the send path ever
 * reverts to blocking I/O.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/ssl.h>

#include "connection.h"
/* Shorten the send budget so the stalled-send test finishes in ms. Redefined
 * before tls.c (included below) expands it inside ssl_write_all. */
#undef CONN_SEND_TIMEOUT_MS
#define CONN_SEND_TIMEOUT_MS 50

#include "test_framework.h"

/* SSL_write/SSL_get_error interposers; g_mode picks the behaviour. */
enum {
    MODE_OK = 0,        /* every SSL_write accepts the whole chunk           */
    MODE_WANT_WRITE,    /* every SSL_write stalls (WANT_WRITE), no progress  */
    MODE_FATAL_FIRST,   /* first SSL_write fails fatally                     */
    MODE_FATAL_AFTER_OK /* first SSL_write succeeds, the next fails fatally  */
};

static int g_mode;
static size_t g_max_num;     /* largest length any SSL_write was asked to send */
static size_t g_total;       /* bytes "sent" so far                            */
static int g_calls;          /* number of SSL_write calls                      */
static int g_fd = -1;        /* fd handed back by the wrapped SSL_get_fd       */
static int g_saw_blocking;   /* set if any SSL_write ran with the fd blocking  */
static volatile char g_sink; /* defeats dead-store elimination of buf reads */

static void wrap_reset(int mode)
{
    g_mode = mode;
    g_max_num = 0;
    g_total = 0;
    g_calls = 0;
    g_saw_blocking = 0;
}

int __wrap_SSL_get_fd(const SSL *ssl)
{
    (void)ssl;
    return g_fd;
}

int __wrap_SSL_write(SSL *ssl, const void *buf, int num)
{
    (void)ssl;
    g_calls++;
    if ((size_t)num > g_max_num)
        g_max_num = (size_t)num;

    /* The whole point of the fix: the fd must be non-blocking while we write, so
     * a stalled peer surfaces as WANT_WRITE (and is bounded by poll) instead of
     * blocking inside SSL_write. Flag any call that runs with it still blocking. */
    if (g_fd >= 0 && !(fcntl(g_fd, F_GETFL) & O_NONBLOCK))
        g_saw_blocking = 1;

    /* Touch both ends of the range, like a real SSL_write: under ASan this flags
     * any out-of-bounds offset/length ssl_write_all computes -- the only
     * sanitizer coverage of the real TLS path (other builds use the stub). */
    if (num > 0) {
        const volatile char *p = buf;
        g_sink = (char)(p[0] ^ p[num - 1]);
    }

    switch (g_mode) {
    case MODE_WANT_WRITE:
        return -1;
    case MODE_FATAL_FIRST:
        return -1;
    case MODE_FATAL_AFTER_OK:
        if (g_calls == 1) {
            g_total += (size_t)num;
            return num;
        }
        return -1;
    case MODE_OK:
    default:
        g_total += (size_t)num;
        return num;
    }
}

int __wrap_SSL_get_error(const SSL *ssl, int ret)
{
    (void)ssl;
    (void)ret;
    return (g_mode == MODE_WANT_WRITE) ? SSL_ERROR_WANT_WRITE : SSL_ERROR_SSL;
}

/* Pull in the static function under test (and the rest of the TLS backend). */
#include "../../tls.c"

/* A non-NULL opaque handle; the wrappers never dereference it. */
static int ssl_dummy;
static SSL *const FAKE_SSL = (SSL *)&ssl_dummy;

static int g_sp[2] = {-1, -1}; /* g_sp[0] == g_fd (the "send" end) */

/* Shrink and then fill g_sp[0]'s send buffer so a subsequent poll(POLLOUT) on it
 * blocks (the peer end is never drained) -- the real signal ssl_write_all waits
 * on when SSL_write reports WANT_WRITE. */
static void fill_sendbuf(void)
{
    int snd = 4096;
    setsockopt(g_sp[0], SOL_SOCKET, SO_SNDBUF, &snd, sizeof(snd));
    int fl = fcntl(g_sp[0], F_GETFL);
    fcntl(g_sp[0], F_SETFL, fl | O_NONBLOCK);

    char block[8192];
    memset(block, 'f', sizeof(block));
    for (;;) {
        ssize_t n = write(g_sp[0], block, sizeof(block));
        if (n < 0)
            break; /* EAGAIN: buffer is full */
    }
    fcntl(g_sp[0], F_SETFL, fl); /* ssl_write_all re-sets O_NONBLOCK itself */
}

/* The fix's defining behaviour: ssl_write_all must switch the socket to
 * non-blocking for the duration of the send (and restore it afterwards). A
 * regression to a plain blocking SSL_write would be flagged here. */
static void test_uses_nonblocking_io(void)
{
    char buf[8 * 1024];
    memset(buf, 'n', sizeof(buf));

    /* start from a known blocking fd */
    int fl = fcntl(g_fd, F_GETFL);
    fcntl(g_fd, F_SETFL, fl & ~O_NONBLOCK);

    wrap_reset(MODE_OK);
    int sent = ssl_write_all(FAKE_SSL, buf, sizeof(buf));

    CHECK_EQ_INT(sent, (long long)sizeof(buf));
    CHECK(g_calls > 0);
    CHECK(g_saw_blocking == 0); /* the send ran non-blocking */
    /* and the caller's blocking mode is restored once done */
    CHECK((fcntl(g_fd, F_GETFL) & O_NONBLOCK) == 0);
}

/* The core guarantee: no single SSL_write() exceeds one TLS record. Without the
 * cap the whole buffer goes out in one call and this fails. */
static void test_chunk_capped(void)
{
    size_t len = 64 * 1024 + 123; /* several records, deliberately not a multiple */
    char *buf = malloc(len);
    REQUIRE(buf != NULL);
    memset(buf, 'x', len);

    wrap_reset(MODE_OK);
    int sent = ssl_write_all(FAKE_SSL, buf, len);

    CHECK_EQ_INT(sent, (long long)len);
    CHECK_EQ_INT(g_total, (long long)len);
    CHECK(g_max_num <= TLS_WRITE_CHUNK);
    CHECK(g_calls > 1); /* it genuinely chunked rather than sending once */
    free(buf);
}

/* A peer that never drains (perpetual WANT_WRITE + a full send buffer so poll()
 * blocks) must hit the deadline instead of stalling forever. */
static void test_deadline_terminates(void)
{
    char buf[32 * 1024];
    memset(buf, 'y', sizeof(buf));

    fill_sendbuf();
    wrap_reset(MODE_WANT_WRITE);
    long long t0 = conn_now_ms();
    int sent = ssl_write_all(FAKE_SSL, buf, sizeof(buf));
    long long elapsed = conn_now_ms() - t0;

    CHECK_EQ_INT(sent, 0); /* nothing got through */
    CHECK(elapsed >= 40);  /* it actually waited for the ~50 ms deadline */
    CHECK(elapsed < 2000); /* ...and not indefinitely */
}

/* A fatal error before any byte is sent surfaces as -EIO. */
static void test_fatal_first(void)
{
    char buf[1024];
    memset(buf, 'z', sizeof(buf));

    wrap_reset(MODE_FATAL_FIRST);
    int ret = ssl_write_all(FAKE_SSL, buf, sizeof(buf));

    CHECK_EQ_INT(ret, -EIO);
}

/* A fatal error after some bytes returns the partial count (a short, non-error
 * return the caller treats as truncation), not -EIO. */
static void test_partial_then_fatal(void)
{
    size_t len = 64 * 1024;
    char *buf = malloc(len);
    REQUIRE(buf != NULL);
    memset(buf, 'w', len);

    wrap_reset(MODE_FATAL_AFTER_OK);
    int ret = ssl_write_all(FAKE_SSL, buf, len);

    CHECK_EQ_INT(ret, TLS_WRITE_CHUNK); /* exactly the first record made it out */
    CHECK(ret > 0);
    free(buf);
}

int main(void)
{
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, g_sp) != 0) {
        perror("socketpair");
        return 1;
    }
    g_fd = g_sp[0];

    /* g_sp is closed by process exit; TEST_SUITE_END returns from main. */
    TEST_SUITE_BEGIN("tls ssl_write_all");
    RUN_TEST(test_uses_nonblocking_io);
    RUN_TEST(test_chunk_capped);
    RUN_TEST(test_deadline_terminates);
    RUN_TEST(test_fatal_first);
    RUN_TEST(test_partial_then_fatal);
    TEST_SUITE_END();
}
