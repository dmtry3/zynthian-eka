/* CPU of fold_core: 4 voices for 20 s of audio at 48 kHz, in blocks of 64, for a few settings.
 * Only measurements on the Pi 5 count (TZ 8, A4); in WSL it only shows the relative cost.
 * Pi 5, 2026-11-11 (2x + ADAA): 1.4 % of one core with everything on, 4.3 % with the Saw shaper too;
 * 2026-11-12 (stage 3, with the lopass gate): 2.3 / 2.1 / 6.7 %; stage 4a: VA 3.7, FM2 3.1, Wave 6.3 % (2x, before
 * the engines went to 4x).
 * Usage: fold_bench (built by scripts/bench.sh with the plugin's flags) */
#include <stdio.h>
#include <time.h>

#include "../src/fold_core.h"

#define SR 48000
#define BLOCK 64
#define SECS 20

static FoldCore core;
static int full_matrix;                           /* every mod-matrix cell at 10 % */
static int delay_mode;                            /* FC_DL_* for every voice */
static int nvoices = 4;                           /* voices playing (the rows before stage 5b: 4) */
static float l[BLOCK], r[BLOCK];

static double now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec * 1e-9;
}

static void bench(const char* label, int source, int shape, float index, int fm, int am, int sync, float timbre,
                  float order) {
  fc_init(&core, SR);
  FcParams p;
  fc_default_params(&p);
  /* the defaults: FG1 Gate, the lopass gate in Combo (low-pass and level): the full voice */
  p.source = source;
  p.voices = nvoices;
  p.delay_mode = delay_mode; p.delay_fb = 0.9f; p.delay_mix = 0.5f; p.delay_shaper = 0.5f;
  p.delay_eq_db = -0.5f; p.delay_time = 50.0f;   /* Tuned: the note; Free: 250 ms */
  for (int s = 0; s < FC_NMSRC; s++)
    for (int d = 0; d < FC_NMDST; d++) p.matrix[s][d] = full_matrix ? 0.1f : 0.0f;
  p.mod_shape = shape; p.mod_index = index; p.route_fm = fm; p.route_am = am; p.route_sync = sync;
  p.mod_freq = 40.0f;                                 /* Ratio 3/2-ish: sync and FM both busy */
  p.timbre = timbre; p.order = order;
  fc_set_params(&core, &p);
  const int notes[8] = { 48, 55, 60, 64, 67, 71, 74, 77 };
  for (int i = 0; i < nvoices; i++) fc_note_on(&core, notes[i], 100);
  double sum = 0;
  const double t0 = now();
  for (int b = 0; b < SR * SECS / BLOCK; b++) {
    fc_process(&core, l, r, BLOCK);
    sum += l[0];
  }
  const double secs = now() - t0;
  printf("%-44s %6.2f %% of one core (%5.1f ns per voice per sample)%s\n", label, 100.0 * secs / SECS,
         secs * 1e9 / ((double)SR * SECS * nvoices), sum != sum ? " NaN!" : "");
}

int main(void) {
  printf("fold_core, 4 voices (8 where it says so), 48 kHz (voices at %dx), blocks of %d:\n", FC_OS, BLOCK);
  bench("sine, Index 0, Timbre 0", FC_SRC_SINE, FC_MOD_SINE, 0.0f, 1, 0, 0, 0.0f, 0.0f);
  bench("saw Mod, FM+AM+Sync, folder", FC_SRC_SINE, FC_MOD_SAW, 0.7f, 1, 1, 1, 0.7f, 0.0f);
  bench("saw Mod, FM+AM+Sync, folder + Saw shaper", FC_SRC_SINE, FC_MOD_SAW, 0.7f, 1, 1, 1, 0.7f, 0.25f);
  full_matrix = 1;
  bench("saw Mod, FM+AM+Sync, folder, all cells", FC_SRC_SINE, FC_MOD_SAW, 0.7f, 1, 1, 1, 0.7f, 0.0f);
  bench("VA, FM+AM+Sync, folder, all cells", FC_SRC_VA, FC_MOD_SAW, 0.7f, 1, 1, 1, 0.7f, 0.0f);
  full_matrix = 0;
  delay_mode = FC_DL_TUNED;
  bench("saw Mod, FM+AM+Sync, folder, delay Tuned", FC_SRC_SINE, FC_MOD_SAW, 0.7f, 1, 1, 1, 0.7f, 0.0f);
  delay_mode = FC_DL_FREE;
  bench("saw Mod, FM+AM+Sync, folder, delay Free", FC_SRC_SINE, FC_MOD_SAW, 0.7f, 1, 1, 1, 0.7f, 0.0f);
  delay_mode = FC_DL_MODAL;
  bench("saw Mod, FM+AM+Sync, folder, Modal", FC_SRC_SINE, FC_MOD_SAW, 0.7f, 1, 1, 1, 0.7f, 0.0f);
  delay_mode = FC_DL_STRING;
  bench("saw Mod, FM+AM+Sync, folder, String", FC_SRC_SINE, FC_MOD_SAW, 0.7f, 1, 1, 1, 0.7f, 0.0f);
  delay_mode = FC_DL_OFF_MODE;
  bench("VA, FM+AM+Sync, folder", FC_SRC_VA, FC_MOD_SAW, 0.7f, 1, 1, 1, 0.7f, 0.0f);
  bench("FM2, FM+AM+Sync, folder", FC_SRC_FM2, FC_MOD_SAW, 0.7f, 1, 1, 1, 0.7f, 0.0f);
  bench("Wave, FM+AM+Sync, folder", FC_SRC_WAVE, FC_MOD_SAW, 0.7f, 1, 1, 1, 0.7f, 0.0f);
  bench("Harm, FM+AM+Sync, folder", FC_SRC_HARM, FC_MOD_SAW, 0.7f, 1, 1, 1, 0.7f, 0.0f);
  bench("PD, FM+AM+Sync, folder", FC_SRC_PD, FC_MOD_SAW, 0.7f, 1, 1, 1, 0.7f, 0.0f);
  nvoices = 8;                                       /* stage 5b: up to 8 voices */
  full_matrix = 1;
  bench("8 voices: VA, FM+AM+Sync, folder, all cells", FC_SRC_VA, FC_MOD_SAW, 0.7f, 1, 1, 1, 0.7f, 0.0f);
  full_matrix = 0;
  delay_mode = FC_DL_TUNED;
  bench("8 voices: Wave, FM+AM+Sync, folder, delay", FC_SRC_WAVE, FC_MOD_SAW, 0.7f, 1, 1, 1, 0.7f, 0.0f);
  delay_mode = FC_DL_MODAL;
  bench("8 voices: Wave, FM+AM+Sync, folder, Modal", FC_SRC_WAVE, FC_MOD_SAW, 0.7f, 1, 1, 1, 0.7f, 0.0f);
  delay_mode = FC_DL_OFF_MODE;
  return 0;
}
