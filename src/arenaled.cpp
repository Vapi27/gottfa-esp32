#include <Adafruit_NeoPixel.h>
#include <Preferences.h>
#include <math.h>
#include <string.h>
#include "arenaled.h"
#include "arena_map.h"

namespace arenaled {

// ---------------------------------------------------------------------------
//  State
// ---------------------------------------------------------------------------
static Adafruit_NeoPixel s_strip(LED_COUNT_DEFAULT, PIN_LED_DATA, NEO_GRBW + NEO_KHZ800);
#if LED_CHAIN2_ENABLE
static Adafruit_NeoPixel s_strip2(LED_COUNT_DEFAULT, PIN_LED_DATA2, NEO_GRBW + NEO_KHZ800);
#endif

static Rgbw     s_frame[LED_MAX];      // render target for the current effect
static Rgbw     s_scratch[LED_MAX];    // second render target (sub-effect crossfade)
static Rgbw     s_render[LED_MAX];     // last frame as rendered (pre-brightness/gamma)
static Rgbw     s_prev[LED_MAX];       // snapshot taken on a mode change
static uint8_t  s_spark[LED_MAX];      // "random inserts" decay envelopes

static Mode     s_mode      = MODE_CLASSIC;
static uint8_t  s_bright    = ARENA_BRIGHT_DEFAULT;
static uint8_t  s_speed     = ARENA_SPEED_DEFAULT;
static Rgbw     s_color     = { ARENA_WARM_R, ARENA_WARM_G, ARENA_WARM_B, ARENA_WARM_W };
static uint16_t s_count     = LED_COUNT_DEFAULT;
static uint16_t s_budget    = LED_POWER_BUDGET_MA;

static float    s_phase     = 0.0f;    // animation clock (s, speed-scaled)
static uint32_t s_lastUs    = 0;
static uint32_t s_lastFrame = 0;
static uint32_t s_frames    = 0;
static uint16_t s_fps       = 0;
static uint32_t s_fpsT0     = 0, s_fpsN = 0;
static float    s_amps      = 0.0f;
static bool     s_limited   = false;

static uint32_t s_xfadeT0   = 0;       // mode-change crossfade
static const uint32_t XFADE_MS = 500;

static int      s_idLed     = -1;      // mapping wizard: spotlit pixel
static int      s_idZone    = -1;
static uint32_t s_idUntil   = 0;

static Preferences s_prefs;
static uint32_t s_dirtyAt   = 0;       // debounce NVS writes

// ---------------------------------------------------------------------------
//  Small helpers
// ---------------------------------------------------------------------------
static inline uint8_t clamp8(float v) {
  if (v <= 0.0f) return 0;
  if (v >= 255.0f) return 255;
  return (uint8_t)(v + 0.5f);
}
static inline Rgbw scale(const Rgbw& c, float k) {
  return { clamp8(c.r * k), clamp8(c.g * k), clamp8(c.b * k), clamp8(c.w * k) };
}
static inline Rgbw mix(const Rgbw& a, const Rgbw& b, float t) {
  return { clamp8(a.r + (b.r - a.r) * t), clamp8(a.g + (b.g - a.g) * t),
           clamp8(a.b + (b.b - a.b) * t), clamp8(a.w + (b.w - a.w) * t) };
}
static inline Rgbw addSat(const Rgbw& a, const Rgbw& b) {
  return { (uint8_t)min(255, a.r + b.r), (uint8_t)min(255, a.g + b.g),
           (uint8_t)min(255, a.b + b.b), (uint8_t)min(255, a.w + b.w) };
}
static inline float fsin01(float x) { return 0.5f + 0.5f * sinf(x); }

// 0..255 -> x0.25 .. x4, geometric so the middle of the slider is x1.
static inline float speedFactor() { return powf(2.0f, ((float)s_speed - 128.0f) / 64.0f); }

static Rgbw hsv(float h, float s, float v) {   // h in turns [0,1), s/v in [0,1]
  h -= floorf(h);
  float i = floorf(h * 6.0f), f = h * 6.0f - i;
  float p = v * (1 - s), q = v * (1 - f * s), t = v * (1 - (1 - f) * s);
  float r, g, b;
  switch ((int)i % 6) {
    case 0:  r = v; g = t; b = p; break;
    case 1:  r = q; g = v; b = p; break;
    case 2:  r = p; g = v; b = t; break;
    case 3:  r = p; g = q; b = v; break;
    case 4:  r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
  }
  return { clamp8(r * 255), clamp8(g * 255), clamp8(b * 255), 0 };
}

// Cheap deterministic per-LED hash — gives every pixel its own flicker phase
// without storing a table.
static inline float ledPhase(uint16_t i) {
  uint32_t h = i * 2654435761u;
  h ^= h >> 15;
  return (float)(h & 0xFFFF) / 65536.0f * 6.2831853f;
}

static void fill(Rgbw* buf, const Rgbw& c) {
  for (uint16_t i = 0; i < s_count; i++) buf[i] = c;
}

// ---------------------------------------------------------------------------
//  Effects — each renders a full frame at full scale into buf[]
// ---------------------------------------------------------------------------

// Static warm white with a very subtle filament wobble (two detuned slow sines,
// ~5 % peak-to-peak). Reads as "incandescent" rather than "LED" on camera and in
// the room, without ever looking like a broken pixel.
static void fxClassic(Rgbw* buf) {
  for (uint16_t i = 0; i < s_count; i++) {
    float p = ledPhase(i);
    float k = 1.0f + 0.030f * sinf(s_phase * 1.7f + p)
                   + 0.018f * sinf(s_phase * 4.3f + p * 1.7f);
    buf[i] = scale(s_color, k);
  }
}

static void fxPulse(Rgbw* buf) {                       // whole field breathing
  float k = 0.25f + 0.75f * fsin01(s_phase * 1.1f);
  fill(buf, scale(s_color, k));
}

static void fxWave(Rgbw* buf) {                        // travelling sine along the chain
  for (uint16_t i = 0; i < s_count; i++) {
    float x = (float)i / (float)s_count;
    float k = 0.15f + 0.85f * fsin01(s_phase * 2.0f - x * 6.2831853f * 2.0f);
    buf[i] = scale(s_color, k);
  }
}

static void fxChase(Rgbw* buf) {                       // comet with a decaying tail
  const float TAIL = 9.0f;
  float head = fmodf(s_phase * 14.0f, (float)s_count);
  for (uint16_t i = 0; i < s_count; i++) {
    float d = head - (float)i;
    if (d < 0) d += (float)s_count;                    // wrap the tail around
    float k = (d < TAIL) ? (1.0f - d / TAIL) : 0.0f;
    buf[i] = scale(s_color, 0.10f + 0.90f * k * k);
  }
}

// Random inserts: a dim base glow with pixels firing at random and decaying,
// the way a real attract mode strobes single lamps.
static void fxSparkle(Rgbw* buf) {
  const Rgbw base = scale(s_color, 0.12f);
  static uint32_t lastMs = 0;
  uint32_t now = millis();
  uint32_t dt = now - lastMs;
  lastMs = now;
  if (dt > 200) dt = 200;

  uint8_t decay = (uint8_t)min<uint32_t>(255, (dt * 200) / 1000);   // ~200 units/s
  for (uint16_t i = 0; i < s_count; i++)
    s_spark[i] = (s_spark[i] > decay) ? (uint8_t)(s_spark[i] - decay) : 0;

  // Fire rate scales with speed so the slider is meaningful here too.
  uint8_t fires = (uint8_t)(1 + (dt * s_count * speedFactor()) / 3000);
  for (uint8_t k = 0; k < fires; k++) s_spark[random(s_count)] = 255;

  const Rgbw gold = { ARENA_GOLD_R, ARENA_GOLD_G, ARENA_GOLD_B, ARENA_GOLD_W };
  for (uint16_t i = 0; i < s_count; i++)
    buf[i] = addSat(base, scale(gold, (float)s_spark[i] / 255.0f));
}

// Zone sweep: light the mapped playfield areas one after the other.
static void fxZoneSweep(Rgbw* buf) {
  fill(buf, scale(s_color, 0.10f));
  uint8_t nz = arenamap::count();
  if (!nz) { fxWave(buf); return; }

  float pos = fmodf(s_phase * 0.6f, (float)nz);
  for (uint8_t z = 0; z < nz; z++) {
    float d = fabsf(pos - (float)z);
    if (d > nz / 2.0f) d = nz - d;                     // circular distance
    float k = (d < 1.5f) ? (1.0f - d / 1.5f) : 0.0f;
    if (k <= 0.0f) continue;
    const arenamap::Zone* zn = arenamap::zone(z);
    for (uint16_t i = zn->first; i < zn->first + zn->count && i < s_count; i++)
      buf[i] = scale(s_color, 0.10f + 0.90f * k);
  }
}

// Attract = the five slow animations above, auto-cycled with a crossfade so the
// wall never "cuts" from one effect to the next.
static void fxAttract(Rgbw* buf) {
  typedef void (*FxFn)(Rgbw*);
  static FxFn FX[] = { fxPulse, fxWave, fxChase, fxSparkle, fxZoneSweep };
  const uint8_t N = sizeof(FX) / sizeof(FX[0]);
  const float STEP = 14.0f, BLEND = 1.5f;              // seconds (animation clock)

  float t   = s_phase / STEP;
  uint8_t i = ((uint32_t)t) % N;
  float frac = (t - floorf(t)) * STEP;                 // seconds into this step

  FX[i](buf);
  if (frac > STEP - BLEND) {                           // crossfade into the next one
    FX[(i + 1) % N](s_scratch);
    float k = (frac - (STEP - BLEND)) / BLEND;
    for (uint16_t p = 0; p < s_count; p++) buf[p] = mix(buf[p], s_scratch[p], k);
  }
}

// Arena mode: a "ball" runs the zones in chain order, each hit flashes its zone
// and decays; every ~20 s the whole playfield flashes (jackpot).
static void fxArena(Rgbw* buf) {
  const Rgbw amber = { ARENA_AMBER_R, ARENA_AMBER_G, ARENA_AMBER_B, ARENA_AMBER_W };
  fill(buf, scale(s_color, 0.14f));

  uint8_t nz = arenamap::count();
  if (nz) {
    float pos = fmodf(s_phase * 1.1f, (float)nz);
    for (uint8_t z = 0; z < nz; z++) {
      float d = pos - (float)z;
      if (d < 0) d += nz;                              // time since this zone was hit
      float k = expf(-d * 1.6f);                       // exponential decay after the hit
      if (k < 0.02f) continue;
      const arenamap::Zone* zn = arenamap::zone(z);
      for (uint16_t i = zn->first; i < zn->first + zn->count && i < s_count; i++) {
        float ph = 1.0f - fabsf((float)(i - zn->first) / (float)max<uint16_t>(1, zn->count) - 0.5f);
        buf[i] = addSat(buf[i], scale(amber, k * ph));
      }
    }
  }

  float jp = fmodf(s_phase, 20.0f);                    // jackpot flash
  if (jp < 0.9f) {
    float k = fsin01(jp * 20.0f) * (1.0f - jp / 0.9f);
    const Rgbw gold = { ARENA_GOLD_R, ARENA_GOLD_G, ARENA_GOLD_B, ARENA_GOLD_W };
    for (uint16_t i = 0; i < s_count; i++) buf[i] = addSat(buf[i], scale(gold, k));
  }
}

static void fxRainbow(Rgbw* buf) {
  for (uint16_t i = 0; i < s_count; i++)
    buf[i] = hsv((float)i / (float)s_count * 1.5f + s_phase * 0.08f, 1.0f, 1.0f);
}

// Wiring/colour-order check: the field cycles R -> G -> B -> W at 25 %, with one
// bright pixel walking the chain so you can count LEDs and spot a dead link.
static void fxTest(Rgbw* buf) {
  uint32_t step = (uint32_t)(s_phase / 2.0f) % 4;
  Rgbw c = { 0, 0, 0, 0 };
  switch (step) {
    case 0: c.r = 64; break;
    case 1: c.g = 64; break;
    case 2: c.b = 64; break;
    default: c.w = 64; break;
  }
  fill(buf, c);
  uint16_t walk = (uint16_t)(fmodf(s_phase * 5.0f, (float)s_count));
  if (walk >= s_count) walk = s_count - 1;            // float edge case
  buf[walk] = { 255, 255, 255, 255 };
}

// ---------------------------------------------------------------------------
//  Frame assembly
// ---------------------------------------------------------------------------
// With LED_REPEATER_PIXEL the physical chain carries one extra pixel in front:
// a hidden SK6812 fed at ~4.4 V that accepts 3.3 V data and regenerates it for
// the 5 V chain behind it (see arena_config.h). It is always kept dark, and all
// indices the user ever sees stay 0-based on the first *visible* LED.
static const uint16_t OFFS = LED_REPEATER_PIXEL ? 1 : 0;

static void renderMode(Mode m, Rgbw* buf) {
  switch (m) {
    case MODE_CLASSIC: fxClassic(buf); break;
    case MODE_ATTRACT: fxAttract(buf); break;
    case MODE_ARENA:   fxArena(buf);   break;
    // Night is deliberately the warm-white die whatever the current colour is:
    // at 10 % an amber made of R+G reads orange and dirty, W stays clean.
    case MODE_NIGHT:   fill(buf, { ARENA_WARM_R, ARENA_WARM_G, ARENA_WARM_B, ARENA_WARM_W }); break;
    case MODE_RAINBOW: fxRainbow(buf); break;
    case MODE_TEST:    fxTest(buf);    break;
    case MODE_OFF:
    default:           fill(buf, { 0, 0, 0, 0 }); break;
  }
}

// Brightness + gamma, in place, so that what the power meter sees below is exactly
// what goes out on the wire. The eye is not linear and low levels on a 4-die RGBW
// pixel band badly without gamma; x^2 is a good cheap approximation of 2.2.
static void applyGainGamma(Rgbw* buf, uint8_t gain) {
  float g = (float)gain / 255.0f;
  for (uint16_t i = 0; i < s_count; i++) {
    Rgbw c = scale(buf[i], g);
    buf[i] = { (uint8_t)((c.r * c.r) / 255), (uint8_t)((c.g * c.g) / 255),
               (uint8_t)((c.b * c.b) / 255), (uint8_t)((c.w * c.w) / 255) };
  }
}

// Estimate the chain current for the frame about to be pushed and, if it exceeds
// the budget, scale the whole frame down. The ratio is applied uniformly so
// colours and relative levels are preserved — the wall just dims.
static float meterAndLimit(Rgbw* buf) {
  const float quiescent = (s_count + OFFS) * LED_MA_QUIESCENT;
  uint32_t sum = 0;
  for (uint16_t i = 0; i < s_count; i++)
    sum += buf[i].r + buf[i].g + buf[i].b + buf[i].w;

  float chans = (float)sum / 255.0f;                 // dice-equivalents at full drive
  float ma = chans * LED_MA_PER_CHANNEL + quiescent;

  s_limited = false;
  if (ma > (float)s_budget && chans > 0.0f) {
    float avail = (float)s_budget - quiescent;
    if (avail < 0) avail = 0;
    for (uint16_t i = 0; i < s_count; i++) buf[i] = scale(buf[i], avail / (chans * LED_MA_PER_CHANNEL));
    ma = (float)s_budget;
    s_limited = true;
  }
  return ma / 1000.0f;
}

static void push(const Rgbw* buf) {
  if (OFFS) s_strip.setPixelColor(0, 0, 0, 0, 0);
  for (uint16_t i = 0; i < s_count; i++)
    s_strip.setPixelColor(i + OFFS, buf[i].r, buf[i].g, buf[i].b, buf[i].w);
  s_strip.show();
#if LED_CHAIN2_ENABLE
  for (uint16_t i = 0; i < s_count; i++)
    s_strip2.setPixelColor(i + OFFS, s_strip.getPixelColor(i + OFFS));
  s_strip2.show();
#endif
}

// ---------------------------------------------------------------------------
//  Public API
// ---------------------------------------------------------------------------
static const char* MODE_NAMES[MODE_COUNT] = {
  "off", "classic", "attract", "arena", "night", "rainbow", "test"
};

const char* modeName(Mode m) { return (m < MODE_COUNT) ? MODE_NAMES[m] : "?"; }

Mode modeFromName(const char* s) {
  if (!s) return MODE_COUNT;
  for (uint8_t i = 0; i < MODE_COUNT; i++)
    if (strcasecmp(s, MODE_NAMES[i]) == 0) return (Mode)i;
  return MODE_COUNT;
}

static void markDirty() { s_dirtyAt = millis(); }

void setMode(Mode m) {
  if (m >= MODE_COUNT || m == s_mode) return;
  memcpy(s_prev, s_render, sizeof(Rgbw) * LED_MAX);    // snapshot for the crossfade
  s_xfadeT0 = millis();
  s_mode = m;
  markDirty();
}

Mode mode() { return s_mode; }

void nextMode() {
  // Button cycle skips TEST (a diagnostic, not a look) and OFF stays reachable
  // by cycling all the way round.
  Mode m = (Mode)((s_mode + 1) % MODE_TEST);
  setMode(m);
}

void setBrightness(uint8_t b) { s_bright = b; markDirty(); }
uint8_t brightness() { return s_bright; }
void setSpeed(uint8_t s) { s_speed = s; markDirty(); }
uint8_t speed() { return s_speed; }
void setColor(Rgbw c) { s_color = c; markDirty(); }
Rgbw color() { return s_color; }
uint16_t count() { return s_count; }
uint16_t budgetMa() { return s_budget; }

void setCount(uint16_t n) {
  if (n < 1) n = 1;
  if (n > LED_MAX) n = LED_MAX;
  if (n == s_count) return;
  s_count = n;
  s_strip.updateLength(n + OFFS);
  s_strip.begin();
  s_strip.clear();
  s_strip.show();
#if LED_CHAIN2_ENABLE
  s_strip2.updateLength(n + OFFS);
  s_strip2.begin();
#endif
  markDirty();
}

void setBudgetMa(uint16_t ma) {
  s_budget = ma ? ma : 1;
  markDirty();
}

void identifyLed(int led, uint32_t ms) {
  s_idLed = (led >= 0 && led < (int)s_count) ? led : -1;
  s_idZone = -1;
  s_idUntil = millis() + ms;
}

void identifyZone(int zoneIdx, uint32_t ms) {
  s_idZone = (zoneIdx >= 0 && zoneIdx < (int)arenamap::count()) ? zoneIdx : -1;
  s_idLed = -1;
  s_idUntil = millis() + ms;
}

void clearIdentify() { s_idLed = s_idZone = -1; s_idUntil = 0; }
int  identifyingLed() { return s_idLed; }

float    lastAmps()   { return s_amps; }
bool     limited()    { return s_limited; }
uint32_t frameCount() { return s_frames; }
uint16_t fps()        { return s_fps; }

void save() {
  s_prefs.putUChar("mode",   (uint8_t)s_mode);
  s_prefs.putUChar("bright", s_bright);
  s_prefs.putUChar("speed",  s_speed);
  s_prefs.putUShort("count", s_count);
  s_prefs.putUShort("budget", s_budget);
  s_prefs.putBytes("color", &s_color, sizeof(s_color));
  s_dirtyAt = 0;
}

void begin() {
  s_prefs.begin("arena", false);
  s_mode   = (Mode)s_prefs.getUChar("mode", (uint8_t)MODE_CLASSIC);
  if (s_mode >= MODE_COUNT) s_mode = MODE_CLASSIC;
  s_bright = s_prefs.getUChar("bright", ARENA_BRIGHT_DEFAULT);
  s_speed  = s_prefs.getUChar("speed",  ARENA_SPEED_DEFAULT);
  s_count  = s_prefs.getUShort("count", LED_COUNT_DEFAULT);
  s_budget = s_prefs.getUShort("budget", LED_POWER_BUDGET_MA);
  if (s_count < 1 || s_count > LED_MAX) s_count = LED_COUNT_DEFAULT;
  if (s_prefs.getBytesLength("color") == sizeof(s_color))
    s_prefs.getBytes("color", &s_color, sizeof(s_color));

  memset(s_frame, 0, sizeof(s_frame));
  memset(s_render, 0, sizeof(s_render));
  memset(s_prev, 0, sizeof(s_prev));
  memset(s_spark, 0, sizeof(s_spark));

  s_strip.updateLength(s_count + OFFS);
  s_strip.begin();
  s_strip.clear();
  s_strip.show();                       // a defined dark state before the first frame
#if LED_CHAIN2_ENABLE
  s_strip2.updateLength(s_count + OFFS);
  s_strip2.begin();
  s_strip2.clear();
  s_strip2.show();
#endif

  s_lastUs = micros();
  s_fpsT0  = millis();
  Serial.printf("[led] %u px on GPIO%d, mode=%s bright=%u budget=%u mA\n",
                s_count, PIN_LED_DATA, modeName(s_mode), s_bright, s_budget);
}

void tick() {
  uint32_t now = millis();
  if (now - s_lastFrame < (uint32_t)(1000 / LED_FRAME_HZ)) return;
  s_lastFrame = now;

  uint32_t us = micros();
  float dt = (float)(uint32_t)(us - s_lastUs) / 1e6f;
  s_lastUs = us;
  if (dt > 0.25f) dt = 0.25f;                       // after a long WiFi stall, don't jump
  s_phase += dt * speedFactor();

  renderMode(s_mode, s_frame);

  // Mode changes crossfade out of the last frame of the previous mode.
  uint32_t age = now - s_xfadeT0;
  if (s_xfadeT0 && age < XFADE_MS) {
    float k = (float)age / (float)XFADE_MS;
    for (uint16_t i = 0; i < s_count; i++) s_frame[i] = mix(s_prev[i], s_frame[i], k);
  } else {
    s_xfadeT0 = 0;
  }

  // Mapping wizard overlay — works on top of any mode, auto-expires.
  if (s_idUntil) {
    if ((int32_t)(now - s_idUntil) >= 0) clearIdentify();
    else {
      float blink = 0.35f + 0.65f * fsin01((float)now / 1000.0f * 12.0f);
      const Rgbw hot = { 255, 255, 255, 255 };
      if (s_idLed >= 0 && s_idLed < (int)s_count) {
        for (uint16_t i = 0; i < s_count; i++) s_frame[i] = scale(s_frame[i], 0.15f);
        s_frame[s_idLed] = scale(hot, blink);
      } else if (s_idZone >= 0) {
        const arenamap::Zone* zn = arenamap::zone((uint8_t)s_idZone);
        if (zn) {
          for (uint16_t i = 0; i < s_count; i++) s_frame[i] = scale(s_frame[i], 0.15f);
          for (uint16_t i = zn->first; i < zn->first + zn->count && i < s_count; i++)
            s_frame[i] = scale(hot, blink);
        }
      }
    }
  }

  memcpy(s_render, s_frame, sizeof(Rgbw) * LED_MAX);  // pre-gamma copy for the next crossfade

  uint8_t gain = (s_mode == MODE_NIGHT) ? min<uint8_t>(s_bright, ARENA_NIGHT_BRIGHT) : s_bright;
  applyGainGamma(s_frame, gain);
  s_amps = meterAndLimit(s_frame);
  push(s_frame);

  s_frames++;
  s_fpsN++;
  if (now - s_fpsT0 >= 1000) { s_fps = s_fpsN; s_fpsN = 0; s_fpsT0 = now; }

  // NVS is flash: coalesce a burst of slider moves into a single write.
  if (s_dirtyAt && now - s_dirtyAt > 3000) save();
}

}  // namespace arenaled
