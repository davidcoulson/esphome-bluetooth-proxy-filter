# esphome-bluetooth-proxy-filter

A drop-in fork of ESPHome's core `bluetooth_proxy` component that can **filter BLE
advertisements on the device**, before they are queued for the API and cross the network.

Core ESPHome's `bluetooth_proxy` accepts only `active`, `cache_services`,
`connection_slots` and `connections` — it has **no RSSI, MAC or name filtering of any
kind**, and `on_raw_advertisement_()` forwards every packet it receives. The
`mac_address` / `service_uuid` / `manufacturer_id` options on `esp32_ble_tracker` apply
only to its `on_ble_advertise` automation triggers and do **not** gate the proxy stream.

This fork adds four options. All default to "off", so an unconfigured build behaves
exactly like upstream.

| Option | Effect |
| --- | --- |
| `rssi_threshold` | Drop advertisements weaker than N dBm (default `-127` = forward everything) |
| `irks` | Drop Resolvable Private Addresses that resolve to none of the listed Identity Resolving Keys — i.e. other people's phones and watches |
| `mac_allowlist` | Always forward these addresses, bypassing every filter including RSSI |
| `manufacturer_blocklist` | Drop advertisements carrying these Bluetooth SIG company identifiers (AD type `0xFF`) |
| `name_blocklist` | Drop advertisements whose local name contains one of these substrings |
| `drop_non_resolvable` | Drop non-resolvable private addresses — they rotate but carry no identity, so an IRK cannot resolve them and they can never be tracked (default `false`) |
| `allow_espressif` | Exempt Espressif-OUI addresses from the IRK test (default `true`) |

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
  rssi_threshold: -85
  allow_espressif: true
  mac_allowlist:
    - AA:BB:CC:DD:EE:FF      # a beacon that must always be forwarded
  manufacturer_blocklist:
    - 0x004C                 # Apple - AirPods/AirTags/neighbours' devices
  irks:
    - !secret ble_irk_my_phone   # ...but NOT our own Apple devices
```

## Filter order

Cheapest test first, so an advertisement that will be dropped never reaches the AES:

1. `mac_allowlist` hit → **protected**, forward unconditionally
2. RSSI below `rssi_threshold` → drop
3. Non-resolvable private address (with `drop_non_resolvable`) → drop
4. RPA: resolves to a configured IRK → **protected**; resolves to none → drop
5. Not protected, and the payload carries a blocklisted manufacturer id or local
   name → drop
6. Otherwise forward

Both address-class tests are guarded on `addr_type` as well as the address bits.
That guard is load-bearing: real public OUIs exist both in the RPA bit range
(Espressif's `4C:…`) and the non-resolvable range (`00:`, `04:`, `15:…`), and
bit-matching alone would discard them.

**Why steps 1 and 3 mark the advertisement "protected" rather than just letting it
through:** a device matched by an IRK is one of yours, and your phones and watches
advertise Apple manufacturer data. Without the protected flag, a
`manufacturer_blocklist: [0x004C]` entry would pass them at step 3 and then discard
them at step 4 — silently filtering out exactly the devices the IRK list exists to
keep. The name and manufacturer checks share a single pass over the payload.

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
