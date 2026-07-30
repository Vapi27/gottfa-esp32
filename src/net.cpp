#include <WiFi.h>
#include "driver/gpio.h"
#include <LittleFS.h>
#include <ESPmDNS.h>
#include <ESPAsyncWebServer.h>
#include <Update.h>
#include <string.h>       // strcmp/strncpy — used by /routes and the job slot on BOTH targets
#include <SPI.h>          // SPIClass/SPISettings: used by /nor, /norloop and /sdprobe.
                          // It used to arrive only transitively through <SD.h>, which is
                          // #ifndef BOARD_C3, so the C3 build did not see it at all.
#include "board_config.h"
#include "net.h"
#include "diag.h"
#include "jtag.h"
#include "xvc.h"
#include "norprog.h"
#include "fpgalink.h"
#include "dispinject.h"
#include "coiltest.h"
#include "wifiprov.h"
#ifndef BOARD_C3
#include <SD.h>
#endif
#ifndef BOARD_C3
#include "wavplayer.h"
#include "romstore.h"
#include "romcrypt.h"
#include "epromdump.h"
#include "romdb.h"
#include "ownership.h"
#include <string.h>
// scratch buffer for a /romup POST body (auto-freed by AsyncWebServerRequest::_tempObject)
struct RomUp { uint32_t cap; uint32_t got; uint8_t data[1]; };
// /wavup streaming state — tracks bytes written vs received to detect a mid-write SD dropout.
//
// NOT stored in AsyncWebServerRequest::_tempObject any more (2026-07-27).
// ESPAsyncWebServer 3.11.0 frees that pointer with plain free():
//     ~AsyncWebServerRequest()  -> src/WebRequest.cpp:113-115
//         if (_tempObject != NULL) { free(_tempObject); }
// i.e. the library contract is "_tempObject must be malloc-family memory".  This
// struct is not: `File` holds a std::shared_ptr<VFSFileImpl> and `String` owns a
// heap buffer, so free()ing it skips both destructors.  The old code did
// `new WavUp()` and relied on its own completion handler running `delete u`
// first -- which it does on the happy path, but NOT when the client aborts, the
// socket drops, or the request is destroyed before completion.  On those paths
// the String buffer leaked, the shared_ptr control block leaked, and the SD file
// handle was never closed (a permanently lost FatFS max_files slot) leaving a
// half-written file on the card.
//
// A single module-owned slot instead: /wavup writes to ONE file on ONE SD card,
// so two concurrent uploads were never going to work anyway, and this also gives
// somewhere to hang a janitor that cleans up after an abandoned upload -- which
// nothing did before, because the library gives no "request destroyed" callback
// on this path.
struct WavUp { File f; String path; size_t total; size_t written; bool opened; bool failed; };
static WavUp   s_wav;
static bool    s_wav_busy = false;      // slot in use by an upload in progress
static uint32_t s_wav_touch = 0;        // millis() of the last body chunk

// Close the file, drop a partial upload, and release the slot.
static void wavRelease(bool keepFile) {
  if (s_wav.f) { s_wav.f.flush(); s_wav.f.close(); }
  if (!keepFile && s_wav.path.length()) SD.remove(s_wav.path);
  s_wav.path = String();
  s_wav.total = s_wav.written = 0;
  s_wav.opened = s_wav.failed = false;
  s_wav_busy = false;
}

// Called from netLoop(): reclaim a slot whose client vanished mid-upload.
static void wavJanitor() {
  if (!s_wav_busy) return;
  if (millis() - s_wav_touch < 20000) return;      // 20 s without a chunk = gone
  Serial.printf("[wavup] abandoned upload of %s after %u/%u bytes - cleaning up\n",
                s_wav.path.c_str(), (unsigned)s_wav.written, (unsigned)s_wav.total);
  wavRelease(false);
}
#endif

// Dead-man timer for the two diagnostics that PARK THE HARDWARE: /norhold (drives
// CS/CLK/MOSI low) and /norloop (repeats a JEDEC read). Both also hold the FPGA in
// reset, i.e. the pinball is dead while they are engaged, and before v1 nothing
// ever undid that. 0 = nothing held; otherwise the millis() when the hold started.
static uint32_t s_holdSince = 0;
static const uint32_t HOLD_MAX_MS = 300000;   // 5 min, then netLoop() releases

// /fsup upload slot (see the upload handler): one File, one uploader at a time.
static bool     s_fsup_busy  = false;
static uint32_t s_fsup_touch = 0;
static File    *s_fsup_file  = nullptr;

static AsyncWebServer server(80);
static AsyncWebSocket ws("/ws");
static uint32_t s_idcode = 0;
static String   s_mode = "init";
static String   s_ip   = "0.0.0.0";

void netSetFpgaIdcode(uint32_t id) { s_idcode = id; }

// ===========================================================================
// DEFERRED JOB SLOT   (2026-07-27)
// ===========================================================================
// WHY.  Every route handler, body handler, upload handler and WebSocket
// callback registered on an AsyncWebServer runs on AsyncTCP's single
// `async_tcp` service task -- priority 10, no core affinity, and it is also the
// LwIP event pump.  Anything slow in a handler therefore stalls ALL http, ALL
// websocket traffic and the TCP event queue (64 deep, then events are dropped),
// at a priority above everything else in this firmware.  If the stall starves
// IDLE0 for 5 s the task watchdog panics and reboots -- and because the task has
// no core affinity, whether a given stall lands on the monitored idle task is
// non-deterministic between runs.  This project has already been bitten once:
// a 30 kHz bit-bang inside a handler rebooted the ESP mid-request and triggered
// a silent OTA rollback (see NOR_W25Q32 bring-up notes).
//
// The offenders are the NOR programming routes (a 16 KB /norflash or /norbig is
// 4 sector erases at up to 400 ms each + 64 page programs + a byte-at-a-time
// verify: 0.5-2 s), /nor (~100 ms of SPI probing), /sdprobe, /grant (410 ms, of
// which 320 ms is a hard no-yield digitalRead busy-wait) and /verify (CRC32 over
// a whole file at 1 MHz SPI + a linear CSV scan).
//
// THE FIX.  One job slot.  Handlers validate their parameters, claim the slot,
// and return 202 immediately with a job id; netLoop() -- which runs on the
// Arduino loopTask at priority 1 -- executes the job and stores its JSON result;
// GET /jobstatus?id=N polls it.  The URLs are unchanged, so existing scripts and
// bookmarks still work; only the response shape changes, and every 202 carries
// the poll URL.
//
// A SINGLE slot on purpose: all of these routes drive the same shared SPI bus
// (PIN_SPI_SCLK/MISO/MOSI/CS + the PIN_FPGA_RESET bus grant), so they were never
// safe to overlap.  A second request while one is running gets 409 instead of
// silently corrupting the first.  Running them from loopTask additionally
// serialises them against diag::tick() and the /norloop burst, which already
// bit-bang the same pins from that task -- that race is gone too.
//
// This mirrors the mechanism the sound path already uses (wavplayer's FreeRTOS
// request queue drained by mixTask); nothing new is being invented here.
// ===========================================================================
enum JobKind : uint8_t {
  JOB_NONE = 0, JOB_NOR, JOB_NORWRITE, JOB_NORBIG, JOB_NORFLASH,
  JOB_SDPROBE, JOB_GRANT, JOB_VERIFY
};
// JS_CLAIMED = the slot belongs to a handler that is still filling in its
// arguments; the runner must NOT start it yet.  jobAccepted() flips it to
// JS_PENDING as the last thing it does, so a job can never be picked up with a
// half-written payload / path.
enum JobState : uint8_t { JS_IDLE = 0, JS_CLAIMED, JS_PENDING, JS_RUNNING, JS_DONE };

struct Job {
  volatile JobState state   = JS_IDLE;
  JobKind           kind    = JOB_NONE;
  uint32_t          id      = 0;
  uint32_t          t_start = 0;
  uint32_t          ms      = 0;
  uint32_t          arg     = 0;        // /norflash: target address
  void             *payload = nullptr;  // /norflash: the RomUp buffer we took ownership of
  uint32_t          len     = 0;        // /norflash: payload bytes
  char              path[64];           // /verify: file to check
  char              result[400];        // JSON produced by the runner
};
static Job     s_job;
static uint32_t s_job_seq = 0;
static portMUX_TYPE s_job_mux = portMUX_INITIALIZER_UNLOCKED;

static const char *jobKindName(JobKind k) {
  switch (k) {
    case JOB_NOR:      return "nor";
    case JOB_NORWRITE: return "norwrite";
    case JOB_NORBIG:   return "norbig";
    case JOB_NORFLASH: return "norflash";
    case JOB_SDPROBE:  return "sdprobe";
    case JOB_GRANT:    return "grant";
    case JOB_VERIFY:   return "verify";
    default:           return "none";
  }
}
static const char *jobStateName(JobState s) {
  switch (s) {
    case JS_CLAIMED: return "pending";
    case JS_PENDING: return "pending";
    case JS_RUNNING: return "running";
    case JS_DONE:    return "done";
    default:         return "idle";
  }
}

// Claim the slot from the async task.  Returns 0 if a job is still pending or
// running.  A DONE job's result is kept until the next claim overwrites it, so
// there is always exactly one result to poll.
static uint32_t jobClaim(JobKind k) {
  uint32_t id = 0;
  portENTER_CRITICAL(&s_job_mux);
  if (s_job.state == JS_IDLE || s_job.state == JS_DONE) {
    s_job.kind = k;
    s_job.id   = ++s_job_seq;
    s_job.ms   = 0;
    s_job.arg  = 0;
    s_job.len  = 0;
    s_job.path[0]   = 0;
    s_job.result[0] = 0;
    // the runner always frees and clears payload before it sets JS_DONE, so
    // nothing can leak here; the assignment is belt and braces.
    s_job.payload = nullptr;
    s_job.state   = JS_CLAIMED;      // armed by jobAccepted(), see JS_CLAIMED
    id = s_job.id;
  }
  portEXIT_CRITICAL(&s_job_mux);
  return id;
}

static void jobAccepted(AsyncWebServerRequest *r, uint32_t id, const char *what) {
  char j[240];
  snprintf(j, sizeof(j),
    "{\"accepted\":true,\"async\":true,\"job\":%u,\"kind\":\"%s\","
    "\"poll\":\"/jobstatus?id=%u\","
    "\"note\":\"runs from the main loop; poll until state==done, then read result\"}",
    (unsigned)id, what, (unsigned)id);
  s_job.state = JS_PENDING;          // arm LAST: all arguments are now in place
  r->send(202, "application/json", j);
}

static void jobBusy(AsyncWebServerRequest *r) {
  char j[200];
  snprintf(j, sizeof(j),
    "{\"accepted\":false,\"busy\":true,\"job\":%u,\"kind\":\"%s\",\"state\":\"%s\","
    "\"poll\":\"/jobstatus\"}",
    (unsigned)s_job.id, jobKindName(s_job.kind), jobStateName(s_job.state));
  r->send(409, "application/json", j);
}

