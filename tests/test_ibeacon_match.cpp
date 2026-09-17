// Host-compiled tests for the two pieces of BLE filtering that are pure
// functions over a byte buffer and a little config: the iBeacon matcher and
// the per-category limit resolution.
//
// They cannot include bluetooth_proxy.cpp - it pulls in the whole ESPHome and
// ESP-IDF world - so the logic under test is transcribed here. That is a real
// weakness: the transcription can drift from the component. It is guarded by
// tools/check_test_sync.py, which fails if the source of either function
// changes without this file changing too.
//
//   c++ -std=c++17 -Wall -Wextra -o /tmp/t tests/test_ibeacon_match.cpp && /tmp/t

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

static constexpr int8_t IBEACON_RSSI_INHERIT = -128;

struct IBeaconRule { uint32_t key; int8_t rssi; };
struct IBeaconMajorRule { uint16_t key; int8_t rssi; };

struct Cfg {
  bool allow_ibeacon = false;
  int8_t ibeacon_any_rssi = IBEACON_RSSI_INHERIT;
  std::vector<IBeaconMajorRule> majors;
  std::vector<IBeaconRule> pairs;
  int8_t rssi_threshold = -127;
  int8_t rssi_floor = -127;
  int8_t rssi_mac_allowlist = -127;
  int8_t rssi_irk = -127;
  int8_t rssi_service_uuid = -127;
  int8_t findmy_rssi = IBEACON_RSSI_INHERIT;
};

// --- transcribed from BluetoothProxy::ibeacon_match_ ---
static bool ibeacon_match(const Cfg &c, const uint8_t *data, uint16_t len, int8_t *limit_out) {
  uint16_t i = 0;
  while (i < len) {
    const uint8_t field_len = data[i];
    if (field_len == 0) break;
    if (static_cast<uint32_t>(i) + 1u + field_len > len) break;
    if (data[i + 1] == 0xFF && field_len >= 4) {
      const uint16_t company = static_cast<uint16_t>(data[i + 2]) | (static_cast<uint16_t>(data[i + 3]) << 8);
      if (company == 0x004C && data[i + 4] == 0x02) {
        if (c.allow_ibeacon) { *limit_out = c.ibeacon_any_rssi; return true; }
        if (field_len < 26) return false;
        const uint16_t major = (static_cast<uint16_t>(data[i + 22]) << 8) | data[i + 23];
        const uint16_t minor = (static_cast<uint16_t>(data[i + 24]) << 8) | data[i + 25];
        const uint32_t want = (static_cast<uint32_t>(major) << 16) | minor;
        for (const auto &pr : c.pairs) if (pr.key == want) { *limit_out = pr.rssi; return true; }
        for (const auto &mj : c.majors) if (mj.key == major) { *limit_out = mj.rssi; return true; }
        return false;
      }
    }
    i += field_len + 1;
  }
  return false;
}

// --- transcribed from BluetoothProxy::findmy_match_ ---
static bool findmy_match(const uint8_t *data, uint16_t len) {
  uint16_t i = 0;
  while (i < len) {
    const uint8_t field_len = data[i];
    if (field_len == 0) break;
    if (static_cast<uint32_t>(i) + 1u + field_len > len) break;
    if (data[i + 1] == 0xFF && field_len >= 4) {
      const uint16_t company = static_cast<uint16_t>(data[i + 2]) | (static_cast<uint16_t>(data[i + 3]) << 8);
      if (company == 0x004C && data[i + 4] == 0x12) return true;
    }
    i += field_len + 1;
  }
  return false;
}

enum Cat { CAT_DEFAULT, CAT_MAC, CAT_IRK, CAT_UUID, CAT_IBEACON, CAT_FINDMY };

