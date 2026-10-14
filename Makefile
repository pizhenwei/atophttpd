CFLAGS = -Iatop -g -O2 -lz -Wall -Wcast-align -std=gnu11
OBJS = cache.o httpd.o json.o output.o rawlog.o version.o connection.o socket.o tls.o http_args.o
BIN = atophttpd
PREFIX := $(prefix)
CC=gcc

# atop version atophttpd is compiled against. It MUST match the atop release
# installed on the target host, because atophttpd reads atop's version-specific
# binary raw-log format. The source targets the cgroup-v2 raw-log generation
# (roughly atop 2.9-2.11); v2.11.1 (Debian trixie) is the default and is
# CI-tested. Override with another tag of the same generation, e.g.:
#   make ATOP_VERSION=v2.10.0
ATOP_VERSION ?= v2.11.1

ifneq (,$(filter $(USE_TLS),yes YES y Y 1))
	CFLAGS += -lssl -lcrypto -DUSE_TLS
endif

all: submodule bin
	$(CC) -o $(BIN) $(OBJS) $(CFLAGS)

install: bin
	install -D -s $(BIN) $(PREFIX)/usr/bin/$(BIN)
	install -D atophttpd.service $(PREFIX)/lib/systemd/system/atophttpd.service
	install -D man/atophttpd.1 $(PREFIX)/usr/share/man/man1/atophttpd.1

deb: bin
	@sh packaging/debian/makedeb.sh `pwd`

bin: $(OBJS)
	$(CC) -o $(BIN) $(OBJS) $(CFLAGS)

%.o: %.c
	$(CC) -c $(CFLAGS) $*.c -o $*.o

version.o:
	@make -C atop versdate.h
	$(CC) -c $(CFLAGS) atop/version.c -o version.o

submodule:
	git submodule update --init --recursive
	git -C atop fetch --tags --quiet || true
	git -C atop checkout --quiet $(ATOP_VERSION)

# --- testing -------------------------------------------------------------
# Build the atop submodule binary (used to generate raw-logs for the tests).
atop-bin: submodule
	@make -C atop versdate.h
	@make -C atop atop

unit:
	@make -C tests unit

integration: bin atop-bin
	@make -C tests integration

check: unit integration

# Full instrumented coverage run (unit + integration) with a threshold gate.
# Override the threshold with: make coverage COV=85
coverage:
	@bash tests/coverage.sh $(or $(COV),90)

.PHONY: all install deb bin submodule atop-bin unit integration check coverage clean

clean:
	@rm -f $(BIN) *.o *.deb *.gcda *.gcno coverage.info
	@rm -rf coverage
	@make -C tests clean 2>/dev/null || true
