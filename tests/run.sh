#!/usr/bin/env bash
# This fork's bluetooth_proxy.cpp cannot be compiled off-device, so it is tested
# by proxy: check that its filter is textually identical to
# esphome-ble-advert-filter's (tools/check_parity.py), then run THAT repo's
# suite, which compiles the real filter source under ASan/UBSan.
#
#   tests/run.sh                                  # sibling checkout, else a fresh clone
#   HOOK_REPO=/path/to/esphome-ble-advert-filter tests/run.sh
set -euo pipefail
cd "$(dirname "$0")/.."

hook="${HOOK_REPO:-}"
if [[ -z "$hook" && -d ../esphome-ble-advert-filter/components ]]; then
  hook=../esphome-ble-advert-filter
fi
if [[ -z "$hook" ]]; then
  hook=$(mktemp -d)/esphome-ble-advert-filter
  git clone -q --depth 1 https://github.com/davidcoulson/esphome-ble-advert-filter "$hook"
fi

echo "== parity with $hook =="
python3 tools/check_parity.py --hook "$hook"

echo
echo "== filter tests (real source, from the hook component) =="
"$hook/tests/run.sh"
