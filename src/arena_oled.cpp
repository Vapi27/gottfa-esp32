// arena_oled.cpp — SSD1306 control screen for the wall.
// (C) 2026 Valere Pillet / Pstore. Original implementation.
//
// Navigation a trois boutons : deux fleches et OK. C'est le montage de la carte
// definitive, et il commande un vrai arbre de menus plutot qu'une liste a plat -
// une liste unique melangeait le mode, la luminosite, le code d'appairage et les
// diagnostics au meme niveau, et il fallait la parcourir en entier pour changer
// une seule chose.
//
// L'encodeur rotatif est ABANDONNE (ARENA_ENC_ENABLE 0) : trois poussoirs
// lateraux en bord de carte font le meme travail sans piece traversante. Le code
// de lecture reste sous condition pour qui voudrait le recabler, mais aucune
// interruption n'est posee sur GPIO4/5, qui sont donc libres.
#include "arena_config.h"
#if ARENA_OLED_ENABLE

#include <Wire.h>
#include <WiFi.h>            // RSSI, affiche par l'entree Network
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "arena_oled.h"
#include "arenaled.h"
#include "arena_net.h"
#include "arena_peers.h"
#include "arena_qr.h"

namespace arenaoled {

static Adafruit_SSD1306 s_d(ARENA_OLED_W, ARENA_OLED_H, &Wire, -1);
static bool     s_found = false;
static bool     s_awake = false;
static uint32_t s_lastIn = 0;

// ---------------------------------------------------------------------------
//  Encodeur (optionnel)
//
//  Lu sous interruption et non depuis loop() : un EC11 tourne a la main emet ses
//  crans en quelques millisecondes, et loop() n'est pas libre - pousser 41
//  pixels RGBW bloque ~1,6 ms, un hoquet WiFi bien davantage. Un scrutin rate
//  des crans exactement quand le proprietaire tourne vite, ce qui se lit comme
//  un bouton casse et non comme un echantillon manque.
// ---------------------------------------------------------------------------
#if ARENA_ENC_ENABLE
static volatile int8_t  s_encDelta = 0;
static volatile uint8_t s_encPrev  = 0;

static void IRAM_ATTR encIsr() {
  static const int8_t STEP[16] = { 0, -1, 1, 0,  1, 0, 0, -1,
                                  -1, 0, 0, 1,   0, 1, -1, 0 };
  const uint8_t cur = (uint8_t)((digitalRead(PIN_ARENA_ENC_A) << 1) | digitalRead(PIN_ARENA_ENC_B));
  const int8_t  st  = STEP[((s_encPrev & 3) << 2) | cur];
  s_encPrev = cur;
  if (!st) return;
  static volatile int8_t acc = 0;
  acc += st;
  if (cur == 3) {                 // retour au repos = un cran
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
#else
// Pas d'encodeur cable : rien a lire, et surtout aucune interruption posee sur
// des broches en l'air. GPIO4 et GPIO5 restent disponibles.
static inline int8_t encTake() { return 0; }
#endif

// ---------------------------------------------------------------------------
//  Arbre de menus
// ---------------------------------------------------------------------------
enum Kind : uint8_t {
  K_MENU,    // porte des enfants
  K_MODE,    // choix du mode d'eclairage
  K_PCT,     // valeur 0..255 presentee en pourcents
  K_BOOL,
  K_LINK,    // comportement face aux autres murs
  K_INFO,    // se consulte, ne se regle pas
  K_QR,      // le code d'appairage, en plein ecran
  K_SLEEP,   // eteindre l'ecran tout de suite
  K_CONFIRM, // efface quelque chose : demande un appui maintenu
};

enum : uint8_t {
  N_ROOT = 0,
  N_LIGHT, N_FX, N_WALLS, N_PAIR, N_NET, N_ABOUT, N_RESET, N_OFF,  // 1..8  enfants de ROOT
  N_MODE, N_BRIGHT, N_GLOW, N_WARM,                                // 9..12 enfants de LIGHT
  N_SPEED, N_FILAMENT, N_FLIES,                                    // 13..15 enfants de FX
  N_LINK, N_PEERS,                                                 // 16..17 enfants de WALLS
  N_R_REBOOT, N_R_LOOK, N_R_HOMES, N_R_ALL,                        // 18..21 enfants de RESET
                                                                   // MEME ORDRE que NODES[]
  N_COUNT
};

struct Node {
  const char* label;
  uint8_t     kind;
  uint8_t     first;   // premier enfant
  uint8_t     count;   // nombre d'enfants
};

// L'ordre compte : les enfants d'un noeud doivent etre contigus a partir de
// `first`. C'est ce qui permet a la table de tenir en quelques octets et a la
// navigation de se resumer a une addition.
static const Node NODES[N_COUNT] = {
  { "Menu",        K_MENU,  N_LIGHT,  8 },
  { "Lighting",    K_MENU,  N_MODE,   4 },
  { "Effects",     K_MENU,  N_SPEED,  3 },
  { "Other walls", K_MENU,  N_LINK,   2 },
  { "Pairing code",K_QR,    0, 0 },
  { "Network",     K_INFO,  0, 0 },
  { "About",       K_INFO,  0, 0 },
  { "Reset",       K_MENU,  N_R_REBOOT, 4 },
  { "Screen off",  K_SLEEP, 0, 0 },
  { "Mode",        K_MODE,  0, 0 },
  { "Brightness",  K_PCT,   0, 0 },
  { "Glow",        K_PCT,   0, 0 },
  { "Warm white",  K_PCT,   0, 0 },
  { "Speed",       K_PCT,   0, 0 },
  { "Filament",    K_BOOL,  0, 0 },
  { "Fireflies",   K_PCT,   0, 0 },
  { "Link",        K_LINK,  0, 0 },
  { "Neighbours",  K_INFO,  0, 0 },
  // Du plus anodin au plus definitif, et dans cet ordre : la fleche qui depasse
  // d'un cran tombe sur quelque chose de moins grave, jamais de plus grave.
  { "Restart",     K_CONFIRM, 0, 0 },
  { "Reset look",  K_CONFIRM, 0, 0 },
  { "Forget homes",K_CONFIRM, 0, 0 },
  { "Erase all",   K_CONFIRM, 0, 0 },
};

// Pile de navigation. Trois niveaux suffisent a l'arbre ci-dessus, et une pile
// bornee ne peut pas deriver : on ne descend jamais dans un noeud sans enfants.
struct Frame { uint8_t node; uint8_t cursor; };
static Frame   s_stack[4] = { { N_ROOT, 0 } };
static uint8_t s_depth = 0;
static bool    s_edit  = false;

// Confirmation d'un effacement : le noeud arme, et l'instant ou le doigt s'est
// pose. Trois secondes de maintien, pas un appui - sur trois boutons, un simple
// OK est exactement le geste qu'on fait par erreur en cherchant autre chose, et
// il ne doit pas pouvoir depairer un mur ou effacer son cablage.
static const uint32_t CONFIRM_MS = 3000;
static uint8_t  s_confirm  = 0;      // 0 = rien d'arme
static uint32_t s_holdFrom = 0;

static uint8_t curNode()   { const Frame& f = s_stack[s_depth];
                             return NODES[f.node].count ? NODES[f.node].first + f.cursor : f.node; }
static uint8_t parentNode(){ return s_stack[s_depth].node; }

// Modes proposes a l'ecran. TEST est volontairement absent - c'est un controle
// de cablage, et personne devant un mur fini ne veut y tomber d'une fleche de
// trop. OFF l'est aussi : l'ecran est ce par quoi on rallume le mur, il ne doit
// pas pouvoir l'eteindre a l'aveugle.
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
//  Valeurs
// ---------------------------------------------------------------------------
static uint8_t valueOf(uint8_t n) {
  switch (n) {
    case N_BRIGHT: return arenaled::brightness();
    case N_GLOW:   return arenaled::gi();
    case N_WARM:   return arenaled::warm();
    case N_SPEED:  return arenaled::speed();
    case N_FLIES:  return arenaled::density();
    default:       return 0;
  }
}
static void setValue(uint8_t n, uint8_t v) {
  switch (n) {
    case N_BRIGHT: arenaled::setBrightness(v ? v : 1); break;   // 0 = mur noir sans le vouloir
    case N_GLOW:   arenaled::setGi(v);         break;
    case N_WARM:   arenaled::setWarm(v);       break;
    case N_SPEED:  arenaled::setSpeed(v);      break;
    case N_FLIES:  arenaled::setDensity(v);    break;
    default: break;
  }
}

// ---------------------------------------------------------------------------
//  Dessin
// ---------------------------------------------------------------------------
static void drawValueBar(uint8_t v) {
  const int16_t y = ARENA_OLED_H - 5;
  s_d.drawRect(0, y, ARENA_OLED_W, 5, SSD1306_WHITE);
  s_d.fillRect(1, y + 1, (int16_t)((ARENA_OLED_W - 2) * v / 255), 3, SSD1306_WHITE);
}

static void drawQr() {
  s_d.clearDisplay();

  // Sur un panneau de 32 pixels de haut, le QR est INUTILISABLE, et ce n'est pas
  // une question de reglage : le code fait 29 modules, donc 1 pixel par module,
  // donc des modules de l'ordre du dixieme de millimetre. Pour les resoudre un
  // iPhone doit s'approcher plus pres que sa distance minimale de mise au point,
  // et il n'y arrive pas. Constate sur la vraie machine. Aucun firmware ne
  // corrige une limite optique.
  //
  // Donc ici on n'affiche PAS de QR : toute la surface va au code en chiffres,
  // qui s'appaire a la main dans l'app Maison sans le moindre appareil photo.
  // Le QR reste dessine sur un panneau de 64, ou il passe a 2 px/module.
  s_d.setTextColor(SSD1306_WHITE);
  if (ARENA_OLED_H < 64) {
    s_d.ssd1306_command(SSD1306_SETCONTRAST);
    s_d.ssd1306_command(0xCF);
    s_d.setTextSize(1);
    s_d.setCursor(0, 0);
    // Le nom du mur en tete, pas un titre : quand on appaire le troisieme d'une
    // serie, il faut voir sans ambiguite devant lequel on se trouve - les
    // murs partagent tous le meme code d'appairage.
    s_d.print(arenanet::wallName());
    s_d.setTextSize(2);
    s_d.setCursor(4, 10);  s_d.print(F("3497-011"));
    s_d.setCursor(28, 26); s_d.print(F("2332"));
    s_d.display();
    return;
  }

  const int16_t side = ARENA_QR_SIDE;
  const int16_t x0 = 2;
  const int16_t y0 = (ARENA_OLED_H - side) / 2;
  // Contraste bas pour le QR : a pleine luminosite un OLED bave, les modules
  // allumes debordent sur leurs voisins eteints et l'appareil photo perd la
  // grille. Un QR se lit mieux terne.
  s_d.ssd1306_command(SSD1306_SETCONTRAST);
  s_d.ssd1306_command(0x20);
  for (int16_t y = 0; y < side; y++)
    for (int16_t x = 0; x < side; x++)
      if (ARENA_QR[y][x >> 3] & (0x80 >> (x & 7)))
        s_d.drawPixel(x0 + x, y0 + y, SSD1306_WHITE);
  const int16_t tx = x0 + side + 6;
  s_d.setTextSize(2);
  s_d.setCursor(tx, 6);   s_d.print(F("3497"));
  s_d.setCursor(tx, 24);  s_d.print(F("011"));
  s_d.setCursor(tx, 42);  s_d.print(F("2332"));
  s_d.display();
}

// Ce que fait vraiment chaque effacement. Le libelle du menu est court par
// necessite ; c'est cette ligne qui doit lever l'ambiguite AVANT le geste.
static const char* resetWhat(uint8_t n) {
  switch (n) {
    case N_R_REBOOT: return "restarts the wall";
    case N_R_LOOK:   return "colours + levels";
    case N_R_HOMES:  return "unpairs from Siri";
    case N_R_ALL:    return "all, incl. wiring";
    default:         return "";
  }
}

// Une entree d'information tient en deux courtes lignes : sur 32 pixels il n'y
// a pas de troisieme ligne, et une valeur tronquee ne renseigne personne.
static void infoLines(uint8_t n, String& a, String& b) {
  switch (n) {
    case N_NET:
      a = String(arenanet::mode()) + " " + arenanet::ip();
      b = "rssi " + String(WiFi.RSSI()) + " dBm";
      break;
    case N_PEERS: {
      const uint8_t c = arenapeers::count();
      a = c ? (String(c) + (c > 1 ? " walls seen" : " wall seen")) : String("none seen");
      b = "this one is #" + String(arenapeers::rank() + 1);
      break;
    }
    default:
      a = arenanet::wallName();
      b = "v" ARENA_FW_VERSION;
      break;
  }
}

// Barre qui se remplit pendant le maintien. Elle sert autant a doser l'effort
// qu'a prevenir : trois secondes sans retour visuel se lisent comme un bouton
// mort, et on relache juste avant la fin.
static void drawConfirm(uint32_t held) {
  s_d.clearDisplay();
  s_d.setTextColor(SSD1306_WHITE);
  s_d.ssd1306_command(SSD1306_SETCONTRAST);
  s_d.ssd1306_command(0xCF);
  s_d.setTextSize(1);
  s_d.setCursor(0, 0);
  s_d.print(NODES[s_confirm].label);
  s_d.setCursor(0, 10);
  s_d.print(resetWhat(s_confirm));
  s_d.setCursor(0, 20);
  s_d.print(held ? F("keep holding...") : F("hold OK for 3 s"));
  const int16_t y = ARENA_OLED_H - 5;
  s_d.drawRect(0, y, ARENA_OLED_W, 5, SSD1306_WHITE);
  if (held) {
    uint32_t w = (ARENA_OLED_W - 2) * (held > CONFIRM_MS ? CONFIRM_MS : held) / CONFIRM_MS;
    s_d.fillRect(1, y + 1, (int16_t)w, 3, SSD1306_WHITE);
  }
  s_d.display();
}

static void draw() {
  if (!s_awake) return;
  // Un defaut de sortie passe AVANT tout le reste : c'est la seule chose que le
  // proprietaire doit voir sans avoir a chercher. Il ne s'affiche que s'il a
  // persiste, donc jamais pour le passage en limitation du demarrage.
  if (arenaled::ledFault()) {
    s_d.clearDisplay();
    s_d.setTextColor(SSD1306_WHITE);
    s_d.ssd1306_command(SSD1306_SETCONTRAST);
    s_d.ssd1306_command(0xCF);
    s_d.setTextSize(1);
    s_d.setCursor(0, 0);
    s_d.print(F("DEFAUT SORTIE"));
    s_d.setTextSize(1);
    s_d.setCursor(0, 12);
    s_d.print(F("verifie le cablage"));
    s_d.setCursor(0, 22);
    s_d.print(F("du plateau"));
    s_d.display();
    return;
  }
  if (s_confirm) { drawConfirm(s_holdFrom ? millis() - s_holdFrom : 0); return; }
  const uint8_t n = curNode();

  if (NODES[n].kind == K_QR && s_edit) { drawQr(); return; }

  s_d.clearDisplay();
  s_d.setTextColor(SSD1306_WHITE);
  // Contraste nominal partout ailleurs : le menu se lit de loin, lui.
  s_d.ssd1306_command(SSD1306_SETCONTRAST);
  s_d.ssd1306_command(0xCF);

  // Ligne de tete : ou l'on se trouve, et a quel rang dans la liste. Sans ce
  // reperage, un ecran d'une seule ligne ne dit jamais s'il reste des entrees.
  s_d.setTextSize(1);
  s_d.setCursor(0, 0);
  const Frame& f = s_stack[s_depth];
  if (s_edit) {
    s_d.print(F("< "));
    s_d.print(NODES[n].label);
  } else {
    s_d.print(NODES[f.node].label);
    if (NODES[f.node].count) {
      char pos[10];
      snprintf(pos, sizeof(pos), "%u/%u", f.cursor + 1, NODES[f.node].count);
      s_d.setCursor(ARENA_OLED_W - (int16_t)strlen(pos) * 6, 0);
      s_d.print(pos);
    }
  }

  s_d.setCursor(0, 11);
  switch (NODES[n].kind) {
    case K_MENU:
      s_d.setTextSize(2);
      s_d.print(NODES[n].label);
      break;
    case K_MODE:
      s_d.setTextSize(2);
      s_d.print(arenaled::modeLabel(arenaled::mode()));
      break;
    case K_PCT:
      s_d.setTextSize(2);
      if (!s_edit) { s_d.print(NODES[n].label); }
      else         { s_d.print((int)(valueOf(n) * 100 / 255)); s_d.print('%');
                     drawValueBar(valueOf(n)); }
      break;
    case K_BOOL:
      s_d.setTextSize(2);
      s_d.print(NODES[n].label);
      s_d.setTextSize(1);
      s_d.setCursor(ARENA_OLED_W - 24, 18);
      s_d.print(arenaled::incandescent() ? F("ON") : F("off"));
      break;
    case K_LINK:
      s_d.setTextSize(2);
      s_d.print(arenapeers::linkName(arenapeers::link()));
      break;
    case K_QR:
      s_d.setTextSize(1);
      s_d.print(F("OK shows the"));
      s_d.setCursor(0, 22);
      s_d.print(F("pairing code"));
      break;
    case K_SLEEP:
      s_d.setTextSize(2);
      s_d.print(NODES[n].label);
      break;
    case K_CONFIRM:
      s_d.setTextSize(2);
      s_d.print(NODES[n].label);
      s_d.setTextSize(1);
      s_d.setCursor(0, 25);
      s_d.print(resetWhat(n));
      break;
    case K_INFO: {
      String a, b;
      infoLines(n, a, b);
      s_d.setTextSize(1);
      s_d.print(a);
      s_d.setCursor(0, 22);
      s_d.print(b);
      break;
    }
  }
  s_d.display();
}

// ---------------------------------------------------------------------------
//  Veille
// ---------------------------------------------------------------------------
static void sleepNow() {
  if (!s_awake) return;
  s_awake    = false;
  s_edit     = false;
  s_confirm  = 0;
  s_holdFrom = 0;
  s_depth    = 0;
  s_stack[0].node = N_ROOT;
  s_stack[0].cursor = 0;              // on revient en haut, pas au milieu d'un reglage
  s_d.clearDisplay();
  s_d.display();
  s_d.ssd1306_command(SSD1306_DISPLAYOFF);   // coupe, pas seulement noirci
}

void poke() {
  s_lastIn = millis();
  if (s_awake || !s_found) return;
  s_awake = true;
  s_d.ssd1306_command(SSD1306_DISPLAYON);
  draw();
}

void showQr() {
  if (!s_found) return;
  s_depth = 1;
  s_stack[0] = { N_ROOT, (uint8_t)(N_PAIR - N_LIGHT) };
  s_stack[1] = { N_PAIR, 0 };
  s_edit = true;
  poke();
  draw();
}

// Le geste est alle au bout : on execute, on l'ecrit a l'ecran, et on redemarre
// quand il le faut. Le message reste une seconde - un mur qui repart sans un mot
// laisse croire que rien ne s'est passe.
static void runReset(uint8_t n) {
  s_d.clearDisplay();
  s_d.setTextColor(SSD1306_WHITE);
  s_d.setTextSize(2);
  s_d.setCursor(0, 8);

  switch (n) {
    case N_R_REBOOT:
      s_d.print(F("restarting"));
      s_d.display();
      delay(600);
      ESP.restart();
      return;
    case N_R_LOOK:
      arenaled::resetLook();
      s_d.print(F("look reset"));
      break;
    case N_R_HOMES:
      s_d.print(F("unpaired"));
      s_d.display();
      delay(600);
      arenanet::forgetHomes();       // redemarre de lui-meme dans la version Matter
      delay(1500);
      ESP.restart();
      return;
    case N_R_ALL:
      arenaled::resetAll();
      arenanet::resetNetwork();
      arenapeers::resetAll();
      s_d.print(F("erased"));
      s_d.display();
      delay(600);
      arenanet::forgetHomes();
      delay(1500);
      ESP.restart();
      return;
    default: break;
  }
  s_d.display();
  delay(900);
  s_confirm  = 0;
  s_holdFrom = 0;
  draw();
}

// ---------------------------------------------------------------------------
//  Entrees
// ---------------------------------------------------------------------------
// Un pas de 8 sur 0..255 : 32 crans d'un bout a l'autre, soit une pression
// maintenue de quelques secondes. Un pas de 1 en demanderait 255 et tout le
// monde croirait le bouton casse.
static void onStep(int8_t d) {
  poke();
  const uint8_t n = curNode();

  if (!s_edit) {                       // on parcourt la liste du niveau courant
    Frame& f = s_stack[s_depth];
    const uint8_t cnt = NODES[f.node].count;
    if (cnt) {
      int8_t c = (int8_t)f.cursor + d;
      while (c < 0) c += cnt;
      f.cursor = (uint8_t)(c % cnt);
    }
    draw();
    return;
  }

  switch (NODES[n].kind) {             // on modifie la valeur pointee
    case K_MODE: {
      int8_t i = (int8_t)wheelIndexOfCurrent() + d;
      while (i < 0) i += WHEEL_N;
      arenaled::setMode(WHEEL[i % WHEEL_N]);
      break;
    }
    case K_PCT: {
      int v = (int)valueOf(n) + d * 8;
      setValue(n, (uint8_t)constrain(v, 0, 255));
      break;
    }
    case K_BOOL:
      arenaled::setIncandescent(!arenaled::incandescent());
      break;
    case K_LINK: {
      int8_t v = (int8_t)arenapeers::link() + d;
      while (v < 0) v += 3;
      arenapeers::setLink((arenapeers::Link)(v % 3));
      break;
    }
    default: break;
  }
  draw();
}

// OK bref = entrer / valider. OK maintenu = revenir en arriere, et depuis la
// racine, eteindre l'ecran. Avec trois boutons il faut un geste pour sortir, et
// l'appui long est le seul disponible sans en ajouter un quatrieme.
static void onOk(bool longPress) {
  poke();
  const uint8_t n = curNode();

  if (longPress) {
    if (s_edit)        { s_edit = false; arenaled::save(); }
    else if (s_depth)  { s_depth--; }
    else               { sleepNow(); return; }
    draw();
    return;
  }

  if (s_edit) {                        // sortir d'un reglage, et le retenir
    s_edit = false;
    arenaled::save();
    draw();
    return;
  }

  if (NODES[n].kind == K_MENU && NODES[n].count && s_depth + 1 < 4) {
    s_depth++;
    s_stack[s_depth] = { n, 0 };
    draw();
    return;
  }
  if (NODES[n].kind == K_CONFIRM) {   // arme, mais n'execute rien
    s_confirm  = n;
    s_holdFrom = 0;
    draw();
    return;
  }
  if (NODES[n].kind == K_SLEEP) { sleepNow(); return; }
  if (NODES[n].kind == K_INFO)  { draw(); return; }   // rien a regler

  // Un basculement se fait d'un seul appui : entrer en reglage pour appuyer une
  // deuxieme fois serait deux gestes pour une seule decision.
  if (NODES[n].kind == K_BOOL) {
    arenaled::setIncandescent(!arenaled::incandescent());
    arenaled::save();
    draw();
    return;
  }
  s_edit = true;
  draw();
}

// ---------------------------------------------------------------------------
void begin() {
  Wire.begin(PIN_ARENA_OLED_SDA, PIN_ARENA_OLED_SCL);
  // On sonde avant de faire confiance au panneau : un mur sans ecran ne doit pas
  // passer sa boucle a parler dans le vide, et le begin() d'Adafruit repond
  // volontiers vrai sur des clones absents.
  Wire.beginTransmission(ARENA_OLED_ADDR);
  s_found = (Wire.endTransmission() == 0) &&
            s_d.begin(SSD1306_SWITCHCAPVCC, ARENA_OLED_ADDR);
  if (!s_found) { Serial.println("[oled] no panel - screen disabled"); return; }

  pinMode(PIN_ARENA_BTN_UP,   INPUT_PULLUP);
  pinMode(PIN_ARENA_BTN_DOWN, INPUT_PULLUP);
  pinMode(PIN_ARENA_BTN_OK,   INPUT_PULLUP);

#if ARENA_ENC_ENABLE
  pinMode(PIN_ARENA_ENC_A, INPUT_PULLUP);
  pinMode(PIN_ARENA_ENC_B, INPUT_PULLUP);
  s_encPrev = (uint8_t)((digitalRead(PIN_ARENA_ENC_A) << 1) | digitalRead(PIN_ARENA_ENC_B));
  attachInterrupt(digitalPinToInterrupt(PIN_ARENA_ENC_A), encIsr, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_ARENA_ENC_B), encIsr, CHANGE);
#endif

  s_d.setRotation(0);
  Serial.printf("[oled] SSD1306 %dx%d SDA%d/SCL%d - up=%d down=%d ok=%d%s\n",
                ARENA_OLED_W, ARENA_OLED_H, PIN_ARENA_OLED_SDA, PIN_ARENA_OLED_SCL,
                PIN_ARENA_BTN_UP, PIN_ARENA_BTN_DOWN, PIN_ARENA_BTN_OK,
                ARENA_ENC_ENABLE ? " (+ encodeur)" : "");
  if (ARENA_OLED_BOOT_QR) showQr();
  else                    poke();
}

bool found() { return s_found; }

// Un poussoir maintenu doit repeter, sinon regler la luminosite de 0 a 100
// demanderait trente-deux appuis distincts. Premiere repetition apres une pause
// franche, pour qu'un appui simple reste un appui simple.
struct Btn { uint8_t pin; uint32_t downAt; uint32_t nextRep; bool wasDown; };

static bool pollRepeat(Btn& b, uint32_t now) {
  const bool down = digitalRead(b.pin) == LOW;
  bool fire = false;
  if (down && !b.wasDown) {
    if (now - b.downAt > 25) { fire = true; b.nextRep = now + 500; }   // anti-rebond
    b.downAt = now;
  } else if (down && (int32_t)(now - b.nextRep) >= 0) {
    fire = true;
    b.nextRep = now + 120;
  }
  b.wasDown = down;
  return fire;
}

void tick() {
  if (!s_found) return;
  const uint32_t now = millis();

  // Pendant une confirmation, les boutons ne veulent plus dire la meme chose :
  // OK se maintient, et TOUTE fleche annule. On court-circuite donc la gestion
  // normale plutot que de la faire cohabiter, ou un pas de menu passerait sous
  // l'ecran de confirmation sans qu'on le voie.
  if (s_confirm) {
    s_lastIn = now;                            // ne pas s'endormir en pleine decision
    encTake();
    if (digitalRead(PIN_ARENA_BTN_UP) == LOW || digitalRead(PIN_ARENA_BTN_DOWN) == LOW) {
      s_confirm = 0; s_holdFrom = 0; draw();
      return;
    }
    if (digitalRead(PIN_ARENA_BTN_OK) == LOW) {
      if (!s_holdFrom) s_holdFrom = now;
      if (now - s_holdFrom >= CONFIRM_MS) { runReset(s_confirm); return; }
      drawConfirm(now - s_holdFrom);
    } else if (s_holdFrom) {                   // relache trop tot = on renonce
      s_confirm = 0; s_holdFrom = 0; draw();
    } else {
      drawConfirm(0);
    }
    return;
  }

  const int8_t d = encTake();
  if (d) onStep(d > 0 ? 1 : -1);

  static Btn up   = { PIN_ARENA_BTN_UP,   0, 0, false };
  static Btn down = { PIN_ARENA_BTN_DOWN, 0, 0, false };
  // Trace de brochage : un appui dit quelle GPIO a repondu et quel role elle
  // porte. Sans elle, un poussoir mal nommee ne se diagnostique qu'a tatons,
  // parce que le seul retour est un menu qui bouge dans le mauvais sens.
  if (pollRepeat(up,   now)) { Serial.printf("[btn] GPIO%d -> UP/gauche\n",  PIN_ARENA_BTN_UP);   onStep(-1); }
  if (pollRepeat(down, now)) { Serial.printf("[btn] GPIO%d -> DOWN/droite\n", PIN_ARENA_BTN_DOWN); onStep(+1); }

  // OK, anti-rebond, avec un appui long pour "revenir en arriere".
  static uint32_t okAt  = 0;
  static bool     okLong = false;
  const bool ok = digitalRead(PIN_ARENA_BTN_OK) == LOW;
  if (ok && !okAt) { okAt = now; okLong = false; Serial.printf("[btn] GPIO%d -> OK\n", PIN_ARENA_BTN_OK); }
  else if (ok && !okLong && now - okAt > 700) { onOk(true); okLong = true; }
  else if (!ok && okAt) {
    if (!okLong && now - okAt > 25) onOk(false);
    okAt = 0;
  }

  if (!s_awake) return;

  // Repeindre lentement pendant la veille active, pour qu'un changement venu du
  // web ou de Siri se voie ici aussi : l'ecran ne doit pas afficher un mode que
  // le mur a quitte depuis dix secondes.
  static uint32_t lastDraw = 0;
  if (now - lastDraw > 400) { lastDraw = now; draw(); }

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
