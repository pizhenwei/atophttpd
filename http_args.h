/*
 * Copyright 2022 zhenwei pi
 *
 * Authors:
 *   zhenwei pi <pizhenwei@bytedance.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef _HTTP_ARGS_H_
#define _HTTP_ARGS_H_

/*
 * Parse a "needle=<long>" argument out of an HTTP query string.
 * Returns 0 and stores the value in *l on success, -1 otherwise.
 */
int http_arg_long(const char *req, const char *needle, long *l);

/*
 * Parse a "needle=<string>" argument out of an HTTP query string into the
 * caller supplied buffer p of size len (including the terminating NUL).
 * Any URL-encoded comma ("%2C") in the value is decoded to ','.
 * Returns 0 on success, -1 if the argument is missing or does not fit.
 */
int http_arg_str(const char *req, const char *needle, char *p, int len);

#endif /* _HTTP_ARGS_H_ */
