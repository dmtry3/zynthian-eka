#!/bin/bash
# Builds and runs the tests: pulse_fx without LV2 (renders wav files to build_test/),
# the built LV2 plugin in a tiny test host, and the widget unit check.
#   ./scripts/test.sh
#   SAN=1 ./scripts/test.sh
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
"$ROOT/scripts/build.sh"
[ -z "$LV2_CFLAGS" ] && LV2_CFLAGS="$(pkg-config --cflags lv2)"
mkdir -p "$ROOT/build_test"
OPT="-O2"
[ -n "$SAN" ] && OPT="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
gcc -std=c11 $OPT -Wall -Wextra -o "$ROOT/build_test/test_core" "$ROOT/test/test_core.c" "$ROOT/src/pulse_fx.c" -lm
gcc -std=c11 $OPT -Wall -Wextra $LV2_CFLAGS -o "$ROOT/build_test/test_plugin" "$ROOT/test/test_plugin.c" -ldl -lm
cd "$ROOT/build_test"
./test_core
./test_plugin "$ROOT/build/eka-pulse.lv2/eka_pulse.so" "$ROOT/build/eka-pulse.lv2/presets.ttl"
python3 "$ROOT/test/widget_check.py"
