#!/bin/bash
# Run from WSL/Linux; build and install on Zynthian using the existing installer.
set -euo pipefail

usage() {
    echo "Usage: bash scripts/deploy-zynthian.sh [user@host] [absolute-remote-directory]"
    echo "Defaults: root@zynthian.local /root/zynthian-eka-grain-flow"
    echo "Copies local sources, installs on Zynthian and restarts its interface (audio stops)."
}
if [[ "${1:-}" == --help || "${1:-}" == -h ]]; then
    usage
    exit 0
fi
if (( $# > 2 )); then
    usage >&2
    exit 1
fi
TARGET="${1:-root@zynthian.local}"
DEST="${2:-/root/zynthian-eka-grain-flow}"
if [[ "$TARGET" == -* || "$DEST" != /* || "$DEST" == / ]]; then
    echo "Specify an SSH destination and an absolute project directory (not /)." >&2
    exit 1
fi
for tool in ssh tar; do
    command -v "$tool" >/dev/null || { echo "Missing command: $tool" >&2; exit 1; }
done
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# Quote the directory for the remote bash; do not interpolate it as shell code.
printf -v REMOTE_SCRIPT 'set -euo pipefail\ncd -- %q\nbash scripts/install.sh\nsystemctl restart zynthian\n' "$DEST"
printf -v INSTALL_CMD 'bash -c %q' "$REMOTE_SCRIPT"
printf -v REMOTE_SCRIPT 'set -euo pipefail\nif [ "$EUID" -ne 0 ]; then echo "Connect as root to install on Zynthian." >&2; exit 1; fi\nif [ ! -f /zynthian/zynthian-ui/zyngine/zynthian_lv2.py ]; then echo "Zynthian UI not found on this host." >&2; exit 1; fi\nmkdir -p -- %q\ntar -xzf - -C %q\n' "$DEST" "$DEST"
printf -v COPY_CMD 'bash -c %q' "$REMOTE_SCRIPT"

echo "Sending sources to $TARGET:$DEST (GitHub is not used)."
# Exclude machine-specific binaries, Git history and local credentials/settings.
# Existing remote files are kept; the installer rebuilds the plugin on the Pi.
tar -C "$ROOT" --exclude='./.git' --exclude='./build' --exclude='./build_test' \
    --exclude='./dist' --exclude='./.codex' --exclude='./.agents' --exclude='./.aws' \
    --exclude='./.env' --exclude='./.env.*' --exclude='__pycache__' \
    --exclude='*.o' --exclude='*.so' -czf - . | ssh -- "$TARGET" "$COPY_CMD"

echo "Installing and restarting Zynthian; audio will stop."
ssh -- "$TARGET" "$INSTALL_CMD"
echo "Done. Eka Grain Flow is installed. Keep $DEST in place: the plugin links to it."
