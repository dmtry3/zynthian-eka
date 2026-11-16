/* Tests of grain_fx without LV2. Renders a few examples into build_test/ as wav files to listen to.
 *   ./scripts/test.sh */
#include "../src/grain_fx.h"
#include "presets.inc"   /* build/, from scripts/gen_presets.py */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SR 48000.0
#define BLOCK 256   /* like Zynthian's jack period */

static int fails = 0, checks = 0;
#define CHECK(c, ...) do { checks++; if (!(c)) { printf("FAIL %s:%d: %s  ", __FILE__, __LINE__, #c); \
  printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static GrainFx fx;

static void write_wav(const char* path, const float* l, const float* r, int n) {
  FILE* f = fopen(path, "wb");
  if (!f) return;
  int bytes = n * 4;
  int hdr[] = { 0x46464952, 36 + bytes, 0x45564157, 0x20746d66, 16, 0x00020001, (int)SR, (int)SR * 4,
                0x00100004, 0x61746164, bytes };
  fwrite(hdr, 4, 11, f);
  for (int i = 0; i < n; i++) {
    short s[2] = { (short)(fmaxf(-1, fminf(1, l[i])) * 32767), (short)(fmaxf(-1, fminf(1, r[i])) * 32767) };
    fwrite(s, 2, 2, f);
  }
  fclose(f);
}

/* Runs n frames of a mono input through the effect, in blocks; params may change per block via cb. */
typedef void (*BlockCb)(GxParams* p, int frame);
static void run(GxParams* p, const float* in, float* ol, float* orr, int n, BlockCb cb, int check_heads) {
  for (int o = 0; o < n; o += BLOCK) {
    int m = n - o < BLOCK ? n - o : BLOCK;
    if (cb) cb(p, o);
    gx_process(&fx, p, in + o, in + o, ol + o, orr + o, m);
    if (check_heads) {
      int s; float back;
      for (int k = 0; gx_grain_info(&fx, k, &s, &back); k++)
        if (back <= 0.0f) { CHECK(back > 0.0f, "grain %d of stream %d reads ahead of the head: %g s", k, s, back); break; }
    }
  }
}

static float rms(const float* a, int n) {
  double s = 0;
  for (int i = 0; i < n; i++) s += (double)a[i] * a[i];
  return (float)sqrt(s / n);
}

/* Frequency of the strongest component near f0 (+- 1 semitone), by a fine DFT scan. */
static double peak_freq(const float* a, int n, double f0) {
  double best = 0, bf = f0;
  for (double c = -100; c <= 100; c += 0.5) {
    double f = f0 * pow(2.0, c / 1200.0), re = 0, im = 0;
    for (int i = 0; i < n; i++) {
      double wv = 0.5 - 0.5 * cos(2 * M_PI * i / (n - 1));
      re += wv * a[i] * cos(2 * M_PI * f * i / SR);
      im += wv * a[i] * sin(2 * M_PI * f * i / SR);
    }
    double m = re * re + im * im;
    if (m > best) { best = m; bf = f; }
  }
  return bf;
}

static float* sine(int n, double f, float amp) {
  float* a = malloc(sizeof(float) * n);
  for (int i = 0; i < n; i++) a[i] = amp * (float)sin(2 * M_PI * f * i / SR);
  return a;
}

static GxParams one_stream(void) {
  GxParams p;
  gx_default_params(&p);
  p.mix = 1.0f;
  for (int s = 0; s < GX_STREAMS; s++) p.s[s].pitch = 0;  /* neutral fixtures, independent of Init */
  return p;
}

static void test_dry(void) {
  int n = (int)SR;
  float* in = sine(n, 440, 0.5f);
  float* ol = malloc(sizeof(float) * n); float* orr = malloc(sizeof(float) * n);
  GxParams p = one_stream();
  p.mix = 0.0f;
  gx_reset(&fx);
  run(&p, in, ol, orr, n, NULL, 0);
  float worst = 0;
  for (int i = 0; i < n; i++) worst = fmaxf(worst, fabsf(ol[i] - in[i]));
  CHECK(worst < 1e-6f, "Mix 0: dry signal not untouched and without delay, worst diff %g", worst);
  free(in); free(ol); free(orr);
}

static void test_level_and_pitch(void) {
  int n = 4 * (int)SR;
  float* in = sine(n, 440, 0.5f);
  float* ol = malloc(sizeof(float) * n); float* orr = malloc(sizeof(float) * n);
  float pitches[] = { 0, 7, 12, -12, 24, -24 };
  for (unsigned k = 0; k < sizeof pitches / sizeof *pitches; k++) {
    /* the pitch of one grain: long grains far apart, measured inside one grain */
    GxParams p = one_stream();
    p.s[0].pitch = pitches[k];
    p.s[0].length_ms = 1000;
    p.s[0].rate_ms = 2000;
    p.s[0].offset_ms = 2000;          /* the grain reads input that is already there */
    gx_reset(&fx);
    run(&p, in, ol, orr, n, NULL, 1);
    /* the grain born at 2 s sounds 2.0 .. 3.0 s; its middle 0.5 s */
    int a = (int)(2.25 * SR), m = (int)(0.5 * SR);
    double want = 440 * pow(2.0, pitches[k] / 12.0);
    double cents = 1200 * log2(peak_freq(ol + a, m, want) / want);
    CHECK(fabs(cents) <= 5.0, "pitch %+g: %.2f cents off", pitches[k], cents);
  }
  /* overlapping grains: the level stays in a sane range at any pitch */
  for (unsigned k = 0; k < sizeof pitches / sizeof *pitches; k++) {
    GxParams p = one_stream();
    p.s[0].pitch = pitches[k];
    p.s[0].length_ms = 120;
    p.s[0].rate_ms = 60;
    gx_reset(&fx);
    run(&p, in, ol, orr, n, NULL, 1);
    int a = (int)SR, m = (int)SR;
    float lv = rms(ol + a, m) / rms(in + a, m);
    CHECK(lv > 0.2f && lv < 1.6f, "pitch %+g: level %.2f of the input", pitches[k], lv);
    if (pitches[k] == 12) write_wav("pitch_+12.wav", ol, orr, n);
  }
  /* pitch 0: grains read the same delayed signal, so the level stays close to the input */
  GxParams p = one_stream();
  gx_reset(&fx);
  run(&p, in, ol, orr, n, NULL, 1);
  float lv = rms(ol + (int)SR, (int)SR) / rms(in + (int)SR, (int)SR);
  CHECK(lv > 0.8f && lv < 1.25f, "pitch 0, Rate 100 Length 500: level %.2f of the input", lv);
  free(in); free(ol); free(orr);
}

