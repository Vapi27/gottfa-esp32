#include "jtag.h"
#include "board_config.h"

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

const char* jtag::idcodeName(uint32_t id) {
  switch (id) {
    case 0x020F10DDUL: return "Cyclone 10 LP 10CL006/010 (Cyclone IV E EP4CE6/10 die)";
    case 0x020F30DDUL: return "Cyclone 10 LP 10CL025 (EP4CE22 die)";
    default:           return "unknown / check P5 wiring";
  }
}
