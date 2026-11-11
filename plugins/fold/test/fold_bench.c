/* CPU of fold_core: 4 voices for 20 s of audio at 48 kHz, in blocks of 64, for a few settings.
 * Only measurements on the Pi 5 count (TZ 8, A4); in WSL it only shows the relative cost.
 * Usage: fold_bench (built by scripts/bench.sh with the plugin's flags) */
#include <stdio.h>
#include <time.h>

#include "../src/fold_core.h"

#define SR 48000
#define BLOCK 64
#define SECS 20

static FoldCore core;
static float l[BLOCK], r[BLOCK];

static double now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec * 1e-9;
}

static void bench(const char* label, int shape, float index, int fm, int am, int sync) {
  fc_init(&core, SR);
  FcParams p;
  fc_default_params(&p);
  p.sustain = 1.0f;
  p.mod_shape = shape; p.mod_index = index; p.route_fm = fm; p.route_am = am; p.route_sync = sync;
  p.mod_freq = 40.0f;                                 /* Ratio 3/2-ish: sync and FM both busy */
  fc_set_params(&core, &p);
  const int notes[FC_VOICES] = { 48, 55, 60, 64 };
  for (int i = 0; i < FC_VOICES; i++) fc_note_on(&core, notes[i], 100);
  double sum = 0;
  const double t0 = now();
  for (int b = 0; b < SR * SECS / BLOCK; b++) {
    fc_process(&core, l, r, BLOCK);
    sum += l[0];
  }
  const double secs = now() - t0;
  printf("%-34s %6.2f %% of one core (%5.1f ns per voice per sample)%s\n", label, 100.0 * secs / SECS,
         secs * 1e9 / ((double)SR * SECS * FC_VOICES), sum != sum ? " NaN!" : "");
}

int main(void) {
  printf("fold_core, %d voices, 48 kHz, blocks of %d:\n", FC_VOICES, BLOCK);
  bench("sine, Index 0", FC_MOD_SINE, 0.0f, 1, 0, 0);
  bench("sine Mod, FM", FC_MOD_SINE, 0.7f, 1, 0, 0);
  bench("saw Mod, FM + AM + Sync", FC_MOD_SAW, 0.7f, 1, 1, 1);
  bench("triangle Mod, FM + AM + Sync", FC_MOD_TRI, 0.7f, 1, 1, 1);
  return 0;
}
