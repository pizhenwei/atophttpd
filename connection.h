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

/* Single-shot non-blocking I/O contract:
 *
 *   read()  returns 0 on EOF, >0 on data, -EAGAIN when nothing is ready yet,
 *           or a negative errno on a hard error.
 *   write()  returns >0 on bytes accepted by the kernel (or one TLS record),
 *           -EAGAIN when the fd cannot take more data right now, or a negative
 *           errno on a hard error.
 *
 * After -EAGAIN the caller re-arms the fd with conn->wait_events, which the
 * backend fills in before returning: a TLS backend may need to read in order
 * to write and vice versa (renegotiation), so it cannot be derived from the
 * call direction alone. Plain sockets use the obvious mapping
 * (read -> EPOLLIN, write -> EPOLLOUT).
 */
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/uio.h>
#include <time.h>

typedef struct connection connection;

typedef struct connection_type {
    const char *(*get_type)(struct connection *conn);

    void (*init)(void);
    void (*cleanup)(void);
    int (*configure)(void *priv, int reconfiguration);

    connection *(*conn_create)(int port, char *addr);
    int (*listen)(connection *listener);

    /* Drive the backend handshake (TLS) in a single non-blocking step; NULL
     * when the type needs no handshake (plain TCP). Returns 0 once the
     * handshake is complete, -EAGAIN to continue later (see wait_events),
     * or a negative errno on a fatal error. */
    int (*handshake)(struct connection *conn);

    int (*accept)(struct connection *listener, struct connection *conn);
    void (*close)(struct connection *conn);
    void (*shutdown)(struct connection *conn);

    int (*write)(struct connection *conn, const void *data, size_t data_len);
    int (*writev)(struct connection *conn, const struct iovec *iov, int iovcnt);
    int (*read)(struct connection *conn, void *buf, size_t buf_len);
} connection_type;

struct connection {
    connection_type *type;
    int fd;
    int port;
    int is_local;
    char *bindaddr;
    void *priv;      /* opaque per-owner data (the event loop's client) */
    int wait_events; /* epoll interest to re-arm after -EAGAIN; see above */
};

static inline int conn_configure(connection_type *ct, void *priv, int reconfigure)
{
    return ct->configure(priv, reconfigure);
}

static inline connection *conn_create(connection_type *ct, int port, char *addr)
{
    return ct->conn_create(port, addr);
}

static inline int conn_listen(connection *listener)
{
    return listener->type->listen(listener);
}

static inline int conn_accept(connection *listener, connection *conn)
{
    return conn->type->accept(listener, conn);
}

static inline int conn_handshake(connection *conn)
{
    return conn->type->handshake ? conn->type->handshake(conn) : 0;
}

static inline void conn_close(connection *conn)
{
    return conn->type->close(conn);
}

static inline void conn_shutdown(connection *conn)
{
    return conn->type->shutdown(conn);
}

static inline int conn_write(connection *conn, const void *data, size_t data_len)
{
    return conn->type->write(conn, data, data_len);
}

static inline int conn_writev(connection *conn, const struct iovec *iovs, int iovcnt)
{
    return conn->type->writev(conn, iovs, iovcnt);
}

static inline int conn_read(connection *conn, void *buf, size_t buf_len)
{
    return conn->type->read(conn, buf, buf_len);
}

void conntype_initialize(void);
int conntype_register(connection_type *ct);

int register_conntype_socket();
int register_conntype_tls();

int listen_to_port(int port, char *bindaddr, int af);

/* Monotonic clock in milliseconds, used to bound the per-state deadlines. */
static inline long long conn_now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
connection_type *get_conntype_by_name(const char *typename);
int get_conntype_index_by_name(const char *typename);

#endif /* _CONNECTION_H_ */
