/*
 * Copyright 2022 zhenwei pi
 *
 * Authors:
 *   zhenwei pi <pizhenwei@bytedance.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef _HTTPD_H_
#define _HTTPD_H_

extern int __debug;

#include "connection.h"

#define log_debug(fmt, args...)					\
	if (__debug) {						\
		fprintf(stdout, "%s[%d] " fmt,			\
				__func__, __LINE__, ##args);	\
	}

int rawlog_parse_all(const char *path);
int rawlog_get_record(time_t ts, char *lables, connection *conn);

typedef struct atophttpd_tls_context_config {
	int tls_port;
	char *tls_addr;
	char *ca_cert_file;
	char *cert_file;
	char *key_file;
} atophttpd_tls_context_config;

struct atophttd_context {
	int port;
        int daemonmode;
	char *addr;
	char *log_path;

	atophttpd_tls_context_config tls_ctx_config;

	connection* listeners[CONN_TYPE_MAX];
};

#endif
