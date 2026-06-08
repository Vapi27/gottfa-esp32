# Running Pstore firmware on Ralf's GOSOWAV board (port)

The user has **GOSOWAV_11** boards (Ralf / lisy.dev). Goal: run **our** PSOWAV firmware on them
(commercial-OK, our features) instead of the CC-BY-NC-SA `pwavplayer`. Hardware (from the GOSOWAV
gerbers/BOM + `bontango/pwavplayer` pin defines):

## GOSOWAV hardware = ESP32-WROVER + MCP4921 + SDMMC + ULN2803 cmd-bus + TDA7267
| Function | WROVER GPIO | Note |
|---|---|---|
| **MCP4921 DAC** | SDI/MOSI=**23**, SCK=**18**, CS=**5** | 12-bit SPI DAC (our S3 board uses PCM5102A I2S instead) |
| **SD card (4-bit SDMMC)** | CLK=14, CMD=15, D0=2, D1=4, D2=12, D3=13 | fixed SDMMC slot-1 pins; our S3 board uses SPI SD |
| UART | TX=22, **RX=36** | RX=36 is input-only (no pull-up) — FPGA→ESP command link lands here |
| LED "playing" | 21 | active-LOW |
| Cmd bus (BW11, via ULN2803) | PB0..7 + CB1/CB2 + RES | 8-bit parallel sound command from a machine (Bally/Williams-style) |

(pwavplayer is `#define ESP32_WROVER` by default; it also has an ESP32_S3 variant — confirming the
GOSOWAV is WROVER.)

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
