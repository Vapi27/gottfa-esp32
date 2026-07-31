#include <Adafruit_NeoPixel.h>
#include <Preferences.h>
#include <math.h>
#include <string.h>
#include "arenaled.h"
#include "arena_map.h"
#include "arena_pf.h"

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
static char     s_order[8]  = ARENA_ORDER_DEFAULT;
static uint32_t s_bootMs    = 0;       // soft-start reference

// The web handlers run in the AsyncTCP task, the renderer in loop(). Anything
// that reallocates or re-initialises the strip is therefore NOT done inline from
// a request — it is flagged here and applied at the top of the next tick(), on
// the render task, where nothing else is touching the pixel buffer.
static volatile bool s_pendLen = false;

// The animation clock. It only ever accumulates, so it must be a double: a float
// carries 24 mantissa bits, and at ~0.017 s per frame its ulp reaches the frame
// increment after a few days of uptime — every effect would first run ~2x too
// fast, then freeze outright because `s_phase += dt` rounds to no change at all.
// A wall piece runs for months, so precision here is a correctness requirement,
// not a nicety. Effects never use it raw: phase() below reduces it modulo that
// effect's own period FIRST, in double, and only then narrows to float — so the
// value a sine or an fmod actually sees always stays small and exact.
static double   s_phase     = 0.0;     // animation clock (s, speed-scaled)
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

static const double TAU = 6.283185307179586;

// s_phase * rate, reduced modulo `period` before narrowing to float. Every effect
// gets its time base through this, which is what keeps them exact indefinitely
// (and continuous — there is no global wrap point to step over).
static inline float phase(double rate, double period) {
  return (float)fmod(s_phase * rate, period);
}

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
  const float t1 = phase(1.7, TAU), t2 = phase(4.3, TAU);
  for (uint16_t i = 0; i < s_count; i++) {
    float p = ledPhase(i);
    float k = 1.0f + 0.030f * sinf(t1 + p)
                   + 0.018f * sinf(t2 + p * 1.7f);
    buf[i] = scale(s_color, k);
  }
}

static void fxPulse(Rgbw* buf) {                       // whole field breathing
  float k = 0.25f + 0.75f * fsin01(phase(1.1, TAU));
  fill(buf, scale(s_color, k));
}

static void fxWave(Rgbw* buf) {                        // travelling sine along the chain
  const float t = phase(2.0, TAU);
  for (uint16_t i = 0; i < s_count; i++) {
    float x = (float)i / (float)s_count;
    float k = 0.15f + 0.85f * fsin01(t - x * 6.2831853f * 2.0f);
    buf[i] = scale(s_color, k);
  }
}

