/*
 * Copyright 2022 zhenwei pi
 *
 * Authors:
 *   zhenwei pi <pizhenwei@bytedance.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cache.h"

#define RECORDS_TRUNK (60 * 60 / 10)

static int nr_caches;
static struct cache_t **caches;

struct cache_t *cache_find(const char *name)
{
	for (int i = 0; i < nr_caches; i++) {
		struct cache_t *cache = caches[i];
		assert(cache);
		if (!strcmp(cache->name, name))
			return cache;
	}

	return NULL;
}

struct cache_t *cache_alloc(const char *name)
{
	struct cache_t *cache = cache_find(name);

	assert(!cache);

	cache = calloc(1, sizeof(*cache));
	assert(cache);

	cache->name = strdup(name);
	assert(cache->name);

	cache->nr_elems = 0;
	cache->max_elems = RECORDS_TRUNK;
	cache->elems = calloc(cache->max_elems, sizeof(struct cache_elem_t));
	assert(cache->elems);

	if (!caches)
		caches = calloc(1, sizeof(struct cache_t *));
	else
		caches = realloc(caches, sizeof(struct cache_t *) * (nr_caches + 1));

	caches[nr_caches++] = cache;

	return cache;
}

void cache_free(const char *name)
{
	struct cache_t *cache = cache_find(name);

	if (!cache)
		return;

	free(cache->elems);
	free(cache->name);
	free(cache);

	for (int i = 0; i < nr_caches; i++) {
		if (caches[i] == cache) {
			memmove(&caches[i], &caches[i + 1], (nr_caches - i - 1) * sizeof(caches[0]));
			break;
		}
	}

	nr_caches--;
	caches = realloc(caches, sizeof(struct cache_t *) * (nr_caches));
}

static struct cache_t *__cache_get(time_t time)
{
	for (int i = 0; i < nr_caches; i++) {
		struct cache_t *cache = caches[i];
		struct cache_elem_t *first_elem, *last_elem;

		assert(cache->nr_elems);
		first_elem = &cache->elems[0];
		last_elem = &cache->elems[cache->nr_elems - 1];
		if ((time >= first_elem->time) && (time <= last_elem->time))
			return cache;
	}

	return NULL;
}

struct cache_t *cache_get(time_t time, off_t *off)
{
	struct cache_t *cache = __cache_get(time);

	if (!cache)
		return NULL;

	if (cache->nr_elems <= 2) {
		*off = cache->elems[0].off;
		return cache;
	}

	int left = 0;
	int right = cache->nr_elems;

	while (left < right) {
		int mid = (left + right) >> 1;
		struct cache_elem_t *elem = &cache->elems[mid];
		if (time == elem->time) {
			*off = elem->off;
			return cache;
		}

		if (time < elem->time) {
			right = mid;
		} else {
			left = mid + 1;
		}
	}

	struct cache_elem_t *elem = &cache->elems[left];
	*off = elem->off;
	return cache;
}

static int cache_elems_cmp(const void *p1, const void *p2)
{
	struct cache_elem_t *e1 = (struct cache_elem_t *)p1;
	struct cache_elem_t *e2 = (struct cache_elem_t *)p2;

	return (e1->time > e2->time) - (e1->time < e2->time);
}

void cache_set(struct cache_t *cache, time_t time, off_t off)
{
	if (cache->nr_elems == cache->max_elems) {
		cache->max_elems += RECORDS_TRUNK;
		cache->elems = realloc(cache->elems, sizeof(struct cache_elem_t) * cache->max_elems);
	}

	struct cache_elem_t *new_elem = &cache->elems[cache->nr_elems++];
	new_elem->time = time;
	new_elem->off = off;

	if (cache->nr_elems == 1)
		return;

	struct cache_elem_t *last_elem = &cache->elems[cache->nr_elems - 2];
	if (last_elem->time > new_elem->time) {
		qsort(cache->elems, cache->nr_elems, sizeof(struct cache_elem_t), cache_elems_cmp);
	}
}

static int cache_cmp(const void *p1, const void *p2)
{
	struct cache_t **c1 = (struct cache_t **)p1;
	struct cache_t **c2 = (struct cache_t **)p2;

	return ((*c1)->elems[0].time > (*c2)->elems[0].time) -
	       ((*c1)->elems[0].time < (*c2)->elems[0].time);
}

void cache_sort()
{
	/* qsort() is declared nonnull: skip it when there is nothing to sort
	 * (caches is NULL while no cache has been allocated yet). */
	if (nr_caches < 2)
		return;

	qsort(caches, nr_caches, sizeof(struct cache_t *), cache_cmp);
}

struct cache_t *cache_get_recent()
{
	if (!nr_caches)
		return NULL;

	return caches[nr_caches - 1];
}
