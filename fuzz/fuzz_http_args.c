/*
 * libFuzzer harness for the HTTP query-string parser (http_args.c).
 *
 * This is the first place untrusted bytes from a client are turned into C
 * strings, including the in-place "%2C" -> ',' rewrite in http_arg_str()
 * (a memmove loop over a caller-supplied buffer). The harness feeds the raw
 * input both directly as a query string and wrapped as a parameter value, and
 * decodes into buffers of several sizes to exercise the length-rejection and
 * decode paths.
 *
 * Build: see tests/fuzz.sh ("make fuzz").
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "http_args.h"

static void drive(const char *req)
{
	long l;
	http_arg_long(req, "timestamp", &l);
	http_arg_long(req, "x", &l);

	char out[64];
	http_arg_str(req, "labels", out, sizeof(out));
	http_arg_str(req, "x", out, sizeof(out));

	/* a deliberately tiny buffer stresses the length-rejection boundary */
	char tiny[4];
	http_arg_str(req, "x", tiny, sizeof(tiny));
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	/* 1, the raw input used verbatim as a query string */
	char *req = malloc(size + 1);
	if (!req)
		return 0;
	memcpy(req, data, size);
	req[size] = '\0';
	drive(req);

	/* 2, the input wrapped as the value of parameter "x", which guarantees
	 * the value-decoding path (memcpy + "%2C" rewrite) is reached */
	char *wrapped = malloc(size + 3);
	if (wrapped) {
		wrapped[0] = 'x';
		wrapped[1] = '=';
		memcpy(wrapped + 2, data, size);
		wrapped[size + 2] = '\0';
		drive(wrapped);
		free(wrapped);
	}

	free(req);
	return 0;
}
