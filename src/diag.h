#pragma once
#include <Arduino.h>
class AsyncWebSocket;
class AsyncWebSocketClient;

// LISYcontrol diagnostic backend (WebSocket <-> lisyctrl SPI register map).
// v0.2 runs a MOCK so the whole UI works with no FPGA; every command/readback
// is marked with the lisyctrl register it binds to once the SPI bridge exists.
namespace diag {
  void begin();
  void setInfo(const char *fw, uint32_t idcode, const char *mode, const char *ip);
  void attach(AsyncWebSocket *ws);
  void onConnect(AsyncWebSocketClient *c);
  void onText(AsyncWebSocketClient *c, const char *data, size_t len);
  void tick();   // call from loop()
}