// --- transcribed from the limit-resolution block of on_raw_advertisement_ ---
static int8_t resolve_limit(const Cfg &c, Cat category, int8_t ibeacon_limit, bool ibeacon_has_limit,
                            bool findmy_has_limit = false) {
  int8_t limit;
  switch (category) {
    case CAT_IBEACON: limit = ibeacon_has_limit ? ibeacon_limit : c.rssi_threshold; break;
    case CAT_FINDMY: limit = findmy_has_limit ? c.findmy_rssi : c.rssi_threshold; break;
    case CAT_MAC:     limit = c.rssi_mac_allowlist; break;
    case CAT_IRK:     limit = c.rssi_irk != -127 ? c.rssi_irk : c.rssi_threshold; break;
    case CAT_UUID:    limit = c.rssi_service_uuid != -127 ? c.rssi_service_uuid : c.rssi_threshold; break;
    default:          limit = c.rssi_threshold; break;
  }
  if (!ibeacon_has_limit && !findmy_has_limit && c.rssi_floor != -127 && (limit == -127 || c.rssi_floor > limit))
    limit = c.rssi_floor;
  return limit;
}

// ---------------------------------------------------------------- helpers
static std::vector<uint8_t> ibeacon(uint16_t major, uint16_t minor, uint8_t subtype = 0x02,
                                    uint16_t company = 0x004C, uint8_t field_len = 26) {
  std::vector<uint8_t> d;
  d.push_back(field_len);
  d.push_back(0xFF);
  d.push_back(company & 0xFF);
  d.push_back(company >> 8);
  d.push_back(subtype);
  d.push_back(0x15);
  for (int k = 0; k < 16; k++) d.push_back(0xAB);
  d.push_back(major >> 8); d.push_back(major & 0xFF);
  d.push_back(minor >> 8); d.push_back(minor & 0xFF);
  d.push_back(0xC5);
  return d;
}

static int failures = 0;
static void check(bool ok, const char *what) {
  if (!ok) { printf("  FAIL  %s\n", what); failures++; }
  else       printf("  ok    %s\n", what);
}

