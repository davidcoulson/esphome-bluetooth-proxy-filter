# esphome-bluetooth-proxy-filter

A drop-in fork of ESPHome's core `bluetooth_proxy` component that can **filter BLE
advertisements on the device**, before they are queued for the API and cross the network.

Core ESPHome's `bluetooth_proxy` accepts only `active`, `cache_services`,
`connection_slots` and `connections` — it has **no RSSI, MAC or name filtering of any
kind**, and `on_raw_advertisement_()` forwards every packet it receives. The
`mac_address` / `service_uuid` / `manufacturer_id` options on `esp32_ble_tracker` apply
only to its `on_ble_advertise` automation triggers and do **not** gate the proxy stream.

This fork adds the options below. All default to "off", so an unconfigured build behaves
exactly like upstream.

| Option | Effect |
| --- | --- |
| `rssi_threshold` | RSSI limit for everything not matched by a protection category (default `-127` = forward everything) |
| `irks` | Drop Resolvable Private Addresses that resolve to none of the listed Identity Resolving Keys — i.e. other people's phones and watches |
| `mac_allowlist` | Always forward these addresses, bypassing the per-category RSSI limits below |
| `rssi_floor` | Absolute reception limit applied to **every** advertisement, allowlisted ones included, before anything is categorised (default `-127` = off) |
| `rssi_mac_allowlist` | RSSI limit for `mac_allowlist` hits (default `-127` = bounded only by `rssi_floor`) |
| `rssi_irk` | RSSI limit for IRK-matched devices (default `-127` = inherit `rssi_threshold`) |
| `rssi_service_uuid` | RSSI limit for `service_uuid_allowlist` hits (default `-127` = inherit `rssi_threshold`) |
| `manufacturer_blocklist` | Drop advertisements carrying these Bluetooth SIG company identifiers (AD type `0xFF`) |
| `name_blocklist` | Drop advertisements whose local name contains one of these substrings |
| `drop_non_resolvable` | Drop non-resolvable private addresses — they rotate but carry no identity, so an IRK cannot resolve them and they can never be tracked (default `false`) |
| `allow_espressif` | Exempt Espressif-OUI addresses from the IRK test (default `true`) |
| `allow_ibeacon` | Exempt iBeacons from `manufacturer_blocklist` — `true` for all, or a list of `major`/`minor`/`rssi` filters (default `false`) |

It also exposes advertisement counters (`get_adv_forwarded()`, `get_adv_dropped()`,
`get_adv_dropped_rpa()`) so the effect is measurable per-proxy rather than guessed.

## Usage

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/davidcoulson/esphome-bluetooth-proxy-filter
      ref: main
    components: [bluetooth_proxy]

bluetooth_proxy:
  id: ble_proxy
  active: true
  rssi_threshold: -75
  rssi_floor: -90          # bounds mac_allowlist too
  allow_espressif: true
  mac_allowlist:
    - AA:BB:CC:DD:EE:FF      # a beacon that must always be forwarded
  manufacturer_blocklist:
    - 0x004C                 # Apple - AirPods/AirTags/neighbours' devices
  irks:
    - !secret ble_irk_my_phone   # ...but NOT our own Apple devices
```

## Filter order

An advertisement is **categorised first**, then measured against that category's own
RSSI limit. This is what makes the limits independent: any category can be looser or
stricter than any other.

1. `mac_blocklist` hit → drop, ahead of every allow rule
2. RSSI below `rssi_floor` → drop. Global, applies to allowlisted devices too
3. Categorise, first hit wins, cheapest test first:
   `mac_allowlist` → `MAC` · RPA resolving to an IRK → `IRK`
   (an RPA resolving to none → drop) · allowlisted service UUID → `UUID`
   · anything else → `DEFAULT`
4. RSSI below **that category's** limit → drop
5. `allowlist_exclusive` and `DEFAULT` → drop
6. Non-resolvable private address (with `drop_non_resolvable`), unprotected → drop
7. Blocklisted manufacturer id or local name, unprotected → drop
8. Otherwise forward

### Why a floor as well as a threshold

`mac_allowlist` deliberately exempts tracked beacons from `rssi_threshold`, because a
weak reading at one proxy is exactly what places the tag nearer another. But that
exemption used to be unbounded: a tag heard at the receiver's noise limit was still
forwarded, and an RSSI that far down carries no usable distance information. It does
not help a tracker triangulate, it misleads it. `rssi_floor` bounds the exemption
without removing it.

### Ordering caveat (changed in v1.1.0)

Before v1.1.0 the single `rssi_threshold` ran partway down the chain, so only
`mac_allowlist` could be given a looser limit — IRKs and service UUIDs were resolved
*after* the threshold had already dropped the advertisement, and their limits could
only tighten. Categorising first fixes that, at the cost of running IRK resolution and
the service-UUID payload walk for advertisements between `rssi_floor` and
`rssi_threshold` that the old order discarded earlier. `rssi_floor` is the mitigation:
cheapest test, runs first, applies to everything.

An unset (`-127`) category limit **inherits**, chosen so a config that sets none of
them behaves exactly as it did before v1.1.0: `mac_allowlist` is bounded only by
`rssi_floor`; `irk` and `service_uuid` inherit `rssi_threshold`.

Both address-class tests are guarded on `addr_type` as well as the address bits.
That guard is load-bearing: real public OUIs exist both in the RPA bit range
(Espressif's `4C:…`) and the non-resolvable range (`00:`, `04:`, `15:…`), and
bit-matching alone would discard them.

**Why the categories mark the advertisement "protected":** a device matched by an IRK
is one of yours, and your phones and watches advertise Apple manufacturer data.
Without the protected flag, a `manufacturer_blocklist: [0x004C]` entry would discard
exactly the devices the IRK list exists to keep. The name and manufacturer checks
share a single pass over the payload.

## Exempting iBeacons

iBeacon is Apple manufacturer data with subtype `0x02`, so a
`manufacturer_blocklist: [0x004C]` entry — the usual way to kill AirPods, AirTag
and neighbour noise — silently takes **every iBeacon** with it. `allow_homekit`
does not help: that exempts subtype `0x06`.

This matters if you run [BPS](https://github.com/Hogster/BPS) receiver
auto-calibration, where each ESPHome probe advertises an iBeacon so its siblings
can range it. Those adverts come from the probe's public Espressif MAC, so they
carry no IRK and nothing else rescues them.

```yaml
bluetooth_proxy:
  manufacturer_blocklist: [0x004C]
  allow_ibeacon: true          # every iBeacon exempt
