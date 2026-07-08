// test_wavmix.cpp — verifie sur Mac les correctifs du mixeur embarque (meme code que la carte).
// Build: g++ -std=c++17 -I/Users/vapi27/gottfa-esp32/src /tmp/test_wavmix.cpp /Users/vapi27/gottfa-esp32/src/wavmix.cpp -o /tmp/test_wavmix
#include "wavmix.h"
#include <cstdio>
#include <cstring>
using namespace wavmix;

static int g_fills = 0;
// source vivante : remplit tout ce qu'on demande (valeur 1000)
static size_t fillLive(void*, int16_t* d, size_t fr) { g_fills++; for (size_t i = 0; i < fr * 2; i++) d[i] = 1000; return fr; }
// source courte : 16 frames par passage puis epuisee (boucle via rewind)
struct Short { int left; };
static size_t fillShort(void* c, int16_t* d, size_t fr) { Short* s = (Short*)c; size_t n = fr < (size_t)s->left ? fr : s->left; for (size_t i = 0; i < n * 2; i++) d[i] = 500; s->left -= n; return n; }
static bool rewindOk(void* c) { ((Short*)c)->left = 16; return true; }
// source MORTE : rewind echoue, fill ne rend rien (cas SD KO / fichier ferme)
static size_t fillDead(void*, int16_t*, size_t) { g_fills++; return 0; }
static bool rewindKo(void*) { return false; }

static int fails = 0;
#define CHECK(cond, msg) do { if (cond) printf("  OK  %s\n", msg); else { printf("  FAIL %s\n", msg); fails++; } } while (0)

int main() {
  int16_t out[BLOCK * 2];

  printf("1) voix-zombie : boucle morte doit se LIBERER (avant: active a jamais, SD martelee)\n");
  { Mixer m; m.reset();
    VoiceCfg c; c.fill = fillDead; c.ctx = nullptr; c.rewind = rewindKo; c.gain = 255; c.tag = 7; c.loop = true;
    int id = m.trigger(c);
    g_fills = 0; m.mix(out, BLOCK);
    CHECK(!m.active(id), "voix liberee apres source morte");
    int f1 = g_fills; m.mix(out, BLOCK);
    CHECK(g_fills == f1, "plus AUCUNE lecture apres liberation (SD epargnee)"); }

  printf("2) boucle courte (16 fr) : un bloc de 128 doit etre rempli SEAMLESS (B1)\n");
  { Mixer m; m.reset(); Short s{16};
    VoiceCfg c; c.fill = fillShort; c.ctx = &s; c.rewind = rewindOk; c.gain = 255; c.tag = 1; c.loop = true;
    int id = m.trigger(c);
    m.mix(out, BLOCK);
    bool full = true; for (int i = 0; i < BLOCK * 2; i++) if (out[i] == 0) { full = false; break; }
    CHECK(full && m.active(id), "bloc plein, boucle toujours active"); }

  printf("3) remplacement du fond : stopBgLoops ne coupe QUE l'ancien fond\n");
  { Mixer m; m.reset();
    VoiceCfg bg; bg.fill = fillLive; bg.rewind = rewindOk; bg.gain = 255; bg.tag = 2; bg.loop = true; bg.bg = true;
    VoiceCfg fx; fx.fill = fillLive; fx.rewind = rewindOk; fx.gain = 255; fx.tag = 3; fx.loop = true;            // effet en boucle
    VoiceCfg vx; vx.fill = fillLive; vx.gain = 255; vx.tag = 4; vx.voice = true;                                  // voix
    int iBg = m.trigger(bg), iFx = m.trigger(fx), iVx = m.trigger(vx);
    m.stopBgLoops();                                            // = nouveau fond qui remplace l'ancien
    CHECK(!m.active(iBg), "ancien fond coupe");
    CHECK(m.active(iFx) && m.active(iVx), "effet + voix INTACTS (avant: fond garde -> empilement)");
    int iBg2 = m.trigger(bg);
    CHECK(m.active(iBg2), "nouveau fond demarre"); }

  printf("4) stop effets (chef) : stopActiveLoops epargne fond + voix\n");
  { Mixer m; m.reset();
    VoiceCfg bg; bg.fill = fillLive; bg.rewind = rewindOk; bg.gain = 255; bg.tag = 2; bg.loop = true; bg.bg = true;
    VoiceCfg fx; fx.fill = fillLive; fx.rewind = rewindOk; fx.gain = 255; fx.tag = 3; fx.loop = true;
    int iBg = m.trigger(bg), iFx = m.trigger(fx);
    m.stopActiveLoops();
    CHECK(!m.active(iFx), "boucle d'effet coupee");
    CHECK(m.active(iBg), "fond epargne"); }

  printf("5) one-shot : se libere a l'epuisement (pas de zombie non plus)\n");
  { Mixer m; m.reset(); Short s{16};
    VoiceCfg c; c.fill = fillShort; c.ctx = &s; c.gain = 255; c.tag = 5; c.loop = false;
    int id = m.trigger(c);
    m.mix(out, BLOCK);
    CHECK(!m.active(id), "one-shot libere apres sa fin"); }

  printf(fails ? "\n>>> %d ECHEC(S)\n" : "\n>>> TOUT PASSE\n", fails);
  return fails ? 1 : 0;
}
