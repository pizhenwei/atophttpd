# Base build flags.
CFLAGS = -Iatop -g -O2 -Wall -Wcast-align -std=gnu11 -pthread
# Hardening flags, matching Debian's dpkg-buildflags defaults: stack canaries,
# fortified libc calls, a position-independent executable (full ASLR), and a
# read-only GOT resolved at startup (full RELRO).
HARDEN_CFLAGS = -D_FORTIFY_SOURCE=2 -fstack-protector-strong -fPIE \
               -Wformat -Werror=format-security
HARDEN_LDFLAGS = -pie -Wl,-z,relro -Wl,-z,now
CFLAGS += $(HARDEN_CFLAGS)
LDFLAGS += $(HARDEN_LDFLAGS)
LDLIBS = -lz
OBJS = cache.o httpd.o json.o output.o rawlog.o version.o connection.o socket.o tls.o http_args.o log.o
BIN = atophttpd
PREFIX := $(prefix)
CC=gcc
CLANGFMT = clang-format-18

# atop version atophttpd is compiled against. It MUST match the atop release
# installed on the target host, because atophttpd reads atop's version-specific
# binary raw-log format. The source targets the cgroup-v2 raw-log generation
# (roughly atop 2.9-2.11); v2.11.1 (Debian trixie) is the default and is
# CI-tested. Override with another tag of the same generation, e.g.:
#   make ATOP_VERSION=v2.10.0
ATOP_VERSION ?= v2.11.1

ifneq (,$(filter $(USE_TLS),yes YES y Y 1))
	CFLAGS += -DUSE_TLS
	LDLIBS += -lssl -lcrypto
endif

all: submodule bin
	$(CC) -o $(BIN) $(OBJS) $(CFLAGS) $(LDFLAGS) $(LDLIBS)

install: bin
	install -D -s $(BIN) $(PREFIX)/usr/bin/$(BIN)
	install -D -m 0644 atophttpd.service $(PREFIX)/usr/lib/systemd/system/atophttpd.service
	install -D -m 0644 packaging/debian/atophttpd.default $(PREFIX)/etc/default/atophttpd
	install -D -m 0644 man/atophttpd.1 $(PREFIX)/usr/share/man/man1/atophttpd.1

deb: bin
	@sh packaging/debian/makedeb.sh "$(CURDIR)"

bin: $(OBJS)
	$(CC) -o $(BIN) $(OBJS) $(CFLAGS) $(LDFLAGS) $(LDLIBS)

%.o: %.c
	$(CC) -c $(CFLAGS) $*.c -o $*.o

# struct rawrecord gained the cgroup record-length fields (ccomplen/icomplen)
# in atop 2.11; detect them in the pinned atop/rawlog.h so rawlog.c stays
# buildable on both sides of that boundary (v2.11+ has them, v2.9/v2.10 do
# not). This rule overrides the generic %.o pattern above.
rawlog.o: rawlog.c
	@if grep -q 'ccomplen' atop/rawlog.h; then \
		$(CC) -c $(CFLAGS) -DRECORD_HAS_CGROUPLEN rawlog.c -o rawlog.o; \
	else \
		$(CC) -c $(CFLAGS) rawlog.c -o rawlog.o; \
	fi

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

unit: version.o
	@make -C tests unit

integration: bin atop-bin
	@make -C tests integration

check: unit integration

# Full instrumented coverage run (unit + integration) with a threshold gate.
# Override the threshold with: make coverage COV=85
coverage:
	@bash tests/coverage.sh $(or $(COV),90)

# Build + run the whole suite under AddressSanitizer and UBSan.
sanitize:
	@bash tests/sanitize.sh

# Build + run the whole suite under ThreadSanitizer (concurrency race gate).
tsan:
	@bash tests/tsan.sh

# Build the libFuzzer harnesses (clang + ASan/UBSan) and run a short, bounded
# session against the untrusted-input parsers. Override the per-target budget
# with: make fuzz FUZZ_SECONDS=120 (or target a single one: make fuzz FUZZ=rawlog).
fuzz:
	@bash tests/fuzz.sh $(or $(FUZZ),all) $(or $(FUZZ_SECONDS),30)

.PHONY: all install deb bin submodule atop-bin unit integration check coverage sanitize tsan fuzz clean

clean:
	@rm -f $(BIN) *.o *.deb *.gcda *.gcno coverage.info
	@rm -rf coverage
	@make -C tests clean 2>/dev/null || true

# Format all project sources with clang-format; keep the atop submodule
# untouched so it stays pristine for upstream diffing.
format:
	@$(CLANGFMT) -i $(filter-out ./atop/%,$(shell find . -name '*.c' -o -name '*.h'))