// ---------------------------------------------------------------------------
// The job bodies.  These are the ORIGINAL handler bodies, moved verbatim except
// for writing their JSON into s_job.result instead of calling r->send().  They
// now run on loopTask, so a multi-second erase no longer stalls the network.
// ---------------------------------------------------------------------------

// /norflash — program the uploaded image into the W25Q at s_job.arg
static void runNorFlash() {
  bool ok = false;
  uint32_t got = s_job.len;
  if (s_job.payload && got > 0 && got <= 0x4000) {
    const uint8_t *img = (const uint8_t *)s_job.payload;
    norprog::enter();
    uint32_t id = norprog::jedecId();
    ok = (id == 0xEF4016UL) && norprog::program(s_job.arg, img, got, true);
    norprog::leave();
  }
  if (s_job.payload) { free(s_job.payload); s_job.payload = nullptr; }
  snprintf(s_job.result, sizeof(s_job.result),
           "{\"ok\":%s,\"addr\":\"0x%06X\",\"bytes\":%u}",
           ok ? "true" : "false", (unsigned)s_job.arg, (unsigned)got);
}

// /norbig — 16 KB erase+program+verify at 0x3F0000 (production-size write test)
static void runNorBig() {
  const uint32_t A = 0x3F0000UL;
  const size_t   N = 0x4000;                 // 16 KB
  uint8_t *buf = (uint8_t *)malloc(N);
  if (!buf) {
    snprintf(s_job.result, sizeof(s_job.result),
             "{\"ok\":false,\"err\":\"out of heap for the 16KB pattern\"}");
    return;
  }
  for (size_t i = 0; i < N; i++) {
    uint32_t a = A + i;
    buf[i] = (uint8_t)((a ^ (a >> 5) ^ (a >> 11) ^ 0x5A) & 0xFF);
  }
  norprog::enter();
  uint32_t id = norprog::jedecId();
  bool ok = (id == 0xEF4016UL) && norprog::program(A, buf, N, true);
  norprog::leave();
  free(buf);
  snprintf(s_job.result, sizeof(s_job.result),
           "{\"ok\":%s,\"jedec\":\"0x%06X\",\"addr\":\"0x3F0000\",\"bytes\":%u,"
           "\"sectors\":%u,\"pages\":%u,\"verdict\":\"%s\"}",
           ok ? "true" : "false", (unsigned)id, (unsigned)N, (unsigned)(N / 0x1000),
           (unsigned)(N / 256),
           ok ? "16KB ERASE+PROGRAM+VERIFY OK - production-size write proven"
              : (id != 0xEF4016UL ? "chip not found" : "verify FAILED at some page"));
}

// /norwrite — 512 B erase+program+verify in the last 4 KB sector
static void runNorWrite() {
  const uint32_t A = 0x3FF000UL;
  const size_t   N = 512;
  static uint8_t pat[N];
  for (size_t i = 0; i < N; i++) {
    uint32_t a = A + i;
    pat[i] = (uint8_t)((a ^ (a >> 8) ^ 0xA5) & 0xFF);
  }
  norprog::enter();
  uint32_t id = norprog::jedecId();
  if (id != 0xEF4016UL) {
    norprog::leave();
    snprintf(s_job.result, sizeof(s_job.result),
             "{\"ok\":false,\"jedec\":\"0x%06X\",\"err\":\"chip not found\"}", (unsigned)id);
    return;
  }
  bool ok = norprog::program(A, pat, N, true);       // erase + program + verify
  norprog::leave();
  snprintf(s_job.result, sizeof(s_job.result),
           "{\"ok\":%s,\"jedec\":\"0x%06X\",\"addr\":\"0x3FF000\",\"bytes\":%u,"
           "\"verdict\":\"%s\"}",
           ok ? "true" : "false", (unsigned)id, (unsigned)N,
           ok ? "ERASE+PROGRAM+VERIFY OK - norprog chain proven"
              : "verify FAILED - data did not stick");
}

// /nor — breadboard/machine JEDEC probe, both D0/D1 assignments, 3 clock rates
static void runNorProbe() {
  pinMode(PIN_FPGA_RESET, OUTPUT); digitalWrite(PIN_FPGA_RESET, LOW);   // bus grant
  delay(60);                                                             // tri-state + settle
  // NOT named HZ: the RISC-V newlib headers pulled in by the C3 build define HZ as
  // a macro, so `static const uint32_t HZ[]` expanded to garbage and was one of the
  // three errors that made `pio run -e esp32c3` broken.
  static const uint32_t PROBE_HZ[] = {1000000, 400000, 100000};
  uint32_t best = 0; int bestHz = 0; bool bestSwap = false;
  uint32_t seen[2][3] = {{0}};

  for (int sw = 0; sw < 2 && !bestHz; sw++) {
    int miso = sw ? PIN_SPI_MOSI : PIN_SPI_MISO;
    int mosi = sw ? PIN_SPI_MISO : PIN_SPI_MOSI;
    for (int k = 0; k < 3; k++) {
      SPIClass s(FSPI);
      pinMode(PIN_SPI_CS_SD, OUTPUT); digitalWrite(PIN_SPI_CS_SD, HIGH);
      s.begin(PIN_SPI_SCLK, miso, mosi, PIN_SPI_CS_SD);
      s.beginTransaction(SPISettings(PROBE_HZ[k], MSBFIRST, SPI_MODE0));
      digitalWrite(PIN_SPI_CS_SD, LOW);
      s.transfer(0x9F);
      uint32_t v = 0;
      for (int i = 0; i < 3; i++) v = (v << 8) | s.transfer(0x00);
      digitalWrite(PIN_SPI_CS_SD, HIGH);
      s.endTransaction(); s.end();
      seen[sw][k] = v;
      if (v == 0xEF4016UL) { best = v; bestHz = PROBE_HZ[k]; bestSwap = sw; break; }
      delay(2);
    }
  }

  // liveness: with /CS asserted and no clock, a working chip holds DO; nothing
  // connected leaves the pin floating (reads as whatever the ESP pull gives).
  pinMode(PIN_SPI_CS_SD, OUTPUT); digitalWrite(PIN_SPI_CS_SD, HIGH);
  pinMode(PIN_SPI_MISO, INPUT_PULLDOWN); delayMicroseconds(300);
  uint8_t pd_hi = digitalRead(PIN_SPI_MISO);
  digitalWrite(PIN_SPI_CS_SD, LOW);      delayMicroseconds(300);
  uint8_t pd_lo = digitalRead(PIN_SPI_MISO);
  pinMode(PIN_SPI_MISO, INPUT_PULLUP);   delayMicroseconds(300);
  uint8_t pu_lo = digitalRead(PIN_SPI_MISO);
  digitalWrite(PIN_SPI_CS_SD, HIGH);
  pinMode(PIN_SPI_MISO, INPUT); pinMode(PIN_SPI_CS_SD, INPUT);
  pinMode(PIN_FPGA_RESET, INPUT);   // release the grant -> FPGA reboots + reloads ROM

  bool floating = (pd_lo == 0) && (pu_lo == 1);
  const char *verdict =
    bestHz    ? (bestSwap ? "OK but SWAPPED - D0 to GPIO11, D1 to GPIO13"
                          : "OK - W25Q32 alive, wiring as documented") :
    floating  ? "MISO floats even when selected - the module is not driving: dead chip, "
                "cold joint on its own PCB, or a breadboard contact" :
                "MISO is driven but the id is wrong - read the marking on the chip";
  snprintf(s_job.result, sizeof(s_job.result),
           "{\"expect\":\"0xEF4016\",\"found\":\"0x%06X\",\"okAtHz\":%u,\"swapped\":%s,"
           "\"normal\":[\"0x%06X\",\"0x%06X\",\"0x%06X\"],"
           "\"swap\":[\"0x%06X\",\"0x%06X\",\"0x%06X\"],"
           "\"miso_pulldown_csHigh\":%u,\"miso_pulldown_csLow\":%u,\"miso_pullup_csLow\":%u,"
           "\"floating\":%s,\"verdict\":\"%s\"}",
           (unsigned)best, (unsigned)bestHz, bestSwap ? "true" : "false",
           (unsigned)seen[0][0], (unsigned)seen[0][1], (unsigned)seen[0][2],
           (unsigned)seen[1][0], (unsigned)seen[1][1], (unsigned)seen[1][2],
           (unsigned)pd_hi, (unsigned)pd_lo, (unsigned)pu_lo,
           floating ? "true" : "false", verdict);
}

// /grant — count CLK/MOSI edges with the bus grant released vs asserted
static void runGrant() {
  auto countEdges = [](int pin, uint32_t ms) -> uint32_t {
    pinMode(pin, INPUT);
    uint32_t n = 0, t0 = millis();
    int last = digitalRead(pin);
    while (millis() - t0 < ms) {
      int v = digitalRead(pin);
      if (v != last) { n++; last = v; }
    }
    return n;
  };
  pinMode(PIN_FPGA_RESET, INPUT);                  // grant released
  delay(30);
  uint32_t clk_free  = countEdges(PIN_SPI_SCLK, 80);
  uint32_t mosi_free = countEdges(PIN_SPI_MOSI, 80);
  pinMode(PIN_FPGA_RESET, OUTPUT); digitalWrite(PIN_FPGA_RESET, LOW);   // grant asserted
  delay(60);
  uint32_t clk_grant  = countEdges(PIN_SPI_SCLK, 80);
  uint32_t mosi_grant = countEdges(PIN_SPI_MOSI, 80);
  pinMode(PIN_FPGA_RESET, INPUT);                  // release
  const char *verdict =
    (clk_free > 10 && clk_grant <= 2) ? "GRANT WORKS - FPGA traffic stops when asserted" :
    (clk_free <= 2 && clk_grant <= 2) ? "bus always quiet - FPGA idle (no SD?) or old bitstream in reset-loop" :
    (clk_grant > 10)                  ? "GRANT FAILS - FPGA still drives CLK while granted (old bitstream?)" :
                                        "ambiguous - re-run";
  snprintf(s_job.result, sizeof(s_job.result),
           "{\"clk_edges_free\":%u,\"mosi_edges_free\":%u,\"clk_edges_granted\":%u,"
           "\"mosi_edges_granted\":%u,\"verdict\":\"%s\"}",
           (unsigned)clk_free, (unsigned)mosi_free, (unsigned)clk_grant,
           (unsigned)mosi_grant, verdict);
}

