#pragma once
#include <Arduino.h>

// ============================================================================
//  GottFA80 PLuS — ESP32-S3 companion  (Pstore Pinball Platform)
//  Board-side wiring reference + ESP32-S3 GPIO map.
//  Target MCU: ESP32-S3 (DevKitC-1). All I/O is 3.3 V -> no level shifting.
//  Common ground with the GottFA board is mandatory.
// ============================================================================

#define FW_NAME    "GottFA80-PLuS ESP32-S3 companion"
#define FW_VERSION "0.2.0"
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
#define PIN_FPGA_DEBUG   3
#define PIN_JTAG_TCK     0
#define PIN_JTAG_TMS     1
#define PIN_JTAG_TDI    20
#define PIN_JTAG_TDO    21
#else
// --- ESP32-S3 (DevKitC-1): avoid strapping(0,3,45,46), USB(19,20), UART0(43,44), flash/PSRAM(26-37) ---
#define PIN_SPI_SCLK    12
#define PIN_SPI_MOSI    11
#define PIN_SPI_MISO    13
#define PIN_SPI_CS_SD   10
#define PIN_FPGA_RESET  14
#define PIN_FPGA_DEBUG  15
#define PIN_JTAG_TCK     4
#define PIN_JTAG_TMS     5
#define PIN_JTAG_TDI     6
#define PIN_JTAG_TDO     7
#endif

// ---- FPGA identity (JTAG IDCODE) -------------------------------------------
// Cyclone 10 LP 10CL006 reuses the Cyclone IV E die -> same IDCODE.
#define FPGA_IDCODE_10CL006  0x020F10DDUL
#define FPGA_IR_LEN          10
