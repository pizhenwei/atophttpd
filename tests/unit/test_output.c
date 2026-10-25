/*
 * Unit tests for output.c (the sample output sink abstraction).
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "output.h"
#include "test_framework.h"

static int done_called;
static int done_offset;

static void capture_done(struct output *op, connection *conn)
{
    (void)conn;
    done_called++;
    done_offset = op->ob.offset;
}

static void test_buf_basic(void)
{
    struct output op = {.output_type = OUTPUT_BUF};
    output_samp(&op, "hello", 5);
    output_samp(&op, "world", 5);
    CHECK_EQ_INT(op.ob.offset, 10);
    if (op.ob.buf != NULL)
        CHECK(memcmp(op.ob.buf, "helloworld", 10) == 0);
    free(op.ob.buf);
}

/* Writing more than the default 1 MiB buffer must grow it (output_buf). */
static void test_buf_growth(void)
{
    struct output op = {.output_type = OUTPUT_BUF};
    const int chunk = 600 * 1024;
    char *a = malloc(chunk);
    char *b = malloc(chunk);
    memset(a, 'A', chunk);
    memset(b, 'B', chunk);

    output_samp(&op, a, chunk);
    output_samp(&op, b, chunk);

    CHECK_EQ_INT(op.ob.offset, 2 * chunk);
    CHECK(op.ob.size >= 2 * chunk);
    CHECK(op.ob.buf[0] == 'A');
    CHECK(op.ob.buf[chunk - 1] == 'A');
    CHECK(op.ob.buf[chunk] == 'B');
    CHECK(op.ob.buf[2 * chunk - 1] == 'B');

    free(a);
    free(b);
    free(op.ob.buf);
}

/* output_samp_done calls the completion callback and resets a BUF sink. */
static void test_buf_done_resets(void)
{
    struct output op = {.output_type = OUTPUT_BUF, .done = capture_done};
    done_called = 0;
    done_offset = -1;

    output_samp(&op, "data", 4);
    output_samp_done(&op, NULL);

    CHECK_EQ_INT(done_called, 1);
    CHECK_EQ_INT(done_offset, 4);
    /* buffer reset for the next record */
    CHECK_EQ_INT(op.ob.offset, 0);

    /* done may be NULL */
    struct output op2 = {.output_type = OUTPUT_BUF, .done = NULL};
    output_samp(&op2, "x", 1);
    output_samp_done(&op2, NULL);
    CHECK_EQ_INT(op2.ob.offset, 0);

    free(op.ob.buf);
    free(op2.ob.buf);
}

static void test_fd_output(void)
{
    char tmpl[256];
    tf_tmpl(tmpl, sizeof(tmpl), "atophttpd_out_XXXXXX");
    int fd = mkstemp(tmpl);
    CHECK(fd >= 0);
    if (fd < 0)
        return;

    struct output op = {.output_type = OUTPUT_FD, .fd = fd};
    output_samp(&op, "to-fd\n", 6);

    /* read it back */
    lseek(fd, 0, SEEK_SET);
    char rb[16] = {0};
    ssize_t n = read(fd, rb, sizeof(rb) - 1);
    CHECK_EQ_INT(n, 6);
    CHECK_STR_EQ(rb, "to-fd\n");

    /* OUTPUT_FD has no buffer -> done is a no-op aside from callback */
    output_samp_done(&op, NULL);

    close(fd);
    unlink(tmpl);
}

static void test_stdout_output(void)
{
    /* Just exercises the OUTPUT_STDOUT branch (prints to stdout). */
    struct output op = {.output_type = OUTPUT_STDOUT};
    output_samp(&op, "", 0);
}

/* An unknown output type is a fatal programming error: output_samp must abort
 * with EINVAL.  Verified in a child process so the test itself survives. */
static void test_unknown_output_type(void)
{
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        struct output op = {.output_type = 0x7fff};
        output_samp(&op, "x", 1);
        _exit(0); /* should not be reached */
    }
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status));
    CHECK_EQ_INT(WEXITSTATUS(status), EINVAL);
}

int main(void)
{
    TEST_SUITE_BEGIN("output");
    RUN_TEST(test_buf_basic);
    RUN_TEST(test_buf_growth);
    RUN_TEST(test_buf_done_resets);
    RUN_TEST(test_fd_output);
    RUN_TEST(test_stdout_output);
    RUN_TEST(test_unknown_output_type);
    TEST_SUITE_END();
}
