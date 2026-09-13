#pragma once

#include "esphome/core/defines.h"

#ifdef USE_BLUETOOTH_PROXY

#include <array>
#include <vector>

#include "esphome/components/api/api_connection.h"
#include "esphome/components/api/api_pb2.h"
#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

#include "esphome/components/bluetooth_connection/bluetooth_connection.h"

#include "esphome/components/ble_device_base/ble_hub_impl.h"

#include "esphome/components/bluetooth_connection/bluetooth_connection_hub.h"

namespace esphome::bluetooth_proxy {

// The connection-domain types live in the bluetooth_connection component;
// re-exported here so the proxy code reads unqualified.
using bluetooth_connection::CONN_OK;
using bluetooth_connection::conn_err_t;
using bluetooth_connection::GATT_NOT_CONNECTED;
using bluetooth_connection::DONE_SENDING_SERVICES;
using bluetooth_connection::INIT_SENDING_SERVICES;
using bluetooth_connection::SERVICES_DONE_PENDING;

#ifdef USE_BLUETOOTH_PROXY_CONNECTIONS
using BluetoothConnection = bluetooth_connection::BluetoothConnection;
using ClientState = ble_device_base::ClientState;
#endif

// Legacy versions:
// Version 1: Initial version without active connections
// Version 2: Support for active connections
// Version 3: New connection API
// Version 4: Pairing support
// Version 5: Cache clear support
static constexpr uint32_t LEGACY_ACTIVE_CONNECTIONS_VERSION = 5;
static constexpr uint32_t LEGACY_ACTIVE_NO_CACHE_CLEAR_VERSION = 4;
static constexpr uint32_t LEGACY_ACTIVE_NO_PAIRING_VERSION = 3;
static constexpr uint32_t LEGACY_PASSIVE_ONLY_VERSION = 1;

enum BluetoothProxyFeature : uint32_t {
  FEATURE_PASSIVE_SCAN = 1 << 0,
  FEATURE_ACTIVE_CONNECTIONS = 1 << 1,
  FEATURE_REMOTE_CACHING = 1 << 2,
  FEATURE_PAIRING = 1 << 3,
  FEATURE_CACHE_CLEARING = 1 << 4,
  FEATURE_RAW_ADVERTISEMENTS = 1 << 5,
  FEATURE_STATE_AND_MODE = 1 << 6,
  FEATURE_CONNECTION_PARAMS_SETTING = 1 << 7,
};

enum BluetoothProxySubscriptionFlag : uint32_t {
  SUBSCRIPTION_RAW_ADVERTISEMENTS = 1 << 0,
};

#ifdef USE_BLUETOOTH_PROXY_CONNECTIONS
/// One owed address-keyed reply in a single word: 48-bit address low, 16-bit
/// error on top. Every error that reaches it fits int16_t.
class PendingReply {
 public:
  constexpr void set(uint64_t address, conn_err_t error) {
    // Mask: the address originates from the client, and a stray high bit
    // must not corrupt the reason.
    this->word_ = (address & ADDRESS_MASK) | (static_cast<uint64_t>(static_cast<uint16_t>(error)) << 48);
  }
  constexpr void clear() { this->word_ = 0; }
  // Whole-word test: only (address 0, error 0) reads back as nothing owed.
  // A zero-address failure still latches, which is correct - that reply is
  // owed too. Neither backend can unpair address 0 successfully.
  constexpr bool empty() const { return this->word_ == 0; }
  // Masked like set(), so a stray high bit cannot defeat the pool lookups.
  constexpr bool matches(uint64_t address) const { return this->address() == (address & ADDRESS_MASK); }
  constexpr uint64_t address() const { return this->word_ & ADDRESS_MASK; }
  constexpr conn_err_t error() const { return static_cast<int16_t>(this->word_ >> 48); }

