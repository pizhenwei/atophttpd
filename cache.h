/*
 * Copyright 2022 zhenwei pi
 *
 * Authors:
 *   zhenwei pi <pizhenwei@bytedance.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef _CACHE_H_
#define _CACHE_H_

#include <sys/types.h>
#include <time.h>

struct cache_elem_t {
	time_t time;
	off_t off;
};

struct cache_t {
	char *name;
	int flags;
	int max_elems;
	int nr_elems;
	struct cache_elem_t *elems;
	off_t st_size;
	struct timespec st_mtim;
};

struct cache_t *cache_alloc(const char *name);
struct cache_t *cache_find(const char *name);
void cache_free(const char *name);
void cache_set(struct cache_t *cache, time_t time, off_t off);
struct cache_t *cache_get(time_t time, off_t *off);
void cache_done(struct cache_t *cache);
void cache_sort();
struct cache_t *cache_get_recent();

#endif
