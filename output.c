/*
 * Copyright 2022 zhenwei pi
 *
 * Authors:
 *   zhenwei pi <pizhenwei@bytedance.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "output.h"

#define OUTBUF_DEF_SIZE (1024 * 1024)

static void output_buf(struct output *op, char *buf, int size)
{
	if (!op->ob.buf)
	{
		op->ob.size = OUTBUF_DEF_SIZE;
		op->ob.buf = calloc(1, op->ob.size);
	}

	/* no enought buf, grow it */
	if (op->ob.size - op->ob.offset < size)
	{
		op->ob.size *= 2;
		op->ob.buf = realloc(op->ob.buf, op->ob.size);
	}

	memcpy(op->ob.buf + op->ob.offset, buf, size);
	op->ob.offset += size;
}

void output_samp(struct output *op, char *buf, int size)
{
	switch (op->output_type)
	{
		case OUTPUT_STDOUT:
		printf("%s", buf);
		break;

		case OUTPUT_FD:
		write(op->fd, buf, size);
		break;

		case OUTPUT_BUF:
		output_buf(op, buf, size);
		break;

		default:
		fprintf(stderr, "Error, unknown output type\n");
		exit(EINVAL);
	}
}

void output_samp_done(struct output *op)
{
	if (op->done)
		op->done(op);

	if (op->output_type == OUTPUT_BUF)
	{
		memset(op->ob.buf, 0x00, op->ob.offset);
		op->ob.offset = 0;
	}
}
