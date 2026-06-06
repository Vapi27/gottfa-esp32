#pragma once
#include <Arduino.h>

void netSetFpgaIdcode(uint32_t id);   // call before netBegin() to show it on the page
void netBegin();                      // WiFi (STA -> SoftAP fallback) + mDNS + HTTP
void netLoop();                       // call from loop()
const char* netIp();                  // current IP (for the OLED/status)
const char* netMode();                // "STA" / "SoftAP"
