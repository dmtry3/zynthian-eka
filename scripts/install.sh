#!/bin/bash
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
"$ROOT/scripts/build.sh"
LV2_DIR="${LV2_DIR:-/usr/local/lib/lv2}"
UI_DIR="${ZYNTHIAN_UI_DIR:-/zynthian/zynthian-ui}"
mkdir -p "$LV2_DIR" "$UI_DIR/zyngui/widgets_user"
ln -sfn "$ROOT/build/generative_arp.lv2" "$LV2_DIR/generative_arp.lv2"
ln -sfn "$ROOT/ui/zynthian_widget_generative_arp.py" "$UI_DIR/zyngui/widgets_user/zynthian_widget_generative_arp.py"
ENVARS="${ZYNTHIAN_ENVARS:-/zynthian/config/zynthian_envars.sh}"
# User presets bundle. Zynthian's "New bank" appends to its manifest.ttl but doesn't create the folder
# (jalv creates it only on the first save), so saving into a new bank fails silently until it exists.
MY_DATA_DIR="$( [ -f "$ENVARS" ] && source "$ENVARS" >/dev/null 2>&1; echo "${ZYNTHIAN_MY_DATA_DIR:-/zynthian/zynthian-my-data}" )"
if [ -d "$MY_DATA_DIR" ]; then
    USER_PSET="$MY_DATA_DIR/presets/lv2/Generative_Arp.presets.lv2"
    mkdir -p "$USER_PSET"
    [ -s "$USER_PSET/manifest.ttl" ] || cat > "$USER_PSET/manifest.ttl" <<'EOF'
@prefix lv2:   <http://lv2plug.in/ns/lv2core#> .
@prefix pset:  <http://lv2plug.in/ns/ext/presets#> .
@prefix rdfs:  <http://www.w3.org/2000/01/rdf-schema#> .
EOF
    echo "User presets: $USER_PSET"
fi
# Zynthian keeps a cache of each plugin's preset list: rebuild it for this plugin only, with Zynthian's own tool.
if [ -f "$UI_DIR/zyngine/zynthian_lv2.py" ] && [ -f "$ENVARS" ]; then
    ( set +e; source "$ENVARS"; cd "$UI_DIR" && python3 zyngine/zynthian_lv2.py presets urn:generativearp:arp ) \
        && echo "Preset list updated." || echo "Warning: preset list not updated (run install.sh again on Zynthian)."
else
    echo "Note: $ENVARS or zynthian_lv2.py not found, preset list cache not updated."
fi
echo "Installed. Restart Zynthian: systemctl restart zynthian"
