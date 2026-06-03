#pragma once
#include <Arduino.h>

// SPI NOR (W25Q-style) programmer for the GottFA game-ROM store.
// Shares the FPGA's SD/EEPROM SPI bus: enter() arbitrates by holding the FPGA in
// reset (it tri-states the bus), then the ESP is SPI master. leave() releases the
// bus + reset so the FPGA reboots and reads the new image.
namespace norprog {
  void     begin();
  bool     enter();                                            // take the bus (FPGA reset)
  void     leave();                                            // release -> FPGA reboots
  uint32_t jedecId();                                          // 0xEF4016 = W25Q32
  bool     program(uint32_t addr, const uint8_t *data, size_t len, bool verify = true);
}
