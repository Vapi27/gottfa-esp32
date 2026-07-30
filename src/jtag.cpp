#include "jtag.h"
#include "board_config.h"
#include <Arduino.h>
#include "soc/gpio_struct.h"   // direct GPIO regs for the fast XVC shift path

namespace {

// JTAG timing: the device samples TMS/TDI on the TCK rising edge and updates TDO
// on the falling edge. Host sequence per bit:
//   set TMS/TDI (TCK low) -> read TDO -> TCK high (device samples) -> TCK low.
inline uint8_t clockBit(uint8_t tms, uint8_t tdi) {
  digitalWrite(PIN_JTAG_TMS, tms ? HIGH : LOW);
  digitalWrite(PIN_JTAG_TDI, tdi ? HIGH : LOW);
  delayMicroseconds(1);
  uint8_t tdo = (digitalRead(PIN_JTAG_TDO) == HIGH) ? 1 : 0;  // valid before the edge
  digitalWrite(PIN_JTAG_TCK, HIGH);
  delayMicroseconds(1);
  digitalWrite(PIN_JTAG_TCK, LOW);
  delayMicroseconds(1);
  return tdo;
}

// Drive the 3 JTAG outputs, or release everything to Hi-Z so the running FPGA
// TAP is left alone (board has 10K pull-ups on TMS/TDI).
void drive(bool on) {
  pinMode(PIN_JTAG_TDO, INPUT);
  if (on) {
    pinMode(PIN_JTAG_TCK, OUTPUT);
    pinMode(PIN_JTAG_TMS, OUTPUT);
    pinMode(PIN_JTAG_TDI, OUTPUT);
    digitalWrite(PIN_JTAG_TCK, LOW);
    digitalWrite(PIN_JTAG_TMS, HIGH);   // TMS idle high
    digitalWrite(PIN_JTAG_TDI, LOW);
  } else {
    pinMode(PIN_JTAG_TCK, INPUT);
    pinMode(PIN_JTAG_TMS, INPUT);
    pinMode(PIN_JTAG_TDI, INPUT);
  }
}

} // namespace

void jtag::begin() { drive(false); }

uint32_t jtag::readIdcode() {
  drive(true);
  for (int i = 0; i < 6; i++) clockBit(1, 0);  // -> Test-Logic-Reset (loads IDCODE)
  clockBit(0, 0);   // TLR  -> Run-Test/Idle
  clockBit(1, 0);   //      -> Select-DR-Scan
  clockBit(0, 0);   //      -> Capture-DR
  clockBit(0, 0);   //      -> Shift-DR  (IDCODE bit0 now on TDO)

  uint32_t id = 0;
  for (int i = 0; i < 32; i++) {
    uint8_t last = (i == 31) ? 1 : 0;          // last bit: TMS=1 -> Exit1-DR
    id |= (uint32_t)clockBit(last, 0) << i;     // shifted out LSB-first
  }

  clockBit(1, 0);   // Exit1-DR -> Update-DR
  clockBit(0, 0);   //          -> Run-Test/Idle
  drive(false);     // back to Hi-Z
  return id;
}

void jtag::enable(bool on) { drive(on); }

void jtag::reset() {                                  // -> TLR -> Run-Test/Idle
  for (int i = 0; i < 6; i++) clockBit(1, 0);
  clockBit(0, 0);
}

void jtag::runTest(uint32_t n) { for (uint32_t i = 0; i < n; i++) clockBit(0, 0); }

uint32_t jtag::shiftIR(uint32_t ir, int n) {          // RTI -> Shift-IR -> RTI, LSB-first
  clockBit(1, 0); clockBit(1, 0); clockBit(0, 0); clockBit(0, 0);
  uint32_t r = 0;
  for (int i = 0; i < n; i++) r |= (uint32_t)clockBit((i == n - 1) ? 1 : 0, (ir >> i) & 1) << i;
  clockBit(1, 0); clockBit(0, 0);                     // Exit1 -> Update -> RTI
  return r;
}

uint64_t jtag::shiftDR(uint64_t tdi, int n) {         // RTI -> Shift-DR -> RTI, LSB-first
  clockBit(1, 0); clockBit(0, 0); clockBit(0, 0);
  uint64_t r = 0;
  for (int i = 0; i < n; i++) r |= (uint64_t)clockBit((i == n - 1) ? 1 : 0, (uint8_t)((tdi >> i) & 1)) << i;
  clockBit(1, 0); clockBit(0, 0);                     // Exit1 -> Update -> RTI
  return r;
}

