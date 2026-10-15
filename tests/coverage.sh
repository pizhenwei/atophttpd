#!/usr/bin/env bash
#
# Build atophttpd (and its unit tests) with gcov instrumentation, run the
# unit and integration suites, then produce an lcov coverage report and
# enforce a minimum line-coverage threshold.
#
# Usage: tests/coverage.sh [threshold]
#        threshold defaults to 90 (percent).
#
set -euo pipefail

THRESHOLD="${1:-90}"
ATOP_VERSION="${ATOP_VERSION:-v2.11.1}"

cd "$(dirname "$0")/.."
ROOT="$(pwd)"

CC="${CC:-gcc}"
CFLAGS="-Iatop -I. -g -O0 -Wall -std=gnu11 --coverage"
LDLIBS="-lz"

MODULES="cache httpd json output rawlog connection socket tls http_args log"
OBJS="cache.o httpd.o json.o output.o rawlog.o version.o connection.o socket.o tls.o http_args.o log.o"

echo ">> cleaning previous artefacts"
make clean >/dev/null 2>&1 || true
find . -path ./atop -prune -o \( -name '*.gcda' -o -name '*.gcno' \) -print -delete >/dev/null 2>&1 || true
rm -rf coverage coverage.info

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

echo ">> compiling instrumented objects"
for m in $MODULES; do
	$CC -c $CFLAGS "$m.c" -o "$m.o"
done
$CC -c $CFLAGS atop/version.c -o version.o

echo ">> linking instrumented atophttpd"
# shellcheck disable=SC2086
$CC --coverage -o atophttpd $OBJS $LDLIBS

echo ">> building unit tests"
$CC $CFLAGS tests/unit/test_cache.c      cache.o                          -o tests/unit/test_cache      $LDLIBS
$CC $CFLAGS tests/unit/test_output.c     output.o                         -o tests/unit/test_output     $LDLIBS
$CC $CFLAGS tests/unit/test_http_args.c  http_args.o                      -o tests/unit/test_http_args  $LDLIBS
$CC $CFLAGS tests/unit/test_connection.c connection.o socket.o tls.o log.o -o tests/unit/test_connection $LDLIBS
$CC $CFLAGS tests/unit/test_rawlog.c     rawlog.o cache.o json.o output.o log.o version.o -o tests/unit/test_rawlog $LDLIBS
$CC $CFLAGS tests/unit/test_json.c       json.o output.o version.o         -o tests/unit/test_json      $LDLIBS

echo ">> running unit tests"
rc=0
for t in test_cache test_output test_http_args test_connection test_rawlog test_json; do
	echo "--- $t ---"
	RAWLOG_SAMPLE="$RAWLOG_SAMPLE" "./tests/unit/$t" || rc=1
done
[[ "$rc" -eq 0 ]] || { echo "UNIT TESTS FAILED"; exit 1; }

echo ">> running integration tests"
ATOPHTTPD_BIN="$ROOT/atophttpd" ATOP_BIN="$ROOT/atop/atop" \
	python3 -m unittest -v tests.integration.test_integration

echo ">> capturing coverage"
LCOV_OPTS="--rc branch_coverage=0 --ignore-errors inconsistent,unused,empty,mismatch"
# shellcheck disable=SC2086
lcov --capture --directory . --output-file coverage.info \
	$LCOV_OPTS >/dev/null 2>&1
# shellcheck disable=SC2086
lcov --remove coverage.info '*/atop/*' '*/tests/*' '/usr/*' \
	--output-file coverage.info $LCOV_OPTS >/dev/null 2>&1

echo
echo "================= COVERAGE ================"
# shellcheck disable=SC2086
lcov --list coverage.info $LCOV_OPTS

if command -v genhtml >/dev/null 2>&1; then
	genhtml coverage.info --output-directory coverage/html >/dev/null 2>&1 || true
	echo ">> HTML report: coverage/html/index.html"
fi

echo
python3 tests/check_coverage.py coverage.info "$THRESHOLD"
