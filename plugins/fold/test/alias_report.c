/* Aliasing of the folder, the other shapers and FM in the four anti-aliasing modes of the engine (stage 2):
 * the plugin rate (1x), ADAA on the folder, the voices at 2x (halfband decimator), 2x with ADAA.
 * Metric: power outside the harmonics of the note (the only lines a periodic sound has; with Ratio p/q the lines
 * are at multiples of f/q), relative to the power on them, in dB, in the band to 16 kHz.
 * Also the cost of ADAA: it averages over a sample, a gentle low-pass on everything.
 * Phase modulation was compared here at stage 1 and dropped on 2026-11-11 (saw Mod OSC: -19 dB on C5 against
 * -61 for linear FM).
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
  float ratio;                    /* Mod ratio (Ratio mode); lines at multiples of f / q */
  double q;
  float index; int shape;         /* Mod OSC into FM */
  float timbre, order, sym, drive;
} Case;

/* NFFT samples at 48 kHz of one held note (after 0.2 s of settling). */
static void render_note(float* out, int note, const Case* k, int aa) {
  fc_init(&core, (float)SR);
  fc_set_aa(&core, aa);
  FcParams p;
  fc_default_params(&p);
  p.attack_ms = 1; p.sustain = 1.0f;
  int r = 0;
  while (r < FC_NRATIOS - 1 && fc_ratios[r] != k->ratio) r++;
  p.mod_mode = FC_MODE_RATIO; p.mod_freq = (float)r * FC_FREQ_MAX / (FC_NRATIOS - 1);
  p.mod_index = k->index; p.mod_shape = k->shape; p.route_fm = 1;
  p.timbre = k->timbre; p.order = k->order; p.symmetry = k->sym; p.drive = k->drive;
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

static double level(const float* x, double f) {          /* amplitude of one component (Hann, one DFT bin) */
  double a = 0, b = 0, ws = 0;
  for (int i = 0; i < NFFT; i++) {
    const double w = 0.5 - 0.5 * cos(2 * M_PI * i / NFFT), ph = 2 * M_PI * f * i / SR;
    a += w * x[i] * cos(ph); b += w * x[i] * sin(ph); ws += w;
  }
  return 2 * sqrt(a * a + b * b) / ws;
}

int main(void) {
  static float x[NFFT];
  const int notes[4] = { 60, 72, 84, 96 };
  const Case cases[] = {
    { "folder, Timbre 30%", 1, 1, 0, FC_MOD_SINE, 0.3f, 0, 0, 0 },
    { "folder, Timbre 70%", 1, 1, 0, FC_MOD_SINE, 0.7f, 0, 0, 0 },
    { "folder, Timbre 100%", 1, 1, 0, FC_MOD_SINE, 1.0f, 0, 0, 0 },
    { "folder, Timbre 70%, Symmetry +30", 1, 1, 0, FC_MOD_SINE, 0.7f, 0, 0.3f, 0 },
    { "folder 70% + FM sine ratio 3/2, Index 50%", 1.5f, 2, 0.5f, FC_MOD_SINE, 0.7f, 0, 0, 0 },
    { "Saw, Timbre 100%", 1, 1, 0, FC_MOD_SINE, 1.0f, 0.5f, 0, 0 },
    { "M, Timbre 100%, Drive 100%", 1, 1, 0, FC_MOD_SINE, 1.0f, 1.0f, 0, 1.0f },
    { "FM saw ratio 1, Index 100% (no Timbre)", 1, 1, 1.0f, FC_MOD_SAW, 0, 0, 0, 0 },
  };
  const char* modes[4] = { "1x", "ADAA", "2x", "2x+ADAA" };
  printf("Aliasing to 16 kHz, dB below the harmonics. Lower is better; about -60 dB is inaudible,\n"
         "above -30 dB is clearly audible.\n\n");
  printf("%-44s %-8s %6s %6s %6s %6s\n", "", "", "C4", "C5", "C6", "C7");
  for (unsigned c = 0; c < sizeof cases / sizeof cases[0]; c++) {
    for (int m = 0; m < 4; m++) {
      printf("%-44s %-8s", m == 0 ? cases[c].label : "", modes[m]);
      for (int ni = 0; ni < 4; ni++) {
        const double f = 440.0 * pow(2.0, (notes[ni] - 69) / 12.0);
        render_note(x, notes[ni], &cases[c], m);
        printf(" %6.0f", alias_db(x, f / cases[c].q));
      }
      printf("\n");
    }
  }
  /* what ADAA costs: the level of a plain sine (Timbre 0) at high frequencies, against 1x */
  printf("\nLevel of a plain sine against 1x, dB (ADAA averages over a sample):\n");
  const Case plain = { "", 1, 1, 0, FC_MOD_SINE, 0, 0, 0, 0 };
  const int hi[3] = { 108, 115, 120 };                    /* C8 4.2 kHz, G8 6.3 kHz, C9 8.4 kHz */
  for (int m = 1; m < 4; m++) {
    printf("  %-8s", modes[m]);
    for (int ni = 0; ni < 3; ni++) {
      const double f = 440.0 * pow(2.0, (hi[ni] - 69) / 12.0);
      render_note(x, hi[ni], &plain, 0);
      const double l0 = level(x, f);
      render_note(x, hi[ni], &plain, m);
      printf("  %.0f Hz %+.2f", f, 20 * log10(level(x, f) / l0));
    }
    printf("\n");
  }
  return 0;
}
