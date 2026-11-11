#!/bin/bash
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
"$ROOT/scripts/build.sh"
LV2_DIR="${LV2_DIR:-/usr/local/lib/lv2}"
UI_DIR="${ZYNTHIAN_UI_DIR:-/zynthian/zynthian-ui}"
MY_DATA="${ZYNTHIAN_MY_DATA_DIR:-/zynthian/zynthian-my-data}"
mkdir -p "$LV2_DIR"
ln -sfn "$ROOT/build/eka-fold.lv2" "$LV2_DIR/eka-fold.lv2"
if [ -f "$ROOT/ui/zynthian_widget_eka_fold.py" ]; then
    mkdir -p "$UI_DIR/zyngui/widgets_user"
    ln -sfn "$ROOT/ui/zynthian_widget_eka_fold.py" "$UI_DIR/zyngui/widgets_user/zynthian_widget_eka_fold.py"
fi
# User presets bundle. Zynthian's "New bank" appends to its manifest.ttl but doesn't create the folder
# (jalv creates it only on the first save), so saving into a new bank fails silently until it exists.
USER_PSET="$MY_DATA/presets/lv2/Eka_Fold.presets.lv2"
mkdir -p "$USER_PSET"
[ -s "$USER_PSET/manifest.ttl" ] || cat > "$USER_PSET/manifest.ttl" <<'EOF2'
@prefix lv2:   <http://lv2plug.in/ns/lv2core#> .
@prefix pset:  <http://lv2plug.in/ns/ext/presets#> .
@prefix rdfs:  <http://www.w3.org/2000/01/rdf-schema#> .
EOF2
echo "User presets: $USER_PSET"
# Zynthian keeps a cache of each plugin's preset list: rebuild it for this plugin only, with Zynthian's own tool.
ENVARS="${ZYNTHIAN_ENVARS:-/zynthian/config/zynthian_envars.sh}"
if [ -f "$UI_DIR/zyngine/zynthian_lv2.py" ] && [ -f "$ENVARS" ]; then
    ( set +e; source "$ENVARS"; cd "$UI_DIR" && python3 zyngine/zynthian_lv2.py presets http://dmtry3.github.io/zynthian-eka/fold ) \
        && echo "Preset list updated." || echo "Warning: preset list not updated (run install.sh again on Zynthian)."
else
    echo "Note: $ENVARS or zynthian_lv2.py not found, preset list cache not updated."
fi
echo "Installed. Restart Zynthian: systemctl restart zynthian"
