#!/bin/bash
# Copy local sources over SSH; build with the existing installers on Zynthian.
set -euo pipefail

usage() {
    echo "Usage: bash scripts/deploy-zynthian.sh <all|tape|roll|bloom|mist|fold> [user@host] [absolute-remote-directory]"
    echo "Defaults: all root@zynthian.local /root/zynthian-eka"
    echo "Installs local changes without GitHub; restarts Zynthian (audio stops)."
}
if [[ "${1:-}" == --help || "${1:-}" == -h ]]; then usage; exit 0; fi
if (( $# > 3 )); then usage >&2; exit 1; fi
PLUGIN="${1:-all}"
TARGET="${2:-root@zynthian.local}"
DEST="${3:-/root/zynthian-eka}"
case "$PLUGIN" in
    all) PLUGINS=(tape roll bloom mist fold) ;;
    tape|roll|bloom|mist|fold) PLUGINS=("$PLUGIN") ;;
    *) usage >&2; exit 1 ;;
esac
if [[ -z "$TARGET" || "$TARGET" == -* || "$DEST" != /* || "$DEST" == / || "$DEST" == *$'\n'* ]]; then
    echo "Specify an SSH destination and an absolute project directory (not /)." >&2
    exit 1
fi
for tool in ssh tar; do
    command -v "$tool" >/dev/null || { echo "Missing command: $tool" >&2; exit 1; }
done
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FILES=(Makefile common)
for p in "${PLUGINS[@]}"; do FILES+=("plugins/$p"); done
# Bash quoting protects destination paths, including spaces.
printf -v REMOTE_SCRIPT 'set -euo pipefail\nif [ "$EUID" -ne 0 ]; then echo "Connect as root to install on Zynthian." >&2; exit 1; fi\nif [ ! -f /zynthian/zynthian-ui/zyngine/zynthian_lv2.py ]; then echo "Zynthian UI not found on this host." >&2; exit 1; fi\nmkdir -p -- %q\ntar -xzf - -C %q\n' "$DEST" "$DEST"
printf -v COPY_CMD 'bash -c %q' "$REMOTE_SCRIPT"
printf -v REMOTE_SCRIPT 'set -euo pipefail\ncd -- %q\n' "$DEST"
for p in "${PLUGINS[@]}"; do
    REMOTE_SCRIPT+="make install-$p"$'\n'
done
REMOTE_SCRIPT+=$'systemctl restart zynthian\n'
printf -v INSTALL_CMD 'bash -c %q' "$REMOTE_SCRIPT"

echo "Sending $PLUGIN sources to $TARGET:$DEST (GitHub is not used)."
# No --delete: keep user files. Include factory samples and vendored DSP sources.
tar -C "$ROOT" --exclude='.git' --exclude='.claude' --exclude='.codex' \
    --exclude='.agents' --exclude='.aws' --exclude='AGENTS.md' --exclude='CLAUDE.md' \
    --exclude='.env' --exclude='.env.*' --exclude='deploy.env' \
    --exclude='build' --exclude='build_test' --exclude='build_obj' --exclude='dist' \
    --exclude='out' --exclude='__pycache__' --exclude='*.o' --exclude='*.so' \
    -czf - "${FILES[@]}" | ssh -- "$TARGET" "$COPY_CMD"
echo "Building and installing $PLUGIN; Zynthian will restart (audio stops)."
ssh -- "$TARGET" "$INSTALL_CMD"
echo "Done. Keep $DEST in place: installed plugins link to it."
