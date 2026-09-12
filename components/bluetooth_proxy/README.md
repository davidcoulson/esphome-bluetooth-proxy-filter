# bluetooth_proxy (local fork)

Fork of the **core** ESPHome `bluetooth_proxy` component, taken verbatim from
tag **2026.8.2**, with one feature added: **`rssi_threshold`**.

It is loaded via `external_components` (`type: local`, `path: components`) from
`common/ble-proxy.yaml` and `common/ble.yaml`. ESPHome's loader inserts external
components at `sys.meta_path[0]`, so this directory **shadows the built-in
component of the same name** for every device that includes those packages.

## Why this exists

Core `bluetooth_proxy` accepts only `active`, `cache_services`,
`connection_slots` and `connections`. It has **no MAC allowlist and no RSSI
filter**, and `BluetoothProxy::on_raw_advertisement_()` forwards every packet it
receives unconditionally. The `mac_address` / `service_uuid` / `manufacturer_id`
filters in `esp32_ble_tracker` apply only to the `on_ble_advertise` automation
triggers and do **not** gate the proxy stream.

`on_raw_advertisement_()` is the only point at which a packet can be suppressed
before it is queued for the API and crosses the network, so the filter lives
there.

Note that `api: batch_delay` does **not** affect this traffic: the advertisement
flush calls `api_connection_->send_message()` directly, bypassing the deferred
batch queue that `batch_delay` governs. Flush cadence is hardcoded (a 16-packet
batch, or the proxy's own ~200ms Wi-Fi / 100ms Ethernet loop gate).

## The change

Total diff vs upstream: **24 added lines, 0 deletions, 0 modifications.**

| File | Change |
| --- | --- |
| `__init__.py` | `CONF_RSSI_THRESHOLD`; optional key in `_COMMON_SCHEMA_KEYS` and the outer walkable schema; `set_rssi_threshold()` codegen in both `to_code` arms |
| `bluetooth_proxy.h` | `int8_t rssi_threshold_{-127}` member + `set_rssi_threshold()` / `get_rssi_threshold()`; `adv_forwarded_` / `adv_dropped_` counters + getters |
| `bluetooth_proxy.cpp` | Early return in `on_raw_advertisement_()` when `raw.rssi < rssi_threshold_`; bumps the two counters |

### Advertisement counters

`get_adv_forwarded()` / `get_adv_dropped()` are free-running `uint32_t` totals
that are never reset — unsigned wraparound is well defined, so a consumer taking
deltas stays correct across the rollover.

Both packages expose three diagnostic template sensors built on them:
**BLE Adverts Forwarded** and **BLE Adverts Dropped** (delta per minute, so they
graph as a rate) and **BLE Advert Drop Rate** (%), which is the one to tune
against. The drop-rate sensor returns `NAN` rather than `0` when the proxy heard
nothing at all in the interval, so an idle proxy is not misreported as 0%.

Default is `-127`, which forwards everything — i.e. **identical to upstream
unless configured.**

## `service_uuid_allowlist` — pairing/commissioning passthrough

> **Note:** the change table above documents only the original RSSI work and predates the IRK,
> `drop_non_resolvable`, `manufacturer_blocklist` and `mac_allowlist` filters. Treat the source as
> authoritative; `on_raw_advertisement_()` has the filter chain in order.

16-bit service UUIDs that bypass **every** filter, including the address-type tests:

```yaml
bluetooth_proxy:
  service_uuid_allowlist:
    - 0xFFF6   # Matter commissioning
```

**Why it cannot be done with `mac_allowlist`.** A device in pairing mode advertises from a rotating
private address, so `drop_non_resolvable: true` or the unresolved-RPA test discards it before
anything can identify it — and its address is not knowable in advance, so no MAC could be
allowlisted. Matching on the service UUID is the only handle that exists at that point.

**Placement.** The check runs after `mac_allowlist` and **before** the RSSI test, so a pairing device
is forwarded no matter how weakly it is heard. That is deliberate: a pairing window is short and
user-initiated, so a missed advertisement costs a retry while the extra traffic lasts only as long
as the pairing does.

**Cost.** It walks the advertisement payload, which the other filters defer to last. Guarded on a
non-empty list, so a build that does not set the option keeps the original ordering and pays
nothing.

**Matched AD types** — all four, because a device in pairing mode does not consistently use one:
`0x02`/`0x03` (incomplete/complete 16-bit UUID list), `0x14` (16-bit solicitation), `0x16` (service
data, 16-bit UUID). 128-bit UUIDs are not matched: the transient pairing services this targets are
SIG-allocated shorts, and a vendor 128-bit UUID is device-specific so allowlisting one would not
generalise.

**Verifying it fired.** `get_adv_allowed_service_uuid()` counts advertisements forwarded *only*
because of this list. It sits at zero while nothing is pairing, so any movement is direct evidence
the passthrough did the work — which is what makes a failed commissioning attempt diagnosable
instead of guesswork.

## Runtime tuning

The YAML `rssi_threshold:` key is a compile-time fallback only. Both packages
also define a `BLE RSSI Threshold` template number that calls
`set_rssi_threshold()` so the value can be changed from Home Assistant without
reflashing ~18 proxies.

That number uses **`on_value`, not `set_action`**, deliberately:
`TemplateNumber::setup()` restores a saved value by calling `publish_state()`
and never `control()`, so `set_action` would not fire on boot — the restored
value would show in the UI while the proxy silently kept the YAML default.
`on_value` fires on both boot-restore and user changes.

Ordering is safe: ESPHome emits all `cg.add()` configuration statements into
`main.cpp` before `App.setup()` runs, so the YAML default is applied first and
the restored number then overwrites it.

## Re-syncing on an ESPHome upgrade

**This fork pins a copy of a fast-moving core component. Re-sync it on every
ESPHome version bump**, or devices will build against stale proxy code.

```bash
V=<new-esphome-version>
for f in __init__.py bluetooth_proxy.cpp bluetooth_proxy.h; do
  curl -sL "https://raw.githubusercontent.com/esphome/esphome/$V/esphome/components/bluetooth_proxy/$f" -o "upstream-$f"
done
# Re-apply the three changes in the table above, then update this README's tag.
```

If upstream ever gains a native RSSI/MAC filter, **delete this directory** and
the `external_components` blocks in `common/ble-proxy.yaml` and
`common/ble.yaml`.

## Tuning guidance

Bermuda has no RSSI cutoff of its own (only `max_area_radius`, applied in HA
after the packet has already crossed the network), so this threshold is the
only thing that reduces actual traffic.

With Bermuda's configured `ref_power: -55` and `attenuation: 3`, the mapping is
`distance = 10 ^ ((-55 - rssi) / 30)`:

| Threshold | Approx. distance |
| --- | --- |
| `-75 dBm` | ~4.6 m |
| `-85 dBm` | ~10 m |
| `-90 dBm` | ~14 m |
| `-94 dBm` | ~20 m (Bermuda's `max_area_radius`) |

`-75` is far more aggressive than the 20 m radius Bermuda is configured for and
would blind it well inside that radius. Default here is **-85** (~10 m); adjust
from the number entity while watching the `sensor.*_area_stable` entities.

**Trade-off:** Home Assistant discovers a BLE device by hearing it advertise. A
device below the threshold at *every* proxy becomes invisible to HA entirely —
not just to Bermuda — and cannot be connected to.
