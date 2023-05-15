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
#include "httpd.h"

#include <errno.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <unistd.h>

static connection_type CT_Socket;

static const char* conn_socket_get_type(connection* conn) {
	return CONN_TYPE_SOCKET;
}

static connection* conn_socket_create(int port, char *addr) {
	connection* conn = calloc(1, sizeof(connection));
	conn->type = &CT_Socket;
	conn->fd = -1;
	conn->port = port;
	conn->bindaddr = addr;

	return conn;
}

static int conn_socket_listen(connection* listener) {
	int listenfd = -1;

	if (listener->port == -1) {
		return -EINVAL;
	}

	if (strchr(listener->bindaddr, ':')) {
		listenfd = listen_to_port(listener->port, listener->bindaddr, AF_INET6);
	} else {
		listenfd = listen_to_port(listener->port, listener->bindaddr, AF_INET);
	}

	if (listenfd == -1) {
		return -EINVAL;
	}

	listener->fd = listenfd;
	// printf("socket listen on port %d, addr %s\n", listener->port, listener->bindaddr);
	return 0;
}

static int conn_socket_accept(connection* listener, connection* conn) {
	struct sockaddr_in cliaddr;
	socklen_t addrlen = sizeof(cliaddr);

	int clifd = accept(listener->fd, (struct sockaddr*)&cliaddr, &addrlen);
	if (clifd < 0)
		return -errno;

	conn->fd = clifd;
	return 0;
}

static void conn_socket_shutdown(connection* conn) {
	if (conn->fd == -1)
		return;

	shutdown(conn->fd, SHUT_RDWR);
}

static void conn_socket_close(connection* conn) {
	if (conn->fd == -1)
		return;

	close(conn->fd);
	conn->fd = -1;
}

static int conn_socket_write(struct connection* conn, const void* data, size_t data_len) {
	if (conn->fd == -1)
		return -EINVAL;

	return write(conn->fd, data, data_len);
}

static int conn_socket_writev(struct connection* conn, const struct iovec* iov, int iovcnt) {
	if (conn->fd == -1)
		return -EINVAL;

	return writev(conn->fd, iov, iovcnt);
}

static int conn_socket_read(struct connection* conn, void* buf, size_t buf_len) {
	if (conn->fd == -1)
		return -EINVAL;

	int ret = read(conn->fd, buf, buf_len);
	if (ret <= 0) {
		if (errno != EAGAIN) {
			return -errno;
		}
		return -EAGAIN;
	}

	return ret;
}

static connection_type CT_Socket = {
	.get_type = conn_socket_get_type,

	.init = NULL,
	.configure = NULL,
	.cleanup = NULL,

	.conn_create = conn_socket_create,

	.listen = conn_socket_listen,
	.accept = conn_socket_accept,
	.shutdown = conn_socket_shutdown,
	.close = conn_socket_close,

	.write = conn_socket_write,
	.writev = conn_socket_writev,
	.read = conn_socket_read};

int register_conntype_socket() {
	return conntype_register(&CT_Socket);
}
