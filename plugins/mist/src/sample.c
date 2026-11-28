#include "sample.h"
#include <math.h>
#include <sndfile.h>
#include <stdlib.h>
#include <string.h>

#define CHUNK 4096

/* Half-band low-pass for decimation by 2: Kaiser-windowed sinc, cutoff at the new Nyquist.
 * Every other tap is zero, so only HB_HALF taps per side are used, plus the centre 0.5. */
#define HB_HALF 16            /* 4*16-1 = 63 taps */

static double bessel_i0(double x) {
  double s = 1.0, t = 1.0;
  for (int k = 1; k < 30; k++) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; }
  return s;
}

/* hb[k]: tap at offset +-(2k+1). Computed per call (16 values): no shared state between threads. */
static void hb_taps(float* hb) {
  const double beta = 8.0, m = 2.0 * HB_HALF;    /* window half-length in taps */
  for (int k = 0; k < HB_HALF; k++) {
    const int o = 2 * k + 1;
    const double x = (double)o / m;
    const double w = bessel_i0(beta * sqrt(1.0 - x * x)) / bessel_i0(beta);
    hb[k] = (float)(sin(M_PI * o / 2.0) / (M_PI * o) * w);
  }
}

void gs_decimate2(const float* in, int64_t n, float* out) {
  float hb[HB_HALF];
  hb_taps(hb);
  const int64_t m = n / 2;
  for (int64_t i = 0; i < m; i++) {
    const int64_t c = 2 * i;
    float l = 0.5f * in[2 * c], r = 0.5f * in[2 * c + 1];
    for (int k = 0; k < HB_HALF; k++) {
      const int64_t o = 2 * k + 1;
      const int64_t a = c - o, b = c + o;
      /* the sample is treated as a loop, like grains read it */
      const int64_t ia = a >= 0 ? a : ((a % n) + n) % n, ib = b < n ? b : b % n;
      l += hb[k] * (in[2 * ia] + in[2 * ib]);
      r += hb[k] * (in[2 * ia + 1] + in[2 * ib + 1]);
    }
    out[2 * i] = l;
    out[2 * i + 1] = r;
  }
}

static void add_guard(float* d, int64_t frames) {
  for (int64_t i = 0; i < GS_GUARD; i++) {
    d[2 * (frames + i)] = d[2 * (i % frames)];
    d[2 * (frames + i) + 1] = d[2 * (i % frames) + 1];
  }
}

/* ---------- pitch detection (YIN, de Cheveigne & Kawahara 2002) ---------- */
#define PD_WINDOWS 9
#define PD_FMIN    30.0     /* Hz */
#define PD_FMAX    2000.0
#define PD_THRESH  0.15     /* YIN absolute threshold: below it the period is trusted */

/* Period in samples of x[0 .. n + maxlag), or 0. d: scratch of maxlag + 1. */
static double yin_period(const float* x, int n, int minlag, int maxlag, double* d) {
  double run = 0.0;
  d[0] = 1.0;
  for (int t = 1; t <= maxlag; t++) {
    double sum = 0.0;
    for (int i = 0; i < n; i++) { const double e = (double)x[i] - x[i + t]; sum += e * e; }
    run += sum;
    d[t] = run > 0.0 ? sum * t / run : 1.0;    /* cumulative mean normalized difference */
  }
  int best = -1;
  for (int t = minlag; t < maxlag; t++)
    if (d[t] < PD_THRESH) {                   /* first dip under the threshold, then down to its bottom */
      while (t + 1 < maxlag && d[t + 1] < d[t]) t++;
      best = t;
      break;
    }
  if (best < 1) return 0.0;
  const double a = d[best - 1], b = d[best], c = d[best + 1], den = a - 2.0 * b + c;
  const double sh = den != 0.0 ? 0.5 * (a - c) / den : 0.0;   /* parabolic: a fraction of a sample */
  return best + (sh > -1.0 && sh < 1.0 ? sh : 0.0);
}

static int cmp_float(const void* a, const void* b) {
  const float x = *(const float*)a, y = *(const float*)b;
  return x < y ? -1 : x > y;
}

