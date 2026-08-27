// Enregistre le SON que la ROM produit, pendant que l'attract tourne.
//
//   tools/capture_sound <pinmame_name> [secondes_emulees] > sortie.wav
//
// Meme mise en route que tools/capture_attract.cpp - meme ROM, meme horloge,
// meme facon de compter le temps de jeu (les rappels d'affichage, une trame
// emulee a la fois). Ce qui change : on garde les echantillons audio au lieu de
// les jeter, et on ecrit un WAV 16 bits.
//
// La cadence est celle du jeu, pas celle de l'hote : PinMAME tourne ici a ~3x le
// temps reel, donc pacer sur l'horloge murale donnerait un son 3x trop aigu. On
// ne pace rien du tout - on prend TOUS les echantillons produits, et leur nombre
// divise par la frequence d'echantillonnage EST la duree de jeu enregistree.
//
// Sortie sur stdout, diagnostic sur stderr (dont le niveau RMS : c'est lui qui
// dit s'il y a vraiment du son, plutot que de le supposer).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <vector>
#include <atomic>
#include <thread>
#include <chrono>
#include "libpinmame.h"

static volatile int g_running = 0;
void PINMAMECALLBACK OnStateUpdated(int state, const void*) { g_running = state; }
void PINMAMECALLBACK OnDisplayAvailable(int, int, PinmameDisplayLayout*, const void*) {}
// ⚠️ NE PAS se servir du rappel d'affichage comme horloge.
//
// Il n'est PAS appele une fois par trame emulee : libpinmame le declenche par
// AFFICHEUR, et Alien Poker en a plusieurs. Mesure sur la carte : 8,96 rappels
// d'affichage pour un rappel audio. Une capture pacee dessus s'arrete donc bien
// avant la duree demandee - 16,7 s d'audio pour 150 s reclamees - et on croit
// avoir ecoute deux minutes d'attract alors qu'on en a entendu seize secondes.
//
// Le rappel AUDIO, lui, vient de sound_update(), appele une fois par trame
// emulee dans updatescreen() SANS condition (le saut d'image ne saute que le
// dessin). Une trame = samplesPerFrame echantillons = 1/60 s de temps de jeu,
// exactement. C'est la seule horloge fiable ici.
static std::atomic<long> g_frame{0};
void PINMAMECALLBACK OnDisplayUpdated(int, void*, PinmameDisplayLayout*, const void*) {}

static std::vector<int16_t> g_pcm;
static int    g_channels = 1;
static double g_rate     = 48000.0;
static std::atomic<bool> g_record{false};

// ⚠️ Ce que ce rappel RENVOIE devient samples_this_frame dans le mixeur de MAME.
//
//   mixer_sh_start()  ->  r = osd_start_audio_stream(...)  ->  samples_this_frame = r
//   et osd_start_audio_stream() retourne directement ce que rend ce rappel.
//
// Renvoyer le FORMAT (INT16 = 0, comme le fait tools/capture_attract.cpp) revient
// donc a dire "zero echantillon par trame" : le mixeur tourne, le rappel audio est
// appele a chaque trame, et il annonce toujours 0. Aucune erreur, aucun message -
// juste le silence. C'est ce qui a fait croire pendant une heure que la
// bibliotheque etait construite sans son.
//
// La bonne valeur est samplesPerFrame, que PinMAME vient de calculer pour nous
// (sample_rate / frames_per_second = 48000/60 = 800).
int PINMAMECALLBACK OnAudioAvailable(PinmameAudioInfo* a, const void*) {
  if (!a) return 0;
  g_channels = a->channels;
  g_rate     = a->sampleRate;
  return a->samplesPerFrame;
}
int PINMAMECALLBACK OnAudioUpdated(void* buf, int samples, const void*) {
  g_frame++;                       // une trame emulee, l'horloge de reference
  if (g_record.load() && buf && samples > 0) {
    const int16_t* p = (const int16_t*)buf;
    g_pcm.insert(g_pcm.end(), p, p + (size_t)samples * g_channels);
  }
  return samples;
}
void PINMAMECALLBACK OnMechAvailable(int, PinmameMechInfo*, const void*) {}
void PINMAMECALLBACK OnMechUpdated(int, PinmameMechInfo*, const void*) {}
void PINMAMECALLBACK OnSolenoidUpdated(PinmameSolenoidState*, const void*) {}
void PINMAMECALLBACK OnConsoleDataUpdated(void*, int, const void*) {}
int  PINMAMECALLBACK IsKeyPressed(PINMAME_KEYCODE, const void*) { return 0; }
void PINMAMECALLBACK OnLogMessage(PINMAME_LOG_LEVEL, const char* fmt, va_list args, const void*) {
  if (getenv("PM_VERBOSE")) { vfprintf(stderr, fmt, args); fprintf(stderr, "\n"); }
}
void PINMAMECALLBACK OnSoundCommand(int, int, const void*) {}

