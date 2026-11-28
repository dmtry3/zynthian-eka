#!/bin/bash
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
B="$ROOT/build/eka-pads.lv2"
if ! pkg-config --exists lv2 sndfile; then
    echo "Missing headers: sudo apt install lv2-dev libsndfile1-dev" >&2
    exit 1
fi
mkdir -p "$B"
cp "$ROOT/lv2/manifest.ttl" "$ROOT/lv2/eka_pads.ttl" "$B/"
CPU=""
[ "$(uname -m)" = "aarch64" ] && CPU="${EKA_AARCH64_CPU--mcpu=cortex-a76}"  # CI: cortex-a72, runs on Pi 4 and Pi 5
gcc -O3 $CPU -fPIC -shared -Wall -Wextra $(pkg-config --cflags lv2 sndfile) \
    -o "$B/eka_pads.so" "$ROOT/src/eka_pads.c" "$ROOT/src/pads_core.c" "$ROOT/src/sample.c" "$ROOT/src/aux_jack.c" \
    $(pkg-config --libs sndfile) -lpthread -ldl -lm
echo "Built: $B"
