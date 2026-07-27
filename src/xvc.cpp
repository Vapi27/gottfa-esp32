#include <WiFi.h>
#include "xvc.h"
#include "jtag.h"

// ---------------------------------------------------------------------------
// XVC 1.0 wire protocol (verified against Xilinx xvcServer + openFPGALoader
// src/xvc_client.cpp). Three commands stream back-to-back on one TCP socket;
// we parse by the fixed prefixes, never by packet boundaries.
//
//   "getinfo:"                      (8 ASCII bytes, no length)
//        -> reply ASCII "xvcServer_v1.0:<N>\n"  (<N> = max BYTES per shift vector).
//           The client splits on [_:] and REQUIRES exactly 3 tokens, then does
//           _buffer_size = N/2 (N covers tms+tdi together) -> max num_bits = (N/2)*8.
//
//   "settck:" + uint32 LE period_ns (7 + 4 bytes)
//        -> reply EXACTLY 4 bytes = the period actually set, uint32 LE (we echo it).
//
//   "shift:"  + uint32 LE num_bits + ceil(num_bits/8) TMS + ceil(num_bits/8) TDI
//              (6 + 4 + 2*numbytes bytes; num_bits = TCK toggles, NOT bytes)
//        -> reply EXACTLY ceil(num_bits/8) raw TDO bytes, no header/newline.
//
// All length fields are little-endian uint32. Bits within each TMS/TDI/TDO
// vector are LSB-first (bit i -> byte[i>>3] mask (1<<(i&7))). TDO is sampled
// before each TCK rising edge (handled inside jtag::shift -> clockBit).
// ---------------------------------------------------------------------------

static const uint16_t XVC_PORT     = 2542;
// Advertise 2048: client halves it -> 1024 bytes/direction -> num_bits <= 8192.
// NOTE: 8192 (4 KB vectors) made openFPGALoader's SVF player fail its TDO
// compares (deterministic "error at line B79") — some client-side buffer
// assumption breaks past 1024 B/vector. Keep 2048 until that's understood.
static const uint32_t XVC_ADV_SIZE = 2048;
static const uint32_t XVC_MAXBYTES = XVC_ADV_SIZE / 2;   // 1024 bytes per vector

static WiFiServer xvcServer(XVC_PORT);

// Idle timeout for a stalled session, both directions. Generous: an EPCS erase
// or a host-side pause can legitimately go quiet for a while; the primary exit
// is peer disconnect — the timer is a safety net so a half-open socket can
// never wedge the task with the TAP pins driven (that would fight the FPGA).
static const uint32_t XVC_IDLE_MS = 90000;

// True while a client session is being serviced. net.cpp's /jtag route checks
// this so a stray IDCODE read can never bit-bang the TAP mid-programming.
static volatile bool s_active = false;
bool xvc::active() { return s_active; }

// Per-connection scratch (one client at a time; sized to the advertised cap).
static uint8_t s_tms[XVC_MAXBYTES];
static uint8_t s_tdi[XVC_MAXBYTES];
static uint8_t s_tdo[XVC_MAXBYTES];

