#include <Adafruit_NeoPixel.h>
#include <Preferences.h>
#include <math.h>
#include <string.h>
#include "arenaled.h"
#include "arena_map.h"
#include "arena_pf.h"
#include "arena_attract.h"

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
static uint8_t  s_gi        = ARENA_GI_DEFAULT;   // GI level under ROM attract, 0 = off
static uint8_t  s_density   = 110;   // mode lucioles : combien vivent a la fois
static uint8_t  s_warm      = ARENA_WARM_DEFAULT; // 0 = spectral/orange, 255 = white-forward
static uint64_t s_latched   = 0;                 // lamps held lit from the last game
static bool     s_inc       = true;              // incandescent simulation
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
//  Incandescent filament model
//
//  A bulb switching on does not just get brighter, it changes COLOUR: the
//  filament climbs from dull red through amber to warm white as it heats, and
//  falls back through red as it cools. Fading an LED's brightness keeps its hue
//  fixed at every level, and that is what reads as "digital" rather than as a
//  playfield. With RGBW pixels the real curve is reachable.
//
//  FILAMENT is Planck's law for a black body from 800 K to 2700 K, converted to
//  sRGB and scaled by T^4 — Stefan-Boltzmann, which is why the light collapses
//  so much faster than the temperature does and why the dim red tail lasts so
//  long.
//
//  The RGBW split is deliberately NOT the colorimetric one. Splitting so the
//  white die only carries the neutral part of the spectrum is what a meter would
//  do, and it came out looking orange: a 2700 K source measures orange, but the
//  eye adapts to the dominant illuminant and reads a bulb as warm white. So the
//  white die takes a growing share of the output as the filament heats (none
//  below t=0.35, ~85 % at full), which keeps the physical trajectory — dull red
//  when cold, warm white when hot — and lands where the eye expects it.
//  Regenerate with tools/filament_lut.py if the wall wants a different bulb.
static const uint8_t FILAMENT_HOT[33][4] = {
  {  2,  0,  0,  0}, {  3,  0,  0,  0}, {  3,  0,  0,  0}, {  4,  0,  0,  0},
  {  6,  0,  0,  0}, {  7,  0,  0,  0}, {  9,  1,  0,  0}, { 10,  1,  0,  0},
  { 13,  1,  0,  0}, { 15,  2,  0,  0}, { 18,  2,  0,  0}, { 21,  3,  0,  0},
  { 25,  4,  0,  0}, { 29,  5,  0,  0}, { 34,  6,  0,  0}, { 39,  8,  0,  0},
  { 45,  9,  0,  0}, { 51, 11,  0,  0}, { 59, 14,  0,  0}, { 66, 17,  0,  0},
  { 75, 20,  0,  0}, { 84, 23,  0,  0}, { 94, 28,  0,  0}, {106, 32,  0,  0},
  {118, 37,  0,  0}, {131, 43,  0,  0}, {145, 50,  0,  0}, {160, 57,  0,  0},
  {176, 66,  0,  0}, {194, 75,  0,  0}, {213, 85,  0,  0}, {233, 96,  0,  0},
  {255,108,  0,  0},
};
static const uint8_t FILAMENT_COOL[33][4] = {
  {  2,  0,  0,  0}, {  3,  0,  0,  0}, {  3,  0,  0,  0}, {  4,  0,  0,  0},
  {  6,  0,  0,  0}, {  7,  0,  0,  0}, {  9,  1,  0,  0}, { 10,  1,  0,  0},
  { 13,  1,  0,  0}, { 15,  2,  0,  0}, { 18,  2,  0,  0}, { 21,  3,  0,  0},
  { 23,  3,  0,  2}, { 25,  4,  0,  4}, { 27,  5,  0,  7}, { 29,  6,  0, 10},
  { 31,  6,  0, 14}, { 33,  7,  0, 19}, { 35,  8,  0, 24}, { 36,  9,  0, 30},
  { 37, 10,  0, 38}, { 38, 11,  0, 46}, { 39, 11,  0, 56}, { 38, 12,  0, 67},
  { 38, 12,  0, 80}, { 37, 12,  0, 94}, { 35, 12,  0,110}, { 32, 11,  0,128},
  { 28, 10,  0,149}, { 23,  9,  0,171}, { 17,  7,  0,196}, {  9,  4,  0,224},
  {  0,  0,  0,255},
};
static const uint8_t FILAMENT_N = sizeof(FILAMENT_HOT) / sizeof(FILAMENT_HOT[0]);