#ifndef BOARD_C3
// /sdprobe — mount the FPGA's own game-ROM card on the shared bus.
//
// BUG FIXED 2026-07-27.  This used to call SD.begin(PIN_SPI_CS_SD, bus, ...) on
// the GLOBAL SD object, which wavplayer::begin() has already mounted on the
// SOUND card.  Arduino's SDFS::begin() starts with
//     if (_pdrv != 0xFF) { return true; }
// (framework-arduinoespressif32 libraries/SD/src/SD.cpp), so the call returned
// true instantly WITHOUT touching the requested pins: the route reported the
// sound card's size as if it were the FPGA card -- a false positive every time
// -- and then SD.end() really did unmount the sound card, so every mixTask read
// failed until the next reboot.  The probe now refuses to run while the sound
// card is mounted and says so, instead of lying and breaking the sound.
static void runSdProbe() {
  if (wavplayer::ready()) {
    snprintf(s_job.result, sizeof(s_job.result),
             "{\"ok\":false,\"err\":\"sound SD is mounted on the global SD object\","
             "\"why\":\"SDFS::begin() returns true without remounting, so this probe would "
             "report the SOUND card and SD.end() would unmount it\","
             "\"do\":\"power up with the sound card removed, or use /nor for a bus check\"}");
    return;
  }
  pinMode(PIN_FPGA_RESET, OUTPUT); digitalWrite(PIN_FPGA_RESET, LOW);   // FPGA held -> bus free
  delay(50);
  SPIClass bus(HSPI);
  bus.begin(PIN_SPI_SCLK, PIN_SPI_MISO, PIN_SPI_MOSI, PIN_SPI_CS_SD);
  bool ok = SD.begin(PIN_SPI_CS_SD, bus, 400000, "/fpgasd", 2);
  uint64_t sz = ok ? SD.cardSize() / (1024ULL * 1024ULL) : 0;
  int type = ok ? (int)SD.cardType() : -1;
  if (ok) SD.end();
  bus.end();
  pinMode(PIN_SPI_SCLK, INPUT); pinMode(PIN_SPI_MOSI, INPUT);           // back to Hi-Z
  pinMode(PIN_SPI_MISO, INPUT); pinMode(PIN_SPI_CS_SD, INPUT);
  pinMode(PIN_FPGA_RESET, INPUT);                                       // release the FPGA (reboots)
  snprintf(s_job.result, sizeof(s_job.result),
           "{\"ok\":%s,\"type\":%d,\"sizeMB\":%llu}",
           ok ? "true" : "false", type, (unsigned long long)sz);
}

// /verify — CRC32 a stored file and look it up in the known-good ROM DB
static void runVerify() {
  romdb::Match m;
  bool ok = romdb::identifyFile(s_job.path, &m);
  if (!m.crc && !ok) {
    snprintf(s_job.result, sizeof(s_job.result),
             "{\"ok\":false,\"err\":\"file not found\",\"path\":\"%s\"}", s_job.path);
    return;
  }
  snprintf(s_job.result, sizeof(s_job.result),
           "{\"ok\":true,\"crc\":\"%08x\",\"known\":%d,\"game\":\"%s\",\"title\":\"%s\"}",
           (unsigned)m.crc, m.found ? 1 : 0, m.game, m.title);
}
#endif

// Executed from netLoop() on loopTask.  One job at a time, no preemption of the
// network stack.
static void jobRun() {
  if (s_job.state != JS_PENDING) return;
  s_job.state   = JS_RUNNING;
  s_job.t_start = millis();
  switch (s_job.kind) {
    case JOB_NOR:      runNorProbe(); break;
    case JOB_NORWRITE: runNorWrite(); break;
    case JOB_NORBIG:   runNorBig();   break;
    case JOB_NORFLASH: runNorFlash(); break;
    case JOB_GRANT:    runGrant();    break;
#ifndef BOARD_C3
    case JOB_SDPROBE:  runSdProbe();  break;
    case JOB_VERIFY:   runVerify();   break;
#endif
    default:
      snprintf(s_job.result, sizeof(s_job.result), "{\"ok\":false,\"err\":\"unknown job\"}");
      break;
  }
  if (s_job.payload) { free(s_job.payload); s_job.payload = nullptr; }   // never leak
  s_job.ms    = millis() - s_job.t_start;
  s_job.state = JS_DONE;
  Serial.printf("[job] #%u %s done in %u ms\n",
                (unsigned)s_job.id, jobKindName(s_job.kind), (unsigned)s_job.ms);
}

// ===========================================================================
// ROUTE INDEX   (GET /routes)
// ===========================================================================
// Every HTTP route this firmware answers, with the group it belongs to. The
// board is its own documentation: a machine recovered years from now can be
// asked what it can do, and nobody has to guess whether /norwrite is safe to
// click. Groups:
//
//   product     everyday use. The web UI drives these. Safe.
//   diagnostic  bench / bring-up instruments. Read-only or momentary; they may
//               briefly take the shared SPI bus, which resets the FPGA.
//   service     CHANGES PERSISTENT STATE (firmware, NOR flash, ROM store, the
//               web UI itself) or holds hardware lines. Not for a customer.
//
// This table is documentation only -- it does not register anything. Keep it in
// step with the server.on() calls below; /routes is the thing people will read.
struct RouteDoc { const char *path; const char *grp; const char *desc; };
static const RouteDoc ROUTES[] = {
  // ---- product ----------------------------------------------------------
  { "/",              "product", "web UI (LittleFS index.html)" },
  { "/ws",            "product", "WebSocket: live LISYcontrol state + commands" },
  { "/sysinfo",       "product", "board identity: firmware, git, FPGA, WiFi, memory" },
  { "/link",          "product", "FPGA UART telemetry: diag flag, game in progress, ball" },
  { "/routes",        "product", "this index" },
  { "/wifi",          "product", "provisioning portal (also served on the captive AP)" },
  { "/wifi/status",   "product", "WiFi state, current SSID, last failure reason" },
  { "/wifi/scan",     "product", "cached 2.4 GHz scan for the portal" },
  { "/wifi/connect",  "product", "POST ssid+pass -> store in NVS and join" },
  { "/wifi/aponly",   "product", "POST: stay a hotspot for ever" },
  { "/wifi/appass",   "product", "POST: change the hotspot password" },
  { "/wifi/forget",   "product", "POST: erase stored credentials (factory WiFi reset)" },
  { "/snd",           "product", "PSOWAV: play a sound / load a game set / status" },
  { "/game",          "product", "select a game's sound set by FPGA game number" },
  { "/roms",          "product", "ROM store contents + device key + Free-Play flag" },
  { "/fp",            "product", "read/set the Free-Play variant served to the FPGA" },
  { "/owned",         "product", "ownership gate: list / toggle / add" },
  { "/jobstatus",     "product", "poll a deferred job started by a service route" },
  // ---- diagnostic -------------------------------------------------------
  { "/beep",          "diagnostic", "440 Hz sine straight to the DAC (isolates I2S from the SD)" },
  { "/sndtrace",      "diagnostic", "sound-command capture ring (CSV/JSON) -- map a title's cues" },
  { "/ramsnap",       "diagnostic", "640-byte game RAM snapshot over the FPGA link" },
  { "/dispinj",       "diagnostic", "drive the score glass by hand (time-attack must be disarmed)" },
  { "/coiltest",      "diagnostic", "solenoid test by switch feedback: learn/replay a signature" },
  { "/jtag",          "diagnostic", "re-read the FPGA JTAG IDCODE" },
  { "/pin",           "diagnostic", "read the level of a machine-wired input (GPIO 14 or 8)" },
  { "/led",           "diagnostic", "force the status LED to a colour -- identify this board" },
  { "/nor",           "diagnostic", "probe the W25Q32 NOR (takes the bus: RESETS THE FPGA)" },
  { "/sdprobe",       "diagnostic", "probe the FPGA's game-ROM SD card (RESETS THE FPGA)" },
  { "/grant",         "diagnostic", "verify the FPGA releases the shared bus (RESETS THE FPGA)" },
  { "/verify",        "diagnostic", "CRC32 a stored dump against the known-good ROM DB" },
  { "/dump",          "diagnostic", "EPROM-reader daughterboard: dump a chip (off by default)" },
  { "/norloop",       "diagnostic", "repeat the NOR JEDEC read for a scope; auto-stops after 5 min" },
  { "/norhold",       "diagnostic", "hold CS/CLK/MOSI low for a multimeter; auto-releases after 5 min" },
  { "/norrelease",    "diagnostic", "release what /norhold and /norloop are holding" },
  // ---- service ----------------------------------------------------------
  { "/ota",           "service", "POST a firmware .bin -> flash + reboot (UNSIGNED)" },
  { "/fsup",          "service", "POST a file -> LittleFS (this is how the web UI is replaced)" },
  { "/romup",         "service", "POST a 16384-byte game ROM -> encrypted into the store" },
  { "/romdel",        "service", "delete a game slot from the ROM store" },
  { "/wavup",         "service", "POST a WAV -> the sound SD card" },
  { "/norflash",      "service", "POST an image -> program it into the NOR (erase+verify)" },
  { "/norwrite",      "service", "NOR self-test: erase+program+verify the last 4 KB sector" },
  { "/norbig",        "service", "NOR self-test: erase+program+verify a whole 16 KB game slot" },
};
static const size_t N_ROUTES = sizeof(ROUTES) / sizeof(ROUTES[0]);

static void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
                      AwsEventType type, void *arg, uint8_t *data, size_t len) {
  switch (type) {
    case WS_EVT_CONNECT:    Serial.printf("[ws] #%u connected\n", client->id()); diag::onConnect(client); break;
    case WS_EVT_DISCONNECT: Serial.printf("[ws] #%u left\n", client->id()); break;
    case WS_EVT_DATA: {
      AwsFrameInfo *info = (AwsFrameInfo *)arg;
      if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT)
        diag::onText(client, (const char *)data, len);
      break;
    }
    default: break;
  }
}

