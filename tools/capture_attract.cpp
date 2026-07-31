// Run the real Arena ROM under PinMAME and record the lamp matrix it drives.
//
// The point: Arena's attract mode is a program in prom1/prom2, not something to
// imitate by eye. Running the ROM and writing down which lamps it turns on, and
// when, gives the original sequence exactly — and every lamp number here is the
// same L<n> that names an insert on the playfield plan, so the capture drops
// straight onto the wall.
#include "libpinmame.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>
#include <vector>

static volatile int g_running = 0;

void PINMAMECALLBACK OnStateUpdated(int state, const void*) { g_running = state; }
void PINMAMECALLBACK OnDisplayAvailable(int, int, PinmameDisplayLayout*, const void*) {}
// PinMAME calls this once per EMULATED frame, and the Gottlieb driver runs at
// MDRV_FRAMES_PER_SECOND(60). It is therefore the only honest clock here: on
// this Mac the emulation runs about 3x real time, so pacing the capture on the
// host's wall clock stretched every interval by that factor and the sequence
// played back three times too fast. Count emulated frames instead.
#include <atomic>
static std::atomic<long> g_frame{0};
void PINMAMECALLBACK OnDisplayUpdated(int, void*, PinmameDisplayLayout*, const void*) { g_frame++; }
int PINMAMECALLBACK OnAudioAvailable(PinmameAudioInfo*, const void*) {
  return PINMAME_AUDIO_FORMAT_FLOAT;
}
int  PINMAMECALLBACK OnAudioUpdated(void*, int frames, const void*) { return frames; }
void PINMAMECALLBACK OnMechAvailable(int, PinmameMechInfo*, const void*) {}
void PINMAMECALLBACK OnMechUpdated(int, PinmameMechInfo*, const void*) {}
void PINMAMECALLBACK OnSolenoidUpdated(PinmameSolenoidState*, const void*) {}
void PINMAMECALLBACK OnConsoleDataUpdated(void*, int, const void*) {}
int  PINMAMECALLBACK IsKeyPressed(PINMAME_KEYCODE, const void*) { return 0; }
void PINMAMECALLBACK OnLogMessage(PINMAME_LOG_LEVEL, const char* fmt, va_list args, const void*) {
  if (getenv("PM_VERBOSE")) { vfprintf(stderr, fmt, args); fprintf(stderr, "\n"); }
}
void PINMAMECALLBACK OnSoundCommand(int, int, const void*) {}

int main(int argc, char** argv) {
  const double seconds = (argc > 1) ? atof(argv[1]) : 30.0;   // EMULATED seconds

  PinmameConfig config = {
    PINMAME_AUDIO_FORMAT_FLOAT, 48000, "",
    &OnStateUpdated, &OnDisplayAvailable, &OnDisplayUpdated,
    &OnAudioAvailable, &OnAudioUpdated,
    &OnMechAvailable, &OnMechUpdated, &OnSolenoidUpdated,
    &OnConsoleDataUpdated, &IsKeyPressed, &OnLogMessage, &OnSoundCommand,
  };
  snprintf((char*)config.vpmPath, PINMAME_MAX_PATH, "%s/.pinmame/", getenv("HOME"));
  PinmameSetConfig(&config);
  PinmameSetHandleKeyboard(0);
  PinmameSetHandleMechanics(0);

  if (PinmameRun("arena") != PINMAME_STATUS_OK) {
    fprintf(stderr, "PinmameRun(arena) failed\n");
    return 1;
  }

  // 3 emulated frames = 50 ms of game time. System 80 strobes its lamp matrix
  // far faster, but PinMAME's lamp API reports the de-multiplexed logical state,
  // which is what a human sees.
  const int FRAMES_PER_STEP = 3;
  const int STEP_MS = FRAMES_PER_STEP * 1000 / 60;
  const int steps = (int)(seconds * 1000 / STEP_MS);
  std::vector<PinmameLampState> chg(PinmameGetMaxLamps() > 0 ? PinmameGetMaxLamps() : 128);

  printf("{\"step_ms\":%d,\"frames\":[\n", STEP_MS);
  int wrote = 0;
  for (int t = 0; t < steps; t++) {
    // Wait for the GAME to advance, however fast the host happens to run it.
    const long until = g_frame + FRAMES_PER_STEP;
    while (g_frame < until) std::this_thread::sleep_for(std::chrono::microseconds(200));
    const int n = PinmameGetChangedLamps(chg.data());
    if (n <= 0) continue;
    if (wrote++) printf(",\n");
    printf(" {\"t\":%d,\"c\":[", t * STEP_MS);
    for (int i = 0; i < n; i++)
      printf("%s[%d,%d]", i ? "," : "", chg[i].lampNo, chg[i].state);
    printf("]}");
    fflush(stdout);
  }
  printf("\n]}\n");
  fprintf(stderr, "captured %.0f s, %d frames with lamp changes, maxLamps=%d\n",
          seconds, wrote, PinmameGetMaxLamps());
  PinmameStop();
  return 0;
}
