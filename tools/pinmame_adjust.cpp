// Parcourt le menu de reglages d'une machine sous PinMAME, comme un operateur
// devant le flipper, et LIT l'afficheur a chaque pas.
//
//   tools/pinmame_adjust <jeu> [nb_appuis] [auto]
//   auto = 1 : inverseur porte a monnaie sur Auto (avance dans les reglages)
//   auto = 0 : Manual (RECULE - ce n'est pas un mode d'edition)
//
// Ce qui a ete etabli par mesure sur alpok_l6, et qui ne se devine pas :
//
//  - S6_SWADVANCE = -7, S6_SWUPDN = -6 (src/wpc/s6.h).
//  - Advance seul, inverseur a 0, reste bloque dans le TEST D'AFFICHEUR : 140
//    appuis sans en sortir. Il faut mettre l'inverseur a 1 d'abord.
//  - Une fois dedans : afficheur 0 = valeur, afficheurs 4 et 5 = numero du
//    reglage sur deux chiffres. Le reglage N est atteint au (N+2)eme appui.
//  - ⚠️ La NVRAM persiste bien entre deux lancements (verifie : valeur laissee a
//    03, relue a 03 au demarrage suivant), donc ce qu'on change ici tient.
//  - ⚠️ En revanche l'increment de valeur n'a PAS ete trouve. Pulser
//    l'interrupteur 3 fait monter l'afficheur 01 -> 02 -> 42 -> 82 : c'est un
//    compteur qui monte, donc un AUDIT, pas un parametre. Ne pas conclure que
//    l'interrupteur 3 edite un reglage.
//
// Le decodage 7 segments ci-dessous marche : c'est lui qui a permis de lire les
// numeros de reglage au lieu de compter les appuis a l'aveugle.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <vector>
#include <string>
#include <atomic>
#include <thread>
#include <chrono>
#include <mutex>
#include "libpinmame.h"

static std::atomic<long> g_frame{0};
static std::mutex g_m;
static std::vector<std::string> g_disp;      // texte decode, un par afficheur

void PINMAMECALLBACK OnStateUpdated(int, const void*) {}
void PINMAMECALLBACK OnDisplayAvailable(int index, int, PinmameDisplayLayout* l, const void*) {
  if (l) fprintf(stderr, "[disp] %d : type=%d %dx%d length=%d\n",
                 index, (int)l->type, l->width, l->height, l->length);
}
// Decodage 7 segments -> caractere. Table standard a,b,c,d,e,f,g = bits 0..6.
static char seg2char(uint16_t s) {
  static const struct { uint8_t m; char c; } T[] = {
    {0x3F,'0'},{0x06,'1'},{0x5B,'2'},{0x4F,'3'},{0x66,'4'},
    {0x6D,'5'},{0x7D,'6'},{0x07,'7'},{0x7F,'8'},{0x6F,'9'},
    {0x00,' '},{0x40,'-'},{0x77,'A'},{0x7C,'b'},{0x39,'C'},
    {0x5E,'d'},{0x79,'E'},{0x71,'F'},
  };
  const uint8_t m = s & 0x7F;
  for (auto& t : T) if (t.m == m) return t.c;
  return '?';
}
void PINMAMECALLBACK OnDisplayUpdated(int index, void* buf, PinmameDisplayLayout* l, const void*) {
  if (!buf || !l) return;
  std::string out;
  const uint16_t* p = (const uint16_t*)buf;
  for (int i = 0; i < l->length; i++) out += seg2char(p[i]);
  std::lock_guard<std::mutex> g(g_m);
  if ((int)g_disp.size() <= index) g_disp.resize(index + 1);
  g_disp[index] = out;
}
int PINMAMECALLBACK OnAudioAvailable(PinmameAudioInfo* a, const void*) { return a ? a->samplesPerFrame : 800; }
int PINMAMECALLBACK OnAudioUpdated(void*, int n, const void*) { g_frame++; return n; }
void PINMAMECALLBACK OnMechAvailable(int, PinmameMechInfo*, const void*) {}
void PINMAMECALLBACK OnMechUpdated(int, PinmameMechInfo*, const void*) {}
void PINMAMECALLBACK OnSolenoidUpdated(PinmameSolenoidState*, const void*) {}
void PINMAMECALLBACK OnConsoleDataUpdated(void*, int, const void*) {}
int  PINMAMECALLBACK IsKeyPressed(PINMAME_KEYCODE, const void*) { return 0; }
void PINMAMECALLBACK OnLogMessage(PINMAME_LOG_LEVEL, const char* f, va_list a, const void*) {
  if (getenv("PM_VERBOSE")) { vfprintf(stderr, f, a); fprintf(stderr, "\n"); }
}
void PINMAMECALLBACK OnSoundCommand(int, int, const void*) {}

static void waitF(long n) { const long u = g_frame + n; while (g_frame < u) std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
static std::string show() {
  std::lock_guard<std::mutex> g(g_m);
  std::string s;
  for (size_t i = 0; i < g_disp.size(); i++) { s += "["; s += g_disp[i]; s += "]"; }
  return s;
}
static void pulse(int sw, long hold = 12, long gap = 24) {
  PinmameSetSwitch(sw, 1); waitF(hold);
  PinmameSetSwitch(sw, 0); waitF(gap);
}

int main(int argc, char** argv) {
  const char* game = (argc > 1) ? argv[1] : "alpok_l6";
  const int presses = (argc > 2) ? atoi(argv[2]) : 45;
  const int updn    = (argc > 3) ? atoi(argv[3]) : 0;   // 1 = Auto, 0 = Manual
  PinmameConfig c = { PINMAME_AUDIO_FORMAT_INT16, 48000, "",
    &OnStateUpdated,&OnDisplayAvailable,&OnDisplayUpdated,&OnAudioAvailable,&OnAudioUpdated,
    &OnMechAvailable,&OnMechUpdated,&OnSolenoidUpdated,&OnConsoleDataUpdated,&IsKeyPressed,
    &OnLogMessage,&OnSoundCommand };
  snprintf((char*)c.vpmPath, PINMAME_MAX_PATH, "%s/.pinmame/", getenv("HOME"));
  PinmameSetConfig(&c); PinmameSetHandleKeyboard(0); PinmameSetHandleMechanics(0);
  if (PinmameRun(game) != PINMAME_STATUS_OK) { fprintf(stderr, "PinmameRun a echoue\n"); return 1; }
  waitF(10 * 60);
  PinmameSetSwitch(-6, updn);     // Auto/Manual de la porte a monnaie
  waitF(60);
  printf("attract (updn=%d) : %s\n", updn, show().c_str());
  for (int i = 1; i <= presses; i++) {
    pulse(-7);
    printf("advance %2d : %s\n", i, show().c_str());
    fflush(stdout);
  }
  PinmameStop();
  return 0;
}
