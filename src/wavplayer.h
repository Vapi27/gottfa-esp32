// wavplayer.h — SD-streamed polyphonic WAV playback on an MCP4921 12-bit SPI DAC
// (mono, -> on-board TDA7267 +12 V amp) for the GottFA80+ sound engine. Sound is an
// ESP32-S3 "sound tier" feature (not on C3).
//
// Architecture (race-free): play()/setTheme() may be called from any task; they only
// enqueue requests. The mix task (core 1) owns the mixer + SD: it drains the queue,
// streams WAVs into mixer voices, mixes, down-mixes to mono 12-bit, and fills a
// lock-free ring buffer. A dac task (core 0) drains the ring at the sample rate and
// clocks each sample to the MCP4921 over a dedicated SPI bus. So the mixer/SD are
// owned by one task and the ring is single-producer/single-consumer.
// (C) 2026 Valere Pilpil / Pstore. Original implementation.
#pragma once
#include <stdint.h>
#ifndef BOARD_C3
namespace wavplayer {
  bool begin();                       // mount SD, init the MCP4921 DAC, start the tasks
  void setTheme(const char* theme);   // per-game folder under the SD root (e.g. "747")
  bool play(int soundId);             // play "<theme>/<id>.wav" on a free voice (unconditional — web/diag test)
  bool playLive(int soundId);         // FPGA live path: applies hybrid routing (skips GOSOF80-handled cmds)
  bool soundHybrid();                 // true if sndmode=hybrid (config.txt) — GOSOF80 does part of the sound
  void stopAll();
  void testTone(int ms);              // HW test: 440 Hz sine straight to I2S (no SD/WAV)
  bool ready();
  // --- status for the web UI (cached; safe to read from another task for display) ---
  const char* curTheme();             // currently loaded game/theme folder
  uint32_t    soundMask();            // bit i set => sound id i (0..31) exists in the loaded set
  uint32_t    loopMask();             // bit i set => sound i loops (attr 'l')
  uint32_t    voiceMask();            // bit i set => sound i is on the voice bus (attr 'v')
  int         soundCount();           // number of sounds in the loaded set
  int         soundList(uint16_t* out, int max);  // present sounds: out[i]=(id<<2)|loop|voice<<1; count (ids 0..95)
  int         themeCount();           // game folders found on the SD root (cached at begin)
  const char* themeName(int idx);     // name of cached theme idx, or "" if out of range
  // game-select: /games.txt maps the FPGA game number (GottFA80_PLuS gamelist No) -> romname/folder
  void        selectGame(int gameNo); // load the set for FPGA game No (via games.txt)
  const char* gameRom(int gameNo);    // romname mapped to a game No, or "" if unmapped
  int         lastSound();            // last sound id played (-1 = none) — for the OLED/status
}
#endif
