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

int http_arg_long(const char *req, const char *needle, long *l)
{
	const char *s = strstr(req, needle);
	if (!s)
		return -1;

	s += strlen(needle);
	if (*s != '=')
		return -1;

	*l = atol(++s);

	return 0;
}

int http_arg_str(const char *req, const char *needle, char *p, int len)
{
	const char *s = strstr(req, needle);
	if (!s)
		return -1;

	s += strlen(needle);
	if (*s++ != '=')
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
