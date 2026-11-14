// AES-128, single block (ECB), encrypt and decrypt. Portable C++ so the same code runs on the
// ESP32 and in the host tests; checked against the FIPS-197 vector in the self-test.
#pragma once
#include <stdint.h>

namespace aes {
void encrypt(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);
void decrypt(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);
}