// XVC bulk shift: TMS/TDI/TDO are LSB-first byte vectors (bit i lives in
// byte[i>>3] at mask (1<<(i&7))) — the exact XVC layout. TDO is sampled before
// each rising edge. No enable() here: the XVC server drives enable(true)/
// enable(false) around the whole session.
//
// FAST PATH: direct GPIO register access (out_w1ts/out_w1tc/in), no delays.
// All 4 TAP pins are < GPIO32 (TCK=4 TMS=5 TDI=6 TDO=7) => single low bank.
// Effective TCK ~2 MHz (vs ~250 kHz for the delayMicroseconds clockBit) — the
// Cyclone 10 TAP is rated far above that, and TDO (updated on the falling
// edge) has hundreds of ns to settle before we sample it next cycle. The slow
// clockBit stays for the scalar helpers (IDCODE etc.) — negligible traffic.
// ~20 ns per iteration: calibrated settle for clean edges on flying leads.
// 4 MHz (bare nops) desynced the TAP (SVF TDO-compare fails at line B79);
// ~150 ns half-periods (~1.7 MHz) are solid and still ~7x the old clockBit.
static inline void settle(uint32_t n) {
  for (volatile uint32_t i = 0; i < n; i++) { __asm__ __volatile__("nop"); }
}

// The GPIO peripheral struct is not the same shape on every ESP32 family: on the
// S3 `GPIO.out_w1ts` is a plain uint32_t, on the C3 it is an anonymous union and
// the value lives in `.val`. Writing the bare integer therefore fails to compile
// on the C3 -- one of the three errors that made `pio run -e esp32c3` broken.
// These macros keep the S3 expressions EXACTLY as they were (this is the proven
// WiFi-XVC bitstream path; its ~150 ns settle timing was calibrated on hardware
// and must not change) and only add the `.val` spelling for the C3.
#if defined(CONFIG_IDF_TARGET_ESP32C3) || defined(CONFIG_IDF_TARGET_ESP32C6) || \
    defined(CONFIG_IDF_TARGET_ESP32H2)
  #define JTAG_W1TS(m)  (GPIO.out_w1ts.val = (m))
  #define JTAG_W1TC(m)  (GPIO.out_w1tc.val = (m))
  #define JTAG_IN()     (GPIO.in.val)
#else
  #define JTAG_W1TS(m)  (GPIO.out_w1ts = (m))
  #define JTAG_W1TC(m)  (GPIO.out_w1tc = (m))
  #define JTAG_IN()     (GPIO.in)
#endif

void jtag::shift(uint32_t nbits, const uint8_t* tms, const uint8_t* tdi, uint8_t* tdo) {
  const uint32_t TCK = 1u << PIN_JTAG_TCK;
  const uint32_t TMS = 1u << PIN_JTAG_TMS;
  const uint32_t TDI = 1u << PIN_JTAG_TDI;
  for (uint32_t i = 0; i < nbits; i++) {
    uint8_t m = (tms[i >> 3] >> (i & 7)) & 1;
    uint8_t d = (tdi[i >> 3] >> (i & 7)) & 1;
    uint32_t setm = (m ? TMS : 0u) | (d ? TDI : 0u);
    uint32_t clrm = (m ? 0u : TMS) | (d ? 0u : TDI);
    JTAG_W1TS(setm);                            // drive TMS/TDI (TCK low)
    JTAG_W1TC(clrm);
    settle(7);                                  // ~150 ns: lines + TDO settle
    uint8_t o = (JTAG_IN() >> PIN_JTAG_TDO) & 1; // sample TDO before the rising edge
    JTAG_W1TS(TCK);                             // rising edge: device samples TMS/TDI
    settle(7);                                  // ~150 ns TCK high
    JTAG_W1TC(TCK);                             // falling edge: device updates TDO
    if (o) tdo[i >> 3] |=  (uint8_t)(1u << (i & 7));
    else   tdo[i >> 3] &= (uint8_t)~(1u << (i & 7));
  }
}

const char* jtag::idcodeName(uint32_t id) {
  switch (id) {
    case 0x020F10DDUL: return "Cyclone 10 LP 10CL006/010 (Cyclone IV E EP4CE6/10 die)";
    case 0x020F30DDUL: return "Cyclone 10 LP 10CL025 (EP4CE22 die)";
    default:           return "unknown / check P5 wiring";
  }
}