/* Offset: the first grain sound comes Offset after the input starts. */
static void test_offset(void) {
  int n = (int)SR;
  float* in = calloc(n, sizeof(float));
  for (int i = 4800; i < n; i++) in[i] = 0.5f * (float)sin(2 * M_PI * 440 * i / SR);
  float* ol = malloc(sizeof(float) * n); float* orr = malloc(sizeof(float) * n);
  GxParams p = one_stream();
  p.s[0].offset_ms = 200;
  p.s[0].rate_ms = 5;
  p.s[0].length_ms = 20;
  gx_reset(&fx);
  run(&p, in, ol, orr, n, NULL, 1);
  int first = -1;
  for (int i = 0; i < n; i++) if (fabsf(ol[i]) > 0.01f) { first = i; break; }
  double ms = (first - 4800) * 1000.0 / SR;
  CHECK(first > 0 && ms > 185 && ms < 215, "Offset 200 ms: grains start %.1f ms after the input", ms);
  free(in); free(ol); free(orr);
}

/* Freeze keeps the sound after the input stops; without it, it dies away. No clicks around it. */
static int freeze_from, freeze_to;
static void freeze_cb(GxParams* p, int frame) { p->freeze = frame >= freeze_from && frame < freeze_to; }

static void test_freeze(void) {
  int n = 6 * (int)SR;
  float* in = sine(n, 440, 0.5f);
  for (int i = 2 * (int)SR; i < n; i++) in[i] = 0.0f;    /* input stops at 2 s */
  float* ol = malloc(sizeof(float) * n); float* orr = malloc(sizeof(float) * n);
  GxParams p = one_stream();
  p.s[0].offset_ms = 300; p.s[0].rate_ms = 40; p.s[0].length_ms = 150; p.s[0].pitch = 5; p.s[0].reverse = 0.3f;
  freeze_from = (int)(1.5 * SR); freeze_to = (int)(4.5 * SR);
  gx_reset(&fx);
  run(&p, in, ol, orr, n, freeze_cb, 1);
  write_wav("freeze.wav", ol, orr, n);
  float during = rms(ol + 3 * (int)SR, (int)SR / 2), after = rms(ol + (int)(5.5 * SR), (int)SR / 4);
  CHECK(during > 0.1f, "frozen sound after the input stopped: rms %g", during);
  CHECK(after < 1e-3f, "after Freeze off and silence, grains still sound: rms %g", after);
  /* clicks: the step between frames stays near what a 440 Hz sine at 0.5 has (0.029) times a few grains */
  float worst = 0; int at = 0;
  for (int i = 1; i < n; i++) { float d = fabsf(ol[i] - ol[i - 1]); if (d > worst) { worst = d; at = i; } }
  CHECK(worst < 0.12f, "a step of %g at %.3f s", worst, at / SR);
  free(in); free(ol); free(orr);
}

/* With zero Offset, sounding forward grains sit only a few samples behind the head.
 * Freezing must not cut them at full amplitude, even at the crest of a bass note. */
static void test_freeze_near_head(void) {
  int n = 3 * (int)SR;
  float* in = sine(n, 55, 0.5f);
  float* ol = malloc(sizeof(float) * n); float* orr = malloc(sizeof(float) * n);
  GxParams p = one_stream();
  p.s[0].offset_ms = 0; p.s[0].rate_ms = 10; p.s[0].length_ms = 200;
  freeze_from = 2 * (int)SR + BLOCK; freeze_to = n;
  gx_reset(&fx);
  run(&p, in, ol, orr, n, freeze_cb, 1);
  write_wav("freeze_near_head.wav", ol, orr, n);
  float worst = 0;
  for (int i = freeze_from; i < freeze_from + (int)(0.03 * SR); i++)
    worst = fmaxf(worst, fabsf(ol[i] - ol[i - 1]));
  CHECK(worst < 0.01f, "Freeze near the head cuts a bass note: largest step %g", worst);
  CHECK(fx.freeze_tail == 0 && fx.w == fx.frames + freeze_from + (int)(0.02 * SR),
        "Freeze must stop recording after its short transition, tail %d", fx.freeze_tail);
  free(in); free(ol); free(orr);
}

