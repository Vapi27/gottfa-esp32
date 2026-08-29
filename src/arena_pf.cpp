// arena_pf.cpp — see arena_pf.h.
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <string.h>
#include <ctype.h>
#include "arena_pf.h"

namespace arenapf {

static const char* PF_PATH   = "/arena_pf.json";     // the playfield (read-only)
// This wall's assignment lives in NVS, NOT on LittleFS. It used to be a file
// there, and pushing a new web UI wiped it: `/update?target=fs` rewrites the
// whole filesystem partition, so shipped data and the owner's own work shared a
// blast radius. NVS is a separate partition that neither OTA touches. The old
// file is still read once, so an install that predates this keeps its map.
static const char* LEDS_PATH = "/arena_leds.json";   // legacy, read-only migration
static Preferences s_prefs;
static const char* NVS_NS  = "arenapf";
static const char* NVS_KEY = "ledmap";
static const char* NVS_COL = "inscol";
static const char* NVS_NAM = "insnam";
static const char* NVS_HID = "inshid";

static Insert  s_ins[INSERT_MAX];
static uint8_t s_nIns = 0;
static uint8_t s_led[LED_MAX];                       // chain index -> insert index
static bool    s_any = false;
static Colour  s_col[INSERT_MAX];      // per-insert plastic colour, all-zero = unset
static char    s_nam[INSERT_MAX][NAME_LEN];  // owner's label, empty = use the shipped one
// Hidden from the plan. The eighteen general-illumination pads are classified
// from the ROM capture, but only the owner sees the real playfield: whatever we
// got wrong, they can put away themselves. Stored here rather than in the plan
// file so it survives a game-bundle upload, and on the board rather than in the
// browser so it is the same from every phone in the house.
static uint8_t s_hid[INSERT_MAX];      // 1 = masque

// Correction de luminosite, par PIXEL et non par insert, 255 = neutre.
//
// La teinte appartient a l'insert - c'est le plastique moule, elle doit survivre
// a un rerouteage de la chaine, et c'est deja ainsi. La luminosite, elle,
// appartient au pixel : deux SK6812 d'un meme lot ne rendent pas la meme chose,
// un insert large avale plus qu'un petit, et un plastique epais assombrit ce
// qu'il y a dessous. Aucune de ces trois causes ne suit le fil quand on le
// deplace ; toutes suivent la LED.
static const char* NVS_TRIM = "trim";
static uint8_t s_trim[LED_MAX];

uint8_t trimOf(uint16_t led)  { return (led < LED_MAX) ? s_trim[led] : 255; }

bool setTrim(uint16_t led, uint8_t v) {
  if (led >= LED_MAX) return false;
  s_trim[led] = v;
  return true;
}

void clearTrims() { memset(s_trim, 255, sizeof(s_trim)); }

bool saveTrims() {
  return s_prefs.putBytes(NVS_TRIM, s_trim, sizeof(s_trim)) == sizeof(s_trim);
}

String trimsJson() {
  String j = "{\"trims\":[";
  bool first = true;
  for (uint16_t i = 0; i < LED_MAX; i++) {
    if (s_trim[i] == 255) continue;          // le neutre ne s'ecrit pas
    if (!first) j += ',';
    j += "{\"i\":" + String(i) + ",\"t\":" + String(s_trim[i]) + "}";
    first = false;
  }
  j += "]}";
  return j;
}

uint8_t       insertCount()       { return s_nIns; }
const Insert* insert(uint8_t i)   { return (i < s_nIns) ? &s_ins[i] : nullptr; }
uint8_t       ledInsert(uint16_t led) { return (led < LED_MAX) ? s_led[led] : UNASSIGNED; }
bool          anyAssigned()       { return s_any; }

// Matches the label the owner sees first, then the shipped one, so /api/latch
// keeps working with either.
int indexOf(const char* name) {
  for (uint8_t i = 0; i < s_nIns; i++)
    if (s_nam[i][0] && strncmp(s_nam[i], name, NAME_LEN) == 0) return i;
  for (uint8_t i = 0; i < s_nIns; i++)
    if (strncmp(s_ins[i].name, name, NAME_LEN) == 0) return i;
  return -1;
}

static void recountAssigned() {
  s_any = false;
  for (uint16_t i = 0; i < LED_MAX && !s_any; i++) s_any = (s_led[i] != UNASSIGNED);
}

bool setLedInsert(uint16_t led, uint8_t ins) {
  if (led >= LED_MAX) return false;
  if (ins != UNASSIGNED && ins >= s_nIns) return false;
  s_led[led] = ins;
  if (ins != UNASSIGNED) s_any = true; else recountAssigned();
  return true;
}

void clearAssignment() {
  memset(s_led, UNASSIGNED, sizeof(s_led));
  s_any = false;
}

const char* nameOf(uint8_t ins) {
  if (ins >= s_nIns) return "";
  return s_nam[ins][0] ? s_nam[ins] : s_ins[ins].name;
}
bool setName(uint8_t ins, const char* name) {
  if (ins >= s_nIns) return false;
  strncpy(s_nam[ins], name ? name : "", NAME_LEN - 1);
  s_nam[ins][NAME_LEN - 1] = 0;
  return true;
}
bool saveNames() { return s_prefs.putBytes(NVS_NAM, s_nam, sizeof(s_nam)) == sizeof(s_nam); }

bool hidden(uint8_t ins)              { return ins < s_nIns && s_hid[ins]; }
bool setHidden(uint8_t ins, bool h)   { if (ins >= s_nIns) return false; s_hid[ins] = h ? 1 : 0; return true; }
bool saveHidden() { return s_prefs.putBytes(NVS_HID, s_hid, sizeof(s_hid)) == sizeof(s_hid); }

Colour colourOf(uint8_t ins) {
  return (ins < s_nIns) ? s_col[ins] : Colour{ 0, 0, 0, 0 };
}
Colour colourOfLed(uint16_t led) {
  const uint8_t a = ledInsert(led);
  return (a == UNASSIGNED) ? Colour{ 0, 0, 0, 0 } : colourOf(a);
}
bool setColour(uint8_t ins, Colour c) {
  if (ins >= s_nIns) return false;
  s_col[ins] = c;
  return true;
}
void clearColours() { memset(s_col, 0, sizeof(s_col)); }

bool saveColours() {
  return s_prefs.putBytes(NVS_COL, s_col, sizeof(s_col)) == sizeof(s_col);
}
// Only inserts that carry a colour, by the machine's own name: a fresh wall is
// two bytes, and the UI never has to know the table's internal indices.
String coloursJson() {
  String j = "{\"colours\":[";
  bool first = true;
  for (uint8_t i = 0; i < s_nIns; i++) {
    const Colour& c = s_col[i];
    if (!(c.r | c.g | c.b | c.w)) continue;
    if (!first) j += ',';
    first = false;
    j += "{\"n\":\"" + String(s_ins[i].name) + "\",\"i\":" + String(i) +
         ",\"r\":" + String(c.r) + ",\"g\":" + String(c.g) +
         ",\"b\":" + String(c.b) + ",\"w\":" + String(c.w) + "}";
  }
  j += "]}";
  return j;
}

int lampOfLed(uint16_t led) {
  const uint8_t a = ledInsert(led);
  return (a == UNASSIGNED || a >= s_nIns) ? -1 : s_ins[a].lamp;
}

bool xy(uint16_t led, float& x, float& y) {
  const uint8_t a = ledInsert(led);
  if (a == UNASSIGNED || a >= s_nIns) return false;
  x = s_ins[a].x;
  y = s_ins[a].y;
  return true;
}

// --- the fixed playfield table -------------------------------------------
// Streamed back to the browser verbatim rather than re-serialised: it is
// static data, and re-encoding 99 records would cost heap for nothing.
// Built from memory rather than streamed from the file, so the owner's renames
// and colours are part of the one description the UI reads. "d" is the shipped
// label, kept so the UI can show what a rename replaced.
String insertsJson() {
  String j = "{\"inserts\":[";
  for (uint8_t i = 0; i < s_nIns; i++) {
    if (i) j += ',';
    const Colour& c = s_col[i];
    j += "{\"n\":\"" + String(nameOf(i)) + "\",\"d\":\"" + String(s_ins[i].name) + "\"";
    if (s_ins[i].func[0]) j += ",\"f\":\"" + String(s_ins[i].func) + "\"";
    j += ",\"l\":" + String(s_ins[i].lamp) +
         ",\"k\":\"" + String(s_ins[i].kind) + "\"" +
         ",\"x\":" + String(s_ins[i].x, 4) + ",\"y\":" + String(s_ins[i].y, 4);
    if (c.r | c.g | c.b | c.w)
      j += ",\"c\":[" + String(c.r) + "," + String(c.g) + "," + String(c.b) + "," + String(c.w) + "]";
    if (s_hid[i]) j += ",\"h\":1";
    j += "}";
  }
  j += "]}";
  return j;
}

// --- une table d'inserts EDITABLE ------------------------------------------
//
// L'en-tete disait de cette table "it does not change: it is the machine", et
// c'etait vrai tant que la machine etait l'Arena livree avec le firmware. Pour
// un autre plateau - un Alien Poker - il n'y a ni table Visual Pinball ni ROM
// sous la main : il y a une photo, et quelqu'un qui sait ou sont ses inserts.
// Poser un point la ou l'on tape doit donc etre possible, sinon le mur ne peut
// exister que pour les machines que le depot connait deja.
//
// Le fichier reste la source : on l'ecrit, et loadInserts() le relit tel quel
// au demarrage suivant. Rien de nouveau a charger.
bool saveInserts() {
  File f = LittleFS.open(PF_PATH, "w");
  if (!f) return false;
  String j = insertsJson();
  const size_t w = f.print(j);
  f.close();
  return w == j.length();
}

int addInsert(float x, float y, const char* name) {
  if (s_nIns >= INSERT_MAX) return -1;
  Insert& it = s_ins[s_nIns];
  memset(&it, 0, sizeof(it));
  if (name && *name) { strncpy(it.name, name, NAME_LEN - 1); it.name[NAME_LEN - 1] = 0; }
  else               { snprintf(it.name, NAME_LEN, "P%u", (unsigned)(s_nIns + 1)); }
  it.func[0] = 0;
  it.kind = 'i';

  // ⚠️ UN NUMERO DE LAMPE, sinon le point ne pourra JAMAIS etre anime.
  //
  // Il valait -1, au motif qu'un point pose a la main ne vient d'aucune ROM.
  // C'est vrai de son origine, mais le numero de lampe n'est pas qu'une trace :
  // c'est l'ADRESSE par laquelle une sequence l'allume. Le lecteur d'attract fait
  // (masque >> lampe) & 1, et un insert a -1 est invisible pour lui - donc un
  // plateau entierement pose a la main, le cas d'un ELECTROMECANIQUE qui n'a ni
  // ROM ni table Visual Pinball, ne pouvait recevoir aucune animation.
  //
  // On lui donne le plus petit numero libre. Le masque etant un u64, il y a 64
  // adresses : au-dela le point existe et s'affiche, mais reste hors sequence -
  // et l'interface doit le dire plutot que de laisser croire a une panne.
  bool pris[64] = { false };
  for (uint8_t k = 0; k < s_nIns; k++)
    if (s_ins[k].lamp >= 0 && s_ins[k].lamp < 64) pris[s_ins[k].lamp] = true;
  it.lamp = -1;
  for (uint8_t k = 0; k < 64; k++) if (!pris[k]) { it.lamp = (int8_t)k; break; }
  it.x = (x < 0) ? 0 : (x > 1 ? 1 : x);
  it.y = (y < 0) ? 0 : (y > 1 ? 1 : y);
  s_col[s_nIns] = Colour{ 0, 0, 0, 0 };
  s_nam[s_nIns][0] = 0;
  s_hid[s_nIns] = 0;
  return (int)s_nIns++;
}

bool moveInsert(uint8_t ins, float x, float y) {
  if (ins >= s_nIns) return false;
  s_ins[ins].x = (x < 0) ? 0 : (x > 1 ? 1 : x);
  s_ins[ins].y = (y < 0) ? 0 : (y > 1 ? 1 : y);
  return true;
}

bool removeInsert(uint8_t ins) {
  if (ins >= s_nIns) return false;
  // Les pixels designent les inserts par INDICE. Retirer un insert sans
  // recaler ces indices ne casse rien de visible : chaque pixel pose apres lui
  // glisse d'un cran et se retrouve sur le voisin. Un plan qui a bouge tout
  // seul est plus couteux qu'un plan casse, parce qu'il reste credible.
  for (uint16_t k = 0; k < LED_MAX; k++) {
    if (s_led[k] == ins)                              s_led[k] = UNASSIGNED;
    else if (s_led[k] != UNASSIGNED && s_led[k] > ins) s_led[k]--;
  }
  for (uint8_t k = ins; k + 1 < s_nIns; k++) {
    s_ins[k] = s_ins[k + 1];
    s_col[k] = s_col[k + 1];
    s_hid[k] = s_hid[k + 1];
    memcpy(s_nam[k], s_nam[k + 1], NAME_LEN);
  }
  s_nIns--;
  s_any = false;
  for (uint16_t k = 0; k < LED_MAX; k++) if (s_led[k] != UNASSIGNED) { s_any = true; break; }
  return true;
}

void clearInserts() {
  s_nIns = 0;
  memset(s_led, UNASSIGNED, sizeof(s_led));
  memset(s_col, 0, sizeof(s_col));
  memset(s_hid, 0, sizeof(s_hid));
  memset(s_nam, 0, sizeof(s_nam));
  s_any = false;
}

static bool loadInserts() {
  s_nIns = 0;
  if (!LittleFS.exists(PF_PATH)) return false;
  File f = LittleFS.open(PF_PATH, "r");
  if (!f) return false;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return false;
  JsonArray arr = doc["inserts"].as<JsonArray>();
  if (arr.isNull()) return false;
  for (JsonObject o : arr) {
    if (s_nIns >= INSERT_MAX) break;
    Insert& it = s_ins[s_nIns];
    strncpy(it.name, o["n"] | "", NAME_LEN - 1);
    it.name[NAME_LEN - 1] = 0;
    const char* k = o["k"] | "i";
    it.kind = k[0];
    // Two numbers, and confusing them cost an evening. "l" is PinMAME's internal
    // lamp index, which is what the captured ROM sequence is indexed by. The
    // NAME carries what the machine calls that lamp (PinMAME + 8, per
    // GTS80_lamp2m), because that is what is written in the manual and on the
    // playfield — so it is what the plan must show and what the firmware must
    // never compute with.
    const int l = o["l"] | -1;
    it.lamp = (l >= 0 && l < 64) ? (int8_t)l : -1;
    strncpy(it.func, o["f"] | "", FUNC_LEN - 1);
    it.func[FUNC_LEN - 1] = 0;
    it.x = o["x"] | 0.0f;
    it.y = o["y"] | 0.0f;
    s_nIns++;
  }
  return s_nIns > 0;
}

// --- this wall's assignment ----------------------------------------------
// Only assigned pixels are written: a fresh wall is a two-byte file, and the
// common early state (six pixels placed out of 150) does not carry 144 nulls.
String toJson() {
  String j = "{\"leds\":[";
  bool first = true;
  for (uint16_t i = 0; i < LED_MAX; i++) {
    if (s_led[i] == UNASSIGNED) continue;
    if (!first) j += ',';
    first = false;
    j += "{\"i\":" + String(i) + ",\"a\":" + String(s_led[i]) + "}";
  }
  j += "]}";
  return j;
}

bool fromJson(const char* json) {
  JsonDocument doc;
  if (deserializeJson(doc, json)) return false;
  JsonArray arr = doc["leds"].as<JsonArray>();
  if (arr.isNull()) return false;
  // Build into a scratch copy: a half-applied assignment after a bad record
  // would leave the wall mapped to something nobody asked for.
  uint8_t tmp[LED_MAX];
  memset(tmp, UNASSIGNED, sizeof(tmp));
  for (JsonObject o : arr) {
    const int i = o["i"] | -1;
    const int a = o["a"] | -1;
    if (i < 0 || i >= LED_MAX) return false;
    if (a < 0 || (a >= s_nIns && a != UNASSIGNED)) return false;
    tmp[i] = (uint8_t)a;
  }
  memcpy(s_led, tmp, sizeof(s_led));
  recountAssigned();
  return true;
}

bool save() {
  return s_prefs.putBytes(NVS_KEY, s_led, sizeof(s_led)) == sizeof(s_led);
}

static bool loadAssignment() {
  if (s_prefs.getBytesLength(NVS_KEY) == sizeof(s_led)) {
    s_prefs.getBytes(NVS_KEY, s_led, sizeof(s_led));
    recountAssigned();
    return true;
  }
  // Migration: an install from before the move still has its map on LittleFS.
  // Read it once and write it where it belongs.
  if (!LittleFS.exists(LEDS_PATH)) return false;
  File f = LittleFS.open(LEDS_PATH, "r");
  if (!f) return false;
  String j = f.readString();
  f.close();
  if (!fromJson(j.c_str())) return false;
  save();
  Serial.println("[pf] migrated the insert map from LittleFS to NVS");
  return true;
}

void begin() {
  s_prefs.begin(NVS_NS, false);
  clearAssignment();
  clearColours();
  memset(s_nam, 0, sizeof(s_nam));
  memset(s_hid, 0, sizeof(s_hid));
  const bool haveTable = loadInserts();
  const bool haveMap   = haveTable && loadAssignment();
  if (s_prefs.getBytesLength(NVS_COL) == sizeof(s_col))
    s_prefs.getBytes(NVS_COL, s_col, sizeof(s_col));
  if (s_prefs.getBytesLength(NVS_NAM) == sizeof(s_nam))
    s_prefs.getBytes(NVS_NAM, s_nam, sizeof(s_nam));
  if (s_prefs.getBytesLength(NVS_HID) == sizeof(s_hid))
    s_prefs.getBytes(NVS_HID, s_hid, sizeof(s_hid));
  clearTrims();
  if (s_prefs.getBytesLength(NVS_TRIM) == sizeof(s_trim))
    s_prefs.getBytes(NVS_TRIM, s_trim, sizeof(s_trim));
  uint16_t n = 0;
  for (uint16_t i = 0; i < LED_MAX; i++) if (s_led[i] != UNASSIGNED) n++;
  Serial.printf("[pf] %u inserts%s, %u pixels placed\n",
                s_nIns, haveTable ? "" : " (MISSING /arena_pf.json)", n);
  (void)haveMap;
}

}  // namespace arenapf
