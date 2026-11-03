/*
 * Unit tests for the TLS send path (tls.c: ssl_write_all).
 *
 * ssl_write_all is static, so this file #includes tls.c (-DUSE_TLS) and
 * interposes on its only two OpenSSL calls -- SSL_write()/SSL_get_error() --
 * via the linker's --wrap, driving it with no real TLS session, socket or
 * timing.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

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
static volatile char g_sink; /* defeats dead-store elimination of buf reads */

static void wrap_reset(int mode)
{
    g_mode = mode;
    g_max_num = 0;
    g_total = 0;
    g_calls = 0;
}

int __wrap_SSL_write(SSL *ssl, const void *buf, int num)
{
    (void)ssl;
    g_calls++;
    if ((size_t)num > g_max_num)
        g_max_num = (size_t)num;

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

/* A peer that never drains (perpetual WANT_WRITE) must hit the deadline instead
 * of spinning forever. */
static void test_deadline_terminates(void)
{
    char buf[32 * 1024];
    memset(buf, 'y', sizeof(buf));

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
    TEST_SUITE_BEGIN("tls ssl_write_all");
    RUN_TEST(test_chunk_capped);
    RUN_TEST(test_deadline_terminates);
    RUN_TEST(test_fatal_first);
    RUN_TEST(test_partial_then_fatal);
    TEST_SUITE_END();
}
