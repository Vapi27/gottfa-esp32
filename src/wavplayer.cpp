// wavplayer.cpp — see wavplayer.h. ESP32-S3 sound tier. Not built on C3.
//
// Output: MCP4921 12-bit SPI DAC (mono). A cycle-paced "dac" task (core 0) clocks one
// framed SPI word per sample from a lock-free SPSC ring that the "mix"+SD task (core 1)
// fills. The mix task owns the mixer + SD + the sound-set index (wavset): it scans the
// theme folder, resolves a sound id (incl. random/sequential groups) to a file, applies
// the PSOWAV attributes (loop / break / kill / soft-kill / quit / voice bus / per-sound
// volume), streams the WAV into a mixer voice, mixes, down-mixes to mono 12-bit.
// (C) 2026 Valere Pilpil / Pstore. Original implementation.
#ifndef BOARD_C3
#include "wavplayer.h"
#include "ownership.h"
#include <math.h>
#include "wavmix.h"
#include "wavsrc.h"
#include "wavfile.h"
#include "wavset.h"
#include "sndmap.h"
#include "sndroute.h"
#include "board_config.h"
#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <string.h>
#include <atomic>
#include "esp_task_wdt.h"
#include "esp_random.h"
#include "driver/i2s.h"   // legacy I2S (Arduino-ESP32 2.0.17 / IDF 4.4) — PCM5102A output

namespace {
  constexpr int RATE   = AUDIO_RATE;
  constexpr int FRAMES = 256;                  // stereo frames mixed per I2S block

  wavmix::Mixer  mixer;                         // mix task only
  // --- theme index, DOUBLE BUFFERED (non-destructive load) ---------------------------
  // loadTheme() scans into the slot that is NOT live and only flips g_setPub once the scan
  // produced something. A transient SD dropout while re-scanning the SAME theme therefore
  // leaves the machine playing instead of going permanently silent (the old code called
  // sndset.reset() FIRST and lost the live set whatever happened next).
  wavset::Set    g_set[2];                      // mix task writes [pub^1], everyone reads [pub]
  volatile uint8_t g_setPub = 0;
  inline wavset::Set& S()  { return g_set[g_setPub]; }       // live set
  inline wavset::Set& Sx() { return g_set[g_setPub ^ 1]; }   // scratch set
  // --- per-title command map, DOUBLE BUFFERED for the same reason --------------------
  // Written by the mix task in loadTheme(), read by playLive() on the LOOP task, so it gets
  // the ramSnapCopy treatment: publish by flipping an index, plus a generation counter so a
  // reload into the same slot still re-binds the decoder (an armed bank never crosses themes).
  sndmap::Map    g_map[2];
  volatile uint8_t g_mapPub = 0;
  volatile uint32_t g_mapGen = 0;
  sndmap::Decoder  g_dec = { nullptr, 0, false, 0, -1, 0 };   // loop task only
  uint32_t         g_decGen = 0xFFFFFFFFu;                    // loop task only
  wavset::Config cfg;                           // global config.txt (loaded once)
  SPIClass       sdspi(HSPI);                   // SD only (the DAC is now I2S/DMA, no SPI)
  char           theme[24] = "orgsnd";          // mix task only
  volatile bool  g_ready = false;
  // cached status for the web UI (written by mix task, read for display elsewhere)
  volatile uint32_t sndMask = 0;                  // bit i => sound id i present in the set
  volatile uint32_t loopM = 0, voiceM = 0;        // bit i => sound i loops / is on the voice bus
  volatile int   nSnd   = 0;                       // # sounds in the loaded set
  char           themes[24][24];                   // SD-root game folders (cached at begin)
  volatile int   nThemes = 0;
  char           gameMap[64][24] = {{0}};          // /games.txt: FPGA game No -> romname/folder
  volatile int   g_lastSound = -1;                 // last sound id played (OLED/status)
  volatile int   curGameNo = -1;                   // FPGA game No of the loaded set (for hybrid routing)
  bool           g_hybrid = false;                 // config.txt sndmode=hybrid -> GOSOF80 does part of the sound
  bool           g_hasBanks = false;               // loaded set has banked sounds (id>=32) -> 80B bank/stop semantics
  // Why the last load ended the way it did — shown by the UI so "no sound" is never a mystery.
  // "ok" | "empty" (folder there, no usable WAV) | "nofolder" | "locked" (ownership gate).
  volatile const char* g_setStatus = "boot";
  volatile bool  g_silent = true;                  // no usable set -> play nothing, but NEVER wedge
  // --- mix-loop instrument (see the I2S latency note in begin()) ---------------------
  // busyUs = time a pass spends OUTSIDE i2s_write (mix + SD). When that approaches the audio
  // period the DMA queue is draining and the next hiccup is an underrun. This is the meter
  // the buffer size has to be chosen against — measured on the real card, not estimated.
  volatile uint32_t g_busyMaxUs = 0, g_busyLastUs = 0, g_lateN = 0, g_passN = 0;
  int            g_dmaCount = 8, g_dmaLen = 256;   // actual installed I2S geometry

