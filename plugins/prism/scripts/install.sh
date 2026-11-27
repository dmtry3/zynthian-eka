#!/bin/bash
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
"$ROOT/scripts/build.sh"
LV2_DIR="${LV2_DIR:-/usr/local/lib/lv2}"
UI_DIR="${ZYNTHIAN_UI_DIR:-/zynthian/zynthian-ui}"
mkdir -p "$LV2_DIR"
ln -sfn "$ROOT/build/eka-prism.lv2" "$LV2_DIR/eka-prism.lv2"
if [ -f "$ROOT/ui/zynthian_widget_eka_prism.py" ]; then
    mkdir -p "$UI_DIR/zyngui/widgets_user"
    ln -sfn "$ROOT/ui/zynthian_widget_eka_prism.py" "$UI_DIR/zyngui/widgets_user/zynthian_widget_eka_prism.py"
fi
MY_DATA="${ZYNTHIAN_MY_DATA_DIR:-/zynthian/zynthian-my-data}"
USER_PSET="$MY_DATA/presets/lv2/Eka_Prism.presets.lv2"
mkdir -p "$USER_PSET"
[ -s "$USER_PSET/manifest.ttl" ] || cat > "$USER_PSET/manifest.ttl" <<'EOF2'
@prefix lv2:   <http://lv2plug.in/ns/lv2core#> .
@prefix pset:  <http://lv2plug.in/ns/ext/presets#> .
@prefix rdfs:  <http://www.w3.org/2000/01/rdf-schema#> .
EOF2
echo "User presets: $USER_PSET"
ENVARS="${ZYNTHIAN_ENVARS:-/zynthian/config/zynthian_envars.sh}"
CFG_DIR="${ZYNTHIAN_CONFIG_DIR:-/zynthian/config}"
if [ -f "$UI_DIR/zyngine/zynthian_lv2.py" ] && [ -f "$ENVARS" ]; then
    ( set +e; source "$ENVARS"; cd "$UI_DIR" && python3 zyngine/zynthian_lv2.py presets http://dmtry3.github.io/zynthian-eka/prism ) \
        && echo "Preset list updated." || echo "Warning: preset list not updated (run install.sh again on Zynthian)."
fi
python3 - "$CFG_DIR" <<'PYEOF'
import json, os, sys
from pathlib import Path

cfg_dir = Path(sys.argv[1])
uri = "http://dmtry3.github.io/zynthian-eka/prism"
for rel in ("engine_config.json", "jalv/plugins.json"):
    p = cfg_dir / rel
    if not p.is_file():
        continue
    try:
        data = json.loads(p.read_text(encoding="utf-8"))
    except Exception:
        continue
    if not isinstance(data, dict):
        continue
    changed = False
    for k, v in data.items():
        if isinstance(v, dict) and (v.get("URL") == uri or k in ("JV/Eka Prism", "Eka Prism")):
            if v.get("CAT") != "Filter":
                v["CAT"] = "Filter"
                changed = True
            if "CLASS" in v and v["CLASS"] != "Filter":
                v["CLASS"] = "Filter"
                changed = True
    if changed:
        tmp = p.with_name(p.name + ".prism.tmp")
        tmp.write_text(json.dumps(data, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
        os.chmod(tmp, p.stat().st_mode & 0o777)
        tmp.replace(p)
        print(f"Updated category to 'Filter' in {p}")
PYEOF
echo "Installed. Restart Zynthian: systemctl restart zynthian"
echo "If Eka Prism is not in the list of audio effects: webconf -> Software -> LV2 Plugins, enable it there."
