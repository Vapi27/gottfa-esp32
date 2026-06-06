# PSOWAV — Pstore Sound On WAV

PSOWAV is **our own** WAV-based sound engine for the GottFA80+ / Pstore Pinball Platform
(Gottlieb System 80 / 80A / 80B). It is **not** bontango's `GOSOF80` (FPGA-internal sound)
nor `pwavplayer` (CC BY-NC-SA, non-commercial): PSOWAV is original code (commercial-OK).
The on-SD *layout* is kept compatible with the pwavplayer format **for interchange only** —
the parser is clean-room and our own generator produces the sets.

The idea: instead of emulating the sound hardware live on the ESP, we **render each game's
sounds once, offline, with PinMAME (= bit-exact to the real board)** and store them as WAVs.
The ESP just plays the right WAV when the FPGA sends a sound command. Simple, bit-exact,
and the existing ESP player runs it with ~0 extra CPU.

> Why WAV and not live chip emulation? On a real cabinet the sound exits a small cone
> speaker + the board's analog amp, which band-passes everything (~150 Hz–7 kHz). We
> measured: through that speaker, a stored PinMAME render and a live-emulated render are
> **indistinguishable** (AC-RMS within 0.06 %). So the per-chip timbre nuances a live
> emulator would add are inaudible on the target — WAV playback is the pragmatic win.

## Pipeline

```
  OFFLINE (build host / rig)                         ON THE PINBALL (ESP32-S3)
  ──────────────────────────                         ─────────────────────────
  PinMAME ROM  ─►  psowavgen  ─►  PSOWAV set   ──SD─► wavset (index)  ─► wavplayer
  (real board     (libpinmame    /<game>/*.wav        + config.txt       ├─ wavmix (8 voices)
   sound, exact)   headless)     bit-exact WAVs                          └─ MCP4921 DAC ─► TDA7267 ─► speaker
                                                       ▲
                              FPGA sends 5-bit sound cmd over Debug/K2 ──► fpgalink ──► wavplayer::play(id)
```

## 1. `psowavgen` — the offline generator (`lisy_5_28/src/libpinmame/psowavgen.cpp`)

Boots a Gottlieb game headless in **libpinmame**, and for each sound command `0..N`:
- injects it straight into the sound board (`PinmamePlaySound` → `sndbrd_data_w`),
- captures the **bit-exact** audio the real board would make (mono, 44.1 kHz),
- auto-detects loop period, trims silence, classifies the sound type,
- writes `NNNN[-attr]-VVV-snd.wav` (the PSOWAV set convention, below).

Build & run (on the rig; needs cmake + zlib, no SDL):
```
g++ -O2 -std=c++14 -Isrc/libpinmame src/libpinmame/psowavgen.cpp -Lbuild -lpinmame -o build/psowavgen
LD_LIBRARY_PATH=build ./build/psowavgen <game> <outdir> /path/to/vpm/ [ncmds=32] [maxsecs=30]
```

Type classification → attributes:
| type | meaning | attr |
|---|---|---|
| `oneshot` | finite (plays then stops) | none |
| `tone` | sustained, short stable cycle | `l` (loop) |
| `music` | sustained, evolving phrase | `l` (loop) |
| `speech` | speech chip is the sole source | `v` (voice bus) |

Bonus: each command also gets a `NNNN.score.txt` (the chip register-write log) for the
**optional** score-replay path (live chip emulation on the ESP — see `replay.c`). PSOWAV
itself does not need the scores.

Helper tools (same dir): `wavstat.c` (RMS / AC-RMS / DC / peak / brightness), `spksim.c`
(cabinet-speaker band-pass sim, for judging sound "as heard on the machine"), and **`psowavqc.c`**
— PSOWAV **set QA**: per-sound table (dur/rms/peak/clip%) + flags (SILENT/CLIP/SHORT/LONG), or a
master report over many sets (one health line each: ok / minor / CHECK / EMPTY). Run it on the
batch output to see which game sets are good vs need a re-render. Read-only (safe during a batch).

Batch all games: `psowav_batch.sh` renders a PSOWAV set per game for the 75 Gottlieb 80/80A/80B
titles with ROMs, **sequentially** (PinMAME global state ⇒ one psowavgen at a time), detached
(`setsid`/`nohup`), with per-game timeout + `SUMMARY.txt`.

