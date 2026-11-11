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
gcc -O3 $CPU -fPIC -shared -Wall -Wextra $(pkg-config --cflags lv2) \
    -o "$B/eka_fold.so" "$ROOT/src/eka_fold.c" "$ROOT/src/fold_core.c" -lm
echo "Built: $B"
