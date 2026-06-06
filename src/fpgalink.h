// fpgalink.h — the single 8N1 UART link from the FPGA, tapped on the Debug pin
// (PIN_FPGA_LINK, = FPGA PIN_11 / K2, right next to the FPGA). One self-describing
// byte (see sound_link.vhd): 0xF0|diag (mode token), 0x80|sound[4:0], 0x40|game[5:0].
// Diag and gameplay sound never overlap, so one wire carries both. Compiled on both
// tiers: the mode token drives diag bus arbitration (S3 + C3); the sound/game bytes
// drive the WAV player (S3 only).
// (C) 2026 Valere Pilpil / Pstore. Original implementation.
#pragma once
namespace fpgalink {
  void begin();        // open the UART (RX only) on PIN_FPGA_LINK
  void poll();         // drain the FIFO: update diag mode (+ S3: play sounds). Call from loop().
  bool diagActive();   // last diag-mode token (0xF1 -> true, 0xF0 -> false)
  bool gameRunning();  // last game-state token (0xF3 -> true, 0xF2 -> false) — tournament timer
}