 private:
  static constexpr uint64_t ADDRESS_MASK = 0x0000FFFFFFFFFFFFULL;
  uint64_t word_{0};
};
// Pin the packing at compile time: mask and sign round-trip for every
// reachable shape (negative, GATT status, ESP_ERR_* range, stray high bit).
constexpr bool pending_reply_round_trips(uint64_t address, uint64_t expected_address, conn_err_t error) {
  PendingReply p;
  p.set(address, error);
  return p.address() == expected_address && p.error() == error && !p.empty() && p.matches(address);
}
static_assert(pending_reply_round_trips(0x0000112233445566ULL, 0x0000112233445566ULL, -1));
static_assert(pending_reply_round_trips(0x0000FFFFFFFFFFFFULL, 0x0000FFFFFFFFFFFFULL, 0x8F));
static_assert(pending_reply_round_trips(0xABCD112233445566ULL, 0x0000112233445566ULL, 0x110));
static_assert(PendingReply{}.empty());
#endif

class BluetoothProxy final : public Component {
#ifdef USE_BLUETOOTH_PROXY_CONNECTIONS
  // Allow the connection to update connections_free_response_
  friend bluetooth_connection::BluetoothConnection;
#endif
 public:
  BluetoothProxy();
  void set_ble_hub(ble_device_base::BLEHub *hub) { this->hub_ = hub; }
  void dump_config() override;
  void setup() override;
  void loop() override;

#ifdef USE_BLUETOOTH_PROXY_CONNECTIONS
  void register_connection(BluetoothConnection *connection);
#endif  // USE_BLUETOOTH_PROXY_CONNECTIONS
#ifndef USE_ESP32
  // Run after the hub's setup() (the trackers use AFTER_WIFI): setup() below
  // snapshots scan_active()/scan_running() and installs the raw callback, and
  // the BLEHub contract does not promise those are settled any earlier than
  // the hub's own setup().
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI - 1.0f; }
#endif  // !USE_ESP32

#ifdef USE_BLUETOOTH_PROXY_CONNECTIONS
  void bluetooth_device_request(const api::BluetoothDeviceRequest &msg);
  void bluetooth_gatt_read(const api::BluetoothGATTReadRequest &msg);
  void bluetooth_gatt_write(const api::BluetoothGATTWriteRequest &msg);
  void bluetooth_gatt_read_descriptor(const api::BluetoothGATTReadDescriptorRequest &msg);
  void bluetooth_gatt_write_descriptor(const api::BluetoothGATTWriteDescriptorRequest &msg);
  void bluetooth_gatt_send_services(const api::BluetoothGATTGetServicesRequest &msg);
  void bluetooth_gatt_notify(const api::BluetoothGATTNotifyRequest &msg);
  void bluetooth_set_connection_params(const api::BluetoothSetConnectionParamsRequest &msg);
#endif

  void subscribe_api_connection(api::APIConnection *api_connection, uint32_t flags);
  void unsubscribe_api_connection(api::APIConnection *api_connection);
  api::APIConnection *get_api_connection() { return this->api_connection_; }
  /// Whether the subscribed API client understands 16/32-bit UUID fields.
  bool client_supports_efficient_uuids() const {
    return this->api_connection_ != nullptr && this->api_connection_->client_supports_api_version(1, 12);
  }

#ifdef USE_BLUETOOTH_PROXY_CONNECTIONS
  /// False only when a subscriber refused the frame; true = delivered or
  /// nobody subscribed. Refusals latch in send_device_disconnected_() and
  /// send_connected_reply_(); other callers report via log_reply_dropped_().
  bool send_device_connection(uint64_t address, bool connected, uint16_t mtu = 0, conn_err_t error = CONN_OK);
  void send_connections_free();
  void send_connections_free(api::APIConnection *api_connection);
  /// Same convention as send_device_connection: false only on a refused frame.
  bool send_gatt_services_done(uint64_t address);
  /// False only when the API refused the frame, so the reply is still owed.
  bool send_gatt_error(uint64_t address, uint16_t handle, conn_err_t error);
  void send_device_pairing(uint64_t address, bool paired, conn_err_t error = CONN_OK);
  /// No default error: the drain rebuilds success as (error == CONN_OK), so a
  /// caller that omitted it would have a reported failure resent as a success.
  void send_device_unpairing(uint64_t address, bool success, conn_err_t error);
  void send_device_clear_cache(uint64_t address, bool success, conn_err_t error = CONN_OK);
#endif

  void bluetooth_scanner_set_mode(bool active);

  void set_active(bool active) { this->active_ = active; }
  bool has_active() { return this->active_; }

  /// The limit for the DEFAULT category - everything not matched by the MAC
  /// allowlist, an IRK, or an allowlisted service UUID. Also what the irk and
  /// service_uuid categories inherit when their own limit is unset.
  /// Advertisements weaker than this are dropped before they are queued for the
  /// API, so they never reach the network. -127 (the default) forwards
  /// everything, matching upstream behaviour.
  void set_rssi_threshold(int8_t rssi) { this->rssi_threshold_ = rssi; }
  int8_t get_rssi_threshold() const { return this->rssi_threshold_; }

