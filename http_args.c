/*
 * Copyright 2022 zhenwei pi
 *
 * Authors:
 *   zhenwei pi <pizhenwei@bytedance.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include <stdlib.h>
#include <string.h>

#include "http_args.h"

/*
 * Locate query-string parameter `needle` in `req` and return a pointer to the
 * first character of its value (just after '='), or NULL if absent.
 *
 * The match must start at a parameter boundary -- the beginning of the string,
 * or right after '?' or '&' -- and be immediately followed by '='. This avoids
 * a parameter name matching inside a longer token (e.g. "lables" inside
 * "notlables"), which a plain strstr() would wrongly accept.
 */
static const char *http_arg_value(const char *req, const char *needle)
{
	size_t nlen = strlen(needle);
	const char *s = req;

	while ((s = strstr(s, needle)) != NULL) {
		int at_boundary = (s == req) || (s[-1] == '?') || (s[-1] == '&');
		if (at_boundary && s[nlen] == '=')
			return s + nlen + 1;
		s++;
	}

	return NULL;
}

int http_arg_long(const char *req, const char *needle, long *l)
{
	const char *s = http_arg_value(req, needle);
	if (!s)
		return -1;

	*l = atol(s);

	return 0;
}

int http_arg_str(const char *req, const char *needle, char *p, int len)
{
	const char *s = http_arg_value(req, needle);
	if (!s)
		return -1;

	const char *e = strchr(s, '&');
	if (!e)
		e = s + strlen(s);

	if (e - s >= len)
		return -1;

	memcpy(p, s, e - s);
	p[e - s] = '\0';
	while ((p = strstr(p, "%2C")) != NULL)
	{
		*(p++) = ',';
		memmove(p, p + 2, strlen(p + 2) + 1);
	}

	return 0;
}
