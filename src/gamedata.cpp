// gamedata.cpp — see gamedata.h for why this table exists and where it comes from.
// (C) 2026 Valere Pilpil / Pstore. Original implementation.
#include "gamedata.h"
#include <LittleFS.h>
#include <ArduinoJson.h>

namespace gamedata {
namespace {

// The parsed document is kept alive for the whole session and queried in place. ArduinoJson
// copies every string into the document when deserialising from a File, so the const char*
// we hand out stay valid exactly as long as g_doc does — which is why nothing here returns
// a pointer into a local buffer. A title file is ~1.5 KB; one resident document is cheaper
// than 60-odd Strings and has no fragmentation cost.
JsonDocument g_doc;
bool g_ok    = false;
int  g_game  = -1;

const char* str(JsonVariantConst v) { const char* s = v.as<const char*>(); return s ? s : ""; }

// Selected language, and the "<key>_en" lookup that backs it. English is opt-IN: a file that
// carries no _en variant simply keeps its French prose rather than showing an empty string,
// because a missing sentence is worse than a sentence in the wrong language.
bool g_en = false;

const char* loc(JsonVariantConst o, const char* key) {
  if (g_en) {
    char k[24];
    snprintf(k, sizeof k, "%s_en", key);
    const char* s = o[k].as<const char*>();
    if (s && *s) return s;
  }
  const char* s = o[key].as<const char*>();
  return s ? s : "";
}

JsonObjectConst coil(uint8_t c) {
  if (!g_ok || c < 1 || c > 9) return JsonObjectConst();
  for (JsonObjectConst o : g_doc["c"].as<JsonArrayConst>())
    if ((o["n"] | 0) == (int)c) return o;
  return JsonObjectConst();
}

bool readInto(int game, JsonDocument& doc) {
  // Flat, not "/gd/NN.json": LittleFS does not create intermediate directories on open(),
  // so a subdirectory would need an mkdir the uploader cannot do through /fsup. Same shape
  // as the /coilsig-NN.json files next to them.
  char p[24];
  snprintf(p, sizeof p, "/gd-%d.json", game);
  File f = LittleFS.open(p, "r");
  if (!f) return false;
  DeserializationError e = deserializeJson(doc, f);
  f.close();
  if (e) { Serial.printf("[gd] %s illisible (%s)\n", p, e.c_str()); return false; }
  return true;
}

} // anonymous namespace

// ---------------------------------------------------------------------------------

void unload() { g_doc.clear(); g_ok = false; g_game = -1; }

bool load(int game) {
  unload();
  if (game < 0 || game > 63) return false;
  if (!readInto(game, g_doc)) return false;

  // One level of alias, and one only: a variant that shares a playfield (speech vs
  // sound-only) points at the real table. Following further would let two files point at
  // each other and hang the boot, so a chain is treated as a malformed file.
  int alias = g_doc["alias"] | -1;
  if (alias >= 0) {
    if (alias == game) { Serial.printf("[gd] %d: alias sur lui-même\n", game); unload(); return false; }
    JsonDocument d2;
    if (!readInto(alias, d2)) {
      Serial.printf("[gd] %d: alias vers %d introuvable\n", game, alias);
      unload(); return false;
    }
    if ((d2["alias"] | -1) >= 0) {
      Serial.printf("[gd] %d: chaîne d'alias refusée\n", game);
      unload(); return false;
    }
    g_doc = d2;
  }
  g_ok   = true;
  g_game = game;
  Serial.printf("[gd] jeu %d = %s (source: conf=%u)\n", game, title(), (unsigned)conf());
  return true;
}

void setLang(const char* two) {
  if (!two) return;
  if (!strncmp(two, "en", 2)) g_en = true;
  else if (!strncmp(two, "fr", 2)) g_en = false;
}
const char* lang()   { return g_en ? "en" : "fr"; }

int         loaded() { return g_ok ? g_game : -1; }
const char* title()  { return g_ok ? str(g_doc["t"])           : ""; }
const char* source() { return g_ok ? loc(g_doc, "src")         : ""; }
const char* note()   { return g_ok ? loc(g_doc, "note")        : ""; }
uint8_t     conf()   { return g_ok ? (uint8_t)(g_doc["conf"] | 0) : 0; }

uint8_t bitOfId(int id) {
  int s = id / 10, r = id % 10;
  if (id < 0 || s > 7 || r > 7) return 0xFF;
  return (uint8_t)(s * 8 + r);
}
uint8_t idOfBit(uint8_t bit) { return (uint8_t)((bit >> 3) * 10 + (bit & 7)); }

const char* swName(uint8_t id) {
  if (!g_ok) return "";
  char k[4]; snprintf(k, sizeof k, "%u", (unsigned)id);
  return str(g_doc["sw"][k]);
}

// "f" is the manual's own SOLENOID ASSIGNMENTS wording and is deliberately NOT localised.
const char* coilFn(uint8_t c)     { JsonObjectConst o = coil(c); return o.isNull() ? "" : str(o["f"]); }
const char* coilWhyNot(uint8_t c) { JsonObjectConst o = coil(c); return o.isNull() ? "" : loc(o, "x"); }
const char* coilPre(uint8_t c)    { JsonObjectConst o = coil(c); return o.isNull() ? "" : loc(o, "p"); }

uint64_t coilSw(uint8_t c) {
  JsonObjectConst o = coil(c);
  if (o.isNull()) return 0;
  uint64_t m = 0;
  for (JsonVariantConst v : o["s"].as<JsonArrayConst>()) {
    uint8_t b = bitOfId(v | -1);
    if (b != 0xFF) m |= (1ULL << b);
  }
  return m;
}
bool hasExpectation(uint8_t c) { return coilSw(c) != 0; }

const char* lampName(uint8_t n) {
  if (!g_ok) return "";
  char k[4]; snprintf(k, sizeof k, "%u", (unsigned)n);
  return str(g_doc["lp"][k]);
}

bool lampIsCoil(uint8_t n) {
  if (!g_ok) return false;
  for (JsonVariantConst v : g_doc["lpc"].as<JsonArrayConst>())
    if ((v | -1) == (int)n) return true;
  return false;
}

String namesJson() {
  JsonDocument d;
  d["t"] = "names";
  JsonObject sw = d["sw"].to<JsonObject>();
  JsonObject lp = d["lamp"].to<JsonObject>();
  JsonObject co = d["coil"].to<JsonObject>();
  if (g_ok) {
    for (JsonPairConst p : g_doc["sw"].as<JsonObjectConst>()) sw[p.key()] = p.value();
    // LAMP INDEXING -- measured, not assumed.
    // The UI's squares are indexed from zero and merely LABELLED i+1. The first version of
    // this wrote the manual's L(n) to key n-1, on the assumption that L1 was the first lamp.
    // That was wrong: on the real machine (Volcano, 2026-07-30) the square labelled 13 --
    // i.e. bit index 12 -- fires the Motor Relay, which the manual calls L12. So the bit
    // index IS the manual's L number, and Gottlieb numbers these outputs from ZERO, which
    // is also why the Arena manual has an L0 at all.
    // An off-by-one here puts every lamp name on its neighbour: plausible-looking and
    // entirely wrong, which is the worst kind of defect this project can ship.
    for (JsonPairConst p : g_doc["lp"].as<JsonObjectConst>()) {
      int n = atoi(p.key().c_str());
      if (n < 0 || n > 47) continue;
      char k[4]; snprintf(k, sizeof k, "%d", n);
      String nm = p.value().as<const char*>() ? p.value().as<const char*>() : "";
      if (lampIsCoil((uint8_t)n)) nm = "⚡ " + nm;   // drives a mechanism, not a bulb
      lp[k] = nm;
    }
    for (JsonObjectConst o : g_doc["c"].as<JsonArrayConst>()) {
      int n = o["n"] | 0;
      if (n < 1 || n > 9) continue;
      char k[3]; snprintf(k, sizeof k, "%d", n);
      co[k] = str(o["f"]);
    }
  }
  String s; serializeJson(d, s); return s;
}

String describe(uint64_t mask) {
  String s;
  for (uint8_t b = 0; b < 64; b++) {
    if (!(mask & (1ULL << b))) continue;
    if (s.length()) s += ", ";
    uint8_t id = idOfBit(b);
    s += String((int)id);
    const char* n = swName(id);
    if (n[0]) { s += ' '; s += n; }
  }
  return s;
}

} // namespace gamedata