  /// Absolute reception floor, applied to EVERY advertisement including ones
  /// mac_allowlist / service_uuid_allowlist would otherwise protect. Where
  /// rssi_threshold answers "is this close enough to be interesting", this
  /// answers "is this reading usable at all" - below it the RSSI is dominated
  /// by noise and a tracker would only be misled by it.
  ///
  /// Ordering matters: this runs BEFORE the allowlist bypasses are evaluated,
  /// which is the whole point - an allowlisted tag heard at -100 dBm is still
  /// dropped. -127 (the default) disables the floor entirely, matching the
  /// behaviour of every build before this option existed.
  ///
  /// NOTE: the floor also caps the useful range of rssi_threshold. Setting a
  /// threshold weaker (more negative) than the floor has no effect, because
  /// the floor has already discarded those advertisements.
  void set_rssi_floor(int8_t rssi) { this->rssi_floor_ = rssi; }
  int8_t get_rssi_floor() const { return this->rssi_floor_; }

  /// Per-category RSSI limits. Every advertisement is categorised first (MAC
  /// allowlist / IRK match / allowlisted service UUID / everything else) and
  /// then measured against that category's own limit, so all four are fully
  /// independent - any one can be looser or stricter than any other.
  ///
  /// -127 (the default) means INHERIT, and what it inherits reproduces
  /// pre-v1.1.0 behaviour exactly for a config that sets none of these:
  ///   mac_allowlist  -> no limit beyond rssi_floor (it has always been a full
  ///                     bypass of rssi_threshold)
  ///   irk            -> rssi_threshold (they were resolved after it ran, so it
  ///   service_uuid      always applied to them)
  ///
  /// The categories want different distances. Beacon tags (mac_allowlist) are
  /// the reason the bypass exists: a weak reading at one proxy is exactly what
  /// places the tag nearer another, so they want the loosest limit. Phones
  /// (irks) are tracked the same way but are far chattier. A device in pairing
  /// mode (service_uuid_allowlist) is in your hand, so it can afford the
  /// strictest limit of the three.
  ///
  /// rssi_floor still bounds all of them - a category limit cannot loosen past
  /// it, and configuration validation rejects one that tries.
  /// Exempt iBeacon advertisements (Apple company id, subtype 0x02) from
  /// manufacturer_blocklist, the same way allow_homekit exempts HAP. Needed
  /// whenever 0x004C is blocklisted and something you care about advertises an
  /// iBeacon - notably ESPHome proxies beaconing for BLE positioning
  /// self-calibration, which advertise from a public Espressif MAC and so have
  /// no IRK to protect them.
  ///
  /// Two scopes, mirroring the YAML:
  ///   allow_ibeacon: true   -> set_allow_ibeacon(true), every iBeacon exempt
  ///   allow_ibeacon: [...]  -> the major/minor filters added below
  ///
  /// A filter is either a whole major (add_ibeacon_major) or an exact
  /// major/minor pair (add_ibeacon_major_minor). Stored flat rather than as a
  /// vector-of-vectors: a pair packs into one uint32 and the lists run to a
  /// handful of entries, so a linear scan beats the allocations.
  ///
  /// These NARROW the exemption; they never add a drop rule. An iBeacon
  /// matching nothing falls through to the normal manufacturer test, exactly
  /// as if the exemption were off - which is what lets one fleet-wide major
  /// exempt your own probes while every other iBeacon in range stays blocked.
  /// Each filter carries its own RSSI limit (-127 = forward at any strength),
  /// which OVERRIDES both rssi_threshold and rssi_floor for adverts it matches.
  /// That is the point: probe-to-probe ranging for BLE positioning wants the
  /// weak cross-room readings the fleet threshold exists to discard, and only
  /// for the beacons doing the ranging.
  /// Sentinel for "this rule brought no RSSI limit of its own". Outside the
  /// valid -127..0 range, so it cannot collide with a real setting - unlike
  /// -127, which means the opposite (forward at any strength).
  static constexpr int8_t IBEACON_RSSI_INHERIT = -128;

