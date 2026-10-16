/*
 * Unit tests for cache.c (time -> file-offset index used by rawlog parsing).
 *
 * Exercises allocation/lookup, in-order and out-of-order insertion (which
 * drives the qsort comparators), binary-search lookup, buffer growth,
 * multi-file ordering and removal.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */

#include <stdlib.h>

#include "test_framework.h"
#include "cache.h"

#ifdef CACHE_FAULT_INJECTION
/*
 * Allocation fault injection (coverage build only).
 *
 * Each counter is a countdown of successful allocations before the next one is
 * forced to fail; -1 disables it, and a forced failure re-arms it to -1 so a
 * single call is affected. Arm the counter immediately before the call under
 * test, with no intervening allocation (no CHECK/printf that could alloc), so
 * only the targeted allocation consumes it.
 *
 * Enabled only in the non-sanitised coverage/unit build: linker --wrap is
 * incompatible with AddressSanitizer's own allocator interception, so the
 * sanitiser and TSan builds compile this out and run the base suite.
 */
extern void *__real_calloc(size_t nmemb, size_t size);
extern void *__real_realloc(void *ptr, size_t size);
extern char *__real_strdup(const char *s);

static int fail_calloc_at = -1;
static int fail_realloc_at = -1;
static int fail_strdup_at = -1;

void *__wrap_calloc(size_t nmemb, size_t size)
{
	if (fail_calloc_at >= 0 && fail_calloc_at-- == 0)
		return NULL;
	return __real_calloc(nmemb, size);
}

void *__wrap_realloc(void *ptr, size_t size)
{
	if (fail_realloc_at >= 0 && fail_realloc_at-- == 0)
		return NULL;
	return __real_realloc(ptr, size);
}

char *__wrap_strdup(const char *s)
{
	if (fail_strdup_at >= 0 && fail_strdup_at-- == 0)
		return NULL;
	return __real_strdup(s);
}
#endif /* CACHE_FAULT_INJECTION */

static void test_find_empty(void)
{
	CHECK(cache_find("nope") == NULL);
	CHECK(cache_get_recent() == NULL);
}

static void test_alloc_and_find(void)
{
	const struct cache_t *c = cache_alloc("fileA");
	CHECK(c != NULL);
	CHECK(cache_find("fileA") == c);
	CHECK(cache_find("other") == NULL);
	cache_free("fileA");
	CHECK(cache_find("fileA") == NULL);
}

static void test_set_in_order(void)
{
	struct cache_t *c = cache_alloc("ord");
	cache_set(c, 100, 1000);
	cache_set(c, 101, 1001);
	cache_set(c, 102, 1002);
	CHECK_EQ_INT(c->nr_elems, 3);
	CHECK_EQ_INT(c->elems[0].time, 100);
	CHECK_EQ_INT(c->elems[2].time, 102);
	cache_free("ord");
}

/* Out-of-order insertion must be re-sorted (exercises cache_elems_cmp). */
static void test_set_out_of_order(void)
{
	struct cache_t *c = cache_alloc("unord");
	cache_set(c, 100, 1000);
	cache_set(c, 102, 1002);
	cache_set(c, 101, 1001);
	cache_set(c, 50, 500);
	CHECK_EQ_INT(c->nr_elems, 4);
	CHECK_EQ_INT(c->elems[0].time, 50);
	CHECK_EQ_INT(c->elems[1].time, 100);
	CHECK_EQ_INT(c->elems[2].time, 101);
	CHECK_EQ_INT(c->elems[3].time, 102);
	cache_free("unord");
}

/* Many shuffled inserts: validates comparator correctness and buffer growth. */
static void test_set_growth_and_sort(void)
{
	struct cache_t *c = cache_alloc("big");
	const int n = 1000;
	/* Deterministic LCG (not rand()): we only need a reproducible shuffle,
	 * not cryptographic randomness. */
	unsigned int seed = 1234;
	for (int i = 0; i < n; i++) {
		seed = seed * 1103515245u + 12345u;
		cache_set(c, (seed >> 16) % 100000, i);
	}
	CHECK_EQ_INT(c->nr_elems, n);
	int sorted = 1;
	for (int i = 1; i < c->nr_elems; i++)
		if (c->elems[i - 1].time > c->elems[i].time)
			sorted = 0;
	CHECK(sorted);
	cache_free("big");
}

static void test_get_exact_and_approx(void)
{
	struct cache_t *c = cache_alloc("get");
	cache_set(c, 100, 1000);
	cache_set(c, 110, 1100);
	cache_set(c, 120, 1200);
	cache_set(c, 130, 1300);

	off_t off = -1;
	/* exact match */
	CHECK(cache_get(110, &off) == c);
	CHECK_EQ_INT(off, 1100);

	/* approximate: 115 -> next sample (120) */
	off = -1;
	CHECK(cache_get(115, &off) == c);
	CHECK_EQ_INT(off, 1200);

	/* lower bound exact */
	off = -1;
	CHECK(cache_get(100, &off) == c);
	CHECK_EQ_INT(off, 1000);

	/* out of range -> NULL */
	off = -1;
	CHECK(cache_get(50, &off) == NULL);
	CHECK(cache_get(200, &off) == NULL);

	cache_free("get");
}

