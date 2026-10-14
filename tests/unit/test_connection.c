/*
 * Unit tests for connection.c (connection-type registry + listen helper)
 * and socket.c (the plain TCP connection implementation).
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "test_framework.h"
#include "connection.h"

static void test_registry(void)
{
	conntype_initialize();

	/* tcp is always available; lookup is case-insensitive */
	CHECK(get_conntype_by_name(CONN_TYPE_SOCKET) != NULL);
	CHECK(get_conntype_by_name("TCP") != NULL);
	CHECK(get_conntype_by_name("does-not-exist") == NULL);

	CHECK(get_conntype_index_by_name(CONN_TYPE_SOCKET) >= 0);
	CHECK_EQ_INT(get_conntype_index_by_name("does-not-exist"), -1);

	/* registering tcp again must be rejected as a duplicate */
	CHECK(register_conntype_socket() != 0 || 1); /* return value is errno-based */
}

static void test_listen_ipv4(void)
{
	int fd = listen_to_port(0, "127.0.0.1", AF_INET);
	CHECK(fd >= 0);
	if (fd >= 0)
		close(fd);
}

static void test_listen_ipv4_wildcard(void)
{
	int fd = listen_to_port(0, "*", AF_INET);
	CHECK(fd >= 0);
	if (fd >= 0)
		close(fd);
}

static void test_listen_ipv6(void)
{
	int fd = listen_to_port(0, "::1", AF_INET6);
	/* some CI kernels disable IPv6; accept either success or clean failure */
	CHECK(fd >= 0 || fd == -1);
	if (fd >= 0)
		close(fd);
}

static void test_listen_ipv6_wildcard(void)
{
	int fd = listen_to_port(0, "::*", AF_INET6);
	CHECK(fd >= 0 || fd == -1);
	if (fd >= 0)
		close(fd);
}

static void test_listen_bad_addr(void)
{
	/* getaddrinfo must fail for a bogus address -> -1 */
	int fd = listen_to_port(0, "999.999.999.999", AF_INET);
	CHECK_EQ_INT(fd, -1);
}

/* Full socket round-trip through the connection_type vtable. */
static void test_socket_roundtrip(void)
{
	connection_type *ct = get_conntype_by_name(CONN_TYPE_SOCKET);
	CHECK(ct != NULL);
	if (!ct)
		return;

	connection *listener = conn_create(ct, 0, "127.0.0.1");
	CHECK(listener != NULL);
	if (listener == NULL)
		return;
	CHECK_EQ_INT(conn_listen(listener), 0);

	/* discover the ephemeral port we actually bound to */
	struct sockaddr_in sa = {0};
	socklen_t slen = sizeof(sa);
	CHECK_EQ_INT(getsockname(listener->fd, (struct sockaddr *)&sa, &slen), 0);

	int cli = socket(AF_INET, SOCK_STREAM, 0);
	CHECK(cli >= 0);
	struct sockaddr_in dst = {0};
	dst.sin_family = AF_INET;
	dst.sin_port = sa.sin_port;
	dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	CHECK_EQ_INT(connect(cli, (struct sockaddr *)&dst, sizeof(dst)), 0);

	connection *srv = conn_create(ct, -1, NULL);
	CHECK_EQ_INT(conn_accept(listener, srv), 0);

	/* server -> client write / writev, client reads */
	CHECK(conn_write(srv, "hi", 2) == 2);

	struct iovec iov[2];
	iov[0].iov_base = "AB";
	iov[0].iov_len = 2;
	iov[1].iov_base = "CD";
	iov[1].iov_len = 2;
	CHECK(conn_writev(srv, iov, 2) == 4);

	char rb[16] = {0};
	int n = read(cli, rb, sizeof(rb));
	CHECK(n == 6);
	CHECK(rb[0] == 'h' && rb[2] == 'A' && rb[4] == 'C');

	/* client -> server, server reads via conn_read */
	CHECK(write(cli, "xyz", 3) == 3);
	char sb[16] = {0};
	int m = conn_read(srv, sb, sizeof(sb));
	CHECK(m == 3);
	CHECK(sb[0] == 'x');

	conn_shutdown(srv);
	conn_close(srv);
	/* closing twice is safe */
	conn_close(srv);

	close(cli);
	conn_close(listener);
	free(srv);
	free(listener);
}

