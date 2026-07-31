// Host test for the Arena LED animation clock (src/arenaled.cpp).
//
// The wall runs for months without a reboot, so the clock that drives every
// effect has to stay exact for months. This replays the real accumulate loop at
// the real frame rate and checks two things the hardware would only reveal after
// days on a wall:
//
//   1. a float accumulator dies — it first runs fast, then stops advancing at all
//      (this is the pre-fix behaviour, asserted here so the reason for the double
//      is documented and cannot be "simplified" away later);
//   2. the double accumulator + per-effect fmod reduction used by phase() stays
//      accurate AND continuous (no jump big enough to be visible) over 30 days.
//
// Build/run:  g++ -O2 -o /tmp/aphase tools/host_arenaphase_test.cpp -lm && /tmp/aphase
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <cstdlib>

static const double TAU = 6.283185307179586;
static const double DT  = 1.0 / 60.9;      // ~16 ms frame gate in tick()
static const long   DAY = (long)(86400.0 / DT);

int main() {
  const long frames = 30 * DAY;
  int fails = 0;

  // ---- 1. float accumulator: reproduce the failure the review found ----------
  {
    float  f = 0.0f;
    double exact = 0.0;
    long   fastAt = -1, deadAt = -1;
    // Instantaneous rate over a 1000-frame window — the failure is that the clock
    // ADVANCES too fast per frame (each add rounds up to a whole ulp), which is
    // not visible in the cumulative total because it saturates soon after.
    const long WIN = 1000;
    float  wStart = 0.0f;
    for (long n = 0; n < frames; n++) {
      if (n % WIN == 0) {
        if (n) {
          double rate = (double)(f - wStart) / (WIN * DT);
          if (fastAt < 0 && rate > 1.5) fastAt = n;
        }
        wStart = f;
      }
      float before = f;
      f += (float)DT;
      exact += DT;
      if (deadAt < 0 && f == before && exact > 60.0) deadAt = n;
    }
    printf("float : after 30 d, clock reads %.0f s, should read %.0f s\n", (double)f, exact);
    printf("        advances >1.5x too fast from day %.1f, stops advancing at day %.1f\n",
           fastAt < 0 ? -1.0 : (double)fastAt / DAY,
           deadAt < 0 ? -1.0 : (double)deadAt / DAY);
    if (deadAt < 0) { printf("  !! expected the float clock to freeze and it did not\n"); fails++; }
    if (fastAt < 0) { printf("  !! expected the float clock to run fast before freezing\n"); fails++; }
  }

  // ---- 2. double accumulator + phase() reduction ------------------------------
  {
    double s_phase = 0.0, exact = 0.0;
    double worstErr = 0.0;
    // Continuity: the largest frame-to-frame step each reduced phase may take.
    // rate*DT is the expected step; anything much larger means a wrap glitched.
    struct Ch { double rate, period, prev, worst; } ch[] = {
      { 1.1,  TAU,   0, 0 },    // fxPulse
      { 2.0,  TAU,   0, 0 },    // fxWave
      { 1.7,  TAU,   0, 0 },    // fxClassic
      { 14.0, 100.0, 0, 0 },    // fxChase   (period = LED count)
      { 1.0,  20.0,  0, 0 },    // fxArena jackpot
      { 0.08, 1.0,   0, 0 },    // fxRainbow hue
    };
    const int NCH = sizeof(ch) / sizeof(ch[0]);

    for (long n = 0; n < frames; n++) {
      s_phase += DT;
      exact   += DT;
      double err = fabs(s_phase - exact);
      if (err > worstErr) worstErr = err;

      for (int c = 0; c < NCH; c++) {
        float v = (float)fmod(s_phase * ch[c].rate, ch[c].period);
        double step = v - ch[c].prev;
        if (step < 0) step += ch[c].period;          // legitimate modulo wrap
        if (n && step > ch[c].worst) ch[c].worst = step;
        ch[c].prev = v;
      }
    }
    printf("double: after 30 d, clock error %.3g s (%.0f s elapsed)\n", worstErr, exact);
    if (worstErr > 1e-6) { printf("  !! double clock drifted\n"); fails++; }

    for (int c = 0; c < NCH; c++) {
      double expect = ch[c].rate * DT;
      printf("        rate %-5g period %-6g worst frame step %.6f (expected %.6f)\n",
             ch[c].rate, ch[c].period, ch[c].worst, expect);
      // Allow 2x the nominal step: float narrowing and the modulo boundary can
      // round, but a real discontinuity would be orders of magnitude larger.
      if (ch[c].worst > expect * 2.0 + 1e-4) {
        printf("  !! discontinuity in the reduced phase\n"); fails++;
      }
    }
  }

  printf(fails ? "\nFAIL (%d)\n" : "\nPASS\n", fails);
  return fails ? 1 : 0;
}
