/*
 * Copyright 2022 zhenwei pi
 *
 * Authors:
 *   zhenwei pi <pizhenwei@bytedance.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/types.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#include "config.h"

#include "atop.h"
#include "cache.h"
#include "httpd.h"
#include "json.h"
#include "log.h"
#include "rawlog.h"

/* a little tricky: implemented in atop/version.c */
unsigned short getnumvers(void);

static int rawlog_verify_rawheader(struct rawheader *rh)
{
	if (rh->magic != MYMAGIC)
		return -EINVAL;

	if ((rh->rawheadlen != sizeof(struct rawheader)) ||
		(rh->rawreclen != sizeof(struct rawrecord)) ||
		(rh->sstatlen != sizeof(struct sstat)) ||
		(rh->tstatlen != sizeof(struct tstat))) {
		return -EINVAL;
	}

	if (rh->aversion & 0x8000 && (rh->aversion & 0x7fff) != getnumvers())
		return -EINVAL;

	return 0;
}

static int rawlog_rebuild_one(struct cache_t *cache)
{
	struct cache_elem_t *elem = &cache->elems[cache->nr_elems - 1];
	struct rawrecord rr;
	int ret = 0;

	int fd = open(cache->name, O_RDONLY);
	if (fd < 0) {
		log_err("%s: open \"%s\" failed: %m\n", __func__, cache->name);
		return -errno;
	}

	off_t off = elem->off;
	ssize_t len = pread(fd, &rr, sizeof(rr), off);
	if (len < sizeof(rr))
		goto close_fd;

	off = off + sizeof(rr) + rr.scomplen + rr.pcomplen + rr.ccomplen + rr.icomplen;
	while (1) {
		len = pread(fd, &rr, sizeof(rr), off);
		if (len < sizeof(rr))
			break;

		cache_set(cache, rr.curtime, off);
		off = off + sizeof(rr) + rr.scomplen + rr.pcomplen + rr.ccomplen + rr.icomplen;
	}

	log_debug("\"%s\" has %d records of time[%ld - %ld]\n", cache->name,
		cache->nr_elems, cache->elems[0].time, cache->elems[cache->nr_elems - 1].time);

	struct stat statbuf;
	ret = fstat(fd, &statbuf);
	if (ret < 0) {
		log_err("%s: fstat \"%s\" failed: %m\n", __func__, cache->name);
		ret = -errno;
		goto close_fd;
	}

	cache->st_size = statbuf.st_size;
	cache->st_mtim = statbuf.st_mtim;

close_fd:
	close(fd);
	return ret;
}

static int rawlog_parse_one(const char *path)
{
	struct rawheader rh;
	struct rawrecord rr;
	struct stat statbuf;
	struct cache_t *cache;
	ssize_t len;
	off_t off;
	int fd;
	int ret;

	/* if we have already build a cache, rebuild it */
	cache = cache_find(path);
	if (cache) {
		ret = stat(path, &statbuf);
		if (ret < 0) {
			log_err("%s: stat \"%s\" failed: %m\n", __func__, path);
			return -errno;
		}

		if (cache->st_size == statbuf.st_size) {
			return 0;
		}

		return rawlog_rebuild_one(cache);
	}

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		log_err("%s: open \"%s\" failed: %m\n", __func__, path);
		return -errno;
	}

	/* 1, read rawheader */
	len = read(fd, &rh, sizeof(rh));
	if (len < sizeof(rh)) {
		log_err("%s: \"%s\" is not atop rawlog file\n", __func__, path);
		ret = -EINVAL;
		goto close_fd;
	}

	ret = rawlog_verify_rawheader(&rh);
	if (ret) {
		log_err("%s: \"%s\" verify rawheader failed\n", __func__, path);
		goto close_fd;
	}

	/* 2, create cache for this file */
	cache = cache_alloc(path);
	if (!cache) {
		ret = -ENOMEM;
		goto close_fd;
	}
	cache->flags = rh.supportflags;

	/* 3, read all rawrecords, cache time&off mapping */
	off = lseek(fd, 0, SEEK_CUR);
	if (off < 0) {
		log_err("%s: move offset failed\n", __func__);
		ret = -errno;
		goto close_fd;
	}
	while (1) {
		len = pread(fd, &rr, sizeof(rr), off);
		if (len < sizeof(rr))
			break;

		cache_set(cache, rr.curtime, off);
		off = off + sizeof(rr) + rr.scomplen + rr.pcomplen + rr.ccomplen + rr.icomplen;
	}

	/* 4, update rawlog size & st_mtim */
	ret = fstat(fd, &statbuf);
	if (ret < 0) {
		log_err("%s: fstat \"%s\" failed: %m\n", __func__, path);
		ret = -errno;
		goto close_fd;
	}
	cache->st_size = statbuf.st_size;
	cache->st_mtim = statbuf.st_mtim;

	if (!cache->nr_elems)
		goto free_cache;

	log_debug("\"%s\" has %d records of time[%ld - %ld]\n", path, cache->nr_elems,
		cache->elems[0].time, cache->elems[cache->nr_elems - 1].time);
	ret = 0;
	goto close_fd;

