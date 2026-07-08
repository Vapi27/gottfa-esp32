// chefv2.cpp — voir chefv2.h. Regles de PROPRIETE de canal, calquees sur le fonctionnement du vrai
// board (mono-DAC, canaux AY partages, voix YM2151 partagees, SP0250 unique) :
//   COMMANDE id   -> START id ; id REVENDIQUE les ressources de sa signature ; un son qui perd sa
//                    derniere ressource est STOPpe (vol de canal)... SAUF s'il est soutenu et que le
//                    voleur est un one-shot : il est alors SUSPENDU (son WAV continue, l'effet se
//                    mixe par-dessus) et HERITE du canal a la mort du voleur — c'est la ROM qui
//                    decide ensuite de sa vraie fin (silence) ou de sa survie (le flux reprend).
//   vol=0 / KOFF  -> le canal du proprietaire s'eteint (AY : ecriture volume 0 ; YM2151 : key-off) ;
//                    toutes ressources muettes au-dela de sa pause vraiment-silencieuse max MESUREE
//                    = STOP exact.
//   one-shot      -> plafond dur a 1,25x sa duree MESUREE (le DAC mono partage rend le flux ambigu :
//                    les percus d'une musique ne doivent pas maintenir un effet en vie).
// (C) 2026 Valere Pilpil / Pstore.
#include "chefv2.h"
#include <string.h>

