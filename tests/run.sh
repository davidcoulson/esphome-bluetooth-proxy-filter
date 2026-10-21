#!/usr/bin/env bash
# This fork's bluetooth_proxy.cpp cannot be compiled off-device, so it is tested
# by proxy: check that its filter is textually identical to
# esphome-ble-advert-filter's (tools/check_parity.py), then run THAT repo's
# suite, which compiles the real filter source under ASan/UBSan.
#
# This fork's filter matches esphome-ble-advert-filter v2.0.3, so parity is only
# meaningful against that exact commit. A newer or locally modified checkout
# could pass here without saying anything about the code this fork ships, so
# the script refuses one.
#
#   tests/run.sh                                  # clones v2.0.3
#   HOOK_REPO=/path/to/esphome-ble-advert-filter tests/run.sh   # must be at v2.0.3, clean
set -euo pipefail
cd "$(dirname "$0")/.."

PARITY_TAG=v2.0.3
PARITY_COMMIT=0bb63ba740c9488a5aaeaccbdc61c83caf6b0096

hook="${HOOK_REPO:-}"
if [[ -z "$hook" ]]; then
  hook=$(mktemp -d)/esphome-ble-advert-filter
  git clone -q --depth 1 --branch "$PARITY_TAG" https://github.com/davidcoulson/esphome-ble-advert-filter "$hook"
fi

head=$(git -C "$hook" rev-parse HEAD)
if [[ "$head" != "$PARITY_COMMIT" ]]; then
  echo "error: $hook is at $head, not $PARITY_TAG ($PARITY_COMMIT)." >&2
  echo "Check out $PARITY_TAG there, or unset HOOK_REPO to clone it." >&2
  exit 1
fi
if [[ -n "$(git -C "$hook" status --porcelain --untracked-files=no)" ]]; then
  echo "error: $hook has local changes; parity must be against $PARITY_TAG as released." >&2
  exit 1
fi

echo "== parity with $hook ($PARITY_TAG) =="
python3 tools/check_parity.py --hook "$hook"

echo
echo "== filter tests (real source, from the hook component at $PARITY_TAG) =="
"$hook/tests/run.sh"
