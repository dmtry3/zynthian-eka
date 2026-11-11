/* Aliasing of the FM (TZ 4.2, 6), linear through zero, at the plugin rate and at 2x: to decide on 2x
 * oversampling after the CPU measurement on the Pi. Phase modulation was compared here too and dropped on
 * 2026-11-11 (with a saw Mod OSC: -19 dB on C5 against -61 for linear FM). The 2x case is simulated here: the engine runs at 96 kHz, then a long windowed-sinc
 * low-pass (511 taps, Blackman, cut at 21 kHz, about -74 dB above 24 kHz) and every second sample; a halfband
 * filter in the plugin would be cheaper and somewhat weaker.
 * Metric: power outside the harmonics of the note (the only lines a periodic sound has; with Ratio p/q the
 * lines are at multiples of f/q), relative to the power on them, in dB. In the band to 16 kHz and in the whole band.
 * Usage: alias_report (prints a table; no pass/fail) */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/fold_core.h"

#define SR 48000
#define NFFT 65536
#define TAPS 511

static float buf[2 * NFFT + 2 * TAPS + 96000];
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

/* NFFT samples at 48 kHz of one held note into out[] (after 0.2 s of settling). */
static void render_note(float* out, int note, float ratio_units, float index, int shape, int os) {
  const int sr = SR * os;
  fc_init(&core, (float)sr);
  FcParams p;
  fc_default_params(&p);
  p.attack_ms = 1; p.sustain = 1.0f;
  p.mod_mode = FC_MODE_RATIO; p.mod_freq = ratio_units; p.mod_index = index; p.mod_shape = shape;
  p.route_fm = 1;
  fc_set_params(&core, &p);
  fc_note_on(&core, note, 127);
  static float r[64];
  for (int i = 0; i < sr / 5; i += 64) fc_process(&core, buf, r, 64);           /* settle */
  const int n = NFFT * os + (os > 1 ? TAPS : 0);
  for (int i = 0; i < n; i += 64) fc_process(&core, buf + i, r, 64);
  if (os == 1) { memcpy(out, buf, NFFT * sizeof(float)); return; }
  static double h[TAPS];
  static int made = 0;
  if (!made) {                                                               /* low-pass at 21 kHz for 96 kHz */
    const double fc = 21000.0 / (2.0 * SR);
    double sum = 0;
    for (int i = 0; i < TAPS; i++) {
      const double m = i - (TAPS - 1) / 2.0;
      const double sinc = m == 0 ? 2 * fc : sin(2 * M_PI * fc * m) / (M_PI * m);
      const double w = 0.42 - 0.5 * cos(2 * M_PI * i / (TAPS - 1)) + 0.08 * cos(4 * M_PI * i / (TAPS - 1));
      h[i] = sinc * w;
      sum += h[i];
    }
    for (int i = 0; i < TAPS; i++) h[i] /= sum;
    made = 1;
  }
  for (int k = 0; k < NFFT; k++) {
    double acc = 0;
    const float* x = buf + 2 * k;
    for (int i = 0; i < TAPS; i++) acc += h[i] * x[i];
    out[k] = (float)acc;
  }
}

/* Aliasing in dB below the harmonics (lines at multiples of f0): [0] to 16 kHz, [1] the whole band. */
static void alias_db(const float* x, double f0, double out[2]) {
  for (int i = 0; i < NFFT; i++) {
    const double a = 2.0 * M_PI * i / (NFFT - 1);             /* Blackman-Harris, sidelobes -92 dB */
    const double w = 0.35875 - 0.48829 * cos(a) + 0.14128 * cos(2 * a) - 0.01168 * cos(3 * a);
    re[i] = x[i] * w;
    im[i] = 0;
  }
  fft(re, im, NFFT);
  const double bin = (double)SR / NFFT;
  double good = 0, bad[2] = { 0, 0 };
  for (int k = 1; k < NFFT / 2; k++) {
    const double f = k * bin, pw = re[k] * re[k] + im[k] * im[k];
    const double near = fabs(f - f0 * floor(f / f0 + 0.5));
    if (near <= 6 * bin) good += pw;                          /* main lobe of a line: +-4 bins, some margin */
    else { bad[1] += pw; if (f < 16000) bad[0] += pw; }
  }
  for (int j = 0; j < 2; j++) out[j] = 10.0 * log10((bad[j] + 1e-30) / (good + 1e-30));
}

int main(void) {
  static float x[NFFT];
  const int notes[4] = { 60, 72, 84, 96 };
  const char* names[4] = { "C4", "C5", "C6", "C7" };
  int r1 = 0, r32 = 0;
  while (fc_ratios[r1] != 1.0f) r1++;
  while (fc_ratios[r32] != 1.5f) r32++;
  struct { const char* label; int ratio; double q; float index; int shape; } cases[] = {
    { "ratio 1, sine, Index 50%", r1, 1, 0.5f, FC_MOD_SINE },
    { "ratio 1, sine, Index 100%", r1, 1, 1.0f, FC_MOD_SINE },
    { "ratio 3/2, sine, Index 100%", r32, 2, 1.0f, FC_MOD_SINE },
    { "ratio 1, saw, Index 50%", r1, 1, 0.5f, FC_MOD_SAW },
    { "ratio 1, saw, Index 100%", r1, 1, 1.0f, FC_MOD_SAW },
    { "ratio 1, square, Index 50%", r1, 1, 0.5f, FC_MOD_SQUARE },
    { "ratio 1, triangle, Index 100%", r1, 1, 1.0f, FC_MOD_TRI },
  };
  printf("Aliasing, dB below the harmonics: to 16 kHz / whole band. Lower is better; about -60 dB is inaudible,\n"
         "above -30 dB is clearly audible. Linear FM; each cell: 1x | 2x.\n\n");
  for (unsigned c = 0; c < sizeof cases / sizeof cases[0]; c++) {
    printf("%s\n", cases[c].label);
    printf("  %-10s", "");
    for (int ni = 0; ni < 4; ni++) printf(" %-27s", names[ni]);
    printf("\n");
    printf("  %-10s", "");
    for (int ni = 0; ni < 4; ni++) {
      double a1[2], a2[2];
      const double f = 440.0 * pow(2.0, (notes[ni] - 69) / 12.0);
      const float u = (float)cases[c].ratio * FC_FREQ_MAX / (FC_NRATIOS - 1);
      render_note(x, notes[ni], u, cases[c].index, cases[c].shape, 1);
      alias_db(x, f / cases[c].q, a1);
      render_note(x, notes[ni], u, cases[c].index, cases[c].shape, 2);
      alias_db(x, f / cases[c].q, a2);
      printf(" %5.0f/%-5.0f | %5.0f/%-5.0f  ", a1[0], a1[1], a2[0], a2[1]);
    }
    printf("\n");
    printf("\n");
  }
  return 0;
}
