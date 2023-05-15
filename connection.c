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

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "assert.h"
#include "connection.h"

static connection_type* conn_types[CONN_TYPE_MAX];

void conntype_initialize(void) {
	register_conntype_socket();
	register_conntype_tls();
}

int conntype_register(connection_type* ct) {
	const char* typename = ct->get_type(NULL);
	connection_type* tmpct;
	int type;

	/* find an empty slot to store the new connection type */
	for (type = 0; type < CONN_TYPE_MAX; type++) {
		tmpct = conn_types[type];
		if (!tmpct)
			break;

		/* ignore case, we really don't care "tls"/"TLS" */
		if (!strcasecmp(typename, tmpct->get_type(NULL))) {
			printf("Connection types %s already registered\n", typename);
			return -errno;
		}
	}

	printf("Connection type %s registered\n", typename);
	conn_types[type] = ct;

	if (ct->init) {
		ct->init();
	}

	return -errno;
}

connection_type* get_conntype_by_name(const char* typename) {
	connection_type* ct;

	for (int type = 0; type < CONN_TYPE_MAX; type++) {
		ct = conn_types[type];
		if (!ct)
			break;

		if (!strcasecmp(typename, ct->get_type(NULL)))
			return ct;
	}

	printf("Missing implement of connection type %s\n", typename);

	return NULL;
}

int get_conntype_index_by_name(const char* typename) {
	connection_type* ct;
	for (int type = 0; type < CONN_TYPE_MAX; type++) {
		ct = conn_types[type];
		if (!ct)
			break;

		if (!strcasecmp(typename, ct->get_type(NULL)))
			return type;
	}

	printf("Missing implement of connection type %s. Please re-compile atophttpd.\n", typename);

	return -1;
}

int listen_to_port(int port, char* bindaddr, int af) {
	struct addrinfo hints;
	struct addrinfo *res, *p;
	int ret, sockfd = -1;
	char _port[6];
	static const int reuse = 1;

	snprintf(_port, 6, "%d", port);
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = af;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;

	if (bindaddr && !strcmp("*", bindaddr))
		bindaddr = NULL;
	if (af == AF_INET6 && bindaddr && !strcmp("::*", bindaddr))
		bindaddr = NULL;

	ret = getaddrinfo(bindaddr, _port, &hints, &res);
	if (ret != 0) {
		perror("failed get addr info: ");
		goto error;
	}

	for (p = res; p != NULL; p = p->ai_next) {
		sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
		if (sockfd == -1) {
			perror("failed get socket fd: ");
			continue;
		}

		ret = setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
		if (ret < 0) {
			perror("socket set REUSEADDR failed: ");
			goto error;
		}
		ret = setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, (const char*)&reuse, sizeof(reuse));
		if (ret < 0) {
			perror("socket set REUSEPORT failed: ");
			goto error;
		}

		ret = bind(sockfd, p->ai_addr, p->ai_addrlen);
		if (ret == -1) {
			perror("socket bind failed: ");
			close(sockfd);
			continue;
		}

		ret = listen(sockfd, 128);
		if (ret == -1) {
			perror("socket listen failed: ");
			close(sockfd);
			continue;
		}

		goto end;
	}

error:
	if (sockfd != -1)
		close(sockfd);
	sockfd = -1;
end:
	freeaddrinfo(res);
	return sockfd;
}
