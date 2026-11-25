#!/usr/bin/env bash
# Synthetic bounded host-source check; compiles guard with sanitizers.
set -euo pipefail
cd "$(dirname "$0")/.."
tmp=$(mktemp -d "${TMPDIR:-/tmp}/kraken-cal-evidence.XXXXXXXX")
trap 'rm -rf -- "$tmp"' EXIT HUP INT TERM
c++ -std=c++20 -O1 -g0 -Wall -Wextra -Wpedantic -Werror \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    tests/calibration_evidence_guard_test.cpp -o "$tmp/test"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "$tmp/test"