/* All streams, parameters at random and at their edges, Freeze flipping: no NaN, no runaway level. */
static unsigned seed = 1;
static float frand(void) { seed = seed * 1103515245u + 12345u; return (float)((seed >> 8) & 0xFFFF) / 65535.0f; }
static float pick(float lo, float hi) {
  float u = frand();
  return u < 0.1f ? lo : u > 0.9f ? hi : lo + (hi - lo) * frand();
}
/* The continuous Amount controls renewal; the button overrides it without changing its value. */
static void amount_cb(GxParams* p, int frame) {
  p->freeze_amount = frame >= freeze_from && frame < freeze_to ? 0.85f : 0;
}
static void test_partial_freeze(void) {
  float l[GX_CHUNK], r[GX_CHUNK], ol[GX_CHUNK], orr[GX_CHUNK];
  const float amounts[] = { 0, 0.5f, 0.9f };
  for (int mode = 0; mode < 3; mode++) {
    GxParams p = one_stream();
    p.buffer_s = 1;
    gx_reset(&fx);
    for (int i = 0; i < GX_CHUNK; i++) { l[i] = 0.25f; r[i] = -0.5f; }
    for (int n = 0; n < (int)SR; n += GX_CHUNK) gx_process(&fx, &p, l, r, ol, orr, GX_CHUNK);
    p.freeze_amount = amounts[mode];
    for (int i = 0; i < GX_CHUNK; i++) { l[i] = 0.75f; r[i] = 0.25f; }
    for (int n = 0; n < 3 * (int)SR; n += GX_CHUNK) gx_process(&fx, &p, l, r, ol, orr, GX_CHUNK);
    uint64_t center = ((fx.w - (int)SR / 2) & (fx.frames - 1)) * 2;
    float retained = powf(p.freeze_amount, 3);
    CHECK(fabsf(fx.buf[center] - (0.75f - 0.5f * retained)) < 1e-5f &&
          fabsf(fx.buf[center + 1] - (0.25f - 0.75f * retained)) < 1e-5f,
          "Amount %g: stereo renewal after three turns", p.freeze_amount);
    CHECK(!fx.frozen && fx.w == fx.frames + 4 * (int)SR,
          "intermediate Amount must keep recording new input");
    if (mode > 0) {
      p.buffer_s = 8;
      gx_process(&fx, &p, l, r, ol, orr, GX_CHUNK);
      CHECK(fx.hold_period == (uint64_t)SR, "renewal period stays stable while Amount is active");
    }
    p.freeze_amount = 0;
    for (int n = 0; n < (int)(0.03 * SR); n += GX_CHUNK) gx_process(&fx, &p, l, r, ol, orr, GX_CHUNK);
    CHECK(!fx.freeze_loop && !fx.frozen, "Amount 0 returns to live recording after a short fade");
  }

  GxParams p = one_stream();
  p.buffer_s = 1; p.freeze_amount = 0.4f;
  gx_reset(&fx);
  for (int n = 0; n < (int)(0.03 * SR); n += GX_CHUNK) gx_process(&fx, &p, l, r, ol, orr, GX_CHUNK);
  p.freeze = 1;
  for (int n = 0; n < (int)(0.03 * SR); n += GX_CHUNK) gx_process(&fx, &p, l, r, ol, orr, GX_CHUNK);
  uint64_t held = fx.w;
  gx_process(&fx, &p, l, r, ol, orr, GX_CHUNK);
  CHECK(fx.frozen && !fx.freeze_loop && fx.w == held, "button overrides Amount with a complete capture");
  p.freeze = 0;
  for (int n = 0; n < (int)(0.03 * SR); n += GX_CHUNK) gx_process(&fx, &p, l, r, ol, orr, GX_CHUNK);
  CHECK(!fx.frozen && fx.freeze_loop && fabsf(fx.hold_mix - 0.4f) < 1e-6f,
        "releasing the button returns to the unchanged 40 percent Amount");
  p.freeze_amount = 1;
  for (int n = 0; n < (int)(0.03 * SR); n += GX_CHUNK) gx_process(&fx, &p, l, r, ol, orr, GX_CHUNK);
  held = fx.w;
  for (uint64_t n = 0; n < fx.frames; n += GX_CHUNK) gx_process(&fx, &p, l, r, ol, orr, GX_CHUNK);
  CHECK(fx.frozen && fx.w == held, "Amount 100 percent stops all recording, even beyond a physical ring turn");

  int n = 8 * (int)SR;
  float* in = calloc(n, sizeof(float));
  float* out_l = malloc(n * sizeof(float)); float* out_r = malloc(n * sizeof(float));
  for (int i = 0; i < 4 * (int)SR; i++)
    in[i] = 0.4f * sinf((float)(2 * M_PI * (i < 2 * (int)SR ? 220 : 660) * i / SR));
  p = one_stream();
  p.buffer_s = 1;
  p.s[0].offset_ms = 120; p.s[0].rate_ms = 20; p.s[0].length_ms = 80;
  freeze_from = 2 * (int)SR; freeze_to = 7 * (int)SR;
  gx_reset(&fx);
  run(&p, in, out_l, out_r, n, amount_cb, 1);
  write_wav("partial_freeze.wav", out_l, out_r, n);
  float worst = 0;
  for (int i = 1; i < n; i++) worst = fmaxf(worst, fabsf(out_l[i] - out_l[i - 1]));
  CHECK(worst < 0.12f, "partial Freeze and release: largest audio step %g", worst);
  CHECK(rms(out_l + 5 * (int)SR, (int)SR) > 0.025f, "partially held audio remains after input stops");
  free(in); free(out_l); free(out_r);
}

static double youngest_start(int stream, double* inc) {
  const GxGrain* best = NULL;
  for (int i = 0; i < GX_POOL; i++) {
    const GxGrain* g = &fx.g[i];
    if (g->active && g->stream == stream && (!best || g->age < best->age)) best = g;
  }
  if (!best) { *inc = 0; return 0; }
  *inc = best->inc;
  return best->pos - best->inc * best->age;
}

static void test_scan(void) {
  float zero[GX_CHUNK] = {0}, l[GX_CHUNK], r[GX_CHUNK];
  GxParams p = one_stream();
  p.freeze = 1; p.s[0].on = 0;
  gx_reset(&fx);
  for (int i = 0; i < 24; i++) gx_process(&fx, &p, zero, zero, l, r, GX_CHUNK);
  p.s[0].on = 1; p.s[0].offset_ms = 1000; p.s[0].length_ms = 40; p.s[0].rate_ms = 100;
  p.s[0].scan = 0.5f;
  p.s[1] = p.s[0]; p.s[1].scan = -0.25f;
  gx_process(&fx, &p, zero, zero, l, r, GX_CHUNK);
  double inc[2], first[2];
  for (int s = 0; s < 2; s++) first[s] = youngest_start(s, &inc[s]);
  for (int i = 0; i < 75; i++) gx_process(&fx, &p, zero, zero, l, r, GX_CHUNK);
  for (int s = 0; s < 2; s++) {
    double start = youngest_start(s, &inc[s]);
    double want = p.s[s].scan * 0.1 * SR;
    CHECK(first[s] > 0 && fabs(start - first[s] - want) < 0.01,
          "stream %d: cursor moved %g frames, expected %g in 100 ms", s, start - first[s], want);
    CHECK(inc[s] == 1.0, "Scan must not transpose grains: stream %d speed %g", s, inc[s]);
  }
  double at = fx.scan_pos[0];
  p.s[0].offset_ms += 100;
  gx_process(&fx, &p, zero, zero, l, r, GX_CHUNK);
  CHECK(fabs(fx.scan_pos[0] - at + 0.1 * SR - 0.5 * GX_CHUNK) < 0.01,
        "Offset moves the scanning cursor without changing its speed");
  p.s[0].scan = 0;
  gx_process(&fx, &p, zero, zero, l, r, GX_CHUNK);
  CHECK(!fx.scan_active[0] && fx.scan_active[1], "Scan Off affects only its stream");

  /* Reverse, Spray, high pitches and long grains must stay inside a short frozen window. */
  p.buffer_s = 1;
  for (int s = 0; s < GX_STREAMS; s++) {
    p.s[s] = p.s[0]; p.s[s].on = 1; p.s[s].scan = s == 1 ? -2 : 2;
    p.s[s].length_ms = 8000; p.s[s].rate_ms = 10; p.s[s].pitch = s == 1 ? -24 : 24;
    p.s[s].reverse = s * 0.5f; p.s[s].spray = 1;
  }
  gx_reset(&fx);
  int bad = 0;
  for (int i = 0; i < (int)SR * 5; i += GX_CHUNK) {
    gx_process(&fx, &p, zero, zero, l, r, GX_CHUNK);
    if (fx.freeze_tail) continue;
    for (int k = 0; k < GX_POOL; k++) if (fx.g[k].active) {
      double age = (double)fx.w - fx.g[k].pos;
      if (age < 1 || age > SR + 1) bad++;
    }
  }
  CHECK(bad == 0, "scanning read escaped the captured window: %d grains", bad);
  CHECK(gx_min_offset_ms(&fx, &p, 0) < 1000, "minimum Offset reflects Scan's shortened grain length");
}

