#!/bin/bash
LV2_DIR="${LV2_DIR:-/usr/local/lib/lv2}"
UI_DIR="${ZYNTHIAN_UI_DIR:-/zynthian/zynthian-ui}"
rm -rf "$LV2_DIR/generative_arp.lv2"
rm -f  "$UI_DIR/zyngui/widgets_user/zynthian_widget_generative_arp.py"
echo "Removed links."
