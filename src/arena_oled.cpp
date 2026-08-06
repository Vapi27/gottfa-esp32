// arena_oled.cpp — SSD1306 control screen + rotary encoder for the wall.
// (C) 2026 Valere Pilpil / Pstore. Original implementation.
#include "arena_config.h"
#if ARENA_OLED_ENABLE

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "arena_oled.h"
#include "arenaled.h"
#include "arena_net.h"
#include "arena_qr.h"

namespace arenaoled {

static Adafruit_SSD1306 s_d(ARENA_OLED_W, ARENA_OLED_H, &Wire, -1);
static bool     s_found = false;
static bool     s_awake = false;
static uint32_t s_lastIn = 0;

// ---------------------------------------------------------------------------
//  Rotary encoder
//
//  Read under interrupt rather than polled from loop(). A hand-turned EC11
//  emits detents in a few milliseconds, and loop() is not free: pushing 41 RGBW
//  pixels blocks ~1.6 ms and a Matter/WiFi hiccup blocks far longer. Polling
//  drops steps exactly when the owner turns the knob quickly, which reads as a
//  broken control rather than a missed sample.
//
//  Full-step decoding: the detent lands on a stable state, so only count when
//  the quadrature pair returns to rest. Half-step counting on an EC11 gives two
//  units per click and drives everyone mad.
// ---------------------------------------------------------------------------
static volatile int8_t  s_encDelta = 0;
static volatile uint8_t s_encPrev  = 0;

static void IRAM_ATTR encIsr() {
  // Gray-code transition table: index = (previous << 2) | current.
  static const int8_t STEP[16] = { 0, -1, 1, 0,  1, 0, 0, -1,
                                  -1, 0, 0, 1,   0, 1, -1, 0 };
  const uint8_t cur = (uint8_t)((digitalRead(PIN_ARENA_ENC_A) << 1) | digitalRead(PIN_ARENA_ENC_B));
  const int8_t  st  = STEP[((s_encPrev & 3) << 2) | cur];
  s_encPrev = cur;
  if (!st) return;
  static volatile int8_t acc = 0;
  acc += st;
  if (cur == 3) {                 // back at rest = one detent
    if (acc > 1)       s_encDelta++;
    else if (acc < -1) s_encDelta--;
    acc = 0;
  }
}

static int8_t encTake() {
  noInterrupts();
  const int8_t d = s_encDelta;
  s_encDelta = 0;
  interrupts();
  return d;
}

// ---------------------------------------------------------------------------
//  Menu
// ---------------------------------------------------------------------------
enum Item : uint8_t { IT_MODE = 0, IT_BRIGHT, IT_SPEED, IT_GI, IT_FILAMENT, IT_SIRI, IT_INFO, IT_N };
static const char* ITEM_NAME[IT_N] = { "Mode", "Brightness", "Speed", "Glow", "Filament", "Siri code", "Info" };

static uint8_t s_item   = IT_MODE;
static bool    s_edit   = false;   // false = choosing an item, true = changing its value

// Modes offered on the knob. TEST is deliberately absent: it is a wiring check,
// and nobody standing in front of a finished wall wants to land on it by
// turning one notch too far. OFF is absent for the same reason - the screen is
// how you get the wall back, so it must not be able to switch it off blindly.
static const arenaled::Mode WHEEL[] = {
  arenaled::MODE_ATTRACT, arenaled::MODE_CLASSIC, arenaled::MODE_ARENA,
  arenaled::MODE_RAINBOW, arenaled::MODE_NIGHT,   arenaled::MODE_MUSIC,
};
static const uint8_t WHEEL_N = sizeof(WHEEL) / sizeof(WHEEL[0]);

static uint8_t wheelIndexOfCurrent() {
  const arenaled::Mode m = arenaled::mode();
  for (uint8_t i = 0; i < WHEEL_N; i++) if (WHEEL[i] == m) return i;
  return 0;
}

// ---------------------------------------------------------------------------
//  Drawing
// ---------------------------------------------------------------------------
static void drawQr() {
  s_d.clearDisplay();

  // Sur un panneau de 32 pixels de haut, le QR est INUTILISABLE, et ce n'est pas
  // une question de reglage : le code fait 29 modules, donc 1 pixel par module,
  // donc des modules de l'ordre du dixieme de millimetre. Pour les resoudre un
  // iPhone doit s'approcher plus pres que sa distance minimale de mise au point,
  // et il n'y arrive jamais. Constate sur la vraie machine : "l'iphone n'arrive
  // pas a faire le focus". Aucun firmware ne corrige une limite optique.
  //
  // Donc ici on n'affiche PAS de QR : toute la surface va au code en chiffres,
  // qui s'appaire a la main dans l'app Maison sans le moindre appareil photo.
  // Le QR reste dessine sur un panneau de 64, ou il passe a 2 px/module.
  if (ARENA_OLED_H < 64) {
    s_d.ssd1306_command(SSD1306_SETCONTRAST);
    s_d.ssd1306_command(0xCF);
    s_d.setTextColor(SSD1306_WHITE);
    s_d.setTextSize(1);
    s_d.setCursor(0, 0);
    s_d.print(F("Code d'appairage"));
    s_d.setTextSize(2);
    s_d.setCursor(4, 10);
    s_d.print(F("3497-011"));
    s_d.setTextSize(2);
    s_d.setCursor(28, 26);
    s_d.print(F("2332"));
    s_d.display();
    return;
  }

  const int16_t side = ARENA_QR_SIDE;
  const int16_t x0 = 2;
  const int16_t y0 = (ARENA_OLED_H - side) / 2;

  s_d.ssd1306_command(SSD1306_SETCONTRAST);
  s_d.ssd1306_command(0x20);
  s_d.fillRect(x0, y0, side, side, SSD1306_BLACK);
  for (int16_t y = 0; y < side; y++)
    for (int16_t x = 0; x < side; x++)
      if (ARENA_QR[y][x >> 3] & (0x80 >> (x & 7)))
        s_d.drawPixel(x0 + x, y0 + y, SSD1306_WHITE);

  const int16_t tx = x0 + side + 6;
  s_d.setTextColor(SSD1306_WHITE);
  s_d.setTextSize(2);
  s_d.setCursor(tx, 6);   s_d.print(F("3497"));
  s_d.setCursor(tx, 24);  s_d.print(F("011"));
  s_d.setCursor(tx, 42);  s_d.print(F("2332"));
  s_d.display();
}

static void drawBar(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t v);

// Sur un panneau de 32 px, un nombre en taille 2 pose a y=12 occupe jusqu'a
// y=28, et une barre a y=H-7 commence a 25 : trois pixels de chevauchement.
// La barre passe donc a DROITE du nombre sur les panneaux courts, et reste en
// pied de page sur les grands, ou il y a la place.
static void drawValueBar(uint8_t v) {
  if (ARENA_OLED_H >= 64) drawBar(0, ARENA_OLED_H - 9, ARENA_OLED_W, 9, v);
  else                    drawBar(62, 14, ARENA_OLED_W - 64, 12, v);
}

static void drawBar(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t v) {
  s_d.drawRect(x, y, w, h, SSD1306_WHITE);
  const int16_t fill = (int16_t)(((int32_t)(w - 2) * v) / 255);
  if (fill > 0) s_d.fillRect(x + 1, y + 1, fill, h - 2, SSD1306_WHITE);
}

static void draw() {
  s_d.clearDisplay();
  s_d.setTextColor(SSD1306_WHITE);

  if (s_item == IT_SIRI && s_edit) { drawQr(); s_d.display(); return; }

  // Contraste nominal partout ailleurs : le menu se lit de loin, lui.
  s_d.ssd1306_command(SSD1306_SETCONTRAST);
  s_d.ssd1306_command(0xCF);

  // Header: the item, and whether the knob is currently changing it.
  s_d.setTextSize(1);
  s_d.setCursor(0, 0);
  if (s_edit) s_d.print(F(">"));
  s_d.print(ITEM_NAME[s_item]);

  // Value line, big enough to read from a couple of metres.
  s_d.setCursor(0, 12);
  switch (s_item) {
    case IT_MODE:
      s_d.setTextSize(2);
      s_d.print(arenaled::modeLabel(arenaled::mode()));
      break;
    case IT_BRIGHT:
      s_d.setTextSize(2); s_d.print((int)(arenaled::brightness() * 100 / 255)); s_d.print('%');
      drawValueBar(arenaled::brightness());
      break;
    case IT_SPEED:
      s_d.setTextSize(2); s_d.print((int)(arenaled::speed() * 100 / 255)); s_d.print('%');
      drawValueBar(arenaled::speed());
      break;
    case IT_GI:
      s_d.setTextSize(2); s_d.print((int)(arenaled::gi() * 100 / 255)); s_d.print('%');
      drawValueBar(arenaled::gi());
      break;
    case IT_FILAMENT:
      s_d.setTextSize(2); s_d.print(arenaled::incandescent() ? F("ON") : F("off"));
      break;
    case IT_SIRI:
      s_d.setTextSize(1);
      s_d.print(F("press to show"));
      s_d.setCursor(0, 22); s_d.print(F("the pairing code"));
      break;
    case IT_INFO:
      s_d.setTextSize(1);
      s_d.print(arenanet::ip());
      s_d.setCursor(0, 22);
      s_d.print(F("v" ARENA_FW_VERSION));
      break;
    default: break;
  }
  s_d.display();
}

// ---------------------------------------------------------------------------
//  Sleep
// ---------------------------------------------------------------------------
static void sleepNow() {
  if (!s_awake) return;
  s_awake = false;
  s_edit  = false;
  s_item  = IT_MODE;                       // come back to the top, not mid-edit
  s_d.clearDisplay();
  s_d.display();
  s_d.ssd1306_command(SSD1306_DISPLAYOFF); // powered down, not just black
}

void showQr() {
  if (!s_found) return;
  s_item = IT_SIRI;
  s_edit = true;
  poke();
  draw();
}

void poke() {
  s_lastIn = millis();
  if (s_awake || !s_found) return;
  s_awake = true;
  s_d.ssd1306_command(SSD1306_DISPLAYON);
  draw();
}

// ---------------------------------------------------------------------------
//  Input handling
// ---------------------------------------------------------------------------
static void applyTurn(int8_t d) {
  switch (s_item) {
    case IT_MODE: {
      int8_t i = (int8_t)wheelIndexOfCurrent() + d;
      while (i < 0) i += WHEEL_N;
      arenaled::setMode(WHEEL[i % WHEEL_N]);
      break;
    }
    // Steps of 8 on a 0..255 range: 32 detents end to end, about a full turn on
    // an EC11 with 20 detents per revolution. Steps of 1 would need eight turns
    // and everyone would think the knob was broken.
    case IT_BRIGHT: {
      int v = (int)arenaled::brightness() + d * 8;
      arenaled::setBrightness((uint8_t)constrain(v, 1, 255));
      break;
    }
    case IT_SPEED: {
      int v = (int)arenaled::speed() + d * 8;
      arenaled::setSpeed((uint8_t)constrain(v, 0, 255));
      break;
    }
    case IT_GI: {
      int v = (int)arenaled::gi() + d * 8;
      arenaled::setGi((uint8_t)constrain(v, 0, 255));
      break;
    }
    case IT_FILAMENT:
      arenaled::setIncandescent(!arenaled::incandescent());
      break;
    default: break;
  }
}

static void onTurn(int8_t d) {
  poke();
  if (!s_edit) {
    int8_t i = (int8_t)s_item + d;
    while (i < 0) i += IT_N;
    s_item = (uint8_t)(i % IT_N);
  } else {
    applyTurn(d);
  }
  draw();
}

static void onPress(bool longPress) {
  poke();
  if (longPress) {                 // long press always means "out of here"
    if (s_edit) { s_edit = false; arenaled::save(); }
    else        sleepNow();
    draw();
    return;
  }
  if (s_item == IT_INFO) { draw(); return; }        // nothing to edit
  s_edit = !s_edit;
  // Leaving an edit is the moment to persist: one NVS write per adjustment
  // session instead of one per detent.
  if (!s_edit) arenaled::save();
  draw();
}

// ---------------------------------------------------------------------------
void begin() {
  Wire.begin(PIN_ARENA_OLED_SDA, PIN_ARENA_OLED_SCL);
  // Probe before trusting the panel: a wall with no screen fitted must not spend
  // its loop talking to nothing, and Adafruit's begin() happily returns true on
  // some clones that are not there.
  Wire.beginTransmission(ARENA_OLED_ADDR);
  s_found = (Wire.endTransmission() == 0) &&
            s_d.begin(SSD1306_SWITCHCAPVCC, ARENA_OLED_ADDR);
  if (!s_found) { Serial.println("[oled] no panel - screen disabled"); return; }

  pinMode(PIN_ARENA_ENC_A,  INPUT_PULLUP);
  pinMode(PIN_ARENA_ENC_B,  INPUT_PULLUP);
  pinMode(PIN_ARENA_ENC_SW, INPUT_PULLUP);
  s_encPrev = (uint8_t)((digitalRead(PIN_ARENA_ENC_A) << 1) | digitalRead(PIN_ARENA_ENC_B));
  attachInterrupt(digitalPinToInterrupt(PIN_ARENA_ENC_A), encIsr, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_ARENA_ENC_B), encIsr, CHANGE);

