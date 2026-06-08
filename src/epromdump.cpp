// epromdump.cpp — see epromdump.h. Bit-banged 74HC595 (address+/CE+/OE) + 74HC165 (data) reader.
// (C) 2026 Valere Pilpil / Pstore. Original implementation.
#include "epromdump.h"
#include "board_config.h"

#ifndef BOARD_C3
#include <Arduino.h>
#include <SD.h>
#include <stdlib.h>

namespace epromdump {

static bool g_ready = false;

// Shift a 16-bit word into the 595 chain (MSB first) and latch it.
// bit layout: [15]=spare(1) [14]=/OE [13]=/CE [12..0]=A12..A0
static void setBus(uint16_t w) {
  for (int i = 15; i >= 0; i--) {
    digitalWrite(PIN_EPR_SER, (w >> i) & 1);
    digitalWrite(PIN_EPR_SCLK, HIGH); digitalWrite(PIN_EPR_SCLK, LOW);
  }
  digitalWrite(PIN_EPR_RCLK, HIGH); digitalWrite(PIN_EPR_RCLK, LOW);   // latch -> outputs
}

// Build the bus word for `addr` with active-low /CE,/OE asserted (true) or deasserted (false).
static uint16_t busWord(uint32_t addr, bool ceAssert, bool oeAssert) {
  uint16_t w = (uint16_t)(addr & 0x1FFF);             // A0..A12
  if (!ceAssert) w |= (1u << 13);                     // /CE high = deselected
  if (!oeAssert) w |= (1u << 14);                     // /OE high = outputs off
  w |= (1u << 15);                                    // spare held high
  return w;
}

// 2332 mask ROM (Gottlieb U2/U3): bottom-justified in the 2764-wired ZIF, the three quirky pins
// remap onto controllable 595 outputs -> A11=Q13 (socket20), CS1=Q14 (socket22), CS2=Q11 (socket23).
// So reading is firmware-only (no adapter / no 7404): drive A0..A11 + the two chip-selects at the
// Gottlieb mask polarity. csA/csB = the LEVEL to drive on CS1(pin20)/CS2(pin21) to enable the read.
static uint16_t busWord2332(uint32_t addr, bool csA, bool csB) {
  uint16_t w = (uint16_t)(addr & 0x07FF);             // A0..A10 -> Q0..Q10
  if ((addr >> 11) & 1) w |= (1u << 13);              // A11 -> Q13 (socket pin 20)
  if (csA)              w |= (1u << 14);              // CS1 -> Q14 (socket pin 22)
  if (csB)              w |= (1u << 11);              // CS2 -> Q11 (socket pin 23)
  w |= (1u << 15);                                    // spare held high
  return w;
}

// Parallel-load the 165 then shift the byte in. Wiring: EPROM D0->165 A ... D7->165 H, so QH=D7
// first; reading MSB-first reconstructs the byte.
static uint8_t readData() {
  digitalWrite(PIN_EPR_LOAD, LOW);                    // capture parallel inputs
  digitalWrite(PIN_EPR_LOAD, HIGH);
  uint8_t v = 0;
  for (int i = 7; i >= 0; i--) {
    if (digitalRead(PIN_EPR_QH)) v |= (1u << i);
    digitalWrite(PIN_EPR_SCLK, HIGH); digitalWrite(PIN_EPR_SCLK, LOW);
  }
  return v;
}

void begin() {
#if defined(EPROM_READER_ENABLE) && EPROM_READER_ENABLE
  pinMode(PIN_EPR_SER, OUTPUT);  pinMode(PIN_EPR_SCLK, OUTPUT); pinMode(PIN_EPR_RCLK, OUTPUT);
  pinMode(PIN_EPR_LOAD, OUTPUT); pinMode(PIN_EPR_QH, INPUT);
  digitalWrite(PIN_EPR_SCLK, LOW); digitalWrite(PIN_EPR_RCLK, LOW); digitalWrite(PIN_EPR_LOAD, HIGH);
  setBus(busWord(0, false, false));                   // idle: deselected
  g_ready = true;
  log_i("[eprom] reader ready (595/165, 5 GPIO)");
#endif
}

bool available() { return g_ready; }

size_t readChip(Type t, uint8_t* buf, size_t bufLen) {
  if (!g_ready || !buf) return 0;
  size_t n = sizeOf(t);
  if (bufLen < n) return 0;
  const bool m2332 = is2332(t);
  const bool csA = true;                              // CS1 (pin20) = HIGH to read (U2 and U3)
  const bool csB = (t == T2332_U3);                   // CS2 (pin21): U3 = HIGH, U2 = LOW
  for (size_t a = 0; a < n; a++) {
    if (m2332) setBus(busWord2332(a, csA, csB));
    else       setBus(busWord(a, true, true));        // EPROM: address + /CE=0 + /OE=0
    delayMicroseconds(2);                             // access time + settle
    buf[a] = readData();
  }
  setBus(m2332 ? busWord2332(0, false, false) : busWord(0, false, false));   // deselect
  return n;
}

bool dumpToSD(Type t, const char* path) {
  if (!g_ready || !path) return false;
  size_t n = sizeOf(t);
  uint8_t* buf = (uint8_t*)malloc(n);
  if (!buf) return false;
  bool ok = (readChip(t, buf, n) == n);
  if (ok) {
    if (!SD.exists("/dumps")) SD.mkdir("/dumps");
    SD.remove(path);
    File f = SD.open(path, FILE_WRITE);
    ok = (bool)f;
    if (ok) { ok = (f.write(buf, n) == n); f.close(); }
    log_i("[eprom] dump %s -> %s (%u B)", ok ? "OK" : "FAIL", path, (unsigned)n);
  }
  free(buf);
  return ok;
}

} // namespace epromdump

#else   // BOARD_C3 — no SD tier

namespace epromdump {
void   begin() {}
bool   available() { return false; }
size_t readChip(Type, uint8_t*, size_t) { return 0; }
bool   dumpToSD(Type, const char*) { return false; }
} // namespace epromdump

#endif
