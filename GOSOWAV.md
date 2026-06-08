# Running Pstore firmware on Ralf's GOSOWAV board (port)

The user has **GOSOWAV_11** boards (Ralf / lisy.dev). Goal: run **our** PSOWAV firmware on them
(commercial-OK, our features) instead of the CC-BY-NC-SA `pwavplayer`. Hardware (from the GOSOWAV
gerbers/BOM + `bontango/pwavplayer` pin defines):

## GOSOWAV hardware = ESP32-WROVER-E-N16R8 + MCP4921 + SDMMC + ULN2803 cmd-bus + TDA7267
**Pinout VERIFIED against the official schematic** `GOSOWAV_11_SCH.PDF` (lisy.dev/swrep/soundboards/
GOSOWAV/schematics/), IC1 net labels — 2026-06-08.
| Function | WROVER GPIO | Note |
|---|---|---|
| **MCP4921 DAC** | SDI/MOSI=**23**, SCK=**18**, CS=**5** | ✅ schematic-exact. LDAC tied to **GND** (immediate update, no latch). VOUT→2.2K/10µF→150K→TDA7267 amp. Our S3 board uses PCM5102A I2S instead |
| **SD card (4-bit SDMMC, TF-01A)** | CLK=14, CMD=15, D0=2, D1=4, D2=12, D3=13 | ✅ schematic-exact, fixed SDMMC slot-1 IOMUX pins. **NO external pull-ups on any SD line** (R1/R2/R3 4.7K are LED resistors, not SD) → the board RELIES on the ESP's INTERNAL pull-ups. Arduino `SD_MMC.begin()` doesn't enable them → 1-bit returns false (D3/CS floats→card latches SPI mode) + 4-bit hangs. FIX (commit 8e385d6): `gpio_set_pull_mode(GPIO_PULLUP_ONLY)` on all 6 pins before begin (= Ralf's `SDMMC_SLOT_FLAG_INTERNAL_PULLUP`) |
| **Sound-cmd bus** (from a real System 80 machine, via ULN2803 IC2) | S1_A=**27**, S2_B=**26**, S4_C=**25**, S8_D=**33**, S16_E=**32**, F=**35**, Strobe=**34** | inverted by the ULN2803; the machine drives a 5-bit sound code (S1..S16) + F + Strobe. Read these for real-pinball mode; PSOROM bench injects via web/serial instead |
| UART0 (USB debug, via CH340C) | TXD0, RXD0 | the USB-serial console |
| Status LEDs | D1 on GPIO39, D2 on GPIO36 (Sig$54), D3 (power) | GPIO36/39 are input-only sensor pins; LEDs via 4.7K (R1/R2). IO21, IO22 = **NC** |

(pwavplayer is `#define ESP32_WROVER` by default; it also has an ESP32_S3 variant — confirming the
GOSOWAV is WROVER. Ralf mounts the SD 4-bit @40MHz with internal pull-ups, ground-truth from
`github.com/bontango/pwavplayer` main/wavplayer.c MountSDCard() + main/pgpio.h.)

## The 4 deltas vs our S3 firmware
1. **MCU = WROVER** (Xtensa LX6, no octal PSRAM, different reserved pins) → new PlatformIO env +
   `BOARD_GOSOWAV` in `board_config.h`.
2. **Audio out = MCP4921 SPI** (we migrated to PCM5102A I2S in commit `3e0a436`). **Recover the old
   MCP4921 path from commit `61391c9`** (SPSC ring of 12-bit samples + a core-0 `dacTask` that does
   `dacspi.transfer16(0x3000 | sample)`; mixTask fills the ring) and make `wavplayer` output
   **board-conditional**: `#ifdef BOARD_GOSOWAV` → MCP4921 ring+dacTask, `#else` → I2S.
3. **SD = 4-bit SDMMC** (we use SD.h over SPI). Make SD begin board-conditional: `SD_MMC.begin()`
   (4-bit, default pins) for GOSOWAV vs `SD.begin(SPI)` for the S3 board.
4. **Command input**: two scenarios (see below) — pick per the user's setup.

## Command-input scenario (NEEDS the user's answer)
- **A) GOSOWAV + GottFA FPGA (Pstore):** the FPGA sends the 5-bit sound command over UART to the
  GOSOWAV **RX=GPIO36**. → point `fpgalink` at GPIO36. Simplest, fits our architecture. PSOROM/PSOWAV
  triggered by the FPGA link, exactly like our S3 build.
