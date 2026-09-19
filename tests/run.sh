#!/usr/bin/env bash
# Everything that can be checked without an ESP32 or an ESPHome build.
set -euo pipefail
cd "$(dirname "$0")/.."

echo "== drift guard =="
python3 tools/check_test_sync.py

echo
echo "== filter logic (host-compiled) =="
out=$(mktemp -d)/t
c++ -std=c++17 -Wall -Wextra -Werror -o "$out" tests/test_ibeacon_match.cpp
"$out"

echo
echo "== runtime IRK loader (host-compiled) =="
c++ -std=c++17 -Wall -Wextra -Werror -o "$out" tests/test_set_irks.cpp
"$out"

echo
echo "== pre-gate computation =="
python3 tests/test_effective_gate.py