namespace chefv2 {

static const Sig* s_sigs = nullptr; static int s_nSig = 0;
static uint32_t s_clkMs = 1000;

// ressources : bits 0-2 ay0, 3-5 ay1, 6 dac, 7 sp, 8-15 voix YM2151
struct Owner {                       // etat d'un son actif
  uint8_t  id, active;
  uint16_t res;                      // ressources possedees
  uint16_t want;                     // ressources de la SIGNATURE (res<want = vole partiel : preuve incomplete)
  uint8_t  liveAy;                   // canaux AY possedes ACTUELLEMENT sonores (vol>0), bits 0-5
  uint8_t  liveYm;                   // voix YM possedees actuellement keyees (KON), bits 0-7
  uint8_t  everAct;                  // a deja montre de l'activite (avant : grace de demarrage)
  uint32_t lastAct;                  // derniere activite VRAIE d'une de SES ressources (clk)
  uint32_t grace;                    // pas de stop-silence avant cet instant (heritage de canal)
  uint32_t born;                     // clk de demarrage
  const Sig* sig;
};
static Owner s_own[8];
static int8_t s_ayOwn[2][3];         // proprietaire courant de chaque canal AY (-1 libre)
static int8_t s_ymOwn[8];            // proprietaire courant de chaque voix YM2151
static int8_t s_dacOwn, s_spOwn;
static int8_t s_ayPrev[2][3];        // proprietaire PRECEDENT (heritage : le canal revient au suspendu a la mort du voleur)
static int8_t s_ymPrev[8];
static int8_t s_dacPrev, s_spPrev;
static uint8_t s_vol[2][3];          // volumes AY courants (suivi)
static uint8_t s_keyYm = 0;          // cles YM actuellement tenues (etat puce, tous proprietaires confondus)
static Action s_act[32]; static uint8_t s_aH, s_aT;
static uint32_t s_clk;
static uint32_t s_lastCmd = 0;       // derniere commande bus recue (quelle qu'elle soit)
static uint32_t s_lastGlob = 0;      // derniere activite GLOBALE (tout canal confondu) : silence total = fin de tout
static uint8_t s_kill[96][12];       // [victime][voleur/8] : paires MESUREES (pair_scan) ou le voleur REMPLACE
static uint8_t s_keep[96][12];       // [victime][voleur/8] : paires MESUREES ou la victime SURVIT a un voleur SOUTENU (roar sous musique !)
static uint16_t s_keepTtl[96];       // duree de survie MESUREE sous un soutenu (ms) — 0 = pleine duree


static void emit(uint32_t clk, uint8_t op, uint8_t id) {
  uint8_t n = (uint8_t)((s_aH + 1) & 31);
  if (n == s_aT) return;
  s_act[s_aH] = Action{ clk / s_clkMs, op, id }; s_aH = n;
}
static const Sig* find(uint8_t id) {
  for (int i = 0; i < s_nSig; i++) if (s_sigs[i].id == id) return &s_sigs[i];
  return nullptr;
}
static Owner* ownerOf(uint8_t id) {
  for (auto& o : s_own) if (o.active && o.id == id) return &o;
  return nullptr;
}
static bool sigWants(const Sig* g, uint16_t resBit) {          // la signature reclame-t-elle cette ressource ?
  if (resBit & 0x003F) { uint8_t b = (uint8_t)resBit; return ((g->ay0 | (g->ay1 << 3)) & b) != 0; }
  if (resBit == (1u << 6)) return g->dac != 0;
  if (resBit == (1u << 7)) return g->sp  != 0;
  return (g->ym & (resBit >> 8)) != 0;
}
static void inherit(int8_t& own, int8_t& prev, uint16_t resBit, uint8_t ayBit, uint8_t ymBit,
                    bool sounding, uint32_t clk) {       // mort du proprietaire -> le canal va a qui le RECLAME :
  int8_t p = prev; prev = -1;                            // d'abord l'heritier designe (prev), sinon RECHERCHE parmi
  Owner* po = (p >= 0) ? ownerOf((uint8_t)p) : nullptr;  // les vivants qui veulent ce canal sans le posseder
  if (!po) {                                             // (les chaines de vol a 3+ cassaient la chaine prev -> orphelins)
    Owner* best = nullptr;
    for (auto& w : s_own)
      if (w.active && !(w.res & resBit) && sigWants(w.sig, resBit)) {
        if (!best
            || (w.sig->sustained && !best->sig->sustained)                       // prefere un soutenu (musique)
            || (w.sig->sustained == best->sig->sustained && (int32_t)(w.born - best->born) > 0))   // puis le plus recent
          best = &w;
      }
    po = best;
  }
  if (po) { own = (int8_t)po->id;                              // grace (pas un faux lastAct !) : la ROM a une fenetre
            po->res |= resBit;                                 // de confirmation fraiche pour reprendre... ou se taire
            po->grace = clk + (po->sig->gapMs * 5 / 2 + 80) * s_clkMs;
            if (sounding) { po->liveAy |= ayBit; po->liveYm |= ymBit; } }   // note TENUE pendant le vol : elle sonne
  else own = -1;                                               // encore (la puce n'a jamais coupe) -> bit live reconstruit
}
static void stopOwner(Owner& o, uint32_t clk) {
  if (!o.active) return;
  o.active = 0;
  for (int c = 0; c < 2; c++) for (int ch = 0; ch < 3; ch++) {
    if (s_ayOwn[c][ch] == (int8_t)o.id) inherit(s_ayOwn[c][ch], s_ayPrev[c][ch], (uint16_t)(1u << (c * 3 + ch)),
                                                (uint8_t)(1u << (c * 3 + ch)), 0, s_vol[c][ch] != 0, clk);
    if (s_ayPrev[c][ch] == (int8_t)o.id) s_ayPrev[c][ch] = -1;
  }
  for (int v = 0; v < 8; v++) {
    if (s_ymOwn[v] == (int8_t)o.id) inherit(s_ymOwn[v], s_ymPrev[v], (uint16_t)(1u << (8 + v)),
                                            0, (uint8_t)(1u << v), (s_keyYm & (1u << v)) != 0, clk);
    if (s_ymPrev[v] == (int8_t)o.id) s_ymPrev[v] = -1;
  }
  if (s_dacOwn == (int8_t)o.id) inherit(s_dacOwn, s_dacPrev, 1u << 6, 0, 0, false, clk);
  if (s_spOwn  == (int8_t)o.id) inherit(s_spOwn,  s_spPrev,  1u << 7, 0, 0, false, clk);
  if (s_dacPrev == (int8_t)o.id) s_dacPrev = -1;
  if (s_spPrev  == (int8_t)o.id) s_spPrev  = -1;
  emit(clk, 1, o.id);
}
// revendication d'un canal par le voleur o/g : l'ancien proprietaire perd la ressource ;
// stop s'il n'a plus rien — sauf soutenu vole par un one-shot : SUSPENDU (heritage plus tard).
static void claim(int8_t& own, int8_t& prev, uint16_t resBit, uint8_t ayBit, uint8_t ymBit,
                  Owner* o, const Sig* g, uint32_t clk) {
  int8_t pv = own;
  own = (int8_t)o->id; o->res |= resBit;
  if (g->sustained)                                          // une MUSIQUE s'installe sur ce canal : tout suspendu qui
    for (auto& w : s_own)                                    // l'ATTENDAIT perd son droit d'y revenir (la vraie carte le
      if (w.active && w.id != o->id && !(w.res & resBit) && (w.want & resBit)
          && !((s_keep[w.id][o->id >> 3] >> (o->id & 7)) & 1)) {   // remplace)... SAUF paire "keeps" MESUREE
        w.want &= (uint16_t)~resBit;                               // (le roar SURVIT sous la musique sur la vraie ROM)
        if (!w.want && !w.res) stopOwner(w, clk);
      }
  if (pv >= 0 && pv != (int8_t)o->id) {
    Owner* po = ownerOf((uint8_t)pv);
    if (po) {
      po->res &= (uint16_t)~resBit;
      if (ayBit) po->liveAy &= (uint8_t)~ayBit;
      if (ymBit) po->liveYm &= (uint8_t)~ymBit;
      bool kills = (s_kill[po->id][o->id >> 3] >> (o->id & 7)) & 1;   // verites MESUREES des paires (pair_scan) :
      bool keeps = (s_keep[po->id][o->id >> 3] >> (o->id & 7)) & 1;   // kills = remplace ; keeps = la victime survit
      bool keep = keeps || (!g->sustained && !kills);         // defauts : un one-shot suspend, un soutenu remplace
      if (!po->res && !keep) stopOwner(*po, clk);
      if (po->active) prev = pv;                              // le vole survit (suspendu/partiel) -> il devient l'heritier ;
      return;                                                 // s'il MEURT, prev reste INCHANGE : un suspendu plus ancien
    }                                                         // y est peut-etre accroche (l'ecraser = orphelin eternel !)
  }
  prev = -1;
}

void begin(const Sig* sigs, int n, uint32_t clkPerMs) { s_sigs = sigs; s_nSig = n; s_clkMs = clkPerMs ? clkPerMs : 1000; reset(); }
void reset() {
  memset(s_own, 0, sizeof(s_own));
  memset(s_ayOwn, -1, sizeof(s_ayOwn));
  memset(s_ayPrev, -1, sizeof(s_ayPrev));
  memset(s_ymOwn, -1, sizeof(s_ymOwn));
  memset(s_ymPrev, -1, sizeof(s_ymPrev));
  memset(s_vol, 0, sizeof(s_vol));
  s_dacOwn = s_spOwn = s_dacPrev = s_spPrev = -1; s_aH = s_aT = 0; s_clk = 0; s_keyYm = 0; s_lastCmd = 0;
}

void command(uint8_t id, uint32_t clk) {
  s_clk = clk; s_lastCmd = clk;
  const Sig* g = find(id);
  if (!g || (!g->ay0 && !g->ay1 && !g->dac && !g->sp && !g->ym)) return;   // commande muette/controle : rien a jouer
  Owner* prev = ownerOf(id);
  if (prev && g->sustained) return;                               // re-commande d'une MUSIQUE active : la ROM l'IGNORE
                                                                  // (mesure : 30,30,30 == 30 a 2,7%) -> pas de coupure !
  if (prev) { emit(clk, 2, id); prev->res = 0; prev->active = 0;  // RE-declenchement one-shot = RESTART (roulette etc.)
              for (int c = 0; c < 2; c++) for (int ch = 0; ch < 3; ch++) {
                if (s_ayOwn[c][ch] == (int8_t)id) s_ayOwn[c][ch] = -1;
                if (s_ayPrev[c][ch] == (int8_t)id) s_ayPrev[c][ch] = -1; }
              for (int v = 0; v < 8; v++) {
                if (s_ymOwn[v] == (int8_t)id) s_ymOwn[v] = -1;
                if (s_ymPrev[v] == (int8_t)id) s_ymPrev[v] = -1; }
              if (s_dacOwn == (int8_t)id) s_dacOwn = -1; if (s_spOwn == (int8_t)id) s_spOwn = -1;
              if (s_dacPrev == (int8_t)id) s_dacPrev = -1; if (s_spPrev == (int8_t)id) s_spPrev = -1; }
  else emit(clk, 0, id);
  Owner* o = nullptr;                                            // place libre (ou la plus vieille)
  for (auto& w : s_own) if (!w.active) { o = &w; break; }
  if (!o) { Owner* old = &s_own[0]; for (auto& w : s_own) if (w.born < old->born) old = &w; stopOwner(*old, clk); o = old; }
  *o = Owner{ id, 1, 0, 0, 0, 0, 0, clk, 0, clk, g };
  o->want = (uint16_t)(g->ay0 | (g->ay1 << 3) | (g->dac ? 1u << 6 : 0) | (g->sp ? 1u << 7 : 0) | ((uint16_t)g->ym << 8));
  bool blip = !g->sustained && g->durMs < 400;                   // un BLIP ne vole pas : il se superpose
  for (int c = 0; c < 2; c++) {                                  // REVENDICATION des canaux de la signature
    uint8_t m = c ? g->ay1 : g->ay0;
    for (int ch = 0; ch < 3; ch++) if (m & (1u << ch)) {
      if (blip && s_ayOwn[c][ch] >= 0 && s_ayOwn[c][ch] != (int8_t)id) continue;
      claim(s_ayOwn[c][ch], s_ayPrev[c][ch], (uint16_t)(1u << (c * 3 + ch)), (uint8_t)(1u << (c * 3 + ch)), 0, o, g, clk); }
  }
  for (int v = 0; v < 8; v++) if (g->ym & (1u << v)) {
    if (blip && s_ymOwn[v] >= 0 && s_ymOwn[v] != (int8_t)id) continue;
    claim(s_ymOwn[v], s_ymPrev[v], (uint16_t)(1u << (8 + v)), 0, (uint8_t)(1u << v), o, g, clk); }
  if (g->dac && !(blip && s_dacOwn >= 0 && s_dacOwn != (int8_t)id)) claim(s_dacOwn, s_dacPrev, 1u << 6, 0, 0, o, g, clk);
  if (g->sp  && !(blip && s_spOwn  >= 0 && s_spOwn  != (int8_t)id)) claim(s_spOwn,  s_spPrev,  1u << 7, 0, 0, o, g, clk);
}

void feed(const psorom::Ev* ev, int n) {
  for (int i = 0; i < n; i++) {
    const psorom::Ev& e = ev[i];
    if ((int32_t)(e.t - s_clk) > 0) s_clk = e.t;             // modulaire : wallclk wrappe (~72 min Gen1)
    switch (e.ty) {
      case 0: case 1: {
        int c = e.ty;
        if (e.a >= 8 && e.a <= 10) {
          int ch = e.a - 8; uint8_t v = e.b & 0x1F;
          if (v) s_lastGlob = e.t;                           // activite GLOBALE, meme sans proprietaire
          s_vol[c][ch] = v;
          int8_t ow = s_ayOwn[c][ch];
          if (ow >= 0) { Owner* o = ownerOf((uint8_t)ow);
            if (o) { uint8_t bit = (uint8_t)(1u << (c * 3 + ch));
                     if (v) { s_lastGlob = e.t; o->liveAy |= bit; o->lastAct = e.t; o->everAct = 1; }
                     else     o->liveAy &= (uint8_t)~bit; } }   // vol=0 ECRIT : extinction exacte du canal
        }
        break; }
      case 2: s_lastGlob = e.t;
              if (s_spOwn >= 0) { Owner* o = ownerOf((uint8_t)s_spOwn); if (o) { o->lastAct = e.t; o->everAct = 1; } } break;
      case 3: s_lastGlob = e.t;
              if (s_dacOwn >= 0) { Owner* o = ownerOf((uint8_t)s_dacOwn); if (o) { o->lastAct = e.t; o->everAct = 1; } } break;
      case 4: { uint8_t v = e.b & 7;                            // YM2151 KON : key-on/off exact par voix
        if (e.b & 0x78) { s_keyYm |= (uint8_t)(1u << v); s_lastGlob = e.t; } else s_keyYm &= (uint8_t)~(1u << v);
        int8_t ow = s_ymOwn[v];
        if (ow >= 0) { Owner* o = ownerOf((uint8_t)ow);
          if (o) { uint8_t bit = (uint8_t)(1u << v);
                   if (e.b & 0x78) { s_lastGlob = e.t; o->liveYm |= bit; o->lastAct = e.t; o->everAct = 1; }
                   else              o->liveYm &= (uint8_t)~bit; } }
        break; }
    }
  }
}

void tick(uint32_t clk) {
  if ((int32_t)(clk - s_clk) > 0) s_clk = clk;               // modulaire (wrap)
  {                                                          // BALAYAGE GLOBAL : silence TOTAL au-dela de la pire
    bool anyHeld = s_keyYm != 0;                             // pause mesuree des sons actifs = plus rien ne joue
    for (int c = 0; c < 2; c++) for (int ch = 0; ch < 3; ch++) if (s_vol[c][ch]) anyHeld = true;
    uint32_t maxGap = 0; bool any = false;
    for (auto& o : s_own) if (o.active) { any = true; if (o.sig->gapMs > maxGap) maxGap = o.sig->gapMs; }
    uint32_t ref = s_lastGlob;                               // silence compte depuis la derniere activite OU la
    if ((int32_t)(s_lastCmd - ref) > 0) ref = s_lastCmd;     // derniere commande (le nouveau-ne n'a pas encore parle !)
    if (any && !anyHeld && ref && s_clk - ref > (maxGap * 5 / 2 + 700) * s_clkMs)
      for (auto& o : s_own) if (o.active) stopOwner(o, s_clk);
  }
  for (auto& o : s_own) {
    if (!o.active) continue;
    uint32_t confirm = (o.sig->gapMs * 5 / 2 + 80) * s_clkMs;    // 2,5x la pause vraiment-silencieuse max MESUREE + 80 ms
    if (!o.sig->sustained &&                                     // plafond dur d'un one-shot : 1,25x sa duree MESUREE
        s_clk - o.born > o.sig->durMs * 5 / 4 * s_clkMs + confirm) { stopOwner(o, s_clk); continue; }
    if (o.res != o.want) {                                       // VOLE (totalement ou en partie) : preuve incomplete,
      if (!o.sig->sustained && s_keepTtl[o.id]                    // ...mais sa SURVIE sous un soutenu est MESUREE :
          && s_clk - o.born > (uint32_t)(s_keepTtl[o.id] + s_keepTtl[o.id] / 8) * s_clkMs)
        { stopOwner(o, s_clk); continue; }                       // coupe a sa vraie fin (le roar = 6,5 s sous musique,
      if (s_clk - o.lastAct > 30000u * s_clkMs) stopOwner(o, s_clk);   // pas 13 !) ; sinon filet 30 s sans activite
      continue;                                                  // (plafond one-shot + remplacement par un soutenu restent)
    }
    if (o.liveAy || o.liveYm) continue;                          // un canal AY (vol>0) ou une voix YM (KON) sonne -> vivant
    if (!o.everAct) {                                            // pas encore demarre : grace = latence MESUREE x4 + 200 ms
      uint32_t grace = (o.sig->onMs * 4 + 200) * s_clkMs;
      if (s_clk - o.born > grace) stopOwner(o, s_clk);           // jamais demarre -> commande avortee, on coupe le WAV
      continue;
    }
    // toutes ses ressources sont muettes : confirme au-dela de SA pause interne max (mesuree).
    // Pour un son SOUTENU, le silence seul est une preuve faible (reprise apres interruption =
    // pause de partition possible) ; le silence CONSECUTIF a une commande bus (le declencheur
    // reel d'un stop/changement) est une preuve forte -> fenetre normale. Sinon, fenetre elargie.
    if ((int32_t)(o.grace - s_clk) > 0) continue;                    // canal herite a l'instant : laisse la ROM trancher (modulaire)
    uint32_t conf2 = confirm;
    if (o.sig->sustained && (int32_t)(o.lastAct - (s_lastCmd + confirm)) > 0)   // aucune commande depuis le debut du silence (modulaire)
      conf2 = (o.sig->gapMs * 8 + 2500) * s_clkMs;
    if (s_clk - o.lastAct > conf2) stopOwner(o, s_clk);
  }
}

void clearKills() { memset(s_kill, 0, sizeof(s_kill)); memset(s_keep, 0, sizeof(s_keep)); memset(s_keepTtl, 0, sizeof(s_keepTtl)); }
void setKill(uint8_t victim, uint8_t thief) { if (victim < 96 && thief < 96) s_kill[victim][thief >> 3] |= (uint8_t)(1u << (thief & 7)); }
void setKeep(uint8_t victim, uint8_t thief, uint16_t ttlMs) {
  if (victim >= 96 || thief >= 96) return;
  s_keep[victim][thief >> 3] |= (uint8_t)(1u << (thief & 7));
  if (ttlMs > s_keepTtl[victim]) s_keepTtl[victim] = ttlMs;       // max sur les paires (prudent)
}

uint64_t aliveMask() {
  uint64_t m = 0;
  for (auto& o : s_own) if (o.active) m |= 1ULL << (o.id & 63);
  return m;
}

int drain(Action* out, int maxN) {
  int n = 0;
  while (n < maxN && s_aT != s_aH) { out[n++] = s_act[s_aT]; s_aT = (uint8_t)((s_aT + 1) & 31); }
  return n;
}

} // namespace chefv2
