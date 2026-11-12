/* Aliasing of the folder, the other shapers and FM as the plugin makes them: voices at 2x (the engines at 4x since
 * 2026-11-12), ADAA on the folder, halfband decimators. Chosen on 2026-11-11 from four modes; their table then (to 16 kHz, dB):
 *   folder Timbre 70 %   C4  C5  C6  C7:   1x -45 -37 -28 -18   ADAA -57 -49 -44 -33
 *                                          2x -58 -50 -40 -30   2x + ADAA -75 -70 -59 -55
 *   ADAA alone cost -1.4 dB at 8.4 kHz (an average over a sample), at 2x -0.3 dB.
 * Metric: power outside the harmonics of the note (the only lines a periodic sound has; with Ratio p/q the lines
 * are at multiples of f/q), relative to the power on them, in dB, in the band to 16 kHz.
 * Phase modulation was compared here at stage 1 and dropped on 2026-11-11 (saw Mod OSC: -19 dB on C5 against
 * -61 for linear FM).
 * Stage 4: the engines VA, FM2, Wave at harmonic settings (VA: Harmonics 50 % = unison; FM2: Harmonics 30/128 =
 * ratio 1), with FM and Sync. Known limits (2026-11-12): a bright Wave on high notes (most of its 64 harmonics
 * above the voices' Nyquist, as in the original engine), and a VA saw under strong FM from a saw Mod OSC (the
 * frequency step bends the saw's slope; polyBLEP corrects only steps of the wave). At 4x, on C5 (at 2x before):
 * VA saw + folder 70 % -44 (-36), Wave bright -71 (-54), VA saw + saw FM -51 (-29).
 * Usage: alias_report (prints a table; no pass/fail) */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/fold_core.h"

#define SR 48000
#define NFFT 65536

static double re[NFFT], im[NFFT];
static FoldCore core;
static int engines_4x;                            /* alias_report 4x: the engine voices at 4x (TEMPORARY switch) */

static void fft(double* xr, double* xi, int n) {
  for (int i = 1, j = 0; i < n; i++) {
    int bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) { double t = xr[i]; xr[i] = xr[j]; xr[j] = t; t = xi[i]; xi[i] = xi[j]; xi[j] = t; }
  }
  for (int len = 2; len <= n; len <<= 1) {
    const double a = -2.0 * M_PI / len;
    for (int i = 0; i < n; i += len)
      for (int k = 0; k < len / 2; k++) {
        const double wr = cos(a * k), wi = sin(a * k);
        const int p = i + k, q = i + k + len / 2;
        const double tr = xr[q] * wr - xi[q] * wi, ti = xr[q] * wi + xi[q] * wr;
        xr[q] = xr[p] - tr; xi[q] = xi[p] - ti;
        xr[p] += tr; xi[p] += ti;
      }
  }
}

typedef struct {
  const char* label;
  int source;                     /* FC_SRC_* */
  float h, c, m;                  /* Harmonics, Color, Morph */
  float ratio;                    /* Mod ratio (Ratio mode); lines at multiples of f / q */
  double q;
  float index; int shape;         /* Mod OSC into FM */
  float timbre, order, sym, drive;
  int sync;
} Case;

/* NFFT samples at 48 kHz of one held note (after 0.2 s of settling). */
static void render_note(float* out, int note, const Case* k) {
  fc_init(&core, (float)SR);
  FcParams p;
  fc_default_params(&p);
  p.gate_mode = FC_GATE_VCA; p.level = 1.0f; p.fg_rise_ms[0] = 1;   /* a held note, the gate open, no filter */
  int r = 0;
  while (r < FC_NRATIOS - 1 && fc_ratios[r] != k->ratio) r++;
  p.mod_mode = FC_MODE_RATIO; p.mod_freq = (float)r * FC_FREQ_MAX / (FC_NRATIOS - 1);
  p.mod_index = k->index; p.mod_shape = k->shape; p.route_fm = 1;
  p.timbre = k->timbre; p.order = k->order; p.symmetry = k->sym; p.drive = k->drive;
  p.source = k->source; p.harmonics = k->h; p.color = k->c; p.morph = k->m;
  p.route_sync = k->sync;
  p.engines_4x = engines_4x;
  fc_set_params(&core, &p);
  fc_note_on(&core, note, 127);
  static float r_[64], skip[64];
  for (int i = 0; i < SR / 5; i += 64) fc_process(&core, skip, r_, 64);
  for (int i = 0; i < NFFT; i += 64) fc_process(&core, out + i, r_, 64);
}

/* Aliasing in dB below the harmonics (lines at multiples of f0), to 16 kHz. */
static double alias_db(const float* x, double f0) {
  for (int i = 0; i < NFFT; i++) {
    const double a = 2.0 * M_PI * i / (NFFT - 1);             /* Blackman-Harris, sidelobes -92 dB */
    const double w = 0.35875 - 0.48829 * cos(a) + 0.14128 * cos(2 * a) - 0.01168 * cos(3 * a);
    re[i] = x[i] * w;
    im[i] = 0;
  }
  fft(re, im, NFFT);
  const double bin = (double)SR / NFFT;
  double good = 0, bad = 0;
  for (int k = 1; k < NFFT / 2; k++) {
    const double f = k * bin, pw = re[k] * re[k] + im[k] * im[k];
    if (f > 16000) break;
    if (fabs(f - f0 * floor(f / f0 + 0.5)) <= 6 * bin) good += pw;   /* main lobe of a line, some margin */
    else bad += pw;
  }
  return 10.0 * log10((bad + 1e-30) / (good + 1e-30));
}

