#!/bin/bash
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
"$ROOT/scripts/build.sh"
mkdir -p "$ROOT/build_test"
OPT="-O2"
[ -n "$SAN" ] && OPT="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"

gcc $OPT -Wall -Wextra $(pkg-config --cflags sndfile) -o "$ROOT/build_test/test_core" \
    "$ROOT/test/test_core.c" "$ROOT/src/pads_core.c" "$ROOT/src/sample.c" "$ROOT/src/aux_jack.c" \
    $(pkg-config --libs sndfile) -lpthread -ldl -lm

gcc $OPT -Wall -Wextra $(pkg-config --cflags lv2) -o "$ROOT/build_test/test_plugin" \
    "$ROOT/test/test_plugin.c" -ldl -lm

"$ROOT/build_test/test_core" "$ROOT/samples"
"$ROOT/build_test/test_plugin" "$ROOT/build/eka-pads.lv2/eka_pads.so" "$ROOT/samples/01_kick.wav"
python3 "$ROOT/test/test_widget.py" "$ROOT/ui/zynthian_widget_eka_pads.py"

# Test optional SMC-PAD patch script against a temporary mock Zynthian driver
TMP_UI="$(mktemp -d)"
trap 'rm -rf "$TMP_UI"' EXIT
mkdir -p "$TMP_UI/zyngine/ctrldev"
cat > "$TMP_UI/zyngine/ctrldev/zynthian_ctrldev_sinco_smk25.py" <<'EOF'
class zynthian_ctrldev_sinco_smk25:
    dev_ids = ["SINCO IN 2"]
    driver_name = "SINCO SMK25"
EOF
! ZYNTHIAN_UI_DIR="$TMP_UI" "$ROOT/scripts/patch-smcpad.sh" --status >/dev/null
ZYNTHIAN_UI_DIR="$TMP_UI" "$ROOT/scripts/patch-smcpad.sh"
ZYNTHIAN_UI_DIR="$TMP_UI" "$ROOT/scripts/patch-smcpad.sh" --status
ZYNTHIAN_UI_DIR="$TMP_UI" "$ROOT/scripts/patch-smcpad.sh"
ZYNTHIAN_UI_DIR="$TMP_UI" "$ROOT/scripts/patch-smcpad.sh" --revert
! ZYNTHIAN_UI_DIR="$TMP_UI" "$ROOT/scripts/patch-smcpad.sh" --status >/dev/null

echo "All Eka Pads tests passed."