  void set_allow_ibeacon(bool allow) { this->allow_ibeacon_ = allow; }
  void set_ibeacon_any_rssi(int8_t rssi) { this->ibeacon_any_rssi_ = rssi; }
  void add_ibeacon_major(uint16_t major, int8_t rssi) { this->ibeacon_majors_.push_back({major, rssi}); }
  void add_ibeacon_major_minor(uint16_t major, uint16_t minor, int8_t rssi) {
    this->ibeacon_pairs_.push_back({(static_cast<uint32_t>(major) << 16) | minor, rssi});
  }
  /// Cheap pre-gate: the loosest limit any rule could apply. Anything weaker
  /// than this is dropped before the categoriser runs, so a fleet that lets its
  /// own beacons through at -127 still does not pay an AES resolve and a
  /// payload walk for every distant advert in the neighbourhood.
  void set_min_rssi_gate(int8_t rssi) { this->min_rssi_gate_ = rssi; }

  void set_rssi_mac_allowlist(int8_t rssi) { this->rssi_mac_allowlist_ = rssi; }
  void set_rssi_irk(int8_t rssi) { this->rssi_irk_ = rssi; }
  void set_rssi_service_uuid(int8_t rssi) { this->rssi_service_uuid_ = rssi; }

  /// Advertisement counters, for measuring what rssi_threshold actually costs.
  /// Free-running and never reset: unsigned wraparound is well defined, so a
  /// consumer taking deltas stays correct across the 32-bit rollover.
  uint32_t get_adv_forwarded() const { return this->adv_forwarded_; }
  uint32_t get_adv_dropped() const { return this->adv_dropped_; }
  /// Subset of get_adv_dropped(): RPAs discarded for matching no configured
  /// IRK. Tracked separately because it answers a different question - how
  /// much of the noise is untrackable phones rather than distant devices.
  uint32_t get_adv_dropped_rpa() const { return this->adv_dropped_rpa_; }
  /// Subset of get_adv_forwarded(): advertisements that reached Home Assistant
  /// only because they carried an allowlisted service UUID. Zero while nothing
  /// is pairing, so a non-zero reading is direct evidence the passthrough fired.
  uint32_t get_adv_allowed_service_uuid() const { return this->adv_allowed_service_uuid_; }
  /// Subset of get_adv_dropped(): advertisements discarded by the rssi_floor.
  /// Separated out because it is the only counter that can include otherwise
  /// protected devices, so a rising value means a tracked tag is being cut -
  /// which is exactly when the floor needs revisiting.
  uint32_t get_adv_dropped_floor() const { return this->adv_dropped_floor_; }