Deploy to SD: `psowav-deploy.sh <outdir> <game...|--all>` assembles a ready-to-copy SD layout —
copies **only the .wav** files (no scores/logs), writes a root `config.txt` (`stheme=` = first
game) + `README.txt`, and runs a `psowavqc` health check. Drag `<outdir>/*` to the SD root.

## 2. PSOWAV set — the on-SD layout

```
/config.txt                      # volv, vols, mix, stheme (optional; sane defaults)
/games.txt                       # FPGA game-select No -> romname (GottFA80_PLuS gamelist)
/<game>/NNNN-AAAA-VVV-desc.wav    # one WAV per sound command  (NNNN=id, AAAA=attrs, VVV=vol 0..100)
/<game>/NNNN-A-M1-M2-...-desc.grp # optional sound group (A = m random | r sequential)
```

**Game select.** The FPGA selects the game via DIP **S1 (1-6) = game No 0..62** (GottFA80_PLuS
user manual v1.01, Appendix A "Gamelist") and sends it on the link as `0x40|No`. `games.txt`
maps each No to a romname/folder — **identical to the manual's gamelist** (e.g. `51 arena`,
`14 blckhole`, `5 jamesb` = James Bond Timed Play). `wavplayer::loadGames()` reads it at boot;
`fpgalink` turns the game token into `wavplayer::selectGame(No)` → loads `/<romname>/`. Edit a
line to remap a number; no recompile. For a single-game machine the `config.txt` `stheme=` default
is enough and games.txt is optional.
Attributes (`AAAA`): `l` loop · `b` break (stop same id) · `i` init/background ·
`v` voice bus · `k` kill all · `c` soft-kill non-bg · `q` quit (keep loops+voices) · `x` placeholder.

`<game>` = the theme folder. The FPGA can select it live (game number → theme `"<n>"`),
or the default `stheme` in `config.txt` is used (single-game machine).

## 3. ESP runtime (`src/`)

- **`fpgalink`** — RX from the FPGA Debug/K2 pin: mode token (diag), `0x80|id` → `wavplayer::play(id)`,
  `0x40|game` → `wavplayer::setTheme`.
- **`wavset`** — clean-room parser/index of the PSOWAV set (host-unit-tested).
- **`wavmix`** — 8-voice polyphonic mixer; PSOWAV attributes (loop/break/kill/voice/…); seamless loop.
- **`wavplayer`** — owns SD + mixer on core 1 (scan theme → resolve id → stream WAV into a voice);
  a cycle-paced DAC task on core 0 clocks each mono 12-bit sample to the **MCP4921** over a
  dedicated SPI bus. Lock-free SPSC ring between them.
- **`/snd` HTTP endpoint** (`net.cpp`, bring-up) — play any sound from a browser/curl with **no
  FPGA**: `GET /snd?id=N` (0..31) plays it, `/snd?theme=NAME` loads a game set, `/snd?stop=1`
  stops, bare `/snd` shows ready-state + usage. (`http://gottfa.local/snd?id=6`)

## 4. Audio output hardware

`MCP4921` 12-bit SPI DAC (mono) sits near the amp → `DAC_R` (Audio1 socket pin 4) →
on-board `TDA7267` (+12 V mono amp) → cabinet speaker. The ESP has its **own SD** for the
PSOWAV sets (the board SD is on the FPGA's SPI bus). Pins: `board_config.h`
(`PIN_DAC_SCK/SDI/CS`, `PIN_FPGA_LINK`).

## Status
- ✅ `psowavgen` builds & renders bit-exact PSOWAV sets (Arena, Black Hole verified).
- ✅ ESP `wavset`/`wavmix`/`wavplayer`/`fpgalink` implemented; host-unit-tested; builds for esp32s3.
- ✅ Mix balance calibrated to PinMAME (per-chip AC-RMS); through cabinet speaker = indistinguishable.
- ⏳ Next: deploy a PSOWAV set to SD, hardware bring-up on a real 80B, then batch all 75 Gottlieb 80/80A/80B games.