  struct Slot {
    File f; wavsrc::Source src;
    uint32_t dataOffset, dataLen; uint8_t channels;
    int vid; bool used;
  };
  Slot slot[wavmix::MAX_VOICES];

  struct Req { uint8_t type; int sound; char theme[24]; };   // 0=play id, 1=set-theme, 2=stop all, 3=test tone (sound=ms)
  QueueHandle_t reqQ = nullptr;

  // /beep test tone.  Generated INSIDE the mix pass (see mixTask) rather than by a
  // blocking i2s_write loop: the old testTone() ran on whatever task called it --
  // in practice the AsyncTCP task -- so it (a) stalled the whole web server for up
  // to 3 s and (b) wrote I2S_NUM_0 concurrently with mixTask, the only other writer
  // of that channel.  Now the tone is just another thing the single I2S writer emits.
  volatile int  g_toneFrames = 0;    // frames of tone still to emit
  double        g_tonePh     = 0.0;
  double        g_toneStep   = 0.0;

  size_t file_read(void* ctx, uint8_t* d, size_t n) { return ((File*)ctx)->read(d, n); }
  // voice adapters: ctx = &Slot, so loop can re-seek the file
  size_t slot_fill(void* p, int16_t* dst, size_t frames) { return wavsrc::fill(&((Slot*)p)->src, dst, frames); }
  bool   slot_rewind(void* p) {
    Slot* s = (Slot*)p;
    if (!s->f || !s->f.seek(s->dataOffset)) return false;
    wavsrc::init(s->src, file_read, &s->f, s->channels, s->dataLen);
    return true;
  }

  // Walk RIFF chunks to find fmt + data (offset/len). Leaves f at data on success.
  bool findWav(File& f, WavInfo& wi) {
    wi = WavInfo{};
    uint8_t b[16];
    if (f.read(b, 12) != 12 || memcmp(b, "RIFF", 4) || memcmp(b + 8, "WAVE", 4)) return false;
    while (f.read(b, 8) == 8) {
      uint32_t len = b[4] | (b[5] << 8) | (b[6] << 16) | ((uint32_t)b[7] << 24);
      if (!memcmp(b, "fmt ", 4)) {
        uint8_t fmt[16];
        if (f.read(fmt, 16) != 16) return false;
        wi.format   = fmt[0]  | (fmt[1] << 8);
        wi.channels = fmt[2]  | (fmt[3] << 8);
        wi.rate     = fmt[4]  | (fmt[5] << 8) | (fmt[6] << 16) | ((uint32_t)fmt[7] << 24);
        wi.bits     = fmt[14] | (fmt[15] << 8);
        uint32_t extra = (len > 16 ? len - 16 : 0) + (len & 1);
        if (extra) f.seek(f.position() + extra);
      } else if (!memcmp(b, "data", 4)) {
        wi.dataOffset = f.position();
        wi.dataLen    = len;
        wi.ok = (wi.format == 1 && (wi.channels == 1 || wi.channels == 2) && wi.bits == 16);
        return wi.ok;
      } else {
        f.seek(f.position() + len + (len & 1));
      }
    }
    return false;
  }

  void reapSlots() {                            // free slots whose voice ended/was stopped
    for (int i = 0; i < wavmix::MAX_VOICES; i++)
      if (slot[i].used && !mixer.active(slot[i].vid)) { slot[i].f.close(); slot[i].used = false; }
  }