- **B) GOSOWAV standalone in a machine:** read the parallel **cmd bus** (PB0..7 + CB1 strobe) through
  the ULN2803 → a new parallel-command-input path (sample the 8 GPIOs on the CB1 edge). This is the
  GOSOWAV's native (Bally/Williams) use; for Gottlieb the command is 5-bit on the low lines.

## DECIDED (user): scenario **B** — GOSOWAV standalone, reads the machine's parallel cmd bus.
Note: the user's machine is **Gottlieb System 80** (4-5-bit sound command), but the GOSOWAV/pwavplayer
bus mapping is **Bally/Williams System 11** (8-bit). The command-input pins must be confirmed against
the GOSOWAV_11 schematic; the likely production mapping (pwavplayer WROVER "trace prototype", which
does NOT clash with the MCP4921 on 23/18/5) is: PB0=39, PB1=34, PB2=35, PB3=32, PB4=33, PB5=25,
PB6=26, PB7=27, CB1(strobe)=36, CB2=19. For Gottlieb, read the low 4-5 bits (PB0..PB4) on the strobe.

## Product profile: GOSOWAV = a **sound-only WROVER build** (key insight)
The GOSOWAV is a standalone sound board — it does NOT need the FPGA diag bridge / JTAG / coil / OLED
that our S3 board has. So `BOARD_GOSOWAV` should **compile those out** (like `BOARD_C3` gates the
sound tier, but the opposite split) and **keep** the sound tier (PSOWAV + PSOROM) + WiFi UI. This is
the real work: a new feature profile, not just a pin map.

## Recovered MCP4921 output (from commit `61391c9`, to graft board-conditionally into wavplayer)
SPSC ring of 12-bit samples + a **core-0 `dacTask`**: `dacspi.transfer16(0x3000 | (s & 0x0FFF))`
(0x3000 = MCP4921 cfg: DAC-A, unbuffered Vref, 1× gain, active; 0x0800 = mid-scale silence). mixTask
fills `ringBuf[(h+i)&(RING-1)] = (uint16_t)((mono + 32768) >> 4)`. `dacspi=SPIClass(FSPI)`,
`dacspi.begin(DAC_CLK=18, -1, DAC_MOSI=23, DAC_CS=5)`, `SPISettings(20000000, MSBFIRST, SPI_MODE0)`.

## Plan (status)
- [x] research: GOSOWAV pinout + MCP4921 recovery point + scenario decided.
- [x] `GOSOWAV.md` (this doc) — complete actionable spec.
- [ ] `board_config.h`: add `#elif defined(BOARD_GOSOWAV)` branch (WROVER: MCP4921, SDMMC, cmd bus,
      UART RX 36, LED 21); compile out the FPGA-bridge symbols (or define unused).
- [ ] `platformio.ini`: `[env:gosowav]` (board = esp32 WROVER devkit, PSRAM enabled, LittleFS).
- [ ] `wavplayer`: `#ifdef BOARD_GOSOWAV` MCP4921 ring+dacTask + `SD_MMC.begin()` (4-bit) `#else`
      I2S + SD-over-SPI.
- [ ] new `cmdbus.{h,cpp}`: sample PB0..n on the CB1 strobe edge → `wavplayer::playLive(cmd)`.
- [ ] gate FPGA-bridge/jtag/diag/coil out under BOARD_GOSOWAV (sound-only profile).
- [ ] `pio run -e gosowav`; user flashes + tests on the real GOSOWAV (the only HW-validation step).