static void test_quantized_pitch(void) {
  float zero[GX_CHUNK] = {0}, l[GX_CHUNK], r[GX_CHUNK];
  static const struct { int mode; float pitch, note; } cases[] = {
    {9, 5, 0}, {9, 6, 12}, {9, -6, -12}, {9, -11, -12}, {9, 24, 24},
    {10, 5, 7}, {10, -3, -5}, {11, 3, 4}, {12, 4, 3}, {13, 3.2f, 4}
  };
  for (unsigned k = 0; k < sizeof cases / sizeof *cases; k++) {
    GxParams p = one_stream();
    p.s[0].interval = cases[k].mode; p.s[0].pitch = cases[k].pitch; p.s[0].fine = 10;
    p.s[0].length_ms = 50; p.s[0].offset_ms = 1500;
    gx_reset(&fx);
    gx_process(&fx, &p, zero, zero, l, r, GX_CHUNK);
    CHECK(fx.g[0].active && fx.g[0].pitch_locked &&
          fabs(12 * log2(fx.g[0].inc) - cases[k].note - 0.1) < 1e-5,
          "mode %d: Pitch %g snaps to %g, Fine stays +10 cents", cases[k].mode, cases[k].pitch, cases[k].note);
  }
  GxParams p = one_stream();
  p.s[0].interval = GX_Q_FIRST; p.s[0].offset_ms = 1500;
  p.s[0].length_ms = 50; p.s[0].rate_ms = 5;
  p.mod[0][0][GX_M_PITCH] = 1;
  p.lfo[0].shape = GX_LFO_TRI; p.lfo[0].rate = GX_LFO_MIN_HZ;
  gx_reset(&fx); fx.lfo_ph[0] = 0.49;
  gx_process(&fx, &p, zero, zero, l, r, GX_CHUNK);
  CHECK(fabs(fx.g[0].inc - 4) < 1e-6 && fx.pmax[0] == 24, "Q Oct modulation reaches two octaves up");
  fx.lfo_ph[0] = 0.99;
  for (int n = 0; n < 3; n++) gx_process(&fx, &p, zero, zero, l, r, GX_CHUNK);
  int up = 0, down = 0;
  for (int k = 0; k < GX_POOL; k++) if (fx.g[k].active) {
    up += fabs(fx.g[k].inc - 4) < 1e-6;
    down += fabs(fx.g[k].inc - 0.25) < 1e-6;
  }
  CHECK(up > 0 && down > 0, "sounding quantized grains hold their pitch while new grains take the new note");
  p.mod[1][0][GX_M_PITCH] = p.mod[2][0][GX_M_PITCH] = 1;
  p.lfo[1].shape = GX_LFO_TRI; p.lfo[1].rate = GX_LFO_MIN_HZ;
  gx_reset(&fx); fx.lfo_ph[0] = fx.lfo_ph[1] = 0.49;
  float loud[GX_CHUNK]; for (int i = 0; i < GX_CHUNK; i++) loud[i] = 0.9f;
  gx_process(&fx, &p, loud, loud, l, r, GX_CHUNK);
  CHECK(fx.g[0].active && fabs(fx.g[0].inc - 8) < 1e-6,
        "multiple quantized modulation sources clamp to +36 semitones");
}

static void stress_cb(GxParams* p, int frame) {
  (void)frame;
  if (frand() > 0.05f) return;
  for (int s = 0; s < GX_STREAMS; s++) {
    GxStream* t = &p->s[s];
    t->on = frand() > 0.3f;
    t->offset_ms = pick(0, 8000); t->rate_ms = pick(GX_MIN_RATE_MS, GX_MAX_RATE_MS);
    t->length_ms = pick(GX_MIN_LENGTH_MS, GX_MAX_LENGTH_MS); t->pitch = pick(-24, 24);
    t->fine = pick(-50, 50); t->reverse = pick(0, 1); t->filter = pick(-1, 1); t->spread = pick(0, 1); t->gain = gx_db(pick(-60, 6));
    t->scan = frand() > 0.3f ? pick(-2, 2) : 0;
  }
  p->mix = pick(0, 1); p->gain = gx_db(pick(-60, 6)); p->buffer_s = pick(1, 8);
  p->freeze = frand() > 0.7f; p->shape = frand();
  p->freeze_amount = pick(0, 1);
  for (int l = 0; l < GX_LFOS; l++) {
    p->lfo[l].rate = pick(GX_LFO_MIN_HZ, GX_LFO_MAX_HZ);
    p->lfo[l].shape = (int)(frand() * GX_NLFO_SHAPES) % GX_NLFO_SHAPES;
    p->lfo_rate_mod[l] = pick(-1, 1);
  }
  for (int l = 0; l < GX_MSRC; l++)
    for (int s = 0; s < GX_STREAMS; s++)
      for (int d = 0; d < GX_MDST; d++) p->mod[l][s][d] = frand() > 0.6f ? pick(-1, 1) : 0.0f;
  for (int s = 0; s < GX_STREAMS; s++) {
    p->s[s].spray = pick(0, 1);
    p->s[s].interval = (int)(frand() * GX_NINTERVALS) % GX_NINTERVALS;
  }
  p->feedback = pick(0, 0.95f);
  p->sync = frand() > 0.7f;
  gx_set_tempo(&fx, pick(20, 400), frand() > 0.8f ? pick(0, 1000) : -1.0);
}

static void test_stress(int seconds) {
  int n = seconds * (int)SR;
  float* in = malloc(sizeof(float) * n);
  for (int i = 0; i < n; i++) in[i] = (frand() * 2 - 1) * (i % 96000 < 48000 ? 1.0f : 0.0f);
  float* ol = malloc(sizeof(float) * n); float* orr = malloc(sizeof(float) * n);
  GxParams p = one_stream();
  gx_reset(&fx);
  run(&p, in, ol, orr, n, stress_cb, 1);
  float worst = 0; int bad = 0;
  for (int i = 0; i < n; i++) {
    if (!isfinite(ol[i]) || !isfinite(orr[i])) bad++;
    worst = fmaxf(worst, fmaxf(fabsf(ol[i]), fabsf(orr[i])));
  }
  CHECK(bad == 0, "%d frames not finite", bad);
  /* 3 streams at +6 dB, output +6 dB, full-scale noise: anything far above that is a bug */
  CHECK(worst < 20.0f, "peak %g", worst);
  CHECK(gx_active(&fx) <= GX_POOL, "pool overflow");
  free(in); free(ol); free(orr);
}