int main() {
  printf("ibeacon_match_\n");
  {
    Cfg c; c.majors = {{1, -95}};
    int8_t lim = 0;
    auto a = ibeacon(1, 7);
    check(ibeacon_match(c, a.data(), a.size(), &lim) && lim == -95, "major rule matches, carries its rssi");

    auto b = ibeacon(2, 7);
    lim = 0;
    check(!ibeacon_match(c, b.data(), b.size(), &lim), "non-matching major falls through");
  }
  {
    // An exact pair must win over a whole-major rule regardless of list order.
    Cfg c; c.majors = {{1, -95}}; c.pairs = {{(1u << 16) | 7u, -70}};
    int8_t lim = 0;
    auto a = ibeacon(1, 7);
    check(ibeacon_match(c, a.data(), a.size(), &lim) && lim == -70, "exact major+minor beats whole-major");
    auto b = ibeacon(1, 8);
    lim = 0;
    check(ibeacon_match(c, b.data(), b.size(), &lim) && lim == -95, "other minor still gets the major rule");
  }
  {
    Cfg c; c.allow_ibeacon = true; c.ibeacon_any_rssi = IBEACON_RSSI_INHERIT;
    int8_t lim = 0;
    auto a = ibeacon(9, 9);
    check(ibeacon_match(c, a.data(), a.size(), &lim) && lim == IBEACON_RSSI_INHERIT,
          "bare allow_ibeacon matches anything and inherits");
  }
  {
    // Truncated: carries no major/minor, so a filter cannot match it.
    Cfg c; c.majors = {{1, -95}};
    int8_t lim = 0;
    auto a = ibeacon(1, 7, 0x02, 0x004C, 10);
    a.resize(11);
    check(!ibeacon_match(c, a.data(), a.size(), &lim), "truncated iBeacon does not match a filter");

    Cfg c2; c2.allow_ibeacon = true;
    lim = 0;
    check(ibeacon_match(c2, a.data(), a.size(), &lim), "...but bare allow_ibeacon still exempts it");
  }
  {
    Cfg c; c.majors = {{1, -95}};
    int8_t lim = 0;
    auto hap = ibeacon(1, 7, 0x06);              // HomeKit, not iBeacon
    check(!ibeacon_match(c, hap.data(), hap.size(), &lim), "Apple subtype 0x06 (HAP) is not an iBeacon");
    auto other = ibeacon(1, 7, 0x02, 0x0075);    // Samsung
    check(!ibeacon_match(c, other.data(), other.size(), &lim), "non-Apple company id is not an iBeacon");
  }
  {
    // A length byte claiming more than the buffer holds must not read past it.
    Cfg c; c.allow_ibeacon = true;
    int8_t lim = 0;
    std::vector<uint8_t> d = {40, 0xFF, 0x4C, 0x00, 0x02};
    check(!ibeacon_match(c, d.data(), d.size(), &lim), "over-long field_len is rejected, not read");
    std::vector<uint8_t> z = {0};
    check(!ibeacon_match(c, z.data(), z.size(), &lim), "zero-length field terminates the walk");
  }

  printf("\nlimit resolution (threshold -75, floor -90)\n");
  {
    Cfg c; c.rssi_threshold = -75; c.rssi_floor = -90;
    check(resolve_limit(c, CAT_DEFAULT, 0, false) == -75, "DEFAULT uses the threshold");
    check(resolve_limit(c, CAT_MAC, 0, false) == -90, "MAC with no limit is bounded by the floor");
    check(resolve_limit(c, CAT_IRK, 0, false) == -75, "IRK inherits the threshold");
    check(resolve_limit(c, CAT_UUID, 0, false) == -75, "UUID inherits the threshold");
    check(resolve_limit(c, CAT_IBEACON, -95, true) == -95, "iBeacon explicit rssi overrides the floor");
    check(resolve_limit(c, CAT_IBEACON, -127, true) == -127, "iBeacon -127 forwards at any strength");
    c.findmy_rssi = -85;
    check(resolve_limit(c, CAT_FINDMY, 0, false, true) == -85, "FindMy explicit rssi overrides the floor");
    check(resolve_limit(c, CAT_FINDMY, 0, false, false) == -75, "FindMy without rssi inherits the threshold");
    {
      // An AirTag: flags, then Apple manufacturer data with the Offline
      // Finding subtype 0x12 and a status byte + 22 key bytes.
      uint8_t airtag[31] = {0x02, 0x01, 0x1A, 0x1B, 0xFF, 0x4C, 0x00, 0x12, 0x19, 0x10};
      check(findmy_match(airtag, sizeof(airtag)), "Offline Finding subtype 0x12 matches");
      uint8_t hap[8] = {0x07, 0xFF, 0x4C, 0x00, 0x06, 0x31, 0x00, 0x00};
      check(!findmy_match(hap, sizeof(hap)), "HomeKit subtype 0x06 does not match");
      uint8_t other[8] = {0x07, 0xFF, 0x4C, 0x00, 0x02, 0x15, 0x00, 0x00};
      check(!findmy_match(other, sizeof(other)), "iBeacon subtype 0x02 does not match");
      uint8_t truncated[3] = {0x1B, 0xFF, 0x4C};
      check(!findmy_match(truncated, sizeof(truncated)), "a truncated structure never matches");
    }
    check(resolve_limit(c, CAT_IBEACON, IBEACON_RSSI_INHERIT, false) == -75,
          "iBeacon inheriting is bounded like anything else");
  }
  {
    // The floor must bind when it is STRICTER than a category's own limit.
    Cfg c; c.rssi_threshold = -75; c.rssi_floor = -60; c.rssi_mac_allowlist = -80;
    check(resolve_limit(c, CAT_MAC, 0, false) == -60, "a stricter floor overrides a looser category limit");
    check(resolve_limit(c, CAT_DEFAULT, 0, false) == -60, "a stricter floor overrides the threshold");
  }
  {
    Cfg c;  // everything default/off
    check(resolve_limit(c, CAT_DEFAULT, 0, false) == -127, "unconfigured forwards everything");
  }

  printf("\n%s\n", failures ? "FAILED" : "all passed");
  return failures ? 1 : 0;
}
