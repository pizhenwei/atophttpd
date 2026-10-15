# Test helper Makefile for atophttpd.
#
#   make -C tests unit          build & run the C unit tests (no coverage)
#   make -C tests integration   run the Python integration tests
#   make -C tests clean
#
# Coverage (build + unit + integration + lcov report + threshold gate) is
# driven by the top-level Makefile: `make coverage`.

ROOT    := ..
ABSROOT := $(abspath $(ROOT))
CC      ?= gcc
CFLAGS  := -I$(ROOT) -I$(ROOT)/atop -g -O0 -Wall -std=gnu11
LDLIBS  := -lz

UNIT_BINS := unit/test_cache unit/test_output unit/test_http_args \
             unit/test_connection unit/test_rawlog unit/test_json

.PHONY: unit integration clean

unit: $(UNIT_BINS)
	@rc=0; for t in $(UNIT_BINS); do echo "--- $$t ---"; ./$$t || rc=1; done; exit $$rc

unit/test_cache: unit/test_cache.c $(ROOT)/cache.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

unit/test_output: unit/test_output.c $(ROOT)/output.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

unit/test_http_args: unit/test_http_args.c $(ROOT)/http_args.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

unit/test_connection: unit/test_connection.c $(ROOT)/connection.c $(ROOT)/socket.c $(ROOT)/tls.c $(ROOT)/log.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

unit/test_rawlog: unit/test_rawlog.c $(ROOT)/rawlog.c $(ROOT)/cache.c $(ROOT)/json.c $(ROOT)/output.c $(ROOT)/log.c $(ROOT)/version.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

unit/test_json: unit/test_json.c $(ROOT)/json.c $(ROOT)/output.c $(ROOT)/version.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

integration:
	cd $(ABSROOT) && ATOPHTTPD_BIN=$(ABSROOT)/atophttpd ATOP_BIN=$(ABSROOT)/atop/atop \
		python3 -m unittest -v tests.integration.test_integration

clean:
	rm -f $(UNIT_BINS) unit/*.o *.gcda *.gcno unit/*.gcda unit/*.gcno