static void test_min_offset(void) {
  GxParams p;
  gx_default_params(&p);
  p.s[0].pitch = 12; p.s[0].length_ms = 500;
  gx_reset(&fx);
  float up = gx_min_offset_ms(&fx, &p, 0);
  CHECK(fabsf(up - 500.1f) < 0.1f, "+12, 500 ms grain: min Offset %g ms, want 500", up);
  p.s[0].pitch = 0;
  CHECK(gx_min_offset_ms(&fx, &p, 0) < 0.2f, "pitch 0: no extra Offset");
  fx.frozen = 1;
  CHECK(fabsf(gx_min_offset_ms(&fx, &p, 0) - 500.1f) < 0.1f, "frozen: the whole grain behind the head");
  fx.frozen = 0;
  p.s[0].pitch = 12; p.s[0].length_ms = 8000; p.buffer_s = 8;
  CHECK(fabsf(gx_min_offset_ms(&fx, &p, 0) - 4000.1f) < 0.1f, "8 s grain at +12 reads 8 s: shortened to 4 s");
  p.buffer_s = 2;
  CHECK(fabsf(gx_min_offset_ms(&fx, &p, 0) - 2000.1f) < 0.1f, "Length is capped by Buffer (2 s)");
}

/* LFO 1 on Pitch: at its top the grains read 2 semitones up, also while they sound. LFO 2 on Level: at its
 * bottom the stream is silent. */
static void test_lfo(void) {
  int n = 2 * (int)SR;
  float* in = sine(n, 440, 0.5f);
  float* ol = malloc(sizeof(float) * n); float* orr = malloc(sizeof(float) * n);
  GxParams p = one_stream();
  p.s[0].offset_ms = 200; p.s[0].rate_ms = 100; p.s[0].length_ms = 400;
  p.lfo[0].rate = 0.25f; p.lfo[0].shape = GX_LFO_SINE;   /* sine: top at 1 s */
  p.mod[0][0][GX_M_PITCH] = 1.0f;
  gx_reset(&fx);
  run(&p, in, ol, orr, n, NULL, 1);
  double want = 440 * pow(2.0, 2.0 / 12.0);
  double cents = 1200 * log2(peak_freq(ol + (int)(0.95 * SR), (int)(0.1 * SR), want) / want);
  CHECK(fabs(cents) <= 8.0, "LFO Pitch at its top: %.1f cents off +2 semitones", cents);
  write_wav("lfo_pitch.wav", ol, orr, n);

  p = one_stream();
  p.lfo[1].rate = 0.5f; p.lfo[1].shape = GX_LFO_TRI;     /* triangle from -1: bottom at 0 s, top at 1 s */
  p.mod[1][0][GX_M_LEVEL] = 1.0f;
  gx_reset(&fx);
  run(&p, in, ol, orr, n, NULL, 1);
  float low = rms(ol + (int)(0.6 * SR) - 2400, 4800), high = rms(ol + (int)(1.0 * SR) - 2400, 4800);
  CHECK(high > 0.25f && low < high * 0.85f, "LFO Level: rms %.3f near the middle, %.3f at the top", low, high);
  free(in); free(ol); free(orr);
}

/* The other LFO moves rate exponentially without resetting phase, including mutual links. */
static void test_lfo_rate_mod(void) {
  GxParams p = one_stream();
  float zero[GX_CHUNK] = {0}, l[GX_CHUNK], r[GX_CHUNK];
  p.lfo[0].rate = p.lfo[1].rate = 1;
  p.lfo_rate_mod[0] = p.lfo_rate_mod[1] = 1;
  gx_reset(&fx);
  fx.lfo_v[0] = 1; fx.lfo_v[1] = -1;
  gx_process(&fx, &p, zero, zero, l, r, GX_CHUNK);
  CHECK(fabs(fx.lfo_ph[0] - GX_CHUNK / (SR * 8)) < 1e-9,
        "negative source slows the other LFO eightfold");
  CHECK(fabs(fx.lfo_ph[1] - GX_CHUNK * 8 / SR) < 1e-9,
        "positive source speeds the other LFO eightfold, using the previous chunk");
  gx_reset(&fx);
  p.lfo_rate_mod[0] = -1;
  fx.lfo_v[1] = -1;
  gx_process(&fx, &p, zero, zero, l, r, GX_CHUNK);
  CHECK(fabs(fx.lfo_ph[0] - GX_CHUNK * 8 / SR) < 1e-9, "negative depth reverses the modulation");
  gx_reset(&fx);
  p.lfo[0].rate = GX_LFO_MAX_HZ; p.lfo_rate_mod[0] = 1; fx.lfo_v[1] = 1;
  gx_process(&fx, &p, zero, zero, l, r, GX_CHUNK);
  CHECK(fabs(fx.lfo_ph[0] - GX_CHUNK * GX_LFO_MAX_HZ / SR) < 1e-8, "modulated rate stays within its limit");
}

/* Intervals: grains of the "Oct" set sound at the played pitch and an octave up, nothing else. */
static void test_intervals(void) {
  int n = 3 * (int)SR;
  float* in = sine(n, 220, 0.5f);
  float* ol = malloc(sizeof(float) * n); float* orr = malloc(sizeof(float) * n);
  GxParams p = one_stream();
  p.s[0].interval = 1;
  p.s[0].offset_ms = 600; p.s[0].rate_ms = 40; p.s[0].length_ms = 200;
  gx_reset(&fx);
  run(&p, in, ol, orr, n, NULL, 1);
  write_wav("intervals.wav", ol, orr, n);
  /* energy at 220, 440, and at 330 (should be nearly none) */
  double e[3] = { 0, 0, 0 }, fr[3] = { 220, 440, 330 };
  int a = (int)SR, m = (int)SR;
  for (int k = 0; k < 3; k++) {
    double re = 0, im = 0;
    for (int i = 0; i < m; i++) {
      double wv = 0.5 - 0.5 * cos(2 * M_PI * i / (m - 1));
      re += wv * ol[a + i] * cos(2 * M_PI * fr[k] * i / SR);
      im += wv * ol[a + i] * sin(2 * M_PI * fr[k] * i / SR);
    }
    e[k] = re * re + im * im;
  }
  CHECK(e[0] > 0 && e[1] > e[0] * 0.2 && e[2] < e[0] * 0.01, "Oct: 220 %.3g, 440 %.3g, 330 %.3g", e[0], e[1], e[2]);
  free(in); free(ol); free(orr);
}

/* Spray: grains at a fixed Rate and a Pitch other than 0 make sidebands 1 / Rate apart (a buzz); Spray
 * spreads them out. Sine 440 Hz, +7 semitones (659 Hz), Rate 10 ms: sidebands at 559 and 759 Hz. */
static double band(const float* a, int m, double f) {
  double re = 0, im = 0;
  for (int i = 0; i < m; i++) {
    double wv = 0.5 - 0.5 * cos(2 * M_PI * i / (m - 1));
    re += wv * a[i] * cos(2 * M_PI * f * i / SR);
    im += wv * a[i] * sin(2 * M_PI * f * i / SR);
  }
  return re * re + im * im;
}

