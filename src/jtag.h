#pragma once
#include <Arduino.h>

// Minimal bit-bang JTAG master for the Cyclone 10 module (single-device chain).
// v0.1: TAP navigation + IDCODE read — the safe bring-up test. Reading IDCODE is
// non-intrusive: a configured, running FPGA is NOT disturbed (only the TAP
// controller is reset, not the fabric/configuration).
// Bitstream programming (an SVF/JAM vector player) comes in a later step.
namespace jtag {
  void        begin();              // pins -> safe Hi-Z idle
  uint32_t    readIdcode();         // TAP reset -> Shift-DR -> 32 bits; restores Hi-Z
  const char* idcodeName(uint32_t id);

  // --- low-level TAP primitives (call enable(true) first, enable(false) after).
  //     Building blocks for the OTA bitstream player (SVF/JAM or direct config). ---
  void        enable(bool on);                    // drive / release (Hi-Z) the JTAG pins
  void        reset();                            // -> Test-Logic-Reset -> Run-Test/Idle
  uint32_t    shiftIR(uint32_t ir, int nbits);    // Cyclone IV/10 IR length = 10
  uint64_t    shiftDR(uint64_t tdi, int nbits);   // scalar (<=64b); stream version TBD for bitstream
  void        runTest(uint32_t clocks);           // idle clocks in Run-Test/Idle
}