  /// Concatenated 32-hex-char IRKs, parsed once in setup(). When the list is
  /// empty no IRK gating happens at all (upstream behaviour).
  void set_irks_hex(const char *hex) { this->irks_hex_ = hex; }
  /// Exempt Espressif-assigned addresses from the unresolved-RPA test below.
  /// NOTE: this does NOT bypass the RSSI threshold (an earlier version of this
  /// comment claimed it did). It is also close to inert on its own: ESPHome
  /// advertises on its public MAC and a public address is never an RPA, so
  /// those advertisements never reach that test anyway. To keep a distant
  /// unprovisioned ESP forwarded, allowlist the Improv service UUID instead.
  void set_allow_espressif(bool allow) { this->allow_espressif_ = allow; }
  /// Substring (already lowercased) matched against the advertised local name;
  /// a hit drops the advertisement.
  void add_blocked_name(const char *needle) { this->name_blocklist_.push_back(needle); }
  /// Bluetooth SIG company identifier whose advertisements are dropped.
  /// IRK-matched and mac_allowlist devices are exempt, so blocking e.g. Apple
  /// does not discard our own phones.
  void add_blocked_manufacturer(uint16_t company) { this->manufacturer_blocklist_.push_back(company); }
  /// Drop non-resolvable private addresses. These rotate like an RPA but carry
  /// no identity at all - an IRK cannot resolve them - so they can never be
  /// tracked or reliably connected to. Off by default (upstream behaviour).
  void set_drop_non_resolvable(bool drop) { this->drop_non_resolvable_ = drop; }
  /// Exempt HomeKit (HAP) accessory advertisements from manufacturer_blocklist.
  ///
  /// HAP-over-BLE accessories advertise Apple's company id (0x004C) with
  /// subtype 0x06, so blocklisting Apple to suppress phone/AirPods/AirTag noise
  /// also silently discards every HomeKit BLE accessory - and unlike our own
  /// phones they have no IRK, so nothing else rescues them. On by default: the
  /// blocklist is aimed at untrackable consumer noise, not at accessories the
  /// user deliberately owns.
  void set_allow_homekit(bool allow) { this->allow_homekit_ = allow; }
  /// Address that bypasses every filter. Use for beacons that must always be
  /// forwarded (tracked tags), which typically advertise no local name.
  void add_allowed_mac(uint64_t addr) { this->mac_allowlist_.push_back(addr); }
  /// Address this proxy ignores entirely, everything else proceeding normally.
  ///
  /// Checked before mac_allowlist and before every other filter, so an entry
  /// here cannot be overridden by a broader allow rule.
  ///
  /// For multi-proxy bonding conflicts: when several proxies are in range of a
  /// device that requires bonding, only one can hold the bond cleanly and the
  /// others cause connection thrashing. Home Assistant picks a proxy from those
  /// reporting a device, so dropping its advertisements here takes this proxy
  /// out of contention without turning it into a single-purpose bridge.
  void add_blocked_mac(uint64_t addr) { this->mac_blocklist_.push_back(addr); }
  /// Turn mac_allowlist from a bypass list into an exclusive one: nothing but
  /// those addresses is forwarded.
  ///
  /// Reproduces the observable behaviour of the ESP-IDF controller whitelist
  /// (esphome/esphome#14353) without needing it. That filters in hardware, so
  /// where it is available it is cheaper - but it is ESP32-only, and this works
  /// on every platform bluetooth_proxy supports.
  void set_allowlist_exclusive(bool exclusive) { this->allowlist_exclusive_ = exclusive; }
  /// 16-bit service UUID that bypasses every filter, matched against the
  /// advertisement's service UUID lists, solicitation lists and service data.
  ///
  /// Unlike add_allowed_mac() this protects a device whose address is not known
  /// in advance, which is the whole point: a device advertising a transient
  /// commissioning/pairing service (Matter uses 0xFFF6) does so from a rotating
  /// private address, so the address-type filters below would discard it and no
  /// MAC could be allowlisted ahead of time.
  void add_allowed_service_uuid(uint16_t uuid) { this->service_uuid_allowlist_.push_back(uuid); }
  /// As add_allowed_service_uuid(), for a 128-bit (vendor) service UUID.
  /// Takes the canonical big-endian byte order; advertisements carry these
  /// little-endian, and the walker reverses before comparing.
  ///
  /// Improv Wi-Fi (00467768-6228-2272-4663-277478268000) is the case this
  /// exists for: an unprovisioned ESP advertises it from its public Espressif
  /// address, so the address-type tests never touch it, but the RSSI threshold
  /// does - allow_espressif does NOT exempt it, despite what this header used
  /// to claim. Allowlisting the UUID is what actually makes provisioning work
  /// on a far-away board.
  void add_allowed_service_uuid128(const char *hex) { this->service_uuid128_hex_.push_back(hex); }

  uint32_t get_legacy_version() const {
    if (!this->active_) {
      return LEGACY_PASSIVE_ONLY_VERSION;
    }
    // Legacy clients (which predate the feature flags) map versions to
    // capability sets: 5 adds cache clearing, 4 adds pairing, 3 is active
    // connections only.
    if (bluetooth_connection::SUPPORTS_CACHE_CLEARING) {
      return LEGACY_ACTIVE_CONNECTIONS_VERSION;
    }
    return bluetooth_connection::SUPPORTS_PAIRING ? LEGACY_ACTIVE_NO_CACHE_CLEAR_VERSION
                                                  : LEGACY_ACTIVE_NO_PAIRING_VERSION;
  }

