#!/usr/bin/env python3
"""Fail if this fork's filter has drifted from esphome-ble-advert-filter.

The same filter exists twice: here, inside a fork of bluetooth_proxy, and in
esphome-ble-advert-filter, which plugs into stock bluetooth_proxy through the
AdvertisementFilter hook that ships in ESPHome 2026.10. The plan is to retire
this fork for that component, which only works if the two behave identically.

This fork's bluetooth_proxy.cpp cannot be compiled off-device (it drags in the
whole API server), so it has no real tests of its own. The hook component does:
its suite compiles the real source under ASan. Textual parity is therefore what
carries those tests over to this fork - if every function below matches, the
fork's logic is the logic that was tested.

Compared, after stripping comments and whitespace and mapping the unavoidable
differences (class name, `raw`/`adv`, `return;`/`return false;`):
  - every filter helper function
  - the filter chain itself
  - the Espressif OUI table
  - the public filter API, so YAML lambdas are portable between the two

    python3 tools/check_parity.py --hook ../esphome-ble-advert-filter
"""

import argparse
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parent.parent
FORK_CPP = ROOT / "components" / "bluetooth_proxy" / "bluetooth_proxy.cpp"
FORK_H = ROOT / "components" / "bluetooth_proxy" / "bluetooth_proxy.h"

FUNCTIONS = [
    "is_espressif_oui_",
    "findmy_match_",
    "ibeacon_match_",
    "payload_blocked_",
    "uuid128_matches_",
    "payload_has_allowed_service_uuid_",
    "address_is_rpa_",
    "address_is_non_resolvable_",
    "set_irks",
    "recompute_gate_",
    "irk_matches_",
]

CHAIN_START = "  // 1. Ignored outright"
CHAIN_END = "    this->adv_forwarded_irk_++;\n"


def normalise(text: str) -> str:
    text = re.sub(r"//[^\n]*", "", text)
    text = re.sub(r"\bBluetoothProxy::|\bBLEAdvertFilter::", "CLS::", text)
    text = re.sub(r"\braw\.", "adv.", text)
    text = re.sub(r"\breturn;", "return false;", text)
    return re.sub(r"\s+", "", text)


def function_body(src: str, name: str, path: Path) -> str:
    m = re.search(
        rf"^[^\n;{{}}]*\b(?:BluetoothProxy|BLEAdvertFilter)::{re.escape(name)}\(",
        src,
        re.M,
    )
    if not m:
        # Reported as a difference rather than aborting, so one run lists
        # everything that is missing.
        return f"<{name} missing from {path.name}>"
    depth = 0
    i = src.index("{", m.start())
    for j in range(i, len(src)):
        if src[j] == "{":
            depth += 1
        elif src[j] == "}":
            depth -= 1
            if depth == 0:
                return src[m.start() : j + 1]
    raise SystemExit(f"check_parity: unbalanced braces in {name} ({path})")


def region(src: str, start: str, end: str, path: Path) -> str:
    i = src.find(start)
    j = src.find(end, i)
    if i < 0 or j < 0:
        raise SystemExit(f"check_parity: filter chain markers not found in {path}")
    return src[i : j + len(end)]


def oui_table(src: str, path: Path) -> str:
    m = re.search(r"ESPRESSIF_OUIS\[\]\s*=\s*\{(.*?)\};", src, re.S)
    if not m:
        raise SystemExit(f"check_parity: OUI table not found in {path}")
    return m.group(1)


def public_api(header: str) -> set[str]:
    """Filter-related public methods: the surface YAML lambdas call."""
    names = set(
        re.findall(
            r"^\s+(?:[\w:<>,\s&\*]+?)\b((?:set|get|add|clear)_(?:rssi|irk|adv|min_rssi|allow|ibeacon|findmy|"
            r"drop_non|blocked|allowed|allowlist)\w*)\(",
            header,
            re.M,
        )
    )
    return names


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--hook", required=True, help="path to an esphome-ble-advert-filter checkout"
    )
    args = ap.parse_args()
    hook_dir = Path(args.hook) / "components" / "ble_advert_filter"
    hook_cpp_path, hook_h_path = (
        hook_dir / "ble_advert_filter.cpp",
        hook_dir / "ble_advert_filter.h",
    )

    fork_cpp, hook_cpp = FORK_CPP.read_text(), hook_cpp_path.read_text()
    bad = []

    for name in FUNCTIONS:
        a = normalise(function_body(fork_cpp, name, FORK_CPP))
        b = normalise(function_body(hook_cpp, name, hook_cpp_path))
        print(f"  {'ok  ' if a == b else 'DIFF'}  {name}")
        if a != b:
            bad.append(name)

    a = normalise(region(fork_cpp, CHAIN_START, CHAIN_END, FORK_CPP))
    b = normalise(region(hook_cpp, CHAIN_START, CHAIN_END, hook_cpp_path))
    print(f"  {'ok  ' if a == b else 'DIFF'}  filter chain")
    if a != b:
        bad.append("filter chain")

    a, b = (
        normalise(oui_table(fork_cpp, FORK_CPP)),
        normalise(oui_table(hook_cpp, hook_cpp_path)),
    )
    print(f"  {'ok  ' if a == b else 'DIFF'}  Espressif OUI table")
    if a != b:
        bad.append("OUI table")

    fork_api, hook_api = (
        public_api(FORK_H.read_text()),
        public_api(hook_h_path.read_text()),
    )
    only_fork, only_hook = sorted(fork_api - hook_api), sorted(hook_api - fork_api)
    same = not only_fork and not only_hook
    print(
        f"  {'ok  ' if same else 'DIFF'}  public filter API ({len(fork_api & hook_api)} methods)"
    )
    if not same:
        bad.append("public API")
        for n in only_fork:
            print(f"          only in the fork: {n}")
        for n in only_hook:
            print(f"          only in the hook component: {n}")

    if bad:
        print(
            f"\ncheck_parity: DRIFT in {', '.join(bad)}.\n"
            "Port the change to the other repo so the two stay interchangeable.",
            file=sys.stderr,
        )
        return 1
    print("\ncheck_parity: the fork and the hook component implement the same filter")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