static void put32(FILE* f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void put16(FILE* f, uint16_t v) { fwrite(&v, 2, 1, f); }

int main(int argc, char** argv) {
  if (argc < 2) { fprintf(stderr, "usage: %s <pinmame_name> [emulated_seconds] > out.wav\n", argv[0]); return 2; }
  const char* game = argv[1];
  const double seconds = (argc > 2) ? atof(argv[2]) : 60.0;
  const double skip    = (argc > 3) ? atof(argv[3]) : 8.0;   // laisser le boot passer

  PinmameConfig config = {
    PINMAME_AUDIO_FORMAT_INT16, 48000, "",
    &OnStateUpdated, &OnDisplayAvailable, &OnDisplayUpdated,
    &OnAudioAvailable, &OnAudioUpdated,
    &OnMechAvailable, &OnMechUpdated, &OnSolenoidUpdated,
    &OnConsoleDataUpdated, &IsKeyPressed, &OnLogMessage, &OnSoundCommand,
  };
  snprintf((char*)config.vpmPath, PINMAME_MAX_PATH, "%s/.pinmame/", getenv("HOME"));
  PinmameSetConfig(&config);
  PinmameSetHandleKeyboard(0);
  PinmameSetHandleMechanics(0);

  if (PinmameRun(game) != PINMAME_STATUS_OK) {
    fprintf(stderr, "PinmameRun(%s) failed - le zip de la ROM est-il dans ~/.pinmame/roms/ ?\n", game);
    return 1;
  }

  // Le temps de JEU se compte en trames emulees (60/s), jamais sur l'horloge de
  // l'hote : l'emulation tourne plus vite que le temps reel.
  const long skipFrames = (long)(skip * 60.0);
  while (g_frame < skipFrames) std::this_thread::sleep_for(std::chrono::milliseconds(2));
  g_pcm.clear();
  g_record.store(true);
  const long until = g_frame + (long)(seconds * 60.0);
  while (g_frame < until) std::this_thread::sleep_for(std::chrono::milliseconds(2));
  g_record.store(false);
  PinmameStop();

  const size_t frames = g_pcm.size() / (size_t)g_channels;
  // Niveau reel, pour ne pas livrer un fichier muet en croyant avoir capture.
  double sum = 0; int16_t peak = 0;
  for (int16_t v : g_pcm) { sum += (double)v * v; if (abs(v) > peak) peak = abs(v); }
  const double rms = g_pcm.empty() ? 0 : sqrt(sum / g_pcm.size());

  FILE* o = stdout;
  const uint32_t dataBytes = (uint32_t)(g_pcm.size() * 2);
  fwrite("RIFF", 1, 4, o); put32(o, 36 + dataBytes); fwrite("WAVE", 1, 4, o);
  fwrite("fmt ", 1, 4, o); put32(o, 16); put16(o, 1); put16(o, (uint16_t)g_channels);
  put32(o, (uint32_t)g_rate); put32(o, (uint32_t)(g_rate * g_channels * 2));
  put16(o, (uint16_t)(g_channels * 2)); put16(o, 16);
  fwrite("data", 1, 4, o); put32(o, dataBytes);
  fwrite(g_pcm.data(), 2, g_pcm.size(), o);

  fprintf(stderr, "%zu trames, %d canaux, %.0f Hz -> %.1f s de jeu\n",
          frames, g_channels, g_rate, frames / g_rate);
  fprintf(stderr, "niveau : RMS %.1f, crete %d (sur 32767) -> %s\n", rms, peak,
          rms < 20 ? "MUET ou quasi" : "il y a du son");
  return 0;
}
