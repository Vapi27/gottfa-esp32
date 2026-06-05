// wavplayer.h — SD-streamed polyphonic WAV playback on I2S (PCM5102A) for the
// GottFA80+ sound engine. Sound is an ESP32-S3 "sound tier" feature (not on C3).
//
// Architecture (race-free): play()/setTheme() may be called from any task; they
// only enqueue requests. A single audio task (core 0) owns the mixer + SD + I2S:
// it drains the queue, opens/streams WAVs into mixer voices, mixes stereo, and
// writes to I2S. So the mixer/SD are never touched cross-core.
// (C) 2026 Valere Pilpil / Pstore. Original implementation.
#pragma once
#ifndef BOARD_C3
namespace wavplayer {
  bool begin();                       // mount SD, init I2S, start the audio task
  void setTheme(const char* theme);   // per-game folder under the SD root (e.g. "747")
  bool play(int soundId);             // play "<theme>/<id>.wav" on a free voice
  void stopAll();
  bool ready();
}
#endif