free_cache:
	cache_free(path);

close_fd:
	close(fd);

	return ret;
}

int rawlog_parse_all(const char *path)
{
	struct dirent *dirent;
	DIR *dir = opendir(path);
	char name[PATH_MAX] = {0};

	if (!dir) {
		log_err("%s: open \"%s\" failed: %m\n", __func__, path);
		return -errno;
	}

	/* Single writer: exclude all cache readers while the array is rewritten. */
	cache_wrlock();
	while ((dirent = readdir(dir))) {
		if (dirent->d_type != DT_REG)
			continue;

		/* skip entries whose full path would not fit (prevents overflow) */
		if (snprintf(name, sizeof(name), "%s/%s", path, dirent->d_name) < (int)sizeof(name))
			rawlog_parse_one(name);
	}

	cache_sort();
	cache_unlock();

	closedir(dir);

	return 0;
}

static int rawlog_uncompress_record(int fd, void *outbuf, unsigned long *outlen, unsigned long inlen)
{
	Byte *inbuf;
	int ret = 0;

	inbuf = malloc(inlen);
	if (!inbuf)
		return -ENOMEM;

	if (read(fd, inbuf, inlen) != inlen) {
		ret = -EIO;
		goto free_buf;
	}

	if (uncompress(outbuf, outlen, inbuf, inlen) != Z_OK)
		ret = -ENODATA;

free_buf:
	free(inbuf);

	return ret;
}

static int rawlog_get_sstat(int fd, struct sstat *sstat, unsigned long len)
{
	unsigned long outlen = sizeof(struct sstat);

	return rawlog_uncompress_record(fd, sstat, &outlen, len);
}

static void rawlog_free_devtstat(struct devtstat *devtstat)
{
	free(devtstat->taskall);
	free(devtstat->procall);
	free(devtstat->procactive);
}

static int rawlog_get_devtstat(int fd, struct devtstat *devtstat, struct rawrecord *rr)
{
	unsigned long outlen = sizeof(struct tstat) * rr->ndeviat;
	int ret;
	unsigned long ntaskall = 0, nprocall = 0, nprocactive = 0, ntaskactive = 0;

	/* Reject implausible counts before they size any allocation: a sample
	 * holds at most ndeviat tasks, so it cannot contain more processes
	 * (totproc) or active processes (nactproc) than that, and zlib cannot
	 * expand the compressed task blob by more than ~1032x, so it cannot
	 * decompress to more tstats than that ratio allows. With pcomplen already
	 * bounded to the file size by the caller, this bounds every allocation
	 * below (and the process pointer arrays are sized by ndeviat so the fill
	 * loop can never overrun them). */
	if (rr->totproc > rr->ndeviat || rr->nactproc > rr->ndeviat ||
	    outlen > (unsigned long)rr->pcomplen * 1032 + sizeof(struct tstat))
		return -EINVAL;

	/* 1, allocate memory (taskall is calloc'd so a short decompression
	 * cannot leave the fill loop reading indeterminate task flags) */
	memset(devtstat, 0x00, sizeof(struct devtstat));
	devtstat->taskall = calloc(rr->ndeviat, sizeof(struct tstat));
	if (!devtstat->taskall) {
		ret = -ENOMEM;
		goto free_devtstat;
	}

	devtstat->procall = malloc(sizeof(struct tstat*) * rr->ndeviat);
	if (!devtstat->procall) {
		ret = -ENOMEM;
		goto free_devtstat;
	}

	devtstat->procactive = malloc(sizeof(struct tstat*) * rr->ndeviat);
	if (!devtstat->procactive) {
		ret = -ENOMEM;
		goto free_devtstat;
	}

	/* 2, read record and uncompress */
	ret = rawlog_uncompress_record(fd, devtstat->taskall, &outlen, rr->pcomplen);
	if (ret)
		goto free_devtstat;

	/* 3, build devtstat */
	for ( ; ntaskall < rr->ndeviat; ntaskall++)
	{
		struct tstat *tstat = devtstat->taskall + ntaskall;
		if (tstat->gen.isproc)
		{
			devtstat->procall[nprocall++] = tstat;
			if (!tstat->gen.wasinactive)
				devtstat->procactive[nprocactive++] = tstat;
		}

		if (!tstat->gen.wasinactive)
			ntaskactive++;
	}

	devtstat->ntaskall = ntaskall;
	devtstat->nprocall = nprocall;
	devtstat->nprocactive = nprocactive;
	devtstat->ntaskactive = ntaskactive;
	devtstat->totrun = rr->totrun;
	devtstat->totslpi = rr->totslpi;
	devtstat->totslpu = rr->totslpu;
	devtstat->totzombie = rr->totzomb;


	return 0;

free_devtstat:
	rawlog_free_devtstat(devtstat);

	return ret;
}

