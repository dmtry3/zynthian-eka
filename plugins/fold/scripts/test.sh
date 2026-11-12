#!/bin/bash
# Builds and runs the tests: fold_core without LV2 (offline render in build_test/), the built plugin in a tiny
# test host, the .ttl files through lv2ls (if installed), and the widget with stub Zynthian modules.
#   ./scripts/test.sh
#   SAN=1 ./scripts/test.sh          the tests with AddressSanitizer and UBSan (not the plugin .so itself)
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
"$ROOT/scripts/build.sh"
mkdir -p "$ROOT/build_test"
OPT="-O2"
[ -n "$SAN" ] && OPT="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
source "$ROOT/scripts/engines.sh"
build_engines "$ROOT/build_test/obj" "$OPT"
gcc $OPT -Wall -Wextra -c "$ROOT/test/test_core.c" -o "$ROOT/build_test/obj/test_core.o"
gcc $OPT -Wall -Wextra -c "$ROOT/src/fold_core.c" -o "$ROOT/build_test/obj/fold_core.o"
g++ $OPT -o "$ROOT/build_test/test_core" "$ROOT/build_test/obj/test_core.o" "$ROOT/build_test/obj/fold_core.o" \
    $ENGINE_OBJS -lm
gcc $OPT -Wall -Wextra $(pkg-config --cflags lv2) -o "$ROOT/build_test/test_plugin" "$ROOT/test/test_plugin.c" -ldl -lm
cd "$ROOT/build_test"
./test_core
./test_plugin "$ROOT/build/eka-fold.lv2/eka_fold.so"
if command -v lv2ls >/dev/null; then
    LV2_PATH="$ROOT/build" lv2ls | grep -qx "http://dmtry3.github.io/zynthian-eka/fold" \
        && echo "PASS: lv2ls finds the plugin" || { echo "FAIL: lv2ls does not find the plugin"; exit 1; }
else
    echo "Note: lv2ls not found (sudo apt install lilv-utils), .ttl not checked."
fi
# the engines as a small library: the widget's formulas are checked against them
build_engines "$ROOT/build_test/obj_pic" "-O2 -fPIC"
g++ -shared -o "$ROOT/build_test/libengines.so" $ENGINE_OBJS -lm
python3 "$ROOT/test/widget_preview.py"
