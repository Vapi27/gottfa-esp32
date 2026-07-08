// pair_scan.cpp — MATRICE D'INTERACTIONS mesuree : pour chaque paire (victime A longue/soutenue,
// voleur B one-shot qui couvre les canaux de A), la ROM tranche ELLE-MEME : on joue A, on envoie B,
// et on sonde apres la fin naturelle de B si A sonne encore. A mort = B le REMPLACE (kill) ;
// A vivant = B se superpose/est ignore (suspension). Zero heuristique : c'est la verite du programme.
// Sortie : lignes JSONL {"kills":"<A>","by":"<B>"} a CONCATENER au sounds.sig du jeu.
// Usage : pair_scan <gen> <yrom> <drom> <sigs.jsonl>
#include "psorom.h"
#include "chefv2.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
static std::vector<uint8_t> load(const char* p){ FILE* f=fopen(p,"rb"); if(!f) return {}; fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET); std::vector<uint8_t> b(n); if(fread(b.data(),1,n,f)!=(size_t)n) b.clear(); fclose(f); return b; }
static int jint(const char* s, const char* k){ char pat[32]; snprintf(pat,sizeof(pat),"\"%s\":",k); const char* p=strstr(s,pat); return p?atoi(p+strlen(pat)):0; }

struct S { int id; uint16_t want; uint32_t durMs; bool sus; const char* cmdStr; char buf[16]; };
static psorom::Board g_board; static std::vector<uint8_t> g_r1, g_r2; static uint32_t g_clkMs;

// joue la sequence et retourne l'etat d'activite ROM (tenu || evenement < 300 ms) a l'instant probeMs
static bool activeAt(int a, int hdrA, int b, int hdrB, uint32_t bAtMs, uint32_t probeMs) {
  psorom::begin(g_board, g_r1.data(), g_r1.size(), g_r2.data(), g_r2.size());
  psorom::command(0); psorom::run(20000);
  psorom::liveEvents(true);
  { psorom::Ev t[256]; while (psorom::liveDrain(t, 256) > 0) {} }
  const uint32_t t0 = psorom::clockNow();
  uint8_t vol[2][3] = {{0}}; uint8_t ymKey = 0; uint32_t lastAct = 0;
  bool sentA = false, sentAh = false, sentB = false, sentBh = false;
  psorom::Ev ev[512];
  const uint32_t W = 9500;                              // demarre apres le jingle de reset
  while (psorom::clockNow() - t0 < (probeMs + 400) * g_clkMs) {
    uint32_t el = (psorom::clockNow() - t0) / g_clkMs;
    if (!sentAh && hdrA && el >= W - 200) { psorom::command((uint8_t)hdrA); sentAh = true; }
    if (!sentA && el >= W) { psorom::command((uint8_t)a); sentA = true; }
    if (!sentBh && hdrB && el >= W + bAtMs - 200) { psorom::command((uint8_t)hdrB); sentBh = true; }
    if (!sentB && el >= W + bAtMs) { psorom::command((uint8_t)b); sentB = true; }
    psorom::run(8000);
    int n;
    while ((n = psorom::liveDrain(ev, 512)) > 0)
      for (int i = 0; i < n; i++) { const psorom::Ev& e = ev[i]; bool on = false;
        if (e.ty <= 1 && e.a >= 8 && e.a <= 10) { uint8_t v = e.b & 0x1F; vol[e.ty][e.a - 8] = v; if (v) on = true; }
        else if (e.ty == 2 || e.ty == 3) on = true;
        else if (e.ty == 4) { uint8_t c = e.b & 7; if (e.b & 0x78) { ymKey |= 1u << c; on = true; } else ymKey &= (uint8_t)~(1u << c); }
        if (on) lastAct = e.t; }
  }
  bool held = ymKey != 0;
  for (int c = 0; c < 2; c++) for (int ch = 0; ch < 3; ch++) if (vol[c][ch]) held = true;
  uint32_t now = psorom::clockNow();
  return held || (lastAct && now - lastAct < 300u * g_clkMs);
}

int main(int argc, char** argv) {
  if (argc < 5) { printf("usage: %s <gen> <yrom> <drom> <sigs.jsonl>\n", argv[0]); return 2; }
  g_board = (argv[1][0]=='1') ? psorom::GTS80B_GEN1 : (argv[1][0]=='2') ? psorom::GTS80B_GEN2 : psorom::GTS80B_GEN3;
  g_clkMs = (g_board == psorom::GTS80B_GEN1) ? 1000 : 2000;
  g_r1 = load(argv[2]); g_r2 = load(argv[3]);
  std::vector<S> all;
  { FILE* f = fopen(argv[4], "r"); if (!f) { fprintf(stderr, "sigs KO\n"); return 1; } char ln[512];
    while (fgets(ln, sizeof(ln), f)) { const char* c = strstr(ln, "\"cmd\":\""); if (!c) continue;
      S s2; int h = atoi(c + 7); const char* dot = strchr(c + 7, '.');
      s2.id = dot ? (((~h) & 0x1F) * 32 + (atoi(dot + 1) & 0x1F)) : h;
      { const char* e = strchr(c + 7, '"'); size_t L = e - (c + 7); if (L > 15) L = 15; memcpy(s2.buf, c + 7, L); s2.buf[L] = 0; }
      int ay0 = jint(ln,"ay0"), ay1 = jint(ln,"ay1"), dac = jint(ln,"dac"), sp = jint(ln,"sp"), ym = jint(ln,"ym");
      s2.want = (uint16_t)(ay0 | (ay1 << 3) | (dac ? 1u<<6 : 0) | (sp ? 1u<<7 : 0) | ((uint16_t)ym << 8));
      s2.durMs = (uint32_t)jint(ln,"durMs");
      { const char* p = strstr(ln, "\"sustained\":"); s2.sus = p && !strncmp(p+12,"true",4); }
      if (s2.want) all.push_back(s2); }
    fclose(f); }
  auto hdrOf = [](int id) { return id < 32 ? 0 : (id < 64 ? 30 : 29); };
  auto valOf = [](int id) { return id & 31; };
  int n = 0;
  for (auto& A : all) {
    if (!(A.sus || A.durMs > 3000)) continue;                          // victimes : assez longues pour etre sondables
    for (auto& B : all) {
      if (B.id == A.id || B.sus || B.durMs < 400) continue;            // voleurs : one-shots non-blips
      if ((B.want & A.want) != A.want) continue;                       // ... qui COUVRENT les canaux de A (cas ambigu)
      uint32_t probe = 2500 + B.durMs + 1200;                          // apres la fin naturelle de B
      if (!A.sus && probe > A.durMs - 800) continue;                   // A (one-shot) doit encore jouer a la sonde
      bool solo = activeAt(valOf(A.id), hdrOf(A.id), 0, 0, 0, probe);  // A seul, actif a la sonde ?
      if (!solo) continue;                                             // pas sondable (repos interne) -> defaut suspension
      bool duo = activeAt(valOf(A.id), hdrOf(A.id), valOf(B.id), hdrOf(B.id), 2500, probe);
      if (!duo) { printf("{\"kills\":\"%s\",\"by\":\"%s\"}\n", A.buf, B.buf); fflush(stdout); }
      n++;
    }
  }
  fprintf(stderr, "pair_scan: %d paires sondees\n", n);
  return 0;
}
