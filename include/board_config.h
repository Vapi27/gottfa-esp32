#pragma once
#include <Arduino.h>

// ============================================================================
//  GottFA80 PLuS — ESP32-S3 companion  (Pstore Pinball Platform)
//  Board-side wiring reference + ESP32-S3 GPIO map.
//  Target MCU: ESP32-S3 (DevKitC-1). All I/O is 3.3 V -> no level shifting.
//  Common ground with the GottFA board is mandatory.
//
//  DO NOT `git update-index --skip-worktree` THIS FILE.
//  It was marked skip-worktree to keep a local WiFi password out of commits, and
//  the cost was invisible for weeks: every later edit to this header stopped being
//  committed too. By v1.0.0 the version in git had PIN_OLED_SCL still on GPIO48
//  (the value that latched the status LED white) and no PIN_RGB_LED at all, so a
//  fresh clone of main DID NOT COMPILE -- the firmware only built on one laptop.
//  Verified 2026-07-30 by cloning HEAD and running `pio run -e esp32s3`: four
//  "'PIN_RGB_LED' was not declared" errors. There are no secrets in this file any
//  more (credentials live in NVS via the provisioning wizard), so it must stay a
//  normal tracked file. build.sh refuses to build if the flag comes back.
// ============================================================================

#define FW_NAME    "GottFA80-PLuS ESP32-S3 companion"
#define FW_VERSION "1.0.0"
#define MDNS_HOST  "gottfa"               // -> http://gottfa.local/

// Build identity. version.py (a PlatformIO pre-script) overrides both from git:
// FW_GIT = short commit (+ "-dirty"), FW_BUILD = the COMMIT date. The fallbacks
// below only apply when the tree is built outside a checkout (tarball, IDE that
// skips extra_scripts) -- so an unidentifiable board says so instead of lying.
#ifndef FW_GIT
#define FW_GIT   "nogit"
#endif
#ifndef FW_BUILD
#define FW_BUILD "unknown"
#endif
// One string for logs / the OLED / the tab title: "1.0.0+951b327".
#define FW_VERSION_FULL FW_VERSION "+" FW_GIT

// ---- WiFi -------------------------------------------------------------------
// NO compile-time SSID/password. Credentials live in NVS and are entered through
// the provisioning wizard (SoftAP + captive portal, see wifiprov.cpp / WIFI_SETUP.md)
// because a customer has no toolchain. The three #defines that used to sit here --
// WIFI_STA_SSID / WIFI_STA_PASS / WIFI_AP_SSID / WIFI_AP_PASS -- had been dead since
// the wizard landed and one of them was a real home-network password committed to
// git. Removed for v1; the AP name/password defaults live in wifiprov.cpp
// (WIFIPROV_AP_BASE / WIFIPROV_AP_PASS_DEFAULT), overridable per batch with -D.
#define WIFI_STA_TIMEOUT_MS 12000         // boot-time STA connect budget (wifiprov)

