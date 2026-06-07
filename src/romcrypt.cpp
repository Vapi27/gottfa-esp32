// romcrypt.cpp — see romcrypt.h. Device-bound AES-256-CTR for the /roms ROM-image store.
// (C) 2026 Valere Pilpil / Pstore. Original implementation.
#include "romcrypt.h"
#include <string.h>

#ifndef BOARD_C3
#include <Arduino.h>
#include <Preferences.h>
#include <esp_random.h>
#include "mbedtls/aes.h"

namespace romcrypt {

static uint8_t  g_key[32];
static uint32_t g_keyId = 0;
static bool     g_have  = false;

// CRC-32 (IEEE 802.3, poly 0xEDB88320) — plaintext integrity / wrong-key detection.
static uint32_t crc32(const uint8_t* d, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) {
    c ^= d[i];
    for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1)));
  }
  return ~c;
}

bool begin() {
  if (g_have) return true;
  Preferences p;
  if (!p.begin("romcrypt", false)) { log_e("[romcrypt] NVS open failed"); return false; }
  size_t n = p.getBytes("k", g_key, sizeof(g_key));
  if (n != sizeof(g_key)) {
    esp_fill_random(g_key, sizeof(g_key));     // hardware RNG
    g_keyId = esp_random();                    // independent random fingerprint (no key leakage)
    p.putBytes("k", g_key, sizeof(g_key));
    p.putUInt("id", g_keyId);
    log_i("[romcrypt] new device key generated, id=%08X", (unsigned)g_keyId);
  } else {
    g_keyId = p.getUInt("id", 0);
    if (!g_keyId) { g_keyId = esp_random(); p.putUInt("id", g_keyId); }
    log_i("[romcrypt] device key loaded, id=%08X", (unsigned)g_keyId);
  }
  p.end();
  g_have = true;
  return true;
}

bool     available() { return g_have; }
uint32_t keyId()     { return g_keyId; }

// AES-256-CTR over PLAIN_SIZE bytes. CTR is symmetric (same call both ways). iv is copied (the
// call mutates its counter), so the header keeps the original iv intact.
static bool ctr(const uint8_t* iv, const uint8_t* in, uint8_t* out) {
  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  bool ok = (mbedtls_aes_setkey_enc(&ctx, g_key, 256) == 0);
  if (ok) {
    unsigned char nc[16]; memcpy(nc, iv, 16);
    unsigned char sb[16]; memset(sb, 0, sizeof(sb));
    size_t off = 0;
    ok = (mbedtls_aes_crypt_ctr(&ctx, PLAIN_SIZE, &off, nc, sb, in, out) == 0);
  }
  mbedtls_aes_free(&ctx);
  return ok;
}

bool encrypt(const uint8_t* plain, uint8_t* container, uint8_t flags) {
  if (!g_have || !plain || !container) return false;
  memcpy(container, "PSRC", 4);
  container[4] = VERSION;
  container[5] = flags;
  container[6] = container[7] = 0;
  uint8_t iv[16];
  esp_fill_random(iv, sizeof(iv));
  memcpy(container + 8, iv, 16);
  uint32_t crc = crc32(plain, PLAIN_SIZE);
  container[24] = (uint8_t)(crc);        container[25] = (uint8_t)(crc >> 8);
  container[26] = (uint8_t)(crc >> 16);  container[27] = (uint8_t)(crc >> 24);
  return ctr(iv, plain, container + HDR_SIZE);
}

bool isContainer(const uint8_t* buf, size_t len) {
  return buf && len == (size_t)CONT_SIZE && memcmp(buf, "PSRC", 4) == 0;
}

bool decrypt(const uint8_t* container, size_t len, uint8_t* plain, uint8_t* outFlags) {
  if (!g_have || !plain || !isContainer(container, len)) return false;
  const uint8_t* iv = container + 8;
  uint32_t crcStored = (uint32_t)container[24] | ((uint32_t)container[25] << 8) |
                       ((uint32_t)container[26] << 16) | ((uint32_t)container[27] << 24);
  if (!ctr(iv, container + HDR_SIZE, plain)) return false;
  if (crc32(plain, PLAIN_SIZE) != crcStored) return false;   // wrong key or corruption
  if (outFlags) *outFlags = container[5];
  return true;
}

} // namespace romcrypt

#else   // BOARD_C3 — no /roms store, no crypto

namespace romcrypt {
bool     begin()     { return false; }
bool     available() { return false; }
uint32_t keyId()     { return 0; }
bool encrypt(const uint8_t*, uint8_t*, uint8_t) { return false; }
bool isContainer(const uint8_t*, size_t) { return false; }
bool decrypt(const uint8_t*, size_t, uint8_t*, uint8_t*) { return false; }
} // namespace romcrypt

#endif
