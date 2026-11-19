#!/bin/bash
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
make -C "$ROOT"
echo "Built: $ROOT/build/eka-weave4.lv2"
