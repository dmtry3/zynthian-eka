#!/bin/bash
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
make -C "$ROOT" uninstall
echo "Removed."
