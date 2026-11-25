#!/bin/bash
# Optional Zynthian patch for M-VAVE SMC-PAD / SMC-PAD Pocket over USB.
#
# Both M-VAVE SMC-PAD and SINCO SMK25 expose the USB MIDI port "SINCO IN 2".
# Zynthian autoloads zynthian_ctrldev_sinco_smk25.py on that port, which sends
# SysEx and overwrites the SMC-PAD layout. This script sets autoload_flag = False
# in that driver so it stops autoloading (it can still be enabled manually).
#
# Usage:
#   ./scripts/patch-smcpad.sh           # apply patch
#   ./scripts/patch-smcpad.sh --revert  # undo patch
#   ./scripts/patch-smcpad.sh --status  # check whether patch is applied
set -euo pipefail

UI_DIR="${ZYNTHIAN_UI_DIR:-/zynthian/zynthian-ui}"
DRIVER="$UI_DIR/zyngine/ctrldev/zynthian_ctrldev_sinco_smk25.py"
MODE="apply"

case "${1:-}" in
    ""|--apply)  MODE="apply" ;;
    --revert)    MODE="revert" ;;
    --status)    MODE="status" ;;
    -h|--help)
        sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'
        exit 0
        ;;
    *)
        echo "Usage: $0 [--apply|--revert|--status]" >&2
        exit 1
        ;;
esac

if [ ! -f "$DRIVER" ]; then
    echo "Driver not found: $DRIVER (nothing to patch)."
    exit 0
fi

python3 - "$DRIVER" "$MODE" <<'PY'
import py_compile
import re
import sys

path, mode = sys.argv[1], sys.argv[2]
with open(path, "r", encoding="utf-8") as f:
    src = f.read()

MARKER = "    autoload_flag = False  # eka-pads: prevent SINCO SMK25 autoload on M-VAVE SMC-PAD\n"
DEV_ID_RE = re.compile(r'^([ \t]*dev_ids\s*=\s*\[[^\n]*SINCO IN 2[^\n]*\]\n)', re.MULTILINE)
AUTOLOAD_FALSE_RE = re.compile(r'^[ \t]*autoload_flag\s*=\s*False\b.*$', re.MULTILINE)
AUTOLOAD_TRUE_RE = re.compile(r'^([ \t]*)autoload_flag\s*=\s*True\b.*$', re.MULTILINE)

# Restrict checks/edits to the zynthian_ctrldev_sinco_smk25 class section.
cls_match = re.search(r'^class\s+zynthian_ctrldev_sinco_smk25\b', src, re.MULTILINE)
if not cls_match:
    print(f"Class zynthian_ctrldev_sinco_smk25 not found in {path}", file=sys.stderr)
    sys.exit(1)

head = src[:cls_match.start()]
body = src[cls_match.start():]

if mode == "status":
    if AUTOLOAD_FALSE_RE.search(body):
        print(f"Patched (autoload_flag = False): {path}")
        sys.exit(0)
    else:
        print(f"Not patched (autoload enabled): {path}")
        sys.exit(1)

if mode == "apply":
    if AUTOLOAD_FALSE_RE.search(body):
        print(f"Already patched: {path}")
        sys.exit(0)
    if AUTOLOAD_TRUE_RE.search(body):
        new_body = AUTOLOAD_TRUE_RE.sub(
            r'\1autoload_flag = False  # eka-pads: prevent SINCO SMK25 autoload on M-VAVE SMC-PAD',
            body,
            count=1,
        )
    else:
        m = DEV_ID_RE.search(body)
        if not m:
            print(f'Expected dev_ids with "SINCO IN 2" not found in {path}', file=sys.stderr)
            sys.exit(1)
        new_body = body[:m.end()] + MARKER + body[m.end():]
    new_src = head + new_body
    with open(path, "w", encoding="utf-8") as f:
        f.write(new_src)
    py_compile.compile(path, doraise=True)
    print(f"Patched {path} (set autoload_flag = False).")
    print("Restart Zynthian: systemctl restart zynthian")
elif mode == "revert":
    if MARKER in body:
        new_body = body.replace(MARKER, "", 1)
    elif AUTOLOAD_FALSE_RE.search(body):
        new_body = re.sub(r'^[ \t]*autoload_flag\s*=\s*False\b.*\n?', '', body, count=1, flags=re.MULTILINE)
    else:
        print(f"Nothing to revert in {path}.")
        sys.exit(0)
    new_src = head + new_body
    with open(path, "w", encoding="utf-8") as f:
        f.write(new_src)
    py_compile.compile(path, doraise=True)
    print(f"Reverted {path}.")
    print("Restart Zynthian: systemctl restart zynthian")
PY
