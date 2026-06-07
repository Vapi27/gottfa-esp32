#pragma once
#include <Arduino.h>

// ============================================================================
//  GottFA80 PLuS — ESP32-S3 companion  (Pstore Pinball Platform)
//  Board-side wiring reference + ESP32-S3 GPIO map.
//  Target MCU: ESP32-S3 (DevKitC-1). All I/O is 3.3 V -> no level shifting.
//  Common ground with the GottFA board is mandatory.
// ============================================================================

#define FW_NAME    "GottFA80-PLuS ESP32-S3 companion"
#define FW_VERSION "0.5.0"
#define MDNS_HOST  "gottfa"               // -> http://gottfa.local/

// ---- WiFi -------------------------------------------------------------------
#define WIFI_STA_SSID       ""            // your network (empty = go straight to AP)
#define WIFI_STA_PASS       ""
#define WIFI_STA_TIMEOUT_MS 12000
#define WIFI_AP_SSID        "GottFA80-Setup"
#define WIFI_AP_PASS        "pinball80"   // >= 8 chars

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
// Sound tier (S3): MCP4921 12-bit SPI DAC (MONO) -> DAC_R (Audio1 socket pin 4) ->
// on-board TDA7267 (+12 V mono amp) -> cabinet speaker. This is PSOWAV's audio output
// (our WAV engine). A dedicated SD holds the PSOWAV sets (the board SD is FPGA-owned). All 3.3 V.
// MCP4921: /LDAC tied to GND on the board (output updates on /CS rising edge).
#define AUDIO_RATE     44100
#define PIN_DAC_SCK      16   // MCP4921 SCK
#define PIN_DAC_SDI      17   // MCP4921 SDI  (ESP MOSI)
#define PIN_DAC_CS       18   // MCP4921 /CS  (hardware-framed per sample)
#define PIN_SD_SCK       38
#define PIN_SD_MISO      39
#define PIN_SD_MOSI      40
#define PIN_SD_CS        41
#define PIN_FPGA_LINK     8   // UART RX from FPGA Debug pin (K2): diag-mode token + sound#/game#
#define PIN_FPGA_DISP_TX  9   // UART TX to FPGA Audio_RX/PIN_2 (hybrid): time-attack display digits (option B)
// Optional status OLED (SSD1306 128x32 I2C) — free S3 GPIOs; skipped gracefully if absent.
#define PIN_OLED_SDA     47
#define PIN_OLED_SCL     48
#define OLED_W          128
#define OLED_H           32
#define OLED_ADDR      0x3C
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
