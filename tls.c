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

#include <limits.h>
#include <netinet/in.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <stdlib.h>

#ifdef USE_TLS
typedef struct tls_connection {
	connection c;
	SSL* ssl;
} tls_connection;

SSL_CTX* tls_ctx = NULL;

static connection_type CT_TLS;

static const char* conn_tls_get_type(connection* conn) {
	return CONN_TYPE_TLS;
}

static void tls_init(void) {
	SSL_library_init();
	OpenSSL_add_all_algorithms();
	SSL_load_error_strings();
}

static int tls_configure(void* priv, int reconfigure) {
	atophttpd_tls_context_config* ctx_config = (atophttpd_tls_context_config*)priv;
	SSL_CTX* ctx = NULL;

	if (!ctx_config->cert_file) {
		perror("No tls-cert-file configured!");
		goto error;
	}

	if (!ctx_config->key_file) {
		perror("No tls-key-file configured!");
		goto error;
	}

	if (!ctx_config->ca_cert_file) {
		perror("No tls-ca-cert-file configured!");
		goto error;
	}

	ctx = SSL_CTX_new(TLS_method());
	if (!ctx) {
		perror("Create SSL context Error");
		goto error;
	}

	if (SSL_CTX_use_certificate_file(ctx, ctx_config->cert_file, SSL_FILETYPE_PEM) <= 0) {
		perror("ERROR loading server certificate");
		goto error;
	}

	if (SSL_CTX_use_PrivateKey_file(ctx, ctx_config->key_file, SSL_FILETYPE_PEM) <= 0) {
		perror("ERROR loading server private key");
		goto error;
	}

	if (!SSL_CTX_check_private_key(ctx)) {
		perror("Private key does not match certificate\n");
		goto error;
	}

	if (SSL_CTX_load_verify_locations(ctx, ctx_config->ca_cert_file, NULL) <= 0) {
		perror("ERROR loading ca certificate");
		goto error;
	}

	SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);

	SSL_CTX_free(tls_ctx);
	tls_ctx = ctx;
	return 0;

error:
	if (ctx)
		SSL_CTX_free(ctx);
	return -errno;
}

static void tls_cleanup(void) {
	if (tls_ctx) {
		SSL_CTX_free(tls_ctx);
		tls_ctx = NULL;
	}
}

static connection* conn_tls_create(int port, char *addr) {
	tls_connection* conn = calloc(1, sizeof(tls_connection));
	conn->c.type = &CT_TLS;
	conn->c.fd = -1;
	conn->c.port = port;
	conn->c.bindaddr = addr;

	return (connection*)conn;
}

static int conn_tls_listen(connection* listener) {
	// printf("tls listen on port %d, addr %s\n", listener->port, listener->bindaddr);
	return get_conntype_by_name(CONN_TYPE_SOCKET)->listen(listener);
}


static int conn_tls_accept(connection * listener, connection* conn) {
	int ret = get_conntype_by_name(CONN_TYPE_SOCKET)->accept(listener, conn);
	if (ret < 0) {
		perror("tls conn accept failed at tcp accept.");
		return -errno;
	}

	tls_connection* tls_conn = (tls_connection*)conn;
	SSL_CTX* ctx = tls_ctx;

	if (!ctx) {
		perror("tls conn create but ctx is null");
		return -EINVAL;
	}
	tls_conn->ssl = SSL_new(ctx);

	if (tls_conn->ssl == NULL) {
		perror("accept ssl is null!");
		return -EINVAL;
	}

	if (tls_conn->c.fd == -1) {
		perror("tls clifd is illegal!");
		return -EINVAL;
	}

	SSL_set_fd(tls_conn->ssl, tls_conn->c.fd);
	if (SSL_accept(tls_conn->ssl) <= 0) {
		printf("SSL_TLS handshake failed: %s\n", ERR_error_string(ERR_get_error(), NULL));
		return -errno;
	}

	return 0;
}

static void conn_shutdown_tls(connection* conn) {
	tls_connection* tls_conn = (tls_connection*)conn;

	if (tls_conn->ssl) {
		SSL_shutdown(tls_conn->ssl);
		SSL_free(tls_conn->ssl);

		tls_conn->ssl = NULL;
	}

	get_conntype_by_name(CONN_TYPE_SOCKET)->shutdown(conn);
	return;
}

static void conn_close_tls(connection* conn) {
	tls_connection* tls_conn = (tls_connection*)conn;

	if (tls_conn->ssl) {
		SSL_shutdown(tls_conn->ssl);
		SSL_free(tls_conn->ssl);

		tls_conn->ssl = NULL;
	}

	get_conntype_by_name(CONN_TYPE_SOCKET)->close(conn);
	return;
}

static int conn_write_tls(connection* conn, const void* data, size_t data_len) {
	tls_connection* tls_conn = (tls_connection*)conn;
	if (tls_conn->ssl == NULL) {
		return -EINVAL;
	}

	int ret = SSL_write(tls_conn->ssl, data, data_len);
	if (ret < 0) {
		perror("SSL write error");
		return -errno;
	}

	return 0;
}

static int conn_writev_tls(connection* conn, const struct iovec* iov, int iovcnt) {
	tls_connection* tls_conn = (tls_connection*)conn;

	if (tls_conn->ssl == NULL) {
		return -EINVAL;
	}

	size_t iov_bytes_len = 0;
	for (int i = 0; i < 3; i++) {
		iov_bytes_len += iov[i].iov_len;
	}

	char *ssl_buf;
	ssl_buf = malloc(iov_bytes_len);
	if (!ssl_buf) {
		perror("SSL have no mem");
		return -ENOMEM;
	}
	size_t offset = 0;
	for (int i = 0; i < iovcnt; i++) {
		memcpy(ssl_buf + offset, iov[i].iov_base, iov[i].iov_len);
		offset += iov[i].iov_len;
	}

	int ret = SSL_write(tls_conn->ssl, ssl_buf, iov_bytes_len);
	if (ret < 0) {
		perror("SSL writev error");
		return -errno;
	}

	return 0;
}

static int conn_read_tls(connection* conn, void* buf, size_t buf_len) {
	tls_connection* tls_conn = (tls_connection*)conn;

	if (tls_conn->ssl == NULL) {
		return -EINVAL;
	}

	int ret = SSL_read(tls_conn->ssl, buf, buf_len);
	if (ret <= 0) {
		if (errno != EAGAIN) {
			return -errno;
		}

		return -EAGAIN;
	}
	return ret;
}

static connection_type CT_TLS = {
	.get_type = conn_tls_get_type,

	.init = tls_init,
	.configure = tls_configure,
	.cleanup = tls_cleanup,

	.conn_create = conn_tls_create,

	.listen = conn_tls_listen,

	.accept = conn_tls_accept,
	.shutdown = conn_shutdown_tls,
	.close = conn_close_tls,

	.write = conn_write_tls,
	.writev = conn_writev_tls,
	.read = conn_read_tls};

int register_conntype_tls() {
	return conntype_register(&CT_TLS);
}

#else

int register_conntype_tls() {
	printf("ConnectionType %s not builtin\n", CONN_TYPE_TLS);
	return -EINVAL;
}

#endif
