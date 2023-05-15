/*
 * Copyright 2023 enhua zhou
 *
 * Authors:
 *   enhua zhou <zhouenhua@bytedance.com>
 *
 * Design and some codes are taken from redis.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef _CONNECTION_H_
#define _CONNECTION_H_

#define CONN_TYPE_SOCKET "tcp"
#define CONN_TYPE_TLS "tls"
#define CONN_TYPE_MAX 8

#include <stdio.h>
#include <string.h>
#include <sys/uio.h>
#include <poll.h>

typedef struct connection connection;

typedef struct connection_type {
	const char* (*get_type)(struct connection* conn);

	void (*init)(void);
	void (*cleanup)(void);
	int (*configure)(void* priv, int reconfiguration);

	connection* (*conn_create)(int port, char *addr);
	int (*listen)(connection* listener);

	int (*accept)(struct connection* listener, struct connection* conn);
	void (*close)(struct connection* conn);
	void (*shutdown)(struct connection* conn);

	int (*write)(struct connection* conn, const void* data, size_t data_len);
	int (*writev)(struct connection* conn, const struct iovec* iov, int iovcnt);
	int (*read)(struct connection* conn, void* buf, size_t buf_len);
} connection_type;

struct connection {
	connection_type* type;
	int fd;
	int port;
	int is_local;
	char *bindaddr;
};

static inline int conn_configure(connection_type* ct, void* priv, int reconfigure) {
	return ct->configure(priv, reconfigure);
}

static inline connection* conn_create(connection_type* ct, int port, char *addr) {
	return ct->conn_create(port, addr);
}

static inline int conn_listen(connection* listener) {
	return listener->type->listen(listener);
}

static inline int conn_accept(connection* listener, connection* conn) {
	return conn->type->accept(listener, conn);
}

static inline void conn_close(connection* conn) {
	return conn->type->close(conn);
}

static inline void conn_shutdown(connection* conn) {
	return conn->type->shutdown(conn);
}

static inline int conn_write(connection* conn, const void* data, size_t data_len) {
	return conn->type->write(conn, data, data_len);
}

static inline int conn_writev(connection* conn, const struct iovec* iovs, int iovcnt) {
	return conn->type->writev(conn, iovs, iovcnt);
}

static inline int conn_read(connection* conn, void* buf, size_t buf_len) {
	return conn->type->read(conn, buf, buf_len);
}

void conntype_initialize(void);
int conntype_register(connection_type* ct);

int register_conntype_socket();
int register_conntype_tls();

int listen_to_port(int port, char* bindaddr, int af);
connection_type* get_conntype_by_name(const char* typename);
int get_conntype_index_by_name(const char* typename);

#endif /* _CONNECTION_H_ */