  void startVoice(const wavset::Entry* e) {
    // attribute-driven stops first (apply to existing voices), then reclaim their slots
    if      (e->attr & wavset::A_KILL)  mixer.stopAll();
    else if (e->attr & wavset::A_SKILL) mixer.stopExcept(true, false, false);
    else if (e->attr & wavset::A_QUIT)  mixer.stopExcept(true, true, true);
    if (e->attr & wavset::A_BREAK)      mixer.stopTag(e->id);
    // Mono background music: a new looping (non-voice) sound REPLACES the current loop instead
    // of stacking. Verified against the board (SEQ test on excaliba: a sustained sound is taken
    // over by the next command) and prevents loop pile-up that would exhaust the 8 voices.
    // Oneshot effects still layer + self-terminate; speech (voice bus) is untouched.
    if ((e->attr & wavset::A_LOOP) && !(e->attr & wavset::A_VOICE))
      mixer.stopActiveLoops();
    reapSlots();
    if (e->attr & wavset::A_PLACE) return;       // x = placeholder, no audio

    int si = -1;
    for (int i = 0; i < wavmix::MAX_VOICES; i++) if (!slot[i].used) { si = i; break; }
    if (si < 0) {
      static uint32_t lastW = 0; uint32_t now = millis();
      if (now - lastW > 1000) { lastW = now; log_w("[snd] all voices busy, dropped id %d", e->id); }
      return;
    }
    char path[96]; snprintf(path, sizeof(path), "/%s/%s", theme, e->file);
    File f = SD.open(path, FILE_READ);
    if (!f) { log_w("[snd] open fail %s", path); return; }
    WavInfo wi;
    if (!findWav(f, wi)) { log_w("[snd] not PCM16 %s", path); f.close(); return; }
    if (wi.dataOffset >= (uint32_t)f.size() || !f.seek(wi.dataOffset)) { f.close(); return; }
    uint32_t avail = (uint32_t)f.size() - wi.dataOffset;
    uint32_t dlen  = (wi.dataLen && wi.dataLen <= avail) ? wi.dataLen : avail;

    slot[si].f = f; slot[si].dataOffset = wi.dataOffset; slot[si].dataLen = dlen; slot[si].channels = (uint8_t)wi.channels;
    wavsrc::init(slot[si].src, file_read, &slot[si].f, (uint8_t)wi.channels, dlen);

    uint8_t bus = (e->attr & wavset::A_VOICE) ? cfg.volv : cfg.vols;       // voice vs sound scaling
    uint32_t g = (uint32_t)e->vol * bus * 255 / 10000; if (g > 255) g = 255;

    wavmix::VoiceCfg vc;
    vc.fill = slot_fill; vc.ctx = &slot[si]; vc.rewind = slot_rewind;
    vc.gain = (uint8_t)g; vc.tag = e->id;
    vc.loop = (e->attr & wavset::A_LOOP) != 0;
    vc.bg   = (e->attr & wavset::A_INIT) != 0;
    vc.voice= (e->attr & wavset::A_VOICE) != 0;
    int vid = mixer.trigger(vc);
    if (vid < 0) { slot[si].f.close(); return; }
    slot[si].vid = vid; slot[si].used = true;
  }

  // Read "/<theme>/sound.map" into the scratch map slot. Absent file = the safe defaults
  // (identity mapping, command 0 ignored) PLUS, only when the set has banked samples, the
  // legacy 29/30/31 rule — so every set that works today keeps working byte-for-byte, while
  // any title can override the guess with a file instead of a firmware change. See SOUND_MAP.md.
  void loadMap(const char* name, bool hasBanks) {
    sndmap::Map& m = g_map[g_mapPub ^ 1];
    sndmap::defaults(m);
    char path[64]; snprintf(path, sizeof(path), "/%s/sound.map", name);
    File f = SD.open(path, FILE_READ);
    if (f) {
      static char txt[1024];                       // static: 1 KB off the mix task's stack
      size_t n = f.read((uint8_t*)txt, sizeof(txt) - 1); txt[n] = 0; f.close();
      sndmap::parse(txt, m);
      log_i("[snd] sound.map '%s': gen=%d hdrs=0x%08X stop=0x%08X ignore=0x%08X",
            name, (int)m.gen, (unsigned)m.hdrMask, (unsigned)m.stopMask, (unsigned)m.ignoreMask);
    } else if (hasBanks) {
      sndmap::legacy80b(m);                        // exactly the pre-sndmap firmware behaviour
      log_w("[snd] '%s': no sound.map, set has banked ids -> LEGACY 80B rule (29/30/31) — "
            "unverified, prove it with /sndtrace", name);
    }
    g_mapPub ^= 1; g_mapGen++;                     // publish (loop task re-binds on the gen change)
  }