// Filament at normalised temperature t (0 = cold, 1 = full), interpolated.
static Rgbw filament(float t) {
  if (t <= 0.0f) return { 0, 0, 0, 0 };
  if (t >= 1.0f) t = 0.999f;
  const float f = t * (FILAMENT_N - 1);
  const uint8_t i = (uint8_t)f;
  const float k = f - i;
  // Two physically derived end points, blended live by the warmth setting:
  // HOT is the pure spectral split (the white die only takes the neutral part —
  // the most orange, and what a colorimeter would ask for), COOL lets the white
  // die carry everything above the knee. Anywhere between is a legitimate bulb;
  // which one looks right on a wall is the owner's call, not a compile-time one.
  const float wq = (float)s_warm / 255.0f;
  const uint8_t* ha = FILAMENT_HOT[i];  const uint8_t* hb = FILAMENT_HOT[i + 1];
  const uint8_t* ca = FILAMENT_COOL[i]; const uint8_t* cb = FILAMENT_COOL[i + 1];
  float o[4];
  for (uint8_t c = 0; c < 4; c++) {
    const float h = ha[c] + (hb[c] - ha[c]) * k;
    const float w = ca[c] + (cb[c] - ca[c]) * k;
    o[c] = h + (w - h) * wq;
  }
  return { clamp8(o[0]), clamp8(o[1]), clamp8(o[2]), clamp8(o[3]) };
}

