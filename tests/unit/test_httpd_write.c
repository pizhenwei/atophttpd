/*
 * Unit tests for the response-write driver in httpd.c (client_write).
 *
 * The kernel's TCP buffers are too large to force -EAGAIN or partial writes
 * deterministically end-to-end, so this file #includes httpd.c (whose static
 * client_write() is the unit under test) and drives it with a fake connection
 * type whose write() results are scripted per call. It pins down:
 *
 *  - every conn_write() is bounded by WRITE_CHUNK and the remaining budget
 *  - after -EAGAIN the SAME pointer and length are replayed (the OpenSSL retry
 *    contract), even when the budget has been replenished in between
 *  - the interest the backend asks for is the one armed in epoll
 *  - an exhausted budget yields to the event loop
 *  - a hard error or an expired deadline drops the connection
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

#include "test_framework.h"

/* httpd.c has its own main(); rename it out of the way */
#define main atophttpd_main
#include "../../httpd.c"
#undef main

/* The scenarios below are built from the constants, not hard-coded sizes. */
#define NCHUNKS (WRITE_BUDGET / WRITE_CHUNK)
_Static_assert(WRITE_BUDGET % WRITE_CHUNK == 0 && NCHUNKS >= 2 && NCHUNKS <= 12,
               "tests assume WRITE_BUDGET is 2..12 times WRITE_CHUNK");
#define ALL 0 /* script value: the backend takes everything it is offered */

/* Scripted fake backend: one entry per conn_write() call; a positive entry is
 * a byte count, ALL takes the whole offer, -EAGAIN parks, other negatives are
 * hard errors. Past the end of the script every write takes everything. */
static int g_script[16];
static int g_nscript, g_ncalls;
static struct {
    const char *ptr;
    size_t len;
} g_calls[16];
static int g_closed;
static int g_wait_events = EPOLLOUT; /* what -EAGAIN asks the loop to arm */

#define SCRIPT(...)                                                                                \
    do {                                                                                           \
        int s_[] = {__VA_ARGS__};                                                                  \
        memcpy(g_script, s_, sizeof(s_));                                                          \
        g_nscript = sizeof(s_) / sizeof(int);                                                      \
    } while (0)

static void reset(void)
{
    g_nscript = g_ncalls = g_closed = 0;
    g_wait_events = EPOLLOUT;
}

static int fake_write(connection *c, const void *data, size_t len)
{
    if (g_ncalls >= (int)(sizeof(g_calls) / sizeof(g_calls[0])))
        return -EIO; /* runaway loop: fail the test instead of overflowing */

    int step = g_ncalls < g_nscript ? g_script[g_ncalls] : ALL;

    g_calls[g_ncalls].ptr = data;
    g_calls[g_ncalls].len = len;
    g_ncalls++;

    if (step == -EAGAIN)
        c->wait_events = g_wait_events;
    return step == ALL ? (int)len : step;
}

static void fake_close(connection *c)
{
    (void)c;
    g_closed++;
}

static connection_type CT_Fake = {.close = fake_close, .write = fake_write};

static int g_fd; /* a pollable fd for client_drop()'s EPOLL_CTL_DEL */

static struct client *new_client(size_t outlen, long long deadline_in_ms)
{
    connection *conn = calloc(1, sizeof(*conn));
    struct client *cli = calloc(1, sizeof(*cli));

    conn->type = &CT_Fake;
    conn->fd = g_fd;
    conn->priv = cli;
    cli->conn = conn;
    cli->state = CLI_WRITE;
    cli->deadline = conn_now_ms() + deadline_in_ms;
    cli->out = calloc(1, outlen);
    cli->outlen = outlen;
    client_insert(cli);
    return cli;
}

static void test_partial_write_resumes_then_closes(void)
{
    reset();
    struct client *cli = new_client(1000, 60000);

    SCRIPT(400); /* first write is short, the rest flows */
    client_write(cli);

    CHECK_EQ_INT(g_ncalls, 2);
    CHECK_EQ_INT(g_calls[0].len, 1000);
    CHECK_EQ_INT(g_calls[1].len, 600);
    CHECK(g_calls[1].ptr == g_calls[0].ptr + 400);
    CHECK_EQ_INT(g_closed, 1); /* fully flushed: closed */
}

