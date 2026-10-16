#!/usr/bin/env bash
#
# Build the libFuzzer harnesses in fuzz/ with clang (ASan + UBSan) and run a
# short, bounded fuzzing session against each one. Any crash, memory error or
# undefined behaviour makes the run fail, and the reproducer is left in
# fuzz/crashes/ for triage. This doubles as a (fast) CI gate.
#
# Usage:
#   tests/fuzz.sh [target] [seconds]
#     target   one of: http_args | rawlog | all   (default: all)
#     seconds  wall-clock budget per target        (default: 30, CI uses less)
#
set -euo pipefail

TARGET="${1:-all}"
SECONDS_PER_TARGET="${2:-${FUZZ_SECONDS:-30}}"
ATOP_VERSION="${ATOP_VERSION:-v2.11.1}"

cd "$(dirname "$0")/.."
ROOT="$(pwd)"

CC="${CC:-clang}"
if ! command -v "$CC" >/dev/null 2>&1; then
	echo "error: clang is required for libFuzzer (set CC= or install clang)" >&2
	exit 1
fi

SAN="-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer"
CFLAGS="-Iatop -I. -g -O1 -Wall -std=gnu11 $SAN -fsanitize=fuzzer-no-link -pthread"
LDLIBS="-lz -pthread"

CORPUS="$ROOT/fuzz/corpus"
CRASHES="$ROOT/fuzz/crashes"
mkdir -p "$CRASHES"

# libFuzzer run budget. Keep the per-allocation / RSS limits generous enough to
# avoid flagging the parser's (bounded, NULL-checked) size-driven allocations as
# false-positive OOMs, while still catching runaway growth.
RUN_OPTS=(-max_total_time="$SECONDS_PER_TARGET" -max_len=65536 \
	-rss_limit_mb=4096 -malloc_limit_mb=3072 -timeout=25 \
	-print_final_stats=1 -artifact_prefix="$CRASHES/")

export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1"
export ASAN_OPTIONS="abort_on_error=1:detect_leaks=0"

echo ">> cleaning previous artefacts"
make clean >/dev/null 2>&1 || true

echo ">> ensuring submodule + generated headers"
git submodule update --init --recursive >/dev/null 2>&1 || true
git -C atop fetch --tags --quiet >/dev/null 2>&1 || true
git -C atop checkout --quiet "$ATOP_VERSION" >/dev/null 2>&1 || true
make -C atop versdate.h >/dev/null 2>&1 || true

build_http_args() {
	echo ">> building fuzz_http_args"
	$CC $CFLAGS -c http_args.c -o http_args.o
	# shellcheck disable=SC2086
	$CC $CFLAGS -fsanitize=fuzzer fuzz/fuzz_http_args.c http_args.o -o fuzz/fuzz_http_args $LDLIBS
}

run_http_args() {
	echo "=== fuzzing http_args (${SECONDS_PER_TARGET}s) ==="
	./fuzz/fuzz_http_args "${RUN_OPTS[@]}" "$CORPUS/http_args"
}

build_rawlog() {
	echo ">> building atop (raw-log seed generator) + version.o"
	make -C atop atop >/dev/null 2>&1 || echo "   (warning: could not build atop; using existing seeds only)"
	$CC $CFLAGS -c atop/version.c -o version.o

	# Seed the rawlog corpus from a freshly generated atop sample if empty.
	if [[ -x atop/atop ]] && ! ls "$CORPUS/rawlog"/* >/dev/null 2>&1; then
		if atop/atop -w "$CORPUS/rawlog/seed_sample" 1 3 >/dev/null 2>&1; then
			echo "   seeded rawlog corpus from a live atop sample"
		fi
	fi

	echo ">> building fuzz_rawlog"
	for m in cache json output log; do
		$CC $CFLAGS -c "$m.c" -o "$m.o"
	done
	# shellcheck disable=SC2086
	$CC $CFLAGS -fsanitize=fuzzer fuzz/fuzz_rawlog.c \
		cache.o json.o output.o log.o version.o -o fuzz/fuzz_rawlog $LDLIBS
}

run_rawlog() {
	echo "=== fuzzing rawlog (${SECONDS_PER_TARGET}s) ==="
	./fuzz/fuzz_rawlog "${RUN_OPTS[@]}" "$CORPUS/rawlog"
}

case "$TARGET" in
	http_args) build_http_args; run_http_args ;;
	rawlog)    build_rawlog;    run_rawlog ;;
	all)       build_http_args; build_rawlog; run_http_args; run_rawlog ;;
	*) echo "unknown target: $TARGET (use http_args | rawlog | all)" >&2; exit 2 ;;
esac

echo
echo "================= FUZZING CLEAN ================="
make clean >/dev/null 2>&1 || true
