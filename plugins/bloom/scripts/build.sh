#!/bin/bash
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
B="$ROOT/build/generative_arp.lv2"
mkdir -p "$B"
cp "$ROOT/lv2/manifest.ttl" "$ROOT/lv2/generative_arp.ttl" "$ROOT/lv2/presets.ttl" "$B/"
gcc -O2 -fPIC -shared -Wall $(pkg-config --cflags lv2) \
    -o "$B/generative_arp.so" "$ROOT/src/generative_arp.c" "$ROOT/src/arp_core.c" -lm
echo "Built: $B"