  s_d.setRotation(0);
  Serial.printf("[oled] SSD1306 %dx%d on SDA%d/SCL%d, encoder %d/%d/%d\n",
                ARENA_OLED_W, ARENA_OLED_H, PIN_ARENA_OLED_SDA, PIN_ARENA_OLED_SCL,
                PIN_ARENA_ENC_A, PIN_ARENA_ENC_B, PIN_ARENA_ENC_SW);
  // Au demarrage : le code d'appairage pendant la mise au point, le menu sinon.
  if (ARENA_OLED_BOOT_QR) showQr();
  else                    poke();   // greet whoever just powered the wall on
}

bool found() { return s_found; }

void tick() {
  if (!s_found) return;
  const uint32_t now = millis();

  const int8_t d = encTake();
  if (d) onTurn(d > 0 ? 1 : -1);

  // Encoder push, debounced, with a long press for "back / sleep".
  static uint32_t downAt = 0;
  static bool     fired  = false;
  const bool      down   = digitalRead(PIN_ARENA_ENC_SW) == LOW;
  if (down && !downAt) { downAt = now; fired = false; }
  else if (down && !fired && now - downAt > 700) { onPress(true);  fired = true; }
  else if (!down && downAt) {
    if (!fired && now - downAt > 25) onPress(false);
    downAt = 0;
  }

  if (!s_awake) return;

  // Repaint slowly while awake so a change made from the web page or Siri shows
  // up here too - the screen must not claim a mode the wall left ten seconds ago.
  static uint32_t lastDraw = 0;
  if (now - lastDraw > 400) { lastDraw = now; draw(); }

  // 0 = jamais. Voir le bandeau dans arena_config.h.
  if (ARENA_OLED_SLEEP_MS && now - s_lastIn > ARENA_OLED_SLEEP_MS) sleepNow();
}

}  // namespace arenaoled

#else   // ARENA_OLED_ENABLE == 0
namespace arenaoled {
void begin() {}
void tick()  {}
void showQr(){}
bool found() { return false; }
void poke()  {}
}
#endif
