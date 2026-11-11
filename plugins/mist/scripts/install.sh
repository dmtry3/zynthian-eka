#!/bin/bash
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
"$ROOT/scripts/build.sh"
LV2_DIR="${LV2_DIR:-/usr/local/lib/lv2}"
UI_DIR="${ZYNTHIAN_UI_DIR:-/zynthian/zynthian-ui}"
MY_DATA="${ZYNTHIAN_MY_DATA_DIR:-/zynthian/zynthian-my-data}"
mkdir -p "$LV2_DIR"
# Remove links left from the old plugin names (before the Eka rename), so Zynthian doesn't list it twice.
for old in granular_sampler.lv2; do [ -L "$LV2_DIR/$old" ] && rm -f "$LV2_DIR/$old"; done
for old in zynthian_widget_granular_sampler.py; do [ -L "$UI_DIR/zyngui/widgets_user/$old" ] && rm -f "$UI_DIR/zyngui/widgets_user/$old"; done
ln -sfn "$ROOT/build/eka-mist.lv2" "$LV2_DIR/eka-mist.lv2"
if [ -f "$ROOT/ui/zynthian_widget_eka_mist.py" ]; then
    mkdir -p "$UI_DIR/zyngui/widgets_user"
    ln -sfn "$ROOT/ui/zynthian_widget_eka_mist.py" "$UI_DIR/zyngui/widgets_user/zynthian_widget_eka_mist.py"
fi
# Samples: Zynthian's file selector shows files/Samples as "User Samples". Our factory samples
# are a link inside it; the user's own samples go into an empty folder next to it (a real folder,
# not in the repo: it survives reinstalls and git pulls).
# Old names (before the Eka rename): drop the factory link, move the user's samples folder once.
[ -L "$MY_DATA/files/Samples/granular-sampler" ] && rm -f "$MY_DATA/files/Samples/granular-sampler"
if [ -d "$MY_DATA/files/Samples/granular-sampler-user" ] && [ ! -e "$MY_DATA/files/Samples/eka-mist-user" ]; then
    mv "$MY_DATA/files/Samples/granular-sampler-user" "$MY_DATA/files/Samples/eka-mist-user"
fi
mkdir -p "$MY_DATA/files/Samples/eka-mist-user"
ln -sfn "$ROOT/samples" "$MY_DATA/files/Samples/eka-mist"
echo "Samples: $MY_DATA/files/Samples (factory in eka-mist/, yours in eka-mist-user/)"
# User presets bundle. Zynthian's "New bank" appends to its manifest.ttl but doesn't create the folder
# (jalv creates it only on the first save), so saving into a new bank fails silently until it exists.
USER_PSET="$MY_DATA/presets/lv2/Eka_Mist.presets.lv2"
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
    ( set +e; source "$ENVARS"; cd "$UI_DIR" && python3 zyngine/zynthian_lv2.py presets http://dmtry3.github.io/zynthian-eka/mist ) \
        && echo "Preset list updated." || echo "Warning: preset list not updated (run install.sh again on Zynthian)."
else
    echo "Note: $ENVARS or zynthian_lv2.py not found, preset list cache not updated."
fi
echo "Installed. Restart Zynthian: systemctl restart zynthian"
