#!/bin/bash
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
B="$ROOT/build/eka-pulse.lv2"
if [ -z "$LV2_CFLAGS" ]; then
    if ! pkg-config --exists lv2; then
        echo "Missing headers: sudo apt install lv2-dev" >&2
        exit 1
    fi
    LV2_CFLAGS="$(pkg-config --cflags lv2)"
fi
mkdir -p "$B"
python3 "$ROOT/scripts/gen_ttl.py" > /dev/null
cp "$ROOT/lv2/manifest.ttl" "$ROOT/lv2/eka_pulse.ttl" "$B/"
CPU=""
[ "$(uname -m)" = "aarch64" ] && CPU="-mcpu=cortex-a76"
gcc -std=c11 -O3 $CPU -fPIC -shared -Wall -Wextra $LV2_CFLAGS \
    -o "$B/eka_pulse.so" "$ROOT/src/eka_pulse.c" "$ROOT/src/pulse_fx.c" -lm
echo "Built: $B"
