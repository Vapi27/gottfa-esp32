# PSOROM — run the original Gottlieb sound 6502 on the ESP (exact-by-construction sound)

**Idea:** instead of approximating 80B sound with samples + rules, *run the original sound-board
6502 + the game's sound ROM* live on the ESP. Intercept the chip writes: the **DAC** port becomes
audio samples directly (the bulk of 80B sound is DAC), the **control chips (YM2151/AY)** become
PSOWAV sample triggers. Because it IS the original program executing, orchestration
(start / stop / replace / game-over) is **exact, with no per-game rules to verify**.

No new hardware: it runs on the ESP32-S3 (CPU core + ~64 KB sound-ROM + PSRAM cache); the FPGA is
unchanged (it already forwards the 5-bit sound command on the existing link).

## CPU core
Vendored **Fake6502** (`src/fake6502.c`, (c)2011 Mike Chambers, **public domain** — compatible with
our commercial firmware; the GPL fork was deliberately NOT used). It calls `read6502()`/`write6502()`
which `psorom.cpp` provides = the sound-board memory map.

## Sound-board memory maps (authoritative — from PinMAME `gts80s.c`)
- **GTS80S** (base, simplest — *Stage 1*): 1×6502 + **6530 RIOT** + DAC.
  `0000-01FF RAM · 0200-03FF RIOT (port A=DAC, port B=command in, timer+IRQ) · 0400-0FFF ROM ·
  1000-10FF RAM · F800-FFFF ROM`. DAC sample = `((portA<<7)-0x4000)*2`. Command: latch on port B +
  pulse IRQ. **This is also the 80B D-CPU (DAC) core**, so Stage 1 is reused for 80B.
- **GTS80SS** (speech): 1×6502 + **6532 RIOT** + 2×DAC + Votrax (`1000/3000` DA latches, `2000` VS).
- **GTS80BS** (80B, the target): **2×6502** (Y-CPU = YM2151/AY, D-CPU = DAC) + cross-NMI between them
  (`s80bs_cause_dac_nmi`), AY-3-8912 or YM2151. The two CPUs run time-sliced; the Y-CPU's chip
  writes drive YM/AY, the D-CPU streams the DAC. (80B = DAC-dominant per our characterization, so
  the D-CPU/DAC path carries the bulk; the YM is often mono/minor.)

## Staged plan
- **Stage 1 — foundation (DONE, this commit):** vendored PD 6502 + `psorom` GTS80S model (RAM/ROM/
  RIOT map, DAC capture, command/IRQ inject) + `tools/host_psorom_test.cpp`. **Result:** the 6502
  *executes the real ROM* (652.snd: reset vector resolved, ~14 k instructions retire) on host + the
  module builds on esp32s3 + esp32c3. **Limitation:** the Stage-1 6530 is *loose* (timer always
  "expired", minimal port logic) → the real ROM derails into a wait loop and produces no DAC yet.
- **Stage 2 — make it actually play — GTS80S DONE (host):** implemented the **cycle-accurate 6530
  RIOT** (down-counter + /1·/8·/64·/1024 prescaler + timer-IRQ, ports, registers per 6530riot.c) and
  the exact memory map (6530 code ROM `6530sy80.bin` at 0x0C00-0x0FFF + mirror at 0xFC00 for the
  vectors; per-game `.snd` = 4-bit DAC nibbles at 0x0400-0x0BFF; 64 B RAM mirrored over 0x0000-01FF).
  **Result: the real 6530 sound program runs and emits distinct per-command DAC audio** (panthera:
  cmd1 = square, cmd5 = ramp, cmd8/10 = varied tones; ~25-44 k DAC writes/command). Builds host +
  esp32s3 + esp32c3. **Remaining Stage 2:** sample-exact **diff vs PinMAME** (psowavgen render =
  ground truth) to confirm fidelity; then the **80B dual-6502 + YM2151/AY** (time-slice the two CPUs,
  hook chip writes → DAC-direct + YM/AY→PSOWAV triggers).
- **Stage 3 — ESP integration + bench:** run the 6502 on a dedicated RTOS task feeding the existing
  PCM5102A I2S DMA path; wire the FPGA command link → `psorom::command()`; measure real-time CPU
  headroom + WiFi-dropout robustness on the real N16R8 (the only unmeasured PSOROM risk = YM2151
  real-time cost — but per characterization the YM is minor/mono on the tested 80B games).

## Status
Stage 1 + **Stage 2 (GTS80S) DONE**: the original 6530 sound program runs on the vendored 6502 and
produces real, distinct per-command DAC audio (host + ESP builds). Remaining: PinMAME sample-diff to
prove fidelity, the **80B dual-6502 + YM2151/AY** target, and Stage 3 (ESP audio-task + bench).
