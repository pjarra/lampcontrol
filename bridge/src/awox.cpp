#include "awox.h"
#include "aes.h"
#include <string.h>

namespace awox {
namespace {
void reversed16(const uint8_t* in, uint8_t* out) { for (int i = 0; i < 16; i++) out[i] = in[15 - i]; }
int clampInt(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

// [CRC][length][destination BE][type=1][opcode hi lo][data][zero pad], see protocol.js commandBlock.
Bytes commandPacket(const uint8_t key[16], uint16_t target, uint8_t opHi, uint8_t opLo, const Bytes& data) {
  uint8_t block[16] = {0};
  Bytes fields = {(uint8_t)(data.size() + 5), (uint8_t)(target >> 8), (uint8_t)(target & 255), 1, opHi, opLo};
  fields.insert(fields.end(), data.begin(), data.end());
  block[0] = crc8(fields.data(), fields.size());
  memcpy(block + 1, fields.data(), fields.size());
  Bytes out(17);
  out[0] = 0x00;
  attEncrypt(key, block, out.data() + 1);
  return out;
}
// [CRC][length][payload], at least 16 bytes; only the first 16 are encrypted (protocol.js queryBlock/packet).
Bytes queryPacket(const uint8_t key[16], const Bytes& payload) {
  Bytes fields = {(uint8_t)payload.size()};
  fields.insert(fields.end(), payload.begin(), payload.end());
  Bytes block(fields.size() + 1 < 16 ? 16 : fields.size() + 1, 0);
  block[0] = crc8(fields.data(), fields.size());
  memcpy(block.data() + 1, fields.data(), fields.size());
  Bytes out(1 + block.size());
  out[0] = 0x02;
  attEncrypt(key, block.data(), out.data() + 1);
  memcpy(out.data() + 17, block.data() + 16, block.size() - 16);
  return out;
}
void le32(Bytes& b, uint32_t v) { for (int i = 0; i < 4; i++) b.push_back((uint8_t)(v >> (8 * i))); }
}  // namespace

bool combine(const std::string& name, const std::string& password, uint8_t out[16]) {
  if (name.empty() || name.size() > 16 || password.empty() || password.size() > 16) return false;
  memset(out, 0, 16);
  for (size_t i = 0; i < name.size(); i++) out[i] = (uint8_t)name[i];
  for (size_t i = 0; i < password.size(); i++) out[i] ^= (uint8_t)password[i];
  return true;
}

void attEncrypt(const uint8_t key[16], const uint8_t value[16], uint8_t out[16]) {
  uint8_t k[16], v[16], o[16];
  reversed16(key, k); reversed16(value, v);
  aes::encrypt(k, v, o);
  reversed16(o, out);
  memset(k, 0, 16); memset(v, 0, 16); memset(o, 0, 16);
}
void attDecrypt(const uint8_t key[16], const uint8_t value[16], uint8_t out[16]) {
  uint8_t k[16], v[16], o[16];
  reversed16(key, k); reversed16(value, v);
  aes::decrypt(k, v, o);
  reversed16(o, out);
  memset(k, 0, 16); memset(v, 0, 16); memset(o, 0, 16);
}

uint8_t crc8(const uint8_t* data, size_t length) {
  uint8_t crc = 0;
  for (size_t n = 0; n < length; n++) {
    crc ^= data[n];
    for (int i = 0; i < 8; i++) crc = (crc & 1) ? (uint8_t)((crc >> 1) ^ 0x6C) : (uint8_t)(crc >> 1);
  }
  return crc;
}

Bytes pairPacket(const uint8_t combined[16], const uint8_t random[8]) {
  uint8_t r16[16] = {0}, enc[16];
  memcpy(r16, random, 8);
  attEncrypt(r16, combined, enc);
  Bytes out = {0x0C};
  out.insert(out.end(), random, random + 8);
  out.insert(out.end(), enc, enc + 8);
  return out;
}
void sessionKey(const uint8_t combined[16], const uint8_t random[8], const uint8_t response[8], uint8_t key[16]) {
  uint8_t value[16];
  memcpy(value, random, 8); memcpy(value + 8, response, 8);
  attEncrypt(combined, value, key);
  memset(value, 0, 16);
}

bool addressOK(uint16_t t) { return (t >> 8) != 0x80 && t != 0xFFFE; }
Bytes powerPacket(const uint8_t key[16], bool on, uint16_t target) {
  return commandPacket(key, target, 6, 0, {(uint8_t)(on ? 1 : 0)});
}
Bytes brightnessPacket(const uint8_t key[16], int value, uint16_t target) {
  return commandPacket(key, target, 8, 0, {4, (uint8_t)clampInt(value, BRIGHTNESS_MIN, BRIGHTNESS_MAX), 2, 0, 0, 0, 0});
}
Bytes temperaturePacket(const uint8_t key[16], int mired, uint16_t target) {
  int m = clampInt(mired, MIRED_MIN, MIRED_MAX);
  return commandPacket(key, target, 0, 3, {10, (uint8_t)(m & 255), (uint8_t)(m >> 8), 2, 0});
}
Bytes colorPacket(const uint8_t key[16], int hue, int saturation, uint16_t target) {
  return commandPacket(key, target, 0, 3, {6, (uint8_t)clampInt(hue, 0, 254), (uint8_t)clampInt(saturation, 0, 254), 2, 0});
}
Bytes clockPacket(const uint8_t key[16], uint32_t unixSeconds, int32_t utcOffsetSeconds) {
  Bytes p = {0x30};
  le32(p, unixSeconds - EPOCH_2000);
  le32(p, (uint32_t)utcOffsetSeconds);
  return queryPacket(key, p);
}
Bytes timezonePacket(const uint8_t key[16], uint32_t unixSeconds, const std::string& posixTZ) {
  Bytes p = {0x29};
  le32(p, unixSeconds - EPOCH_2000);
  for (size_t i = 0; i < 32; i++) p.push_back(i < posixTZ.size() ? (uint8_t)posixTZ[i] : 0);
  return queryPacket(key, p);
}
Bytes statusQueryPacket(const uint8_t key[16]) { return queryPacket(key, {0x0A, 0xFF, 0xFF}); }

Status parseStatus(const uint8_t key[16], const uint8_t* data, size_t length) {
  Status s = {false, 0, false};
  if (length < 17) return s;
  uint8_t type = data[0];
  if (type != 0x80 && type != 0x82 && type != 0x01) return s;
  uint8_t b[16];
  attDecrypt(key, data + 1, b);
  s.valid = true;
  s.address = (uint16_t)(b[0] | (b[1] << 8));
  s.on = (b[12] & 1) != 0;
  return s;
}

std::string hex(const Bytes& bytes) {
  static const char* digits = "0123456789abcdef";
  std::string s;
  for (uint8_t b : bytes) { s += digits[b >> 4]; s += digits[b & 15]; }
  return s;
}

std::string selfTest() {
  // FIPS-197 appendix C.1.
  {
    uint8_t key[16], in[16], out[16], back[16];
    for (int i = 0; i < 16; i++) { key[i] = (uint8_t)i; in[i] = (uint8_t)(i * 0x11); }
    aes::encrypt(key, in, out);
    if (hex(Bytes(out, out + 16)) != "69c4e0d86a7b0430d8cdb78070b4c55a") return "AES encrypt";
    aes::decrypt(key, out, back);
    if (memcmp(back, in, 16) != 0) return "AES decrypt";
  }
  // Public dummy credentials only; expected values from protocol.js (generated by upstream Python).
  uint8_t combined[16], key[16];
  combine("meshname", "meshpass", combined);
  const uint8_t r[8] = {0, 1, 2, 3, 4, 5, 6, 7}, s[8] = {8, 9, 10, 11, 12, 13, 14, 15};
  if (hex(pairPacket(combined, r)) != "0c00010203040506077f4e2d01cbd9af89") return "pair packet";
  sessionKey(combined, r, s, key);
  if (hex(Bytes(key, key + 16)) != "03a53246e89ea73e0a5d38ae66d370d6") return "session key";
  struct { Bytes got; const char* want; const char* what; } checks[] = {
    {powerPacket(key, true, 0xC7B4), "00665bcbf60bf2f3bdc1fcc25cc02694d5", "power on C7B4"},
    {powerPacket(key, false, 0xC7B4), "002b8370754a97bd6d237ae8ac7ca1025f", "power off C7B4"},
    {powerPacket(key, true, 0x0000), "0007d56316f5a019812579b222244cecbe", "power on 0000"},
    {powerPacket(key, false, 0x0000), "00fae8d7a2ce2b9c4a871b43d416491741", "power off 0000"},
    {powerPacket(key, true, 0xFFFF), "0053b9cd569a2d118cf0383637ec7fd4e3", "power on FFFF"},
    {brightnessPacket(key, 2, 0x0000), "00a924c0808a65a525321a7b65393c4647", "brightness 2"},
    {brightnessPacket(key, 128, 0x0000), "00e4ff57e7faa2289f3c7f3659c92a1158", "brightness 128"},
    {brightnessPacket(key, 254, 0x0000), "00085f6889dde08618db891fed09853baa", "brightness 254"},
    {brightnessPacket(key, 128, 0xC7B4), "0006e2606bc6aafa1241690df377715202", "brightness C7B4"},
    {temperaturePacket(key, 153, 0x0000), "00dbecc3f7b654ee875bd9a600b37f9435", "temperature 153"},
    {temperaturePacket(key, 300, 0x0000), "0048532982640b23181c7a8d3ac98f8008", "temperature 300"},
    {temperaturePacket(key, 454, 0x0000), "00439645e05deb8e6478a46b53f67552d5", "temperature 454"},
    {temperaturePacket(key, 300, 0xC7B4), "00ae2df0c4b33896e62754b6b91afae7f7", "temperature C7B4"},
    {colorPacket(key, 0, 254, 0x0000), "00d64b2eba0099284596911251b907a394", "colour 0/254"},
    {colorPacket(key, 127, 200, 0x0000), "00a5951a8483bed18aa724701c28357535", "colour 127/200"},
    {clockPacket(key, EPOCH_2000 + 838000000, 3600), "027e3bb6e32f19d3b13522774521ba94d3", "clock +1 h"},
    {clockPacket(key, EPOCH_2000 + 838000000, -18000), "02a1a06c418c95ddc63dbd52e7088d029f", "clock -5 h"},
    {clockPacket(key, 1793985122, 3600), "02ef7a0fd9e3bd0eec68ed2229d208f36f", "clock (lampsync vector)"},
    {timezonePacket(key, 1793985122, "CET-1CEST,M3.5.0/2,M10.5.0/3"),
     "02622b8c83769b81ee0eba12b77b10103a2c4d332e352e302f322c4d31302e352e302f3300000000", "time zone (app capture)"},
    {statusQueryPacket(key), "02278f3e9880859d7ce4183806fe6aa70d", "status query (lampsync vector)"},
  };
  for (auto& c : checks) if (hex(c.got) != c.want) return c.what;
  // Status round trip: a block encrypted with the key decodes to address and power again.
  {
    uint8_t plain[16] = {0x37, 0x2A}, frame[17];
    plain[12] = 1;
    frame[0] = 0x80;
    attEncrypt(key, plain, frame + 1);
    Status st = parseStatus(key, frame, sizeof frame);
    if (!st.valid || st.address != 0x2A37 || !st.on) return "status decode";
  }
  memset(key, 0, 16); memset(combined, 0, 16);
  return "";
}
}  // namespace awox
