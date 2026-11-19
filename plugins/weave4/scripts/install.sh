#!/bin/bash
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
make -C "$ROOT" install
echo "Installed. Restart Zynthian: systemctl restart zynthian"
