// keeps_scan.cpp — l'autre moitie de la matrice d'interactions : pour chaque (victime one-shot
// LONGUE, voleur SOUTENU = musique), la ROM tranche par L'AUDIO : on rend [victime puis musique]
// et [musique seule], et on compare le RMS par fenetres apres l'arrivee de la musique. Si le duo
// est nettement plus fort (la victime continue de sonner dessous), la paire est "keeps" : la
// victime SURVIT (ex. Arena : le rugissement 26 continue sous la musique 14 du show d'attract).
// Sortie : lignes JSONL {"keeps":"<victime>","under":"<voleur>"} a concatener au sounds.sig.
// Usage : keeps_scan <gen> <yrom> <drom> <sigs.jsonl>
#include "psorom.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
static std::vector<uint8_t> load(const char* p){ FILE* f=fopen(p,"rb"); if(!f) return {}; fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET); std::vector<uint8_t> b(n); if(fread(b.data(),1,n,f)!=(size_t)n) b.clear(); fclose(f); return b; }
static int jint(const char* s, const char* k){ char pat[32]; snprintf(pat,sizeof(pat),"\"%s\":",k); const char* p=strstr(s,pat); return p?atoi(p+strlen(pat)):0; }

static psorom::Board g_board; static std::vector<uint8_t> g_r1, g_r2;
struct S { int id; uint32_t durMs; bool sus; char buf[16]; };

// rend la sequence (cmdA optionnel a tA, cmdB a tB) et retourne le RMS par fenetres de 500 ms
static std::vector<double> renderRms(int a, int hdrA, uint32_t tA, int b, int hdrB, uint32_t tB, double sec) {
  psorom::setSynth(true);
  psorom::begin(g_board, g_r1.data(), g_r1.size(), g_r2.data(), g_r2.size());
  psorom::command(0); psorom::run(40000);
  int fs = psorom::ayFs(); size_t total = (size_t)(sec * fs);
  std::vector<int16_t> out; out.reserve(total);
  bool sA = !a, sAh = !hdrA, sB = !b, sBh = !hdrB;
  while (out.size() < total) {
    uint32_t ms = (uint32_t)(out.size() * 1000.0 / fs);
    if (!sAh && ms >= tA - 200) { psorom::command((uint8_t)hdrA); sAh = true; }
    if (!sA && ms >= tA) { psorom::command((uint8_t)a); sA = true; }
    if (!sBh && ms >= tB - 200) { psorom::command((uint8_t)hdrB); sBh = true; }
    if (!sB && ms >= tB) { psorom::command((uint8_t)b); sB = true; }
    int16_t buf[256]; int n = psorom::renderMix(buf, 256);
    if (n <= 0) break;
    out.insert(out.end(), buf, buf + n);
  }
  std::vector<double> rms;
  size_t W = fs / 2;
  for (size_t s0 = 0; s0 + W <= out.size(); s0 += W) {
    double acc = 0; for (size_t k = s0; k < s0 + W; k += 4) acc += (double)out[k] * out[k];
    rms.push_back(sqrt(acc / (W / 4)));
  }
  return rms;
}

int main(int argc, char** argv) {
  if (argc < 5) { fprintf(stderr, "usage: %s <gen> <yrom> <drom> <sigs.jsonl>\n", argv[0]); return 2; }
  g_board = (argv[1][0]=='1') ? psorom::GTS80B_GEN1 : (argv[1][0]=='2') ? psorom::GTS80B_GEN2 : psorom::GTS80B_GEN3;
  g_r1 = load(argv[2]); g_r2 = load(argv[3]);
  std::vector<S> all;
  { FILE* f = fopen(argv[4], "r"); if (!f) return 1; char ln[512];
    while (fgets(ln, sizeof(ln), f)) { const char* c = strstr(ln, "\"cmd\":\""); if (!c) continue;
      S s2; int h = atoi(c + 7); const char* dot = strchr(c + 7, '.');
      s2.id = dot ? (((~h) & 0x1F) * 32 + (atoi(dot + 1) & 0x1F)) : h;
      { const char* e = strchr(c + 7, '"'); size_t L = e - (c + 7); if (L > 15) L = 15; memcpy(s2.buf, c + 7, L); s2.buf[L] = 0; }
      s2.durMs = (uint32_t)jint(ln, "durMs");
      { const char* p = strstr(ln, "\"sustained\":"); s2.sus = p && !strncmp(p+12,"true",4); }
      if (jint(ln,"ay0") || jint(ln,"ay1") || jint(ln,"dac") || jint(ln,"sp") || jint(ln,"ym")) all.push_back(s2); }
    fclose(f); }
  auto hdrOf = [](int id) { return id < 32 ? 0 : (id < 64 ? 30 : 29); };
  auto valOf = [](int id) { return id & 31; };
  int n = 0;
  for (auto& A : all) {
    if (A.sus || A.durMs < 1500) continue;                           // victimes : one-shots assez longs (le roar d'attract 25 = 2,2 s !)
    for (auto& B : all) {
      if (!B.sus) continue;                                          // voleurs : SOUTENUS (musiques)
      double sec = 1.1 + 16.0;                                       // fenetre d'analyse : musique a 1100 ms, +16 s (mesure la VRAIE survie)
      auto duo  = renderRms(valOf(A.id), hdrOf(A.id), 500, valOf(B.id), hdrOf(B.id), 1100, sec);
      auto solo = renderRms(0, 0, 0,                  valOf(B.id), hdrOf(B.id), 1100, sec);
      int early = 0, cmp = 0; size_t lastLoud = 0;                   // fenetres post-musique (a partir de 1,5 s)
      for (size_t k = 3; k < duo.size() && k < solo.size(); k++) {
        cmp++;
        if (duo[k] > solo[k] * 1.35) { if (k < 9) early++; lastLoud = k; }   // +2,6 dB = la victime sonne dessous
      }
      n++;
      if (cmp >= 6 && early >= 3) {                                  // survit au DEBUT (1,5-4,5 s) -> keeps...
        unsigned ttl = (unsigned)((lastLoud + 1) * 500 - 500);       // ...jusqu'a QUAND (mesure, depuis SON depart)
        printf("{\"keeps\":\"%s\",\"under\":\"%s\",\"ttlMs\":%u}\n", A.buf, B.buf, ttl); fflush(stdout);
      }
    }
  }
  fprintf(stderr, "keeps_scan: %d paires sondees\n", n);
  return 0;
}
