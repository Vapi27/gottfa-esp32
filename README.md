# GottFA80 PLuS — ESP32-S3 companion firmware

**v1.0.0** · WiFi companion for the GottFA80+ (Cyclone 10 LP) Gottlieb System 80/80A/80B
CPU-board replacement. Part of the **Pstore Pinball Platform**.

The ESP32-S3 sits next to the FPGA board and gives it a network: a web control panel, the
PSOWAV sound engine, the tournament / time-attack engine, a ROM store, firmware and
bitstream updates over WiFi, and the bench instruments the whole project was debugged with.

> **Honesty note.** This README distinguishes what has been **run on a real machine** from
> what has only been **compiled or simulated**. Anything not in the first list has not been
> proven on hardware yet, whatever the code comments say.

---

## Proven on a real machine

Measured on a Gottlieb **Volcano** (System 80, numeric displays):

- **Web UI + WebSocket** control panel (LISYcontrol): live 8×8 switch matrix, 48 lamps,
  9 coils, displays, sound, DIPs — bound to the **real** FPGA SPI register file.
- **LISYcontrol diagnostics** end to end: switches, coils, lamps, sound, 80B text, numeric
  display test. Diag is entered by long-pressing the door TEST switch.
- **PSOWAV sound**: SD card → I2S → PCM5102A DAC → cabinet amp.
- **FPGA link** (UART on GPIO8): diag token, game state, ball number, sound commands.
- **`/ramsnap`** RAM snapshots — the tool that located `$0072` (game in progress) and
  `$0109` (ball counter) by correlation.
- **Time-attack**, full loop live: arm → countdown starts on the game edge → shown on the
  player-2 display while the ROM keeps the real score on player 1 → game events top the
  clock up → zero → slam → game ends → score recorded → self-disarm.
- **FPGA reflash over WiFi** via the ESP's XVC server (SRAM `.svf` and permanent EPCS
  `.rbf`) — no programming cable.
- Game ROM booting from the external SPI NOR instead of the microSD.

## Compiled and unit-tested, **not yet run on hardware**

- **WiFi provisioning wizard** (SoftAP + captive portal + NVS credentials) — compile-verified
  only; the radio path has never been exercised end to end.
- **Data-driven per-title sound map** (`sndmap`) — 39 host unit tests pass; not yet driven
  from a real machine's bus for every title.
- The **job queue** that moved slow work out of the AsyncTCP handlers.
- **esp32c3** target — see *Targets* below.

## Known limitations (v1)

