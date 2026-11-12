#!/bin/bash
# CPU of the engine (fold_bench) and the aliasing table of the FM ways (alias_report).
# Only the CPU figures measured on the Pi 5 count.
#   ./scripts/bench.sh
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$ROOT/build_test"
CPU=""
[ "$(uname -m)" = "aarch64" ] && CPU="-mcpu=cortex-a76"
source "$ROOT/scripts/engines.sh"
O="$ROOT/build_test/obj_bench"
build_engines "$O" "-O3 $CPU"
gcc -O3 $CPU -Wall -Wextra -c "$ROOT/src/fold_core.c" -o "$O/fold_core.o"
gcc -O3 $CPU -Wall -Wextra -o "$ROOT/build_test/fold_bench.o" -c "$ROOT/test/fold_bench.c"
g++ -o "$ROOT/build_test/fold_bench" "$ROOT/build_test/fold_bench.o" "$O/fold_core.o" $ENGINE_OBJS -lm
gcc -O2 -Wall -Wextra -o "$ROOT/build_test/alias_report.o" -c "$ROOT/test/alias_report.c"
g++ -o "$ROOT/build_test/alias_report" "$ROOT/build_test/alias_report.o" "$O/fold_core.o" $ENGINE_OBJS -lm
"$ROOT/build_test/fold_bench"
echo
"$ROOT/build_test/alias_report"
