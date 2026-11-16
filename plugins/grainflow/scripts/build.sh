#!/bin/bash
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
B="$ROOT/build/eka-grainflow.lv2"
# LV2 headers: pkg-config, or LV2_CFLAGS="-I/path/to/lv2/include" when they are not installed
if [ -z "$LV2_CFLAGS" ]; then
    if ! pkg-config --exists lv2; then
        echo "Missing headers: sudo apt install lv2-dev" >&2
        exit 1
    fi
    LV2_CFLAGS="$(pkg-config --cflags lv2)"
fi
mkdir -p "$B"
python3 "$ROOT/scripts/gen_ttl.py" > /dev/null
cp "$ROOT/lv2/manifest.ttl" "$ROOT/lv2/eka_grainflow.ttl" "$ROOT/lv2/presets.ttl" "$B/"
# Pi 5 (Cortex-A76): tuned build; elsewhere (WSL) a generic one, only to check that it compiles.
CPU=""
[ "$(uname -m)" = "aarch64" ] && CPU="-mcpu=cortex-a76"
gcc -O3 $CPU -fPIC -shared -Wall -Wextra $LV2_CFLAGS \
    -o "$B/eka_grainflow.so" "$ROOT/src/eka_grainflow.c" "$ROOT/src/grain_fx.c" -lm
echo "Built: $B"