  // Load a theme WITHOUT destroying the live one until the new one is known good.
  //   same theme + failed re-scan  -> keep playing what we have (SD glitch tolerance)
  //   new theme  + failed scan     -> go SILENT with a status (playing the previous game's
  //                                   samples would be worse than silence), never wedge
  void loadTheme(const char* name) {
    char want[24]; strncpy(want, name, sizeof(want) - 1); want[sizeof(want) - 1] = 0;
    bool sameTheme = (strcmp(want, theme) == 0);

    if (!ownership::allowed(want)) {                   // proof-of-ownership gate: locked until a
      mixer.stopAll(); reapSlots();                    // verified dump of this game
      strncpy(theme, want, sizeof(theme) - 1); theme[sizeof(theme) - 1] = 0;
      Sx().reset(); g_setPub ^= 1;                     // publish an EMPTY set: nothing can play
      g_hasBanks = false; sndMask = loopM = voiceM = 0; nSnd = 0;
      g_silent = true; g_setStatus = "locked";
      loadMap(theme, false);
      log_w("[snd] '%s' LOCKED (ownership gate) — dump this game's CPU ROM to unlock", theme);
      return;
    }

    wavset::Set& scratch = Sx();                       // scan HERE; the live set keeps playing
    scratch.reset();
    bool haveDir = false;
    char dirpath[32]; snprintf(dirpath, sizeof(dirpath), "/%s", want);
    File dir = SD.open(dirpath);
    if (dir && dir.isDirectory()) {
      haveDir = true;
      for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        if (!f.isDirectory()) {
          const char* nm = f.name(); const char* base = strrchr(nm, '/');
          scratch.addName(base ? base + 1 : nm);
        }
        f.close();
      }
    }
    if (dir) dir.close();

    if (scratch.nEntry == 0) {                         // nothing usable in there
      const char* why = haveDir ? "empty" : "nofolder";
      log_w("[snd] theme '%s': %s", want, why);
      if (sameTheme) { g_setStatus = why; return; }    // keep the live set: a re-scan glitch
      mixer.stopAll(); reapSlots();                    // different game: silence beats wrong sound
      strncpy(theme, want, sizeof(theme) - 1); theme[sizeof(theme) - 1] = 0;
      g_setPub ^= 1;                                   // publish the empty scratch set
      g_hasBanks = false; sndMask = loopM = voiceM = 0; nSnd = 0;
      g_silent = true; g_setStatus = why;
      loadMap(theme, false);
      return;
    }

    // --- the new set is good: switch to it -------------------------------------------
    mixer.stopAll(); reapSlots();
    strncpy(theme, want, sizeof(theme) - 1); theme[sizeof(theme) - 1] = 0;
    g_hasBanks = false;
    for (int i = 0; i < scratch.nEntry; i++) if (scratch.entry[i].id >= 32) g_hasBanks = true;
    loadMap(theme, g_hasBanks);

    // sound.map voice=/loop= are an OVERLAY on the filename attributes: the file name still
    // wins where it already says 'l'/'v', the map can only ADD. Applied to the scratch set
    // before it goes live, so no entry is ever half-patched while it is playable.
    { const sndmap::Map& mp = g_map[g_mapPub];
      for (int i = 0; i < scratch.nEntry; i++) {
        wavset::Entry& e = scratch.entry[i];
        if (sndmap::bitGet(mp.voice, e.id)) e.attr |= wavset::A_VOICE;
        if (sndmap::bitGet(mp.loop,  e.id)) e.attr |= wavset::A_LOOP;
      } }

