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
static Rgbw     s_giColor   = { 0, 0, 0, 0 };     // GI tint; all-zero = follow the Colour panel
static uint8_t  s_insBright = 255;                // inserts, separately from the GI behind them
static uint8_t  s_champ     = 255;                // "champignons": a second permanent layer
static bool     s_lvlLock   = false;              // move one level, the other keeps the ratio
// Couple de REFERENCE, fige au moment ou l'on verrouille.
//
// Le suiveur etait calcule a partir de la valeur COURANTE de l'autre etage, puis
// ecrete a 255. Un aller-retour detruisait donc l'equilibre sans retour possible :
// avec fond=90 et champignons=255, monter le fond a 255 demandait 722 pour les
// champignons -> ecrete a 255, sans rien de visible ; redescendre le fond a 90
// recalculait alors 255*90/255 = 90 au lieu de 255. L'interface promet "le
// rapport que tu as regle est conserve" ; il ne l'etait pas.
//
// En gardant le couple de depart, le rapport ne derive plus : l'ecretage
// n'affecte que la valeur emise, jamais la reference. L'aller-retour redonne
// exactement la valeur de depart.
static uint8_t  s_lockGi    = 0;
static uint8_t  s_lockChamp = 0;
static uint8_t  s_density   = 110;   // mode lucioles : combien vivent a la fois
// Ce que la carte fait quand le courant revient. Une piece murale qui reste
// noire apres une coupure passe pour cassee : le proprietaire ne va pas
// rebrancher un telephone pour la rallumer. On garde donc le dernier mode
// ALLUME a part, et par defaut on y revient au demarrage plutot que de
// restaurer un "off" qui pouvait dater d'un ordre Siri d'il y a trois jours.
static Mode     s_lastOn    = MODE_ATTRACT;       // dernier mode non eteint
static bool     s_bootOn    = true;               // true = rallumer, false = restaurer tel quel
static uint8_t  s_warm      = ARENA_WARM_DEFAULT; // 0 = spectral/orange, 255 = white-forward
static uint64_t s_latched   = 0;                 // lamps held lit from the last game
static bool     s_inc       = true;              // incandescent simulation
static volatile bool s_paused   = false;         // BLE pairing in progress
static uint32_t      s_pausedAt = 0;             // quand, pour le degel de securite
static uint8_t  s_speed     = ARENA_SPEED_DEFAULT;
static Rgbw     s_color     = { ARENA_WARM_R, ARENA_WARM_G, ARENA_WARM_B, ARENA_WARM_W };
static uint16_t s_count     = LED_COUNT_DEFAULT;
static uint16_t s_budget    = LED_POWER_BUDGET_MA;
// Diviseur d'alimentation partagee. Voir arenaled.h : ce n'est pas un reglage
// de gout, c'est ce qui empeche N murs sur un seul chargeur de lui reclamer N
// fois son courant et de le faire replier.
static uint8_t  s_share     = 1;
static char     s_order[8]  = ARENA_ORDER_DEFAULT;
static uint32_t s_bootMs    = 0;       // soft-start reference