```

Scope it to your own beacons instead of opening the door to every iBeacon in
radio range:

```yaml
  allow_ibeacon:
    - major: 1
      minor: 7                 # one probe
    - major: 10
      minor: [3, 4, 5]         # several
    - major: 11                # whole major, any minor
```

Each filter can carry its own `rssi`, which **overrides both `rssi_threshold`
and `rssi_floor`** for adverts it matches:

```yaml
  allow_ibeacon:
    - major: 1
      rssi: -127               # our probes: forward at any strength
    - major: 10
      rssi: -85
```

That is the point of the feature. Probe-to-probe ranging wants exactly the weak
cross-room readings the fleet threshold exists to discard — and only for the
beacons doing the ranging. Everything else stays bounded.

**Omitting `rssi` inherits**: the filter exempts the advert from
`manufacturer_blocklist` and nothing else, so `rssi_threshold` and `rssi_floor`
still apply. Omitting a value is never the most permissive setting. A bare
`allow_ibeacon: true` behaves the same way.

Note `rssi: -127` disables the internal pre-gate (see below), so prefer a real
value like `-95` unless you genuinely want everything.

### The pre-gate

Every advert is measured against *some* limit, so anything weaker than the most
permissive limit in the config is dropped before categorisation runs. That keeps
an AES resolve and a payload walk off every distant advert in the neighbourhood
once your own beacons are allowed through at a very low RSSI. It is computed
automatically from `rssi_threshold`, `rssi_floor` and every per-category limit;
a `-127` anywhere disables it, correctly — if something is allowed through at
any strength, nothing can be rejected on RSSI alone.

`minor` is nested under `major` because that is the BLE data model — a minor is
only meaningful inside a major — and flat keys cannot express "major 1 minor 7
**and** major 10 minor 3".

The filters **narrow** the exemption; they never add a drop rule. An iBeacon
matching none of them falls through to the normal manufacturer test, exactly as
if the exemption were off. A truncated iBeacon carrying no major/minor cannot be
matched, so it is not exempted when filters are in use (bare `true` still exempts
it).

Note this exempts iBeacons from the **payload** filters only. The RSSI limits
still apply, so distant probe pairs can still be dropped — relevant for
calibration, which wants the full pair matrix including weak through-wall links.

## Measured results

On a ~30-proxy Home Assistant install:

- RSSI filter at `-85` alone: **~38% fewer bytes** on the wire (control-normalised)
- Adding IRK filtering: **56–66% of all advertisements dropped**
- Unresolved RPAs were a consistent **20–27%** of everything each proxy heard

Packet count falls much less than byte count (~14% vs ~38%), because the proxy batches
up to 16 advertisements per packet — dropping advertisements mostly makes batches
emptier rather than removing packets.

## Caveats

- **Home Assistant discovers BLE devices by hearing them advertise.** A device below the
  RSSI threshold at *every* proxy becomes invisible to HA entirely, not just to your
  tracker, and cannot be connected to.
- **Re-sync this fork on every ESPHome version bump.** It pins a copy of a fast-moving
  core component. See `components/bluetooth_proxy/README.md` for the procedure and the
  exact diff against upstream.

## Licence

ESPHome is dual-licensed MIT / GPLv3; this fork carries the upstream licence of the
files it derives from.
