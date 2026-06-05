# GottFA80+ sound engine — design (our own, from scratch)

We recode the WAV-sound concept from scratch (bontango's `pwavplayer` is CC BY-NC-SA
= non-commercial, so unusable in a product). Goals: **optimised for pinball use**,
**stereo**, able to drive a **+12V** amp from the machine rail, and **interoperable
with bontango's ecosystem** where it's free to be.

## Engine (software, on the ESP32-S3)
- **`wavmix`** — polyphonic **stereo** 16-bit PCM mixer. 8 voices, interleaved L/R,
  per-voice gain (255 = unity), saturating sum. Mono sources duplicate to L/R;
  stereo sources (e.g. **Arena**) pass L/R through. ✅ host-unit-tested.
- **`wavfile`** — RIFF/WAVE PCM16 parser (mono + stereo). ✅ tested.
- **(next) `wavplayer`** — SD WAV streaming → voices + I2S output task + a
  `play(sound#)` API and the per-game command→WAV map.
- Optimised for our case: **fixed project sample rate** (WAV sets pre-converted →
  no runtime resampling), **role-based voices** (1 music + N effects + speech),
  low-latency trigger.

## Audio output hardware (stereo + uses the pinball +12V)
A little 5 V mono I2S amp (MAX98357A) is too weak for a pinball. Recommended chain:

```
ESP32-S3  --I2S(BCK/LRCK/DIN)-->  PCM5102A  --line L/R-->  PAM8610 (12V)  --> 2 speakers
            (3.3 V)               stereo DAC               2x ~10-15 W Class-D
```
- **PCM5102A** — I2S stereo DAC, line-level out, 3.3 V, ~€3, excellent quality.
- **PAM8610** — 2×~10-15 W stereo Class-D, supply **7–15 V → perfect on the +12 V**
  pinball rail, analog differential in. (More headroom? **TPA3116D2**, 2×50 W, up to
  26 V.) The +12 V drives the amp; the ESP + DAC stay on 3.3/5 V.
- The ESP needs its **own SD** for the WAVs (the board's SD is on the FPGA's SPI bus).

## Bontango compatibility (kept where it's free)
- **Same sound-command interface** (RE'd from PinMAME `gts80s.c`/`gts80.c`): the
  Gottlieb 80/80A/80B command is **5-bit**, already captured by GottFA80 as
  `Sound_Meta` → so his FPGA can drive our player unchanged.
- **Same SD layout convention** as pwavplayer (`<sdroot>/<theme>/` per game) so WAV
  sample sets are **interchangeable** between the two.
- Optional: accept his serial (`p <id>`) and WiFi-REST trigger formats too.
- Our engine code is original/ours (commercial-OK); only the *conventions* are shared.

## FPGA → ESP sound command (the one wiring point)
During a game the FPGA must hand the ESP the 5-bit `Sound_Meta`. `Debug` is taken by
the diag handshake, so this needs either a **dedicated soldered FPGA pin** (1-wire
serial `p <n>`, reusing the FPGA's existing UART style) or a multiplex of `Debug`.
To settle once hardware is in hand.

## Status
Core mixer + parser: **done, host-tested, builds for esp32s3** (on the container CI
box). Next: `wavplayer` (SD + I2S + command map), then hardware bring-up.