static void fxChase(Rgbw* buf) {                       // comet with a decaying tail
  const float TAIL = 9.0f;
  float head = phase(14.0, (double)s_count);
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

  float pos = phase(0.6, (double)nz);
  for (uint8_t z = 0; z < nz; z++) {
    float d = fabsf(pos - (float)z);
    if (d > nz / 2.0f) d = nz - d;                     // circular distance
    float k = (d < 1.5f) ? (1.0f - d / 1.5f) : 0.0f;
    if (k <= 0.0f) continue;
    const arenamap::Zone* zn = arenamap::zone(z);
    if (!zn) continue;                                 // table can shrink from the HTTP task
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

  double t   = s_phase / STEP;
  uint8_t i  = (uint8_t)fmod(t, (double)N);
  float frac = (float)((t - floor(t)) * STEP);         // seconds into this step

  FX[i](buf);
  if (frac > STEP - BLEND) {                           // crossfade into the next one
    FX[(i + 1) % N](s_scratch);
    float k = (frac - (STEP - BLEND)) / BLEND;
    for (uint16_t p = 0; p < s_count; p++) buf[p] = mix(buf[p], s_scratch[p], k);
  }
}

// Geometric Arena mode — used when the pixels have been placed on the playfield
// (arenapf). Chain order is an accident of wiring; position is the real thing, so
// once we know where each pixel *is*, the effects should be about the table and
// not about the cable. Two waves at right angles: one climbing from the flippers
// to the back panel, one sweeping across, plus a ripple leaving the middle.
static void fxArenaGeo(Rgbw* buf) {
  const Rgbw amber = { ARENA_AMBER_R, ARENA_AMBER_G, ARENA_AMBER_B, ARENA_AMBER_W };
  const Rgbw gold  = { ARENA_GOLD_R,  ARENA_GOLD_G,  ARENA_GOLD_B,  ARENA_GOLD_W  };
  fill(buf, scale(s_color, 0.14f));

  const float up    = phase(1.0, 1.0);          // 0..1, climbing the playfield
  const float ripT  = phase(1.0, 6.0) / 6.0f;   // ripple every 6 s
  const float rip   = ripT * 1.15f;             // radius, overshoots so it leaves

  for (uint16_t i = 0; i < s_count; i++) {
    float x, y;
    if (!arenapf::xy(i, x, y)) continue;        // pixel not placed: leave it on the wash

    // Wave climbing from the flippers (y=1) to the back panel (y=0).
    float d = fabsf((1.0f - y) - up);
    if (d > 0.5f) d = 1.0f - d;                 // wrap: the wave is continuous
    buf[i] = addSat(buf[i], scale(amber, expf(-d * d * 90.0f)));

    // Ripple leaving the middle of the playfield.
    const float dx = x - 0.5f, dy = y - 0.55f;
    const float r  = sqrtf(dx * dx + dy * dy);
    const float dr = fabsf(r - rip);
    if (dr < 0.12f)
      buf[i] = addSat(buf[i], scale(gold, (1.0f - dr / 0.12f) * (1.0f - ripT) * 0.9f));
  }
}

// Arena mode: a "ball" runs the zones in chain order, each hit flashes its zone
// and decays; every ~20 s the whole playfield flashes (jackpot). Falls back to
// this whenever no pixel has been placed on the playfield yet.
static void fxArena(Rgbw* buf) {
  if (arenapf::anyAssigned()) { fxArenaGeo(buf); return; }

  const Rgbw amber = { ARENA_AMBER_R, ARENA_AMBER_G, ARENA_AMBER_B, ARENA_AMBER_W };
  fill(buf, scale(s_color, 0.14f));

  uint8_t nz = arenamap::count();
  if (nz) {
    float pos = phase(1.1, (double)nz);
    for (uint8_t z = 0; z < nz; z++) {
      float d = pos - (float)z;
      if (d < 0) d += nz;                              // time since this zone was hit
      float k = expf(-d * 1.6f);                       // exponential decay after the hit
      if (k < 0.02f) continue;
      const arenamap::Zone* zn = arenamap::zone(z);
      if (!zn) continue;                               // table can shrink from the HTTP task
      for (uint16_t i = zn->first; i < zn->first + zn->count && i < s_count; i++) {
        float ph = 1.0f - fabsf((float)(i - zn->first) / (float)max<uint16_t>(1, zn->count) - 0.5f);
        buf[i] = addSat(buf[i], scale(amber, k * ph));
      }
    }
  }

  float jp = phase(1.0, 20.0);                        // jackpot flash
  if (jp < 0.9f) {
    float k = fsin01(jp * 20.0f) * (1.0f - jp / 0.9f);
    const Rgbw gold = { ARENA_GOLD_R, ARENA_GOLD_G, ARENA_GOLD_B, ARENA_GOLD_W };
    for (uint16_t i = 0; i < s_count; i++) buf[i] = addSat(buf[i], scale(gold, k));
  }
}

static void fxRainbow(Rgbw* buf) {
  const float hueT = phase(0.08, 1.0);
  for (uint16_t i = 0; i < s_count; i++)
    buf[i] = hsv((float)i / (float)s_count * 1.5f + hueT, 1.0f, 1.0f);
}

// Wiring/colour-order check: the field cycles R -> G -> B -> W, with one bright
// white pixel walking the chain so you can count LEDs and spot a dead link.
static void fxTest(Rgbw* buf) {
  uint32_t step = (uint32_t)fmod(s_phase / 2.0, 4.0);
  Rgbw c = { 0, 0, 0, 0 };
  switch (step) {
    case 0: c.r = 160; break;
    case 1: c.g = 160; break;
    case 2: c.b = 160; break;
    default: c.w = 160; break;
  }
  fill(buf, c);
  // On a single-LED bench there is nowhere for the walker to walk, and drawing it
  // would sit on the only pixel and hold it white forever — which is exactly the
  // colour-order check this mode exists for. Field only below 2 pixels.
  if (s_count > 1) {
    uint16_t walk = (uint16_t)phase(5.0, (double)s_count);
    if (walk >= s_count) walk = s_count - 1;          // float edge case
    buf[walk] = { 255, 255, 255, 255 };
  }
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
//  Pixel colour order
//  SK6812MINI-RGBW is GRBW, but reels and clones vary and the only way to know
//  is to look at the wall. Rather than make that a reflash-and-guess loop, the
//  order is a live setting: Adafruit_NeoPixel::updateType() re-types the chain in
//  place (all these are 4-byte pixels, so nothing is reallocated).
// ---------------------------------------------------------------------------
struct OrderDef { const char* name; neoPixelType type; };
static const OrderDef ORDERS[] = {
  { "grbw", NEO_GRBW }, { "rgbw", NEO_RGBW }, { "gbrw", NEO_GBRW },
  { "brgw", NEO_BRGW }, { "rbgw", NEO_RBGW }, { "bgrw", NEO_BGRW },
};
static const uint8_t ORDER_N = sizeof(ORDERS) / sizeof(ORDERS[0]);

static neoPixelType orderType(const char* name) {
  for (uint8_t i = 0; i < ORDER_N; i++)
    if (strcasecmp(name, ORDERS[i].name) == 0) return ORDERS[i].type;
  return NEO_GRBW;
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
  // s_count itself is safe to move now: every buffer is statically sized to
  // LED_MAX, and Adafruit_NeoPixel::setPixelColor() bounds-checks against its own
  // length, so at worst one frame is pushed at the old chain length.
  s_count = n;
  s_pendLen = true;                     // realloc happens in tick(), see above
  markDirty();
}

// Runs on the render task only.
static void applyPending() {
  if (!s_pendLen) return;
  s_pendLen = false;
  // Blank at the OLD length first. Shrinking the count otherwise stops addressing
  // the dropped pixels without ever telling them to go out, so the tail of the
  // chain stays latched on whatever it last showed — lit, and no longer counted
  // by the power meter, which is the dangerous direction for the estimate.
  s_strip.clear();
  s_strip.show();
  s_strip.updateLength(s_count + OFFS);
  s_strip.begin();
  s_strip.clear();
  s_strip.show();
#if LED_CHAIN2_ENABLE
  s_strip2.clear();
  s_strip2.show();
  s_strip2.updateLength(s_count + OFFS);
  s_strip2.begin();
  s_strip2.clear();
  s_strip2.show();
#endif
}

void setBudgetMa(uint16_t ma) {
  s_budget = ma ? ma : 1;
  markDirty();
}

const char* order() { return s_order; }

bool setOrder(const char* s) {
  if (!s) return false;
  for (uint8_t i = 0; i < ORDER_N; i++) {
    if (strcasecmp(s, ORDERS[i].name) != 0) continue;
    strncpy(s_order, ORDERS[i].name, sizeof(s_order) - 1);
    s_order[sizeof(s_order) - 1] = '\0';
    s_strip.updateType(ORDERS[i].type + NEO_KHZ800);
#if LED_CHAIN2_ENABLE
    s_strip2.updateType(ORDERS[i].type + NEO_KHZ800);
#endif
    markDirty();
    return true;
  }
  return false;
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
  s_prefs.putString("order", s_order);
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
  String ord = s_prefs.getString("order", ARENA_ORDER_DEFAULT);
  strncpy(s_order, ord.c_str(), sizeof(s_order) - 1);
  s_order[sizeof(s_order) - 1] = '\0';

  memset(s_frame, 0, sizeof(s_frame));
  memset(s_render, 0, sizeof(s_render));
  memset(s_prev, 0, sizeof(s_prev));
  memset(s_spark, 0, sizeof(s_spark));

  s_strip.updateType(orderType(s_order) + NEO_KHZ800);
  s_strip.updateLength(s_count + OFFS);
  s_strip.begin();
  s_strip.clear();
  s_strip.show();                       // a defined dark state before the first frame
#if LED_CHAIN2_ENABLE
  s_strip2.updateType(orderType(s_order) + NEO_KHZ800);
  s_strip2.updateLength(s_count + OFFS);
  s_strip2.begin();
  s_strip2.clear();
  s_strip2.show();
#endif

  s_lastUs = micros();
  s_fpsT0  = millis();
  s_bootMs = 0;                         // armed on the first rendered frame, not here:
                                        // arenanet::begin() blocks for 0.5-12 s right
                                        // after this and would eat the whole ramp
  Serial.printf("[led] %u px on GPIO%d, order=%s mode=%s bright=%u budget=%u mA\n",
                s_count, PIN_LED_DATA, s_order, modeName(s_mode), s_bright, s_budget);
}

void tick() {
  uint32_t now = millis();
  if (now - s_lastFrame < (uint32_t)(1000 / LED_FRAME_HZ)) return;
  s_lastFrame = now;

  applyPending();       // strip length changes land here, not in an HTTP handler

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
#if ARENA_SOFTSTART_MS > 0
  // Soft start: ease the chain up over the first second so 100+ pixels lighting
  // at once can't trip the PSU's over-current hiccup or slam the bulk caps.
  if (!s_bootMs) s_bootMs = now ? now : 1;   // first frame: arm the ramp here
  uint32_t sinceBoot = now - s_bootMs;
  if (sinceBoot < ARENA_SOFTSTART_MS)
    gain = (uint8_t)((uint32_t)gain * sinceBoot / ARENA_SOFTSTART_MS);
#endif
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
