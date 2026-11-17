#!/bin/sh
# T580/Linux-only source-model test, no RTL hardware or privileged operations.
set -eu
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/kraken-noise-off-test.XXXXXX")
trap 'rm -rf "$test_dir"' EXIT HUP INT TERM
"${CXX:-c++}" -std=c++20 -O1 -g -Wall -Wextra -Werror -pthread \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    tests/noise_off_result_test.cpp -o "$test_dir/test"
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$test_dir/test"