  uint32_t get_feature_flags() const {
    uint32_t flags = 0;
    flags |= BluetoothProxyFeature::FEATURE_PASSIVE_SCAN;
    flags |= BluetoothProxyFeature::FEATURE_RAW_ADVERTISEMENTS;
#ifdef USE_ESP32
    flags |= BluetoothProxyFeature::FEATURE_STATE_AND_MODE;
#else
    // Advertise mode switching only where the hub honors request_scan_mode();
    // scan_mode_switch is the capability bit for exactly that (#18079) —
    // active_scan alone is not enough, a hub may support active scanning yet
    // refuse the runtime switch.
    if (ble_device_base::BLEHub::get_capabilities().scan_mode_switch) {
      flags |= BluetoothProxyFeature::FEATURE_STATE_AND_MODE;
    }
#endif
    if (this->active_) {
      // REMOTE_CACHING is mandatory for active connections: API clients
      // refuse to connect without it (it selects which V3 connect request
      // they send, not device-side caching).
      flags |= BluetoothProxyFeature::FEATURE_ACTIVE_CONNECTIONS;
      flags |= BluetoothProxyFeature::FEATURE_REMOTE_CACHING;
      flags |= BluetoothProxyFeature::FEATURE_CONNECTION_PARAMS_SETTING;
      if (bluetooth_connection::SUPPORTS_PAIRING) {
        flags |= BluetoothProxyFeature::FEATURE_PAIRING;
      }
      if (bluetooth_connection::SUPPORTS_CACHE_CLEARING) {
        flags |= BluetoothProxyFeature::FEATURE_CACHE_CLEARING;
      }
    }

    return flags;
  }

  void get_bluetooth_mac_address_pretty(std::span<char, MAC_ADDRESS_PRETTY_BUFFER_SIZE> output) {
    uint8_t mac[MAC_ADDRESS_SIZE] = {};
    this->hub_->get_adapter_mac(mac);
    // Unavailable -> empty string: some hubs (rp2040's BTstack) only learn
    // the address once the link layer is up, and report all-zero until then.
    if (mac_address_is_valid(mac)) {
      format_mac_addr_upper(mac, output.data());
    } else {
      output[0] = '\0';
    }
  }

 protected:
  bool send_bluetooth_scanner_state_(ble_device_base::ScannerState state);
#ifdef USE_BLE_SCANNER_STATE_CALLBACK
  void send_scanner_state_(ble_device_base::ScannerState state);
#else
  void send_polled_scanner_state_();
#endif
  void on_raw_advertisement_(const ble_device_base::RawAdvertisement &raw);

  /// Caller must ensure api_connection_ is non-null and API server is connected.
  void flush_pending_advertisements_() {
    if (this->response_.advertisements_len == 0)
      return;
    // Perishable and the highest-frequency send here: a drop only reports at
    // V, anything louder would be the flood the batch pacing exists to avoid.
    [[maybe_unused]] bool sent = this->api_connection_->send_message(this->response_);
#if ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_VERBOSE
    this->log_advertisement_flush_(sent);
#endif
    this->response_.advertisements_len = 0;
  }
  void log_advertisement_flush_(bool sent);

#ifdef USE_BLUETOOTH_PROXY_CONNECTIONS
  BluetoothConnection *get_connection_(uint64_t address, bool reserve);
  void log_connection_request_ignored_(BluetoothConnection *connection, ClientState state);
  void log_connection_info_(BluetoothConnection *connection, const char *message);
  void log_not_connected_gatt_(const char *action, const char *type);
  void handle_gatt_not_connected_(uint64_t address, uint16_t handle, const char *action, const char *type);

  /// Keep the pre-allocated connections-free message in step when a
  /// connection slot changes address (0 = free). Called from the connection
  /// classes' set_address().
  void update_address_slot_(uint64_t old_address, uint64_t new_address) {
    auto &resp = this->connections_free_response_;
    if (new_address == 0 && old_address != 0) {
      if (resp.free < BLUETOOTH_PROXY_MAX_CONNECTIONS) {
        resp.free++;
      } else {
        this->log_slot_accounting_mismatch_();
      }
      this->replace_allocated_slot_(old_address, 0);
    } else if (new_address != 0 && old_address == 0) {
      if (resp.free > 0) {
        resp.free--;
      } else {
        this->log_slot_accounting_mismatch_();
      }
      this->replace_allocated_slot_(0, new_address);
    }
  }
  void replace_allocated_slot_(uint64_t find_value, uint64_t set_value);
  void log_slot_accounting_mismatch_();
  /// Free a connection slot after teardown: notify the API client and reset
  /// the streaming cursor. Important: does NOT send send_gatt_services_done()
  /// when service streaming was interrupted -- the client (aioesphomeapi) has
  /// a 30-second timeout (DEFAULT_BLE_TIMEOUT) to detect incomplete service
  /// discovery and retry, rather than being told a partial list is complete.
  void reset_connection_slot_(BluetoothConnection *connection, conn_err_t reason);
  /// Drop any owed freed-slot notification for this address (client reconnected).
  void clear_pending_disconnection_(uint64_t address);
  /// Send connected=false and pool it for the paced drain if refused. A
  /// dropped disconnect desynchronises the proxy: the client keeps a link it
  /// believes is live and every operation on it times out. Unsolicited and
  /// drained notifications only; request answers use the variant below.
  void send_device_disconnected_(uint64_t address, conn_err_t error = CONN_OK);
  /// Answer a request with connected=false. Never pools: a refusal falls back
  /// to the client's request timeout, keeping the pool for the unsolicited
  /// notifications the client cannot recover on its own.
  void answer_device_disconnected_(uint64_t address);
  /// Pool a refused freed-slot notification for the paced drain.
  void latch_pending_disconnection_(uint64_t address, conn_err_t error);
#endif

