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
- **GTS80BS** (80B, the target): **2×6502** (Y-CPU = YM2151/AY+speech, D-CPU = DAC) + cross-NMI,
  AY-3-8912 (Gen1/2) or YM2151 (Gen3). Maps (Gen3, from gts80s.c):
  - **D-CPU** (`GTS80BS3_dreadmem/dwritemem`) — SIMPLE, reuses our engine: `0000-07FF RAM ·
    4000 = soundlatch_r (command) · 8000-FFFF ROM (drom1.snd) · 8001 = s80bs_dac_data_w (DAC data)`.
    DAC out = `dac_volume * dac_data` (a multiplying DAC, unlike GTS80S's port-A formula).
  - **Y-CPU** (`GTS80BS3_yreadmem/ywritemem`): `0000-07FF RAM · 6800 = soundlatch_r · 8000-FFFF ROM
    (yrom1.snd)`; writes hit YM2151 (`s80bs_ym2151_w`, reg/data port via sound_control bit7),
    `nmi_rate`, `sound_control`, `cause_dac_nmi` (→ D-CPU NMI = the DAC sample clock).
  - **Flow:** MPU command → `s80bs_sh_w` → soundlatch + IRQ(HOLD) to BOTH CPUs. The Y-CPU's
    programmable NMI timer (`nmi_rate`/`nmi_enable`) + `cause_dac_nmi` drive the D-CPU → it streams
    DAC samples. (Bone Busters adds a 3rd CPU = 2nd DAC.)
  - **Needs:** dual-6502 **context switching** (our PD core uses globals → save/restore {pc,sp,a,x,y,
    status,clockticks} at instruction boundaries + a "current CPU" pointer for the memory hooks),
    time-sliced like MAME's INTERLEAVE(50); the simple D-CPU map (DAC); the Y-CPU map; the NMI timer
    + cross-NMI; and **YM2151** (the only hard chip — but 80B is DAC-dominant per our characterization,
    so a stubbed/log YM still yields the bulk of the audio via the D-CPU; YM→PSOWAV triggers later).

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
- **GTS80S DONE**: 6530 program runs → distinct per-command DAC audio.
- **80B Gen3 (GTS80BS3) dual-6502 DONE (host + ESP builds)**: both 6502s run time-sliced (the global
  PD core is context-switched), the command→soundlatch+IRQ + Y-NMI-timer + cross-NMI chain works, and
  **the per-command chip behaviour matches our PinMAME characterization exactly** — badgirls: cmd 22 →
  13 315 DAC writes + 1338 YM (DAC music), cmd 26 → 12 584 DAC (music), cmd 3 → 0 DAC + 3066 YM (pure
  YM effect). So the D-CPU streams the DAC **directly** (the bulk of 80B sound) and the Y-CPU's YM2151
  register writes are **captured** (→ map to PSOWAV sample triggers next). The PSOROM thesis is proven
  on the real target.
- **Remaining:** map captured YM/AY writes → PSOWAV triggers (or light YM synth); sample-exact DAC
  diff vs PinMAME (scaling/timing fidelity); Gen1/Gen2 (AY) maps (trivial vs Gen3); **Stage 3** = ESP
  RTOS audio-task feeding PCM5102A + FPGA command link → `psorom::command()` + real-time bench.
