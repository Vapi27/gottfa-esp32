// sndroute.h — per-game sound routing for the HYBRID FPGA build (GOSOF80 synth + ESP voice/80B).
//
// In a HYBRID build the FPGA's GOSOF80 synthesizes the sounds it supports (80/80A + the 3 early
// 80B), and the ESP/PSOWAV plays only the speech commands and the complex-80B games that GOSOF80
// can't emulate. This table tells the ESP, per game + per sound command, whether GOSOF80 already
// produces it (skip) or the ESP must play it.
//
// SOURCE OF TRUTH: GOSOF80.vhd's per-game `SB_type` + `speech_ctrl` table (lib_common/GOSOF80.vhd).
// REGENERATE if Ralf changes that table. Classes:
//   G (gosof-synth) : GOSOF80 plays everything  -> ESP silent.
//   H (hybride)     : GOSOF80 plays non-speech  -> ESP plays the speech commands (speech_ctrl bit=0).
//   E (esp-full)    : `is_special` / complex 80B -> GOSOF80 can't, ESP plays everything.
// NOTE: only consulted in sndmode=hybrid. In sndmode=full (all-ESP build, no GOSOF80) the ESP
// ignores this and plays everything (the safe default / current behaviour).
// (C) 2026 Valere Pilpil / Pstore. Original implementation.
#pragma once
namespace sndroute {
  // Should the ESP play sound command `cmd` (0..31) for FPGA game `gameNo` (0..62), in hybrid mode?
  // Unknown game -> true (play, safe). G -> false. H -> true only for speech commands. E -> true.
  bool espPlays(int gameNo, int cmd);
}
