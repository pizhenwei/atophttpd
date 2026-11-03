# Test helper Makefile for atophttpd.
#
#   make -C tests unit          build & run the C unit tests (no coverage)
#   make -C tests unit-tls      build & run the TLS backend test (needs OpenSSL)
#   make -C tests integration   run the Python integration tests
#   make -C tests clean
#
# Coverage (build + unit + integration + lcov report + threshold gate) is
# driven by the top-level Makefile: `make coverage`.

ROOT    := ..
ABSROOT := $(abspath $(ROOT))
CC      ?= gcc
CFLAGS  := -I$(ROOT) -I$(ROOT)/atop -g -O0 -Wall -std=gnu11 -pthread
LDLIBS  := -lz

UNIT_BINS := unit/test_cache unit/test_output unit/test_http_args \
             unit/test_connection unit/test_rawlog unit/test_json

.PHONY: unit unit-tls integration clean

unit: $(UNIT_BINS)
	@rc=0; for t in $(UNIT_BINS); do echo "--- $$t ---"; ./$$t || rc=1; done; exit $$rc

# TLS backend test, kept out of `unit` because it needs -DUSE_TLS and OpenSSL;
# the CI "build (USE_TLS=yes)" job runs it. It #includes tls.c to reach the
# static ssl_write_all and --wraps SSL_write/SSL_get_error.
unit-tls: unit/test_tls
	@rc=0; for t in $^; do echo "--- $$t ---"; ./$$t || rc=1; done; exit $$rc

unit/test_cache: unit/test_cache.c $(ROOT)/cache.c
	$(CC) $(CFLAGS) -DCACHE_FAULT_INJECTION \
		-Wl,--wrap=calloc,--wrap=realloc,--wrap=strdup \
		$^ -o $@ $(LDLIBS)

unit/test_output: unit/test_output.c $(ROOT)/output.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

unit/test_http_args: unit/test_http_args.c $(ROOT)/http_args.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

unit/test_connection: unit/test_connection.c $(ROOT)/connection.c $(ROOT)/socket.c $(ROOT)/tls.c $(ROOT)/log.c
	$(CC) $(CFLAGS) -Wl,--wrap=write,--wrap=writev $^ -o $@ $(LDLIBS)

unit/test_rawlog: unit/test_rawlog.c $(ROOT)/rawlog.c $(ROOT)/cache.c $(ROOT)/json.c $(ROOT)/output.c $(ROOT)/log.c $(ROOT)/version.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

unit/test_json: unit/test_json.c $(ROOT)/json.c $(ROOT)/output.c $(ROOT)/version.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

# test_tls.c #includes tls.c, so tls.c is a prerequisite (to trigger rebuilds)
# but is NOT passed to the compiler -- hence the explicit source list instead of
# $^, which would compile tls.c twice and clash.
unit/test_tls: unit/test_tls.c $(ROOT)/tls.c $(ROOT)/connection.c $(ROOT)/socket.c $(ROOT)/log.c
	$(CC) $(CFLAGS) -DUSE_TLS -Wl,--wrap=SSL_write,--wrap=SSL_get_error,--wrap=SSL_get_fd \
		unit/test_tls.c $(ROOT)/connection.c $(ROOT)/socket.c $(ROOT)/log.c \
		-o $@ $(LDLIBS) -lssl -lcrypto

integration:
	cd $(ABSROOT) && ATOPHTTPD_BIN=$(ABSROOT)/atophttpd ATOP_BIN=$(ABSROOT)/atop/atop \
		python3 -m unittest -v tests.integration.test_integration

clean:
	rm -f $(UNIT_BINS) unit/test_tls unit/*.o *.gcda *.gcno unit/*.gcda unit/*.gcno
