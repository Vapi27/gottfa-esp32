// host_romcrypt_test.cpp — host test for the romcrypt container format + AES-256-CTR round-trip.
// Mirrors src/romcrypt.cpp's crypto verbatim (same mbedtls calls + container layout) so it
// exercises the REAL algorithm/format the ESP uses (mbedtls 2.28 == IDF 4.4). No Arduino/NVS here
// — the device-key plumbing (Preferences/esp_random) is out of scope; this proves the bytes.
// Build & run on the container:
//   g++ -std=c++17 tools/host_romcrypt_test.cpp -lmbedcrypto -o /tmp/rct && /tmp/rct
#include "mbedtls/aes.h"
#include "mbedtls/version.h"
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstddef>

// --- constants copied from romcrypt.h ---
static const int     HDR_SIZE   = 28;
static const int     PLAIN_SIZE = 16384;
static const int     CONT_SIZE  = HDR_SIZE + PLAIN_SIZE;   // 16412
static const uint8_t VERSION    = 1;
static const uint8_t FLAG_FREEPLAY = 0x01;

// --- crc32: VERBATIM from romcrypt.cpp ---
static uint32_t crc32(const uint8_t* d, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) {
    c ^= d[i];
    for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1)));
  }
  return ~c;
}

// --- ctr/encrypt/decrypt: VERBATIM from romcrypt.cpp, with the device key passed in ---
static bool ctr(const uint8_t* key, const uint8_t* iv, const uint8_t* in, uint8_t* out) {
  mbedtls_aes_context ctx; mbedtls_aes_init(&ctx);
  bool ok = (mbedtls_aes_setkey_enc(&ctx, key, 256) == 0);
  if (ok) {
    unsigned char nc[16]; memcpy(nc, iv, 16);
    unsigned char sb[16]; memset(sb, 0, sizeof(sb));
    size_t off = 0;
    ok = (mbedtls_aes_crypt_ctr(&ctx, PLAIN_SIZE, &off, nc, sb, in, out) == 0);
  }
  mbedtls_aes_free(&ctx);
  return ok;
}
static bool encrypt(const uint8_t* key, const uint8_t* plain, uint8_t* container,
                    const uint8_t* iv, uint8_t flags) {
  memcpy(container, "PSRC", 4);
  container[4] = VERSION; container[5] = flags; container[6] = container[7] = 0;
  memcpy(container + 8, iv, 16);
  uint32_t crc = crc32(plain, PLAIN_SIZE);
  container[24] = (uint8_t)crc; container[25] = (uint8_t)(crc >> 8);
  container[26] = (uint8_t)(crc >> 16); container[27] = (uint8_t)(crc >> 24);
  return ctr(key, iv, plain, container + HDR_SIZE);
}
static bool isContainer(const uint8_t* b, size_t len) {
  return b && len == (size_t)CONT_SIZE && memcmp(b, "PSRC", 4) == 0;
}
static bool decrypt(const uint8_t* key, const uint8_t* container, size_t len,
                    uint8_t* plain, uint8_t* outFlags) {
  if (!isContainer(container, len)) return false;
  const uint8_t* iv = container + 8;
  uint32_t crcStored = (uint32_t)container[24] | ((uint32_t)container[25] << 8) |
                       ((uint32_t)container[26] << 16) | ((uint32_t)container[27] << 24);
  if (!ctr(key, iv, container + HDR_SIZE, plain)) return false;
  if (crc32(plain, PLAIN_SIZE) != crcStored) return false;
  if (outFlags) *outFlags = container[5];
  return true;
}

static int fails = 0;
static void check(const char* what, bool ok) {
  printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what); if (!ok) fails++;
}