static int rawlog_record_flags(int hflags, int rflags)
{
	int ret = 0;

	if (hflags & RAWLOGNG) {
		if (rflags & RRACCTACTIVE)
			ret |= ACCTACTIVE;

		if (rflags & RRIOSTAT)
			ret |= IOSTAT;
	}

	if (rflags & RRNETATOP)
		ret |= NETATOP;

	if (rflags & RRNETATOPD)
		ret |= NETATOPD;

	if (rflags & RRCGRSTAT)
		ret |= CGROUPV2;

	if (rflags & RRCONTAINERSTAT)
		ret |= CONTAINERSTAT;

	if (rflags & RRGPUSTAT)
		ret |= GPUSTAT;

	return ret;
}

int rawlog_get_record(time_t ts, char *labels, connection *conn, struct output *op)
{
	struct rawrecord rr;
	struct sstat *sstat;
	struct devtstat devtstat;
	ssize_t len;
	int fd;
	int ret = 0;
	int flags;
	off_t off;
	time_t recent_ts;
	char *name = NULL;
	int cache_flags = 0;

	sstat = malloc(sizeof(struct sstat));
	if (sstat == NULL) {
		log_debug("can't alloc mem for sstat\n");
		return -ENOMEM;
	}

	/*
	 * Resolve the record under the cache read lock, then copy out everything
	 * the rest of the function needs (the file name, the record offset and the
	 * header flags) and release the lock *before* any file I/O or the network
	 * send. Holding the read lock across those would let a single slow-reading
	 * client keep it for up to the socket send timeout, stalling the cache
	 * writer -- and therefore the acceptor thread that takes the write lock --
	 * which would stop new connections from being accepted. The raw-log file
	 * stays valid on disk by path even if the writer later frees or rebuilds
	 * the cache entry, so opening it lock-free is safe.
	 */
	cache_rdlock();
	struct cache_t *cache = cache_get(ts, &off);
	if (!cache) {
		log_debug("no record @%ld\n", ts);

		cache = cache_get_recent();
		if (!cache) {
			cache_unlock();
			ret = -EIO;
			goto free_sstat;
		}

		recent_ts = cache->elems[cache->nr_elems - 1].time;
		if (ts < recent_ts) {
			cache_unlock();
			ret = -EIO;
			goto free_sstat;
		}

		off = cache->elems[cache->nr_elems - 1].off;
		log_debug("use recent @%ld from %s\n", cache->elems[cache->nr_elems - 1].time, cache->name);
	}

	name = strdup(cache->name);
	cache_flags = cache->flags;
	cache_unlock();

	if (!name) {
		ret = -ENOMEM;
		goto free_sstat;
	}

	log_debug("time %ld, off %ld in %s\n", ts, off, name);

	fd = open(name, O_RDONLY);
	if (fd < 0) {
		log_err("%s: open \"%s\" failed: %m\n", __func__, name);
		ret = -errno;
		goto free_name;
	}

	ret = lseek(fd, off, SEEK_CUR);
	if (ret < 0) {
		log_err("%s: move offset failed\n", __func__);
		ret = -errno;
		goto close_fd;
	}

	len = read(fd, &rr, sizeof(rr));
	if (len != sizeof(rr)) {
		log_err("%s: time %ld, off %ld in %s, incomplete record\n", __func__, ts, off, name);
		ret = -EIO;
		goto close_fd;
	}
	log_debug("time %ld, off %ld in %s, rr.curtime %ld\n", ts, off, name, rr.curtime);

	/* The record's compressed blobs must physically fit within the file.
	 * Reject lengths that run past EOF before they are used to size any
	 * allocation -- this stops a corrupt or malicious raw-log from driving
	 * an unbounded malloc() in the decompression path below. */
	struct stat rrst;
	if (fstat(fd, &rrst) < 0) {
		log_err("%s: fstat \"%s\" failed: %m\n", __func__, name);
		ret = -errno;
		goto close_fd;
	}
	off_t body = off + (off_t)sizeof(rr);
	unsigned long bloblen = (unsigned long)rr.scomplen + rr.pcomplen +
	                        rr.ccomplen + rr.icomplen;
	if (body > rrst.st_size || bloblen > (unsigned long)(rrst.st_size - body)) {
		log_err("%s: time %ld, off %ld in %s, record exceeds file bounds\n",
			__func__, ts, off, name);
		ret = -EINVAL;
		goto close_fd;
	}

	ret = rawlog_get_sstat(fd, sstat, rr.scomplen);
	if (ret) {
		log_err("%s: time %ld, off %ld in %s, get sstat failed\n", __func__, ts, off, name);
		goto close_fd;
	}

	ret = rawlog_get_devtstat(fd, &devtstat, &rr);
	if (ret) {
		log_err("%s: time %ld, off %ld in %s, get devtstat failed\n", __func__, ts, off, name);
		goto close_fd;
	}

	flags = rawlog_record_flags(cache_flags, rr.flags);
	jsonout(flags, labels, rr.curtime, rr.interval, &devtstat, sstat, rr.nexit, rr.noverflow, 0, conn, op);

	rawlog_free_devtstat(&devtstat);

close_fd:
	close(fd);
free_name:
	free(name);
free_sstat:
	free(sstat);

	return ret;
}
