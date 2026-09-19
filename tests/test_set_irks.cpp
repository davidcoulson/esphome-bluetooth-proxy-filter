// Host-compiled tests for BluetoothProxy::set_irks(), the runtime IRK loader.
//
// Same arrangement as test_ibeacon_match.cpp: the component cannot be built on
// the host, so the parsing loop is transcribed here, and
// tools/check_test_sync.py fails if the source changes without this file.
//
// What matters most is the failure behaviour. The list is fed from a Home
// Assistant entity; when that entity is briefly unavailable the proxy receives
// "unavailable" or an empty string, and with a manufacturer blocklist active a
// wiped list would silently drop our own phones. So "no keys found" must leave
// the current list alone.
//
//   c++ -std=c++17 -Wall -Wextra -o /tmp/t tests/test_set_irks.cpp && /tmp/t

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using Irk = std::array<uint8_t, 16>;

// Stand-in for esphome::parse_hex over a fixed-length run the caller has
// already checked is all hex digits.
static size_t parse_hex(const char *s, size_t n, uint8_t *out, size_t out_len) {
  auto nib = [](char c) -> uint8_t {
    return c <= '9' ? c - '0' : (std::tolower(static_cast<unsigned char>(c)) - 'a' + 10);
  };
  if (n != out_len * 2) return 0;
  for (size_t k = 0; k < out_len; k++) out[k] = static_cast<uint8_t>((nib(s[2 * k]) << 4) | nib(s[2 * k + 1]));
  return n;
}

// --- transcribed from BluetoothProxy::set_irks ---
static int set_irks(std::vector<Irk> &live, const std::string &text) {
  std::vector<Irk> parsed;
  const size_t n = text.size();
  size_t i = 0;
  while (i < n) {
    if (!isxdigit(static_cast<unsigned char>(text[i]))) {
      i++;
      continue;
    }
    size_t j = i;
    while (j < n && isxdigit(static_cast<unsigned char>(text[j])))
      j++;
    if (j - i == 32) {
      Irk irk{};
      if (parse_hex(text.c_str() + i, 32, irk.data(), 16) == 32 &&
          std::find(parsed.begin(), parsed.end(), irk) == parsed.end())
        parsed.push_back(irk);
    }
    i = j;
  }
  if (parsed.empty()) return -1;
  live = std::move(parsed);
  return static_cast<int>(live.size());
}

static int failures = 0;
static void check(bool ok, const char *what) {
  printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) failures++;
}

static const std::string K1 = "00112233445566778899aabbccddeeff";
static const std::string K2 = "ffeeddccbbaa99887766554433221100";
static const std::string K3 = "0123456789ABCDEF0123456789abcdef";

int main() {
  {
    std::vector<Irk> l;
    check(set_irks(l, K1 + "," + K2) == 2, "comma-separated list");
    check(set_irks(l, K1 + "\n" + K2 + "\n" + K3 + "\n") == 3, "newline-separated, trailing newline");
    check(set_irks(l, K3) == 1 && l[0][0] == 0x01 && l[0][15] == 0xef, "mixed case parses to the right bytes");
  }
  {
    // The Home Assistant side keeps a label next to each key. A stringified
    // dict is exactly what a template sensor attribute delivers.
    std::vector<Irk> l;
    std::string dict = "{'David phone': '" + K1 + "', 'Dad watch': '" + K2 + "'}";
    check(set_irks(l, dict) == 2, "stringified dict with labels");
    check(set_irks(l, "David phone: " + K1 + "\nFaded AC fob: " + K2) == 2,
          "labels made of hex letters (dad, faded, ac) are not mistaken for keys");
  }
  {
    std::vector<Irk> l;
    set_irks(l, K1 + "," + K2);
    check(set_irks(l, "") == -1 && l.size() == 2, "empty input keeps the current list");
    check(set_irks(l, "unavailable") == -1 && l.size() == 2, "'unavailable' keeps the current list");
    check(set_irks(l, "unknown") == -1 && l.size() == 2, "'unknown' keeps the current list");
    check(set_irks(l, "{}") == -1 && l.size() == 2, "an empty dict keeps the current list");
  }
  {
    std::vector<Irk> l;
    set_irks(l, K1);
    check(set_irks(l, K1 + "0") == -1 && l.size() == 1, "33 hex digits is not a key with a suffix");
    check(set_irks(l, K1.substr(0, 31)) == -1, "31 hex digits is not a key");
    check(set_irks(l, K1 + K2) == -1, "two keys run together (64 digits) are rejected, not split");
    check(set_irks(l, "0x" + K2) == 1 && l[0][0] == 0xff, "a 0x prefix is tolerated");
  }
  {
    std::vector<Irk> l;
    check(set_irks(l, K1 + "," + K1 + "," + K2) == 2, "duplicates are dropped");
  }
  {
    // A partially bad list still loads the good keys; it does not keep the old list.
    std::vector<Irk> l;
    set_irks(l, K3);
    check(set_irks(l, K1 + ", not-a-key, " + K2.substr(0, 20)) == 1 && l[0][0] == 0x00,
          "a list with some junk replaces the old list with the keys it does contain");
  }

  printf("\n%s\n", failures ? "FAILED" : "all passed");
  return failures ? 1 : 0;
}
