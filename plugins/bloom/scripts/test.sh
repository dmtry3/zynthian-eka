#!/bin/bash
# Builds and runs the arp_core tests (no LV2 needed).
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$ROOT/build_test"
gcc -O2 -Wall -Wextra -o "$ROOT/build_test/test_arp" "$ROOT/test/test_arp.c" "$ROOT/src/arp_core.c" -lm
"$ROOT/build_test/test_arp"
