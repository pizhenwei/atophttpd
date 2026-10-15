#!/usr/bin/env bash
#
# Build atophttpd and its unit/integration tests with ThreadSanitizer, then run
# the whole suite. TSan detects data races between the acceptor thread, the
# worker-pool threads and the cache updater; any race aborts the run with a
# non-zero exit, so this doubles as a CI gate for the concurrency work (the
# worker pool, the cache reader/writer lock and shared request-path state).
#
# TSan is mutually exclusive with AddressSanitizer, hence a separate script.
#
# Usage: tests/tsan.sh
#
set -euo pipefail

ATOP_VERSION="${ATOP_VERSION:-v2.11.1}"

cd "$(dirname "$0")/.."
ROOT="$(pwd)"

CC="${CC:-gcc}"
SAN="-fsanitize=thread -fno-sanitize-recover=all -fno-omit-frame-pointer"
CFLAGS="-Iatop -I. -g -O1 -Wall -std=gnu11 $SAN -pthread"
LDLIBS="-lz -pthread"

MODULES="cache httpd json output rawlog connection socket tls http_args log"
OBJS="cache.o httpd.o json.o output.o rawlog.o version.o connection.o socket.o tls.o http_args.o log.o"

echo ">> cleaning previous artefacts"
make clean >/dev/null 2>&1 || true

echo ">> ensuring submodule + generated headers"
git submodule update --init --recursive >/dev/null 2>&1 || true
git -C atop fetch --tags --quiet >/dev/null 2>&1 || true
git -C atop checkout --quiet "$ATOP_VERSION" >/dev/null 2>&1 || true
make -C atop versdate.h >/dev/null 2>&1 || true

echo ">> building atop (used to generate a raw-log for the tests)"
make -C atop atop >/dev/null 2>&1 || echo "   (warning: could not build atop; sample tests will be skipped)"

RAWLOG_SAMPLE=""
if [[ -x atop/atop ]]; then
	SAMPLE_DIR="$(mktemp -d)"
	if atop/atop -w "$SAMPLE_DIR/atop_sample" 1 3 >/dev/null 2>&1; then
		RAWLOG_SAMPLE="$SAMPLE_DIR/atop_sample"
	fi
fi

echo ">> compiling thread-sanitized objects"
for m in $MODULES; do
	$CC -c $CFLAGS "$m.c" -o "$m.o"
done
$CC -c $CFLAGS atop/version.c -o version.o

echo ">> linking thread-sanitized atophttpd"
# shellcheck disable=SC2086
$CC $SAN -o atophttpd $OBJS $LDLIBS

echo ">> building thread-sanitized unit tests"
$CC $CFLAGS tests/unit/test_cache.c      cache.o                                    -o tests/unit/test_cache      $LDLIBS
$CC $CFLAGS tests/unit/test_output.c     output.o                                   -o tests/unit/test_output     $LDLIBS
$CC $CFLAGS tests/unit/test_http_args.c  http_args.o                                -o tests/unit/test_http_args  $LDLIBS
$CC $CFLAGS tests/unit/test_connection.c connection.o socket.o tls.o log.o          -o tests/unit/test_connection $LDLIBS
$CC $CFLAGS tests/unit/test_rawlog.c     rawlog.o cache.o json.o output.o log.o version.o -o tests/unit/test_rawlog $LDLIBS
$CC $CFLAGS tests/unit/test_json.c       json.o output.o version.o                  -o tests/unit/test_json      $LDLIBS

# A benign, by-design "race": httpd_stop is a volatile sig_atomic_t written by
# the SIGTERM handler and polled by the acceptor loop -- the standard async
# shutdown idiom, which TSan cannot recognise. Everything else must be clean.
# history_size=2 caps TSan's per-thread access history so the concurrency stress
# test stays within memory on modest machines/CI runners (it still keeps enough
# history to attribute a racing access to its stack).
export TSAN_OPTIONS="halt_on_error=1:history_size=2:suppressions=$ROOT/tests/tsan.supp"

echo ">> running thread-sanitized unit tests"
rc=0
for t in test_cache test_output test_http_args test_connection test_rawlog test_json; do
	echo "--- $t ---"
	RAWLOG_SAMPLE="$RAWLOG_SAMPLE" "./tests/unit/$t" || rc=1
done
[[ "$rc" -eq 0 ]] || { echo "UNIT TESTS FAILED UNDER TSAN"; exit 1; }

echo ">> running thread-sanitized integration tests"
ATOPHTTPD_BIN="$ROOT/atophttpd" ATOP_BIN="$ROOT/atop/atop" \
	python3 -m unittest -v tests.integration.test_integration

echo
echo "================= THREADSANITIZER CLEAN ================="
make clean >/dev/null 2>&1 || true
