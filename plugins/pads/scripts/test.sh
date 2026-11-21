#!/bin/bash
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
"$ROOT/scripts/build.sh"
mkdir -p "$ROOT/build_test"
OPT="-O2"
[ -n "$SAN" ] && OPT="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"

gcc $OPT -Wall -Wextra $(pkg-config --cflags sndfile) -o "$ROOT/build_test/test_core" \
    "$ROOT/test/test_core.c" "$ROOT/src/pads_core.c" "$ROOT/src/sample.c" \
    $(pkg-config --libs sndfile) -lm

gcc $OPT -Wall -Wextra $(pkg-config --cflags lv2) -o "$ROOT/build_test/test_plugin" \
    "$ROOT/test/test_plugin.c" -ldl -lm

"$ROOT/build_test/test_core" "$ROOT/samples"
"$ROOT/build_test/test_plugin" "$ROOT/build/eka-pads.lv2/eka_pads.so" "$ROOT/samples/01_kick.wav"
python3 "$ROOT/test/test_widget.py" "$ROOT/ui/zynthian_widget_eka_pads.py"
echo "All Eka Pads tests passed."
