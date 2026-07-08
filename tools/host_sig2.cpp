// host_sig2.cpp — SIGNATURE EVENEMENTIELLE d'un son : le flux EXACT d'ecritures puces de la ROM.
// C'est la matiere premiere du chef v2 : par son, on sait QUELS canaux il possede, COMMENT il finit
// (ecriture vol=0 / fin du flux DAC / fin des trames SP), et QUAND — sans aucun seuil devine.
// Build (cf make_chefv2.py) ; Run: host_sig2 <gen 1|2|b> <yrom> <drom> <cmd|h.v> <secondes>
// Sortie (une ligne JSON) : canaux par puce, usage dac/sp, fin par type, duree, soutenu.
#include "psorom.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
static std::vector<uint8_t> load(const char* p){ FILE* f=fopen(p,"rb"); if(!f) return {}; fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET); std::vector<uint8_t> b(n); if(fread(b.data(),1,n,f)!=(size_t)n) b.clear(); fclose(f); return b; }

int main(int argc, char** argv) {
  if (argc < 6) { printf("usage: %s <gen 1|2|b> <yrom> <drom> <cmd|h.v> <sec>\n", argv[0]); return 2; }
  psorom::Board board = (argv[1][0]=='1') ? psorom::GTS80B_GEN1 : (argv[1][0]=='2') ? psorom::GTS80B_GEN2 : psorom::GTS80B_GEN3;
  auto r1 = load(argv[2]), r2 = load(argv[3]);
  int hdr = 0, cmd; { const char* a = argv[4]; const char* dot = strchr(a, '.');
    if (dot) { hdr = atoi(a); cmd = atoi(dot+1); } else cmd = atoi(a); }
  double sec = atof(argv[5]);
  if (!psorom::begin(board, r1.data(), r1.size(), r2.data(), r2.size())) { printf("{\"err\":\"begin\"}\n"); return 1; }
  psorom::command(0); psorom::run(20000);
  psorom::liveEvents(true);
  { psorom::Ev tmp[256]; while (psorom::liveDrain(tmp, 256) > 0) {} }     // purge l'amorcage
  if (hdr) { psorom::command((uint8_t)hdr); psorom::run(300000); { psorom::Ev t2[256]; while (psorom::liveDrain(t2, 256) > 0) {} } }
  psorom::command((uint8_t)cmd);
  uint32_t cmdT = psorom::clockNow(), firstAct = 0;    // latence commande -> premiere activite (calibre la grace de demarrage)

  // suivi par canal : volume courant + derniere ecriture vol>0 et vol=0 ; DAC ; SP ; YM (key-on)
  uint32_t ayUse[2] = {0, 0};                          // masque canaux utilises (vol>0 vu)
  uint32_t lastOn[2][3] = {{0}}, lastOff[2][3] = {{0}};
  uint8_t  vol[2][3] = {{0}};
  uint32_t ymUse = 0; uint8_t ymKey = 0;               // masque voix YM utilisees / actuellement keyees
  uint32_t ymLastOff = 0;
  uint32_t dacN = 0, spN = 0, lastDac = 0, lastSp = 0, lastAyW = 0, lastEv = 0;
  uint32_t lastEvT = 0, maxGap = 0;                    // plus longue pause VRAIMENT silencieuse (rien de tenu) :
                                                       // un intervalle ne compte que si AUCUN vol AY > 0 ni touche YM
  uint32_t addr[2] = {0, 0};                           // (les events AY portent deja addr+val)
  const uint32_t cyc = (board == psorom::GTS80B_GEN1) ? 1000000 : 2000000;   // wallclk units/s
  uint64_t total = (uint64_t)(sec * cyc);
  const uint32_t t0 = psorom::clockNow();
  psorom::Ev ev[512];
  while (psorom::clockNow() - t0 < total) {            // fenetre mesuree sur la VRAIE horloge emulee
    { uint32_t now = psorom::clockNow();               // sortie anticipee : muet 2 s sans demarrage ;
      if (!firstAct) { if (now - t0 > 2 * cyc) break; }                       // fini = rien de tenu + 10 s sans evenement
      else { bool held = ymKey != 0;                   // (pause vraiment-silencieuse max observee ~4,4 s : marge x2)
             for (int c = 0; c < 2; c++) for (int ch = 0; ch < 3; ch++) if (vol[c][ch]) held = true;
             if (!held && now - lastEvT > 10 * cyc) break; } }
    psorom::run(8000);
    int n;
    while ((n = psorom::liveDrain(ev, 512)) > 0) {
      for (int i = 0; i < n; i++) {
        const psorom::Ev& e = ev[i];
        if (e.t > lastEv) lastEv = e.t;
        { bool held = false;                         // etat TENU entre l'evenement precedent et celui-ci :
          for (int c = 0; c < 2; c++) for (int ch = 0; ch < 3; ch++) if (vol[c][ch]) held = true;
          if (ymKey) held = true;                    // un accord AY/YM tenu SONNE sans ecrire -> pas une pause
          if (!held && lastEvT && firstAct && e.t - lastEvT > maxGap) maxGap = e.t - lastEvT;
          lastEvT = e.t; }
        { bool act = false;                          // activite = vol AY > 0 ecrit, key-on YM, feed SP, sample DAC
          if ((e.ty <= 1 && e.a >= 8 && e.a <= 10 && (e.b & 0x1F)) || e.ty == 2 || e.ty == 3
              || (e.ty == 4 && (e.b & 0x78))) act = true;
          if (act && !firstAct) firstAct = e.t; }
        switch (e.ty) {
          case 0: case 1: {
            int c = e.ty; lastAyW = e.t;
            if (e.a >= 8 && e.a <= 10) {               // registres volume canal A/B/C
              int ch = e.a - 8; uint8_t v = e.b & 0x1F;
              if (v && !vol[c][ch]) { ayUse[c] |= 1u << ch; lastOn[c][ch] = e.t; }
              if (!v && vol[c][ch]) lastOff[c][ch] = e.t;
              vol[c][ch] = v;
            }
            break; }
          case 2: spN++; lastSp = e.t; break;
          case 3: dacN++; lastDac = e.t; break;
          case 4: { uint8_t ch = e.b & 7;              // YM2151 KON : key-on/off exact par voix
            if (e.b & 0x78) { ymUse |= 1u << ch; ymKey |= (uint8_t)(1u << ch); }
            else            { if (ymKey & (1u << ch)) ymLastOff = e.t; ymKey &= (uint8_t)~(1u << ch); }
            break; }
        }
      }
    }
  }
  // fin de son : pour chaque canal utilise, derniere extinction ; pour DAC/SP/YM, dernier evenement
  uint32_t endT = 0; bool sustained = false;
  for (int c = 0; c < 2; c++) for (int ch = 0; ch < 3; ch++) if (ayUse[c] & (1u << ch)) {
    if (vol[c][ch]) sustained = true;                  // encore du volume a la fin -> soutenu
    if (lastOff[c][ch] > endT) endT = lastOff[c][ch];
  }
  if (lastDac > endT) endT = lastDac;
  if (lastSp > endT) endT = lastSp;
  if (ymLastOff > endT) endT = ymLastOff;
  if (ymKey) sustained = true;                         // touche YM encore tenue a la fin -> soutenu
  if (dacN && (uint64_t)lastDac > total - cyc/2) sustained = true;          // DAC encore actif a la fin
  printf("{\"cmd\":\"%s\",\"ay0\":%u,\"ay1\":%u,\"dac\":%u,\"sp\":%u,\"ym\":%u,"
         "\"durMs\":%u,\"sustained\":%s,\"dacN\":%u,\"spN\":%u,\"gapMs\":%u,\"onMs\":%u}\n",
         argv[4], ayUse[0], ayUse[1], dacN ? 1 : 0, spN ? 1 : 0, ymUse,
         (unsigned)((uint64_t)endT * 1000 / cyc), sustained ? "true" : "false", dacN, spN,
         (unsigned)((uint64_t)maxGap * 1000 / cyc),
         (unsigned)(firstAct > cmdT ? (uint64_t)(firstAct - cmdT) * 1000 / cyc : 0));
  return 0;
}
