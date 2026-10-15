#!/usr/bin/env bash
#
# Build atophttpd and its unit/integration tests with AddressSanitizer and
# UndefinedBehaviorSanitizer, then run the whole suite. Any memory error
# (overflow, use-after-free, leak) or undefined behaviour aborts the run with
# a non-zero exit, so this doubles as a CI gate.
#
# Usage: tests/sanitize.sh
#
set -euo pipefail

ATOP_VERSION="${ATOP_VERSION:-v2.11.1}"

cd "$(dirname "$0")/.."
ROOT="$(pwd)"

CC="${CC:-gcc}"
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer"
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

echo ">> building atop (used to generate a raw-log for integration tests)"
make -C atop atop >/dev/null 2>&1 || echo "   (warning: could not build atop; sample tests will be skipped)"

RAWLOG_SAMPLE=""
if [[ -x atop/atop ]]; then
	SAMPLE_DIR="$(mktemp -d)"
	if atop/atop -w "$SAMPLE_DIR/atop_sample" 1 3 >/dev/null 2>&1; then
		RAWLOG_SAMPLE="$SAMPLE_DIR/atop_sample"
	fi
fi

echo ">> compiling sanitized objects"
for m in $MODULES; do
	$CC -c $CFLAGS "$m.c" -o "$m.o"
done
$CC -c $CFLAGS atop/version.c -o version.o

echo ">> linking sanitized atophttpd"
# shellcheck disable=SC2086
$CC $SAN -o atophttpd $OBJS $LDLIBS

echo ">> building sanitized unit tests"
$CC $CFLAGS tests/unit/test_cache.c      cache.o                                    -o tests/unit/test_cache      $LDLIBS
$CC $CFLAGS tests/unit/test_output.c     output.o                                   -o tests/unit/test_output     $LDLIBS
$CC $CFLAGS tests/unit/test_http_args.c  http_args.o                                -o tests/unit/test_http_args  $LDLIBS
$CC $CFLAGS tests/unit/test_connection.c connection.o socket.o tls.o log.o          -o tests/unit/test_connection $LDLIBS
$CC $CFLAGS tests/unit/test_rawlog.c     rawlog.o cache.o json.o output.o log.o version.o -o tests/unit/test_rawlog $LDLIBS
$CC $CFLAGS tests/unit/test_json.c       json.o output.o version.o                  -o tests/unit/test_json      $LDLIBS

export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1"

echo ">> running sanitized unit tests (leak detection on)"
export ASAN_OPTIONS="abort_on_error=1:detect_leaks=1"
rc=0
for t in test_cache test_output test_http_args test_connection test_rawlog test_json; do
	echo "--- $t ---"
	RAWLOG_SAMPLE="$RAWLOG_SAMPLE" "./tests/unit/$t" || rc=1
done
[[ "$rc" -eq 0 ]] || { echo "UNIT TESTS FAILED UNDER SANITIZERS"; exit 1; }

echo ">> running sanitized integration tests"
# The long-running server keeps a few process-lifetime allocations (output
# buffer, caches) that are intentionally never freed; disable leak detection
# for it so only real errors (overflows, UAF, UB) fail the run.
ASAN_OPTIONS="abort_on_error=1:detect_leaks=0" \
ATOPHTTPD_BIN="$ROOT/atophttpd" ATOP_BIN="$ROOT/atop/atop" \
	python3 -m unittest -v tests.integration.test_integration

echo
echo "================= SANITIZERS CLEAN ================="
make clean >/dev/null 2>&1 || true