float gs_detect_pitch(const float* data, int64_t frames, double rate) {
  const int maxlag = (int)(rate / PD_FMIN), minlag = (int)(rate / PD_FMAX) > 2 ? (int)(rate / PD_FMAX) : 2;
  const int n = 2 * maxlag;                   /* two periods of the lowest pitch */
  if (frames < (int64_t)(n + maxlag) * 2) return -1.0f;
  float* x = (float*)malloc((size_t)(n + maxlag) * sizeof(float));
  double* d = (double*)malloc((size_t)(maxlag + 1) * sizeof(double));
  float found[PD_WINDOWS];
  int nf = 0;
  if (x && d)
    for (int w = 0; w < PD_WINDOWS; w++) {
      /* windows from 10 % to 70 %: past the attack, before the tail */
      const int64_t st = (int64_t)((double)(frames - n - maxlag) * (0.1 + 0.6 * w / (PD_WINDOWS - 1)));
      double e = 0.0;
      for (int i = 0; i < n + maxlag; i++) {
        x[i] = 0.5f * (data[2 * (st + i)] + data[2 * (st + i) + 1]);
        e += (double)x[i] * x[i];
      }
      if (e / (n + maxlag) < 1e-8) continue;  /* silence (-80 dB) */
      const double per = yin_period(x, n, minlag, maxlag, d);
      if (per > 0.0) found[nf++] = (float)(69.0 + 12.0 * log2(rate / per / 440.0));
    }
  free(x);
  free(d);
  if (nf < (PD_WINDOWS + 1) / 2) return -1.0f;   /* most windows must have a clear period */
  qsort(found, (size_t)nf, sizeof(float), cmp_float);
  const float med = found[nf / 2];
  int agree = 0;
  double sum = 0.0;
  for (int i = 0; i < nf; i++)
    if (fabsf(found[i] - med) < 0.5f) { agree++; sum += found[i]; }
  if (agree < PD_WINDOWS - 2) return -1.0f;          /* a melody or a chord: no single pitch */
  return (float)(sum / agree);
}

static void detect_pitch_windows(GsSample* s, const float* data, int64_t frames, double rate) {
  s->npitch_wins = 0;
  const int maxlag = (int)(rate / PD_FMIN), minlag = (int)(rate / PD_FMAX) > 2 ? (int)(rate / PD_FMAX) : 2;
  const int n = 2 * maxlag, win = n + maxlag;
  if (frames < (int64_t)win) return;
  float* x = (float*)malloc((size_t)win * sizeof(float));
  double* d = (double*)malloc((size_t)(maxlag + 1) * sizeof(double));
  if (!x || !d) { free(x); free(d); return; }
  for (int w = 0; w < GS_PITCH_WINS; w++) {
    const double frac = GS_PITCH_WINS > 1 ? (double)w / (GS_PITCH_WINS - 1) : 0.5;
    const int64_t st = (int64_t)((double)(frames - win) * frac);
    double e = 0.0;
    for (int i = 0; i < win; i++) {
      x[i] = 0.5f * (data[2 * (st + i)] + data[2 * (st + i) + 1]);
      e += (double)x[i] * x[i];
    }
    float p = -1.0f;
    if (e / win >= 1e-8) {
      const double per = yin_period(x, n, minlag, maxlag, d);
      if (per > 0.0) p = (float)(69.0 + 12.0 * log2(rate / per / 440.0));
    }
    s->pitch_win[w] = p;
    s->pitch_pos[w] = (float)(((double)st + 0.5 * (double)win) / (double)frames);
  }
  s->npitch_wins = GS_PITCH_WINS;
  free(x);
  free(d);
}

float gs_region_pitch(const GsSample* s, float mark_start, float mark_end) {
  if (!s) return -1.0f;
  float a = mark_start < 0.0f ? 0.0f : mark_start > 1.0f ? 1.0f : mark_start;
  float b = mark_end < 0.0f ? 0.0f : mark_end > 1.0f ? 1.0f : mark_end;
  if (b < a) { const float t = a; a = b; b = t; }
  if (a <= 0.001f && b >= 0.999f) return s->pitch;
  if (s->npitch_wins <= 0) return s->pitch;
  float found[GS_PITCH_WINS];
  int total = 0, nf = 0;
  for (int i = 0; i < s->npitch_wins; i++) {
    if (s->pitch_pos[i] >= a && s->pitch_pos[i] <= b) {
      total++;
      if (s->pitch_win[i] >= 0.0f) found[nf++] = s->pitch_win[i];
    }
  }
  if (total == 0) {
    /* Narrow marker region: fall back to the window closest to its centre. */
    const float mid = 0.5f * (a + b);
    int best = 0;
    float bd = fabsf(s->pitch_pos[0] - mid);
    for (int i = 1; i < s->npitch_wins; i++) {
      const float d = fabsf(s->pitch_pos[i] - mid);
      if (d < bd) { bd = d; best = i; }
    }
    return s->pitch_win[best];
  }
  if (nf == 0 || nf * 2 < total) return -1.0f;
  /* Simple insertion sort (at most 16 elements, realtime-safe). */
  for (int i = 1; i < nf; i++) {
    const float v = found[i];
    int j = i - 1;
    while (j >= 0 && found[j] > v) { found[j + 1] = found[j]; j--; }
    found[j + 1] = v;
  }
  const float med = found[nf / 2];
  int agree = 0;
  double sum = 0.0;
  for (int i = 0; i < nf; i++)
    if (fabsf(found[i] - med) < 0.5f) { agree++; sum += found[i]; }
  if (agree * 3 < total * 2) return -1.0f;
  return (float)(sum / agree);
}

