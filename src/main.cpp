#include <Arduino.h>
#include <LittleFS.h>
#include "board_config.h"
#include "jtag.h"
#include "net.h"

// In NORMAL mode the FPGA is master of the shared SD/EEPROM SPI bus, so the ESP
// must present Hi-Z on those lines until it is explicitly granted the bus.
static void idleGroupA() {
  pinMode(PIN_SPI_SCLK,   INPUT);
  pinMode(PIN_SPI_MOSI,   INPUT);
  pinMode(PIN_SPI_MISO,   INPUT);
  pinMode(PIN_SPI_CS_SD,  INPUT);
  pinMode(PIN_FPGA_DEBUG, INPUT);
  pinMode(PIN_FPGA_RESET, INPUT);   // open-drain released (board pull-up holds high)
}

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println("\n" FW_NAME " v" FW_VERSION);

  idleGroupA();

  if (!LittleFS.begin(true))
    Serial.println("[fs] LittleFS mount failed (run: pio run -t uploadfs)");

  // --- Bring-up step 1: read the FPGA JTAG IDCODE on header P5 ---
  // Non-intrusive even on a running FPGA. Expect 0x020F10DD for the 10CL006.
  jtag::begin();
  uint32_t id = jtag::readIdcode();
  Serial.printf("[jtag] IDCODE = 0x%08X  (%s)\n", (unsigned)id, jtag::idcodeName(id));
  netSetFpgaIdcode(id);

  netBegin();
  Serial.println("[boot] ready.");
}

void loop() {
  netLoop();
  delay(2);
}