  /// Drop everything the ending session was owed. One list, so a new latch is
  /// one edit rather than two call sites where an omission looks deliberate.
  /// Drops state only, never sends: api_connection_ is the departing
  /// subscriber on subscribe and nullptr on unsubscribe.
  void reset_owed_replies_();
#ifdef USE_BLUETOOTH_PROXY_CONNECTIONS
  /// Report a reply we deliberately do not latch, so no drop is silent.
  void log_reply_dropped_(const char *what, uint64_t address);
  /// A latched reply's leading edge; the drain's re-refusals stay quiet.
  void log_reply_deferred_(const char *what, uint64_t address);
  /// A latched reply lost to a newer one for a different address.
  void log_reply_displaced_(const char *what, uint64_t owed, uint64_t address);
#endif

  // Memory optimized layout for 32-bit systems
  // Group 1: Pointers (4 bytes each, naturally aligned)
  api::APIConnection *api_connection_{nullptr};

#ifdef USE_BLUETOOTH_PROXY_CONNECTIONS
  // Group 2: Fixed-size array of connection pointers
  std::array<BluetoothConnection *, BLUETOOTH_PROXY_MAX_CONNECTIONS> connections_{};
  // Address-keyed pool of owed freed-slot notifications; loop() resends.
  // Proxy-only state, kept off BluetoothConnection; entries are not tied to
  // slot indices.
  std::array<PendingReply, BLUETOOTH_PROXY_MAX_CONNECTIONS> pending_disconnections_{};
  // Owed unpair reply. The bond is already gone when the send is refused, so
  // a retry is told the unpair failed when it succeeded. One slot: a second
  // refused unpair displaces the first, as happened to both before this.
  PendingReply pending_unpairing_{};
#endif
  ble_device_base::BLEHub *hub_{nullptr};
  // Group 3: 4-byte types; paired with hub_ so the 8-aligned messages below
  // start on an even word, closing two alignment holes.
  uint32_t last_advertisement_flush_time_{0};
  // Advertisement accounting (see get_adv_forwarded/get_adv_dropped). Kept in
  // the 4-byte group so they slot into the existing alignment run.
  uint32_t adv_forwarded_{0};
  uint32_t adv_dropped_{0};
  uint32_t adv_dropped_rpa_{0};
  uint32_t adv_allowed_service_uuid_{0};
  uint32_t adv_dropped_floor_{0};

  // Identity Resolving Keys. irks_hex_ is the compile-time blob; it is parsed
  // into irks_ during setup() and then dropped.
  const char *irks_hex_{nullptr};
  std::vector<std::array<uint8_t, 16>> irks_;
  // Lowercased substrings; pointers into flash-resident string literals.
  std::vector<const char *> name_blocklist_;
  // Always-forward addresses, checked before any filter.
  std::vector<uint64_t> mac_allowlist_;

  // Addresses this proxy ignores outright; beats every allow rule.
  std::vector<uint64_t> mac_blocklist_;

  std::vector<uint16_t> service_uuid_allowlist_;
  // key + its own RSSI limit. Two lists rather than one keyed union so the
  // exact-pair lookup stays a plain uint32 compare.
  struct IBeaconRule {
    uint32_t key;   // major, or (major << 16) | minor
    int8_t rssi;    // -127 = any strength
  };
  struct IBeaconMajorRule {
    uint16_t key;
    int8_t rssi;
  };
  std::vector<IBeaconMajorRule> ibeacon_majors_;
  std::vector<IBeaconRule> ibeacon_pairs_;
  // Compile-time 32-hex-char blobs, parsed into service_uuid128_ during setup()
  // and then dropped - same pattern as irks_hex_.
  std::vector<const char *> service_uuid128_hex_;
  std::vector<std::array<uint8_t, 16>> service_uuid128_;
  // Blocked Bluetooth SIG company identifiers (AD type 0xFF).
  std::vector<uint16_t> manufacturer_blocklist_;