// Read EXACTLY need bytes, looping over partial TCP reads. Returns false on
// disconnect or a long idle timeout. Yields (~1 ms) while waiting.
static bool readExact(WiFiClient& c, uint8_t* buf, size_t need) {
  size_t got = 0;
  uint32_t t0 = millis();
  while (got < need) {
    int n = c.read(buf + got, need - got);
    if (n > 0) { got += n; t0 = millis(); continue; }
    if (!c.connected() && c.available() == 0) return false;   // peer closed, nothing buffered
    if (millis() - t0 > XVC_IDLE_MS) return false;            // stalled session
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  return true;
}

// Write EXACTLY len bytes, looping over partial sends. Same stall guard as
// readExact: a peer that keeps the socket open but stops reading (closed TCP
// window / killed client) must not spin here forever.
static bool writeAll(WiFiClient& c, const uint8_t* buf, size_t len) {
  size_t sent = 0;
  uint32_t t0 = millis();
  while (sent < len) {
    if (!c.connected()) return false;
    int n = c.write(buf + sent, len - sent);
    if (n > 0) { sent += n; t0 = millis(); continue; }
    if (millis() - t0 > XVC_IDLE_MS) return false;            // stalled peer
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  return true;
}

static inline uint32_t le32(const uint8_t* b) {
  return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

// Service one connected client until it disconnects or protocol-desyncs.
static void serviceClient(WiFiClient& c) {
  c.setNoDelay(true);   // XVC is tight request/reply RPC; Nagle would add ~40 ms/shift
  bool jtagOn = false;  // drive the TAP lazily, on the first shift of this session

  for (;;) {
    uint8_t c0;
    if (!readExact(c, &c0, 1)) break;   // clean disconnect between commands

    if (c0 == 'g') {                    // "getinfo:"
      uint8_t rest[7];
      if (!readExact(c, rest, 7)) break;                 // consume "etinfo:"
      char reply[40];
      int rn = snprintf(reply, sizeof(reply), "xvcServer_v1.0:%u\n", (unsigned)XVC_ADV_SIZE);
      if (!writeAll(c, (const uint8_t*)reply, (size_t)rn)) break;

    } else if (c0 == 's') {
      uint8_t c1;
      if (!readExact(c, &c1, 1)) break;

      if (c1 == 'e') {                  // "settck:" + uint32 LE period_ns
        uint8_t rest[5];
        if (!readExact(c, rest, 5)) break;               // consume "ttck:"
        uint8_t period[4];
        if (!readExact(c, period, 4)) break;
        if (!writeAll(c, period, 4)) break;              // echo the period we "set"

      } else if (c1 == 'h') {           // "shift:" + uint32 LE num_bits + tms + tdi
        uint8_t rest[4];
        if (!readExact(c, rest, 4)) break;               // consume "ift:"
        uint8_t nb[4];
        if (!readExact(c, nb, 4)) break;
        uint32_t num_bits = le32(nb);
        uint32_t numbytes = (num_bits + 7) >> 3;

        if (numbytes > XVC_MAXBYTES) {                   // client honoured N -> should never happen
          Serial.printf("[xvc] oversized shift %u bits (>%u bytes); dropping client\n",
                        (unsigned)num_bits, (unsigned)XVC_MAXBYTES);
          break;
        }
        if (!readExact(c, s_tms, numbytes)) break;
        if (!readExact(c, s_tdi, numbytes)) break;

        if (!jtagOn) {                     // drive the pins on the first shift
          jtag::enable(true);
          jtag::reset();                   // known TAP state (TLR->RTI); openFPGALoader
          jtagOn = true;                   // sends its own TLR preamble, this is insurance
        }
        jtag::shift(num_bits, s_tms, s_tdi, s_tdo);

        if (!writeAll(c, s_tdo, numbytes)) break;
      } else {
        Serial.printf("[xvc] bad command byte 0x%02X after 's'; dropping client\n", c1);
        break;
      }
    } else {
      Serial.printf("[xvc] unexpected command byte 0x%02X; dropping client\n", c0);
      break;
    }
  }

  if (jtagOn) jtag::enable(false);   // release the TAP back to Hi-Z for the FPGA
}

static void xvcTask(void*) {
  xvcServer.begin();
  xvcServer.setNoDelay(true);
  Serial.printf("[xvc] XVC JTAG server on :%u  (openFPGALoader --cable xvc-client --port %u)\n",
                (unsigned)XVC_PORT, (unsigned)XVC_PORT);
  for (;;) {
    WiFiClient c = xvcServer.available();
    if (c) {
      Serial.printf("[xvc] client %s connected\n", c.remoteIP().toString().c_str());
      s_active = true;                 // blocks the /jtag HTTP route for the session
      serviceClient(c);
      s_active = false;
      c.stop();
      Serial.println("[xvc] client disconnected");
    }
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

void xvc::begin() {
  // 8 KB stack: raw WiFiClient I/O runs the LwIP call chain on our stack (the
  // repo's other socket-owning tasks use 8192 too).
  xTaskCreatePinnedToCore(xvcTask, "xvc", 8192, nullptr, 1, nullptr, 0);   // core 0 = WiFi core
}
