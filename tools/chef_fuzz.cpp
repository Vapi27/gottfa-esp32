// chef_fuzz.cpp — MARTELAGE EXHAUSTIF du chef v2 : genere des milliers de scenarios de commandes
// (paires, rafales, chaines de vol, soupes aleatoires) et verifie a CHAQUE TICK deux invariants,
// avec la ROM elle-meme comme ORACLE (volumes AY tenus, cles YM, flux DAC/SP recents) :
//   IMMORTEL : ROM muette >5 s mais le chef garde un son vivant  -> un WAV bouclerait a l'infini.
//   MUET     : ROM sonore >3 s mais le chef ne fait rien jouer   -> son manquant.
// Chaque echec imprime la ligne chefsim qui le REPRODUIT. Usage :
//   chef_fuzz <gen> <yrom> <drom> <sigs> <set> [stride [offset]]   (stride/offset = parallelisation)
//   set: A=singles B=paires C=rafales D=chaines-de-vol E=aleatoire(2000)
#include "psorom.h"
#include "chefv2.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
static std::vector<uint8_t> load(const char* p){ FILE* f=fopen(p,"rb"); if(!f) return {}; fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET); std::vector<uint8_t> b(n); if(fread(b.data(),1,n,f)!=(size_t)n) b.clear(); fclose(f); return b; }
static int jint(const char* s, const char* k){ char pat[32]; snprintf(pat,sizeof(pat),"\"%s\":",k); const char* p=strstr(s,pat); return p?atoi(p+strlen(pat)):0; }

static chefv2::Sig g_sigs[96]; static int g_nSig = 0;
static psorom::Board g_board; static std::vector<uint8_t> g_r1, g_r2;
static uint32_t g_clkMs;

struct Cmd { uint32_t tMs; uint8_t id; };

// rejoue un scenario ; retourne "" si OK, sinon la description de l'echec
static std::string runScenario(const std::vector<Cmd>& seq, uint32_t totalMs) {
  psorom::begin(g_board, g_r1.data(), g_r1.size(), g_r2.data(), g_r2.size());
  psorom::command(0); psorom::run(20000);
  psorom::liveEvents(true);
  { psorom::Ev t[256]; while (psorom::liveDrain(t, 256) > 0) {} }
  chefv2::begin(g_sigs, g_nSig, g_clkMs);
  const uint32_t t0 = psorom::clockNow();
  // oracle : etat "ca sonne" cote ROM (meme logique que host_sig2 : tenu OU activite recente)
  uint8_t vol[2][3] = {{0}}; uint8_t ymKey = 0;
  uint32_t lastAct = 0;                                  // derniere activite (vol>0 ecrit / KON / SP / DAC)
  uint32_t quietSince = 0, activeSince = 0;              // debuts des etats continus (0 = pas dans l'etat)
  uint32_t chefOnSince = 0, chefOffSince = 0;            // pareil cote chef : les VIOLATIONS = chevauchement des fenetres
  size_t si = 0;
  psorom::Ev ev[512]; chefv2::Action act[64];
  const uint32_t warmup = 9000;                          // jingle de reset (sans commande) : hors verdict
  while (psorom::clockNow() - t0 < totalMs * g_clkMs) {
    uint32_t now = psorom::clockNow();
    while (si < seq.size() && (uint64_t)seq[si].tMs * g_clkMs <= now - t0) {
      psorom::command(seq[si].id); chefv2::command(seq[si].id, now); si++;
    }
    psorom::run(8000);
    int n;
    while ((n = psorom::liveDrain(ev, 512)) > 0) {
      chefv2::feed(ev, n);
      for (int i = 0; i < n; i++) { const psorom::Ev& e = ev[i];
        bool on = false;
        if (e.ty <= 1 && e.a >= 8 && e.a <= 10) { uint8_t v = e.b & 0x1F; vol[e.ty][e.a - 8] = v; if (v) on = true; }
        else if (e.ty == 2 || e.ty == 3) on = true;
        else if (e.ty == 4) { uint8_t c = e.b & 7; if (e.b & 0x78) { ymKey |= 1u << c; on = true; } else ymKey &= (uint8_t)~(1u << c); }
        if (on) lastAct = e.t;
      }
    }
    chefv2::tick(psorom::clockNow());
    chefv2::drain(act, 64);
    now = psorom::clockNow();
    bool held = ymKey != 0;
    for (int c = 0; c < 2; c++) for (int ch = 0; ch < 3; ch++) if (vol[c][ch]) held = true;
    bool romOn = held || (lastAct && now - lastAct < 300u * g_clkMs);
    bool chefOn = chefv2::aliveMask() != 0;
    if (romOn) { activeSince = activeSince ? activeSince : now; quietSince = 0; }
    else       { quietSince = quietSince ? quietSince : now; activeSince = 0; }
    if (chefOn) { chefOnSince = chefOnSince ? chefOnSince : now; chefOffSince = 0; }
    else        { chefOffSince = chefOffSince ? chefOffSince : now; chefOnSince = 0; }
    uint32_t el = (now - t0) / g_clkMs;
    if (el > warmup) {
      if (chefOn && quietSince) {                        // IMMORTEL = (ROM muette) ET (chef vivant) PENDANT 5 s
        uint32_t s0 = quietSince > chefOnSince ? quietSince : chefOnSince;
        if (now - s0 > 5000u * g_clkMs) {
          char b[128]; snprintf(b, sizeof(b), "IMMORTEL a t=%ums (ROM muette 5s, aliveMask=%llx)", el, (unsigned long long)chefv2::aliveMask());
          return b; } }
      if (!chefOn && activeSince) {                      // MUET = (ROM sonore) ET (chef vide) PENDANT 3 s
        uint32_t s0 = activeSince > chefOffSince ? activeSince : chefOffSince;
        if (now - s0 > 3000u * g_clkMs) {
          char b[96]; snprintf(b, sizeof(b), "MUET a t=%ums (ROM sonore 3s, chef vide)", el);
          return b; } }
    }
  }
  return "";
}

