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
#include <pthread.h>
#include <stddef.h>
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
int rawlog_get_record(time_t ts, char *labels, void *conn, struct output *op);

/* globals normally provided by httpd.c */
int __debug = 0;
unsigned int pagesize = 4096;
unsigned short hertz = 100;
struct utsname utsname;
int hidecmdline = 0;

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

/*
 * Open the developer-provided sample raw-log read-only. All access to the
 * sample goes through this single call site, with O_NOFOLLOW to refuse
 * symlinks and no prior stat()/access() check, so there is no
 * time-of-check/time-of-use window. Returns -1 when no sample is configured.
 */
static int open_sample_ro(void)
{
	const char *s = getenv("RAWLOG_SAMPLE");
	if (!s)
		return -1;
	return open(s, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
}

static int load_sample_into_tmpdir(void)
{
	int in = open_sample_ro();
	if (in < 0)
		return 0;

	setup_tmpdir();
	char dst[512];
	snprintf(dst, sizeof(dst), "%s/atop_sample", tmpdir);

	int out = open(dst, O_CREAT | O_WRONLY | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (out < 0) {
		close(in);
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
	struct output op = { .output_type = OUTPUT_BUF, .done = NULL };

	/* a valid timestamp produces JSON output into op */
	strcpy(labels, "CPU,MEM");
	int ret = rawlog_get_record(first_sample_time, labels, NULL, &op);
	CHECK_EQ_INT(ret, 0);
	CHECK(op.ob.offset == 0); /* reset by output_samp_done */
	free(op.ob.buf);
	op.ob.buf = NULL;
	op.ob.size = 0;
	op.ob.offset = 0;

	/* a far-future timestamp falls back to the most recent sample */
	strcpy(labels, "CPU");
	ret = rawlog_get_record(last_sample_time + 100000, labels, NULL, &op);
	CHECK_EQ_INT(ret, 0);
	free(op.ob.buf);
}

/*
 * Read the (valid) rawheader from the sample file so crafted headers can be
 * derived from it.  Returns 0 if no sample is available.
 */
static int read_sample_header(struct rawheader *rh)
{
	int fd = open_sample_ro();
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
	int fd = open(path, O_CREAT | O_WRONLY | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd >= 0) {
		ssize_t w = write(fd, rh, sizeof(*rh));
		(void)w;
		close(fd);
	}
}

/*
 * Take the valid sample header, optionally corrupt it, write it on its own in
 * a fresh directory and confirm parse_all tolerates the rejected file (the
 * NULL corrupter yields a valid header-only file, exercising free_cache).
 */
static void check_header_variant(void (*corrupt)(struct rawheader *))
{
	struct rawheader rh;
	if (!read_sample_header(&rh)) {
		printf("    (skipped: no RAWLOG_SAMPLE provided)\n");
		return;
	}
	if (corrupt)
		corrupt(&rh);
	char dir[256];
	write_header_dir(dir, &rh);
	CHECK_EQ_INT(rawlog_parse_all(dir), 0);
}

static void corrupt_headlen(struct rawheader *rh)
{
	rh->rawheadlen = rh->rawheadlen + 1; /* corrupt the declared length */
}

static void corrupt_version(struct rawheader *rh)
{
	rh->aversion = 0xFFFF; /* MSB set, 0x7fff: cannot match getnumvers() */
}

static void corrupt_magic(struct rawheader *rh)
{
	rh->magic = 0xdeadbeef; /* not MYMAGIC */
}

/* A file with a valid header but no records exercises the free_cache path. */
static void test_parse_header_only(void)
{
	check_header_variant(NULL);
}

/* A header whose declared length is wrong must fail verification. */
static void test_parse_bad_headlen(void)
{
	check_header_variant(corrupt_headlen);
}

/* A header advertising an incompatible atop version must fail verification. */
static void test_parse_bad_version(void)
{
	check_header_variant(corrupt_version);
}

/* A full-size header with a wrong magic must fail verification. */
static void test_parse_bad_magic(void)
{
	check_header_variant(corrupt_magic);
}

/*
 * Copy the sample raw-log into a fresh directory, overwrite a 32-bit field in
 * its first record, then confirm the parser and record decoder reject the
 * record gracefully (negative return, no crash) instead of trusting an
 * attacker-controlled length or count to size an allocation.
 */
static void check_corrupt_first_record(size_t field_off, unsigned int value)
{
	int in = open_sample_ro();
	if (in < 0) {
		printf("    (skipped: no RAWLOG_SAMPLE provided)\n");
		return;
	}

	/* Drop every cache built by earlier tests: cache_get() looks up records
	 * by time range across all files, and the corrupt record's timestamps
	 * overlap the valid sample's, so a stale cache would otherwise serve a
	 * good record and hide the rejection we want to verify. */
	struct cache_t *stale;
	while ((stale = cache_get_recent()) != NULL)
		cache_free(stale->name);

	static unsigned char buf[1 << 20];
	ssize_t n = read(in, buf, sizeof(buf));
	close(in);
	if (n < (ssize_t)(sizeof(struct rawheader) + sizeof(struct rawrecord)))
		return;

	memcpy(buf + sizeof(struct rawheader) + field_off, &value, sizeof(value));

	char dir[256];
	tf_tmpl(dir, sizeof(dir), "atophttpd_corrupt_XXXXXX");
	CHECK(mkdtemp(dir) != NULL);
	char path[512];
	snprintf(path, sizeof(path), "%s/atop_corrupt", dir);
	int out = open(path, O_CREAT | O_WRONLY | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	CHECK(out >= 0);
	if (out >= 0) {
		ssize_t w = write(out, buf, n);
		(void)w;
		close(out);
	}

	CHECK_EQ_INT(rawlog_parse_all(dir), 0);
	const struct cache_t *c = cache_find(path);
	CHECK(c != NULL);
	if (c && c->nr_elems) {
		char labels[] = "CPU";
		struct output op = { .output_type = OUTPUT_BUF, .done = NULL };
		int ret = rawlog_get_record(c->elems[0].time, labels, NULL, &op);
		CHECK(ret < 0); /* rejected, not crashed */
		free(op.ob.buf);
	}
}

/* A record whose declared compressed length runs past EOF must be rejected. */
static void test_record_huge_scomplen(void)
{
	check_corrupt_first_record(offsetof(struct rawrecord, scomplen), 0xfffffff0u);
}

/* A record claiming more processes than tasks must be rejected. */
static void test_record_huge_totproc(void)
{
	check_corrupt_first_record(offsetof(struct rawrecord, totproc), 0xfffffff0u);
}

/* ------------------------------------------------------------------ */
/* Concurrency stress: many reader threads decode records (cache_rdlock)
 * while a writer thread keeps adding new log files, each of which forces a
 * cache_alloc (realloc of the global caches array) + cache_sort under
 * cache_wrlock. This is the exact window the cache reader/writer lock must
 * serialise: without it a reader iterating the caches array in __cache_get()
 * would dereference freed/moved memory. Run under ASan it surfaces as a
 * use-after-free; run under ThreadSanitizer it surfaces as a data race. */

struct churn_ctx {
	char dir[256];
	time_t ts;
	int stop;
};

static int copy_sample_to(const char *path)
{
	int in = open_sample_ro();
	if (in < 0)
		return 0;

	int out = open(path, O_CREAT | O_WRONLY | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (out < 0) {
		close(in);
		return 0;
	}

	char buf[65536];
	ssize_t n;
	int ok = 1;
	while ((n = read(in, buf, sizeof(buf))) > 0) {
		if (write(out, buf, n) != n) {
			ok = 0;
			break;
		}
	}
	close(in);
	close(out);
	return ok;
}

static void *churn_reader(void *arg)
{
	struct churn_ctx *c = arg;
	const char labels[] = "CPU,MEM,DSK,NET";

	while (!__atomic_load_n(&c->stop, __ATOMIC_ACQUIRE)) {
		char lb[sizeof(labels)];
		memcpy(lb, labels, sizeof(labels));
		struct output op = { .output_type = OUTPUT_BUF, .done = NULL };
		rawlog_get_record(c->ts, lb, NULL, &op);
		free(op.ob.buf);
	}

	return NULL;
}

/*
 * Number of fresh log files the writer adds (each forces a caches-array
 * realloc + cache_sort under the write lock) and the reader-thread count.
 * Kept deliberately modest so the test stays well within the ~15x time/memory
 * overhead of ThreadSanitizer while still slamming enough realloc cycles
 * against the readers to catch a missing/broken lock. */
#define CHURN_WRITES  12
#define CHURN_READERS 4

static void *churn_writer(void *arg)
{
	struct churn_ctx *c = arg;

	for (int i = 0; i < CHURN_WRITES; i++) {
		char path[512];
		snprintf(path, sizeof(path), "%s/atop_churn_%d", c->dir, i);
		copy_sample_to(path);
		rawlog_parse_all(c->dir);
	}

	__atomic_store_n(&c->stop, 1, __ATOMIC_RELEASE);
	return NULL;
}

static void test_concurrent_cache_access(void)
{
	/* isolate from caches built by earlier tests */
	struct cache_t *stale;
	while ((stale = cache_get_recent()) != NULL)
		cache_free(stale->name);

	struct churn_ctx c = { .stop = 0 };
	tf_tmpl(c.dir, sizeof(c.dir), "atophttpd_churn_XXXXXX");
	if (!mkdtemp(c.dir)) {
		CHECK(0);
		return;
	}

	char base[512];
	snprintf(base, sizeof(base), "%s/atop_base", c.dir);
	if (!copy_sample_to(base)) {
		printf("    (skipped: no RAWLOG_SAMPLE provided)\n");
		return;
	}

	CHECK_EQ_INT(rawlog_parse_all(c.dir), 0);
	const struct cache_t *bc = cache_find(base);
	CHECK(bc != NULL);
	if (!bc || !bc->nr_elems)
		return;
	c.ts = bc->elems[0].time;

	pthread_t readers[CHURN_READERS];
	pthread_t writer;
	int nr = 0;
	for (int i = 0; i < CHURN_READERS; i++) {
		if (pthread_create(&readers[i], NULL, churn_reader, &c) == 0)
			nr++;
	}
	CHECK_EQ_INT(pthread_create(&writer, NULL, churn_writer, &c), 0);

	pthread_join(writer, NULL);
	for (int i = 0; i < nr; i++)
		pthread_join(readers[i], NULL);

	/* reaching here without a sanitizer abort means the lock held */
	CHECK(nr > 0);
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
	RUN_TEST(test_record_huge_scomplen);
	RUN_TEST(test_record_huge_totproc);
	RUN_TEST(test_concurrent_cache_access);
	TEST_SUITE_END();
}
