#include <Arduino.h>
#include <LittleFS.h>
#include "board_config.h"
#include "jtag.h"
#include "net.h"
#include "wavplayer.h"
#include "romstore.h"
#include "epromdump.h"
#include "romdb.h"
#include "ownership.h"
#include "fpgalink.h"
#include "tourney.h"
#include "dispinject.h"
#include "oled.h"

// In NORMAL mode the FPGA is master of the shared SD/EEPROM SPI bus, so the ESP
// must present Hi-Z on those lines until it is explicitly granted the bus.
static void idleGroupA() {
  pinMode(PIN_SPI_SCLK,   INPUT);
  pinMode(PIN_SPI_MOSI,   INPUT);
  pinMode(PIN_SPI_MISO,   INPUT);
  pinMode(PIN_SPI_CS_SD,  INPUT);
  pinMode(PIN_FPGA_RESET, INPUT);   // open-drain released (board pull-up holds high)
}

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println("\n" FW_NAME " v" FW_VERSION);

  idleGroupA();

  // --- Power-up sequencing: let the FPGA finish configuring from its EPCS ---
  // The JTAG IDCODE read below drives a TAP-reset on the shared JTAG pins. That
  // is harmless on an ALREADY-configured FPGA, but if the ESP and the FPGA power
  // up together, this read can land WHILE the FPGA is still loading its bitstream
  // from the EPCS (~100-200 ms) and abort its configuration -> the FPGA never
  // starts, so no sound_link heartbeat. Symptom: "works only if the ESP is
  // powered before the FPGA". Waiting here lets the FPGA configure first,
  // regardless of power-on order. The bus stays Hi-Z (idleGroupA) meanwhile.
  jtag::begin();                 // pins to Hi-Z idle (does NOT touch the TAP)
  delay(1500);                   // >> EPCS config time; safe margin for any order

  if (!LittleFS.begin(true))
    Serial.println("[fs] LittleFS mount failed (run: pio run -t uploadfs)");
  tourney::begin();              // load saved tournament (players + scores) from LittleFS

  // --- Bring-up step 1: read the FPGA JTAG IDCODE on header P5 ---
  // Non-intrusive now that the FPGA is configured. Expect 0x020F10DD (10CL006).
  uint32_t id = jtag::readIdcode();
  Serial.printf("[jtag] IDCODE = 0x%08X  (%s)\n", (unsigned)id, jtag::idcodeName(id));
  netSetFpgaIdcode(id);

  netBegin();
#ifndef BOARD_C3
  wavplayer::begin();          // SD + PCM5102A I2S polyphonic WAV sound (S3 sound tier)
  romstore::begin();           // game-ROM image store on the SD (/roms/<NN>.img) — one-card foundation
  epromdump::begin();          // optional EPROM-reader daughterboard (no-op unless EPROM_READER_ENABLE)
  romdb::begin();              // known-good ROM checksum DB (/db/roms.csv) — verify dumps vs PinMAME
  ownership::begin();          // proof-of-ownership gate (verified dump -> unlock that game's sound)
#endif
  fpgalink::begin();           // UART from the FPGA Debug pin: diag-mode token (+ sound on S3)
  dispinject::begin();         // UART TX to FPGA Audio_RX: time-attack display digits (S3; no-op C3)
  oled::begin();               // optional SSD1306 status screen (skipped if absent)
  Serial.println("[boot] ready.");
}

void loop() {
  fpgalink::poll();            // refresh diag-mode (+ S3: play sounds) before diag::tick reads it
  netLoop();
  oled::tick();                // refresh the status screen (~2 Hz)
  delay(2);
}