  /// True when addr is a Resolvable Private Address: a *random* address whose
  /// top two bits are 0b01. The addr_type check is essential - plenty of public
  /// OUIs (Espressif's 4C:xx among them) fall in that numeric range and would
  /// otherwise be misread as RPAs and discarded.
  static bool address_is_rpa_(uint64_t addr, uint8_t addr_type);
  /// True for a *random* address whose top two bits are 0b00. The addr_type
  /// check is essential: plenty of real public OUIs begin with a low octet
  /// (00:, 04:, 15: ...) and would otherwise be mistaken for these.
  static bool address_is_non_resolvable_(uint64_t addr, uint8_t addr_type);
  /// Bluetooth Core "ah" hash against every configured IRK.
  bool irk_matches_(uint64_t addr) const;
  /// True when the address's OUI is one of Espressif's IEEE assignments.
  static bool is_espressif_oui_(uint64_t addr);
  /// Walks the advertisement's length/type/value structures once, looking for a
  /// blocklisted manufacturer id (AD type 0xFF) or a local name (0x08/0x09)
  /// containing a blocklisted substring.
  bool payload_blocked_(const uint8_t *data, uint16_t len) const;
  /// True when the advert is an iBeacon accepted by a configured filter;
  /// writes that filter's RSSI limit to limit_out.
  bool ibeacon_match_(const uint8_t *data, uint16_t len, int8_t *limit_out) const;
  /// Walks the same length/type/value structures looking for any allowlisted
  /// 16-bit service UUID. Only called when service_uuid_allowlist_ is non-empty.
  bool payload_has_allowed_service_uuid_(const uint8_t *data, uint16_t len) const;
  /// One 128-bit UUID from an advertisement (little-endian, as transmitted)
  /// against the long allowlist, and against the short one via the Base UUID.
  bool uuid128_matches_(const uint8_t *le_bytes) const;

  // BLE advertisement batching
  api::BluetoothLERawAdvertisementsResponse response_;

#ifdef USE_BLUETOOTH_PROXY_CONNECTIONS
  // Pre-allocated response message - always ready to send
  api::BluetoothConnectionsFreeResponse connections_free_response_;
#endif

  // Group 4: 1-byte types grouped together
  bool active_;
  // Signed: BLE RSSI is negative dBm. -127 forwards everything.
  int8_t rssi_threshold_{-127};
  // Absolute floor applied ahead of every allow rule. -127 disables it.
  int8_t rssi_floor_{-127};
  // Per-category limits; -127 means "no limit beyond rssi_floor_".
  int8_t rssi_mac_allowlist_{-127};
  int8_t rssi_irk_{-127};
  int8_t rssi_service_uuid_{-127};
  bool allow_espressif_{true};
  bool drop_non_resolvable_{false};
  bool allow_homekit_{true};
  bool allow_ibeacon_{false};
  int8_t ibeacon_any_rssi_{-127};
  int8_t min_rssi_gate_{-127};
  bool allowlist_exclusive_{false};
#ifdef USE_BLUETOOTH_PROXY_CONNECTIONS
  // A dropped send (full TCP buffer) would leave the API client with a stale
  // slot state forever; the cached response is current by construction, so
  // retrying it from loop() is an idempotent resync.
  bool connections_free_pending_{false};
  uint8_t connection_count_{0};
#endif
  bool configured_scan_active_{false};  // Configured scan mode from YAML
#ifdef USE_WIFI
  /// Wi-Fi only: flush on every other non-empty tick (~200 ms) so partial
  /// batches fill; an idle tick re-arms, so the first batch after a gap
  /// still ships on the next tick. See loop().
  bool adv_flush_toggle_{false};
#endif
#ifdef USE_BLE_SCANNER_STATE_CALLBACK
  // A dropped push (full TX buffer) is re-queried from the hub and resent
  // from loop(); the hub's current state is idempotent by construction.
  bool scanner_state_pending_{false};
#else
  bool last_scan_running_{false};  // Last scanner state reported to the subscriber
#endif
};

extern BluetoothProxy *global_bluetooth_proxy;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

}  // namespace esphome::bluetooth_proxy

#endif  // USE_BLUETOOTH_PROXY