void netBegin() {
  // WiFi: credentials come from NVS via the provisioning wizard, never from a compile-time
  // #define — a customer has no toolchain. wifiprov guarantees the board always ends up
  // reachable: stored creds -> STA, otherwise (or on failure) the SoftAP + captive portal.
  wifiprov::begin();
  s_mode = wifiprov::mode();
  s_ip   = wifiprov::ip();

  if (MDNS.begin(MDNS_HOST)) { MDNS.addService("http", "tcp", 80); Serial.printf("[net] http://%s.local/\n", MDNS_HOST); }

  diag::begin();
  // Full build id ("1.0.0+951b327"), not just the semver: the web UI's Info and
  // Système tabs show this verbatim, and it is what a support request must quote.
  diag::setInfo(FW_VERSION_FULL, s_idcode, s_mode.c_str(), s_ip.c_str());
  diag::attach(&ws);

  ws.onEvent(onWsEvent);
  server.addHandler(&ws);

  wifiprov::attachRoutes(server);   // /wifi* — must precede serveStatic("/")

  // =========================================================================
  // GROUP: PRODUCT — identity and everyday state. Safe, read-only.
  // =========================================================================

  // --- /sysinfo — WHO IS THIS BOARD? ---------------------------------------
  // The one endpoint to hit when a machine turns up in the field and nobody
  // remembers what is on it. Everything needed to reproduce or replace the
  // firmware: exact version + git commit + commit date, the running partition
  // and its MD5, the chip's factory MAC (the only unforgeable board id), the
  // FPGA it is talking to, and enough memory/filesystem state to tell a healthy
  // board from a sick one.
  //   curl -s http://gottfa.local/sysinfo | jq
  server.on("/sysinfo", HTTP_GET, [](AsyncWebServerRequest *r) {
    uint64_t mac = ESP.getEfuseMac();
    uint8_t  m[6];                                   // efuse MAC is little-endian in that u64
    for (int i = 0; i < 6; i++) m[i] = (uint8_t)(mac >> (8 * i));
    uint32_t total, age; uint8_t last;
    fpgalink::stats(total, last, age);
    AsyncResponseStream *res = r->beginResponseStream("application/json");
    res->printf("{\"name\":\"%s\",\"fw\":\"%s\",\"git\":\"%s\",\"built\":\"%s\","
                "\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"chip\":\"%s\",\"rev\":%u,"
                "\"cores\":%u,\"cpuMHz\":%u,",
                FW_NAME, FW_VERSION, FW_GIT, FW_BUILD,
                m[5], m[4], m[3], m[2], m[1], m[0],
                ESP.getChipModel(), (unsigned)ESP.getChipRevision(),
                (unsigned)ESP.getChipCores(), (unsigned)getCpuFrequencyMhz());
    res->printf("\"host\":\"%s.local\",\"mode\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,"
                "\"ssid\":\"%s\",\"apPassDefault\":%s,",
                MDNS_HOST, wifiprov::mode(), wifiprov::ip(), (int)WiFi.RSSI(),
                WiFi.SSID().c_str(), wifiprov::apPassIsDefault() ? "true" : "false");
    res->printf("\"idcode\":\"0x%08X\",\"fpga\":\"%s\",\"linkBytes\":%u,\"linkAgeMs\":%u,"
                "\"diag\":%s,\"xvc\":%s,",
                (unsigned)s_idcode, jtag::idcodeName(s_idcode),
                (unsigned)total, (unsigned)age,
                fpgalink::diagActive() ? "true" : "false",
                xvc::active() ? "true" : "false");
    res->printf("\"heap\":%u,\"heapMin\":%u,\"psram\":%u,\"flashMB\":%u,"
                "\"sketch\":%u,\"sketchFree\":%u,\"sketchMD5\":\"%s\",",
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
                (unsigned)ESP.getFreePsram(),
                (unsigned)(ESP.getFlashChipSize() / (1024 * 1024)),
                (unsigned)ESP.getSketchSize(), (unsigned)ESP.getFreeSketchSpace(),
                ESP.getSketchMD5().c_str());
    res->printf("\"fsUsed\":%u,\"fsTotal\":%u,",
                (unsigned)LittleFS.usedBytes(), (unsigned)LittleFS.totalBytes());
#ifndef BOARD_C3
    res->printf("\"target\":\"esp32s3\",\"sd\":%s,\"sets\":%u,\"theme\":\"%s\",",
                wavplayer::ready() ? "true" : "false",
                (unsigned)wavplayer::themeCount(), wavplayer::curTheme());
#else
    res->print("\"target\":\"esp32c3\",\"sd\":false,\"sets\":0,\"theme\":\"\",");
#endif
    res->printf("\"upS\":%u,\"routes\":\"/routes\"}", (unsigned)(millis() / 1000));
    r->send(res);
  });

  // --- /routes — the board documents itself (see the ROUTES table above) ----
  //   /routes            grouped plain text, readable straight in a browser
  //   /routes?fmt=json   the same list as JSON
  server.on("/routes", HTTP_GET, [](AsyncWebServerRequest *r) {
    bool json = r->hasParam("fmt") && r->getParam("fmt")->value() == "json";
    AsyncResponseStream *res = r->beginResponseStream(json ? "application/json" : "text/plain");
    if (json) {
      res->printf("{\"fw\":\"%s\",\"git\":\"%s\",\"routes\":[", FW_VERSION, FW_GIT);
      for (size_t i = 0; i < N_ROUTES; i++)
        res->printf("%s{\"path\":\"%s\",\"group\":\"%s\",\"desc\":\"%s\"}",
                    i ? "," : "", ROUTES[i].path, ROUTES[i].grp, ROUTES[i].desc);
      res->print("]}");
    } else {
      res->printf("GottFA80-PLuS ESP  v%s+%s  (%s)\n", FW_VERSION, FW_GIT, FW_BUILD);
      static const char *GRP[]  = { "product", "diagnostic", "service" };
      static const char *HEAD[] = {
        "PRODUCT — everyday use, driven by the web UI. Safe.",
        "DIAGNOSTIC — bench instruments. Read-only or momentary, but the ones marked\n"
        "             RESETS THE FPGA take the shared SPI bus, which reboots the game.",
        "SERVICE — changes persistent state (firmware, NOR, ROM store, web UI) or holds\n"
        "          hardware lines. Not for a customer. Unauthenticated: see DIAGNOSTICS.md."
      };
      for (int g = 0; g < 3; g++) {
        res->printf("\n== %s\n\n", HEAD[g]);
        for (size_t i = 0; i < N_ROUTES; i++)
          if (!strcmp(ROUTES[i].grp, GRP[g]))
            res->printf("  %-14s %s\n", ROUTES[i].path, ROUTES[i].desc);
      }
      res->print("\nAlso listening: TCP 2542 = XVC (JTAG over WiFi -> openFPGALoader).\n");
    }
    r->send(res);
  });

  // --- JTAG re-read (bring-up): re-scan the FPGA IDCODE on demand, no reboot needed ---
  //   /jtag  -> re-read TAP IDCODE, refresh s_idcode, return JSON {idcode, name, ok}
  server.on("/jtag", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (xvc::active()) {   // never bit-bang the TAP while openFPGALoader is mid-programming
      r->send(409, "application/json", "{\"ok\":false,\"err\":\"XVC session active\"}");
      return;
    }
    uint32_t id = jtag::readIdcode();
    s_idcode = id;
    bool ok = (id != 0x00000000 && id != 0xFFFFFFFF);   // 0/all-ones = no TAP response
    char j[96];
    snprintf(j, sizeof(j), "{\"idcode\":\"0x%08X\",\"name\":\"%s\",\"ok\":%s}",
             (unsigned)id, jtag::idcodeName(id), ok ? "true" : "false");
    r->send(200, "application/json", j);
  });

  // --- FPGA link telemetry (bring-up): is the GPIO8 UART receiving bytes? ---
  //   /link -> JSON {total, last (hex), ageMs, diag, running, game, ball, ballN, ballAge}
  //   ball* = telemetrie du token 0xA0|value ($0072); ballAge = -1 tant qu'aucun token recu
  //   game  = etat de partie REEL ($0072 debounce). "running" est le vieux token 0xF2/0xF3,
  //           qui est un latch "le 6502 a demarre" et ne retombe jamais — ne pas s'en servir.
  server.on("/link", HTTP_GET, [](AsyncWebServerRequest *r) {
    uint32_t total, age; uint8_t last;
    fpgalink::stats(total, last, age);
    uint32_t ballN, ballAge; uint8_t ball;
    fpgalink::ballStats(ball, ballN, ballAge);
    char j[352];   // sized for the longest form: every field present + fw/git
    snprintf(j, sizeof(j),
      "{\"fw\":\"" FW_VERSION "\",\"git\":\"" FW_GIT "\","      // so a script polling /link
      "\"total\":%u,\"last\":\"0x%02X\",\"ageMs\":%u,\"diag\":%s,\"running\":%s,\"game\":%s,"
      "\"ball\":%u,\"ballN\":%u,\"ballAge\":%ld}",              // knows which build answered
      (unsigned)total, last, (unsigned)age,
      fpgalink::diagActive() ? "true" : "false",
      fpgalink::gameRunning() ? "true" : "false",
      fpgalink::gameInProgress() ? "true" : "false",
      (unsigned)ball, (unsigned)ballN,
      (ballAge == 0xFFFFFFFF) ? -1L : (long)ballAge);
    r->send(200, "application/json", j);
  });

  // --- display-inject bench: drive the glass by hand -------------------------------
  //   GET /dispinj?ctrl=0x52&val=1234567
  // Latches an arbitrary CONTROL byte (repeated at 4 Hz by dispinject::tick(), so it
  // survives the FPGA's 2 s fail-safe) and pushes one DISPLAY frame. Used to map which
  // physical digits each overlay target reaches — the strobe 12..15 positions the stock
  // ROM never writes are the interesting ones. Time-attack must be DISARMED, else the
  // engine overwrites the control byte on its next tick.
  //   0x02 player 2 (default) | 0x12 player 1 | 0x22 player 4 | 0x32 player 3
  //   0x42 status/credits     | 0x52 group A strobes 12-15 | 0x62 group B strobes 12-15
  //   0x72 full glass (old behaviour)  — never send 0x7E/0x7F, the FPGA drops them
  server.on("/dispinj", HTTP_GET, [](AsyncWebServerRequest *r) {
#ifndef BOARD_C3
    uint8_t  c = 0x02;
    uint32_t v = 1234567;
    if (r->hasParam("ctrl")) c = (uint8_t)strtoul(r->getParam("ctrl")->value().c_str(), nullptr, 0);
    if (r->hasParam("val"))  v = (uint32_t)strtoul(r->getParam("val")->value().c_str(), nullptr, 0);
    dispinject::setCtrl(c);
    uint32_t ms = 60000;                       // repeat for a minute so there is time to look
    if (r->hasParam("ms")) ms = (uint32_t)strtoul(r->getParam("ms")->value().c_str(), nullptr, 0);
    dispinject::holdValue(v, ms);              // NOT send(): dvalid dies 1 s after the last frame
    char j[128];
    snprintf(j, sizeof(j), "{\"ctrl\":\"0x%02X\",\"latched\":\"0x%02X\",\"val\":%lu,\"sel\":%u}",
             c, dispinject::ctrl(), (unsigned long)v, (unsigned)((c >> 4) & 7));
    r->send(200, "application/json", j);
#else
    r->send(501, "text/plain", "no display tier on C3");
#endif
  });

  // --- game RAM snapshot: the whole 640-value RAM image, ~1x/s, on the same GPIO8 link
  //     (frame 0xBF + 640 x [0xC0|hi, 0xD0|lo], see fpgalink.h). This is the tool that found
  //     $0072 (game in progress) and $0109 (ball counter) by correlation on the real machine.
  //   /ramsnap -> {"ms":<millis of the frame>,"age":<ms since, -1 = jamais recu>,
  //                "frames":<complete>,"bad":<aborted>,"n":640,"hex":"<1280 hex chars>"}
  //   hex index   0..127 = CPU $0000..$007F (RIOT U4)   128..255 = $0080..$00FF (U5)
  //             256..383 = $0100..$017F (U6)            384..639 = $1800..$18FF (5101 CMOS,
  //   low nibble only). All-zero hex until the first frame lands.
  server.on("/ramsnap", HTTP_GET, [](AsyncWebServerRequest *r) {
    uint32_t frames, bad, ms, age;
    fpgalink::ramSnapStats(frames, bad, ms, age);
    static uint8_t ram[fpgalink::RAMSNAP_N];   // static: keep the frame off the async-task stack
    fpgalink::ramSnapCopy(ram);                // zero-filled while frames == 0
    char head[128];
    snprintf(head, sizeof(head),
      "{\"ms\":%u,\"age\":%ld,\"frames\":%u,\"bad\":%u,\"n\":%u,\"hex\":\"",
      (unsigned)ms, (age == 0xFFFFFFFF) ? -1L : (long)age,
      (unsigned)frames, (unsigned)bad, (unsigned)fpgalink::RAMSNAP_N);
    String j;
    j.reserve(sizeof(head) + 2 * fpgalink::RAMSNAP_N + 4);   // one alloc, no realloc storm
    j = head;
    static const char HEXD[] = "0123456789abcdef";
    for (uint16_t i = 0; i < fpgalink::RAMSNAP_N; i++) {
      j += HEXD[ram[i] >> 4]; j += HEXD[ram[i] & 0x0F];
    }
    j += "\"}";
    r->send(200, "application/json", j);
  });

  // --- SOUND TRACE: what did the bus ACTUALLY send? (see SOUND_WIRE.md) -------------
  // The instrument for reverse-engineering a title's sound commands: clear the ring, press ONE
  // playfield target, read the ring. Every link byte is timestamped as it arrives (fpgalink.h);
  // this route only formats what is already captured — no work on the hot path, no SD, no
  // blocking call, so it is safe in the AsyncWebServer task (same discipline as /ramsnap:
  // the frame lands in a STATIC buffer, never on the async task's stack).
  //   /sndtrace              CSV: idx,ms,dms,hex,class,value   (dms = ms since the previous row)
  //   /sndtrace?fmt=json     same data as JSON
  //   /sndtrace?n=64         only the newest 64 entries
  //   /sndtrace?clear=1      empty the ring (do this right before the target hit)
  //   /sndtrace?raw=1        capture EVERY byte incl. the 769 B/s RAM snapshot (~0.6 s of ring!)
  //   /sndtrace?raw=0        back to filtered (default): no snapshot payload, no level repeats
  server.on("/sndtrace", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (r->hasParam("raw"))   fpgalink::traceMode(r->getParam("raw")->value().toInt() != 0);
    if (r->hasParam("clear")) fpgalink::traceClear();
    uint16_t want = fpgalink::TRACE_N;
    if (r->hasParam("n")) { long v = r->getParam("n")->value().toInt();
                            if (v > 0 && v < (long)fpgalink::TRACE_N) want = (uint16_t)v; }
    static fpgalink::TraceEv ev[fpgalink::TRACE_N];      // static: 4 KB off the async stack
    uint16_t n = fpgalink::traceCopy(ev, want);
    uint32_t kept, elided, payload, rel, lost;
    fpgalink::traceStats(kept, elided, payload);
    fpgalink::soundMetaStats(rel, lost);
    bool json = r->hasParam("fmt") && r->getParam("fmt")->value() == "json";
    // Chunk-free but incremental: the stream grows as we print, so no single 15 KB alloc.
    AsyncResponseStream *res = r->beginResponseStream(json ? "application/json" : "text/csv");
    if (json) {
      res->printf("{\"n\":%u,\"raw\":%s,\"kept\":%u,\"elided\":%u,\"payload\":%u,"
                  "\"rel\":%u,\"lost\":%u,\"now\":%u,\"ev\":[",
                  (unsigned)n, fpgalink::traceRaw() ? "true" : "false",
                  (unsigned)kept, (unsigned)elided, (unsigned)payload,
                  (unsigned)rel, (unsigned)lost, (unsigned)millis());
    } else {
      res->printf("# sndtrace n=%u raw=%d kept=%u elided=%u payload=%u rel=%u lost=%u now=%u\n"
                  "idx,ms,dms,hex,class,value\n",
                  (unsigned)n, fpgalink::traceRaw() ? 1 : 0, (unsigned)kept, (unsigned)elided,
                  (unsigned)payload, (unsigned)rel, (unsigned)lost, (unsigned)millis());
    }
    uint32_t prev = n ? ev[0].ms : 0;
    for (uint16_t i = 0; i < n; i++) {
      int v; const char* cls = fpgalink::traceDecode(ev[i].b, v);
      uint32_t d = ev[i].ms - prev; prev = ev[i].ms;
      if (json) res->printf("%s[%u,%u,%u,\"%s\",%d]", i ? "," : "",
                            (unsigned)i, (unsigned)ev[i].ms, (unsigned)d, cls, v);
      else      res->printf("%u,%u,%u,0x%02X,%s,%d\n",
                            (unsigned)i, (unsigned)ev[i].ms, (unsigned)d, ev[i].b, cls, v);
    }
    if (json) res->print("]}");
    r->send(res);
  });

  // --- [DIAG] /led — "which board am I?": force the WS2812 to a colour ---
  //   /led?r=255&g=0&b=0    /led  (no args) = off, back to the beacon
  //
  // This route used to advertise a PIN HUNT (`/led?pin=38|47|48`) so the on-board
  // LED's real GPIO could be found without a reflash. That never worked and could
  // not: the Arduino core's neopixelWrite() binds its RMT channel to the FIRST pin
  // it is ever called with and silently ignores the pin argument on every later
  // call -- and statusled::begin() has already called it at boot. The ?pin=
  // parameter was writing to whatever pin was bound and reporting the one you
  // asked for, i.e. it lied. Dropped rather than left as a trap; the pin is a
  // build-time constant (PIN_RGB_LED) and belongs in board_config.h.
  //
  // What survives is genuinely useful in the field: light up ONE board in a rack
  // to confirm which IP is which machine. statusled::tick() only rewrites the LED
  // when its own computed colour CHANGES, so the colour set here sticks until the
  // board's state moves (link lost, diag entered, OTA...).
  server.on("/led", HTTP_GET, [](AsyncWebServerRequest *r) {
#ifndef BOARD_C3
    uint8_t rr = r->hasParam("r") ? r->getParam("r")->value().toInt() : 0;
    uint8_t gg = r->hasParam("g") ? r->getParam("g")->value().toInt() : 0;
    uint8_t bb = r->hasParam("b") ? r->getParam("b")->value().toInt() : 0;
    neopixelWrite(PIN_RGB_LED, rr, gg, bb);
    char j[128];
    snprintf(j, sizeof(j), "{\"pin\":%d,\"r\":%u,\"g\":%u,\"b\":%u,"
             "\"note\":\"the status beacon takes the LED back on its next state change\"}",
             PIN_RGB_LED, rr, gg, bb);
    r->send(200, "application/json", j);
#else
    r->send(501, "text/plain", "no WS2812 on the C3 target");   // PIN_RGB_LED is S3-only
#endif
  });

  // --- PSOWAV sound test (bring-up): play any sound / set theme from a browser, no FPGA ---
  //   /snd?id=N      play PSOWAV sound N (0..31)      /snd?theme=arena   load a game set
  //   /snd?stop=1    stop all voices                  /snd               usage + status
  server.on("/snd", HTTP_GET, [](AsyncWebServerRequest *r) {
#ifndef BOARD_C3
    if (r->hasParam("id")) {
      int id = r->getParam("id")->value().toInt();
      bool ok = (id >= 0 && id <= 95) && wavplayer::play(id);
      r->send(ok ? 200 : 400, "text/plain", ok ? ("play " + String(id)) : "bad id (0..95) or not ready");
      return;
    }
    if (r->hasParam("theme")) {
      wavplayer::setTheme(r->getParam("theme")->value().c_str());
      r->send(200, "text/plain", "theme -> " + r->getParam("theme")->value());
      return;
    }
    if (r->hasParam("stop")) { wavplayer::stopAll(); r->send(200, "text/plain", "stopped"); return; }
    if (r->hasParam("mixreset")) { wavplayer::mixStatsReset(); r->send(200, "text/plain", "mix stats reset"); return; }
    uint32_t bmax, blast, late, pass, period, bufms;
    wavplayer::mixStats(bmax, blast, late, pass, period, bufms);
    r->send(200, "text/plain", String("PSOWAV ") + (wavplayer::ready() ? "ready" : "NOT ready") +
            " theme='" + wavplayer::curTheme() + "' status=" + wavplayer::setStatus() +
            (wavplayer::silent() ? " SILENT" : "") +
            " sounds=" + String(wavplayer::soundCount()) +
            "\nmix: buf=" + String(bufms) + "ms period=" + String(period) + "us busyMax=" +
            String(bmax) + "us busyLast=" + String(blast) + "us late=" + String(late) +
            "/" + String(pass) + " passes  (late>0 => the DMA queue is draining: raise i2sn/i2slen"
            " in /config.txt, or the SD clock)\n"
            "usage: /snd?id=N (0..95) | /snd?theme=NAME | /snd?stop=1 | /snd?mixreset=1 | /beep"
            " | /sndtrace");
#else
    r->send(501, "text/plain", "no sound tier on C3");
#endif
  });

  // --- HARDWARE TEST: /beep -> 440 Hz sine straight to the PCM5102A, no SD/WAV ---
  //   Isolates the DAC. If you hear this, I2S+DAC+wiring are good; any WAV silence is the SD.
  //   FIXED 2026-07-27: this used to call wavplayer::testTone(ms) inline, which
  //   blocks in i2s_write(..., portMAX_DELAY) for the whole tone -- up to 3 s on
  //   the AsyncTCP task -- AND wrote I2S_NUM_0 concurrently with mixTask, which
  //   is the only other writer.  It now goes through the same FreeRTOS request
  //   queue as /snd, so the tone is generated by mixTask: no blocking, one writer.
  server.on("/beep", HTTP_GET, [](AsyncWebServerRequest *r) {
#ifndef BOARD_C3
    int ms = r->hasParam("ms") ? r->getParam("ms")->value().toInt() : 800;
    if (ms < 50) ms = 50; if (ms > 3000) ms = 3000;
    bool ok = wavplayer::requestTestTone(ms);
    r->send(ok ? 200 : 503, "text/plain",
            ok ? ("beep " + String(ms) + "ms queued (I2S sine on the mix task, no SD)")
               : "sound not ready");
#else
    r->send(501, "text/plain", "no sound tier on C3");
#endif
  });

  // --- bench game-select: load a game's PSOWAV sound set without an FPGA token ---
  //   GET /game?id=N   (N = FPGA game No 0..62 = DIP S1 = games.txt index)
  server.on("/game", HTTP_GET, [](AsyncWebServerRequest *r) {
#ifndef BOARD_C3
    if (r->hasParam("id")) {
      int id = r->getParam("id")->value().toInt();
      wavplayer::selectGame(id);
      r->send(200, "text/plain", "game -> " + String(id));
    } else r->send(400, "text/plain", "usage: /game?id=N (0..62)");
#else
    r->send(501, "text/plain", "no sound tier on C3");
#endif
  });

  // --- EPROM reader (optional daughterboard): dump the user's own chip to /dumps/<name>.bin ---
  //   GET /dump?type=2716|2732|2764[&name=foo]   (see EPROM_READER.md)
  server.on("/dump", HTTP_GET, [](AsyncWebServerRequest *r) {
#ifndef BOARD_C3
    if (!epromdump::available()) { r->send(503, "text/plain", "EPROM reader disabled (set EPROM_READER_ENABLE=1 + fit the board)"); return; }
    epromdump::Type t = epromdump::T2764;
    if (r->hasParam("type")) { String s = r->getParam("type")->value();
      t = (s == "2716") ? epromdump::T2716 : (s == "2732") ? epromdump::T2732 :
          (s == "u2")   ? epromdump::T2332_U2 : (s == "u3") ? epromdump::T2332_U3 : epromdump::T2764; }
    String name = r->hasParam("name") ? r->getParam("name")->value() : String("dump");
    String path = "/dumps/" + name + ".bin";
    bool ok = epromdump::dumpToSD(t, path.c_str());
    if (!ok) { r->send(500, "text/plain", "dump failed"); return; }
    romdb::Match m; romdb::identifyFile(path.c_str(), &m);    // verify against known-good (PinMAME)
    char buf[200];
    if (m.found) {
      bool added = ownership::own(m.game);                    // verified dump = proof of ownership
      snprintf(buf, sizeof(buf), "dumped -> %s\nOK %08x = %s (%s) [known-good]%s",
               path.c_str(), (unsigned)m.crc, m.title, m.game, added ? " — sound unlocked" : "");
    } else snprintf(buf, sizeof(buf),
        "dumped -> %s\ncrc %08x NOT in DB -> corrupted chip or unlisted revision (keep as custom, or use the verified backup)",
        path.c_str(), (unsigned)m.crc);
    r->send(200, "text/plain", buf);
#else
    r->send(501, "text/plain", "no SD on C3");
#endif
  });

  // --- ownership gate: list owned games / toggle the gate / add manually ---
  //   GET /owned [?gate=0|1] [?add=romid]  -> {"gate":0|1,"n":N,"games":"a,b,c"}
  server.on("/owned", HTTP_GET, [](AsyncWebServerRequest *r) {
#ifndef BOARD_C3
    if (r->hasParam("gate")) ownership::setGate(r->getParam("gate")->value().toInt() != 0);
    if (r->hasParam("add"))  ownership::own(r->getParam("add")->value().c_str());
    char buf[640]; int n = ownership::list(buf, sizeof(buf));
    String j = "{\"gate\":" + String(ownership::gateEnabled() ? 1 : 0) + ",\"n\":" + String(n) +
               ",\"games\":\"" + String(buf) + "\"}";
    r->send(200, "application/json", j);
#else
    r->send(501, "text/plain", "no SD on C3");
#endif
  });

  // --- verify a stored file against the known-good ROM DB (PinMAME CRCs) ---
  //   GET /verify?path=/dumps/foo.bin   -> {"crc":"...","known":0|1,"game":"...","title":"..."}
  //   DEFERRED (2026-07-27): a bitwise CRC32 over the whole file in 512-byte reads
  //   from a 1 MHz SPI card, then a linear scan of /db/roms.csv building a String
  //   per line -- hundreds of ms to seconds. -> job queue.
  server.on("/verify", HTTP_GET, [](AsyncWebServerRequest *r) {
#ifndef BOARD_C3
    if (!r->hasParam("path")) { r->send(400, "text/plain", "usage: /verify?path=/dumps/foo.bin"); return; }
    String path = r->getParam("path")->value();
    if (path.length() >= (int)sizeof(s_job.path)) { r->send(400, "text/plain", "path too long"); return; }
    uint32_t id = jobClaim(JOB_VERIFY);
    if (!id) { jobBusy(r); return; }
    strncpy(s_job.path, path.c_str(), sizeof(s_job.path) - 1);
    s_job.path[sizeof(s_job.path) - 1] = 0;
    jobAccepted(r, id, "verify");
#else
    r->send(501, "text/plain", "no SD on C3");
#endif
  });

  // --- ROM store: list per-game variants + device key fingerprint + global Free-Play setting ---
  //   GET /roms -> {"key":"<hex>","fp":0|1,"games":[{"n":N,"s":stock,"se":stockEnc,"f":fp,"fe":fpEnc}]}
  server.on("/roms", HTTP_GET, [](AsyncWebServerRequest *r) {
#ifndef BOARD_C3
    // One directory walk (romstore::scan) instead of has()+encrypted() per slot:
    // the old loop did up to 6 SD.open() per game x 63 games = ~380 opens on a
    // 1 MHz SPI card, i.e. 0.5-4 s of blocking inside the AsyncTCP handler. The
    // web UI fetches this route, so it stays SYNCHRONOUS -- it is just fast now.
    static long st[romstore::MAX_GAME], fpv[romstore::MAX_GAME];
    romstore::scan(st, fpv);
    auto present = [](long sz) { return sz == romstore::IMG_SIZE || sz == romcrypt::CONT_SIZE; };
    auto enc     = [](long sz) { return sz == romcrypt::CONT_SIZE; };
    String j = "{\"key\":\"" + String(romcrypt::keyId(), HEX) + "\",\"fp\":" +
               (romstore::freePlay() ? "1" : "0") + ",\"games\":[";
    bool first = true;
    for (int i = 0; i < romstore::MAX_GAME; i++) {
      bool sv = present(st[i]), f = present(fpv[i]);
      if (!sv && !f) continue;
      if (!first) j += ',';
      first = false;
      j += "{\"n\":" + String(i) +
           ",\"s\":"  + (sv ? "1" : "0") + ",\"se\":" + (enc(st[i])  ? "1" : "0") +
           ",\"f\":"  + (f  ? "1" : "0") + ",\"fe\":" + (enc(fpv[i]) ? "1" : "0") + "}";
    }
    j += "]}";
    r->send(200, "application/json", j);
#else
    r->send(501, "text/plain", "no store on C3");
#endif
  });

  // --- ROM delete: remove one game slot (stock+FP, or one variant with ?fp=1) ---
  //   GET /romdel?id=N[&fp=1]   -> delete; fp omitted = delete BOTH variants of that slot
  server.on("/romdel", HTTP_GET, [](AsyncWebServerRequest *r) {
#ifndef BOARD_C3
    if (!r->hasParam("id")) { r->send(400, "text/plain", "usage: /romdel?id=N[&fp=1]"); return; }
    int id = r->getParam("id")->value().toInt();
    bool ok;
    if (r->hasParam("fp")) ok = romstore::remove(id, r->getParam("fp")->value().toInt() != 0);
    else { bool a = romstore::remove(id, false), b = romstore::remove(id, true); ok = a && b; }
    r->send(ok ? 200 : 500, "text/plain",
            ok ? ("deleted game " + String(id)) : "delete failed");
#else
    r->send(501, "text/plain", "no store on C3");
#endif
  });

  // --- Free-Play device setting: which ROM variant is served to the FPGA ---
  //   GET /fp -> {"fp":0|1}   ;   GET /fp?set=0|1 -> set then return the new state
  server.on("/fp", HTTP_GET, [](AsyncWebServerRequest *r) {
#ifndef BOARD_C3
    if (r->hasParam("set")) romstore::setFreePlay(r->getParam("set")->value().toInt() != 0);
    r->send(200, "application/json", String("{\"fp\":") + (romstore::freePlay() ? "1" : "0") + "}");
#else
    r->send(501, "text/plain", "no store on C3");
#endif
  });

  // --- WAV upload: stream a sound file straight to the SD (no RAM buffer) ---
  //   POST /wavup?dir=DIR&name=FILE   body = raw WAV bytes -> /DIR/FILE on the SD.
  //   Creates /DIR if needed. Used to load a game's PSOWAV sound set from a browser.
  server.on("/wavup", HTTP_POST,
    [](AsyncWebServerRequest *r) {
#ifndef BOARD_C3
      bool ok = false;
      if (s_wav_busy) {
        // Success only if the file opened, every byte was written, and the final
        // on-card size matches what was received (catches a mid-write SD dropout).
        if (s_wav.f) { s_wav.f.flush(); s_wav.f.close(); }
        if (s_wav.opened && !s_wav.failed && s_wav.written == s_wav.total) {
          File chk = SD.open(s_wav.path, FILE_READ);
          if (chk) { ok = (chk.size() == s_wav.total); chk.close(); }
        }
        wavRelease(ok);                                  // keeps the file only if ok
      }
      r->send(ok ? 200 : 500, "text/plain", ok ? "ok" : "write failed (SD?)");
#else
      r->send(501, "text/plain", "no SD on C3");
#endif
    },
    NULL,
    [](AsyncWebServerRequest *r, uint8_t *data, size_t len, size_t index, size_t total) {
#ifndef BOARD_C3
      if (index == 0) {
        if (!r->hasParam("dir") || !r->hasParam("name")) return;
        if (s_wav_busy) return;                          // another upload owns the slot
        String dir = "/" + r->getParam("dir")->value();
        if (!SD.exists(dir)) SD.mkdir(dir);
        s_wav_busy   = true;
        s_wav.path   = dir + "/" + r->getParam("name")->value();
        s_wav.total  = total; s_wav.written = 0;
        s_wav.opened = false; s_wav.failed  = false;
        SD.remove(s_wav.path);
        s_wav.f      = SD.open(s_wav.path, FILE_WRITE);
        s_wav.opened = (bool)s_wav.f;
        if (!s_wav.opened) s_wav.failed = true;
      }
      if (!s_wav_busy) return;
      s_wav_touch = millis();
      if (s_wav.f && !s_wav.failed) {
        size_t w = s_wav.f.write(data, len);             // may short-write on a flaky SD
        s_wav.written += w;
        if (w != len) s_wav.failed = true;               // detected a partial write
      }
#endif
    });

  // --- ROM upload: POST a raw 16384-byte GottFA game image -> ENCRYPTED into /roms/<NN>.img ---
  //   POST /romup?id=N[&fp=1]   body = exactly 16384 bytes (the user supplies their own ROM).
  //   Encryption is device-bound (romcrypt) anti-extraction; it does NOT change legality.
  server.on("/romup", HTTP_POST,
    [](AsyncWebServerRequest *r) {
#ifndef BOARD_C3
      RomUp *u = (RomUp *)r->_tempObject;
      if (!u || u->got != u->cap) { r->send(400, "text/plain", "need exactly 16384 bytes"); return; }
      if (!r->hasParam("id")) { r->send(400, "text/plain", "missing ?id=N"); return; }
      int id = r->getParam("id")->value().toInt();
      bool fp = (r->hasParam("fp") && r->getParam("fp")->value().toInt() != 0);
      bool ok = romstore::store(id, fp, u->data);
      r->send(ok ? 200 : 500, "text/plain",
              ok ? ("stored game " + String(id) + (fp ? " (Free Play)" : " (stock)")) : "store failed (key? SD?)");
#else
      r->send(501, "text/plain", "no store on C3");
#endif
    },
    NULL,
    [](AsyncWebServerRequest *r, uint8_t *data, size_t len, size_t index, size_t total) {
#ifndef BOARD_C3
      if (index == 0) {
        if (total != (size_t)romstore::IMG_SIZE) return;     // wrong size -> reject in completion
        RomUp *u = (RomUp *)malloc(sizeof(RomUp) - 1 + total);
        if (!u) return;
        u->cap = (uint32_t)total; u->got = 0;
        r->_tempObject = u;
      }
      RomUp *u = (RomUp *)r->_tempObject;
      if (u && index + len <= u->cap) { memcpy(u->data + index, data, len); u->got += len; }
#endif
    });

  // --- Déploiement: OTA firmware update (POST a firmware .bin). Fails gracefully if the
  //     partition scheme has no OTA slot (Update.begin returns false) -> never bricks. To
  //     enable real OTA: set board_build.partitions to an OTA scheme + one USB flash first.
  server.on("/ota", HTTP_POST,
    [](AsyncWebServerRequest *r){
      bool ok = !Update.hasError();
      AsyncWebServerResponse *res = r->beginResponse(200, "text/plain", ok ? "OK — redémarrage…" : "ÉCHEC OTA (partition ?)");
      res->addHeader("Connection","close"); r->send(res);
      if (ok) { delay(150); ESP.restart(); }
    },
    [](AsyncWebServerRequest *r, String fn, size_t idx, uint8_t *data, size_t len, bool done){
      if (!idx) { Serial.printf("[ota] begin %s\n", fn.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial); }
      if (Update.write(data, len) != len) Update.printError(Serial);
      if (done) { if (Update.end(true)) Serial.printf("[ota] ok %u bytes\n", (unsigned)(idx+len));
                  else Update.printError(Serial); }
    });

  // --- Coil test (see coiltest.h). The web UI drives this over the WebSocket; this
  //     route is the curl/script face of the same state machine.
  //       GET /coiltest                -> live status + last results (JSON)
  //       GET /coiltest?do=learn[&ms=] -> build the per-coil switch signature, save it
  //       GET /coiltest?do=test[&ms=]  -> replay it and report per coil
  //       GET /coiltest?do=abort       -> stop the run in progress
  //       GET /coiltest?key=NN         -> load another game's saved signature
  //     Like the deferred-job routes, the handler only validates and arms: it answers
  //     202 immediately and the pulsing happens on loopTask. Unlike them it is NOT a
  //     jobRun() body -- a job body runs to completion inside netLoop(), and a ~25 s
  //     LEARN there would stall diag::tick(), wifiprov::tick() and the WS pings just as
  //     badly as blocking the async task. Poll this same URL instead of /jobstatus.
  server.on("/coiltest", HTTP_GET, [](AsyncWebServerRequest *r) {
    String act = r->hasParam("do") ? r->getParam("do")->value() : "";
    if (act == "abort") { coiltest::abort(); r->send(200, "application/json", "{\"abort\":true}"); return; }
    if (act == "learn" || act == "test") {
      int key = r->hasParam("key") ? atoi(r->getParam("key")->value().c_str()) : coiltest::keyFor();
      uint8_t ms = r->hasParam("ms") ? (uint8_t)atoi(r->getParam("ms")->value().c_str()) : 0;
      const char *err = coiltest::start(act == "learn", key, ms);
      char j[320];
      if (err) {
        snprintf(j, sizeof(j), "{\"accepted\":false,\"err\":\"%s\"}", err);
        r->send(409, "application/json", j);
      } else {
        snprintf(j, sizeof(j),
                 "{\"accepted\":true,\"async\":true,\"mode\":\"%s\",\"key\":%d,"
                 "\"poll\":\"/coiltest\","
                 "\"note\":\"runs from the main loop; poll until run==0, then read c[]\"}",
                 act.c_str(), key);
        r->send(202, "application/json", j);
      }
      return;
    }
    if (r->hasParam("key") && !coiltest::busy())
      coiltest::load(atoi(r->getParam("key")->value().c_str()));
    r->send(200, "application/json", coiltest::statusJson());
  });

  // --- Scope helper: /norloop?on=1 repeats the JEDEC read (400 kHz, one burst
  //     every ~100 ms, from netLoop, not the handler) so an oscilloscope can
  //     trigger on CS and inspect SLK/DI/DO at the module pins. /norloop?on=0 stops.
  server.on("/norloop", HTTP_GET, [](AsyncWebServerRequest *r) {
    extern volatile bool g_norloop;
    g_norloop = r->hasParam("on") && r->getParam("on")->value() == "1";
    if (g_norloop) { pinMode(PIN_FPGA_RESET, OUTPUT); digitalWrite(PIN_FPGA_RESET, LOW);
                     s_holdSince = millis() ? millis() : 1; }
    else           { pinMode(PIN_FPGA_RESET, INPUT); s_holdSince = 0; }
    r->send(200, "application/json", g_norloop ?
      "{\"loop\":true,\"scope\":\"trigger on CS falling; burst every ~100ms at 400kHz\","
      "\"autoStopS\":300}" :
      "{\"loop\":false}");
  });

  // --- Line-level test: drive CS/CLK/MOSI LOW (through the 600R) and hold, so a
  //     multimeter can read the LOW level actually reached at the NOR's legs. If
  //     board pull-ups are strong, the divider keeps the "low" above VIL(0.8V) and
  //     the chip never hears anything — the breadboard-vs-machine difference.
  //     /norhold drives low and returns; /norrelease frees the pins.
  //     SAFETY (v1): both /norhold and /norloop leave the machine DEAD while they
  //     are engaged — they pull PIN_FPGA_RESET low, so the FPGA is held in reset
  //     and the game cannot run. Nothing used to undo that: one GET (a bookmark, a
  //     browser prefetch, a forgotten tab) parked the pinball until someone
  //     remembered /norrelease or power-cycled it. netLoop() now releases both
  //     after HOLD_MAX_MS. The instrument is unchanged for its real use — five
  //     minutes is far longer than anyone needs to put a probe on three legs.
  server.on("/norhold", HTTP_GET, [](AsyncWebServerRequest *r) {
    pinMode(PIN_FPGA_RESET, OUTPUT); digitalWrite(PIN_FPGA_RESET, LOW);   // grant (si bitstream bg2)
    pinMode(PIN_SPI_CS_SD, OUTPUT); digitalWrite(PIN_SPI_CS_SD, LOW);
    pinMode(PIN_SPI_SCLK,  OUTPUT); digitalWrite(PIN_SPI_SCLK,  LOW);
    pinMode(PIN_SPI_MOSI,  OUTPUT); digitalWrite(PIN_SPI_MOSI,  LOW);
    s_holdSince = millis() ? millis() : 1;                                // 0 is the "idle" value
    r->send(200, "application/json",
      "{\"holding\":\"CS+CLK+MOSI LOW\",\"measure\":\"NOR legs: p1 CS, p6 CLK, p5 DI - expect <0.4V; >=0.8V = divider problem\",\"then\":\"GET /norrelease\",\"warn\":\"the FPGA is held in RESET while this is engaged - the game is dead\",\"autoReleaseS\":300}");
  });
  server.on("/norrelease", HTTP_GET, [](AsyncWebServerRequest *r) {
    extern volatile bool g_norloop;
    g_norloop = false;
    pinMode(PIN_SPI_CS_SD, INPUT); pinMode(PIN_SPI_SCLK, INPUT);
    pinMode(PIN_SPI_MOSI, INPUT);  pinMode(PIN_FPGA_RESET, INPUT);
    s_holdSince = 0;
    r->send(200, "application/json", "{\"released\":true}");
  });

  // --- Bus-grant verifier: counts CLK/MOSI edges seen by the ESP (as inputs)
  //     with the grant RELEASED vs ASSERTED. On the bg2+ bitstream the FPGA's
  //     SD/EEPROM traffic must vanish when GPIO14 pulls S8.2 low. Proves the
  //     esp_bus patch end-to-end without needing the NOR at all.
  //     DEFERRED (2026-07-27): 410 ms wall, 320 ms of it a hard no-yield
  //     digitalRead busy-wait -- the single worst idle-starvation offender in the
  //     firmware.  Now runs from the main loop.
  server.on("/grant", HTTP_GET, [](AsyncWebServerRequest *r) {
    uint32_t id = jobClaim(JOB_GRANT);
    if (!id) { jobBusy(r); return; }
    jobAccepted(r, id, "grant");
  });

  // --- Full per-game write test: programs a real 16 KB region (one game slot:
  //     4x 4KB sector erase + 64 page programs + address-derived verify). Proves
  //     norprog::program on production volume. Pattern is (addr^rot^0x5A) so a
  //     misplaced page fails verify. Default slot 0x3F0000 (high, clobbers nothing).
  //     DEFERRED (2026-07-27): 0.5-2 s of erase/program/verify -> job queue.
  //     The 16 KB pattern buffer is malloc'd by the runner instead of living in
  //     BSS for ever (it was 16 KB of permanently resident RAM for a test route).
  server.on("/norbig", HTTP_GET, [](AsyncWebServerRequest *r) {
    uint32_t id = jobClaim(JOB_NORBIG);
    if (!id) { jobBusy(r); return; }
    jobAccepted(r, id, "norbig");
  });

  // --- W25Q NOR write test: exercises the real norprog path end-to-end on the
  //     LAST 4 KB sector (0x3FF000 — far from any game image at N*0x4000). The
  //     pattern is address-derived, so a page landing at the wrong address fails
  //     the verify (a constant pattern would not catch that).
  //     DEFERRED (2026-07-27): 100-500 ms of erase/program/verify -> job queue.
  //     The comment used to promise "an independent 16-byte readback"; the code
  //     for it was a `bool rb_ok = true;` and an empty block declaring a function
  //     that was never called, and rb_ok was never read.  Removed rather than
  //     left as a claim the code does not honour -- norprog::program(verify=true)
  //     already reads every byte back and compares it.
  server.on("/norwrite", HTTP_GET, [](AsyncWebServerRequest *r) {
    uint32_t id = jobClaim(JOB_NORWRITE);
    if (!id) { jobBusy(r); return; }
    jobAccepted(r, id, "norwrite");
  });

  // --- NOR image upload: POST /norflash?addr=0xNNNNNN with a raw <=16 KB image
  //     body -> programs it into the W25Q at addr (erase+program+verify) via the
  //     esp_bus grant. Used to load a game ROM the FPGA's nor_flash.vhd then reads.
  //     DEFERRED (2026-07-27): the body is still buffered here (a memcpy per TCP
  //     chunk, microseconds), but the 0.5-2 s erase+program+verify now runs from
  //     the main loop.  Same URL, 202 + job id, poll /jobstatus.
  //     Also guarded for BOARD_C3: RomUp only exists in the S3 build, so the C3
  //     build did not compile this file at all.
  server.on("/norflash", HTTP_POST,
    [](AsyncWebServerRequest *r){
#ifndef BOARD_C3
      RomUp *u = (RomUp*)r->_tempObject;
      uint32_t addr = 0;
      if (r->hasParam("addr")) addr = strtoul(r->getParam("addr")->value().c_str(), nullptr, 0);
      if (!u || u->got == 0) {
        r->send(400, "application/json",
                "{\"ok\":false,\"err\":\"empty body, or more than 16384 bytes\"}");
        return;
      }
      uint32_t id = jobClaim(JOB_NORFLASH);
      if (!id) { jobBusy(r); return; }   // slot busy: the library frees _tempObject as usual
      // OWNERSHIP TRANSFER.  The job now owns the malloc'd RomUp and frees it when
      // it finishes.  Clearing _tempObject is what stops ~AsyncWebServerRequest()
      // free()ing the buffer out from under the runner when this response completes.
      s_job.arg      = addr;
      s_job.payload  = u;
      s_job.len      = u->got;
      r->_tempObject = nullptr;
      jobAccepted(r, id, "norflash");
#else
      r->send(501, "text/plain", "no NOR image upload on C3");
#endif
    },
    nullptr,   // no multipart
    [](AsyncWebServerRequest *r, uint8_t *data, size_t len, size_t idx, size_t total){
#ifndef BOARD_C3
      if (idx == 0) {
        if (total == 0 || total > 0x4000) { return; }
        // sizeof(RomUp)-1+total, matching /romup: RomUp ends in data[1], so the
        // -1 is the byte already counted in the struct.  (The two sites used to
        // disagree by one byte for no reason.)
        RomUp *u = (RomUp*)malloc(sizeof(RomUp) - 1 + total);
        if (!u) return;
        u->cap = total; u->got = 0;
        r->_tempObject = u;
      }
      RomUp *u = (RomUp*)r->_tempObject;
      if (u && u->got + len <= u->cap) { memcpy(u->data + u->got, data, len); u->got += len; }
#endif
    });

  // --- W25Q NOR probe  // --- W25Q NOR probe, BREADBOARD mode: ESP + module only, direct wires, no series
  //     resistors, FPGA absent. Everything on the machine build measured good yet the
  //     chip never drove MISO, so this isolates the module itself. Tries both D0/D1
  //     assignments (the DO/DI vs D0/D1 silkscreen ambiguity) at several clock rates,
  //     and reports the MISO level while the chip is selected but unclocked — a live
  //     part holds its DO there, a dead/absent one leaves the line floating.
  //     MACHINE mode: assert the ESP bus grant (GPIO14 -> S8.2 low). With the
  //     SYS80_bg2+ bitstream the FPGA then tri-states MOSI/CLK, releases the NOR's
  //     CS net and keeps the M95256 deselected. On older bitstreams the FPGA keeps
  //     driving the bus and this probe reads garbage — load bg2 first.
  //     DEFERRED (2026-07-27): ~100 ms of SPI probing + delays -> job queue.
  server.on("/nor", HTTP_GET, [](AsyncWebServerRequest *r) {
    uint32_t id = jobClaim(JOB_NOR);
    if (!id) { jobBusy(r); return; }
    jobAccepted(r, id, "nor");
  });

  // --- FPGA-SD direct probe (bring-up): hold the FPGA in reset, master the
  //     shared J3a bus, and run a full SD init on the FPGA's game-ROM card.
  //     Answers "is the card/socket alive" independently of any bitstream.
  //     DEFERRED (2026-07-27) and FIXED: see runSdProbe() -- it used to report the
  //     SOUND card by mistake and then unmount it, killing sound until reboot.
  server.on("/sdprobe", HTTP_GET, [](AsyncWebServerRequest *r) {
#ifndef BOARD_C3
    uint32_t id = jobClaim(JOB_SDPROBE);
    if (!id) { jobBusy(r); return; }
    jobAccepted(r, id, "sdprobe");
#else
    r->send(501, "text/plain", "no SD on C3");
#endif
  });

  // --- raw pin peek (bring-up): read a wired-to-machine line's level ---
  //   /pin?n=14 -> FPGA_RESET / S8.2 (reset_l): 0 = machine holds the FPGA in reset
  server.on("/pin", HTTP_GET, [](AsyncWebServerRequest *r) {
    int n = r->hasParam("n") ? r->getParam("n")->value().toInt() : PIN_FPGA_RESET;
    if (n != PIN_FPGA_RESET && n != PIN_FPGA_LINK) { r->send(400, "text/plain", "pin: 14|8"); return; }
    pinMode(n, INPUT);                       // observe only — never drive machine lines here
    int v = digitalRead(n);
    char j[48]; snprintf(j, sizeof(j), "{\"pin\":%d,\"level\":%d}", n, v);
    r->send(200, "application/json", j);
  });

  // --- LittleFS file upload over WiFi (web UI updates without USB) ---
  //   curl -F "file=@data/index.html" http://<esp>/fsup      (writes /index.html)
  //   optional ?path=/foo.txt to choose the target; always rooted at /.
  server.on("/fsup", HTTP_POST,
    [](AsyncWebServerRequest *r){
      r->send(200, "text/plain", "OK — fichier écrit");
    },
    [](AsyncWebServerRequest *r, String fn, size_t idx, uint8_t *data, size_t len, bool done){
      // `f` is FUNCTION-static, i.e. ONE slot shared by every request -- two
      // overlapping uploads used to interleave into a single file handle and
      // corrupt each other silently.  s_busy makes the second one a no-op (the
      // completion handler reports it); it is also released on `done`, and by the
      // netLoop janitor if the client vanishes mid-upload.
      static File f;
      if (!idx) {
        if (s_fsup_busy) { Serial.println("[fsup] refused: another upload is in progress"); return; }
        String path = r->hasParam("path") ? r->getParam("path")->value() : ("/" + fn);
        if (!path.startsWith("/")) path = "/" + path;
        f = LittleFS.open(path, "w");
        s_fsup_busy = (bool)f;
        s_fsup_file = &f;
        Serial.printf("[fsup] begin %s\n", path.c_str());
      }
      if (!s_fsup_busy) return;
      s_fsup_touch = millis();
      if (f) f.write(data, len);
      if (done) {
        if (f) { f.close(); Serial.printf("[fsup] ok %u bytes\n", (unsigned)(idx+len)); }
        s_fsup_busy = false; s_fsup_file = nullptr;
      }
    });

  // --- deferred-job status (see the DEFERRED JOB SLOT block at the top) ---
  //   GET /jobstatus            -> the current/last job
  //   GET /jobstatus?id=N       -> that job, or 404 once a newer one replaced it
  // The routes that used to block (/nor /norbig /norwrite /norflash /sdprobe
  // /grant /verify) now answer 202 with {"job":N,"poll":"/jobstatus?id=N"}.
  // Poll until "state":"done", then read "result".
  //   curl -s esp/norbig ; sleep 3 ; curl -s 'esp/jobstatus'
  server.on("/jobstatus", HTTP_GET, [](AsyncWebServerRequest *r) {
    uint32_t want = r->hasParam("id") ? strtoul(r->getParam("id")->value().c_str(), nullptr, 0) : 0;
    if (want && want != s_job.id) {
      char j[160];
      snprintf(j, sizeof(j),
               "{\"job\":%u,\"state\":\"unknown\",\"current\":%u,"
               "\"note\":\"only the most recent job is remembered\"}",
               (unsigned)want, (unsigned)s_job.id);
      r->send(404, "application/json", j);
      return;
    }
    JobState st = s_job.state;
    char j[560];
    snprintf(j, sizeof(j),
             "{\"job\":%u,\"kind\":\"%s\",\"state\":\"%s\",\"ms\":%u,\"result\":%s}",
             (unsigned)s_job.id, jobKindName(s_job.kind), jobStateName(st),
             (unsigned)s_job.ms,
             (st == JS_DONE && s_job.result[0]) ? s_job.result : "null");
    r->send(200, "application/json", j);
  });

  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
  server.onNotFound([](AsyncWebServerRequest *r){
    if (wifiprov::captiveRedirect(r)) return;   // pops the OS "sign in to network" sheet
    r->send(404, "text/plain", "404");
  });
  server.begin();
  Serial.println("[net] HTTP + WS on :80");
}

