#!/bin/bash
LV2_DIR="${LV2_DIR:-/usr/local/lib/lv2}"
UI_DIR="${ZYNTHIAN_UI_DIR:-/zynthian/zynthian-ui}"
rm -rf "$LV2_DIR/eka-q4.lv2"
rm -f  "$UI_DIR/zyngui/widgets_user/zynthian_widget_eka_q4.py"
rm -f  "$UI_DIR/zyngui/widgets_user/zynthian_widget_eka_q-4.py"
echo "Removed links."