    g_setPub ^= 1;                                     // PUBLISH: scratch becomes live
    mixer.setMix(cfg.mix);
    { uint32_t m = 0, lm = 0, vm = 0; for (int i = 0; i < S().nEntry; i++) {
        const wavset::Entry& e = S().entry[i]; if (e.id < 0 || e.id >= 32) continue;
        m |= (1u << e.id);
        if (e.attr & wavset::A_LOOP)  lm |= (1u << e.id);
        if (e.attr & wavset::A_VOICE) vm |= (1u << e.id); }            // cache set status for web UI
      sndMask = m; loopM = lm; voiceM = vm; nSnd = S().nEntry; }
    g_silent = false; g_setStatus = "ok";
    log_i("[snd] theme '%s': %d sounds, %d groups", theme, S().nEntry, S().nGroup);
    for (int i = 0; i < S().nEntry; i++)               // autoplay init/background sounds
      if (S().entry[i].attr & wavset::A_INIT) startVoice(&S().entry[i]);
  }

  void handleReq(const Req& r) {
    if (r.type == 1) { loadTheme(r.theme); return; }
    if (r.type == 2) { mixer.stopAll(); reapSlots(); return; }   // stop all voices (web UI)
    if (r.type == 3) {                                          // /beep: arm the 440 Hz test tone
      g_tonePh   = 0.0;
      g_toneStep = 2.0 * 3.14159265 * 440.0 / RATE;
      g_toneFrames = (int)(((long)RATE * r.sound) / 1000);
      return;
    }
    int id = S().pick(r.sound, esp_random());             // resolve random/sequential group
    const wavset::Entry* e = S().find(id);
    if (e) startVoice(e);
    else   log_w("[snd] no sound %d in theme '%s'", r.sound, theme);
  }

  // --- core 1: mixer + SD + set owner; writes mixed audio straight to I2S (DMA paces it) -------
  // No more SPSC ring + cycle-paced DAC task: the I2S peripheral clocks the DMA buffer out by
  // itself, so i2s_write() blocks only when the DMA queue is full -> natural pacing, ~0 CPU, and
  // core 0 is now free.
  void mixTask(void*) {
    static int16_t buf[FRAMES * 2];   // mixer output (stereo int16)
    static int16_t out[FRAMES * 2];   // mono down-mix duplicated to L/R for the I2S frame
    esp_task_wdt_add(nullptr);
    Req req;
    const uint32_t periodUs = (uint32_t)((1000000ull * FRAMES) / RATE);   // audio per pass
    for (;;) {
      uint32_t tA = micros();
      // Drain up to REQ_PER_PASS requests instead of one: a burst of cues used to start one
      // per pass = one every FRAMES/RATE (5.8 ms), so eight queued sounds took ~46 ms to even
      // begin. Bounded, because each start does an SD open (tens of ms on a 1 MHz bus) and the
      // DMA queue is the only thing between this loop and an underrun.
      const int REQ_PER_PASS = 2;
      for (int k = 0; k < REQ_PER_PASS && xQueueReceive(reqQ, &req, 0) == pdTRUE; k++) handleReq(req);
      mixer.mix(buf, FRAMES);
      for (int i = 0; i < FRAMES; i++) {                            // down-mix to mono (clip), duplicate L/R
        int32_t m = ((int32_t)buf[2 * i] + buf[2 * i + 1]) >> 1;
        if (m > 32767) m = 32767; else if (m < -32768) m = -32768;
        out[2 * i] = (int16_t)m; out[2 * i + 1] = (int16_t)m;
      }
      if (g_toneFrames > 0) {          // /beep: replace this pass with the synth tone
        for (int i = 0; i < FRAMES; i++) {
          int16_t sv = (int16_t)(9000.0 * sin(g_tonePh));   // ~0.27 full-scale, comfortable
          g_tonePh += g_toneStep; if (g_tonePh > 6.2831853) g_tonePh -= 6.2831853;
          out[2 * i] = sv; out[2 * i + 1] = sv;
        }
        g_toneFrames -= FRAMES;
        if (g_toneFrames < 0) g_toneFrames = 0;
      }
      reapSlots();
      // Everything above is the pass's REAL work; i2s_write below is pure waiting (it returns
      // immediately while the DMA queue has room). busy >= periodUs means this pass consumed
      // more wall time than the audio it produced -> the queue is draining. See mixStats().
      uint32_t busy = micros() - tA;
      g_busyLastUs = busy;
      if (busy > g_busyMaxUs) g_busyMaxUs = busy;
      if (busy >= periodUs) g_lateN++;
      g_passN++;
      size_t wrote;
      i2s_write(I2S_NUM_0, out, sizeof(out), &wrote, portMAX_DELAY); // blocks only when DMA full -> paces to RATE
      esp_task_wdt_reset();
    }
  }

  void loadConfig() {                            // /config.txt at SD root (optional)
    wavset::defaultConfig(cfg);
    File f = SD.open("/config.txt", FILE_READ);
    if (!f) return;
    char txt[512]; size_t n = f.read((uint8_t*)txt, sizeof(txt) - 1); txt[n] = 0; f.close();
    wavset::parseConfig(txt, cfg);
    // sndmode=hybrid => the FPGA's GOSOF80 synthesises part of the sound; the ESP plays only what
    // GOSOF80 can't (speech + complex 80B), per sndroute. Default (absent) = full = ESP plays all.
    g_hybrid = false;
    char* p = strstr(txt, "sndmode");
    if (p) { char* nl = strchr(p, '\n'); char sv = nl ? *nl : 0; if (nl) *nl = 0;
             g_hybrid = strstr(p, "hybrid") != nullptr; if (nl) *nl = sv; }
    log_i("[snd] config: mix=%d volv=%d vols=%d stheme=%s sndmode=%s",
          cfg.mix, cfg.volv, cfg.vols, cfg.stheme, g_hybrid ? "hybrid" : "full");
  }

  // /games.txt at SD root: lines "<No> <romname>" (# = comment). FPGA game-select No -> folder.
  // No = GottFA80_PLuS gamelist index (manual Appendix A), as sent on the link (0x40|No).
  void loadGames() {
    for (int i = 0; i < 64; i++) gameMap[i][0] = 0;
    File f = SD.open("/games.txt", FILE_READ);
    if (!f) { log_i("[snd] no /games.txt (FPGA game-select uses raw number)"); return; }
    char buf[2048]; size_t len = f.read((uint8_t*)buf, sizeof(buf) - 1); buf[len] = 0; f.close();
    int n = 0; char* save = nullptr;
    for (char* line = strtok_r(buf, "\n", &save); line; line = strtok_r(nullptr, "\n", &save)) {
      if (line[0] == '#' || line[0] == '\r' || !line[0]) continue;
      int no; char rom[24];
      if (sscanf(line, "%d %23s", &no, rom) == 2 && no >= 0 && no < 64) {
        strncpy(gameMap[no], rom, 23); gameMap[no][23] = 0; n++; }
    }
    log_i("[snd] games.txt: %d game mappings", n);
  }
}

