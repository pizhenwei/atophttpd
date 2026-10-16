/*
 * libFuzzer harness for the atop raw-log binary parser (rawlog.c).
 *
 * This is the highest-value target: atophttpd reads atop's version-specific
 * binary raw-log format (header, per-sample records, zlib-compressed sstat and
 * tstat blobs). The harness writes each fuzz input to a scratch file, runs the
 * directory parser's per-file path rawlog_parse_one() on it, and -- when a
 * cache was built -- decodes the first and last sample via rawlog_get_record(),
 * exercising the decompression and devtstat reconstruction code.
 *
 * rawlog_parse_one() is file-static, so rawlog.c is #included directly here to
 * reach it (and to keep the record-decode path in one translation unit).
 *
 * A valid raw-log seed is essential: the header carries a magic, exact struct
 * sizes and the running atop version, which a blind mutator cannot synthesise.
 * tests/fuzz.sh seeds the corpus from a freshly generated atop sample.
 *
 * Build: see tests/fuzz.sh ("make fuzz").
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */

#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "cache.h"
#include "output.h"

/* Pull in the parser, including its file-static rawlog_parse_one(). The "../"
 * prefix is required: -Iatop puts atop's own rawlog.c on the quoted-include
 * search path, so a bare "rawlog.c" would wrongly resolve to that one. */
#include "../rawlog.c"

/* globals normally provided by httpd.c (mirrors tests/unit/test_rawlog.c) */
int __debug = 0;
unsigned int pagesize = 4096;
unsigned short hertz = 100;
struct utsname utsname;
int hidecmdline = 0;

static char tmppath[] = "/tmp/atophttpd_fuzz_rawlog_XXXXXX";

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
	(void)argc;
	(void)argv;
	uname(&utsname);
	int fd = mkstemp(tmppath);
	if (fd >= 0)
		close(fd);
	return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	int fd = open(tmppath, O_WRONLY | O_TRUNC | O_CREAT | O_NOFOLLOW, 0600);
	if (fd < 0)
		return 0;
	ssize_t w = write(fd, data, size);
	(void)w;
	close(fd);

	if (rawlog_parse_one(tmppath) == 0) {
		struct cache_t *c = cache_get_recent();
		if (c && c->nr_elems) {
			char labels[] = "CPU,MEM,DSK,NET";
			char buf[sizeof(labels)];
			struct output op = { .output_type = OUTPUT_BUF, .done = NULL };

			memcpy(buf, labels, sizeof(labels));
			rawlog_get_record(c->elems[0].time, buf, NULL, &op);
			free(op.ob.buf);
			op.ob.buf = NULL;
			op.ob.size = 0;
			op.ob.offset = 0;

			memcpy(buf, labels, sizeof(labels));
			rawlog_get_record(c->elems[c->nr_elems - 1].time, buf, NULL, &op);
			free(op.ob.buf);
		}
	}

	/* reset the global cache state so each input is parsed from scratch */
	cache_free(tmppath);
	return 0;
}
