#!/bin/bash
LV2_DIR="${LV2_DIR:-/usr/local/lib/lv2}"
UI_DIR="${ZYNTHIAN_UI_DIR:-/zynthian/zynthian-ui}"
rm -rf "$LV2_DIR/eka-fold.lv2"
rm -f  "$UI_DIR/zyngui/widgets_user/zynthian_widget_eka_fold.py"
# the user presets bundle (zynthian-my-data/presets/lv2/Eka_Fold.presets.lv2) stays
echo "Removed links."
