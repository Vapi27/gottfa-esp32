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
#ifndef BOARD_C3
namespace wavplayer {
  bool begin();                       // mount SD, init the MCP4921 DAC, start the tasks
  void setTheme(const char* theme);   // per-game folder under the SD root (e.g. "747")
  bool play(int soundId);             // play "<theme>/<id>.wav" on a free voice
  void stopAll();
  bool ready();
}
#endif
