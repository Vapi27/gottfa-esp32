// host_chefsim.cpp — SIMULATEUR du chef v2 : rejoue une SEQUENCE de jeu (commandes horodatees) a
// travers le conducteur (vraie ROM) + le moteur de regles chefv2 (LE MEME code que le firmware) et
// imprime le journal d'actions WAV. C'est ici que les regles se PROUVENT avant de toucher la carte.
// Run: host_chefsim <gen> <yrom> <drom> <sigs.jsonl> "t1:cmd1 t2:cmd2 ..." <dureeSec>
//      (t en ms ; ex: "0:6 3000:18 5000:31")
#include "psorom.h"
#include "chefv2.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
static std::vector<uint8_t> load(const char* p){ FILE* f=fopen(p,"rb"); if(!f) return {}; fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET); std::vector<uint8_t> b(n); if(fread(b.data(),1,n,f)!=(size_t)n) b.clear(); fclose(f); return b; }

static int jint(const char* s, const char* k) {        // extrait "k":N d'une ligne json
  char pat[32]; snprintf(pat, sizeof(pat), "\"%s\":", k);
  const char* p = strstr(s, pat); return p ? atoi(p + strlen(pat)) : 0;
}
static bool jbool(const char* s, const char* k) {
  char pat[32]; snprintf(pat, sizeof(pat), "\"%s\":", k);
  const char* p = strstr(s, pat); return p && !strncmp(p + strlen(pat), "true", 4);
}

int main(int argc, char** argv) {
  if (argc < 7) { printf("usage: %s <gen> <yrom> <drom> <sigs.jsonl> \"t:cmd ...\" <sec>\n", argv[0]); return 2; }
  psorom::Board board = (argv[1][0]=='1') ? psorom::GTS80B_GEN1 : (argv[1][0]=='2') ? psorom::GTS80B_GEN2 : psorom::GTS80B_GEN3;
  auto r1 = load(argv[2]), r2 = load(argv[3]);
  // signatures
  static chefv2::Sig sigs[96]; int nSig = 0;
  { FILE* f = fopen(argv[4], "r"); if (!f) { printf("sigs KO\n"); return 1; }
    char ln[512];
    while (fgets(ln, sizeof(ln), f) && nSig < 96) {
      { const char* k = strstr(ln, "\"kills\":\"");
        if (k) { const char* by = strstr(ln, "\"by\":\"");
          if (by) { auto dec = [](const char* q) { int h = atoi(q); const char* d = strchr(q, '.');
                                                   return d ? (((~h) & 0x1F) * 32 + (atoi(d + 1) & 0x1F)) : h; };
                    chefv2::setKill((uint8_t)dec(k + 9), (uint8_t)dec(by + 6)); }
          continue; } }
      { const char* k = strstr(ln, "\"keeps\":\"");
        if (k) { const char* un = strstr(ln, "\"under\":\"");
          if (un) { auto dec = [](const char* q) { int h = atoi(q); const char* d = strchr(q, '.');
                                                   return d ? (((~h) & 0x1F) * 32 + (atoi(d + 1) & 0x1F)) : h; };
                    { int ttl = 0; const char* tt = strstr(ln, "\"ttlMs\":"); if (tt) ttl = atoi(tt + 8);
                      chefv2::setKeep((uint8_t)dec(k + 9), (uint8_t)dec(un + 9), (uint16_t)ttl); } }
          continue; } }
      if (!strstr(ln, "\"cmd\"")) continue;
      chefv2::Sig& g = sigs[nSig++];
      const char* c = strstr(ln, "\"cmd\":\"");
      { int h = atoi(c + 7); const char* dot = strchr(c + 7, '.');     // "h.v" (banque) -> id etendu
        g.id = (uint8_t)(dot ? (((~h) & 0x1F) * 32 + (atoi(dot + 1) & 0x1F)) : h); }
      g.ay0 = (uint8_t)jint(ln, "ay0"); g.ay1 = (uint8_t)jint(ln, "ay1");
      g.dac = (uint8_t)jint(ln, "dac"); g.sp  = (uint8_t)jint(ln, "sp");
      g.ym  = (uint8_t)jint(ln, "ym");
      g.sustained = jbool(ln, "sustained") ? 1 : 0;
      g.durMs = (uint32_t)jint(ln, "durMs"); g.gapMs = (uint32_t)jint(ln, "gapMs");
      g.onMs = (uint32_t)jint(ln, "onMs");
    }
    fclose(f); }
  // sequence : "t:cmd" simple, ou "t:h.v" (paire banque -> header a t, valeur a t+150 ms, id etendu)
  struct Cmd { uint32_t tMs; uint8_t id; int ext; };
  std::vector<Cmd> seq;
  { char* dup = strdup(argv[5]); char* tk = strtok(dup, " ");
    while (tk) { uint32_t t = (uint32_t)atoi(tk); const char* col = strchr(tk, ':');
                 int h = col ? atoi(col + 1) : 0; const char* dot = col ? strchr(col, '.') : nullptr;
                 if (dot) { int v = atoi(dot + 1);
                            seq.push_back(Cmd{ t, (uint8_t)h, -1 });          // header : ROM seule
                            seq.push_back(Cmd{ t + 150, (uint8_t)v, ((~h) & 0x1F) * 32 + (v & 0x1F) }); }
                 else seq.push_back(Cmd{ t, (uint8_t)h, h });
                 tk = strtok(nullptr, " "); } }
  double sec = atof(argv[6]);

  if (!psorom::begin(board, r1.data(), r1.size(), r2.data(), r2.size())) { printf("begin KO\n"); return 1; }
  psorom::command(0); psorom::run(20000);
  psorom::liveEvents(true);
  { psorom::Ev t[256]; while (psorom::liveDrain(t, 256) > 0) {} }
  const uint32_t clkMs = (board == psorom::GTS80B_GEN1) ? 1000 : 2000;
  chefv2::begin(sigs, nSig, clkMs);

  // UNE seule horloge : le wallclk de psorom (les compteurs separes derivent — quantum overshoot)
  const uint32_t t0 = psorom::clockNow();
  const uint64_t totalClk = (uint64_t)(sec * 1000) * clkMs;
  size_t si = 0;
  psorom::Ev ev[512]; chefv2::Action act[32];
  while (psorom::clockNow() - t0 < totalClk) {
    uint32_t now = psorom::clockNow();
    while (si < seq.size() && (uint64_t)seq[si].tMs * clkMs <= now - t0) {   // injecte les commandes a l'heure
      psorom::command(seq[si].id);                                           // la ROM recoit TOUT (headers inclus)
      if (seq[si].ext >= 0) chefv2::command((uint8_t)seq[si].ext, now);      // chefv2 : ids resolus seulement
      si++;
    }
    psorom::run(4000);                                 // ~2 ms emulees (Gen1)
    int n;
    while ((n = psorom::liveDrain(ev, 512)) > 0) chefv2::feed(ev, n);
    chefv2::tick(psorom::clockNow());
    int na = chefv2::drain(act, 32);
    for (int i = 0; i < na; i++)
      printf("%6u ms  %s %d\n", (unsigned)(act[i].tMs - t0 / clkMs), act[i].op == 0 ? "START  " : act[i].op == 1 ? "STOP   " : "RESTART", act[i].id);
  }
  return 0;
}
