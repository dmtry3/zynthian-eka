#!/bin/bash
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
"$ROOT/scripts/build.sh"
LV2_DIR="${LV2_DIR:-/usr/local/lib/lv2}"
UI_DIR="${ZYNTHIAN_UI_DIR:-/zynthian/zynthian-ui}"
mkdir -p "$LV2_DIR" "$UI_DIR/zyngui/widgets_user"
# Remove links left from the old plugin names (before the Eka rename), so Zynthian doesn't list it twice.
for old in midi_looper.lv2; do [ -L "$LV2_DIR/$old" ] && rm -f "$LV2_DIR/$old"; done
for old in zynthian_widget_midi_looper.py; do [ -L "$UI_DIR/zyngui/widgets_user/$old" ] && rm -f "$UI_DIR/zyngui/widgets_user/$old"; done
ln -sfn "$ROOT/build/eka-roll.lv2" "$LV2_DIR/eka-roll.lv2"
ln -sfn "$ROOT/ui/zynthian_widget_eka_roll.py" "$UI_DIR/zyngui/widgets_user/zynthian_widget_eka_roll.py"
# Saved slots used to live in my-data/midi-looper: move them to the new folder once.
MY_DATA="${ZYNTHIAN_MY_DATA_DIR:-/zynthian/zynthian-my-data}"
if [ -d "$MY_DATA/midi-looper" ] && [ ! -e "$MY_DATA/eka-roll" ]; then
    mv "$MY_DATA/midi-looper" "$MY_DATA/eka-roll" && echo "Moved saved slots: $MY_DATA/eka-roll"
fi
echo "Installed. Restart Zynthian: systemctl restart zynthian"
