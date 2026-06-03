#include <SPI.h>
#include <string.h>
#include "board_config.h"
#include "norprog.h"

// W25Q opcodes
#define CMD_WREN   0x06
#define CMD_RDSR   0x05
#define CMD_READ   0x03
#define CMD_PP     0x02   // page program (<=256 B)
#define CMD_SE     0x20   // sector erase (4 KB)
#define CMD_JEDEC  0x9F

static SPIClass nspi(FSPI);
static const SPISettings CFG(8000000, MSBFIRST, SPI_MODE0);

static inline void csL() { digitalWrite(PIN_SPI_CS_SD, LOW); }
static inline void csH() { digitalWrite(PIN_SPI_CS_SD, HIGH); }
static inline void addr24(uint32_t a) { nspi.transfer((a >> 16) & 0xFF); nspi.transfer((a >> 8) & 0xFF); nspi.transfer(a & 0xFF); }

static void wren() { nspi.beginTransaction(CFG); csL(); nspi.transfer(CMD_WREN); csH(); nspi.endTransaction(); }

static uint8_t rdsr() {
  nspi.beginTransaction(CFG); csL(); nspi.transfer(CMD_RDSR);
  uint8_t s = nspi.transfer(0xFF); csH(); nspi.endTransaction(); return s;
}
static bool waitBusy(uint32_t to_ms = 3000) {
  uint32_t t = millis();
  while ((rdsr() & 0x01) && (millis() - t < to_ms)) delay(1);
  return (rdsr() & 0x01) == 0;
}
static void sectorErase(uint32_t a) {
  wren(); nspi.beginTransaction(CFG); csL(); nspi.transfer(CMD_SE); addr24(a); csH(); nspi.endTransaction(); waitBusy();
}
static void pageProgram(uint32_t a, const uint8_t *d, size_t n) {
  wren(); nspi.beginTransaction(CFG); csL(); nspi.transfer(CMD_PP); addr24(a);
  for (size_t i = 0; i < n; i++) nspi.transfer(d[i]);
  csH(); nspi.endTransaction(); waitBusy();
}
static void readData(uint32_t a, uint8_t *d, size_t n) {
  nspi.beginTransaction(CFG); csL(); nspi.transfer(CMD_READ); addr24(a);
  for (size_t i = 0; i < n; i++) d[i] = nspi.transfer(0xFF);
  csH(); nspi.endTransaction();
}

void norprog::begin() {}

bool norprog::enter() {
  // arbitration: hold the FPGA in reset so it tri-states the shared SPI bus
  pinMode(PIN_FPGA_RESET, OUTPUT); digitalWrite(PIN_FPGA_RESET, LOW);  // open-drain low
  delay(50);
  pinMode(PIN_SPI_CS_SD, OUTPUT); digitalWrite(PIN_SPI_CS_SD, HIGH);
  nspi.begin(PIN_SPI_SCLK, PIN_SPI_MISO, PIN_SPI_MOSI, PIN_SPI_CS_SD);
  // TODO: confirm the FPGA released the bus via the Debug-line handshake
  return true;
}

void norprog::leave() {
  nspi.end();
  pinMode(PIN_SPI_SCLK, INPUT); pinMode(PIN_SPI_MOSI, INPUT);
  pinMode(PIN_SPI_MISO, INPUT); pinMode(PIN_SPI_CS_SD, INPUT);
  pinMode(PIN_FPGA_RESET, INPUT);   // release reset -> FPGA reboots with new ROM
}

uint32_t norprog::jedecId() {
  nspi.beginTransaction(CFG); csL(); nspi.transfer(CMD_JEDEC);
  uint8_t a = nspi.transfer(0xFF), b = nspi.transfer(0xFF), c = nspi.transfer(0xFF);
  csH(); nspi.endTransaction();
  return ((uint32_t)a << 16) | ((uint32_t)b << 8) | c;
}

bool norprog::program(uint32_t addr, const uint8_t *data, size_t len, bool verify) {
  for (uint32_t a = (addr & ~0xFFFu); a < addr + len; a += 0x1000) sectorErase(a);
  size_t off = 0;
  while (off < len) {
    uint32_t a = addr + off;
    size_t n = 256 - (a & 0xFF);          // align to page boundary
    if (n > len - off) n = len - off;
    pageProgram(a, data + off, n);
    off += n;
  }
  if (verify) {
    uint8_t buf[256]; off = 0;
    while (off < len) {
      size_t n = (len - off > 256) ? 256 : (len - off);
      readData(addr + off, buf, n);
      if (memcmp(buf, data + off, n) != 0) return false;
      off += n;
    }
  }
  return true;
}
