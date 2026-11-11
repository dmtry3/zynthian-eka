#!/bin/bash
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
B="$ROOT/build/eka-roll.lv2"
mkdir -p "$B"
cp "$ROOT/lv2/manifest.ttl" "$ROOT/lv2/eka_roll.ttl" "$B/"
gcc -O2 -fPIC -shared -Wall $(pkg-config --cflags lv2) \
    -o "$B/eka_roll.so" "$ROOT/src/eka_roll.c" -lm -pthread
echo "Built: $B"
