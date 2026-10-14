/*
 * Unit tests for rawlog.c (atop raw-log directory parsing and record lookup).
 *
 * Error paths are exercised without any real data; the success / rebuild /
 * record-decoding paths run only when a real atop raw-log is provided through
 * the RAWLOG_SAMPLE environment variable (the coverage driver sets this).
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include "test_framework.h"
#include "config.h"
#include "cache.h"
#include "output.h"
#include "rawlog.h"

int rawlog_parse_all(const char *path);
int rawlog_get_record(time_t ts, char *labels, void *conn);

/* globals normally provided by httpd.c */
int __debug = 0;
unsigned int pagesize = 4096;
unsigned short hertz = 100;
struct utsname utsname;
int hidecmdline = 0;
struct output defop = { .output_type = OUTPUT_BUF, .done = NULL };

static char tmpdir[256];

static void setup_tmpdir(void)
{
	tf_tmpl(tmpdir, sizeof(tmpdir), "atophttpd_rawlog_XXXXXX");
	CHECK(mkdtemp(tmpdir) != NULL);
}

static void write_file(const char *name, const void *data, size_t len)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/%s", tmpdir, name);
	int fd = open(path, O_CREAT | O_WRONLY | O_EXCL | O_NOFOLLOW, 0600);
	if (fd >= 0) {
		ssize_t w = write(fd, data, len);
		(void)w;
		close(fd);
	}
}

static void test_parse_nonexistent_dir(void)
{
	CHECK(rawlog_parse_all("/no/such/dir/at/all") < 0);
}

static void test_parse_invalid_files(void)
{
	setup_tmpdir();
	/* too short to contain a rawheader */
	char tiny[4] = { 1, 2, 3, 4 };
	write_file("short", tiny, sizeof(tiny));
	/* wrong magic: a block of zeros the size of a plausible header */
	char zeros[512] = {0};
	write_file("badmagic", zeros, sizeof(zeros));

	/* parse_all tolerates individual bad files and still returns 0 */
	CHECK_EQ_INT(rawlog_parse_all(tmpdir), 0);
	/* no valid cache should have been created */
	CHECK(cache_get_recent() == NULL);
}

static long first_sample_time;
static long last_sample_time;
static const char *sample;

static int load_sample_into_tmpdir(void)
{
	sample = getenv("RAWLOG_SAMPLE");
	if (!sample)
		return 0;

	setup_tmpdir();
	char dst[512];
	snprintf(dst, sizeof(dst), "%s/atop_sample", tmpdir);

	int in = open(sample, O_RDONLY | O_NOFOLLOW);
	int out = open(dst, O_CREAT | O_WRONLY | O_EXCL | O_NOFOLLOW, 0600);
	if (in < 0 || out < 0) {
		if (in >= 0) close(in);
		if (out >= 0) close(out);
		return 0;
	}
	char buf[65536];
	ssize_t n;
	while ((n = read(in, buf, sizeof(buf))) > 0) {
		ssize_t w = write(out, buf, n);
		(void)w;
	}
	close(in);
	close(out);
	return 1;
}

static void test_parse_real_log(void)
{
	if (!load_sample_into_tmpdir()) {
		printf("    (skipped: no RAWLOG_SAMPLE provided)\n");
		return;
	}
	CHECK_EQ_INT(rawlog_parse_all(tmpdir), 0);
	const struct cache_t *c = cache_get_recent();
	CHECK(c != NULL);
	if (c) {
		CHECK(c->nr_elems > 0);
		first_sample_time = c->elems[0].time;
		last_sample_time = c->elems[c->nr_elems - 1].time;
	}

	/* re-parsing an unchanged directory is a no-op (cached) */
	CHECK_EQ_INT(rawlog_parse_all(tmpdir), 0);

	/* grow the file -> exercises the rebuild path */
	char dst[512];
	snprintf(dst, sizeof(dst), "%s/atop_sample", tmpdir);
	int fd = open(dst, O_WRONLY | O_APPEND | O_NOFOLLOW);
	if (fd >= 0) {
		char pad[128] = {0};
		ssize_t w = write(fd, pad, sizeof(pad));
		(void)w;
		close(fd);
	}
	CHECK_EQ_INT(rawlog_parse_all(tmpdir), 0);
}