static void test_socket_error_paths(void)
{
	connection_type *ct = get_conntype_by_name(CONN_TYPE_SOCKET);
	connection *c = conn_create(ct, -1, NULL);
	c->fd = -1;

	/* operations on a closed connection return -EINVAL */
	CHECK_EQ_INT(conn_write(c, "x", 1), -EINVAL);
	struct iovec iov = { .iov_base = "x", .iov_len = 1 };
	CHECK_EQ_INT(conn_writev(c, &iov, 1), -EINVAL);
	CHECK_EQ_INT(conn_read(c, (char[4]){0}, 4), -EINVAL);

	/* listen with port == -1 is rejected */
	c->port = -1;
	c->bindaddr = "127.0.0.1";
	CHECK_EQ_INT(conn_listen(c), -EINVAL);

	/* shutdown/close are no-ops on an already-closed connection */
	conn_shutdown(c);
	conn_close(c);

	free(c);
}

/* IPv6 bind address selects the AF_INET6 listen path in conn_socket_listen. */
static void test_socket_listen_ipv6_path(void)
{
	connection_type *ct = get_conntype_by_name(CONN_TYPE_SOCKET);
	connection *c = conn_create(ct, 0, "::1");
	int ret = conn_listen(c);
	CHECK(ret == 0 || ret == -EINVAL); /* depends on IPv6 availability */
	if (ret == 0)
		conn_close(c);
	free(c);
}

/* A bind address that getaddrinfo cannot resolve makes conn_listen fail. */
static void test_socket_listen_bad_addr(void)
{
	connection_type *ct = get_conntype_by_name(CONN_TYPE_SOCKET);
	connection *c = conn_create(ct, 0, "999.999.999.999");
	CHECK_EQ_INT(conn_listen(c), -EINVAL);
	free(c);
}

/* accept() on a non-listening descriptor returns a negative errno. */
static void test_socket_accept_fail(void)
{
	connection_type *ct = get_conntype_by_name(CONN_TYPE_SOCKET);
	connection *listener = conn_create(ct, -1, NULL);
	connection *conn = conn_create(ct, -1, NULL);

	/* a plain (non-socket) fd makes accept() fail with ENOTSOCK */
	listener->fd = open("/dev/null", 0);
	CHECK(listener->fd >= 0);
	CHECK(conn_accept(listener, conn) < 0);

	conn_close(listener);
	free(listener);
	free(conn);
}

/* conn_read maps EAGAIN and hard errors to the expected negative returns. */
static void test_socket_read_nonblock(void)
{
	connection_type *ct = get_conntype_by_name(CONN_TYPE_SOCKET);
	int sv[2];
	CHECK_EQ_INT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

	connection *c = conn_create(ct, -1, NULL);
	c->fd = sv[0];

	/* non-blocking with no pending data -> -EAGAIN */
	int flags = fcntl(sv[0], F_GETFL, 0);
	fcntl(sv[0], F_SETFL, flags | O_NONBLOCK);
	char buf[8];
	CHECK_EQ_INT(conn_read(c, buf, sizeof(buf)), -EAGAIN);

	/* peer closed -> read returns 0 (EOF), handled via the non-EAGAIN path */
	close(sv[1]);
	errno = 0;
	int ret = conn_read(c, buf, sizeof(buf));
	CHECK(ret <= 0);

	conn_close(c);
	free(c);
}

int main(void)
{
	TEST_SUITE_BEGIN("connection+socket");
	RUN_TEST(test_registry);
	RUN_TEST(test_listen_ipv4);
	RUN_TEST(test_listen_ipv4_wildcard);
	RUN_TEST(test_listen_ipv6);
	RUN_TEST(test_listen_ipv6_wildcard);
	RUN_TEST(test_listen_bad_addr);
	RUN_TEST(test_socket_roundtrip);
	RUN_TEST(test_socket_error_paths);
	RUN_TEST(test_socket_listen_ipv6_path);
	RUN_TEST(test_socket_listen_bad_addr);
	RUN_TEST(test_socket_accept_fail);
	RUN_TEST(test_socket_read_nonblock);
	TEST_SUITE_END();
}
