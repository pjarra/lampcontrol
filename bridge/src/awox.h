// AwoX non-Telink mesh protocol: C++ port of ../../protocol.js (same frames, same self-test
// vectors). Only what the bridge needs: login, light commands, clock, status query and notifications.
// Protocol adapted from awox-mesh (MIT), like protocol.js; see ../../LICENSE.txt.
#pragma once
#include <stdint.h>
#include <string>
#include <vector>

namespace awox {
using Bytes = std::vector<uint8_t>;

// Brightness 2..254 on the wire; temperature in mired 153 (cold) .. 454 (warm); hue/saturation 0..254.
constexpr int BRIGHTNESS_MIN = 2, BRIGHTNESS_MAX = 254, MIRED_MIN = 153, MIRED_MAX = 454;
constexpr uint16_t BROADCAST = 0xFFFF;
constexpr uint32_t EPOCH_2000 = 946684800;

// name XOR password, each zero-padded to 16 bytes. false if either is not 1..16 bytes.
bool combine(const std::string& name, const std::string& password, uint8_t out[16]);
// Vendor AES: AES-128-ECB with key, input and output byte-reversed.
void attEncrypt(const uint8_t key[16], const uint8_t value[16], uint8_t out[16]);
void attDecrypt(const uint8_t key[16], const uint8_t value[16], uint8_t out[16]);
uint8_t crc8(const uint8_t* data, size_t length);

// Login: 0x0C + client random + first 8 bytes of attEncrypt(random, combined).
Bytes pairPacket(const uint8_t combined[16], const uint8_t random[8]);
void sessionKey(const uint8_t combined[16], const uint8_t random[8], const uint8_t response[8], uint8_t key[16]);

// Light commands (wire flag 0x00) to a mesh address (0x0000 = the connected lamp, 0xFFFF = all).
bool addressOK(uint16_t target);
Bytes powerPacket(const uint8_t key[16], bool on, uint16_t target);
Bytes brightnessPacket(const uint8_t key[16], int value, uint16_t target);
Bytes temperaturePacket(const uint8_t key[16], int mired, uint16_t target);
Bytes colorPacket(const uint8_t key[16], int hue, int saturation, uint16_t target);

// Query frames (wire flag 0x02): only the connected lamp takes clock and time zone.
Bytes clockPacket(const uint8_t key[16], uint32_t unixSeconds, int32_t utcOffsetSeconds);
Bytes timezonePacket(const uint8_t key[16], uint32_t unixSeconds, const std::string& posixTZ);
Bytes statusQueryPacket(const uint8_t key[16]);

// Status notification (characteristic …1911): type byte + 16 encrypted bytes. Power is bit 0 of
// byte 12 of the decrypted block for types 0x80/0x82/0x01 (as tools/lampsync.swift, verified on macOS).
struct Status { bool valid; uint16_t address; bool on; };
Status parseStatus(const uint8_t key[16], const uint8_t* data, size_t length);

// Same checks as protocol.js selfTest(). Returns the name of the first failing check, or "" if all pass.
std::string selfTest();
std::string hex(const Bytes& bytes);
}  // namespace awox
