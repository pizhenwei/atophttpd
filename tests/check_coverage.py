#!/usr/bin/env python3
"""
Parse an lcov .info file and enforce a minimum line-coverage threshold.

Usage: check_coverage.py <coverage.info> [threshold_percent]

Prints a per-file breakdown and the overall total, then exits non-zero if the
overall line coverage is below the threshold (default 90%).
"""
import sys


def parse_lcov(path):
    files = {}
    cur = None
    found = hit = 0
    with open(path) as fh:
        for line in fh:
            line = line.strip()
            if line.startswith("SF:"):
                cur = line[3:]
                found = hit = 0
            elif line.startswith("LF:"):
                found = int(line[3:])
            elif line.startswith("LH:"):
                hit = int(line[3:])
            elif line == "end_of_record" and cur is not None:
                files[cur] = (hit, found)
                cur = None
    return files


def main():
    if len(sys.argv) < 2:
        print("usage: check_coverage.py <coverage.info> [threshold]")
        return 2
    info = sys.argv[1]
    threshold = float(sys.argv[2]) if len(sys.argv) > 2 else 90.0

    files = parse_lcov(info)
    if not files:
        print("no coverage data found in %s" % info)
        return 2

    total_hit = total_found = 0
    width = max(len(f.split("/")[-1]) for f in files)
    print("%-*s  %8s  %8s  %7s" % (width, "file", "hit", "lines", "cover"))
    print("-" * (width + 30))
    for f in sorted(files):
        hit, found = files[f]
        total_hit += hit
        total_found += found
        pct = 100.0 * hit / found if found else 100.0
        print("%-*s  %8d  %8d  %6.1f%%" %
              (width, f.split("/")[-1], hit, found, pct))
    print("-" * (width + 30))
    total_pct = 100.0 * total_hit / total_found if total_found else 100.0
    print("%-*s  %8d  %8d  %6.1f%%" %
          (width, "TOTAL", total_hit, total_found, total_pct))
    print()

    if total_pct + 1e-9 < threshold:
        print("FAIL: line coverage %.1f%% is below the %.1f%% threshold"
              % (total_pct, threshold))
        return 1
    print("PASS: line coverage %.1f%% meets the %.1f%% threshold"
          % (total_pct, threshold))
    return 0


if __name__ == "__main__":
    sys.exit(main())
