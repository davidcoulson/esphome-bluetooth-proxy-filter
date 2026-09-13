"""Tests for the pre-gate computation.

Getting this wrong is silent - the gate stops rejecting anything and the only
symptom is a busier radio - so it is worth pinning. Every case below is a
regression: shipping v1.3.0 with the gate disabled for ordinary configs is
exactly what these would have caught.

    python3 tests/test_effective_gate.py
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "components" / "bluetooth_proxy"))

# Import the pure function without dragging in esphome's codegen machinery.
import ast
import types

_SRC = (Path(__file__).resolve().parent.parent / "components" / "bluetooth_proxy" / "__init__.py").read_text()
_tree = ast.parse(_SRC)
_ns: dict = {}
for _node in _tree.body:
    if isinstance(_node, ast.Assign) and getattr(_node.targets[0], "id", "") == "_IBEACON_RSSI_INHERIT":
        exec(compile(ast.Module([_node], []), "<cfg>", "exec"), _ns)
    if isinstance(_node, ast.FunctionDef) and _node.name == "effective_gate":
        exec(compile(ast.Module([_node], []), "<cfg>", "exec"), _ns)
effective_gate = _ns["effective_gate"]
INHERIT = _ns["_IBEACON_RSSI_INHERIT"]

OFF = -127
failures = 0


def check(actual, expected, what):
    global failures
    if actual != expected:
        print(f"  FAIL  {what}: expected {expected}, got {actual}")
        failures += 1
    else:
        print(f"  ok    {what}")


print("effective_gate")

# The real fleet config, and the case v1.3.0 got wrong: no category keys set,
# so all three defaulted to -127 and disabled the gate.
check(effective_gate(-75, -90, OFF, OFF, OFF, [-95]), -95,
      "fleet config (thr -75, floor -90, one iBeacon -95) gates at -95")

check(effective_gate(-75, -90, OFF, OFF, OFF, []), -90,
      "no iBeacon rules: the floor is the loosest bound")

# Deliberately -127, and worth stating plainly: with no floor, an allowlisted
# MAC is bounded by nothing at all, so it can arrive at any strength and the
# gate cannot reject anything. Conservative rather than clever - the gate is
# only an optimisation, and disabling it costs CPU, never correctness. (It
# stays -127 even when mac_allowlist is empty and the category can therefore
# never occur; tightening that would need the list length plumbed in here.)
check(effective_gate(-75, OFF, OFF, OFF, OFF, []), -127,
      "no floor: an allowlisted MAC is unbounded, so there is no gate")

check(effective_gate(-75, -90, OFF, OFF, OFF, [-127]), -127,
      "an iBeacon rule at -127 genuinely disables the gate")

check(effective_gate(-75, -90, OFF, OFF, OFF, [INHERIT]), -90,
      "an inheriting iBeacon rule contributes nothing new")

check(effective_gate(-75, -90, -95, OFF, OFF, []), -95,
      "an explicit mac_allowlist limit widens the gate")

check(effective_gate(-75, -90, OFF, -85, OFF, []), -90,
      "an irk limit stricter than the floor does not widen it")

check(effective_gate(-75, -90, OFF, -95, OFF, []), -95,
      "an irk limit looser than the floor does widen it")

check(effective_gate(OFF, OFF, OFF, OFF, OFF, []), -127,
      "a fully unconfigured proxy has no gate")

print("\n" + ("FAILED" if failures else "all passed"))
sys.exit(1 if failures else 0)