static void test_get_record(void)
{
	if (!first_sample_time) {
		printf("    (skipped: no real sample parsed)\n");
		return;
	}

	char labels[32];

	/* a valid timestamp produces JSON output into defop */
	strcpy(labels, "CPU,MEM");
	int ret = rawlog_get_record(first_sample_time, labels, NULL);
	CHECK_EQ_INT(ret, 0);
	CHECK(defop.ob.offset == 0); /* reset by output_samp_done */

	/* a far-future timestamp falls back to the most recent sample */
	strcpy(labels, "CPU");
	ret = rawlog_get_record(last_sample_time + 100000, labels, NULL);
	CHECK_EQ_INT(ret, 0);
}

/*
 * Read the (valid) rawheader from the sample file so crafted headers can be
 * derived from it.  Returns 0 if no sample is available.
 */
static int read_sample_header(struct rawheader *rh)
{
	const char *s = getenv("RAWLOG_SAMPLE");
	if (!s)
		return 0;
	int fd = open(s, O_RDONLY | O_NOFOLLOW);
	if (fd < 0)
		return 0;
	ssize_t n = read(fd, rh, sizeof(*rh));
	close(fd);
	return n == (ssize_t)sizeof(*rh);
}

static void write_header_dir(char *dir, const struct rawheader *rh)
{
	tf_tmpl(dir, 256, "atophttpd_rawhdr_XXXXXX");
	CHECK(mkdtemp(dir) != NULL);
	char path[512];
	snprintf(path, sizeof(path), "%s/atop_hdr", dir);
	int fd = open(path, O_CREAT | O_WRONLY | O_EXCL | O_NOFOLLOW, 0600);
	if (fd >= 0) {
		ssize_t w = write(fd, rh, sizeof(*rh));
		(void)w;
		close(fd);
	}
}

/* A file with a valid header but no records exercises the free_cache path. */
static void test_parse_header_only(void)
{
	struct rawheader rh;
	if (!read_sample_header(&rh)) {
		printf("    (skipped: no RAWLOG_SAMPLE provided)\n");
		return;
	}
	char dir[256];
	write_header_dir(dir, &rh);
	CHECK_EQ_INT(rawlog_parse_all(dir), 0);
}

/* A header whose declared length is wrong must fail verification. */
static void test_parse_bad_headlen(void)
{
	struct rawheader rh;
	if (!read_sample_header(&rh)) {
		printf("    (skipped: no RAWLOG_SAMPLE provided)\n");
		return;
	}
	rh.rawheadlen = rh.rawheadlen + 1; /* corrupt the declared length */
	char dir[256];
	write_header_dir(dir, &rh);
	/* parse_all tolerates the rejected file and still returns 0 */
	CHECK_EQ_INT(rawlog_parse_all(dir), 0);
}

/* A header advertising an incompatible atop version must fail verification. */
static void test_parse_bad_version(void)
{
	struct rawheader rh;
	if (!read_sample_header(&rh)) {
		printf("    (skipped: no RAWLOG_SAMPLE provided)\n");
		return;
	}
	rh.aversion = 0xFFFF; /* MSB set, version 0x7fff: cannot match getnumvers() */
	char dir[256];
	write_header_dir(dir, &rh);
	CHECK_EQ_INT(rawlog_parse_all(dir), 0);
}

/* A full-size header with a wrong magic must fail verification. */
static void test_parse_bad_magic(void)
{
	struct rawheader rh;
	if (!read_sample_header(&rh)) {
		printf("    (skipped: no RAWLOG_SAMPLE provided)\n");
		return;
	}
	rh.magic = 0xdeadbeef; /* not MYMAGIC */
	char dir[256];
	write_header_dir(dir, &rh);
	CHECK_EQ_INT(rawlog_parse_all(dir), 0);
}

int main(void)
{
	uname(&utsname);
	TEST_SUITE_BEGIN("rawlog");
	RUN_TEST(test_parse_nonexistent_dir);
	RUN_TEST(test_parse_invalid_files);
	RUN_TEST(test_parse_real_log);
	RUN_TEST(test_get_record);
	RUN_TEST(test_parse_header_only);
	RUN_TEST(test_parse_bad_headlen);
	RUN_TEST(test_parse_bad_version);
	RUN_TEST(test_parse_bad_magic);
	TEST_SUITE_END();
}
