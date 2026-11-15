#!/bin/bash
# Compatibility entry point: local SSH deployment, no GitHub or deploy.env.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
exec bash "$ROOT/scripts/deploy-zynthian.sh" tape "$@"
