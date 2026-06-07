# WIRING — ESP32-S3 (N16R8) ↔ GottFA80_PLuS board (Cyclone IV EP4CE6)

Cabling plan for the Pstore companion. **All signals 3.3 V. Common ground is mandatory.**
ESP side = `include/board_config.h` (the S3 map). FPGA-board side = the carrier refs noted by
Ralf (J3a SD socket, K2 Debug, S8 reset, P5 JTAG) — **confirm the exact connector pinout with
Ralf before soldering** (the carrier uses a 2×14 square-pin header + a 5×2 IDC + a 2-pin power).

## What the HW20 user manual (v1.01, 20.01.2026) confirms / changes
The official manual is **end-user level** (not a schematic): it documents the board's *features*
but NOT the internal tap-header pinouts (J3a/K2/S8/P5 still need Ralf or the SCH). It does confirm:
- **The board already carries TWO SD sockets**: ① "roms on SD" (top-right) = the game-ROM microSD
  (= our J3a target, FPGA-owned); ② a microSD **inside the DFPlayer Mini** (bottom-left, by the
  "soundboard adapter") = Votrax **speech** files only (folders 10/12/14/18/25/63/64 + games.txt).
  → In Pstore, **PSOWAV replaces the integrated soundboard + DFPlayer Mini entirely**, so SD ② and
  the DFPlayer become redundant; our ESP microSD (GPIO 38–41) is a *separate, third* card.
