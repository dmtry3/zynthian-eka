#!/bin/bash
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
B="$ROOT/build/eka-fold.lv2"
if ! pkg-config --exists lv2; then
    echo "Missing headers: sudo apt install lv2-dev" >&2
    exit 1
fi
mkdir -p "$B"
cp "$ROOT/lv2/manifest.ttl" "$ROOT/lv2/eka_fold.ttl" "$B/"
# Pi 5 (Cortex-A76): tuned build; elsewhere (WSL) a generic one, only to check that it compiles.
CPU=""
[ "$(uname -m)" = "aarch64" ] && CPU="-mcpu=cortex-a76"
source "$ROOT/scripts/engines.sh"
O="$ROOT/build_obj"   # not in build/: lv2ls takes every folder there for a bundle
# only lv2_descriptor is exported (LV2_SYMBOL_EXPORT): the engine names cannot clash with another plugin
build_engines "$O" "-O3 $CPU -fPIC -fvisibility=hidden"
gcc -O3 $CPU -fPIC -fvisibility=hidden -Wall -Wextra $(pkg-config --cflags lv2) -c "$ROOT/src/eka_fold.c" -o "$O/eka_fold.o"
gcc -O3 $CPU -fPIC -fvisibility=hidden -Wall -Wextra -c "$ROOT/src/fold_core.c" -o "$O/fold_core.o"
g++ -shared -o "$B/eka_fold.so" "$O/eka_fold.o" "$O/fold_core.o" $ENGINE_OBJS -lm
# the engine tables for the widget (Osc and Fold tabs)
gcc -O2 -Wall -Wextra -o "$O/dump_waves" "$ROOT/scripts/dump_waves.c" "$O/tables.o"
"$O/dump_waves" "$B/engine_tables.bin"
echo "Built: $B"
