#!/bin/bash
# Builds and runs the tests: grain_fx without LV2 (renders wav files to listen to in build_test/),
# then the built plugin in a tiny test host.
#   ./scripts/test.sh
#   SAN=1 ./scripts/test.sh          with AddressSanitizer and UBSan (slower; not the plugin .so itself)
#   STRESS=600 ./scripts/test.sh     seconds of random parameters in the stress test (default 30)
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
"$ROOT/scripts/build.sh"
[ -z "$LV2_CFLAGS" ] && LV2_CFLAGS="$(pkg-config --cflags lv2)"
mkdir -p "$ROOT/build_test"
OPT="-O2"
[ -n "$SAN" ] && OPT="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
gcc $OPT -Wall -Wextra -I"$ROOT/build" -o "$ROOT/build_test/test_core" "$ROOT/test/test_core.c" "$ROOT/src/grain_fx.c" -lm
gcc $OPT -Wall -Wextra $LV2_CFLAGS -o "$ROOT/build_test/test_plugin" "$ROOT/test/test_plugin.c" -ldl -lm
cd "$ROOT/build_test"
./test_core "${STRESS:-30}"
./test_plugin "$ROOT/build/eka-grainflow.lv2/eka_grainflow.so" "$ROOT/build/eka-grainflow.lv2/presets.ttl"
python3 "$ROOT/test/widget_check.py"