volatile bool g_norloop = false;
void netLoop() {
  wifiprov::tick();          // scans/connects run HERE, never inside an HTTP handler
  ws.cleanupClients(); diag::tick();
  // Deferred work, on loopTask (priority 1) instead of the AsyncTCP service task
  // (priority 10, also the LwIP event pump).  Running it HERE also serialises it
  // against diag::tick() and the /norloop burst below, which bit-bang the same
  // SPI pins from this same task -- they can no longer collide.
  jobRun();
#ifndef BOARD_C3
  wavJanitor();                                  // reclaim an abandoned /wavup (SD)
#endif
  if (s_fsup_busy && millis() - s_fsup_touch > 20000) {   // ... and an abandoned /fsup (LittleFS)
    Serial.println("[fsup] abandoned upload - releasing the slot");
    if (s_fsup_file && *s_fsup_file) s_fsup_file->close();
    s_fsup_busy = false; s_fsup_file = nullptr;
  }
  // Dead-man release for /norhold + /norloop: never leave the FPGA in reset for ever
  // because someone closed the tab (see the s_holdSince declaration).
  if (s_holdSince && millis() - s_holdSince > HOLD_MAX_MS) {
    Serial.println("[nor] hold/loop timed out - releasing the bus and the FPGA reset");
    g_norloop = false;
    pinMode(PIN_SPI_CS_SD, INPUT); pinMode(PIN_SPI_SCLK, INPUT);
    pinMode(PIN_SPI_MOSI, INPUT);  pinMode(PIN_FPGA_RESET, INPUT);
    s_holdSince = 0;
  }
  static uint32_t nl_last = 0;
  if (g_norloop && millis() - nl_last > 100) {
    nl_last = millis();
    static uint32_t nl_id = 0; static uint32_t nl_n = 0;
    SPIClass sp(FSPI);
    pinMode(PIN_SPI_CS_SD, OUTPUT); digitalWrite(PIN_SPI_CS_SD, HIGH);
    sp.begin(PIN_SPI_SCLK, PIN_SPI_MISO, PIN_SPI_MOSI, PIN_SPI_CS_SD);
    sp.beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));
    digitalWrite(PIN_SPI_CS_SD, LOW);
    sp.transfer(0x9F);
    uint32_t v = 0; for (int i = 0; i < 3; i++) v = (v << 8) | sp.transfer(0x00);
    digitalWrite(PIN_SPI_CS_SD, HIGH);
    sp.endTransaction(); sp.end();
    if (v != nl_id || (++nl_n % 50) == 0) { nl_id = v; Serial.printf("[norloop] 0x%06X\n", (unsigned)v); }
  }
}
const char* netIp()   { return wifiprov::ip(); }    // live: follows an AP<->STA switch
const char* netMode() { return wifiprov::mode(); }