static void test_eagain_replays_same_arguments(void)
{
    reset();
    /* Spend the budget down to half a chunk, so the write that gets -EAGAIN
     * was clamped by the budget (WRITE_CHUNK / 2) -- a length a recomputation
     * on the next wakeup, with the budget replenished, would NOT reproduce. */
    struct client *cli = new_client(WRITE_BUDGET + WRITE_CHUNK, 60000);
    int k = NCHUNKS - 1;

    for (int i = 0; i < k; i++)
        g_script[i] = ALL;
    g_script[k] = WRITE_CHUNK / 2;
    g_script[k + 1] = -EAGAIN;
    g_nscript = k + 2;

    client_write(cli);

    CHECK_EQ_INT(g_ncalls, k + 2);
    CHECK_EQ_INT(g_calls[k + 1].len, WRITE_CHUNK / 2);
    CHECK_EQ_INT(g_closed, 0); /* parked, not dropped */
    CHECK_EQ_INT(cli->wait_events, EPOLLOUT);

    const char *ptr = g_calls[k + 1].ptr;

    reset();
    client_write(cli); /* next wakeup */

    CHECK_EQ_INT(g_calls[0].len, WRITE_CHUNK / 2); /* replayed, not WRITE_CHUNK */
    CHECK(g_calls[0].ptr == ptr);
    CHECK_EQ_INT(g_ncalls, 2); /* the replay, then the last chunk */
    CHECK_EQ_INT(g_closed, 1);
}

static void test_eagain_arms_backend_interest_then_error_drops(void)
{
    reset();
    struct client *cli = new_client(1000, 60000);

    g_wait_events = EPOLLIN; /* e.g. a TLS write that needs to read first */
    SCRIPT(-EAGAIN);
    client_write(cli);

    CHECK_EQ_INT(cli->wait_events, EPOLLIN);
    CHECK_EQ_INT(g_closed, 0);

    reset();
    SCRIPT(-EIO); /* the retry fails for good */
    client_write(cli);

    CHECK_EQ_INT(g_calls[0].len, 1000); /* retried with the parked length */
    CHECK_EQ_INT(g_closed, 1);
}

static void test_budget_exhausted_yields_to_loop(void)
{
    reset();
    struct client *cli = new_client(WRITE_BUDGET + 100, 60000);

    client_write(cli);

    CHECK_EQ_INT(g_ncalls, NCHUNKS); /* stopped once the budget was spent */
    CHECK_EQ_INT(cli->outsent, WRITE_BUDGET);
    CHECK_EQ_INT(cli->wait_events, EPOLLOUT);
    CHECK_EQ_INT(g_closed, 0);

    reset();
    client_write(cli); /* next wakeup finishes the job */

    CHECK_EQ_INT(g_calls[0].len, 100);
    CHECK_EQ_INT(g_closed, 1);
}

static void test_hard_error_drops(void)
{
    reset();
    struct client *cli = new_client(1000, 60000);

    SCRIPT(-EIO);
    client_write(cli);

    CHECK_EQ_INT(g_closed, 1);
}

static void test_expired_deadline_drops_without_writing(void)
{
    reset();
    struct client *cli = new_client(1000, -1);

    client_write(cli);

    CHECK_EQ_INT(g_ncalls, 0);
    CHECK_EQ_INT(g_closed, 1);
}

int main(void)
{
    int pfd[2];

    TEST_SUITE_BEGIN("httpd client_write");

    if (pipe(pfd) || (epollfd = epoll_create1(0)) < 0)
        return 2;
    g_fd = pfd[1];

    RUN_TEST(test_partial_write_resumes_then_closes);
    RUN_TEST(test_eagain_replays_same_arguments);
    RUN_TEST(test_eagain_arms_backend_interest_then_error_drops);
    RUN_TEST(test_budget_exhausted_yields_to_loop);
    RUN_TEST(test_hard_error_drops);
    RUN_TEST(test_expired_deadline_drops_without_writing);

    TEST_SUITE_END();
}
