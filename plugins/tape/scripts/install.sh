#!/bin/bash
# Запускается на Zynthian: собирает плагин и ставит две ссылки (как у прошлых плагинов).
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
"$ROOT/scripts/build.sh"
LV2_DIR="${LV2_DIR:-/usr/local/lib/lv2}"
UI_DIR="${ZYNTHIAN_UI_DIR:-/zynthian/zynthian-ui}"
mkdir -p "$LV2_DIR" "$UI_DIR/zyngui/widgets_user"
# Remove links left from the old plugin names (before the Eka rename), so Zynthian doesn't list it twice.
for old in audio_looper.lv2 sono-tape.lv2; do [ -L "$LV2_DIR/$old" ] && rm -f "$LV2_DIR/$old"; done
for old in zynthian_widget_audio_looper.py zynthian_widget_sono_tape.py; do [ -L "$UI_DIR/zyngui/widgets_user/$old" ] && rm -f "$UI_DIR/zyngui/widgets_user/$old"; done
ln -sfn "$ROOT/build/eka-tape.lv2" "$LV2_DIR/eka-tape.lv2"
# Обновление Zynthian может стереть ссылку на виджет: install.sh ставит её заново.
ln -sfn "$ROOT/widget/zynthian_widget_eka_tape.py" "$UI_DIR/zyngui/widgets_user/zynthian_widget_eka_tape.py"
echo "Installed. Restart Zynthian: systemctl restart zynthian"