// ---- ESP32-S3 GPIO map ------------------------------------------------------
// Chosen to avoid S3 reserved pins: strapping (0/3/45/46), native-USB (19/20),
// UART0 (43/44), and SPI flash/PSRAM (26-37).
//
// Group A — shared SPI bus + control. Tap at carrier SD socket J3a / K2 / S8.
//   In NORMAL mode the FPGA owns the bus -> SCLK/MOSI/CS idle as INPUTs (Hi-Z).
// Group B — JTAG to FPGA module header P5 (bitstream OTA + IDCODE bring-up).
//   Board side: SCLK=FPGA39(J3a.5) MOSI=42(J3a.3) MISO=34(J3a.7) CS_SD=31(J3a.2)
//   Reset=S8.2 Debug=FPGA11(K2.2) JTAG TCK16/TMS18/TDI15/TDO20 (P5). All 3.3 V.
#ifdef BOARD_C3
// --- ESP32-C3 (e.g. "C3 SuperMini"): avoid flash(12-17), USB(18,19), strapping(2,8,9) ---
#define PIN_SPI_SCLK     4
#define PIN_SPI_MOSI     6
#define PIN_SPI_MISO     5
#define PIN_SPI_CS_SD    7
#define PIN_FPGA_RESET  10
#define PIN_FPGA_LINK    3   // UART RX from FPGA Debug pin: diag-mode token
#define PIN_JTAG_TCK     0
#define PIN_JTAG_TMS     1
#define PIN_JTAG_TDI    20
#define PIN_JTAG_TDO    21
#define PIN_COIL_SENSE   2   // ADC1_CH2 — optional coil current-sense input
#else
// --- ESP32-S3 (DevKitC-1): avoid strapping(0,3,45,46), USB(19,20), UART0(43,44), flash/PSRAM(26-37) ---
#define PIN_SPI_SCLK    12
#define PIN_SPI_MOSI    11
#define PIN_SPI_MISO    13
#define PIN_SPI_CS_SD   10
#define PIN_FPGA_RESET  14
#define PIN_JTAG_TCK     4
#define PIN_JTAG_TMS     5
#define PIN_JTAG_TDI     6
#define PIN_JTAG_TDO     7
#define PIN_COIL_SENSE   1   // ADC1_CH0 — optional coil current-sense input
// Sound tier (S3): PCM5102A I2S DAC (line-level) -> on-board TDA7267 (+12 V mono amp) ->
// cabinet speaker. This is PSOWAV's audio output (our WAV engine), driven by the S3's I2S
// peripheral via DMA (= ~0 CPU; frees the core the old MCP4921 busy-loop burned). A dedicated
// SD holds the PSOWAV sets (the board SD is FPGA-owned). All 3.3 V. Line-level so it still sums
// with GOSOF80's PWM into the shared TDA7267 in a hybrid build.
// PCM5102A module: SCK->GND (internal PLL, no MCLK needed); FLT/DEMP/XSMT/FMT per module defaults.
#define AUDIO_RATE     44100
#define PIN_I2S_BCK      16   // PCM5102A BCK  (bit clock)
#define PIN_I2S_LRCK     17   // PCM5102A LRCK (word / L-R select)
#define PIN_I2S_DOUT     18   // PCM5102A DIN  (data)
#define PIN_SD_SCK       38
#define PIN_SD_MISO      39
#define PIN_SD_MOSI      40
#define PIN_SD_CS        41
#define PIN_FPGA_LINK     8   // UART RX from FPGA Debug pin (K2): diag-mode token + sound#/game#
#define PIN_FPGA_DISP_TX  9   // UART TX to FPGA Audio_RX/PIN_2 (hybrid): time-attack display digits (option B)
// Optional status OLED (SSD1306 128x32 I2C) — free S3 GPIOs; skipped gracefully if absent.
// SCL moved 48 -> 21 (2026-07-09): GPIO48 is the on-board WS2812 RGB LED (hardwired) —
// I2C clock traffic on it kept the LED latched garbage-white. GPIO21 is free
// (EPROM reader is disabled). SDA stays on 47.
#define PIN_OLED_SDA     47
#define PIN_OLED_SCL     21
// On-board WS2812 RGB LED (status beacon — see statusled.h), GPIO48 on this
// board (big "RGB" LED; shares the pin with OLED SCL above — the LED
// auto-disables if a panel is found). CAUTION: the core's neopixelWrite()
// binds its RMT channel to the FIRST pin it is called with and silently
// ignores the pin argument afterwards — so PIN_RGB_LED must be right at boot;
// you cannot probe other pins at runtime.
#define PIN_RGB_LED      48
#define OLED_W          128
#define OLED_H           32
#define OLED_ADDR      0x3C
// ---- Optional EPROM-reader daughterboard (dump your own ROM -> SD; see EPROM_READER.md) ------
// 2x 74HC595 (address + /CE + /OE) + 1x 74HC165 (data) over 5 dedicated GPIOs. Default OFF.
#define EPROM_READER_ENABLE 0
#define PIN_EPR_SER      15   // -> 74HC595 SER
#define PIN_EPR_SCLK     21   // -> 74HC595 SRCLK + 74HC165 CLK (shared)
#define PIN_EPR_RCLK     42   // -> 74HC595 RCLK (latch)
#define PIN_EPR_LOAD      2   // -> 74HC165 SH/LD (parallel load)
#define PIN_EPR_QH        1   // <- 74HC165 QH (serial data in)
#endif

// ---- Coil current sense (OPTIONAL — needs a shunt; default OFF) -------------
// True overcurrent/short detection needs a current sensor: fit a low-side shunt
// on the solenoid common return -> amp/divider to 0-3.3V -> PIN_COIL_SENSE (ADC).
// Then set COIL_SENSE_ENABLE 1 and calibrate the thresholds (12-bit ADC counts).
// Without it the FPGA still protects coils (pulse clamp + cooldown + watchdog =>
// COIL_FAULT reg 0x32); this just adds the electrical open/short read.
#define COIL_SENSE_ENABLE   0
#define COIL_SENSE_OPEN     200    // amp (peak-baseline) <= this during a pulse => open coil
#define COIL_SENSE_SHORT    3500   // peak >= this => overcurrent / short

// ---- FPGA identity (JTAG IDCODE) -------------------------------------------
// Cyclone 10 LP 10CL006 reuses the Cyclone IV E die -> same IDCODE.
#define FPGA_IDCODE_10CL006  0x020F10DDUL
#define FPGA_IR_LEN          10
