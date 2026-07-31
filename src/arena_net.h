#pragma once
#include <Arduino.h>

// WiFi (STA with SoftAP fallback) + mDNS + web UI / REST for the Arena LED wall.
namespace arenanet {

void begin();
void loop();
const char* ip();
const char* mode();          // "STA" / "SoftAP"

}  // namespace arenanet