- **Free Play is a hardware DIP — S2-Dip1 = ON** (manual §5.2.1; "press-and-hold credit >2 s
  simulates a left-chute coin"). So free-play does NOT require a patched ROM. Our `_FP` images give
  *true* free play (real credit logic) — both paths exist; the uploader's FP toggle = pick the
  `_FP` image, independent of the DIP. Other option DIPs: S2-Dip2 init-nvram, S2-Dip3/4 slam.
- **Game select = S1 Dips 1–6** (binary, manual Appendix A) — our `build_romstore.py` GAMES map
  matches Appendix A exactly (0 Panthera … 51 Arena … 56 Excalibur … 62 Amazon Hunt II, 63 unused).
- **Status LEDs**: red "SD card error" (lights if the FPGA can't read the SD) + yellow "ON" (lights
  once code runs / IRQs seen). Boot display: P1=FW ver, P2=selected game#, P3=lisy.dev id, P4=SD ver.
- **SD reader is picky** (manual §8: *fixed sectors, no filenames, needs his 128 MB image,
  "Sandisk cards can NOT be used"*) → confirms the motivation for the one-card NOR route (FPGA off
  the SD). And the **16 KB per-game layout is [4 KB game | 4 KB soundcard | 8 KB system]** (§8.1) —
  the merge recipe the ROM uploader must honor (our pre-merged `_FP.img` files already follow it).
- **Drop-in MPU**: the Gottlieb harness connectors **A1J*/A2J*/A3J*** (manual §3) are the *machine*
  side — untouched (the board replaces the original CPU 1:1). For System 80B, **A1J3 is unconnected**.
  These are NOT our tap points; the ESP taps the internal SD/Debug/reset/JTAG headers only.

## 0. Board / module
- ESP: **ESP32-S3 N16R8** (16 MB QIO flash + 8 MB OPI PSRAM). USB-C for power+flash.
- FPGA: **EP4CE6E22C8N** + IS25LP016D (EPCS16, bitstream) + microSD (GottFA game ROMs) + TDA7267 amp.
- Reserved S3 pins we MUST avoid (already are): **26–37** (QIO flash + octal PSRAM), strapping
  **0/3/45/46**, native-USB **19/20**, UART0 **43/44**.

## 1. Power & ground
| ESP | ↔ | Board | Note |
|---|---|---|---|
| GND | ── | GND | **mandatory common ground** (PZ254V 2-pin or any GND) |
| 5V (USB-C) or 3V3 | | 3V3 rail | power the S3 from USB-C **or** the board 3V3 (don't back-feed both) |

## 2. FPGA → ESP link  (1-wire UART, 115200 8N1)
| ESP GPIO | dir | FPGA-board | carries |
|---|---|---|---|
| **8** `PIN_FPGA_LINK` | ◄── IN | Debug = FPGA pin 11 (**K2.2**) | diag-token + sound# `0x80\|s` + game# `0x40\|No` + game-state `0xF2/F3` |

## 3. ESP → FPGA display inject  (option B, UART TX)
| ESP GPIO | dir | FPGA-board | carries |
|---|---|---|---|
| **9** `PIN_FPGA_DISP_TX` | ──► OUT | Audio_RX (**PIN_2**) | time-attack countdown digits (only when ESP is the sound source) |

## 4. Shared SPI bus + reset  (diag / norprog — Hi-Z in normal play, FPGA owns the bus)
| ESP GPIO | dir | FPGA-board | FPGA pin |
|---|---|---|---|
| **12** `PIN_SPI_SCLK` | ──► | J3a.5 | FPGA39 |
| **11** `PIN_SPI_MOSI` | ──► | J3a.3 | FPGA42 |
| **13** `PIN_SPI_MISO` | ◄── | J3a.7 | FPGA34 |
| **10** `PIN_SPI_CS_SD` | ──► | J3a.2 | FPGA31 |
| **14** `PIN_FPGA_RESET` | ──► | **S8.2** | hold FPGA in reset to take the bus (norprog) |

## 5. JTAG → FPGA  (optional: ESP flashes the bitstream / reads IDCODE — header P5)
| ESP GPIO | FPGA-board (P5) |
|---|---|
| **4** `PIN_JTAG_TCK` | TCK (FPGA16) |
| **5** `PIN_JTAG_TMS` | TMS (FPGA18) |
| **6** `PIN_JTAG_TDI` | TDI (FPGA15) |
| **7** `PIN_JTAG_TDO` | TDO (FPGA20) |

## 6. Sound — PCM5102A I2S DAC → TDA7267 (PSOWAV audio out)
| ESP GPIO | → | PCM5102A |
|---|---|---|
| **16** `PIN_I2S_BCK` | → | BCK |
| **17** `PIN_I2S_LRCK` | → | LRCK |
| **18** `PIN_I2S_DOUT` | → | DIN |
| — | | **SCK → GND** (internal PLL, no MCLK) |
| 3V3 / GND | | VCC / GND ; module defaults for FLT/DEMP/FMT ; XSMT → 3V3 (un-mute) |

PCM5102A **line-out (L or R) → TDA7267 input → cabinet speaker** (line-level, so it still sums
with GOSOF80's PWM into the same TDA7267 in a hybrid build).

## 7. ESP's own microSD  (PSOWAV WAVs + the /roms romstore — dedicated bus, no FPGA contention)
| ESP GPIO | → | microSD |
|---|---|---|
| **38** `PIN_SD_SCK` | → | CLK |
| **39** `PIN_SD_MISO` | ← | DAT0/MISO |
| **40** `PIN_SD_MOSI` | → | CMD/MOSI |
| **41** `PIN_SD_CS` | → | CD/DAT3 (CS) |

Card holds: `/config.txt`, `/games.txt`, `/<game>/NNNN-…-snd.wav` (sounds), `/roms/<NN>.img` (romstore).

## 8. OLED status (optional, SSD1306 128×32 I2C)
| ESP GPIO | → | OLED |
|---|---|---|
| **47** `PIN_OLED_SDA` | ↔ | SDA |
| **48** `PIN_OLED_SCL` | → | SCL |

## 9. Optional coil current-sense
- **GPIO 1** `PIN_COIL_SENSE` (ADC1_CH0) ← shunt amp/divider (0–3.3 V). Default OFF (`COIL_SENSE_ENABLE 0`).

---
### SD cards today (see the architecture notes)
The HW20 board ships with **two** SD sockets; Pstore adds a third on the ESP:
- **FPGA microSD** (on the board, J3a / "roms on SD"): GottFA game ROMs — FPGA-owned, untouched.
- **DFPlayer-Mini microSD** (on the board, by the soundboard adapter): Votrax speech only —
  **redundant in Pstore** (PSOWAV replaces the integrated soundboard + DFPlayer).
- **ESP microSD** (GPIO 38–41): PSOWAV WAVs + `/roms` (game-ROM image store).

### Future one-card evolution (to discuss with Ralf)
Drop the FPGA microSD: the ESP serves the ROM to the FPGA (route a: a W25Q on the SD pins via
`nor_flash.vhd`; route b: reuse the IS25LP016D free ~1.6 MB via ASMI; route c: ESP feeds the ROM
to FPGA block-RAM at boot over the shared SPI bus). → one ESP-owned card + 100 % SD-card
compatibility + WiFi-updatable ROMs. Needs an FPGA-side module — Ralf coordination.
