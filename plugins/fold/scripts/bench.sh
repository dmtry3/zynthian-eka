#!/bin/bash
# CPU of the engine (fold_bench) and the aliasing table of the FM ways (alias_report).
# Only the CPU figures measured on the Pi 5 count.
#   ./scripts/bench.sh
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$ROOT/build_test"
CPU=""
[ "$(uname -m)" = "aarch64" ] && CPU="-mcpu=cortex-a76"
gcc -O3 $CPU -Wall -Wextra -o "$ROOT/build_test/fold_bench" "$ROOT/test/fold_bench.c" "$ROOT/src/fold_core.c" -lm
gcc -O2 -Wall -Wextra -o "$ROOT/build_test/alias_report" "$ROOT/test/alias_report.c" "$ROOT/src/fold_core.c" -lm
"$ROOT/build_test/fold_bench"
echo
"$ROOT/build_test/alias_report"
