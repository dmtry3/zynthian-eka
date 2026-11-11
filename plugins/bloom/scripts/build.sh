#!/bin/bash
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
B="$ROOT/build/eka-bloom.lv2"
mkdir -p "$B"
cp "$ROOT/lv2/manifest.ttl" "$ROOT/lv2/eka_bloom.ttl" "$ROOT/lv2/presets.ttl" "$B/"
gcc -O2 -fPIC -shared -Wall $(pkg-config --cflags lv2) \
    -o "$B/eka_bloom.so" "$ROOT/src/eka_bloom.c" "$ROOT/src/arp_core.c" -lm
echo "Built: $B"