// Thermal step. Heating is a constant-power exponential approach; cooling is
// radiation (T^4) plus conduction out through the leads (linear), which is what
// gives the long tail instead of a clean exponential. Constants tuned to a #47:
// ~80 % of full light 100 ms after switch-on, down to ~3 % 170 ms after cut.
static inline float filamentStep(float T, bool on, float dt) {
  if (on) return T + (1.0f - T) * (1.0f - expf(-dt / 0.035f));
  const float T2 = T * T;
  T -= dt * (0.70f * T2 * T2 + 0.30f * T) / 0.110f;
  return T > 0.0f ? T : 0.0f;
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

// Arena's ORIGINAL attract mode, replayed from the game ROM (arena_attract.h).
// Each frame is the lamp mask Arena's own program was driving at that instant;
// a pixel lights when the lamp its insert sits on is lit. The wall is running
// the machine's attract sequence, not an impression of it.
//
// One deliberate liberty: #47 bulbs do not switch instantly. They take ~40 ms to
// come up and rather longer to die, and without that the chases read as digital
// blinking instead of a playfield. The envelope below is the only thing here
// that is not in the ROM.
static void fxRomAttract(Rgbw* buf) {
  static float T[LED_MAX] = { 0 };
  static uint32_t lastMs = 0;

  const uint32_t now = millis();
  float dt = (lastMs ? (now - lastMs) : 16) / 1000.0f;
  lastMs = now;
  if (dt > 0.2f) dt = 0.2f;                       // after a stall, do not jump

  // The sequence runs on the animation clock, so the speed slider works on it.
  const uint64_t mask = arenaattract::maskAt((uint32_t)(phase(1.0, 3600.0) * 1000.0f)) | s_latched;

  // General illumination. On a real Arena the GI bulbs stay lit through attract,
  // so an insert the ROM never drives still glows rather than sitting dark — but
  // whether the wall should carry that permanent background is taste, so it is a
  // slider; 0 means truly off. With the filament ON it is a bulb on the same
  // curve. With the filament OFF the whole attract runs on the Colour panel —
  // which is what gives that panel a meaning in this mode at all: reported as
  // "the colour menu does not apply", and with the simulation on, it must not.
  const Rgbw giC = !s_gi ? Rgbw{ 0, 0, 0, 0 }
                 : s_inc ? filament(ARENA_GI_T * (float)s_gi / 255.0f)
                         : scale(s_color, (float)s_gi / 255.0f * 0.25f);

  for (uint16_t i = 0; i < s_count; i++) {
    const int lamp = arenapf::lampOfLed(i);
    const bool on  = (lamp >= 0) && ((mask >> lamp) & 1ULL);

    // With the simulation off this is a plain switch: no thermal lag, no colour
    // ramp. Worth having, because an insert that carries its own colour is
    // usually meant to be seen as that colour rather than through a filament.
    T[i] = s_inc ? filamentStep(T[i], on, dt) : (on ? 1.0f : 0.0f);
    if (lamp < 0) { buf[i] = giC; continue; }

    if (s_inc) {
      // The Colour panel is the bulb's GLASS. Pure white (W only) is clear
      // glass — the true black-body #47, colour ramp and all. Anything else is
      // a tinted bulb: the glass sets the hue, the filament keeps the thermal
      // envelope (rise, fall, ember). Through deep-coloured glass the cold-red
      // ramp is invisible on a real bulb too, so nothing of value is lost.
      const Rgbw f = filament(T[i]);
      if ((s_color.r | s_color.g | s_color.b) == 0) {
        buf[i] = addSat(giC, f);
      } else {
        const float lvl = (float)max(max(f.r, f.g), max(f.b, f.w)) / 255.0f;
        buf[i] = addSat(giC, scale(s_color, lvl));
      }
    } else {
      buf[i] = addSat(giC, scale(s_color, T[i]));
    }
  }
  // Insert colours are applied globally in tick(), not here: the plastic
  // filters EVERY mode's light, not just the ROM attract's.
}

// Attract = the five slow animations above, auto-cycled with a crossfade so the
// wall never "cuts" from one effect to the next. Stands in only when the ROM
// capture or the insert placement is missing.
static void fxAttractGeneric(Rgbw* buf) {
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

// ---------------------------------------------------------------------------
//  Music mode — the wall follows the room.
//
//  Two sources, in priority order: an external push (/api/music — a phone app
//  or a PC script sends energy/bass/treble at ~20 Hz) and, failing that for
//  2 s, an electret mic module (MAX4466/MAX9814) on GPIO34 — ADC1, so it
//  coexists with WiFi. With neither, the mode breathes gently instead of going
//  black, so switching to it without hardware is not read as a crash.
//
//  The mapping is spatial, since we know where every pixel IS: bass breathes
//  the whole field, a detected beat launches a ripple from the playfield
//  centre, treble sparkles random inserts. The power limiter already meters
//  every frame, so a loud passage cannot overrun the supply.
// ---------------------------------------------------------------------------
static float    s_musE = 0, s_musB = 0, s_musT = 0;   // 0..1 envelopes
static uint32_t s_musExtMs  = 0;                      // last external push
static uint32_t s_musBeatMs = 0;                      // last detected beat
static float    s_musAvg    = 0.05f;                  // rolling energy average
static float    s_musPeak   = 0.10f;                  // adaptive normaliser

void musicPush(uint8_t e, uint8_t b, uint8_t t) {
  s_musE = e / 255.0f; s_musB = b / 255.0f; s_musT = t / 255.0f;
  const uint32_t now = millis();
  // Beat detection on the pushed energy, same rule as the mic path.
  if (s_musE > 1.4f * s_musAvg && now - s_musBeatMs > 250) s_musBeatMs = now;
  s_musAvg += 0.05f * (s_musE - s_musAvg);
  s_musExtMs = now;
}

#if ARENA_MIC_ENABLE
static void musicSampleMic() {
  // 160 reads ~ 1.6 ms per frame. Mean-removed RMS = energy; a one-pole
  // low-pass splits a bass proxy from the rest. Crude next to an FFT, and
  // enough: lighting needs an envelope, not a spectrum.
  static float lp = 2048;
  float sumSq = 0, sumLpSq = 0;
  for (int i = 0; i < 160; i++) {
    const float x = (float)analogRead(PIN_ARENA_MIC) - 2048.0f;
    lp += 0.10f * (x - lp);
    sumSq   += x * x;
    sumLpSq += lp * lp;
  }
  const float rms  = sqrtf(sumSq / 160.0f)   / 2048.0f;
  const float bass = sqrtf(sumLpSq / 160.0f) / 2048.0f;
  // Adaptive scale: track the recent peak so quiet rooms still modulate.
  s_musPeak = max(rms, s_musPeak * 0.998f);
  if (s_musPeak < 0.02f) {                    // no mic wired: rail noise only
    s_musE = s_musB = s_musT = 0;
    return;
  }
  s_musE = min(1.0f, rms / s_musPeak);
  s_musB = min(1.0f, bass / s_musPeak);
  s_musT = min(1.0f, max(0.0f, (rms - bass) / s_musPeak));
  const uint32_t now = millis();
  if (s_musE > 1.4f * s_musAvg && now - s_musBeatMs > 250) s_musBeatMs = now;
  s_musAvg += 0.05f * (s_musE - s_musAvg);
}

#endif

static void fxMusic(Rgbw* buf) {
  const uint32_t now = millis();
#if ARENA_MIC_ENABLE
  if (now - s_musExtMs > 2000) musicSampleMic();
#else
  if (now - s_musExtMs > 2000) { s_musE = s_musB = s_musT = 0; }  // idle breathe
#endif

  const bool idle = (s_musE + s_musB + s_musT) < 0.02f;
  if (idle) {                                  // no signal: breathe, don't die
    float k = 0.06f + 0.05f * fsin01(phase(0.5, TAU));
    fill(buf, scale(s_color, k));
    return;
  }

  // Bass breathes the field in the panel colour.
  fill(buf, scale(s_color, 0.04f + 0.55f * s_musB * s_musB));

  // Beat: a ripple leaves the centre of the playfield.
  const float tb = (now - s_musBeatMs) / 1000.0f;
  if (tb < 0.6f) {
    const Rgbw gold = { ARENA_GOLD_R, ARENA_GOLD_G, ARENA_GOLD_B, ARENA_GOLD_W };
    const float rip = tb * 1.9f;
    for (uint16_t i = 0; i < s_count; i++) {
      float x, y;
      if (!arenapf::xy(i, x, y)) continue;
      const float dx = x - 0.5f, dy = y - 0.55f;
      const float dr = fabsf(sqrtf(dx * dx + dy * dy) - rip);
      if (dr < 0.10f)
        buf[i] = addSat(buf[i], scale(gold, (1.0f - dr / 0.10f) * (1.0f - tb / 0.6f)));
    }
  }

  // Treble sparkles random inserts, reusing the spark decay buffer.
  uint8_t decay = 26;
  for (uint16_t i = 0; i < s_count; i++)
    s_spark[i] = (s_spark[i] > decay) ? s_spark[i] - decay : 0;
  if (s_musT > 0.15f) {
    const int n = 1 + (int)(s_musT * 3.0f);
    for (int k = 0; k < n; k++) s_spark[esp_random() % s_count] = 255;
  }
  for (uint16_t i = 0; i < s_count; i++)
    if (s_spark[i]) buf[i] = addSat(buf[i], scale(s_color, s_spark[i] / 255.0f * 0.8f));
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

// Night — lucioles. Quelques points s'allument doucement, brillent, s'eteignent,
// ailleurs, sans jamais tout allumer. C'est le mode qu'on laisse vivre a 23 h.
//
// Ne PAS reutiliser filamentStep() ici, c'est l'erreur du premier essai : cette
// courbe est calibree sur une ampoule #47, 3 % de lumiere 170 ms apres la
// coupure. Une luciole batie dessus meurt en un sixieme de seconde et le mur
// reste noir - mesure du 2026-08-04, 43 mA, la consommation a vide.
// Ici chaque luciole a sa propre duree de vie de 1,5 a 4 s et suit une cloche
// (sinus) : elle monte aussi doucement qu'elle descend. C'est cette symetrie qui
// fait respirer, la ou une decroissance seule fait clignoter.
static void fxNight(Rgbw* buf) {
  static const uint8_t FLY_N = 7;         // au plus 7 vivantes a la fois
  struct Fly { int16_t led; float t, dur, gap; };
  static Fly fly[FLY_N] = { { -1, 0, 0, 0 } };
  static uint32_t lastMs = 0;
  static bool armed = false;

  const uint32_t now = millis();
  float dt = (lastMs ? (now - lastMs) : 16) / 1000.0f;
  lastMs = now;
  if (dt > 0.25f) dt = 0.25f;             // apres un hoquet WiFi, ne pas sauter

  if (!armed) {                           // etaler les premieres apparitions
    armed = true;
    for (uint8_t k = 0; k < FLY_N; k++) { fly[k].led = -1; fly[k].gap = k * 0.55f; }
  }

  fill(buf, { 0, 0, 0, 0 });
  const uint16_t n = s_count ? s_count : 1;
  const float sp = speedFactor();

  // Densite : de une luciole a la fois (calme) aux sept (anime), et les temps
  // morts qui raccourcissent avec. Un seul curseur pour les deux, parce que
  // "plus de monde" et "moins d'attente" sont la meme intention.
  const uint8_t live = (uint8_t)(1 + ((uint16_t)s_density * (FLY_N - 1)) / 255);
  const float   gapK = 2.6f - 2.2f * ((float)s_density / 255.0f);

  for (uint8_t k = 0; k < live; k++) {
    Fly& f = fly[k];
    if (f.led < 0) {                      // en attente : compte a rebours
      f.gap -= dt * sp;
      if (f.gap > 0.0f) continue;
      f.led = (int16_t)random(n);
      f.dur = 1.5f + (float)random(1000) / 1000.0f * 2.5f;
      f.t   = 0.0f;
      continue;
    }
    f.t += dt * sp;
    if (f.t >= f.dur) { f.led = -1; f.gap = (0.25f + (float)random(1000) / 1000.0f * 1.6f) * gapK; continue; }

    // Cloche : 0 -> 1 -> 0, pleine amplitude.
    // Le plafond de 0,6 du premier essai etait une fausse bonne idee : avec la
    // simulation d'ampoule active, 0,6 n'est pas 60 % de lumiere mais une
    // TEMPERATURE de filament, donc une lueur rouge sombre - que le gamma
    // ecrase ensuite au carre. Mesure : 10 sur 255 par luciole, invisible.
    // On va au bout de la courbe et c'est le curseur de luminosite qui dose,
    // comme dans tous les autres modes. Peu de pixels allumes suffit a garder
    // une veilleuse : sept au plus, sur quarante et un.
    const float k01 = sinf(3.14159265f * f.t / f.dur);
    // Simulation d'ampoule active : la courbe du filament, qui traverse le rouge
    // avant d'atteindre le blanc - c'est elle qui donne la lueur d'insecte.
    // Desactivee : la couleur choisie dans le panneau Colour. Ce mode ignorait
    // ce panneau, exactement le reproche deja fait sur l'attract.
    const Rgbw c = s_inc ? filament(k01) : scale(s_color, k01);
    if (f.led < (int16_t)s_count) buf[f.led] = c;
  }
}

static void renderMode(Mode m, Rgbw* buf) {
  switch (m) {
    case MODE_CLASSIC: fxClassic(buf); break;
    case MODE_ATTRACT:
      // The real thing needs both halves: the captured sequence, and pixels
      // placed on inserts to paint it onto. Missing either, fall back.
      if (arenaattract::available() && arenapf::anyAssigned()) fxRomAttract(buf);
      else                                                     fxAttractGeneric(buf);
      break;
    case MODE_ARENA:   fxArena(buf);   break;
    case MODE_NIGHT:   fxNight(buf);   break;
    case MODE_RAINBOW: fxRainbow(buf); break;
    case MODE_MUSIC:   fxMusic(buf);   break;
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
  "off", "classic", "attract", "arena", "night", "rainbow", "music", "test"
};

const char* modeName(Mode m) { return (m < MODE_COUNT) ? MODE_NAMES[m] : "?"; }

// Ce que le PROPRIETAIRE lit, distinct de ce que la machine ecrit.
// MODE_NAMES ci-dessus est un identifiant : il part en NVS (putString "modeN"),
// il est la clef de /api/set?mode=..., et modeFromName() le relit. Le renommer
// casserait les reglages de toutes les cartes deja configurees et toutes les URL
// documentees. Les libelles vivent donc a part et peuvent changer librement.
// "arena" -> "Wave" : le nom interne venait du plateau, il ne disait rien a
// l'acheteur; ce qu'il voit, c'est une vague qui parcourt le plateau.
static const char* MODE_LABELS[MODE_COUNT] = {
  "Off", "All on", "Attract", "Wave", "Firefly", "Rainbow", "Music", "Test"
};
const char* modeLabel(Mode m) { return (m < MODE_COUNT) ? MODE_LABELS[m] : "?"; }

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
void setGi(uint8_t g) { s_gi = g; markDirty(); }
void setDensity(uint8_t d) { s_density = d; markDirty(); }
uint8_t density() { return s_density; }
uint8_t gi() { return s_gi; }
void setWarm(uint8_t w) { s_warm = w; markDirty(); }
uint8_t warm() { return s_warm; }
void setLatched(uint64_t m) { s_latched = m; markDirty(); }
uint64_t latched() { return s_latched; }
void setIncandescent(bool on) { s_inc = on; markDirty(); }
bool incandescent() { return s_inc; }
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
  // Compare-and-clear: a setter on the HTTP task can re-arm s_dirtyAt while the
  // multi-millisecond NVS write below is in flight; clearing unconditionally at
  // the end would discard that change until the next unrelated save.
  const uint32_t dirtyAtEntry = s_dirtyAt;
  // By NAME, not by number: inserting MODE_MUSIC mid-enum silently remapped
  // every board whose NVS held mode=6 (TEST before, MUSIC after). Names survive
  // enum surgery; the numeric key is still read once for migration.
  s_prefs.putString("modeN", modeName(s_mode));
  s_prefs.putUChar("bright", s_bright);
  s_prefs.putUChar("speed",  s_speed);
  s_prefs.putUChar("gi",     s_gi);
  s_prefs.putUChar("dens",   s_density);
  s_prefs.putUChar("warm",   s_warm);
  s_prefs.putBytes("latched", &s_latched, sizeof(s_latched));
  s_prefs.putUChar("inc", s_inc ? 1 : 0);
  s_prefs.putUShort("count", s_count);
  s_prefs.putUShort("budget", s_budget);
  s_prefs.putBytes("color", &s_color, sizeof(s_color));
  s_prefs.putString("order", s_order);
  if (s_dirtyAt == dirtyAtEntry) s_dirtyAt = 0;
}

void begin() {
  s_prefs.begin("arena", false);
  {
    String mn = s_prefs.getString("modeN", "");
    Mode m = mn.length() ? modeFromName(mn.c_str()) : MODE_COUNT;
    if (m == MODE_COUNT) m = (Mode)s_prefs.getUChar("mode", (uint8_t)MODE_CLASSIC);
    s_mode = (m < MODE_COUNT) ? m : MODE_CLASSIC;
  }
  if (s_mode >= MODE_COUNT) s_mode = MODE_CLASSIC;
  s_bright = s_prefs.getUChar("bright", ARENA_BRIGHT_DEFAULT);
  s_speed  = s_prefs.getUChar("speed",  ARENA_SPEED_DEFAULT);
  s_gi     = s_prefs.getUChar("gi",     ARENA_GI_DEFAULT);
  s_density = s_prefs.getUChar("dens", 110);
  s_warm   = s_prefs.getUChar("warm",   ARENA_WARM_DEFAULT);
  if (s_prefs.getBytesLength("latched") == sizeof(s_latched))
    s_prefs.getBytes("latched", &s_latched, sizeof(s_latched));
  s_inc = s_prefs.getUChar("inc", 1) != 0;
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

  // Insert plastic colours. On a real playfield the colour IS the moulded
  // plastic over a warm bulb, so it filters whatever light the mode produces —
  // classic, attract, rainbow, all of them. The pixel's rendered level becomes
  // the bulb intensity, the plastic sets the hue. Reported as "the colour menu
  // does not work": it did, but only inside the ROM attract, which reads as
  // broken everywhere else.
  for (uint16_t i = 0; i < s_count; i++) {
    const arenapf::Colour pc = arenapf::colourOfLed(i);
    if (!(pc.r | pc.g | pc.b | pc.w)) continue;
    const Rgbw& c = s_frame[i];
    const float lvl = (float)max(max(c.r, c.g), max(c.b, c.w)) / 255.0f;
    s_frame[i] = scale(Rgbw{ pc.r, pc.g, pc.b, pc.w }, lvl);
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

  // Plus de plafond special pour la nuit : fxNight limite deja son amplitude a
  // 0,6 et n'allume que quelques pixels. Le plafond cache rendait le curseur de
  // luminosite sans effet dans ce mode, ce qui se lit comme un reglage casse.
  uint8_t gain = s_bright;
#if ARENA_SOFTSTART_MS > 0
  // Soft start: ease the chain up over the first second so 100+ pixels lighting
  // at once can't trip the PSU's over-current hiccup or slam the bulk caps.
  // Latched once done: deriving "still ramping?" from millis() forever re-fires
  // the ramp at every 49.7-day u32 wrap - the wall would blink to black for a
  // second every seven weeks, reading as a mains glitch.
  static bool rampDone = false;
  if (!rampDone) {
    if (!s_bootMs) s_bootMs = now ? now : 1;   // first frame: arm the ramp here
    uint32_t sinceBoot = now - s_bootMs;
    if (sinceBoot < ARENA_SOFTSTART_MS)
      gain = (uint8_t)((uint32_t)gain * sinceBoot / ARENA_SOFTSTART_MS);
    else
      rampDone = true;
  }
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
