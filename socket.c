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

static const char *conn_socket_get_type(connection *conn)
{
    return CONN_TYPE_SOCKET;
}

static connection *conn_socket_create(int port, char *addr)
{
    connection *conn = calloc(1, sizeof(connection));
    conn->type = &CT_Socket;
    conn->fd = -1;
    conn->port = port;
    conn->bindaddr = addr;

    return conn;
}

static int conn_socket_listen(connection *listener)
{
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

static int conn_socket_accept(connection *listener, connection *conn)
{
    struct sockaddr_in cliaddr;
    socklen_t addrlen = sizeof(cliaddr);

    int clifd = accept(listener->fd, (struct sockaddr *)&cliaddr, &addrlen);
    if (clifd < 0)
        return -errno;

    conn->fd = clifd;
    return 0;
}

static void conn_socket_shutdown(connection *conn)
{
    if (conn->fd == -1)
        return;

    shutdown(conn->fd, SHUT_RDWR);
}

static void conn_socket_close(connection *conn)
{
    if (conn->fd == -1)
        return;

    close(conn->fd);
    conn->fd = -1;
}

static int conn_socket_write(struct connection *conn, const void *data, size_t data_len)
{
    if (conn->fd == -1)
        return -EINVAL;

    const char *p = data;
    size_t remaining = data_len;
    long long deadline = conn_now_ms() + CONN_SEND_TIMEOUT_MS;

    /* Loop so a short write (the blocking socket's SO_SNDTIMEO firing mid-send)
     * resumes instead of silently truncating the response. Stop once the overall
     * deadline passes to bound a slow client's hold on this worker. */
    while (remaining > 0 && conn_now_ms() < deadline) {
        ssize_t n = write(conn->fd, p, remaining);
        if (n > 0) {
            p += (size_t)n;
            remaining -= (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        /* nothing sent at all: surface the error; otherwise stop on partial */
        if (remaining == data_len && n < 0)
            return -errno;
        break;
    }

    return (int)(data_len - remaining);
}

static int conn_socket_writev(struct connection *conn, const struct iovec *iov, int iovcnt)
{
    if (conn->fd == -1)
        return -EINVAL;
    if (iovcnt <= 0)
        return 0;

    /* Writable copy so partial writes can advance past consumed segments: the
     * caller's iov is const. The per-call malloc is intentional and left simple
     * -- iovcnt is tiny (the response path always passes 3) and this allocation
     * is dwarfed by the kB-sized buffers the same path already allocates (the
     * JSON/deflate output buffers). A stack array would avoid it but adds a
     * branch for no measurable gain. */
    struct iovec *vecs = malloc((size_t)iovcnt * sizeof(*vecs));
    if (!vecs)
        return -ENOMEM;
    memcpy(vecs, iov, (size_t)iovcnt * sizeof(*vecs));

    size_t sent = 0;
    int idx = 0;
    long long deadline = conn_now_ms() + CONN_SEND_TIMEOUT_MS;
    int rc;

    while (idx < iovcnt && conn_now_ms() < deadline) {
        ssize_t n = writev(conn->fd, &vecs[idx], iovcnt - idx);
        if (n <= 0) {
            if (n < 0 && errno == EINTR)
                continue;
            if (sent == 0 && n < 0) {
                rc = -errno;
                goto out;
            }
            break;
        }

        sent += (size_t)n;
        size_t adv = (size_t)n;
        while (idx < iovcnt && adv >= vecs[idx].iov_len) {
            adv -= vecs[idx].iov_len;
            idx++;
        }
        if (idx < iovcnt && adv > 0) {
            vecs[idx].iov_base = (char *)vecs[idx].iov_base + adv;
            vecs[idx].iov_len -= adv;
        }
    }

    rc = (int)sent;
out:
    free(vecs);
    return rc;
}

static int conn_socket_read(struct connection *conn, void *buf, size_t buf_len)
{
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

static connection_type CT_Socket = {.get_type = conn_socket_get_type,

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

int register_conntype_socket()
{
    return conntype_register(&CT_Socket);
}
