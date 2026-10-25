/*
 * Minimal unit-test framework for atophttpd.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */

#ifndef _TEST_FRAMEWORK_H_
#define _TEST_FRAMEWORK_H_

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int tf_tests_run;
static int tf_tests_failed;
static int tf_current_failed;

#define TF_COLOR_RED "\033[31m"
#define TF_COLOR_GREEN "\033[32m"
#define TF_COLOR_RESET "\033[0m"

#define RUN_TEST(fn)                                                                               \
    do {                                                                                           \
        tf_current_failed = 0;                                                                     \
        tf_tests_run++;                                                                            \
        fn();                                                                                      \
        if (tf_current_failed) {                                                                   \
            tf_tests_failed++;                                                                     \
            printf(TF_COLOR_RED "  [FAIL] %s" TF_COLOR_RESET "\n", #fn);                           \
        } else {                                                                                   \
            printf(TF_COLOR_GREEN "  [ OK ] %s" TF_COLOR_RESET "\n", #fn);                         \
        }                                                                                          \
    } while (0)

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            tf_current_failed++;                                                                   \
            printf("    assertion failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__);               \
        }                                                                                          \
    } while (0)

/*
 * Like CHECK, but a failed precondition aborts the current (void) test rather
 * than letting execution continue with an invalid value (e.g. a -1 fd or a
 * NULL buffer). Use it for the preconditions that later statements rely on.
 */
#define REQUIRE(cond)                                                                              \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            tf_current_failed++;                                                                   \
            printf("    requirement failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__);             \
            return;                                                                                \
        }                                                                                          \
    } while (0)

/* Recursively remove a directory tree (best-effort; ignores errors). */
static inline void tf_rmrf(const char *path)
{
    DIR *d = opendir(path);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                continue;
            char p[1024];
            snprintf(p, sizeof(p), "%s/%s", path, e->d_name);
            struct stat st;
            if (!lstat(p, &st) && S_ISDIR(st.st_mode))
                tf_rmrf(p);
            else
                unlink(p);
        }
        closedir(d);
    }
    rmdir(path);
}

#define TF_SCRATCH_DIR "./.sonar-test-scratch"

static inline void tf_scratch_cleanup(void)
{
    tf_rmrf(TF_SCRATCH_DIR);
}

/*
 * Build an mkstemp/mkdtemp template inside a private, owner-only scratch
 * directory under the current working tree -- never the shared, world-writable
 * system temp directory ($TMPDIR // /tmp). Keeping scratch out of the system
 * temp dir avoids the whole publicly-writable-directory / symlink-race class of
 * problems. The scratch directory is removed on process exit.
 */
static inline void tf_tmpl(char *out, size_t n, const char *name)
{
    static int registered = 0;
    mkdir(TF_SCRATCH_DIR, 0700);
    if (!registered) {
        atexit(tf_scratch_cleanup);
        registered = 1;
    }
    snprintf(out, n, "%s/%s", TF_SCRATCH_DIR, name);
}

#define CHECK_EQ_INT(a, b)                                                                         \
    do {                                                                                           \
        long long _a = (long long)(a), _b = (long long)(b);                                        \
        if (_a != _b) {                                                                            \
            tf_current_failed++;                                                                   \
            printf("    expected %s == %s, got %lld vs %lld (%s:%d)\n", #a, #b, _a, _b, __FILE__,  \
                   __LINE__);                                                                      \
        }                                                                                          \
    } while (0)

#define CHECK_STR_EQ(a, b)                                                                         \
    do {                                                                                           \
        const char *_a = (a), *_b = (b);                                                           \
        if (strcmp(_a, _b) != 0) {                                                                 \
            tf_current_failed++;                                                                   \
            printf("    expected \"%s\" == \"%s\" (%s:%d)\n", _a, _b, __FILE__, __LINE__);         \
        }                                                                                          \
    } while (0)

#define TEST_SUITE_BEGIN(name) printf("== %s ==\n", name)

#define TEST_SUITE_END()                                                                           \
    do {                                                                                           \
        printf("\n%d test(s) run, %d failed\n", tf_tests_run, tf_tests_failed);                    \
        return tf_tests_failed ? 1 : 0;                                                            \
    } while (0)

#endif /* _TEST_FRAMEWORK_H_ */
