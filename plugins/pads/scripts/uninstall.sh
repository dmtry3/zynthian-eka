#!/bin/bash
LV2_DIR="${LV2_DIR:-/usr/local/lib/lv2}"
UI_DIR="${ZYNTHIAN_UI_DIR:-/zynthian/zynthian-ui}"
MY_DATA="${ZYNTHIAN_MY_DATA_DIR:-/zynthian/zynthian-my-data}"
rm -rf "$LV2_DIR/eka-pads.lv2"
rm -f  "$UI_DIR/zyngui/widgets_user/zynthian_widget_eka_pads.py"
[ -L "$MY_DATA/files/Samples/eka-pads" ] && rm -f "$MY_DATA/files/Samples/eka-pads"
rmdir "$MY_DATA/files/Samples/eka-pads-user" 2>/dev/null || true
echo "Removed Eka Pads links."