static void test_spray(void) {
  int n = 3 * (int)SR;
  float* in = sine(n, 440, 0.5f);
  float* ol = malloc(sizeof(float) * n); float* orr = malloc(sizeof(float) * n);
  double side[2];
  double f0 = 440 * pow(2.0, 7 / 12.0);
  for (int k = 0; k < 2; k++) {
    GxParams p = one_stream();
    p.s[0].pitch = 7; p.s[0].offset_ms = 100; p.s[0].rate_ms = 10; p.s[0].length_ms = 40; p.s[0].spray = k ? 1.0f : 0.0f;
    gx_reset(&fx);
    run(&p, in, ol, orr, n, NULL, 1);
    int a = (int)SR, m = (int)SR;
    side[k] = (band(ol + a, m, f0 - 100) + band(ol + a, m, f0 + 100)) / band(ol + a, m, f0);
  }
  CHECK(side[0] > 1e-3 && side[1] < side[0] * 0.5, "Spray: sidebands %.4f without, %.4f with", side[0], side[1]);
  free(in); free(ol); free(orr);
}

/* Feedback at its top with three streams up an octave at +6 dB: the loop stays bounded and dies away when
 * Feedback goes back to 0; with Feedback the tail lasts longer than without. */
static int fb_off_at;
static void fb_cb(GxParams* p, int frame) { if (frame >= fb_off_at) p->feedback = 0.0f; }

static void test_feedback(void) {
  int n = 8 * (int)SR;
  float* in = calloc(n, sizeof(float));
  for (int i = 0; i < (int)(0.5 * SR); i++) in[i] = 0.5f * (float)sin(2 * M_PI * 330 * i / SR);
  float* ol = malloc(sizeof(float) * n); float* orr = malloc(sizeof(float) * n);
  GxParams p = one_stream();
  for (int s = 0; s < GX_STREAMS; s++) {
    p.s[s].on = 1; p.s[s].pitch = 12; p.s[s].offset_ms = 300 + 100 * s; p.s[s].rate_ms = 30; p.s[s].length_ms = 150;
    p.s[s].gain = gx_db(6);
  }
  p.feedback = 0.95f;
  fb_off_at = 5 * (int)SR;
  gx_reset(&fx);
  run(&p, in, ol, orr, n, fb_cb, 1);
  write_wav("feedback.wav", ol, orr, n);
  float pk = 0;
  for (int i = 0; i < n; i++) pk = fmaxf(pk, fmaxf(fabsf(ol[i]), fabsf(orr[i])));
  float tail = rms(ol + (int)(3 * SR), (int)SR / 2), end = rms(ol + (int)(7.5 * SR), (int)SR / 2);
  CHECK(pk < 12.0f, "Feedback 95 %%: peak %.2f (3 streams at +6 dB: up to about 6 without feedback)", pk);
  CHECK(tail > 0.01f, "Feedback keeps a tail 2.5 s after the input stops: rms %.4f", tail);
  CHECK(end < 1e-3f, "Feedback back to 0: the tail dies away, rms %.5f", end);
  free(in); free(ol); free(orr);
}

/* Sync at 120 BPM: Rate 130 ms snaps to 1/16 (125 ms); grains start every 6000 frames. */
static void test_sync(void) {
  int n = 2 * (int)SR;
  float* in = calloc(n, sizeof(float));
  float* ol = malloc(sizeof(float) * n); float* orr = malloc(sizeof(float) * n);
  CHECK(gx_snap_div(130, 120) == 6 && gx_snap_div(500, 120) == 12 && gx_snap_div(170, 120) == 7,
        "snap: %d %d %d", gx_snap_div(130, 120), gx_snap_div(500, 120), gx_snap_div(170, 120));
  /* clicks in the input every frame: the output's first sound after each grain start shows the grid */
  for (int i = 0; i < n; i++) in[i] = 0.5f;
  GxParams p = one_stream();
  p.sync = 1; p.s[0].rate_ms = 130; p.s[0].length_ms = 20; p.s[0].offset_ms = 10; p.shape = 1;
  gx_reset(&fx);
  gx_set_tempo(&fx, 120, -1);
  run(&p, in, ol, orr, n, NULL, 1);
  int starts = 0, last = -1, bad = 0;
  for (int i = 1; i < n; i++)
    if (fabsf(ol[i - 1]) < 1e-4f && fabsf(ol[i]) >= 1e-4f) {
      if (last >= 0 && abs(i - last - 6000) > 2) bad++;
      last = i; starts++;
    }
  CHECK(starts >= 14 && bad == 0, "sync: %d grain starts, %d off the 1/16 grid", starts, bad);
  free(in); free(ol); free(orr);
}

/* Env on Level of stream 1 (+100): silent while the input is quiet, open when it is loud. */
static void test_env(void) {
  int n = 2 * (int)SR;
  float* in = sine(n, 440, 0.5f);
  for (int i = 0; i < (int)SR; i++) in[i] *= 0.002f;       /* first second about -60 dB */
  float* ol = malloc(sizeof(float) * n); float* orr = malloc(sizeof(float) * n);
  GxParams p = one_stream();
  p.mix = 1.0f;
  p.mod[GX_SRC_ENV][0][GX_M_LEVEL] = 1.0f;
  gx_reset(&fx);
  run(&p, in, ol, orr, n, NULL, 1);
  float quiet = rms(ol + (int)(0.6 * SR), (int)(0.3 * SR)), loud = rms(ol + (int)(1.6 * SR), (int)(0.3 * SR));
  CHECK(quiet < 1e-4f && loud > 0.1f, "Env on Level: rms %.5f quiet, %.3f loud", quiet, loud);
  free(in); free(ol); free(orr);
}

/* Every factory preset on plucks and drum hits; the Freeze Pad preset gets Freeze from 3 s on.
 * Renders to build_test/presets/<id>.wav. */
static int preset_freeze;
static void preset_cb(GxParams* p, int frame) { p->freeze = preset_freeze && frame >= 3 * (int)SR; }

