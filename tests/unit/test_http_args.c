/*
 * Unit tests for http_args.c (http_arg_long / http_arg_str).
 *
 * These exercise the query-string parsing helpers, including the edge
 * cases that previously caused buffer overflows (off-by-one on the
 * destination buffer and the "%2C" decoding loop).
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */

#include "test_framework.h"
#include "http_args.h"

/* A mutable copy is required: http_arg_str may rewrite the request in place. */
static char req_buf[2048];
static char *mkreq(const char *s)
{
	strncpy(req_buf, s, sizeof(req_buf) - 1);
	req_buf[sizeof(req_buf) - 1] = '\0';
	return req_buf;
}

static void test_arg_long_basic(void)
{
	long v = 0;
	CHECK_EQ_INT(http_arg_long(mkreq("timestamp=1675158274&x=1"), "timestamp", &v), 0);
	CHECK_EQ_INT(v, 1675158274L);
}

static void test_arg_long_missing(void)
{
	long v = 42;
	CHECK_EQ_INT(http_arg_long(mkreq("lables=ALL"), "timestamp", &v), -1);
	/* value untouched on failure */
	CHECK_EQ_INT(v, 42);
}

static void test_arg_long_no_equals(void)
{
	long v = 0;
	/* "timestamp" present but not followed by '=' */
	CHECK_EQ_INT(http_arg_long(mkreq("timestampX=5"), "timestamp", &v), -1);
}

/* Regression: a parameter name must not match inside a longer token
 * (e.g. "timestamp" must not match the tail of "xtimestamp"). */
static void test_arg_long_boundary(void)
{
	long v = 0;
	CHECK_EQ_INT(http_arg_long(mkreq("xtimestamp=5&timestamp=10"), "timestamp", &v), 0);
	CHECK_EQ_INT(v, 10);
}

static void test_arg_str_basic(void)
{
	char buf[64] = {0};
	CHECK_EQ_INT(http_arg_str(mkreq("lables=ALL&encoding=none"), "lables", buf, sizeof(buf)), 0);
	CHECK_STR_EQ(buf, "ALL");
}

static void test_arg_str_last_param(void)
{
	char buf[64] = {0};
	CHECK_EQ_INT(http_arg_str(mkreq("timestamp=1&encoding=deflate"), "encoding", buf, sizeof(buf)), 0);
	CHECK_STR_EQ(buf, "deflate");
}

static void test_arg_str_missing(void)
{
	char buf[64] = {0};
	CHECK_EQ_INT(http_arg_str(mkreq("timestamp=1"), "encoding", buf, sizeof(buf)), -1);
}

static void test_arg_str_no_equals(void)
{
	char buf[64] = {0};
	CHECK_EQ_INT(http_arg_str(mkreq("encodingX"), "encoding", buf, sizeof(buf)), -1);
}

/* Regression: a parameter name must not match inside a longer token
 * (e.g. "lables" must not match inside "notlables"). */
static void test_arg_str_boundary(void)
{
	char buf[64] = {0};
	CHECK_EQ_INT(http_arg_str(mkreq("notlables=XXX&lables=ALL"), "lables", buf, sizeof(buf)), 0);
	CHECK_STR_EQ(buf, "ALL");
}

/* Regression: a value that exactly fills the buffer must be rejected,
 * not written (previously wrote the NUL one byte past the end). */
static void test_arg_str_exact_fit_rejected(void)
{
	char buf[8] = {0};
	/* value is 8 chars -> needs 9 bytes incl. NUL -> must fail */
	CHECK_EQ_INT(http_arg_str(mkreq("v=ABCDEFGH"), "v", buf, sizeof(buf)), -1);
}

static void test_arg_str_max_fit_accepted(void)
{
	char buf[8] = {0};
	/* value is 7 chars -> fits exactly with the NUL */
	CHECK_EQ_INT(http_arg_str(mkreq("v=ABCDEFG"), "v", buf, sizeof(buf)), 0);
	CHECK_STR_EQ(buf, "ABCDEFG");
}

static void test_arg_str_pct2c_single(void)
{
	char buf[64] = {0};
	CHECK_EQ_INT(http_arg_str(mkreq("lables=CPU%2CMEM"), "lables", buf, sizeof(buf)), 0);
	CHECK_STR_EQ(buf, "CPU,MEM");
}

static void test_arg_str_pct2c_multi(void)
{
	char buf[64] = {0};
	CHECK_EQ_INT(http_arg_str(mkreq("lables=CPU%2CMEM%2CDSK"), "lables", buf, sizeof(buf)), 0);
	CHECK_STR_EQ(buf, "CPU,MEM,DSK");
}

/* Regression: "%2C" at the very end of a value that fills the buffer used
 * to read past the end of the string in the decoding memmove. */
static void test_arg_str_pct2c_trailing(void)
{
	char buf[8] = {0};
	/* "ABCD%2C" is 7 chars -> fits in buf[8]; decodes to "ABCD," */
	CHECK_EQ_INT(http_arg_str(mkreq("v=ABCD%2C"), "v", buf, sizeof(buf)), 0);
	CHECK_STR_EQ(buf, "ABCD,");
}

static void test_arg_str_pct2c_leading(void)
{
	char buf[64] = {0};
	CHECK_EQ_INT(http_arg_str(mkreq("v=%2CA%2C"), "v", buf, sizeof(buf)), 0);
	CHECK_STR_EQ(buf, ",A,");
}

int main(void)
{
	TEST_SUITE_BEGIN("http_args");
	RUN_TEST(test_arg_long_basic);
	RUN_TEST(test_arg_long_missing);
	RUN_TEST(test_arg_long_no_equals);
	RUN_TEST(test_arg_long_boundary);
	RUN_TEST(test_arg_str_basic);
	RUN_TEST(test_arg_str_last_param);
	RUN_TEST(test_arg_str_missing);
	RUN_TEST(test_arg_str_no_equals);
	RUN_TEST(test_arg_str_boundary);
	RUN_TEST(test_arg_str_exact_fit_rejected);
	RUN_TEST(test_arg_str_max_fit_accepted);
	RUN_TEST(test_arg_str_pct2c_single);
	RUN_TEST(test_arg_str_pct2c_multi);
	RUN_TEST(test_arg_str_pct2c_trailing);
	RUN_TEST(test_arg_str_pct2c_leading);
	TEST_SUITE_END();
}