int main() {
  printf("romcrypt host test (mbedtls %d.%d)\n", MBEDTLS_VERSION_MAJOR, MBEDTLS_VERSION_MINOR);

  // 1. crc32 against the canonical vector crc32("123456789") == 0xCBF43926
  check("crc32 known vector", crc32((const uint8_t*)"123456789", 9) == 0xCBF43926u);

  // build a 16 KB plaintext with a recognizable pattern
  static uint8_t plain[PLAIN_SIZE], cont[CONT_SIZE], back[PLAIN_SIZE];
  for (int i = 0; i < PLAIN_SIZE; i++) plain[i] = (uint8_t)(i * 31 + (i >> 5));

  uint8_t key[32], key2[32], iv[16];
  for (int i = 0; i < 32; i++) { key[i] = (uint8_t)(i + 1); key2[i] = (uint8_t)(0xA0 ^ i); }
  for (int i = 0; i < 16; i++) iv[i] = (uint8_t)(0x10 + i);

  // 2. encrypt -> size, magic, version, flags, IV stored, ciphertext != plaintext
  bool e = encrypt(key, plain, cont, iv, FLAG_FREEPLAY);
  check("encrypt ok", e);
  check("container size 16412", CONT_SIZE == 16412);
  check("magic PSRC", memcmp(cont, "PSRC", 4) == 0);
  check("version byte", cont[4] == VERSION);
  check("flags byte = FP", cont[5] == FLAG_FREEPLAY);
  check("iv stored at +8", memcmp(cont + 8, iv, 16) == 0);
  check("ciphertext differs from plaintext", memcmp(cont + HDR_SIZE, plain, PLAIN_SIZE) != 0);
  check("isContainer accepts", isContainer(cont, CONT_SIZE));

  // 3. decrypt with the right key -> exact plaintext + flags preserved
  uint8_t flags = 0;
  bool d = decrypt(key, cont, CONT_SIZE, back, &flags);
  check("decrypt ok (right key)", d);
  check("round-trip plaintext identical", memcmp(back, plain, PLAIN_SIZE) == 0);
  check("flags preserved", flags == FLAG_FREEPLAY);

  // 4. wrong key -> CRC mismatch -> false (and plaintext not recovered)
  memset(back, 0, sizeof(back));
  bool dw = decrypt(key2, cont, CONT_SIZE, back, nullptr);
  check("decrypt rejects wrong key", !dw);

  // 5. validation: wrong size and wrong magic rejected
  check("isContainer rejects short", !isContainer(cont, CONT_SIZE - 1));
  uint8_t bad[CONT_SIZE]; memcpy(bad, cont, CONT_SIZE); bad[0] = 'X';
  check("isContainer rejects bad magic", !isContainer(bad, CONT_SIZE));
  check("decrypt rejects bad magic", !decrypt(key, bad, CONT_SIZE, back, nullptr));

  // 6. one tampered ciphertext byte -> CRC catches it
  uint8_t tam[CONT_SIZE]; memcpy(tam, cont, CONT_SIZE); tam[HDR_SIZE + 1234] ^= 0xFF;
  check("decrypt catches tampered ciphertext", !decrypt(key, tam, CONT_SIZE, back, nullptr));

  // 7. a different IV yields different ciphertext for the same plaintext (no nonce reuse footgun)
  uint8_t iv2[16]; for (int i = 0; i < 16; i++) iv2[i] = (uint8_t)(0x77 + i);
  static uint8_t cont2[CONT_SIZE];
  encrypt(key, plain, cont2, iv2, 0);
  check("distinct IV -> distinct ciphertext", memcmp(cont + HDR_SIZE, cont2 + HDR_SIZE, PLAIN_SIZE) != 0);
  uint8_t back2[PLAIN_SIZE];
  check("second container also round-trips", decrypt(key, cont2, CONT_SIZE, back2, nullptr) &&
        memcmp(back2, plain, PLAIN_SIZE) == 0);

  printf("%s (%d failure%s)\n", fails ? "TEST FAILED" : "ALL PASS", fails, fails == 1 ? "" : "s");
  return fails ? 1 : 0;
}
