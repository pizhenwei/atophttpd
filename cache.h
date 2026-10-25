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

/*
 * Reader/writer lock protecting the whole cache module (the global cache
 * array and the contents of every struct cache_t). Request handlers run on a
 * pool of worker threads and read the cache concurrently (cache_rdlock); the
 * single cache updater rewrites it under cache_wrlock. A reader must hold the
 * lock for as long as it dereferences a cache_t it obtained, not just across
 * the lookup, since the writer may free that cache_t.
 */
void cache_rdlock(void);
void cache_wrlock(void);
void cache_unlock(void);

#endif