static void test_presets(void) {
  int n = 6 * (int)SR;
  float* in = calloc(n, sizeof(float));
  static const double notes[] = { 220.0, 277.18, 329.63, 440.0, 329.63, 277.18 };
  for (int k = 0; k < 6; k++) {                       /* plucks every 0.5 s for 3 s */
    int at = k * (int)(0.5 * SR);
    for (int i = 0; i < (int)SR && at + i < n; i++) {
      double t = i / SR, ph = fmod(notes[k] * t, 1.0);
      in[at + i] += 0.35f * (float)((2 * ph - 1) * exp(-t * 4));
    }
  }
  for (int k = 0; k < 8; k++) {                       /* drum hits */
    int at = (int)(0.25 * SR) + k * (int)(0.375 * SR);
    for (int i = 0; i < 4000 && at + i < n; i++) in[at + i] += 0.5f * (frand() * 2 - 1) * (float)exp(-i / 600.0);
  }
  float* ol = malloc(sizeof(float) * n); float* orr = malloc(sizeof(float) * n);
  if (system("mkdir -p presets")) {}
  for (unsigned k = 0; k < sizeof presets / sizeof *presets; k++) {
    GxParams p;
    gx_default_params(&p);
    p.mix = presets[k].mix; p.feedback = presets[k].feedback; p.shape = presets[k].shape;
    p.buffer_s = (float)presets[k].buffer; p.sync = presets[k].sync;
    for (int s = 0; s < GX_STREAMS; s++) {
      GxStream* t = &p.s[s];
      t->on = presets[k].s[s].on; t->offset_ms = presets[k].s[s].offset; t->rate_ms = presets[k].s[s].rate;
      t->length_ms = presets[k].s[s].length; t->pitch = presets[k].s[s].pitch; t->fine = presets[k].s[s].fine;
      t->reverse = presets[k].s[s].reverse; t->filter = presets[k].s[s].filter; t->spread = presets[k].s[s].spread;
      t->gain = gx_db(presets[k].s[s].level_db);
      t->spray = presets[k].s[s].spray; t->interval = presets[k].s[s].interval;
      t->scan = presets[k].s[s].scan;
    }
    for (int l = 0; l < GX_LFOS; l++) {
      p.lfo[l].rate = presets[k].lfo[l].rate; p.lfo[l].shape = presets[k].lfo[l].shape;
      p.lfo_rate_mod[l] = presets[k].rate_mod[l];
    }
    p.env_attack_ms = presets[k].extra[0]; p.env_release_ms = presets[k].extra[1];
    p.env_sensitivity_db = presets[k].extra[2];
    for (int l = 0; l < GX_LFOS; l++) {
      p.lfo_smooth[l] = presets[k].extra[3 + l] * 0.01f;
      p.lfo_sync[l] = (int)presets[k].extra[5 + l];
      p.lfo_div[l] = (int)presets[k].extra[7 + l];
      p.lfo_mode[l] = (int)presets[k].extra[9 + l];
    }
    for (int m = 0; m < presets[k].nmods; m++)
      p.mod[presets[k].mods[m].l][presets[k].mods[m].s][presets[k].mods[m].d] = presets[k].mods[m].depth;
    preset_freeze = !strcmp(presets[k].id, "freeze_pad") || !strcmp(presets[k].id, "slow_motion");
    gx_reset(&fx);
    gx_set_tempo(&fx, 120, -1);
    run(&p, in, ol, orr, n, preset_cb, 1);
    char path[96];
    snprintf(path, sizeof path, "presets/%s.wav", presets[k].id);
    write_wav(path, ol, orr, n);
    float pk = 0; int bad = 0;
    for (int i = 0; i < n; i++) {
      if (!isfinite(ol[i]) || !isfinite(orr[i])) bad++;
      pk = fmaxf(pk, fmaxf(fabsf(ol[i]), fabsf(orr[i])));
    }
    CHECK(bad == 0 && pk < 1.5f, "preset %s: %d bad frames, peak %.2f", presets[k].id, bad, pk);
    /* the wet part alone is audible */
    p.mix = 1.0f;
    gx_reset(&fx);
    run(&p, in, ol, orr, n, preset_cb, 0);
    /* Afterglow ducks while you play: it is heard after the input stops */
    int after = !strcmp(presets[k].id, "afterglow");
    float wet = after ? rms(ol + 3 * (int)SR, (int)SR) : rms(ol, 3 * (int)SR);
    CHECK(wet > 0.02f, "preset %s: grains too quiet, rms %.3f", presets[k].id, wet);
    if (preset_freeze) {
      float held = rms(ol + (int)(4.5 * SR), (int)SR);
      CHECK(held > 0.02f, "%s: frozen sound after the input stops, rms %.3f", presets[k].id, held);
    }
    printf("  preset %-13s peak %.2f, grains rms %.3f\n", presets[k].id, pk, wet);
  }
  free(in); free(ol); free(orr);
}

static void test_filter_scan_mod(void) {
  GxParams p = one_stream(); p.lfo[0].shape = GX_LFO_SH; p.lfo[0].rate = 0.02f;
  p.mod[0][0][GX_M_SCAN] = 1;
  float in[64] = {0}, l[64], r[64];
  gx_reset(&fx); fx.lfo_b[0] = -1;
  gx_process(&fx, &p, in, in, l, r, 64);
  CHECK(fx.scan_active[0] && fx.sm[0] == -2, "Scan modulation enables reverse cursor from Scan OFF");
  double pos = fx.scan_pos[0]; fx.lfo_b[0] = 0;
  gx_process(&fx, &p, in, in, l, r, 64);
  CHECK(fx.scan_active[0] && fx.scan_pos[0] == pos && fx.sm[0] == 0, "Scan modulation holds cursor at zero without reset");
  fx.lfo_b[0] = 1;
  gx_process(&fx, &p, in, in, l, r, 64);
  CHECK(fx.sm[0] == 2 && fx.scan_pos[0] > pos, "Scan reverses direction continuously");
  int n = 3 * (int)SR; float* tone = sine(n, 3000, 0.3f);
  float* ol = malloc(sizeof(float) * n), *orr = malloc(sizeof(float) * n);
  p.mod[0][0][GX_M_SCAN] = 0; p.s[0].filter = -0.8f;
  gx_reset(&fx); run(&p, tone, ol, orr, n, NULL, 1);
  float closed = rms(ol + (int)SR, n - (int)SR);
  p.mod[0][0][GX_M_FILTER] = 1;
  gx_reset(&fx); fx.lfo_b[0] = 1; run(&p, tone, ol, orr, n, NULL, 1);
  CHECK(rms(ol + (int)SR, n - (int)SR) > closed * 3, "Filter modulation audibly opens low-pass");
  free(tone); free(ol); free(orr);
}

