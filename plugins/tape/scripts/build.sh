#!/bin/bash
# Собирает плагин в build/eka-tape.lv2 (на Pi и в WSL одинаково).
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
B="$ROOT/build/eka-tape.lv2"
mkdir -p "$B"
cp "$ROOT/lv2/manifest.ttl" "$ROOT/lv2/eka_tape.ttl" "$B/"
g++ -std=c++17 -O2 -fPIC -shared -Wall -Wextra -fno-exceptions -fno-rtti -fvisibility=hidden \
    $(pkg-config --cflags lv2) \
    -o "$B/eka_tape.so" "$ROOT/lv2/eka_tape.cpp" "$ROOT"/core/*.cpp -lm
echo "Built: $B"
