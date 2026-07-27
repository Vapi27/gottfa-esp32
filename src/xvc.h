#pragma once

// Xilinx Virtual Cable (XVC 1.0) server for the shared Cyclone 10 JTAG TAP.
// Lets openFPGALoader program the FPGA over WiFi:
//   openFPGALoader --cable xvc-client --ip <esp-ip> --port 2542 file.sof   (SRAM, needs .rbf/.svf)
//   openFPGALoader --cable xvc-client --ip <esp-ip> --port 2542 -f ...     (EPCS flash)
//
// Runs in its OWN FreeRTOS task (WiFiServer on core 0, the WiFi/LwIP core), so it
// coexists with the AsyncWebServer(:80). It bit-bangs the SAME 4 TAP pins as
// jtag::readIdcode() / the /jtag HTTP route, so do NOT drive JTAG from elsewhere
// while an XVC client is connected (single shared TAP).
namespace xvc {
  void begin();   // start the WiFiServer task; call AFTER netBegin() (WiFi must be up)
  bool active();  // true while a client session is live — other TAP users must back off
}