static void test_lfo_tempo(void) {
  GxParams p = one_stream(); p.lfo_sync[0] = 1; p.lfo_div[0] = 12;
  float in[64] = {0}, l[64], r[64];
  gx_reset(&fx); gx_set_tempo(&fx, 120, -1);
  gx_process(&fx, &p, in, in, l, r, 64);
  CHECK(fabs(fx.lfo_ph[0] - 2.0 * 64 / SR) < 1e-8, "quarter-note LFO at 120 BPM is 2 Hz");
  CHECK(fabs(fx.lfo_ph[1] - 64 / SR) < 1e-8, "second free LFO unaffected");
  double phase = fx.lfo_ph[0]; gx_set_tempo(&fx, 60, -1);
  gx_process(&fx, &p, in, in, l, r, 64);
  CHECK(fabs(fx.lfo_ph[0] - phase - 64 / SR) < 1e-8, "tempo change preserves phase");
  gx_reset(&fx); gx_set_tempo(&fx, 400, -1); p.lfo_div[0] = 0;
  gx_process(&fx, &p, in, in, l, r, 64);
  CHECK(fabs(fx.lfo_ph[0] - (400.0 / 60 * 16) * 64 / SR) < 1e-7, "1/64 at 400 BPM is not clipped to 20 Hz");
}

static void test_lfo_smooth(void) {
  GxParams p = one_stream(); p.lfo[0].rate = 1; p.lfo[0].shape = GX_LFO_SH;
  float in[64] = {0}, l[64], r[64];
  gx_reset(&fx); fx.lfo_b[0] = 1;
  gx_process(&fx, &p, in, in, l, r, 64);
  CHECK(fx.lfo_v[0] == 1, "Smooth 0 preserves immediate S&H step");
  gx_reset(&fx); fx.lfo_b[0] = 1; p.lfo_smooth[0] = 1;
  gx_process(&fx, &p, in, in, l, r, 64);
  CHECK(fx.lfo_v[0] > 0 && fx.lfo_v[0] < 0.003, "Smooth 100 softens S&H jump");
  double phase = fx.lfo_ph[0];
  for (int b = 1; b < 375; b++) gx_process(&fx, &p, in, in, l, r, 64);
  CHECK(fabsf(fx.lfo_v[0] - 0.63212f) < 0.001, "Smooth 100 follows half-period time constant");
  p.lfo_mode[0] = 1;
  gx_process(&fx, &p, in, in, l, r, 64);
  CHECK(fx.lfo_v[0] >= 0 && fx.lfo_v[0] <= 1 && fx.lfo_ph[0] > phase, "smoothing retains positive range and running phase");
}

static void test_env_controls(void) {
  GxParams p = one_stream(); p.mix = 0;
  p.env_attack_ms = p.env_release_ms = 100;
  float in[64], l[64], r[64];
  gx_reset(&fx);
  for (int i = 0; i < 64; i++) in[i] = 1;
  for (int b = 0; b < 75; b++) gx_process(&fx, &p, in, in, l, r, 64);
  CHECK(fabsf(fx.env - 0.63212f) < 0.003, "Env attack follows 100 ms time constant: %.4f", fx.env);
  float peak = fx.env;
  memset(in, 0, sizeof in);
  for (int b = 0; b < 75; b++) gx_process(&fx, &p, in, in, l, r, 64);
  CHECK(fabsf(fx.env / peak - 0.36788f) < 0.003, "Env release follows 100 ms time constant");
  p.env_attack_ms = 0;
  for (int i = 0; i < 64; i++) in[i] = 0.01f;
  gx_reset(&fx); gx_process(&fx, &p, in, in, l, r, 64);
  float base = fx.env_v;
  p.env_sensitivity_db = 12;
  gx_process(&fx, &p, in, in, l, r, 64);
  CHECK(fabsf(fx.env_v - base - 12.0f / 42.0f) < 0.001, "Sensitivity boosts only modulation follower");
  CHECK(fabsf(l[63] - in[63]) < 1e-6f, "Sensitivity does not boost dry audio");
}

static void test_shape_morph(void) {
  int n = 3 * (int)SR;
  float* in = sine(n, 220, 0.3f);
  float* l = malloc(sizeof(float) * n), *r = malloc(sizeof(float) * n);
  for (int k = 0; k <= 4; k++) {
    GxParams p = one_stream(); p.shape = k * 0.25f;
    gx_reset(&fx); run(&p, in, l, r, n, NULL, 1);
    float level = rms(l + (int)SR, n - (int)SR) / rms(in + (int)SR, n - (int)SR);
    CHECK(level > 0.85f && level < 1.15f, "Shape %.2f keeps steady grain level: %.3f", p.shape, level);
    for (int g = 0; g < GX_POOL; g++) if (fx.g[g].active) {
      float before = fx.g[g].win_step;
      p.shape = 1 - p.shape;
      float a[1], b[1]; gx_process(&fx, &p, in, in, a, b, 1);
      CHECK(fx.g[g].win_step == before, "sounding grain retains its envelope");
      break;
    }
  }
  free(in); free(l); free(r);
}

static void test_spread_width(void) {
  int n = 8 * (int)SR;
  float* in = sine(n, 440, 0.3f);
  float* l = malloc(sizeof(float) * n), *r = malloc(sizeof(float) * n);
  for (int wide = 0; wide < 2; wide++) {
    GxParams p = one_stream();
    p.s[0].spread = (float)wide;
    gx_reset(&fx);
    run(&p, in, l, r, n, NULL, 1);
    double mid = 0, side = 0, ll = 0, rr = 0;
    for (int i = 2 * (int)SR; i < n; i++) {
      mid += (l[i] + r[i]) * (l[i] + r[i]);
      side += (l[i] - r[i]) * (l[i] - r[i]);
      ll += l[i] * l[i]; rr += r[i] * r[i];
    }
    if (!wide) CHECK(side < mid * 1e-10, "Spread 0 preserves mono");
    else {
      CHECK(side > mid * 0.35, "Spread 100: audible width without Spray (Side/Mid %.1f dB)", 10 * log10(side / mid));
      CHECK(ll > rr * 0.5 && rr > ll * 0.5, "wide output remains balanced");
      CHECK(mid / (n - 2 * SR) > 0.002, "wide output survives mono fold-down");
      write_wav("spread_wide.wav", l, r, n);
    }
  }
  free(in); free(l); free(r);
}

int main(int argc, char** argv) {
  int stress = argc > 1 ? atoi(argv[1]) : 30;
  if (gx_init(&fx, SR)) { printf("FAIL: no memory\n"); return 1; }
  test_min_offset();
  test_dry();
  test_offset();
  test_level_and_pitch();
  test_spread_width();
  test_shape_morph();
  test_freeze();
  test_freeze_near_head();
  test_partial_freeze();
  test_scan();
  test_quantized_pitch();
  test_lfo();
  test_lfo_rate_mod();
  test_lfo_smooth();
  test_lfo_tempo();
  test_filter_scan_mod();
  test_intervals();
  test_spray();
  test_feedback();
  test_sync();
  test_env();
  test_env_controls();
  test_stress(stress);
  test_presets();
  gx_free(&fx);
  printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "OK", checks, fails);
  return fails ? 1 : 0;
}