// The web handlers run in the AsyncTCP task, the renderer in loop(). Anything
// that reallocates or re-initialises the strip is therefore NOT done inline from
// a request — it is flagged here and applied at the top of the next tick(), on
// the render task, where nothing else is touching the pixel buffer.
static volatile bool s_pendLen = false;
static uint8_t       s_pin     = PIN_LED_DATA;   // broche data, reglable a chaud
// Broche a laquelle la chaine est REELLEMENT accrochee. s_pin est le souhait,
// celle-ci est le fait ; tant qu'elles sont egales il n'y a rien a refaire, et
// surtout rien a toucher (voir applyPending).
static uint8_t       s_pinLive = PIN_LED_DATA;

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
// Cadence de rafraichissement, reglable a chaud. Elle etait figee a la
// compilation, ce qui en faisait un parametre inaccessible la ou il sert le
// plus : sur une chaine limite. Une cadence basse laisse plus de temps mort
// entre deux trames - donc plus de marge au signal et a l'alimentation - et
// c'est aussi le test qui separe les deux : un defaut de signal se calme
// quand on ralentit, un affaissement de tension non, puisque le courant moyen
// ne bouge pas.
static uint8_t  s_frameHz   = LED_FRAME_HZ;
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
// Composition des etages permanents (fond, champignons) avec la lumiere de la ROM.
//
// Trois formes essayees sur la vraie carte, dans cet ordre :
//
//  1. addSat sur TOUS les pixels : monter le fond eclaircissait aussi les inserts
//     allumes, le contraste s'effondrait et la ROM se noyait dans le halo.
//     "Quand je touche a background ca detruit toutes mes autres lampes."
//  2. maximum par canal : corrige le point 1, mais des que la lueur permanente
//     depasse la lampe de la ROM elle l'avale et le pixel cesse de clignoter.
//     "Les champignons s'allument mais ne clignotent plus."
//  3. ce qui est en place : le plancher prend le bas de la dynamique et la
//     lampe est comprimee dans la place qui RESTE au-dessus. Le clignotement
//     survit a n'importe quel niveau de plancher, et monter la lueur reduit le
//     contraste progressivement au lieu de le supprimer d'un coup.
//
// Voir le calcul de `head` dans fxRomAttract.
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
    // Par APPARTENANCE, pas par tranche : un groupe disperse - des pop bumpers,
    // l'eclairage general - n'occupe plus first..first+count.
    for (uint16_t n = 0; n < zn->count; n++) {
      const int i = arenamap::zoneNth(z, n);
      if (i < 0 || i >= (int)s_count) continue;
      buf[i] = scale(s_color, 0.10f + 0.90f * k);
    }
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

  // L'inertie du filament suit la MEME horloge que la sequence.
  //
  // Elle tournait en temps reel pendant que la sequence, elle, etait acceleree
  // par le curseur de vitesse. A 2x, un eclair de lampe dure deux fois moins
  // longtemps alors que l'ampoule met toujours ~100 ms a monter et ~170 ms a
  // s'eteindre : les flashs n'atteignent plus le plein et ne redescendent plus,
  // tout se fond en une lueur molle. Rapporte comme "l'attract est plus rapide
  // sur le vrai flipper que sur notre simulation" - alors que la mesure disait
  // l'inverse (curseur a 194, soit 2,04x). Ce n'etait pas la vitesse de la
  // sequence, c'etait le flou. Une ampoule vue en accelere doit s'allumer en
  // accelere, sinon accelerer RALENTIT l'image.
  dt *= speedFactor();

  // The sequence runs on the animation clock, so the speed slider works on it.
  const uint64_t mask = arenaattract::maskAt((uint32_t)(phase(1.0, 3600.0) * 1000.0f)) | s_latched;

  // General illumination. On a real Arena the GI bulbs stay lit through attract,
  // so an insert the ROM never drives still glows rather than sitting dark — but
  // whether the wall should carry that permanent background is taste, so it is a
  // slider; 0 means truly off. With the filament ON it is a bulb on the same
  // curve. With the filament OFF the whole attract runs on the Colour panel —
  // which is what gives that panel a meaning in this mode at all: reported as
  // "the colour menu does not apply", and with the simulation on, it must not.
  //
  // MEASURED, not assumed. Averaging 18 samples per setting to get out from
  // under the animation's own current swing (sigma ~40 mA), the previous form
  // fed the setting in as a TEMPERATURE - filament(ARENA_GI_T * gi/255). A hot
  // body's emission is a power law, so from gi=0 to gi=192 - three quarters of
  // the travel - the whole change stayed inside the noise (+9, +7, +13 mA) and
  // everything happened between 192 and 255 (+64 mA). The slider commanded
  // nothing usable. The 0.25 ceiling on the non-filament branch did the same
  // thing by a different route.
  //
  // Now temperature carries only the TINT - it still reddens as you come down,
  // the way a dimmed bulb does - and the setting carries the INTENSITY, which
  // is what a level control is supposed to mean. Renormalising the filament
  // colour to its own peak before rescaling is what separates the two.
  // Un etage permanent du mur, a partir de son seul niveau. Le fond et les
  // champignons sont de meme nature - meme teinte, meme loi - et ne different
  // que par ce niveau, ce qui est exactement ce qui les rend independants :
  // descendre le fond a zero n'eteint pas les champignons.
  auto glow = [&](uint8_t lvl) -> Rgbw {
    if (!lvl) return Rgbw{ 0, 0, 0, 0 };
    const float k = (float)lvl / 255.0f;
    // A tint of its own: the background stops following the Colour panel, so
    // an amber wash can sit under white inserts.
    if (s_giColor.r | s_giColor.g | s_giColor.b | s_giColor.w) return scale(s_giColor, k);
    if (!s_inc) return scale(s_color, k);
    const Rgbw f = filament(ARENA_GI_T * (0.70f + 0.30f * k));
    const float pk = (float)max(max(f.r, f.g), max(f.b, f.w)) / 255.0f;
    return (pk > 0.004f) ? scale(f, k / pk) : Rgbw{ 0, 0, 0, 0 };
  };
  const Rgbw giC = glow(s_gi);       // le fond
  // ⚠️ Le plancher des champignons est PLAFONNE, et ce n'est pas un detail de
  // reglage : c'est ce qui garantit qu'ils clignotent encore.
  //
  // Comprimer la lampe de la ROM dans la place restant au-dessus du plancher ne
  // suffit pas si le plancher peut monter jusqu'au plein : a champignons = 255 il
  // ne reste rien au-dessus, et le clignotement disparait exactement la ou le
  // proprietaire pousse le curseur. Signale deux fois - "les champignons
  // s'allument mais ne clignotent plus quand tu montes la puissance", puis
  // "champignons tu as toujours pas regle le probleme".
  //
  // Un champignon qui brille en permanence a fond n'a plus rien pour clignoter :
  // le curseur promettrait quelque chose d'impossible. On lui reserve donc un
  // quart de la dynamique, toujours. Le curseur va de 0 a 75 % de plancher, et
  // les 25 % du haut appartiennent a la ROM.
  const Rgbw chC = scale(glow(s_champ), ARENA_CHAMP_FLOOR_MAX);
  const float chK = (float)max(max(chC.r, chC.g), max(chC.b, chC.w)) / 255.0f;

  // Le groupe est resolu une fois par trame, pas par pixel : c'est un groupe
  // arenamap ordinaire, donc il peut etre renomme, vide, ou absent.
  const int champZ = arenamap::indexOf(ARENA_CHAMP_ZONE);

  // Les inserts et le fond sont deux etages separes. Le mur porte les deux en
  // meme temps - ce que la ROM allume, et l'eclairage permanent derriere - et
  // l'equilibre entre les deux est un choix de gout qui ne devrait pas dependre
  // de la luminosite generale. Elle reste maitresse par-dessus les deux.
  const float ib = (float)s_insBright / 255.0f;

  for (uint16_t i = 0; i < s_count; i++) {
    const int lamp = arenapf::lampOfLed(i);
    const bool on  = (lamp >= 0) && ((mask >> lamp) & 1ULL);
    const bool isChamp = (champZ >= 0 && arenamap::zoneOfLed(i) == champZ);

    // "Background" ne porte que le FOND : les pixels qui ne sont sur aucun
    // insert. Il s'appliquait a tout le monde, insert compris, donc le monter
    // rallumait les 52 lampes que la ROM laisse eteintes - rapporte tel quel :
    // "background fait background sur toutes les LED". Un insert eteint par la
    // ROM doit rester eteint, sinon le mur cesse de montrer ce que la machine
    // fait. Les champignons, eux, gardent leur plancher meme s'ils portent un
    // insert : c'est un groupe pose a la main, il commande ce qu'on lui a dit
    // de commander.
    const Rgbw base  = isChamp ? chC : giC;
    const Rgbw floorC = isChamp ? chC : Rgbw{ 0, 0, 0, 0 };
    // Le plancher prend le BAS de la dynamique, la ROM garde la tete.
    //
    // Un simple maximum ne marche pas : des que la lueur permanente depasse la
    // lampe de la ROM, elle l'avale et le pixel cesse de clignoter. Rapporte
    // tel quel - "les champignons s'allument mais ne clignotent plus quand tu
    // montes la puissance". Le clignotement est justement ce qui fait qu'un
    // champignon est un champignon et pas une veilleuse.
    //
    // En comprimant la lampe dans la place qui RESTE au-dessus du plancher, le
    // clignotement survit a n'importe quel niveau tant que le plancher n'est pas
    // au maximum : monter la lueur reduit le contraste progressivement au lieu
    // de le supprimer d'un coup. Vaut 1 pour les pixels sans plancher, donc rien
    // ne change pour eux.
    const float head = isChamp ? (1.0f - chK) : 1.0f;

    // With the simulation off this is a plain switch: no thermal lag, no colour
    // ramp. Worth having, because an insert that carries its own colour is
    // usually meant to be seen as that colour rather than through a filament.
    T[i] = s_inc ? filamentStep(T[i], on, dt) : (on ? 1.0f : 0.0f);
    if (lamp < 0) { buf[i] = base; continue; }

    if (s_inc) {
      // The Colour panel is the bulb's GLASS. Pure white (W only) is clear
      // glass — the true black-body #47, colour ramp and all. Anything else is
      // a tinted bulb: the glass sets the hue, the filament keeps the thermal
      // envelope (rise, fall, ember). Through deep-coloured glass the cold-red
      // ramp is invisible on a real bulb too, so nothing of value is lost.
      const Rgbw f = filament(T[i]);
      if ((s_color.r | s_color.g | s_color.b) == 0) {
        buf[i] = addSat(floorC, scale(f, ib * head));
      } else {
        const float lvl = (float)max(max(f.r, f.g), max(f.b, f.w)) / 255.0f;
        buf[i] = addSat(floorC, scale(s_color, lvl * ib * head));
      }
    } else {
      buf[i] = addSat(floorC, scale(s_color, T[i] * ib * head));
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
// L'ADC ne se configurait nulle part. Sans ces deux lignes on depend du defaut
// du coeur Arduino, qui a change entre versions - et une attenuation trop faible
// ecrete tout signal depassant ~1 V, ce qui se voit comme un micro sourd puis
// sature d'un coup.
static void micBegin() {
  analogReadResolution(12);                      // 0..4095, donc mi-echelle a 2048
  analogSetPinAttenuation(PIN_ARENA_MIC, ADC_11db);   // pleine echelle ~3,1 V
  Serial.printf("[led] micro sur GPIO%d (ADC1)\n", PIN_ARENA_MIC);
}

static void musicSampleMic() {
  // 160 reads ~ 1.6 ms per frame. Mean-removed RMS = energy; a one-pole
  // low-pass splits a bass proxy from the rest. Crude next to an FFT, and
  // enough: lighting needs an envelope, not a spectrum.
  // La polarisation est MESUREE, pas supposee. Le code retranchait 2048, c'est
  // a dire la moitie de l'echelle - ce qui imposait un etage de sortie polarise
  // a 1,55 V. Or les front-ends courants ne le sont pas : un MAX9814 sort a
  // 1,25 V, un ampli sur diviseur a VDD/2. Une constante ici, c'est le choix du
  // micro decide par le firmware, et un decalage permanent lu comme du signal.
  //
  // Un suivi tres lent isole donc le continu. Il doit rester bien plus lent que
  // la plus basse frequence utile, sinon il suit la musique et l'efface.
  static float dc = 2048.0f;
  static float lp = 0;
  float sumSq = 0, sumLpSq = 0;
  for (int i = 0; i < 160; i++) {
    const float raw = (float)analogRead(PIN_ARENA_MIC);
    dc += 0.0005f * (raw - dc);
    const float x = raw - dc;
    lp += 0.10f * (x - lp);
    sumSq   += x * x;
    sumLpSq += lp * lp;
  }
  // Normalise sur la dynamique reellement disponible : au-dessus de la
  // polarisation il ne reste que (4095 - dc) points, pas 2048.
  const float span = max(256.0f, min(dc, 4095.0f - dc));
  const float rms  = sqrtf(sumSq / 160.0f)   / span;
  const float bass = sqrtf(sumLpSq / 160.0f) / span;
  // Reglage automatique du gain : le mur doit reagir pareil que la musique soit
  // forte ou faible. On suit la crete recente et on normalise dessus.
  //
  // La montee est INSTANTANEE (on ne rate pas un morceau qui demarre fort) et la
  // descente lente. Elle etait a 0,998 par TRAME, donc dependante de la cadence
  // de rendu : a 60 Hz la crete mettait ~6 s a diminuer de moitie, a 30 Hz le
  // double. Formule en secondes, la cadence n'entre plus en compte, et 4 s de
  // demi-vie laissent le temps de baisser le volume sans que le mur se rallume
  // brutalement.
  s_musPeak = max(rms, s_musPeak * expf(-0.1733f * ARENA_MIC_DT));
  if (s_musPeak < 0.012f) {                   // aucun micro cable : bruit d'alim
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

// --- Hauteur de chaque pixel, y compris ceux qu'aucun insert ne place --------
//
// arenapf::xy() ne sait situer qu'un pixel pose sur un insert. Sur ce mur, 23
// des 75 n'en portent aucun : les laisser de cote, comme le fait fxArenaGeo,
// ferait 23 LED immobiles au milieu d'un mouvement. Mais un ruban est continu -
// un pixel sans insert est physiquement ENTRE ses voisins qui en ont un - donc
// sa hauteur s'interpole le long de la chaine. Ce n'est pas une supposition sur
// le cablage : c'est la seule propriete qu'un ruban garantisse, l'ordre.
//
// Recalcule toutes les 2 s plutot que branche sur une invalidation : la carte a
// 75 pixels, le calcul est negligeable, et une invalidation oubliee donnerait un
// mouvement faux que rien ne signalerait. h = 0 en bas (flippers), 1 en haut.
static float s_ledH[LED_MAX];
static uint32_t s_ledHMs = 0;

static void refreshLedHeights() {
  const uint32_t now = millis();
  if (s_ledHMs && (now - s_ledHMs) < 2000) return;
  s_ledHMs = now ? now : 1;

  float x, y;
  int   lo = -1;                       // dernier pixel place rencontre
  float loH = 0.5f;
  for (uint16_t i = 0; i < s_count; i++) {
    if (arenapf::xy(i, x, y)) {
      const float h = 1.0f - y;        // y=1 aux flippers -> h=0 en bas
      s_ledH[i] = h;
      for (int k = lo + 1; k < (int)i; k++) {
        const float f = (float)(k - lo) / (float)(i - lo);
        s_ledH[k] = loH + f * (h - loH);
      }
      lo = i; loH = h;
    }
  }
  // Queue de chaine sans aucun insert : on prolonge la derniere hauteur connue
  // plutot que d'inventer une pente.
  for (int k = lo + 1; k < (int)s_count; k++) s_ledH[k] = loH;
  if (lo < 0) for (uint16_t i = 0; i < s_count; i++)   // aucun pixel place du tout
    s_ledH[i] = s_count > 1 ? 1.0f - (float)i / (float)(s_count - 1) : 0.5f;
}

static void fxMusic(Rgbw* buf) {
  const uint32_t now = millis();
  // Pas de temps reel : les constantes d'attaque et de retombee ci-dessous sont
  // en secondes, elles ne doivent pas dependre de la cadence de rendu (reglable
  // de 1 a 120 Hz).
  static uint32_t musLast = 0;
  float dt = (musLast ? (now - musLast) : 16) / 1000.0f;
  musLast = now;
  if (dt > 0.2f) dt = 0.2f;
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

  // Le mode monte des flippers vers le fronton. Il remplissait le champ
  // uniformement et faisait partir le battement du CENTRE, donc rien n'avait de
  // direction : demande explicite du proprietaire d'en faire un mouvement de bas
  // en haut. Les trois etages vont maintenant dans le meme sens.
  refreshLedHeights();

  // Un fond faible partout : sans lui, un passage calme eteint le mur au lieu de
  // le faire respirer. Il suit le curseur Background - a zero il disparait, pour
  // qui veut un noir franc entre deux notes.
  fill(buf, scale(s_color, 0.10f * (float)s_gi / 255.0f));

  // Graves : une colonne qui monte du bas, avec ATTAQUE RAPIDE ET RETOMBEE LENTE.
  //
  // Elle suivait l'enveloppe brute. Mesure au micro sur de la vraie musique :
  // les graves oscillent entre 0,23 et 0,86 avec un ecart-type de 0,149, donc
  // la colonne sautait sur plus de 65 % de la hauteur du mur d'un instant a
  // l'autre. Elle ne montait pas, elle sautillait - "ca clignote a moitie".
  //
  // Un vumetre ne suit pas le signal, il le SUIT VITE EN MONTANT et retombe
  // lentement : c'est ce qui donne la frappe sans la nervosite. 25 ms a la
  // montee (on ne rate pas un coup de grosse caisse), 320 ms a la descente.
  static float lvl = 0;
  {
    const float target = 0.08f + 0.92f * s_musB * s_musB;
    const float tau = (target > lvl) ? 0.025f : 0.320f;
    lvl += (target - lvl) * (1.0f - expf(-dt / tau));
  }

  // Temoin de crete : il monte avec la colonne et redescend seul, lentement.
  // C'est ce qui fait lire le mouvement comme voulu plutot que comme un defaut,
  // et ca donne un repere fixe quand la musique est dense.
  static float pk = 0;
  pk -= dt * 0.30f;
  if (lvl > pk) pk = lvl;
  if (pk < 0) pk = 0;

  for (uint16_t i = 0; i < s_count; i++) {
    const float h = s_ledH[i];
    float k = (lvl - h) / 0.12f;
    if (k > 1.0f) k = 1.0f;
    if (k > 0.0f) buf[i] = addSat(buf[i], scale(s_color, k * 0.55f));
    // La crete, en trait fin au-dessus de la colonne.
    const float dpk = fabsf(h - pk);
    if (dpk < 0.035f)
      buf[i] = addSat(buf[i], scale(s_color, (1.0f - dpk / 0.035f) * 0.85f));
  }

  // Battement : une bande qui quitte les flippers et monte. Elle depasse 1.0
  // pour sortir par le haut au lieu de s'eteindre sur place.
  const float tb = (now - s_musBeatMs) / 1000.0f;
  if (tb < 0.6f) {
    const Rgbw gold = { ARENA_GOLD_R, ARENA_GOLD_G, ARENA_GOLD_B, ARENA_GOLD_W };
    const float front = tb * 2.1f;              // 0 -> 1.26 en 0,6 s
    for (uint16_t i = 0; i < s_count; i++) {
      const float d = fabsf(s_ledH[i] - front);
      if (d < 0.14f)
        buf[i] = addSat(buf[i], scale(gold, (1.0f - d / 0.14f) * (1.0f - tb / 0.6f)));
    }
  }

  // Aigus : des etincelles, mais AU-DESSUS de la colonne des graves. Tirees au
  // hasard sur toute la chaine elles annulaient la direction; cantonnees au
  // sommet, elles se lisent comme l'ecume poussee par la colonne. On tire dans
  // la zone libre, avec une garde : si les graves saturent, il n'y a plus de
  // zone libre et on ne veut pas boucler indefiniment.
  // Le seuil des aigus etait FIXE a 0,15. Mesure sur cette piece : les aigus
  // plafonnent a 0,24 et tournent autour de 0,16 - le seuil tombait donc au
  // milieu du signal, et les etincelles apparaissaient une fois sur deux au
  // hasard. On suit la crete recente : la piste peut etre sourde ou brillante,
  // le mur reagit pareil.
  static float tpk = 0.05f;
  tpk = max(s_musT, tpk * 0.995f);
  const float trel = (tpk > 0.01f) ? (s_musT / tpk) : 0.0f;

  const uint8_t decay = 22;
  for (uint16_t i = 0; i < s_count; i++)
    s_spark[i] = (s_spark[i] > decay) ? s_spark[i] - decay : 0;
  if (trel > 0.55f) {
    const int n = 1 + (int)((trel - 0.55f) * 6.0f);
    for (int k = 0; k < n; k++) {
      for (int tries = 0; tries < 8; tries++) {
        const uint16_t c = esp_random() % s_count;
        if (s_ledH[c] > lvl) { s_spark[c] = 255; break; }
      }
    }
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
  // Les vagues suivent le panneau Couleur.
  //
  // Elles etaient codees en dur en ambre et en or : seul le fond de 14 % suivait
  // s_color, donc tourner les curseurs ne changeait rien de ce qu'on regarde et
  // le mode passait pour casse - "wave mode colors don't work". Meme regle que
  // sous l'attract de la ROM : blanc pur (r=g=b=0) = la teinte d'origine, une
  // couleur = c'est elle qui passe. Rien ne change pour qui n'a jamais touche au
  // panneau.
  const bool tinted = (s_color.r | s_color.g | s_color.b) != 0;
  const Rgbw amber = tinted ? s_color : Rgbw{ ARENA_AMBER_R, ARENA_AMBER_G, ARENA_AMBER_B, ARENA_AMBER_W };
  const Rgbw gold  = tinted ? s_color : Rgbw{ ARENA_GOLD_R,  ARENA_GOLD_G,  ARENA_GOLD_B,  ARENA_GOLD_W  };

  // Le fond permanent devient reglable au lieu d'etre impose.
  //
  // Il valait 14 % en dur : le mur ne s'eteignait jamais completement dans ce
  // mode, sans qu'aucun reglage ne permette d'y toucher - "always a bit of light
  // in background". Il suit maintenant le curseur Background, qui porte deja ce
  // role sous l'attract. A zero, le fond est vraiment noir.
  fill(buf, scale(s_color, 0.20f * (float)s_gi / 255.0f));

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
      for (uint16_t n = 0; n < zn->count; n++) {
        const int i = arenamap::zoneNth(z, n);
        if (i < 0 || i >= (int)s_count) continue;
        // Le rang DANS le groupe, plus l'ecart a son premier membre : sur un
        // groupe disperse, l'ecart d'indice ne dit plus rien de la position.
        float ph = 1.0f - fabsf((float)n / (float)max<uint16_t>(1, zn->count) - 0.5f);
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
// ...mais AVOIR un repeteur est un fait de CABLAGE, pas de firmware. Le banc
// Arena en porte un ; une carte controleur toute faite n'en a pas. En constante
// de compilation, une installation sur deux est fausse - et quand elle l'est
// dans ce sens, le firmware tient la premiere VRAIE LED eteinte et decale tout
// le mapping d'un cran. Cela se presente comme "les LED ne s'allument pas", et
// il faut reflasher pour en sortir. Reglable a chaud, garde en NVS.
static bool s_repeater = (LED_REPEATER_PIXEL != 0);
static inline uint16_t offs() { return s_repeater ? 1 : 0; }
#define OFFS (offs())

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
  // Les couleurs et le blanc ne consomment PAS pareil : 9 mA par canal R/G/B
  // contre 18 mA pour le W, d'apres le datasheet SK6812MINI. Les compter d'un
  // seul coefficient surestime les couleurs du double et fausse le pire cas.
  uint32_t sumRGB = 0, sumW = 0;
  for (uint16_t i = 0; i < s_count; i++) {
    sumRGB += buf[i].r + buf[i].g + buf[i].b;
    sumW   += buf[i].w;
  }
  const float maLed = (float)sumRGB / 255.0f * LED_MA_RGB
                    + (float)sumW   / 255.0f * LED_MA_W;
  float ma = maLed + quiescent;

  s_limited = false;
  const float cap = (float)s_budget / (float)(s_share ? s_share : 1);
  if (ma > cap && maLed > 0.0f) {
    float avail = cap - quiescent;
    if (avail < 0) avail = 0;
    // Le facteur s'applique uniformement aux quatre canaux, donc la
    // ponderation se conserve : il suffit de le calculer sur le total.
    const float k = avail / maLed;
    for (uint16_t i = 0; i < s_count; i++) buf[i] = scale(buf[i], k);
    ma = cap;
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

// Ce que le PROPRIETAIRE lit, distinct de ce que la machine ecrit. MODE_NAMES
// est un identifiant : il part en NVS (putString "modeN"), il est la clef de
// /api/set?mode=... et modeFromName() le relit. Le renommer casserait les
// reglages de toutes les cartes deja configurees. Les libelles vivent a part.
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
  if (m != MODE_OFF) s_lastOn = m;
  markDirty();
}

Mode mode() { return s_mode; }
bool bootOn()          { return s_bootOn; }
void setBootOn(bool b) { s_bootOn = b; markDirty(); }

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
void setPaused(bool p) { s_paused = p; s_pausedAt = p ? millis() : 0; }
bool paused()           { return s_paused; }
void setIncandescent(bool on) { s_inc = on; markDirty(); }
bool incandescent() { return s_inc; }
void setSpeed(uint8_t s) { s_speed = s; markDirty(); }
uint8_t speed() { return s_speed; }
void setColor(Rgbw c) { s_color = c; markDirty(); }
Rgbw color() { return s_color; }
void setGiColor(Rgbw c) { s_giColor = c; markDirty(); }
Rgbw giColor() { return s_giColor; }
void setInsBright(uint8_t b) { s_insBright = b; markDirty(); }
void setChamp(uint8_t b) { s_champ = b; markDirty(); }
uint8_t champ() { return s_champ; }
uint8_t insBright() { return s_insBright; }
void setLevelLock(bool on) {
  if (on && !s_lvlLock) { s_lockGi = s_gi; s_lockChamp = s_champ; }
  s_lvlLock = on;
  markDirty();
}
// Refixer l'equilibre : appele quand les deux etages sont poses dans la meme
// requete, ce qui est un reglage explicite et non un suivi.
void setLevelRef(uint8_t g, uint8_t c) { s_lockGi = g; s_lockChamp = c; }
uint8_t levelRefGi()    { return s_lockGi; }
uint8_t levelRefChamp() { return s_lockChamp; }
bool levelLock() { return s_lvlLock; }
uint16_t count() { return s_count; }
uint16_t budgetMa() { return s_budget; }
void     setBudgetShare(uint8_t n) { s_share = n ? n : 1; }
uint8_t  budgetShare()             { return s_share; }

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
  // setPin() n'est PAS anodin ici : quand la chaine est deja demarree, il fait
  // pinMode(nouvelle, OUTPUT) + digitalWrite(LOW) - exactement l'acte que le
  // commentaire ci-dessous decrit comme arrachant la broche au canal RMT. Le
  // banc a rendu le verdict : appele a chaque changement de longueur, meme pour
  // la MEME broche, il noyait le port serie de "rmt_write_items : RMT DRIVER
  // ERR" a chaque trame, et pas une LED ne s'allumait. On ne le touche donc que
  // si la broche change vraiment, et on re-demarre la chaine derriere.
  if (s_pin != s_pinLive) {
    s_strip.setPin(s_pin);
    s_strip.begin();
    s_pinLive = s_pin;
  }
  // Blank at the OLD length first. Shrinking the count otherwise stops addressing
  // the dropped pixels without ever telling them to go out, so the tail of the
  // chain stays latched on whatever it last showed — lit, and no longer counted
  // by the power meter, which is the dangerous direction for the estimate.
  s_strip.clear();
  s_strip.show();
  s_strip.updateLength(s_count + OFFS);
  // PAS de begin() ici, et c'est tout sauf un detail.
  //
  // Adafruit_NeoPixel::begin() fait `pinMode(pin, OUTPUT)`. Sur ESP32 cet appel
  // passe par le gestionnaire de peripheriques, qui ARRACHE la broche au canal
  // RMT pour la rendre a un GPIO ordinaire - puis `digitalWrite(pin, LOW)` la
  // laisse au niveau bas. Ensuite espShow() voit `pin == rmtPin`, en conclut
  // que le RMT est deja pret, et n'en refait rien : il ecrit dans un canal qui
  // n'est plus relie a la broche.
  //
  // Symptome : toute la chaine s'eteint au premier changement de longueur, et
  // AUCUN compteur ne bronche - rmtfail reste a 0, rmtframes continue de
  // monter, fps affiche 63. Le firmware se croit en parfaite sante pendant que
  // le mur est noir. Constate sur la vraie machine le 2026-08-07 en passant de
  // 42 a 40 pixels, et d'abord pris pour un fil de masse debranche.
  //
  // updateLength() ne touche ni a la broche ni au RMT - elle ne fait que
  // reallouer le tampon - donc il n'y a rien a reinitialiser. show() ne
  // regarde que `pixels`, jamais `begun`.
  s_strip.clear();
  s_strip.show();
#if LED_CHAIN2_ENABLE
  s_strip2.clear();
  s_strip2.show();
  s_strip2.updateLength(s_count + OFFS);
  s_strip2.clear();                     // idem : surtout pas de begin()
  s_strip2.show();
#endif
}

bool repeater() { return s_repeater; }

void setRepeater(bool on) {
  if (on == s_repeater) return;
  s_repeater = on;
  s_pendLen  = true;        // la longueur physique de la chaine change
  markDirty();
}

uint8_t pin() { return s_pin; }

void setPin(uint8_t p) {
  if (p == s_pin || p > 48) return;
  s_pin = p;
  s_pendLen = true;         // la sortie est re-instanciee sur la nouvelle broche
  markDirty();
}

void setBudgetMa(uint16_t ma) {
  // Le plafond est materiel avant d'etre logiciel : au-dela de la fenetre de
  // U5, ce n'est plus le firmware qui assombrit la trame, c'est le limiteur qui
  // ecrete et qui leve son drapeau de defaut. Un `/api/led?budget=9000` passait
  // jusqu'ici sans broncher et rendait la protection aveugle.
  if (ma > LED_POWER_BUDGET_MAX) ma = LED_POWER_BUDGET_MAX;
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

// ---------------------------------------------------------------------------
//  Defaut du limiteur de sortie (U5, AP22652, ~FAULT actif bas)
// ---------------------------------------------------------------------------
static bool     s_fault      = false;
static uint16_t s_faultCount = 0;
static uint32_t s_faultSince = 0;

bool     ledFault()      { return s_fault; }
uint16_t ledFaultCount() { return s_faultCount; }

static void pollFault() {
  const uint32_t now = millis();
  // A la mise sous tension, le limiteur limite forcement : il charge la
  // capacite de la chaine. Signaler ce passage serait crier au loup a chaque
  // allumage, et plus personne ne regarderait l'alerte ensuite.
  if (now < ARENA_FAULT_IGNORE_MS) return;

  const bool low = digitalRead(PIN_ARENA_LED_FAULT) == LOW;
  if (!low) { s_faultSince = 0; s_fault = false; return; }

  if (!s_faultSince) { s_faultSince = now; return; }
  if (!s_fault && now - s_faultSince >= ARENA_FAULT_HOLD_MS) {
    s_fault = true;
    s_faultCount++;
    Serial.printf("[led] DEFAUT sortie plateau : le limiteur U5 bride depuis %lu ms "
                  "(court-circuit ou chaine trop gourmande) - episode n°%u\n",
                  (unsigned long)(now - s_faultSince), s_faultCount);
  }
}

float    lastAmps()   { return s_amps; }
bool     limited()    { return s_limited; }
uint32_t frameCount() { return s_frames; }
uint16_t fps()        { return s_fps; }
// Ce que le mode music entend REELLEMENT. Sans ces valeurs, un mur qui respire
// lentement est indiscernable d'un mur qui reagit mal : dans un cas le micro
// n'entend rien, dans l'autre l'effet est trop mou. Deux causes opposees, un
// seul symptome.
float musEnergy() { return s_musE; }
float musBass()   { return s_musB; }
float musTreble() { return s_musT; }
float musPeak()   { return s_musPeak; }
// Bornee a 1..120 : zero arreterait le rendu sans que rien ne le dise, et
// au-dela de 120 la trame ne tient plus sur le fil (150 px RGBW = 4,8 ms).
void setFrameHz(uint8_t h) { s_frameHz = h < 1 ? 1 : (h > 120 ? 120 : h); markDirty(); }
uint8_t frameHz() { return s_frameHz; }

// Voir arenaled.h pour pourquoi il y en a deux.
void resetLook() {
  s_mode    = MODE_CLASSIC;
  s_lastOn  = MODE_CLASSIC;
  s_bootOn  = true;
  s_bright  = ARENA_BRIGHT_DEFAULT;
  s_speed   = ARENA_SPEED_DEFAULT;
  s_gi      = ARENA_GI_DEFAULT;
  s_giColor = { 0, 0, 0, 0 };
  s_insBright = 255;
  s_champ = 255;
  s_frameHz = LED_FRAME_HZ;
  s_lvlLock = false;
  s_lockGi = s_gi; s_lockChamp = s_champ;
  s_warm    = ARENA_WARM_DEFAULT;
  s_density = 110;
  s_inc     = true;
  s_latched = 0;
  save();
  Serial.println("[led] apparence remise aux valeurs d'usine");
}

void resetAll() {
  // Desarmer AVANT d'effacer. tick() finit par "si s_dirtyAt a plus de 3 s,
  // save()", et save() reecrit les 17 cles depuis la RAM sans condition. Le
  // redemarrage, lui, n'est programme qu'a 800 ms (arena_net.cpp). Bouger un
  // curseur puis demander l'effacement dans la fenetre qui suit rendait donc
  // toute l'apparence de l'ancien proprietaire, console affirmant le contraire.
  s_dirtyAt = 0;
  s_prefs.clear();      // le cablage part avec : c'est le but, pas un effet de bord
  Serial.println("[led] NVS 'arena' efface en entier");
}

void save() {
  // Compare-and-clear: a setter on the HTTP task can re-arm s_dirtyAt while the
  // multi-millisecond NVS write below is in flight; clearing unconditionally at
  // the end would discard that change until the next unrelated save.
  const uint32_t dirtyAtEntry = s_dirtyAt;
  // By NAME, not by number: inserting MODE_MUSIC mid-enum silently remapped
  // every board whose NVS held mode=6 (TEST before, MUSIC after). Names survive
  // enum surgery; the numeric key is still read once for migration.
  s_prefs.putString("modeN", modeName(s_mode));
  s_prefs.putString("lastOn", modeName(s_lastOn));
  s_prefs.putUChar("bootOn", s_bootOn ? 1 : 0);
  s_prefs.putUChar("bright", s_bright);
  s_prefs.putUChar("speed",  s_speed);
  s_prefs.putUChar("gi",     s_gi);
  s_prefs.putUChar("dens",   s_density);
  s_prefs.putUChar("warm",   s_warm);
  s_prefs.putBytes("latched", &s_latched, sizeof(s_latched));
  s_prefs.putUChar("inc", s_inc ? 1 : 0);
  s_prefs.putUShort("count", s_count);
  s_prefs.putUShort("budget", s_budget);
  s_prefs.putBool("rep", s_repeater);
  s_prefs.putUChar("pin", s_pin);
  s_prefs.putBytes("color", &s_color, sizeof(s_color));
  s_prefs.putBytes("gicol", &s_giColor, sizeof(s_giColor));
  s_prefs.putUChar("insb", s_insBright);
  s_prefs.putUChar("champ", s_champ);
  s_prefs.putUChar("framehz", s_frameHz);
  s_prefs.putUChar("lvllock", s_lvlLock ? 1 : 0);
  s_prefs.putUChar("lockgi",  s_lockGi);
  s_prefs.putUChar("lockch",  s_lockChamp);
  s_prefs.putString("order", s_order);
  if (s_dirtyAt == dirtyAtEntry) s_dirtyAt = 0;
}

void begin() {
  pinMode(PIN_ARENA_LED_FAULT, INPUT_PULLUP);   // ~FAULT de U5, drain ouvert
#if ARENA_MIC_ENABLE
  micBegin();
#endif
  s_prefs.begin("arena", false);
  {
    String mn = s_prefs.getString("modeN", "");
    Mode m = mn.length() ? modeFromName(mn.c_str()) : MODE_COUNT;
    if (m == MODE_COUNT) m = (Mode)s_prefs.getUChar("mode", (uint8_t)MODE_CLASSIC);
    s_mode = (m < MODE_COUNT) ? m : MODE_CLASSIC;
  }
  if (s_mode >= MODE_COUNT) s_mode = MODE_CLASSIC;
  {
    String lo = s_prefs.getString("lastOn", "");
    Mode m = lo.length() ? modeFromName(lo.c_str()) : MODE_COUNT;
    s_lastOn = (m < MODE_COUNT && m != MODE_OFF) ? m : MODE_ATTRACT;
  }
  s_bootOn = s_prefs.getUChar("bootOn", 1) != 0;
  // Le courant revient : on rallume. Sinon une coupure de secteur laisse le mur
  // noir jusqu'a ce que quelqu'un s'en occupe, ce qui n'est pas un comportement
  // acceptable pour un objet accroche au mur.
  if (s_bootOn && s_mode == MODE_OFF) s_mode = s_lastOn;
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
  s_repeater = s_prefs.getBool("rep", LED_REPEATER_PIXEL != 0);
  s_pin      = s_prefs.getUChar("pin", PIN_LED_DATA);
  if (s_pin != s_pinLive) { s_strip.setPin(s_pin); s_pinLive = s_pin; }
  // Un mur deja en service porte l'ancienne valeur dans sa NVS : sans cette
  // borne, la mise a jour ne changerait rien la ou elle compte, c'est-a-dire
  // sur les cartes deja posees.
  if (s_budget > LED_POWER_BUDGET_MAX) s_budget = LED_POWER_BUDGET_MAX;
  if (s_count < 1 || s_count > LED_MAX) s_count = LED_COUNT_DEFAULT;
  if (s_prefs.getBytesLength("color") == sizeof(s_color))
    s_prefs.getBytes("color", &s_color, sizeof(s_color));
  if (s_prefs.getBytesLength("gicol") == sizeof(s_giColor))
    s_prefs.getBytes("gicol", &s_giColor, sizeof(s_giColor));
  s_insBright = s_prefs.getUChar("insb", 255);
  s_champ     = s_prefs.getUChar("champ", 255);
  s_frameHz   = s_prefs.getUChar("framehz", LED_FRAME_HZ);
  if (s_frameHz < 1 || s_frameHz > 120) s_frameHz = LED_FRAME_HZ;
  s_lvlLock   = s_prefs.getUChar("lvllock", 0) != 0;
  s_lockGi    = s_prefs.getUChar("lockgi",  s_gi);
  s_lockChamp = s_prefs.getUChar("lockch",  s_champ);
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
  // Le piege numero un du banc : avec le repeteur actif, le PREMIER pixel
  // physique de la chaine est tenu eteint EXPRES. Quelqu'un qui branche une
  // seule LED pour essayer voit donc une LED qui ne s'allume jamais, et rien
  // nulle part ne le lui dit. Le dire fort, au demarrage.
  if (OFFS)
    Serial.printf("[led] REPETEUR ACTIF : le 1er pixel physique reste ETEINT expres. "
                  "Il faut donc %u+1 = %u LED cablees, et la premiere visible est la "
                  "DEUXIEME de la chaine. Une carte controleur du commerce n'a PAS de "
                  "repeteur : dans ce cas -> /api/set?repeater=0 (garde en NVS, aucun "
                  "reflashage).\n",
                  s_count, s_count + 1);
  Serial.printf("[led] %u px on GPIO%d, order=%s mode=%s bright=%u budget=%u mA\n",
                s_count, s_pin, s_order, modeName(s_mode), s_bright, s_budget);
}

void tick() {
  if (s_paused) {
    // Filet de securite. Le degel dependait de kCHIPoBLEConnectionClosed, que
    // CHIP n'emet PAS apres un commissioning reussi : il demonte la pile BLE et
    // l'evenement se perd. Le mur restait alors noir indefiniment - et comme
    // fps, ma et le compteur de trames ne sont mis a jour QUE plus bas dans
    // cette fonction, ils restaient figes sur leur derniere valeur et donnaient
    // toutes les apparences d'un rendu qui tourne. Vecu le 2026-08-02, une heure
    // perdue a chercher pourquoi l'image etait noire alors qu'elle n'existait
    // plus. Un appairage ne dure jamais deux minutes : on degele, point.
    if (millis() - s_pausedAt < ARENA_PAUSE_MAX_MS) return;
    s_paused = false;
  }
  pollFault();

  uint32_t now = millis();
  if (now - s_lastFrame < (uint32_t)(1000 / (s_frameHz ? s_frameHz : 1))) return;
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

  // Correction de luminosite pixel par pixel. APRES le plastique et AVANT la
  // luminosite globale : c'est une propriete de la lampe, pas du mode ni de
  // l'ambiance. Posee avant le plastique elle deraperait la teinte, posee apres
  // la luminosite globale elle serait ecrasee des qu'on baisse le mur.
  for (uint16_t i = 0; i < s_count; i++) {
    const uint8_t t = arenapf::trimOf(i);
    if (t != 255) s_frame[i] = scale(s_frame[i], (float)t / 255.0f);
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
          // Montrer le groupe, c'est montrer SES MEMBRES. Eclairer la tranche
          // qui va du premier au dernier ferait clignoter les pixels des autres
          // groupes intercales - et l'assistant de cartographie sert justement
          // a repondre "lesquels ?", donc il ne peut pas mentir la-dessus.
          for (uint16_t n = 0; n < zn->count; n++) {
            const int i = arenamap::zoneNth((uint8_t)s_idZone, n);
            if (i >= 0 && i < (int)s_count) s_frame[i] = scale(hot, blink);
          }
        }
      }
    }
  }

  memcpy(s_render, s_frame, sizeof(Rgbw) * LED_MAX);  // pre-gamma copy for the next crossfade

  // Plus de plafond special pour la nuit : fxNight limite deja son amplitude a
  // 0,6 et n'allume que quelques pixels.
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
