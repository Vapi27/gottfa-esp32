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
#include <Preferences.h>
#include "arena_oled.h"
#include "arenaled.h"
#include "arena_net.h"
#include "arena_peers.h"
#include "arena_qr.h"

namespace arenaoled {

static Adafruit_SSD1306 s_d(ARENA_OLED_W, ARENA_OLED_H, &Wire, -1);
static bool     s_found = false;
static bool     s_awake = false;

// Quel poussoir porte quel role : un fait de MONTAGE, pas de firmware.
// NETLIST.md dit BTN_LEFT=IO15, BTN_RIGHT=IO17, BTN_OK=IO7 - et sur la carte du
// banc, le poussoir physiquement a gauche repond sur IO7. Le document decrit des
// nets, pas des positions ; rien ne garantit que S1 soit le plus a gauche, et
// une carte du commerce ne promet rien du tout. En constante de compilation, le
// proprietaire d'une carte differente doit reflasher pour que sa fleche droite
// aille a droite - ce qui n'est pas un reglage, c'est une impasse.
// Regle a chaud, garde en NVS, defaut = ce que dit la netlist.
static Preferences s_bprefs;
static uint8_t  s_pinUp   = PIN_ARENA_BTN_UP;
static uint8_t  s_pinDown = PIN_ARENA_BTN_DOWN;
static uint8_t  s_pinOk   = PIN_ARENA_BTN_OK;

// Remappage depuis le reseau. Reecrit les broches, les remet en entree tiree au
// haut, et le dit - trois roles pour trois poussoirs, aucun ne peut rester muet.
void setButtons(uint8_t up, uint8_t down, uint8_t ok) {
  if (up > 48 || down > 48 || ok > 48) return;
  if (up == down || up == ok || down == ok) return;   // un poussoir, un role
  s_pinUp = up; s_pinDown = down; s_pinOk = ok;
  pinMode(s_pinUp,   INPUT_PULLUP);
  pinMode(s_pinDown, INPUT_PULLUP);
  pinMode(s_pinOk,   INPUT_PULLUP);
  s_bprefs.putUChar("up", up);
  s_bprefs.putUChar("down", down);
  s_bprefs.putUChar("ok", ok);
  Serial.printf("[btn] remappage : gauche=GPIO%u droite=GPIO%u ok=GPIO%u\n",
                up, down, ok);
}

// Faire tourner les trois roles d'un cran. Trois poussoirs mal ranges, c'est au
// pire deux clics - et surtout, cela ne demande de connaitre AUCUN numero de
// broche. Demander a quelqu'un de lire une netlist pour que sa fleche droite
// aille a droite, c'est lui demander de faire notre travail.
void rotateButtons() {
  setButtons(s_pinOk, s_pinUp, s_pinDown);
}

void buttons(uint8_t& up, uint8_t& down, uint8_t& ok) {
  up = s_pinUp; down = s_pinDown; ok = s_pinOk;
}
static uint32_t s_lastIn = 0;
static uint32_t s_nUp = 0, s_nOk = 0, s_nDown = 0;   // declenchements depuis le boot

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
    // Trente-deux pixels, et il faut y tenir onze chiffres LISIBLES.
    //
    // L'ancienne disposition n'y tenait pas : le nom en corps 1 sur la rangee 0,
    // puis deux lignes de corps 2 posees a y=10 et y=26. Or un caractere de
    // corps 2 fait SEIZE pixels de haut : la seconde ligne courait de 26 a 42
    // sur un panneau qui s'arrete a 32. Les dix derniers pixels de "2332"
    // n'existaient pas - et un code d'appairage ampute est un code inutilisable.
    //
    // Ici les deux lignes occupent exactement les deux moities : 0..15 et 16..31,
    // rien ne deborde. Le nom du mur reste - quand on appaire le troisieme d'une
    // serie, il faut voir devant lequel on se trouve, les murs partageant tous
    // le meme code - mais reduit a son suffixe, glisse dans la place libre a
    // droite de la seconde ligne. C'est le suffixe qui distingue, pas le
    // "Playfield-" que les quatre murs ont en commun.
    s_d.setTextSize(2);
    s_d.setCursor(4,  0);  s_d.print(F("3497-011"));
    s_d.setCursor(4, 16);  s_d.print(F("2332"));