int main(int argc, char** argv) {
  if (argc < 6) { printf("usage: %s <gen> <yrom> <drom> <sigs.jsonl> <A|B|C|D|E> [stride [offset]]\n", argv[0]); return 2; }
  g_board = (argv[1][0]=='1') ? psorom::GTS80B_GEN1 : (argv[1][0]=='2') ? psorom::GTS80B_GEN2 : psorom::GTS80B_GEN3;
  g_clkMs = (g_board == psorom::GTS80B_GEN1) ? 1000 : 2000;
  g_r1 = load(argv[2]); g_r2 = load(argv[3]);
  { FILE* f = fopen(argv[4], "r"); if (!f) { printf("sigs KO\n"); return 1; } char ln[512];
    while (fgets(ln, sizeof(ln), f) && g_nSig < 96) { const char* k = strstr(ln, "\"kills\":\"");
      if (k) { const char* by = strstr(ln, "\"by\":\"");
        if (by) { auto dec = [](const char* q) { int h = atoi(q); const char* d = strchr(q, '.');
                                                 return d ? (((~h) & 0x1F) * 32 + (atoi(d + 1) & 0x1F)) : h; };
                  chefv2::setKill((uint8_t)dec(k + 9), (uint8_t)dec(by + 6)); }
        continue; }
      { const char* k2 = strstr(ln, "\"keeps\":\"");
      if (k2) { const char* un = strstr(ln, "\"under\":\"");
        if (un) { auto dec = [](const char* q) { int h = atoi(q); const char* d = strchr(q, '.');
                                                 return d ? (((~h) & 0x1F) * 32 + (atoi(d + 1) & 0x1F)) : h; };
                  { int ttl = 0; const char* tt = strstr(ln, "\"ttlMs\":"); if (tt) ttl = atoi(tt + 8);
                    chefv2::setKeep((uint8_t)dec(k2 + 9), (uint8_t)dec(un + 9), (uint16_t)ttl); } }
        continue; } }
      const char* c = strstr(ln, "\"cmd\":\""); if (!c) continue;
      chefv2::Sig& g = g_sigs[g_nSig++];
      { int h = atoi(c + 7); const char* dot = strchr(c + 7, '.');
        g.id = (uint8_t)(dot ? (((~h) & 0x1F) * 32 + (atoi(dot + 1) & 0x1F)) : h); }
      g.ay0 = (uint8_t)jint(ln,"ay0"); g.ay1 = (uint8_t)jint(ln,"ay1"); g.dac = (uint8_t)jint(ln,"dac");
      g.sp = (uint8_t)jint(ln,"sp");  g.ym  = (uint8_t)jint(ln,"ym");
      { const char* p = strstr(ln, "\"sustained\":"); g.sustained = (p && !strncmp(p+12,"true",4)) ? 1 : 0; }
      g.durMs = (uint32_t)jint(ln,"durMs"); g.gapMs = (uint32_t)jint(ln,"gapMs"); g.onMs = (uint32_t)jint(ln,"onMs"); }
    fclose(f); }
  char set = argv[5][0];
  int stride = argc > 6 ? atoi(argv[6]) : 1, offset = argc > 7 ? atoi(argv[7]) : 0;

  std::vector<std::vector<Cmd>> S;                       // scenarios (les temps incluent le warmup 9 s)
  const uint32_t W = 9500;
  auto fin = [](std::vector<Cmd> v, uint32_t last) { v.push_back(Cmd{ last + 5000, 31 }); return v; };  // chaque scenario finit par 31
  if (set == 'A') for (int a = 1; a <= 31; a++) S.push_back(fin({ Cmd{W, (uint8_t)a} }, W));
  if (set == 'B') { const uint32_t gaps[3] = {100, 600, 3000};
    for (int a = 1; a <= 31; a++) for (int b = 1; b <= 31; b++) for (int gi = 0; gi < 3; gi++)
      S.push_back(fin({ Cmd{W,(uint8_t)a}, Cmd{W+gaps[gi],(uint8_t)b} }, W+gaps[gi])); }
  if (set == 'C') for (int a = 1; a <= 31; a++) {                       // rafale x5 @150 ms
    std::vector<Cmd> v; for (int k = 0; k < 5; k++) v.push_back(Cmd{ W + (uint32_t)k*150, (uint8_t)a });
    S.push_back(fin(v, W + 600)); }
  if (set == 'D') {                                                     // chaines de vol : soutenu + 2 voleurs rapproches
    std::vector<int> sus, oth;
    for (int i = 0; i < g_nSig; i++) { if (!g_sigs[i].ay0 && !g_sigs[i].ay1 && !g_sigs[i].dac && !g_sigs[i].sp && !g_sigs[i].ym) continue;
                                       (g_sigs[i].sustained ? sus : oth).push_back(g_sigs[i].id); }
    for (int s : sus) for (int e1 : oth) for (int e2 : oth)
      S.push_back(fin({ Cmd{W,(uint8_t)s}, Cmd{W+4000,(uint8_t)e1}, Cmd{W+4600,(uint8_t)e2} }, W+4600)); }
  if (set == 'F') {                                                     // le motif "vent" : musique active + ambiance +
    std::vector<int> sus, oth;                                          // voleur + RE-commande de la musique (RESTART)
    for (int i = 0; i < g_nSig; i++) { if (!g_sigs[i].ay0 && !g_sigs[i].ay1 && !g_sigs[i].dac && !g_sigs[i].sp && !g_sigs[i].ym) continue;
                                       (g_sigs[i].sustained ? sus : oth).push_back(g_sigs[i].id); }
    for (int m : sus) for (int a : sus) { if (m == a) continue;
      for (int e : oth)
        S.push_back(fin({ Cmd{W,(uint8_t)m}, Cmd{W+3500,(uint8_t)a}, Cmd{W+7500,(uint8_t)e}, Cmd{W+7540,(uint8_t)m} }, W+7540)); }
  }
  if (set == 'E') { uint32_t rng = 0xC0FFEE;                            // soupe aleatoire reproductible (graine fixe)
    auto rnd = [&rng](uint32_t m) { rng = rng * 1664525u + 1013904223u; return (rng >> 8) % m; };
    for (int k = 0; k < 2000; k++) { std::vector<Cmd> v; uint32_t t = W; int nc = 4 + (int)rnd(5);
      for (int j = 0; j < nc; j++) { v.push_back(Cmd{ t, (uint8_t)(1 + rnd(31)) }); t += 80 + rnd(3800); }
      S.push_back(fin(v, t)); } }

  int fails = 0, done = 0;
  for (size_t i = offset; i < S.size(); i += stride) {
    const auto& v = S[i];
    uint32_t total = v.back().tMs + 8000;                // 31 final + 8 s d'observation
    std::string r = runScenario(v, total);
    done++;
    if (!r.empty()) { fails++;
      std::string repro;
      for (auto& c : v) { char b[24]; snprintf(b, sizeof(b), "%u:%d ", c.tMs, c.id); repro += b; }
      printf("ECHEC %s\n  repro: /tmp/chefsim %s <roms> <sigs> \"%s\" %u\n", r.c_str(), argv[1], repro.c_str(), (total + 999) / 1000);
      fflush(stdout); }
  }
  printf("set %c [%d/%d]: %d scenarios, %d echec(s)\n", set, offset, stride, done, fails);
  return fails ? 1 : 0;
}