- **Every HTTP route and the XVC port are unauthenticated**, OTA is unsigned, and several
  destructive routes are `GET`. Keep the board on a trusted network. See
  [`DIAGNOSTICS.md`](DIAGNOSTICS.md#security--read-this-before-putting-a-board-on-a-shared-network).
- The factory hotspot password is the same on every board (`pinball80`, published in
  `WIFI_SETUP.md`). Change it from the portal; `/sysinfo` reports whether it is still the
  default.
- The time-attack countdown is **invisible between games** (the attract path goes through
  the FPGA's `boot_message`, which has never produced a visible image on this glass). The
  in-game path works.
- No 80B **alphanumeric** display back-end. The numeric System-80 path is proven.
- No golden/fallback FPGA image: an interrupted EPCS write leaves the FPGA unconfigurable
  until it is burned again.

---

## Build

```sh
./build.sh              # tests + firmware + LittleFS image + MANIFEST -> dist/
./build.sh --all        # also compile-check the experimental C3 target
./build.sh --clean      # from scratch (do this before tagging a release)
```

`dist/MANIFEST.txt` records the sha256, size, git commit, commit date and toolchain of every
artifact, plus the flash offsets. A dirty tree is stamped `-dirty` and the manifest refuses to
pretend otherwise. `build.sh` also aborts if any tracked file is hidden from git with
`skip-worktree` — that is what silently broke the build before v1.

Reproducibility, **measured** rather than claimed:

| artifact | reproducible? |
|---|---|
| `firmware.bin` | **yes**, for the same commit rebuilt from the same absolute path — the version stamp uses the git *commit date*, not the wall clock. A different directory yields a different hash: the toolchain bakes absolute source paths in. |
| `littlefs.bin` | **no.** `mklittlefs` stores each file's mtime, which changes on every checkout. Its hash identifies the image you shipped; it is not one a third party can recompute. |

Plain PlatformIO works too:

```sh
pio run -e esp32s3                  # firmware  -> .pio/build/esp32s3/firmware.bin
pio run -e esp32s3 -t buildfs       # web UI    -> .pio/build/esp32s3/littlefs.bin
pio run -e esp32s3 -t upload        # flash over USB
pio run -e esp32s3 -t uploadfs      # upload the web UI
pio device monitor                  # serial @ 115200
./tools/run_tests.sh                # host unit tests (~1 s, no hardware)
```

### Updating a board already in the field

```sh
curl -F 'file=@dist/firmware.bin' http://gottfa.local/ota    # firmware + reboot
curl -F 'file=@data/index.html'   http://gottfa.local/fsup   # web UI only
curl -s  http://gottfa.local/sysinfo                         # confirm what is running
```

## Targets

| env | status |
|---|---|
| `esp32s3` | **the product.** ESP32-S3 N16R8 (16 MB flash, 8 MB PSRAM). Everything above is built for this. |
| `esp32c3` | **experimental — never run on hardware.** Compiles, and nothing more. No sound tier, no ROM store, no OLED, no status LED, **no time-attack**. Kept compile-checked so the code stays portable. Do not ship. |
| `gosowav`, `gosowav_diag`, `gosowav_nowifi` | PSOROM/6502 benches on a WROVER board. Development rigs, not product firmware. |

## Hardware

ESP32-S3 (DevKitC-1). All 3.3 V — **no level shifting**. **Common ground mandatory.**
Power the ESP from board **+5 V (TP4)** → its onboard regulator (don't load the FPGA's
3.3 V rail).

| Group | ESP signals | Board tap |
|---|---|---|
| SPI bus | SCLK / MOSI / MISO / CS_SD | carrier SD socket **J3a** |
| Control | Reset (open-drain) / Debug | **S8** / **K2** |
| JTAG | TCK / TMS / TDI / TDO | module header **P5** |
| Sound | I2S → PCM5102A, dedicated SD | see `WIRING.md` |
| Power | 5 V in + GND | **TP4** + GND |

Full GPIO map with FPGA pin numbers: [`include/board_config.h`](include/board_config.h).

WiFi credentials are **not** compile-time constants. On first boot the board raises its own
hotspot and a setup page pops up — see [`WIFI_SETUP.md`](WIFI_SETUP.md). Staying on the
hotspot for ever is a supported answer; the pinball is fully usable with no home network.

## Also in this repo — Arena Wall-Art LED
**→ Full project documentation (FR): [WALL_PINBALL_PLAYFIELD.md](WALL_PINBALL_PLAYFIELD.md)**

A separate, standalone firmware (`pio run -e arenaled_d1mini32`) that turns a Gottlieb **Arena**
playfield into an illuminated wall piece: D1 Mini ESP32 (or S3/C3) + up to 150 SK6812MINI-RGBW
on one data chain, pinball-style lighting modes mapped to the real inserts, Wi-Fi web UI + REST + OTA,
and a per-frame current meter that never lets the chain exceed its power budget.
No FPGA, no sound, nothing shared with the app above. See **[ARENA_LED.md](ARENA_LED.md)**
(hardware, PCB, power injection, and how to drive 5 V pixels from 3.3 V logic without a
74AHCT125).

## Where to look next

| File | |
|---|---|
| [`DIAGNOSTICS.md`](DIAGNOSTICS.md) | **every route**, what it does, what it can break, and the security caveats |
| [`WIFI_SETUP.md`](WIFI_SETUP.md) | provisioning wizard, factory reset |
| [`WIRING.md`](WIRING.md) | the harness |
| [`SOUND_WIRE.md`](SOUND_WIRE.md) | how sound commands get from the FPGA to a WAV |
| [`SOUND_MAP.md`](SOUND_MAP.md) | writing a per-title `sound.map` |
| [`TOURNAMENT.md`](TOURNAMENT.md) | tournament + time-attack |
| [`BRING-UP.md`](BRING-UP.md) | first power-on checklist |

`STATUS.md`, `ONE_CARD_PLAN.md`, `PSOWAV.md`, `PSOROM.md`, `GOSOWAV.md` and
`EPROM_READER.md` are development notes kept for reference. They predate v1 and are not
maintained as user documentation.

## Licence

The FPGA design this firmware talks to derives from bontango's GPL-3 **GottFA80**.
This ESP firmware is original work, but it ships as part of that system — **before
distributing any binary, settle the licensing and publish the modified FPGA sources.**
No licence file has been added yet; that is the owner's decision, not a default.