void gs_sample_free(GsSample* s) {
  if (!s) return;
  for (int k = 0; k < GS_LEVELS; k++) free(s->data[k]);
  free(s);
}

GsSample* gs_sample_load(const char* path, double max_seconds, int* err) {
  int e = GS_LOAD_OK;
  GsSample* s = NULL;
  float* tmp = NULL;
  SNDFILE* f = NULL;
  SF_INFO info;
  memset(&info, 0, sizeof info);
  if (!path || !*path || strlen(path) >= GS_PATH_MAX) { e = GS_LOAD_PATH; goto out; }
  f = sf_open(path, SFM_READ, &info);
  if (!f) { e = GS_LOAD_OPEN; goto out; }
  if (info.frames <= 0 || info.channels <= 0 || info.samplerate <= 0) { e = GS_LOAD_EMPTY; goto out; }
  if ((double)info.frames > max_seconds * info.samplerate) { e = GS_LOAD_TOO_LONG; goto out; }

  const int ch = info.channels;
  s = (GsSample*)calloc(1, sizeof(GsSample));
  tmp = (float*)malloc((size_t)CHUNK * ch * sizeof(float));
  if (!s || !tmp) { e = GS_LOAD_NOMEM; goto out; }
  s->frames[0] = info.frames;
  s->data[0] = (float*)malloc((size_t)(info.frames + GS_GUARD) * 2 * sizeof(float));
  if (!s->data[0]) { e = GS_LOAD_NOMEM; goto out; }

  float* d = s->data[0];
  int64_t n = 0;
  while (n < info.frames) {
    sf_count_t want = info.frames - n < CHUNK ? info.frames - n : CHUNK;
    sf_count_t got = sf_readf_float(f, tmp, want);
    if (got <= 0) break;
    float* o = d + 2 * n;
    if (ch == 1) for (sf_count_t i = 0; i < got; i++) { o[2 * i] = o[2 * i + 1] = tmp[i]; }
    else for (sf_count_t i = 0; i < got; i++) { o[2 * i] = tmp[i * ch]; o[2 * i + 1] = tmp[i * ch + 1]; }
    n += got;
  }
  if (n == 0) { e = GS_LOAD_EMPTY; goto out; }
  /* The header may promise more frames than the file holds: zero the rest (this also touches its pages). */
  memset(d + 2 * n, 0, (size_t)(info.frames - n) * 2 * sizeof(float));
  add_guard(d, info.frames);
  for (int64_t i = 0; i < 2 * info.frames; i++) if (fabsf(d[i]) > s->peak) s->peak = fabsf(d[i]);

  for (int k = 1; k < GS_LEVELS; k++) {
    const int64_t m = s->frames[k - 1] / 2;
    if (m < 4) break;                                  /* too short: grains stay on the levels there are */
    s->data[k] = (float*)malloc((size_t)(m + GS_GUARD) * 2 * sizeof(float));
    if (!s->data[k]) { e = GS_LOAD_NOMEM; goto out; }
    gs_decimate2(s->data[k - 1], s->frames[k - 1], s->data[k]);
    s->frames[k] = m;
    add_guard(s->data[k], m);
  }
  s->channels = ch;
  s->rate = info.samplerate;
  /* pitch on a level near 48 kHz or below: the cost grows with the square of the rate */
  int pk = 0;
  while (pk + 1 < GS_LEVELS && s->data[pk + 1] && s->rate / (1 << pk) > 60000.0) pk++;
  s->pitch = gs_detect_pitch(s->data[pk], s->frames[pk], s->rate / (1 << pk));
  detect_pitch_windows(s, s->data[pk], s->frames[pk], s->rate / (1 << pk));
  strcpy(s->path, path);

out:
  if (f) sf_close(f);
  free(tmp);
  if (e != GS_LOAD_OK) { gs_sample_free(s); s = NULL; }
  if (err) *err = e;
  return s;
}