namespace wavplayer {

bool begin() {
  reqQ = xQueueCreate(8, sizeof(Req));
  if (!reqQ) return false;

  // --- SD + config FIRST, but STRICTLY non-fatal ---
  // The invariant that matters is "a flaky SD never silences the sound tier", not the order:
  // every call below tolerates a missing card (SD.begin returns false, the config falls back
  // to defaults) and I2S is installed unconditionally right after. Reading /config.txt first
  // is what lets the DMA geometry — i.e. the output latency — be tuned without a reflash.
  sdspi.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
  // 1 MHz is the most tolerant of long leads / weak 3.3V. NOTE: it is also the sound tier's
  // real bottleneck — one 44.1 kHz mono voice is 88 kB/s and this bus tops out near 125 kB/s.
  // The real fix is a clean, decoupled board (add a 10-100uF cap across the card's 3V3/GND);
  // raise this before shortening the DMA queue (see the latency note below).
  if (!SD.begin(PIN_SD_CS, sdspi, 1000000)) log_e("[snd] SD mount failed (audio still up)");
  loadConfig();

  // --- I2S, unconditional ---
  // The DAC/I2S must come up even if the SD failed to mount, so audio hardware can
  // be tested (testTone) and a flaky SD never silences the whole sound tier.
  // I2S TX -> PCM5102A (16-bit, DMA). Mono is sent on both L/R. SCK->GND on the module (no MCLK).
  //
  // LATENCY. dma_buf_count * dma_buf_len frames sit ahead of the DAC: the stock 8 x 256 is
  // 2048 frames = 46 ms before a new cue is audible. That queue is ALSO the only elasticity
  // covering an SD read: wavsrc tops up 1024 bytes at a time, which at 1 MHz SPI is ~8.2 ms of
  // bus time per voice, so two voices topping up in the same pass already burn ~17 ms. Halving
  // the queue halves the latency AND the margin — so it is a config knob (i2sn/i2slen in
  // /config.txt) checked against mixStats(), not a number changed on a hunch. Default unchanged.
  i2s_config_t i2scfg = {};
  i2scfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  i2scfg.sample_rate = RATE;
  i2scfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  i2scfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  i2scfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  i2scfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  g_dmaCount = cfg.i2sn; g_dmaLen = cfg.i2slen;   // clamped by wavset::parseConfig
  i2scfg.dma_buf_count = g_dmaCount;
  i2scfg.dma_buf_len = g_dmaLen;
  i2scfg.use_apll = false;           // APLL can fail to generate the I2S clock on some S3 -> silence
  i2scfg.tx_desc_auto_clear = true;  // output silence on underrun (no click/repeat)
  if (i2s_driver_install(I2S_NUM_0, &i2scfg, 0, nullptr) != ESP_OK) { log_e("[snd] I2S install failed"); return false; }
  i2s_pin_config_t i2spin = {};
  i2spin.mck_io_num   = I2S_PIN_NO_CHANGE;
  i2spin.bck_io_num   = PIN_I2S_BCK;
  i2spin.ws_io_num    = PIN_I2S_LRCK;
  i2spin.data_out_num = PIN_I2S_DOUT;
  i2spin.data_in_num  = I2S_PIN_NO_CHANGE;
  i2s_set_pin(I2S_NUM_0, &i2spin);

  mixer.reset();
  g_set[0].reset(); g_set[1].reset();
  sndmap::defaults(g_map[0]); sndmap::defaults(g_map[1]);

  // cache the game folders on the SD root for the web UI (now, before the tasks touch SD)
  nThemes = 0;
  { File root = SD.open("/");
    if (root && root.isDirectory())
      for (File f = root.openNextFile(); f && nThemes < 24; f = root.openNextFile()) {
        if (f.isDirectory()) { const char* nm = f.name(); const char* b = strrchr(nm, '/'); b = b ? b + 1 : nm;
          if (b[0] != '.' && strcmp(b, "System Volume Information")) {
            strncpy(themes[nThemes], b, 23); themes[nThemes][23] = 0; nThemes++; } }
        f.close(); }
    if (root) root.close(); }

  loadGames();                                    // /games.txt FPGA game-select map (config: above)
  theme[0] = 0;                                   // no theme yet -> the first load is never a "re-scan"
  loadTheme(cfg.stheme);                          // index + autoplay init sounds

  // I2S DMA clocks the output -> no core-0 busy-loop. One mix task (core 1); core 0 is free.
  xTaskCreatePinnedToCore(mixTask, "mix", 8192, nullptr, 3, nullptr, 1);

  g_ready = true;
  log_i("[snd] wavplayer ready (PCM5102A I2S, %d Hz, mono->L/R, dma %dx%d = %u ms, set '%s' %s)",
        RATE, g_dmaCount, g_dmaLen,
        (unsigned)((1000u * g_dmaCount * g_dmaLen) / RATE), theme, (const char*)g_setStatus);
  return true;
}

void setTheme(const char* t) {
  if (!g_ready || !reqQ) return;
  Req r; r.type = 1; r.sound = 0;
  strncpy(r.theme, t, sizeof(r.theme) - 1); r.theme[sizeof(r.theme) - 1] = 0;
  xQueueSend(reqQ, &r, 0);
}

bool play(int soundId) {
  if (!g_ready || !reqQ) return false;
  g_lastSound = soundId;
  Req r; r.type = 0; r.sound = soundId; r.theme[0] = 0;
  bool ok = xQueueSend(reqQ, &r, 0) == pdTRUE;
  if (!ok) {
    static uint32_t lastW = 0; uint32_t now = millis();
    if (now - lastW > 1000) { lastW = now; log_w("[snd] queue full, dropped sound %d", soundId); }
  }
  return ok;
}

void stopAll() {
  if (!g_ready || !reqQ) return;
  Req r; r.type = 2; r.sound = 0; r.theme[0] = 0;
  xQueueSend(reqQ, &r, 0);                         // mix task does mixer.stopAll()
}
bool ready()   { return g_ready; }

// --- HARDWARE TEST: 440 Hz sine straight to the PCM5102A, no SD/mixer/WAV. ---
// Isolates the DAC from every software path: if you hear this beep, I2S + DAC +
// wiring are all good and any silence is a SD/WAV problem instead.
//
// This used to synthesise AND i2s_write() the whole tone on the CALLER's task,
// which for the /beep route is AsyncTCP's service task: it blocked every HTTP
// and WebSocket request for up to 3 s and wrote I2S_NUM_0 at the same time as
// mixTask.  It is now a queue request like play()/setTheme(); mixTask emits the
// tone one mix pass at a time, so there is exactly one I2S writer and nothing
// blocks.  Returns false if the sound tier is not up.
bool requestTestTone(int ms) {
  if (!g_ready || !reqQ) return false;
  if (ms < 50) ms = 50; if (ms > 3000) ms = 3000;
  Req r; r.type = 3; r.sound = ms; r.theme[0] = 0;
  return xQueueSend(reqQ, &r, 0) == pdTRUE;
}

// --- cached status for the web UI (benign cross-task reads, display only) ---
const char* curTheme()        { return theme; }
uint32_t    soundMask()       { return sndMask; }
int soundList(uint16_t* out, int max) {           // present sounds (incl. banked 32..95) for the web UI
  int n = 0;
  const wavset::Set& s = g_set[g_setPub];
  for (int i = 0; i < s.nEntry && n < max; i++) {
    const wavset::Entry& e = s.entry[i];
    if (e.id < 0 || e.id > 95) continue;
    uint8_t f = 0;
    if (e.attr & wavset::A_LOOP)  f |= 1;
    if (e.attr & wavset::A_VOICE) f |= 2;
    out[n++] = (uint16_t)((e.id << 2) | f);
  }
  return n;
}
uint32_t    loopMask()        { return loopM; }
uint32_t    voiceMask()       { return voiceM; }
int         soundCount()      { return nSnd; }
int         themeCount()      { return nThemes; }
const char* themeName(int i)  { return (i >= 0 && i < nThemes) ? themes[i] : ""; }
const char* gameRom(int no)   { return (no >= 0 && no < 64) ? gameMap[no] : ""; }
int         lastSound()       { return g_lastSound; }
void selectGame(int no) {                         // FPGA game No -> load that game's set
  if (no < 0 || no >= 64 || !gameMap[no][0]) { log_w("[snd] game No %d not in games.txt", no); return; }
  curGameNo = no;                                 // remember for hybrid routing (playLive)
  setTheme(gameMap[no]);
}

// Re-bind the loop task's decoder when the mix task has published a new map. The generation
// counter (not the pointer) is the test: reloading into the same slot must still reset an
// armed bank, or a header from the previous title could land on the first command of the new one.
static const sndmap::Map* liveMap() {
  const sndmap::Map* m = &g_map[g_mapPub];
  uint32_t gen = g_mapGen;
  if (g_decGen != gen || g_dec.m != m) { sndmap::bind(g_dec, m); g_decGen = gen; }
  return m;
}

// FPGA live sound path. What a command MEANS is now data (the title's sound.map, see
// SOUND_MAP.md) instead of a hardcoded 80B guess: sndmap resolves headers/banks, stop-all,
// ignored values and remaps. Hybrid routing still applies afterwards, and play() stays
// unconditional so the web/diag sound test can fire any id.
bool playLive(int soundId) {
  if (soundId < 0 || soundId > 31) return false;
  liveMap();
  sndmap::Out o = sndmap::feed(g_dec, (uint8_t)soundId, millis());
  if (o.act == sndmap::ACT_STOPALL) { stopAll(); return true; }
  if (o.act != sndmap::ACT_PLAY) return false;                            // ignored / header only
  if (g_silent) return false;                                             // no usable set: stay quiet
  if (g_hybrid && !sndroute::espPlays(curGameNo, o.id)) return false;     // GOSOF80 handles it
  return play(o.id);
}

// The sound bus went idle (wire byte 0x30, SOUND_WIRE.md). On System 80/80A the tone plays
// only WHILE a code is presented, so this is a real "stop"; on 80B the command was latched on
// the strobe and the release means nothing. sound.map decides (release=stop|ignore).
void soundRelease() {
  liveMap();
  sndmap::Out o = sndmap::release(g_dec, millis());
  if (o.act == sndmap::ACT_STOPALL) stopAll();
}

bool soundHybrid() { return g_hybrid; }

// --- status / instrument for the UI -----------------------------------------------------
const char* setStatus() { return (const char*)g_setStatus; }
bool        silent()    { return g_silent; }

// Mix-loop health. `busyMax`/`busyLast` = microseconds a pass spent doing real work (mix + SD)
// outside the paced i2s_write; `period` = the microseconds of audio one pass produces. Once
// busy approaches period the DMA queue stops refilling and the next SD hiccup is an audible
// underrun, so `late` (passes where busy >= period) is the number to watch when shortening
// the queue with i2sn/i2slen. bufMs = the current output latency.
void mixStats(uint32_t& busyMaxUs, uint32_t& busyLastUs, uint32_t& lateN, uint32_t& passN,
              uint32_t& periodUs, uint32_t& bufMs) {
  busyMaxUs = g_busyMaxUs; busyLastUs = g_busyLastUs; lateN = g_lateN; passN = g_passN;
  periodUs  = (uint32_t)((1000000ull * FRAMES) / RATE);
  bufMs     = (uint32_t)((1000u * g_dmaCount * g_dmaLen) / RATE);
}
void mixStatsReset() { g_busyMaxUs = 0; g_lateN = 0; g_passN = 0; }

} // namespace wavplayer
#endif // !BOARD_C3