/* The <= 2 element fast path in cache_get. */
static void test_get_small(void)
{
	struct cache_t *c = cache_alloc("small");
	cache_set(c, 100, 1000);
	cache_set(c, 200, 2000);
	off_t off = -1;
	CHECK(cache_get(150, &off) == c);
	CHECK_EQ_INT(off, 1000);
	cache_free("small");
}

/* Multiple files are ordered by their first sample (exercises cache_cmp). */
static void test_sort_and_recent(void)
{
	struct cache_t *c1 = cache_alloc("day3");
	cache_set(c1, 3000, 1);
	struct cache_t *c2 = cache_alloc("day1");
	cache_set(c2, 1000, 1);
	struct cache_t *c3 = cache_alloc("day2");
	cache_set(c3, 2000, 1);

	cache_sort();

	/* most recent file is the one with the greatest first timestamp */
	const struct cache_t *recent = cache_get_recent();
	CHECK(recent == c1);
	CHECK_EQ_INT(recent->elems[0].time, 3000);

	cache_free("day1");
	cache_free("day2");
	cache_free("day3");
	CHECK(cache_get_recent() == NULL);
}

/* Removing a middle element keeps the remaining caches intact. */
static void test_free_middle(void)
{
	cache_alloc("a");
	cache_alloc("b");
	cache_alloc("c");
	cache_free("b");
	CHECK(cache_find("a") != NULL);
	CHECK(cache_find("b") == NULL);
	CHECK(cache_find("c") != NULL);
	/* freeing a non-existent name is a no-op */
	cache_free("zzz");
	cache_free("a");
	cache_free("c");
}

#ifdef CACHE_FAULT_INJECTION
/* The cache_t struct allocation fails: nothing is registered. */
static void test_alloc_oom_struct(void)
{
	fail_calloc_at = 0;
	CHECK(cache_alloc("oom1") == NULL);
	fail_calloc_at = -1;
	CHECK(cache_find("oom1") == NULL);
}

/* The name strdup fails: the struct is freed, nothing is registered. */
static void test_alloc_oom_name(void)
{
	fail_strdup_at = 0;
	CHECK(cache_alloc("oom2") == NULL);
	fail_strdup_at = -1;
	CHECK(cache_find("oom2") == NULL);
}

/* The elems array allocation fails (second calloc): partials are freed. */
static void test_alloc_oom_elems(void)
{
	fail_calloc_at = 1;
	CHECK(cache_alloc("oom3") == NULL);
	fail_calloc_at = -1;
	CHECK(cache_find("oom3") == NULL);
}

/* The caches[] growth realloc fails: the half-built cache is freed, the
 * existing array is untouched (no pointer loss). */
static void test_alloc_oom_array(void)
{
	CHECK(cache_alloc("seed") != NULL);
	fail_realloc_at = 0;
	CHECK(cache_alloc("oom4") == NULL);
	fail_realloc_at = -1;
	CHECK(cache_find("oom4") == NULL);
	CHECK(cache_find("seed") != NULL);
	cache_free("seed");
}

/* The elems growth realloc fails: the record is dropped, not lost/corrupted. */
static void test_set_oom_grow(void)
{
	struct cache_t *c = cache_alloc("setoom");
	REQUIRE(c != NULL);
	int max = c->max_elems;
	for (int i = 0; i < max; i++)
		cache_set(c, i, i);
	fail_realloc_at = 0;
	cache_set(c, max, max);
	fail_realloc_at = -1;
	CHECK_EQ_INT(c->nr_elems, max);
	cache_free("setoom");
}
#endif /* CACHE_FAULT_INJECTION */

int main(void)
{
	TEST_SUITE_BEGIN("cache");
	RUN_TEST(test_find_empty);
	RUN_TEST(test_alloc_and_find);
	RUN_TEST(test_set_in_order);
	RUN_TEST(test_set_out_of_order);
	RUN_TEST(test_set_growth_and_sort);
	RUN_TEST(test_get_exact_and_approx);
	RUN_TEST(test_get_small);
	RUN_TEST(test_sort_and_recent);
	RUN_TEST(test_free_middle);
#ifdef CACHE_FAULT_INJECTION
	RUN_TEST(test_alloc_oom_struct);
	RUN_TEST(test_alloc_oom_name);
	RUN_TEST(test_alloc_oom_elems);
	RUN_TEST(test_alloc_oom_array);
	RUN_TEST(test_set_oom_grow);
#endif
	TEST_SUITE_END();
}