int main(int argc, char** argv) {
  engines_4x = argc > 1 && strcmp(argv[1], "4x") == 0;
  static float x[NFFT];
  const int notes[4] = { 60, 72, 84, 96 };
#define S0 FC_SRC_SINE, 0, 0, 0
#define VA_SAW FC_SRC_VA, 0.5f, 1 / 3.0f, 1 / 3.0f
#define FM2_R1 FC_SRC_FM2, 30 / 128.0f
#define WAVE FC_SRC_WAVE
  const Case cases[] = {
    { "folder, Timbre 30%", S0, 1, 1, 0, FC_MOD_SINE, 0.3f, 0, 0, 0, 0 },
    { "folder, Timbre 70%", S0, 1, 1, 0, FC_MOD_SINE, 0.7f, 0, 0, 0, 0 },
    { "folder, Timbre 100%", S0, 1, 1, 0, FC_MOD_SINE, 1.0f, 0, 0, 0, 0 },
    { "folder, Timbre 70%, Symmetry +30", S0, 1, 1, 0, FC_MOD_SINE, 0.7f, 0, 0.3f, 0, 0 },
    { "folder 70% + FM sine ratio 3/2, Index 50%", S0, 1.5f, 2, 0.5f, FC_MOD_SINE, 0.7f, 0, 0, 0, 0 },
    { "Saw, Timbre 100%", S0, 1, 1, 0, FC_MOD_SINE, 1.0f, 0.5f, 0, 0, 0 },
    { "M, Timbre 100%, Drive 100%", S0, 1, 1, 0, FC_MOD_SINE, 1.0f, 1.0f, 0, 1.0f, 0 },
    { "FM saw ratio 1, Index 100% (no Timbre)", S0, 1, 1, 1.0f, FC_MOD_SAW, 0, 0, 0, 0, 0 },
    { "VA saw", VA_SAW, 1, 1, 0, FC_MOD_SINE, 0, 0, 0, 0, 0 },
    { "VA pulse 90%", FC_SRC_VA, 0.5f, 0.95f, 0.95f, 1, 1, 0, FC_MOD_SINE, 0, 0, 0, 0, 0 },
    { "VA saw + folder 70%", VA_SAW, 1, 1, 0, FC_MOD_SINE, 0.7f, 0, 0, 0, 0 },
    { "VA saw + FM sine ratio 1, Index 100%", VA_SAW, 1, 1, 1.0f, FC_MOD_SINE, 0, 0, 0, 0, 0 },
    { "VA saw + FM saw ratio 1, Index 100%", VA_SAW, 1, 1, 1.0f, FC_MOD_SAW, 0, 0, 0, 0, 0 },
    { "VA saw, Sync 3/2", VA_SAW, 1.5f, 2, 0, FC_MOD_SINE, 0, 0, 0, 0, 1 },
    { "FM2 ratio 1, Color 100%", FM2_R1, 1.0f, 0.5f, 1, 1, 0, FC_MOD_SINE, 0, 0, 0, 0, 0 },
    { "FM2 ratio 1, Color 50%, Sync 3/2", FM2_R1, 0.5f, 0.5f, 1.5f, 2, 0, FC_MOD_SINE, 0, 0, 0, 0, 1 },
    { "Wave middle", WAVE, 0.5f, 0.5f, 0.5f, 1, 1, 0, FC_MOD_SINE, 0, 0, 0, 0, 0 },
    { "Wave bright (H 10%, C 95%, M 50%)", WAVE, 0.1f, 0.95f, 0.5f, 1, 1, 0, FC_MOD_SINE, 0, 0, 0, 0, 0 },
    { "Wave middle, Sync 3/2", WAVE, 0.5f, 0.5f, 0.5f, 1.5f, 2, 0, FC_MOD_SINE, 0, 0, 0, 0, 1 },
  };
  printf("Aliasing to 16 kHz, dB below the harmonics (Sine voices at 2x, engines at %dx, ADAA on the folder).\n"
         "Lower is better; "
         "about -60 dB is inaudible, above -30 dB is clearly audible.\n\n", engines_4x ? FC_OS_HI : FC_OS);
  printf("%-44s %6s %6s %6s %6s\n", "", "C4", "C5", "C6", "C7");
  for (unsigned c = 0; c < sizeof cases / sizeof cases[0]; c++) {
    printf("%-44s", cases[c].label);
    for (int ni = 0; ni < 4; ni++) {
      const double f = 440.0 * pow(2.0, (notes[ni] - 69) / 12.0);
      render_note(x, notes[ni], &cases[c]);
      printf(" %6.0f", alias_db(x, f / cases[c].q));
    }
    printf("\n");
  }
  return 0;
}