    String tag = arenanet::wallName();
    const int dash = tag.lastIndexOf('-');
    if (dash >= 0) tag = tag.substring(dash + 1);
    if (tag.length() > 8) tag = tag.substring(tag.length() - 8);
    s_d.setTextSize(1);
    s_d.setCursor(ARENA_OLED_W - 6 * (int16_t)tag.length(), 22);
    s_d.print(tag);
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
// Un ecran noir a trois causes possibles - appui long, entree de menu, delai
// d'inactivite - et une quatrieme qui n'en est pas une : la carte qui redemarre.
// Vues du banc, les quatre se ressemblent. Chacune s'annonce donc.
// 700 ms suffisaient, et a la racine l'appui long ETEINT l'ecran : un doigt qui
// traine sur un poussoir raide fait donc le noir sans prevenir, ce qui se lit
// comme une carte qui plante. Un geste qui eteint doit etre franc.
static const uint32_t OK_LONG_MS = 1200;

static void sleepNowImpl();

static void sleepNow(const char* why) {
  if (!s_awake) return;                 // deja endormi : ni action, ni ligne
  Serial.printf("[oled] veille : %s\n", why);
  sleepNowImpl();
}

static void sleepNowImpl() {
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

void btnRaw(bool& up, bool& okd, bool& down,
            uint32_t& nUp, uint32_t& nOk, uint32_t& nDown) {
  up   = digitalRead(s_pinUp)   == LOW;
  okd  = digitalRead(s_pinOk)   == LOW;
  down = digitalRead(s_pinDown) == LOW;
  nUp = s_nUp; nOk = s_nOk; nDown = s_nDown;
}

// poke() est appele depuis DEUX taches : la boucle Arduino (boutons) et la
// tache du serveur web (/api/wake, et chaque reglage recu). Or il parlait en
// I2C - DISPLAYON puis draw() - donc deux taches pouvaient piloter le meme bus
// en meme temps que le draw() periodique de tick(). Un bus I2C partage sans
// verrou rend un ecran qui se brouille ou se fige, pas une erreur.
//
// Desormais poke() ne fait que poser deux mots en memoire ; tout l'I2C est
// execute par tick(), c est-a-dire toujours dans la meme tache.
static volatile bool s_wakeReq = false;

void poke() {
  s_lastIn  = millis();
  s_wakeReq = true;
}

// A n'appeler que depuis tick() (tache de la boucle Arduino).
static void serviceWake() {
  if (!s_wakeReq) return;
  s_wakeReq = false;
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
// Un redemarrage VOLONTAIRE et un PLANTAGE se ressemblent parfaitement de
// l'exterieur : ecran noir, puis le menu revient a 1/8 parce que l'etat est
// perdu dans les deux cas. La seule facon de les separer est que le chemin
// volontaire le dise avant de partir. Sans cette ligne, on cherche un bug de
// firmware la ou quelqu'un a simplement valide une entree de menu.
static const char* resetActionName(uint8_t n) {
  if (n == N_R_REBOOT) return "REDEMARRAGE demande par le menu";
  if (n == N_R_LOOK)   return "remise a zero de l APPARENCE (menu)";
  if (n == N_R_HOMES)  return "oubli des maisons Matter (menu)";
  return "remise a zero (menu)";
}

static void runReset(uint8_t n) {
  Serial.printf("[oled] %s\n", resetActionName(n));
  Serial.flush();
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
    else               { sleepNow("appui long OK a la racine"); return; }
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
  if (NODES[n].kind == K_SLEEP) { sleepNow("entree de menu Veille"); return; }
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

  s_bprefs.begin("arenabtn", false);
  s_pinUp   = s_bprefs.getUChar("up",   PIN_ARENA_BTN_UP);
  s_pinDown = s_bprefs.getUChar("down", PIN_ARENA_BTN_DOWN);
  s_pinOk   = s_bprefs.getUChar("ok",   PIN_ARENA_BTN_OK);
  pinMode(s_pinUp,   INPUT_PULLUP);
  pinMode(s_pinDown, INPUT_PULLUP);
  pinMode(s_pinOk,   INPUT_PULLUP);

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
                s_pinUp, s_pinDown, s_pinOk,
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
  serviceWake();
  const uint32_t now = millis();

  // Pendant une confirmation, les boutons ne veulent plus dire la meme chose :
  // OK se maintient, et TOUTE fleche annule. On court-circuite donc la gestion
  // normale plutot que de la faire cohabiter, ou un pas de menu passerait sous
  // l'ecran de confirmation sans qu'on le voie.
  if (s_confirm) {
    s_lastIn = now;                            // ne pas s'endormir en pleine decision
    encTake();
    if (digitalRead(s_pinUp) == LOW || digitalRead(s_pinDown) == LOW) {
      s_confirm = 0; s_holdFrom = 0; draw();
      return;
    }
    if (digitalRead(s_pinOk) == LOW) {
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

  // Les deux etats de repetition sont statiques : ils retiennent la broche du
  // PREMIER passage. Comme le brochage se regle a chaud, on la reecrit a chaque
  // tour, sinon un remappage ne prendrait qu'au redemarrage suivant.
  static Btn up   = { s_pinUp,   0, 0, false };
  static Btn down = { s_pinDown, 0, 0, false };
  up.pin   = s_pinUp;
  down.pin = s_pinDown;
  const bool upFire   = pollRepeat(up,   now);
  const bool downFire = pollRepeat(down, now);
  const bool ok       = digitalRead(s_pinOk) == LOW;

  // TOUTE entree repousse la veille, les fleches comprises.
  //
  // poke() est le seul endroit qui met s_lastIn a jour, et il n'etait appele
  // que par OK et par le demarrage. Naviguer aux fleches laissait donc le
  // compteur courir : au bout de ARENA_OLED_SLEEP_MS l'ecran s'eteignait EN
  // PLEINE navigation, et ne revenait qu'au clic suivant. Vu au banc.
  if (d || upFire || downFire || ok) {
    const bool wasAsleep = !s_awake;
    poke();
    serviceWake();                     // l'allumage doit se voir dans CE tick
    // Ecran eteint : le premier appui ne fait que rallumer. Sinon le menu se
    // deplace dans le noir et on decouvre un autre element en le rallumant -
    // ce qui se lit comme un bouton qui saute une ligne.
    if (wasAsleep) return;
  }

  if (d) onStep(d > 0 ? 1 : -1);

  // Trace de brochage : un appui dit quelle GPIO a repondu et quel role elle
  // porte. Sans elle, un poussoir mal nommee ne se diagnostique qu'a tatons,
  // parce que le seul retour est un menu qui bouge dans le mauvais sens.
  if (upFire)   { s_nUp++;   Serial.printf("[btn] GPIO%d -> UP/gauche\n",  s_pinUp);   onStep(-1); }
  if (downFire) { s_nDown++; Serial.printf("[btn] GPIO%d -> DOWN/droite\n", s_pinDown); onStep(+1); }

  // OK, anti-rebond, avec un appui long pour "revenir en arriere".
  static uint32_t okAt  = 0;
  static bool     okLong = false;
  if (ok && !okAt) { okAt = now; okLong = false; s_nOk++; Serial.printf("[btn] GPIO%d -> OK\n", s_pinOk); }
  else if (ok && !okLong && now - okAt > OK_LONG_MS) { onOk(true); okLong = true; }
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

  // ATTENTION a la soustraction. `now` est pris en HAUT de tick(), et poke() -
  // appele plus bas des qu'une entree arrive - ecrit s_lastIn = millis(), donc
  // une valeur POSTERIEURE a `now`. En arithmetique non signee, now - s_lastIn
  // vaut alors ~4 milliards, ce qui depasse tout seuil : l'ecran s'eteignait a
  // CHAQUE appui, et se rallumait au suivant pour s'eteindre aussitot. C'est le
  // "je clique et je perds l'ecran" du banc, et le log le montrait en boucle.
  // La difference signee rend un nombre NEGATIF dans ce cas, donc pas de veille.
  //
  // Et la ligne porte ses propres chiffres. Dire "delai ecoule" sans dire de
  // combien, c est demander de me croire sur parole - or cette veille a deja
  // survecu a une correction qui devait la regler. Avec le calcul affiche, le
  // log tranche tout seul : un delta de 30000 est un vrai delai, un delta de
  // 15 est un bug, et on n a plus a le deviner.
  const int32_t idle = (int32_t)(millis() - s_lastIn);
  if (ARENA_OLED_SLEEP_MS && idle > (int32_t)ARENA_OLED_SLEEP_MS) {
    char why[72];
    snprintf(why, sizeof(why), "inactif %ld ms (seuil %lu, lastIn=%lu, now=%lu)",
             (long)idle, (unsigned long)ARENA_OLED_SLEEP_MS,
             (unsigned long)s_lastIn, (unsigned long)millis());
    sleepNow(why);
  }
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
