/* fold_core without LV2: pitch, envelope, voices, clicks. Writes chord.wav and steal.wav to listen to.
 * Usage: test_core (run from build_test/) */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/fold_core.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

#define SR 48000
#define BLOCK 64
#define MAXN (SR * 4)

static float L[MAXN], R[MAXN];
static FoldCore core;

/* Renders n samples at buf + at, in blocks like a host. */
static void render(FoldCore* c, uint32_t at, uint32_t n) {
  for (uint32_t i = 0; i < n; i += BLOCK) {
    const uint32_t k = n - i < BLOCK ? n - i : BLOCK;
    fc_process(c, L + at + i, R + at + i, k);
  }
}

/* Frequency from rising zero crossings (linear interpolation) in [a, b). */
static double freq(const float* x, uint32_t a, uint32_t b, double sr) {
  double first = -1, last = -1;
  int n = 0;
  for (uint32_t i = a + 1; i < b; i++)
    if (x[i - 1] < 0.0f && x[i] >= 0.0f) {
      const double t = (i - 1) + (double)(-x[i - 1]) / (double)(x[i] - x[i - 1]);
      if (first < 0) first = t; else n++;
      last = t;
    }
  return n > 0 ? sr * n / (last - first) : 0.0;
}

static float peak(const float* x, uint32_t a, uint32_t b) {
  float p = 0;
  for (uint32_t i = a; i < b; i++) p = fabsf(x[i]) > p ? fabsf(x[i]) : p;
  return p;
}

static float max_step(const float* x, uint32_t a, uint32_t b) {
  float m = 0;
  for (uint32_t i = a + 1; i < b; i++) m = fabsf(x[i] - x[i - 1]) > m ? fabsf(x[i] - x[i - 1]) : m;
  return m;
}

/* Largest second difference: tiny for a sine (A (2 pi f / sr)^2), about the jump size for a click. */
static float max_curve(const float* x, uint32_t a, uint32_t b) {
  float m = 0;
  for (uint32_t i = a + 2; i < b; i++) {
    const float d = fabsf(x[i] - 2.0f * x[i - 1] + x[i - 2]);
    m = d > m ? d : m;
  }
  return m;
}

static int all_finite(const float* x, uint32_t n) {
  for (uint32_t i = 0; i < n; i++) if (!isfinite(x[i])) return 0;
  return 1;
}

static void write_wav(const char* path, const float* x, uint32_t n) {
  FILE* f = fopen(path, "wb");
  if (!f) { printf("can't write %s\n", path); return; }
  const uint32_t bytes = n * 2, sr = SR, br = SR * 2, riff = 36 + bytes;
  const uint16_t fmt = 1, ch = 1, ba = 2, bits = 16;
  const uint32_t fmt_len = 16;
  fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
  fwrite(&fmt_len, 4, 1, f); fwrite(&fmt, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&sr, 4, 1, f);
  fwrite(&br, 4, 1, f); fwrite(&ba, 2, 1, f); fwrite(&bits, 2, 1, f);
  fwrite("data", 1, 4, f); fwrite(&bytes, 4, 1, f);
  for (uint32_t i = 0; i < n; i++) {
    float v = x[i] < -1.0f ? -1.0f : x[i] > 1.0f ? 1.0f : x[i];
    int16_t s = (int16_t)lrintf(v * 32767.0f);
    fwrite(&s, 2, 1, f);
  }
  fclose(f);
}

/* The settings most tests use: the lopass gate as a plain VCA at Level 100 %, FG1 Gate with Rise 1 ms, the
 * fastest vactrol. A held note at velocity 127 then plays at FC_GAIN. */
static void base_params(FcParams* p) {
  fc_default_params(p);
  p->gate_mode = FC_GATE_VCA;
  p->level = 1.0f;
  p->fg_rise_ms[0] = 1.0f;
  p->gate_decay = 0.0f;
}

static void reset(float sr) {
  fc_init(&core, sr);
  FcParams p;
  base_params(&p);
  fc_set_params(&core, &p);
  memset(L, 0, sizeof L);
  memset(R, 0, sizeof R);
}

static void test_pitch(void) {
  reset(SR);
  fc_note_on(&core, 69, 127);
  render(&core, 0, SR / 2);
  const double f = freq(L, SR / 10, SR / 2, SR);
  printf("A4: %.3f Hz\n", f);
  CHECK(fabs(f - 440.0) < 0.2);
  CHECK(peak(L, 0, SR / 2) <= FC_GAIN * 1.01f);       /* the DC blocker overshoots a starting sine by 0.7 % */
  CHECK(fabsf(peak(L, SR / 4, SR / 2) - FC_GAIN) < 0.003f);
  CHECK(!memcmp(L, R, sizeof(float) * SR / 2));                           /* mono for now: L = R */

  FcParams p;
  base_params(&p);
  p.pitch = 12;
  fc_set_params(&core, &p);
  render(&core, 0, SR / 2);
  CHECK(fabs(freq(L, 0, SR / 2, SR) - 880.0) < 0.3);
  p.pitch = 0; p.fine = 100;
  fc_set_params(&core, &p);
  render(&core, 0, SR / 2);
  CHECK(fabs(freq(L, 0, SR / 2, SR) - 466.164) < 0.3);
  p.fine = 0;
  fc_set_params(&core, &p);
  fc_set_bend(&core, -2.0f);
  render(&core, 0, SR / 2);
  CHECK(fabs(freq(L, 0, SR / 2, SR) - 391.995) < 0.3);
  p.level = 0.5f;                                                         /* the Level knob */
  fc_set_params(&core, &p);
  render(&core, 0, SR / 4);
  CHECK(fabsf(peak(L, SR / 8, SR / 4) - FC_GAIN * 0.5f) < 0.003f);

  reset(44100);                                                           /* the rate Zynthian may use */
  fc_note_on(&core, 60, 100);
  render(&core, 0, 22050);
  CHECK(fabs(freq(L, 4410, 22050, 44100) - 261.626) < 0.2);
  /* velocity opens the vactrol to FC_VEL_FLOOR .. 1 */
  CHECK(fabsf(peak(L, 11025, 22050) - FC_GAIN * (FC_VEL_FLOOR + (1 - FC_VEL_FLOOR) * 100.0f / 127.0f)) < 0.003f);
}

static void test_release(void) {
  /* FG1 Gate, Fall 100 ms: after the key the level falls (the vactrol lags a little), then the voice is free */
  reset(SR);
  FcParams p;
  base_params(&p);
  p.fg_fall_ms[0] = 100.0f;
  fc_set_params(&core, &p);
  fc_note_on(&core, 60, 127);
  render(&core, 0, SR / 4);
  CHECK(fc_fg_pos(&core, 0) == 1.5f);                                     /* holding */
  fc_note_off(&core, 60);
  CHECK(fc_active_voices(&core) == 1);
  CHECK(fc_voice_note(&core, 0) == 60);
  render(&core, SR / 4, SR / 20);
  const float pos = fc_fg_pos(&core, 0);
  CHECK(pos > 2.4f && pos < 2.6f);                                        /* half way down */
  const float mid = peak(L, SR / 4 + SR / 20 - 400, SR / 4 + SR / 20) / FC_GAIN;
  printf("FG1 fall: %.2f of the level half way, the vactrol lags\n", mid);
  CHECK(mid > 0.45f && mid < 0.85f);
  render(&core, SR / 4 + SR / 20, SR - SR / 20);
  CHECK(fc_active_voices(&core) == 0);
  CHECK(fc_voice_note(&core, 0) == -1);
  CHECK(peak(L, SR / 4 + SR / 2, SR + SR / 4) == 0.0f);                  /* free within 0.5 s, then silence */
  CHECK(max_step(L, 0, SR + SR / 4) < 0.02f);

  /* Once: the note ends with the key still down */
  reset(SR);
  p.fg_mode[0] = FC_FG_ONCE;
  fc_set_params(&core, &p);
  fc_note_on(&core, 60, 127);
  render(&core, 0, SR / 2);
  CHECK(fc_active_voices(&core) == 0);
}

static void test_voices(void) {
  reset(SR);
  const int chord[4] = { 48, 55, 60, 64 };
  for (int i = 0; i < 4; i++) fc_note_on(&core, chord[i], 127);
  CHECK(fc_active_voices(&core) == 4);
  render(&core, 0, SR);
  CHECK(peak(L, 0, SR) <= 1.0f);
  CHECK(all_finite(L, SR));
  for (int i = 0; i < 4; i++) fc_note_off(&core, chord[i]);
  render(&core, SR, SR);
  write_wav("chord.wav", L, 2 * SR);
  CHECK(fc_active_voices(&core) == 0);

  /* the same note again: its own voice, from where it is (no click) */
  reset(SR);
  fc_note_on(&core, 60, 127);
  render(&core, 0, SR / 4);
  fc_note_off(&core, 60);
  render(&core, SR / 4, SR / 10);
  fc_note_on(&core, 60, 127);
  CHECK(fc_active_voices(&core) == 1);
  render(&core, SR / 4 + SR / 10, SR / 4);
  CHECK(max_curve(L, 0, SR / 2 + SR / 10) < 0.005f);

  /* velocity 0 = note off */
  reset(SR);
  fc_note_on(&core, 60, 127);
  fc_note_on(&core, 60, 0);
  CHECK(core.v[0].held == 0);
}

static void test_steal(void) {
  /* a 5th and 6th note while 4 are held: the oldest voices fade out in 2 ms, no jump in the output */
  reset(SR);
  const int notes[6] = { 48, 52, 55, 59, 62, 65 };
  uint32_t at = 0;
  for (int i = 0; i < 6; i++) {
    fc_note_on(&core, notes[i], 127);
    render(&core, at, SR / 4);
    at += SR / 4;
    CHECK(fc_active_voices(&core) == (i < 4 ? i + 1 : 4));
  }
  int has[128] = { 0 };
  for (int i = 0; i < FC_VOICES; i++) if (fc_voice_note(&core, i) >= 0) has[fc_voice_note(&core, i)] = 1;
  CHECK(!has[48] && !has[52] && has[55] && has[59] && has[62] && has[65]);   /* the oldest two were taken */
  const float curve = max_curve(L, 0, at);
  printf("steal: max second difference %.5f\n", curve);
  /* a cut without the fade would give up to 0.25 (one voice); the 2 ms fade about 0.002 */
  CHECK(curve < 0.005f);
  write_wav("steal.wav", L, at);

  /* a voice let go is taken before a held one */
  reset(SR);
  for (int i = 0; i < 4; i++) fc_note_on(&core, notes[i], 127);
  render(&core, 0, SR / 4);
  fc_note_off(&core, 55);
  render(&core, 0, BLOCK);
  fc_note_on(&core, 70, 127);
  render(&core, 0, SR / 10);
  for (int i = 0; i < FC_VOICES; i++) CHECK(fc_voice_note(&core, i) != 55);
  CHECK(fc_voice_note(&core, 0) == 48);

  /* the stolen note is released before its fade ends: the voice ends free */
  reset(SR);
  for (int i = 0; i < 4; i++) fc_note_on(&core, notes[i], 127);
  render(&core, 0, SR / 4);
  fc_note_on(&core, 70, 127);
  fc_note_off(&core, 70);
  render(&core, 0, SR / 100);
  CHECK(fc_active_voices(&core) == 3);
}

static void test_pedal(void) {
  reset(SR);
  fc_set_sustain(&core, 1);
  fc_note_on(&core, 60, 127);
  render(&core, 0, SR / 4);
  fc_note_off(&core, 60);
  render(&core, 0, SR);
  CHECK(fc_active_voices(&core) == 1);
  CHECK(fc_fg_pos(&core, 0) == 1.5f);                                     /* the pedal holds FG1 */
  fc_set_sustain(&core, 0);
  render(&core, 0, BLOCK);
  CHECK(fc_fg_pos(&core, 0) >= 2.0f);                                     /* falling */

  /* the key is still down when the pedal goes up: the note keeps sounding */
  reset(SR);
  fc_set_sustain(&core, 1);
  fc_note_on(&core, 60, 127);
  fc_set_sustain(&core, 0);
  render(&core, 0, SR / 2);
  CHECK(fc_fg_pos(&core, 0) == 1.5f);
}

static void test_all_off(void) {
  reset(SR);
  for (int i = 0; i < 4; i++) fc_note_on(&core, 60 + i, 127);
  render(&core, 0, SR / 4);
  fc_all_sound_off(&core);
  render(&core, SR / 4, SR / 100);                                         /* 10 ms */
  CHECK(fc_active_voices(&core) == 0);
  CHECK(max_curve(L, 0, SR / 4 + SR / 100) < 0.01f);

  reset(SR);
  for (int i = 0; i < 4; i++) fc_note_on(&core, 60 + i, 127);
  fc_set_sustain(&core, 1);
  render(&core, 0, SR / 4);
  fc_all_notes_off(&core);
  render(&core, 0, BLOCK);
  for (int i = 0; i < 4; i++) CHECK(core.v[i].fg[0].seg == FC_SEG_FALL);
  render(&core, 0, SR);
  CHECK(fc_active_voices(&core) == 0);

  /* nothing playing: silence */
  reset(SR);
  L[0] = 1.0f;
  render(&core, 0, BLOCK);
  CHECK(peak(L, 0, BLOCK) == 0.0f);
}

static void test_extremes(void) {
  /* the highest note, pitch +24 and bend up: capped below Nyquist, no NaN */
  reset(SR);
  FcParams p;
  base_params(&p);
  p.pitch = 24; p.fine = 100;
  for (int i = 0; i < 2; i++) p.fg_rise_ms[i] = p.fg_fall_ms[i] = 0.0f;
  fc_set_params(&core, &p);
  fc_set_bend(&core, 2.0f);
  fc_note_on(&core, 127, 127);
  fc_note_on(&core, 0, 127);
  render(&core, 0, SR / 10);
  CHECK(all_finite(L, SR / 10));
  CHECK(peak(L, 0, SR / 10) <= 2 * FC_GAIN + 1e-4f);
  /* long times */
  p.pitch = 0;
  for (int i = 0; i < 2; i++) p.fg_rise_ms[i] = p.fg_fall_ms[i] = 1e6f;
  p.gate_decay = 1.0f;
  fc_set_params(&core, &p);
  fc_note_on(&core, 60, 1);
  render(&core, 0, SR);
  CHECK(all_finite(L, SR));
}

/* ---------- stage 1: Mod OSC, FM / AM / Sync ---------- */

/* Amplitude of the component at f Hz in x[0..n) (Hann window, one DFT bin at exactly f). */
static double amp_at(const float* x, uint32_t n, double f, double sr) {
  double re = 0, im = 0, wsum = 0;
  for (uint32_t i = 0; i < n; i++) {
    const double w = 0.5 - 0.5 * cos(2.0 * M_PI * i / n);
    const double a = 2.0 * M_PI * f * i / sr;
    re += w * x[i] * cos(a);
    im -= w * x[i] * sin(a);
    wsum += w;
  }
  return 2.0 * sqrt(re * re + im * im) / wsum;
}

static double db(double a) { return 20.0 * log10(a > 1e-12 ? a : 1e-12); }

/* Free mode units for a Mod OSC frequency in Hz (inverse of fc_free_hz). */
static float free_units(double hz) {
  return (float)(FC_FREQ_MAX * log(hz / FC_FREE_MIN) / log(FC_FREE_MAX / FC_FREE_MIN));
}

/* One held note with these Mod settings, 1 s after 0.25 s of settling: the analysis in L[0..SR). */
static void play_mod(int note, int mode, float freq, float index, int shape, int fm, int am, int sync) {
  reset(SR);
  FcParams p;
  base_params(&p);
  p.mod_mode = mode; p.mod_freq = freq; p.mod_index = index; p.mod_shape = shape;
  p.route_fm = fm; p.route_am = am; p.route_sync = sync;
  fc_set_params(&core, &p);
  fc_note_on(&core, note, 127);
  render(&core, 0, SR / 4);
  render(&core, 0, SR);
}

static void test_mod_knob(void) {
  CHECK(fabsf(fc_free_hz(0) - FC_FREE_MIN) < 1e-6f);
  CHECK(fabsf(fc_free_hz(100) - FC_FREE_MAX) < 0.5f);
  CHECK(fabsf(fc_free_hz(free_units(100.0)) - 100.0f) < 0.01f);
  CHECK(fabsf(fc_key_semis(50)) < 1e-5f && fabsf(fc_key_semis(0) + 48) < 1e-4f && fabsf(fc_key_semis(100) - 48) < 1e-4f);
  CHECK(fc_ratios[fc_ratio_index(30)] == 1.0f);                       /* the default: x1 */
  CHECK(fc_ratio_index(0) == 0 && fc_ratio_index(100) == FC_NRATIOS - 1);
  for (int i = 1; i < FC_NRATIOS; i++) CHECK(fc_ratios[i] > fc_ratios[i - 1]);
}

static void test_fm(void) {
  /* A4 440 Hz, Mod OSC sine at 100 Hz (Free): linear FM index beta = 4 * Index * 440 / 100 */
  const float idx = 0.05f;
  const double beta = FC_FM_DEPTH * idx * 440.0 / 100.0;
  play_mod(69, FC_MODE_FREE, free_units(100.0), idx, FC_MOD_SINE, 1, 0, 0);
  const double c0 = amp_at(L, SR, 440, SR), lo = amp_at(L, SR, 340, SR), hi = amp_at(L, SR, 540, SR);
  const double want = fabs(jn(1, beta) / jn(0, beta));
  printf("FM linear beta %.2f: J1/J0 %.2f dB (theory %.2f), sidebands %.2f / %.2f dB\n",
         beta, db(lo / c0), db(want), db(lo / c0), db(hi / c0));
  CHECK(fabs(db(lo / c0) - db(want)) < 0.5 && fabs(db(hi / c0) - db(want)) < 0.5);
  CHECK(amp_at(L, SR, 640, SR) / c0 > 0.5 * fabs(jn(2, beta) / jn(0, beta)));  /* second sidebands too */

  /* FM switched off: a plain sine whatever the Index */
  play_mod(69, FC_MODE_FREE, free_units(100.0), 1.0f, FC_MOD_SINE, 0, 0, 0);
  CHECK(amp_at(L, SR, 540, SR) / amp_at(L, SR, 440, SR) < 1e-3);
}

static void test_am(void) {
  /* Index 100 %: ring modulation, no carrier; 50 %: carrier 0.5, sidebands 0.25 */
  play_mod(69, FC_MODE_FREE, free_units(100.0), 1.0f, FC_MOD_SINE, 0, 1, 0);
  const double c0 = amp_at(L, SR, 440, SR), lo = amp_at(L, SR, 340, SR), hi = amp_at(L, SR, 540, SR);
  printf("ring: carrier %.1f dB below the sidebands\n", db(lo / (c0 + 1e-12)));
  CHECK(c0 < lo * 0.01 && fabs(db(lo / hi)) < 0.2);
  CHECK(fabs(lo - FC_GAIN * 0.5) < 0.005);
  play_mod(69, FC_MODE_FREE, free_units(100.0), 0.5f, FC_MOD_SINE, 0, 1, 0);
  CHECK(fabs(amp_at(L, SR, 440, SR) - FC_GAIN * 0.5) < 0.005);
  CHECK(fabs(amp_at(L, SR, 540, SR) - FC_GAIN * 0.25) < 0.005);
  /* Keytrack +12: the Mod OSC an octave up, ring sidebands at 3 x 440 and 440 */
  play_mod(69, FC_MODE_KEY, 62.5f, 1.0f, FC_MOD_SINE, 0, 1, 0);
  CHECK(amp_at(L, SR, 1320, SR) > FC_GAIN * 0.45 && amp_at(L, SR, 440, SR) > FC_GAIN * 0.45);
  /* Ratio 3/2 */
  int r = 0;
  while (fc_ratios[r] != 1.5f) r++;
  play_mod(69, FC_MODE_RATIO, (float)r * FC_FREQ_MAX / (FC_NRATIOS - 1), 1.0f, FC_MOD_SINE, 0, 1, 0);
  CHECK(amp_at(L, SR, 1100, SR) > FC_GAIN * 0.45 && amp_at(L, SR, 220, SR) > FC_GAIN * 0.45);
}

static void test_sync(void) {
  /* Ratio 1/2 and Sync: the Principal restarts every second cycle, so the sound has 220 Hz in it */
  int r = 0;
  while (fc_ratios[r] != 0.5f) r++;
  const float u = (float)r * FC_FREQ_MAX / (FC_NRATIOS - 1);
  play_mod(69, FC_MODE_RATIO, u, 0.0f, FC_MOD_SINE, 0, 0, 0);
  const double off = amp_at(L, SR, 220, SR);
  /* ratio 1/2 restarts the sine at a zero crossing: nothing changes. Ratio 3/4 (330 Hz) restarts it at
   * phase 1/3 (a step of 0.87 of the voice level) */
  CHECK(off < 1e-4);
  while (fc_ratios[r] != 0.75f) r++;
  const float u34 = (float)r * FC_FREQ_MAX / (FC_NRATIOS - 1);
  const double f34 = 440.0 * 0.75;
  play_mod(69, FC_MODE_RATIO, u34, 0.0f, FC_MOD_SINE, 0, 0, 0);
  const double plain = amp_at(L, SR, f34, SR);
  play_mod(69, FC_MODE_RATIO, u34, 0.0f, FC_MOD_SINE, 0, 0, 1);
  const double synced = amp_at(L, SR, f34, SR);
  printf("sync 3/4: %.1f dB at the Mod frequency (%.1f without sync)\n", db(synced), db(plain));
  CHECK(plain < 1e-4 && synced > FC_GAIN * 0.1);
  CHECK(peak(L, 0, SR) <= FC_GAIN * 1.3f && all_finite(L, SR));   /* 1.18: the synced wave has DC, now removed */
  /* the restarts are band-limited: the step (0.22) is spread over two samples, so the second difference
   * stays below 0.2 (0.15; without the correction 0.24) */
  const float curve = max_curve(L, 0, SR);
  printf("sync 3/4: max second difference %.4f\n", curve);
  CHECK(curve < 0.2f);
}

static void test_mod_smooth(void) {
  /* Index from 0 to 100 % at once, AM ring: the gain glides over FC_SMOOTH_MS, no click */
  reset(SR);
  FcParams p;
  base_params(&p);
  p.mod_mode = FC_MODE_FREE; p.mod_freq = free_units(3.0); p.route_fm = 0; p.route_am = 1;
  fc_set_params(&core, &p);
  fc_note_on(&core, 57, 127);
  render(&core, 0, SR / 4);
  p.mod_index = 1.0f;
  fc_set_params(&core, &p);
  render(&core, SR / 4, SR / 4);
  p.route_am = 0;                                       /* switched off while sounding */
  fc_set_params(&core, &p);
  render(&core, SR / 2, SR / 4);
  const float steady = max_curve(L, SR / 10, SR / 4);
  const float jumps = max_curve(L, SR / 4 - 10, 3 * SR / 4);
  printf("Index jump and AM off: max second difference %.5f (steady %.5f)\n", jumps, steady);
  CHECK(jumps < steady * 1.5f + 1e-4f);

  /* Free mode keeps the Mod phase across notes; Ratio restarts it */
  reset(SR);
  p.mod_mode = FC_MODE_FREE;
  fc_set_params(&core, &p);
  CHECK(fc_mod_phase(&core) == -1.0f);
  fc_note_on(&core, 60, 127);
  render(&core, 0, SR / 7);
  const float ph = fc_mod_phase(&core);
  CHECK(ph > 0.0f && ph < 1.0f);
  fc_all_sound_off(&core);
  render(&core, 0, SR / 100);
  fc_note_on(&core, 60, 127);
  CHECK(fabsf(core.v[0].mphase - ph) < 0.01f);
  p.mod_mode = FC_MODE_RATIO;
  fc_set_params(&core, &p);
  fc_all_sound_off(&core);
  render(&core, 0, SR / 100);
  fc_note_on(&core, 60, 127);
  CHECK(core.v[0].mphase == 0.0f);
}

static void test_mod_extremes(void) {
  /* everything on, every shape, highest notes and Mod frequency: bounded, no NaN */
  for (int shape = 0; shape < FC_NMOD_SHAPES; shape++)
    {
      reset(SR);
      FcParams p;
      fc_default_params(&p);
      p.pitch = 24; p.mod_mode = FC_MODE_FREE; p.mod_freq = 100; p.mod_index = 1; p.mod_shape = shape;
      p.route_fm = p.route_am = p.route_sync = 1;
      fc_set_params(&core, &p);
      for (int i = 0; i < 4; i++) fc_note_on(&core, 100 + 9 * i, 127);
      render(&core, 0, SR / 2);
      CHECK(all_finite(L, SR / 2));
      CHECK(peak(L, 0, SR / 2) <= 4 * FC_GAIN * 1.2f);
    }
}

/* ---------- stage 2: the Timbre section ---------- */

/* One held note through the Timbre section (Mod Index 0), 1 s after 0.25 s of settling: L[0..SR). */
static void play_timbre(int note, float timbre, float order, float sym, float drive) {
  reset(SR);
  FcParams p;
  base_params(&p);
  p.timbre = timbre; p.order = order; p.symmetry = sym; p.drive = drive;
  fc_set_params(&core, &p);
  fc_note_on(&core, note, 127);
  render(&core, 0, SR / 4);
  render(&core, 0, SR);
}

static double harm(double f0, int n) { return amp_at(L, SR, f0 * n, SR); }

static void test_timbre_bypass(void) {
  /* Timbre 0, Symmetry 0, Drive 0: the Principal passes (ADAA on the folder is the mean over a sample: -0.3 dB
   * at 8 kHz at the 2x rate, nothing at 440 Hz) */
  play_timbre(69, 0, 0, 0, 0);
  const double lvl = harm(440, 1);
  CHECK(fabs(freq(L, 0, SR, SR) - 440.0) < 0.2);
  CHECK(harm(440, 2) < lvl * 1e-4 && harm(440, 3) < lvl * 1e-4);
  CHECK(fabs(db(lvl / FC_GAIN)) < 0.1);
  /* 2x: the halfband delays the sound by a few samples only */
  reset(SR);
  fc_note_on(&core, 69, 127);
  render(&core, 0, BLOCK);
  int first = -1;
  for (int i = 0; i < BLOCK && first < 0; i++) if (fabsf(L[i]) > 1e-6f) first = i;
  CHECK(first >= 0 && first < 40);
}

static void test_folder(void) {
  const double f0 = 110.0;
  play_timbre(45, 0.5f, 0, 0, 0);
  const double h1 = harm(f0, 1), h2 = harm(f0, 2), h3 = harm(f0, 3), h5 = harm(f0, 5);
  printf("folder Timbre 50%%: h3 %.1f dB, h5 %.1f dB, h2 %.1f dB\n", db(h3 / h1), db(h5 / h1), db(h2 / h1));
  CHECK(h3 > h1 * 0.1 && h5 > h1 * 0.05);                 /* folds: strong odd harmonics */
  CHECK(h2 < h1 * 1e-3);                                  /* symmetric: no even ones */
  CHECK(peak(L, 0, SR) <= FC_GAIN * 1.01f);
  play_timbre(45, 0.5f, 0, 0.3f, 0);
  printf("folder Symmetry +30: h2 %.1f dB\n", db(harm(f0, 2) / harm(f0, 1)));
  CHECK(harm(f0, 2) > harm(f0, 1) * 0.03);                /* asymmetric: even harmonics */
  /* more Timbre, brighter: the energy above h5 grows */
  double hi[2];
  for (int k = 0; k < 2; k++) {
    play_timbre(45, k ? 1.0f : 0.25f, 0, 0, 0);
    hi[k] = 0;
    for (int n = 7; n < 40; n += 2) hi[k] += harm(f0, n);
  }
  CHECK(hi[1] > hi[0] * 2.0);
}

static void test_cheb_shapers(void) {
  /* Saw, Timbre 100 %: harmonics 1/n, up to FC_CHEB_MAX on a low note */
  const double f0 = 110.0;
  play_timbre(45, 1.0f, 0.5f, 0, 0);
  const double h1 = harm(f0, 1);
  double worst = 0;
  for (int n = 2; n <= 12; n++) {
    const double e = fabs(db(harm(f0, n) / h1) - db(1.0 / n));
    worst = e > worst ? e : worst;
  }
  printf("saw: harmonics 2..12 within %.3f dB of 1/n; h%d %.1f dB, h%d %.1f dB\n", worst, FC_CHEB_MAX,
         db(harm(f0, FC_CHEB_MAX) / h1), FC_CHEB_MAX + 1, db(harm(f0, FC_CHEB_MAX + 1) / h1));
  CHECK(worst < 0.1);
  CHECK(fabs(db(harm(f0, FC_CHEB_MAX) / h1) - db(1.0 / FC_CHEB_MAX)) < 0.5);
  CHECK(harm(f0, FC_CHEB_MAX + 1) < h1 * 1e-4);
  CHECK(peak(L, 0, SR) <= FC_GAIN * 1.01f);
  /* M: odd harmonics 1/n only */
  play_timbre(45, 1.0f, 1.0f, 0, 0);
  const double m1 = harm(f0, 1);
  CHECK(harm(f0, 2) < m1 * 1e-4 && harm(f0, 4) < m1 * 1e-4);
  CHECK(fabs(db(harm(f0, 3) / m1) - db(1.0 / 3)) < 0.1 && fabs(db(harm(f0, 5) / m1) - db(1.0 / 5)) < 0.1);
  /* a high note: N follows it, nothing above 0.45 of the rate (C7: N = 10, h11 would be 23 kHz) */
  const double c7 = 440.0 * pow(2.0, 27.0 / 12.0);
  play_timbre(96, 1.0f, 0.5f, 0, 0);
  printf("saw C7: h10 %.1f dB (1/10 = %.1f), h11 %.1f dB\n", db(harm(c7, 10) / harm(c7, 1)), db(0.1),
         db(harm(c7, 11) / harm(c7, 1)));
  CHECK(fabs(db(harm(c7, 10) / harm(c7, 1)) - db(0.1)) < 0.5);
  CHECK(harm(c7, 11) < harm(c7, 1) * 1e-4);
  /* Timbre 50 %: half way between the sine and the saw */
  play_timbre(45, 0.5f, 0.5f, 0, 0);
  const double s1 = harm(f0, 1), s2 = harm(f0, 2);
  CHECK(s2 > s1 * 0.05 && s2 < s1 * 0.4);
}

static void test_drive(void) {
  play_timbre(57, 0, 0, 0, 1.0f);
  const double h1 = harm(220, 1), h3 = harm(220, 3);
  printf("Drive 100 %%: h3 %.1f dB\n", db(h3 / h1));
  CHECK(h3 > h1 * 0.05 && harm(220, 2) < h1 * 1e-3);
  CHECK(peak(L, 0, SR) <= FC_GAIN * 1.05f);   /* the DC blocker tilts a near-square wave by a few % */
}

/* Timbre knobs at k (0..1) of the way from all 0 to Timbre 100 %, Order 50 %, Symmetry +50, Drive 100 %. */
static void timbre_knobs(FcParams* p, float k) {
  base_params(p);
  p->timbre = k; p->order = 0.5f * k; p->symmetry = 0.5f * k; p->drive = k;
}

static void test_timbre_smooth(void) {
  /* all four knobs at once. The sound passes through the folder at high Timbre on the way (Order 0 -> 50 %), so
   * the jump is compared with a slow turn of the same knobs (the same states, 200 ms): a click would be sharper */
  FcParams p;
  float sharp[2];
  for (int slow = 0; slow < 2; slow++) {
    reset(SR);
    timbre_knobs(&p, 0);
    fc_set_params(&core, &p);
    fc_note_on(&core, 57, 127);
    render(&core, 0, SR / 4);
    const int steps = slow ? 150 : 1;
    for (int i = 1; i <= steps; i++) {
      timbre_knobs(&p, (float)i / steps);
      fc_set_params(&core, &p);
      render(&core, SR / 4 + (i - 1) * BLOCK, BLOCK);
    }
    render(&core, SR / 4 + steps * BLOCK, SR / 4);
    sharp[slow] = max_curve(L, SR / 4 - 2, SR / 4 + steps * BLOCK + SR / 4);
  }
  printf("Timbre knobs: max second difference %.4f at once, %.4f turned slowly\n", sharp[0], sharp[1]);
  CHECK(sharp[0] <= 1.2f * sharp[1]);
}

static void test_timbre_route(void) {
  /* Mod OSC -> Timbre: a 2 Hz sine moves Timbre 50 % to 100 % and to 0 %: the 3rd harmonic follows */
  reset(SR);
  FcParams p;
  base_params(&p);
  p.mod_mode = FC_MODE_FREE; p.mod_freq = free_units(2.0); p.mod_index = 1.0f;
  p.route_fm = 0; p.route_timbre = 1; p.timbre = 0.5f;
  fc_set_params(&core, &p);
  fc_note_on(&core, 57, 127);
  render(&core, 0, SR / 2);                                 /* the LFO starts at phase 0 with the note */
  const double up = amp_at(L + SR / 8 - 1200, 2400, 660, SR) / amp_at(L + SR / 8 - 1200, 2400, 220, SR);
  const double down = amp_at(L + 3 * SR / 8 - 1200, 2400, 660, SR) / amp_at(L + 3 * SR / 8 - 1200, 2400, 220, SR);
  printf("Mod -> Timbre: h3 %.1f dB at the LFO top, %.1f dB at its bottom\n", db(up), db(down));
  CHECK(up > 0.1 && down < 0.01);
}

static void test_timbre_extremes(void) {
  {
    reset(SR);
    FcParams p;
    fc_default_params(&p);
    p.pitch = 24; p.mod_mode = FC_MODE_FREE; p.mod_freq = 100; p.mod_index = 1; p.mod_shape = FC_MOD_SAW;
    p.route_fm = p.route_am = p.route_sync = p.route_timbre = 1;
    p.timbre = 1; p.order = 0.3f; p.symmetry = -1; p.drive = 1;
    fc_set_params(&core, &p);
    for (int i = 0; i < 4; i++) fc_note_on(&core, 90 + 12 * i, 127);
    render(&core, 0, SR / 2);
    CHECK(all_finite(L, SR / 2));
    CHECK(peak(L, 0, SR / 2) <= 4 * FC_GAIN * 1.3f);
  }
}

/* ---------- stage 3: function generators, vactrol, lopass gate ---------- */

/* Peak of each 1 ms (48 samples) of L[from..from + n) into env[] (n / 48 values). */
static void envelope(int from, int n, float* env) {
  for (int b = 0; b < n / 48; b++) env[b] = peak(L, from + b * 48, from + b * 48 + 48);
}

/* First ms at or after `start` where env falls below `level`; -1 = never. */
static int first_below(const float* env, int n, int start, float level) {
  for (int b = start; b < n; b++) if (env[b] < level) return b;
  return -1;
}

static void test_fg(void) {
  /* Gate, Rise 100 ms: half way at 50 ms, holding after; Fall 200 ms: half way 100 ms after the key */
  reset(SR);
  FcParams p;
  base_params(&p);
  p.fg_rise_ms[0] = 100; p.fg_fall_ms[0] = 200;
  p.fg_rise_ms[1] = 50; p.fg_fall_ms[1] = 50; p.fg_mode[1] = FC_FG_CYCLE;
  fc_set_params(&core, &p);
  fc_note_on(&core, 60, 127);
  render(&core, 0, SR / 20);
  CHECK(fabsf(fc_fg_pos(&core, 0) - 0.5f) < 0.02f);
  render(&core, 0, SR / 10);
  CHECK(fc_fg_pos(&core, 0) == 1.5f);
  /* FG2 Cycle 50 + 50 ms: 10 cycles a second while the key is down */
  int cycles = 0;
  float last = fc_fg_pos(&core, 1);
  for (int b = 0; b < SR / BLOCK; b++) {
    render(&core, 0, BLOCK);
    const float pos = fc_fg_pos(&core, 1);
    if (last >= 2.0f && pos < 1.0f) cycles++;                             /* fall -> rise */
    last = pos;
  }
  printf("FG2 Cycle 50 + 50 ms: %d cycles in 1 s\n", cycles);
  CHECK(cycles >= 9 && cycles <= 11);
  fc_note_off(&core, 60);
  render(&core, 0, SR / 10);
  CHECK(fabsf(fc_fg_pos(&core, 0) - 2.5f) < 0.02f);
  render(&core, 0, SR / 5);
  CHECK(fc_fg_pos(&core, 0) == 3.0f || fc_active_voices(&core) == 0);   /* ended */
  CHECK(fc_fg_pos(&core, 1) == 3.0f || fc_active_voices(&core) == 0);   /* Cycle ends its fall after the key */

  /* Gate: the key let go while rising falls from there */
  reset(SR);
  fc_set_params(&core, &p);
  fc_note_on(&core, 60, 127);
  render(&core, 0, SR / 40);                                              /* 25 ms: a quarter up */
  fc_note_off(&core, 60);
  render(&core, 0, BLOCK);
  CHECK(fc_fg_pos(&core, 0) > 2.7f && fc_fg_pos(&core, 0) < 2.8f);       /* falling from about 0.25 */

  /* a retrigger rises from where it is */
  reset(SR);
  fc_set_params(&core, &p);
  fc_note_on(&core, 60, 127);
  render(&core, 0, SR / 4);
  fc_note_off(&core, 60);
  render(&core, 0, SR / 10);                                              /* half way down: 0.5 */
  fc_note_on(&core, 60, 127);
  render(&core, 0, BLOCK);
  CHECK(fc_fg_pos(&core, 0) > 0.5f && fc_fg_pos(&core, 0) < 0.6f);
}

static void test_vactrol(void) {
  /* a strike (FG1 Once, 1 + 1 ms) through the VCA: fast up, slow down, slower as it gets dark;
   * Decay stretches the tail */
  int t20[2], t40[2], top[2];
  float peak_lvl[2];
  for (int k = 0; k < 2; k++) {
    reset(SR);
    FcParams p;
    base_params(&p);
    p.fg_mode[0] = FC_FG_ONCE; p.fg_fall_ms[0] = 1;
    p.gate_decay = k ? 0.6f : 0.2f;
    fc_set_params(&core, &p);
    fc_note_on(&core, 57, 127);
    render(&core, 0, 4 * SR);
    static float env[4000];
    envelope(0, 4 * SR, env);
    int pk = 0;
    for (int b = 0; b < 4000; b++) if (env[b] > env[pk]) pk = b;
    top[k] = pk;
    t20[k] = first_below(env, 4000, pk, env[pk] * 0.1f) - pk;
    t40[k] = first_below(env, 4000, pk, env[pk] * 0.01f) - pk;
    peak_lvl[k] = env[pk] / FC_GAIN;
    printf("strike, Decay %d%% (%.0f ms): peak at %d ms (%.2f), -20 dB after %d ms, -40 dB after %d ms\n",
           k ? 60 : 20, fc_decay_ms(p.gate_decay), pk, env[pk] / FC_GAIN, t20[k], t40[k]);
  }
  CHECK(top[0] <= 6 && top[1] <= 6);                                      /* opens in a few ms */
  CHECK(peak_lvl[0] > 0.3f);                                              /* a 2 ms strike still sounds */
  CHECK(t40[0] > 0 && t40[1] > 0);
  CHECK(t20[1] > 3 * t20[0]);                                             /* Decay stretches the tail */
  /* slower when darker: from fully open (a held note let go, FG1 Fall 1 ms), the second -20 dB takes longer than
   * the first (an exponential would take the same time) */
  reset(SR);
  FcParams p;
  base_params(&p);
  p.fg_fall_ms[0] = 1;
  p.gate_decay = 0.2f;
  fc_set_params(&core, &p);
  fc_note_on(&core, 57, 127);
  render(&core, 0, SR / 4);
  fc_note_off(&core, 57);
  render(&core, 0, SR);
  static float env[1000];
  envelope(0, SR, env);
  const int a = first_below(env, 1000, 0, env[0] * 0.1f), b = first_below(env, 1000, 0, env[0] * 0.01f);
  printf("let go from open: -20 dB after %d ms, -40 dB after %d ms\n", a, b);
  CHECK(a > 0 && b - a > 1.25 * a);                                     /* 1.34 with FC_VAC_SLOW 2: tune by ear */
}

static void test_gate_modes(void) {
  /* the folder (rich) through a strike: LP and Combo darken as it closes, VCA keeps the spectrum */
  double drop[3];
  for (int mode = 0; mode < 3; mode++) {
    reset(SR);
    FcParams p;
    base_params(&p);
    p.gate_mode = mode;
    p.fg_mode[0] = FC_FG_ONCE; p.fg_fall_ms[0] = 300;
    p.gate_decay = 0.5f;
    p.timbre = 0.6f;
    fc_set_params(&core, &p);
    fc_note_on(&core, 45, 127);
    render(&core, 0, SR);
    const int w = 4800;
    const double early = amp_at(L + 2400, w, 110 * 7, SR) / amp_at(L + 2400, w, 110, SR);
    const double late = amp_at(L + 19200, w, 110 * 7, SR) / amp_at(L + 19200, w, 110, SR);
    drop[mode] = db(late) - db(early);
  }
  printf("h7 / h1 from 50 ms to 400 ms: VCA %+.1f dB, LP %+.1f dB, Combo %+.1f dB\n", drop[0], drop[1], drop[2]);
  CHECK(fabs(drop[0]) < 1.0);
  CHECK(drop[1] < -10.0 && drop[2] < -10.0);
}

static void test_resonance(void) {
  /* LP mode held at a cutoff near 1 kHz (the vactrol at the velocity's level); the Saw shaper at 55 Hz makes
   * harmonics every 55 Hz: Resonance lifts those near the cutoff, not the low ones */
  const int vel = 35;                                       /* g = 0.4 + 0.6 * 35 / 127 = 0.565: about 1 kHz */
  const double g = FC_VEL_FLOOR + (1 - FC_VEL_FLOOR) * vel / 127.0;
  const double fcut = FC_LPG_FMIN * pow(FC_LPG_FMAX / FC_LPG_FMIN, g);
  const int hn = (int)lround(fcut / 55.0);
  double h[2][2];
  for (int k = 0; k < 2; k++) {
    reset(SR);
    FcParams p;
    base_params(&p);
    p.gate_mode = FC_GATE_LP;
    p.gate_res = k ? 1.0f : 0.0f;
    p.timbre = 1.0f; p.order = 0.5f;
    fc_set_params(&core, &p);
    fc_note_on(&core, 33, vel);
    render(&core, 0, SR / 2);
    render(&core, 0, SR);
    h[k][0] = harm(55, 2);
    h[k][1] = harm(55, hn);
  }
  printf("resonance at %.0f Hz (h%d): %+.1f dB, h2 %+.1f dB\n", fcut, hn, db(h[1][1] / h[0][1]),
         db(h[1][0] / h[0][0]));
  CHECK(db(h[1][1] / h[0][1]) > 15.0);
  CHECK(fabs(db(h[1][0] / h[0][0])) < 1.5);
  CHECK(all_finite(L, SR));
}

static void test_resonance_peak(void) {
  /* Resonance 100 %: as the vactrol closes, the cutoff passes the fundamental; the band state saturates, so one
   * voice stays far from the output clip (it reached 3.3 before FC_RES_SAT, 2026-11-12) */
  float mx = 0;
  const int notes[] = { 36, 48, 60, 72, 84 };
  for (int m = FC_GATE_LP; m <= FC_GATE_COMBO; m++)
    for (int n = 0; n < 5; n++) {
      reset(SR);
      FcParams p;
      base_params(&p);
      p.gate_mode = m; p.gate_res = 1.0f; p.gate_decay = 0.5f;
      p.fg_mode[0] = FC_FG_ONCE; p.fg_fall_ms[0] = 300;
      fc_set_params(&core, &p);
      fc_note_on(&core, notes[n], 127);
      render(&core, 0, 2 * SR);
      const float pk = peak(L, 0, 2 * SR);
      if (pk > mx) mx = pk;
    }
  printf("resonance 100 %%, one voice: peak %.2f\n", mx);
  CHECK(mx < 0.6f);
  CHECK(all_finite(L, 2 * SR));
}

static void test_gate_switch(void) {
  /* VCA -> LP -> Combo while a note holds at velocity 64 (g = 0.7: VCA 0.7, LP 2.5 kHz): no click */
  reset(SR);
  FcParams p;
  base_params(&p);
  fc_set_params(&core, &p);
  fc_note_on(&core, 57, 64);
  render(&core, 0, SR / 4);
  p.gate_mode = FC_GATE_LP;
  fc_set_params(&core, &p);
  render(&core, SR / 4, SR / 4);
  p.gate_mode = FC_GATE_COMBO;
  fc_set_params(&core, &p);
  render(&core, SR / 2, SR / 4);
  const float steady = fmaxf(max_step(L, SR / 8, SR / 4), max_step(L, SR / 2 - SR / 8, SR / 2));
  const float sw = fmaxf(max_step(L, SR / 4, SR / 4 + SR / 50), max_step(L, SR / 2, SR / 2 + SR / 50));
  printf("gate mode switch: max step %.4f (steady %.4f)\n", sw, steady);
  CHECK(sw <= 1.2f * steady);
}

static void test_fg2_timbre(void) {
  /* FG2 Once (100 + 100 ms) -> Timbre 100 %: the folder at its top, a sine after it */
  reset(SR);
  FcParams p;
  base_params(&p);
  p.fg_mode[1] = FC_FG_ONCE; p.fg_rise_ms[1] = 100; p.fg_fall_ms[1] = 100;
  p.fg2_timbre = 1.0f;
  fc_set_params(&core, &p);
  fc_note_on(&core, 57, 127);
  render(&core, 0, SR / 2);
  const double top = amp_at(L + SR / 10 - 1200, 2400, 660, SR) / amp_at(L + SR / 10 - 1200, 2400, 220, SR);
  const double after = amp_at(L + SR / 3, 4800, 660, SR) / amp_at(L + SR / 3, 4800, 220, SR);
  printf("FG2 -> Timbre: h3 %.1f dB at its top, %.1f dB after\n", db(top), db(after));
  CHECK(top > 0.1 && after < 1e-3);
}

/* ---------- stage 4: the engines ---------- */

/* Share of the power of L[0..SR) that lies on the harmonics of f0 (up to SR / 2: the halfband passes the voice
 * almost to there): 1 for a periodic sound at f0, less if the pitch is off or there is aliasing / inharmonic
 * lines. */
static double harmonic_share(double f0) {
  double total = 0, lines = 0;
  for (uint32_t i = 0; i < SR; i++) total += (double)L[i] * L[i];
  total /= SR;
  for (int k = 1; k * f0 < SR / 2; k++) {
    const double a = harm(f0, k);
    lines += a * a / 2.0;
  }
  return total > 0 ? lines / total : 0;
}

/* Aliasing of L[0..ANFFT): power off the lines at multiples of f0 relative to the power on them, in dB, to
 * 20 kHz (Blackman-Harris window, sidelobes -92 dB), as test/alias_report.c. */
#define ANFFT 32768
static double fre[ANFFT], fim[ANFFT];
static double alias_db(double f0) {
  for (int i = 0; i < ANFFT; i++) {
    const double a = 2.0 * M_PI * i / (ANFFT - 1);
    fre[i] = L[i] * (0.35875 - 0.48829 * cos(a) + 0.14128 * cos(2 * a) - 0.01168 * cos(3 * a));
    fim[i] = 0;
  }
  for (int i = 1, j = 0; i < ANFFT; i++) {
    int bit = ANFFT >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) { double t = fre[i]; fre[i] = fre[j]; fre[j] = t; t = fim[i]; fim[i] = fim[j]; fim[j] = t; }
  }
  for (int len = 2; len <= ANFFT; len <<= 1)
    for (int i = 0; i < ANFFT; i += len)
      for (int k = 0; k < len / 2; k++) {
        const double wr = cos(-2.0 * M_PI * k / len), wi = sin(-2.0 * M_PI * k / len);
        const int a = i + k, b = i + k + len / 2;
        const double tr = fre[b] * wr - fim[b] * wi, ti = fre[b] * wi + fim[b] * wr;
        fre[b] = fre[a] - tr; fim[b] = fim[a] - ti; fre[a] += tr; fim[a] += ti;
      }
  const double bin = (double)SR / ANFFT;
  double good = 0, bad = 0;
  for (int k = 1; k < ANFFT / 2 && k * bin <= 20000.0; k++) {
    const double f = k * bin, pw = fre[k] * fre[k] + fim[k] * fim[k];
    if (fabs(f - f0 * floor(f / f0 + 0.5)) <= 6 * bin) good += pw; else bad += pw;
  }
  return 10.0 * log10((bad + 1e-30) / (good + 1e-30));
}

/* One held note of a source, 1 s after 0.25 s of settling: L[0..SR). */
static void play_source(int src, float h, float col, float m, int note) {
  reset(SR);
  FcParams p;
  base_params(&p);
  p.source = src; p.harmonics = h; p.color = col; p.morph = m;
  fc_set_params(&core, &p);
  fc_note_on(&core, note, 127);
  render(&core, 0, SR / 4);
  render(&core, 0, SR);
}

#define FM2_RATIO1 (30.0f / 128.0f)                 /* Harmonics of FM2 at the ratio 1 of its quantizer */
#define VA_FIFTH 0.622f                             /* Harmonics of VA at +7.01 semitones */
#define VA_SAW (1.0f / 3.0f)                        /* Color / Morph of VA: 0 triangle, 1/3 saw, 2/3 square */

static void test_engine_pitch(void) {
  /* each engine at a harmonic setting: in tune (the power on the harmonics of the note) and at a sane level */
  const struct { int src; float h, c, m; } k[] = {
    { FC_SRC_VA, 0.5f, VA_SAW, VA_SAW }, { FC_SRC_VA, 0.5f, 0.0f, 0.95f },
    { FC_SRC_FM2, FM2_RATIO1, 0.6f, 0.5f }, { FC_SRC_WAVE, 0.5f, 0.5f, 0.5f }, { FC_SRC_WAVE, 0.1f, 0.9f, 0.3f } };
  const int notes[] = { 45, 69, 93 };
  for (unsigned i = 0; i < sizeof k / sizeof k[0]; i++)
    for (int n = 0; n < 3; n++) {
      /* a bright wave has most of its 64 harmonics above the voices' Nyquist at A6: it loses level and aliases
       * (as in the original engine, which runs at 48 kHz); checked up to A4 */
      if (k[i].src == FC_SRC_WAVE && k[i].h < 0.2f && notes[n] > 69) continue;
      play_source(k[i].src, k[i].h, k[i].c, k[i].m, notes[n]);
      const double f0 = 440.0 * pow(2.0, (notes[n] - 69) / 12.0);
      const double share = harmonic_share(f0);
      const float pk = peak(L, 0, SR);
      printf("source %d H%.2f C%.2f M%.2f, note %d: %.5f of the power on the harmonics, peak %.3f\n",
             k[i].src, k[i].h, k[i].c, k[i].m, notes[n], share, pk);
      CHECK(share > 0.995);
      CHECK(pk > 0.25f * FC_GAIN && pk < 2.2f * FC_GAIN);
      CHECK(all_finite(L, SR));
    }
}

static void test_engine_knobs(void) {
  const double f0 = 220.0;
  /* VA: Harmonics at the fifth adds a line at 1.4983 f0; Color / Morph 0 = triangles (no even harmonics),
   * 1/3 = saws (h2 = h1 / 2) */
  play_source(FC_SRC_VA, VA_FIFTH, VA_SAW, VA_SAW, 57);
  const double fifth = amp_at(L, SR, f0 * pow(2.0, 7.01 / 12.0), SR) / harm(f0, 1);
  play_source(FC_SRC_VA, 0.0f + 0.5f, 0.0f, 0.0f, 57);
  const double tri_h2 = harm(f0, 2) / harm(f0, 1), tri_h3 = harm(f0, 3) / harm(f0, 1);
  play_source(FC_SRC_VA, 0.5f, VA_SAW, VA_SAW, 57);
  const double saw_h2 = harm(f0, 2) / harm(f0, 1);
  printf("VA: fifth line %.2f of h1; triangle h2 %.1f dB h3 %.1f dB; saw h2 %.1f dB\n", fifth, db(tri_h2),
         db(tri_h3), db(saw_h2));
  CHECK(fifth > 0.5);
  CHECK(db(tri_h2) < -40 && fabs(db(tri_h3) - db(1.0 / 9)) < 1.5);
  CHECK(fabs(db(saw_h2) - db(0.5)) < 1.0);
  /* FM2: Color 0 = a sine; Color 70 % = sidebands */
  play_source(FC_SRC_FM2, FM2_RATIO1, 0.0f, 0.5f, 57);
  const double fm0 = fmax(harm(f0, 2), harm(f0, 3)) / harm(f0, 1);
  play_source(FC_SRC_FM2, FM2_RATIO1, 0.7f, 0.5f, 57);
  const double fm7 = fmax(harm(f0, 2), harm(f0, 3)) / harm(f0, 1);
  printf("FM2 ratio 1: h2/h3 at Color 0 %.1f dB, at 70 %% %.1f dB\n", db(fm0), db(fm7));
  CHECK(db(fm0) < -60 && db(fm7) > -20);
  /* Wave: Color, Morph and Harmonics each move to other waves (the first 8 harmonics change) */
  double ref[8];
  play_source(FC_SRC_WAVE, 0.3f, 0.3f, 0.3f, 57);
  for (int i = 0; i < 8; i++) ref[i] = db(harm(f0, i + 1));
  const float knob[3][3] = { { 0.3f, 0.8f, 0.3f }, { 0.3f, 0.3f, 0.8f }, { 0.6f, 0.3f, 0.3f } };
  for (int k = 0; k < 3; k++) {
    play_source(FC_SRC_WAVE, knob[k][0], knob[k][1], knob[k][2], 57);
    double d = 0;
    for (int i = 0; i < 8; i++) d = fmax(d, fabs(db(harm(f0, i + 1)) - ref[i]));
    printf("Wave: %s moves a harmonic by %.1f dB\n", k == 0 ? "Color" : k == 1 ? "Morph" : "Harmonics", d);
    CHECK(d > 6.0);
  }
}

static void test_source_switch(void) {
  /* a held note through Sine -> VA -> FM2 -> Wave -> Sine: a crossfade, no click, the new source plays */
  reset(SR);
  FcParams p;
  base_params(&p);
  p.color = 0.3f;                                   /* VA: soft shapes; FM2: some index */
  fc_set_params(&core, &p);
  fc_note_on(&core, 57, 127);
  const int seq[] = { FC_SRC_VA, FC_SRC_FM2, FC_SRC_WAVE, FC_SRC_SINE };
  const uint32_t q = SR / 4;
  render(&core, 0, q);
  for (int i = 0; i < 4; i++) {
    p.source = seq[i];
    fc_set_params(&core, &p);
    render(&core, (i + 1) * q, q);
    CHECK(core.v[0].src == seq[i] && core.v[0].xf == 1.0f);
  }
  for (int i = 0; i < 4; i++) {
    const uint32_t at = (i + 1) * q;
    const float steady = fmaxf(max_step(L, at - SR / 20, at), max_step(L, at + SR / 20, at + SR / 10));
    const float sw = max_step(L, at, at + SR / 50);
    printf("source switch to %d: max step %.4f (steady %.4f)\n", seq[i], sw, steady);
    CHECK(sw <= 1.2f * steady + 1e-4f);
  }
  CHECK(all_finite(L, 5 * q));
}

static void test_rate_switch(void) {
  /* a note keeps the rate of its note-on: Sine 2x, an engine 4x; a change of Source on a held note does not move it
   * (it cannot move without a click); the next note, a stolen voice too, takes the rate of the new Source */
  reset(SR);
  FcParams p;
  base_params(&p);
  p.engines_4x = 1;
  p.color = 0.0f; p.harmonics = FM2_RATIO1;         /* FM2 at Color 0: a sine too */
  fc_set_params(&core, &p);
  fc_note_on(&core, 93, 127);
  const uint32_t q = SR / 4;
  render(&core, 0, q);
  CHECK(core.v[0].hi == 0);
  p.source = FC_SRC_FM2;
  fc_set_params(&core, &p);
  render(&core, q, q);
  CHECK(core.v[0].hi == 0 && core.v[0].src == FC_SRC_FM2 && core.v[0].xf == 1.0f);
  const float steady = max_curve(L, q - SR / 20, q);
  const float sw = max_curve(L, q, q + SR / 20);
  printf("Source change on a held A6: max second difference %.4f (steady %.4f)\n", sw, steady);
  CHECK(sw <= 1.1f * steady);
  fc_note_on(&core, 81, 127);                       /* a new note: 4x */
  render(&core, 2 * q, 64);
  CHECK(core.v[1].hi == 1 && core.v[0].hi == 0);
  /* all voices busy with Sine notes, Source VA: the stolen one comes back at 4x */
  reset(SR);
  base_params(&p);
  p.engines_4x = 1;
  fc_set_params(&core, &p);
  for (int i = 0; i < FC_VOICES; i++) fc_note_on(&core, 60 + 4 * i, 100);
  render(&core, 0, q);
  p.source = FC_SRC_VA; p.color = 0.0f; p.morph = 0.0f;   /* triangles: no steps of their own */
  fc_set_params(&core, &p);
  render(&core, q, q);
  fc_note_on(&core, 84, 100);
  render(&core, 2 * q, q);
  int hi = 0, n84 = 0;
  for (int i = 0; i < FC_VOICES; i++) {
    hi += core.v[i].hi;
    n84 += core.v[i].note == 84;
  }
  CHECK(hi == 1 && n84 == 1);
  p.engines_4x = 0;                                 /* switched off: the next note is at 2x */
  fc_set_params(&core, &p);
  fc_note_off(&core, 84);
  fc_note_on(&core, 86, 100);
  render(&core, 3 * q, 64);
  for (int i = 0; i < FC_VOICES; i++) if (core.v[i].note == 86) CHECK(core.v[i].hi == 0);
  const float st2 = fmaxf(max_step(L, 2 * q - SR / 20, 2 * q), max_step(L, 2 * q + SR / 10, 2 * q + SR / 5));
  const float sw2 = max_step(L, 2 * q, 2 * q + SR / 50);
  printf("steal into a 4x voice: max step %.4f (steady %.4f)\n", sw2, st2);
  CHECK(sw2 <= 1.2f * st2 && all_finite(L, 3 * q));
}

static void test_engine_sync(void) {
  /* Sync from a Mod OSC at 3/2: the lines are at f0 / 2; the step of every source is band-limited */
  for (int src = 0; src < FC_NSOURCES; src++) {
    reset(SR);
    FcParams p;
    base_params(&p);
    p.source = src; p.color = VA_SAW; p.morph = VA_SAW;
    p.harmonics = src == FC_SRC_FM2 ? FM2_RATIO1 : 0.5f;
    p.route_sync = 1; p.mod_mode = FC_MODE_RATIO;
    int r = 0;
    while (fc_ratios[r] != 1.5f) r++;
    p.mod_freq = (float)r * FC_FREQ_MAX / (FC_NRATIOS - 1);
    fc_set_params(&core, &p);
    fc_note_on(&core, 72, 127);
    render(&core, 0, SR / 4);
    render(&core, 0, SR);
    const double al = alias_db(523.2511 / 2.0);
    printf("sync 3/2, source %d, C5: aliasing %.1f dB\n", src, al);
    CHECK(al < (src == FC_SRC_WAVE ? -45.0 : -55.0));
  }
}

static void test_engine_extremes(void) {
  /* every source, every knob at its ends, FM through zero (saw, 100 %), AM, Sync, the highest notes: bounded,
   * no NaN; knobs jumping while playing */
  const float ends[2] = { 0.0f, 1.0f };
  for (int src = 1; src < FC_NSOURCES; src++)
    for (int e = 0; e < 8; e++) {
      reset(SR);
      FcParams p;
      fc_default_params(&p);
      p.source = src; p.harmonics = ends[e & 1]; p.color = ends[(e >> 1) & 1]; p.morph = ends[e >> 2];
      p.pitch = 24; p.mod_index = 1; p.mod_shape = FC_MOD_SAW;
      p.route_fm = p.route_am = p.route_sync = 1;
      fc_set_params(&core, &p);
      for (int i = 0; i < 4; i++) fc_note_on(&core, 20 + 30 * i, 127);
      render(&core, 0, SR / 8);
      p.harmonics = 1 - p.harmonics; p.color = 1 - p.color; p.morph = 1 - p.morph;
      fc_set_params(&core, &p);
      render(&core, SR / 8, SR / 8);
      CHECK(all_finite(L, SR / 4));
      CHECK(peak(L, 0, SR / 4) <= 1.0f);
    }
}

static void test_soft_clip(void) {
  /* 4 voices, full resonance, folder and Drive: the soft clip keeps the output below 1 */
  reset(SR);
  FcParams p;
  base_params(&p);
  p.gate_mode = FC_GATE_LP; p.gate_res = 1.0f; p.timbre = 1.0f; p.drive = 1.0f;
  fc_set_params(&core, &p);
  for (int i = 0; i < 4; i++) fc_note_on(&core, 40 + 3 * i, 50);
  render(&core, 0, SR);
  printf("soft clip: peak %.3f\n", peak(L, 0, SR));
  CHECK(peak(L, 0, SR) < 1.0f && all_finite(L, SR));
}

int main(void) {
  test_pitch();
  test_release();
  test_voices();
  test_steal();
  test_pedal();
  test_all_off();
  test_extremes();
  test_mod_knob();
  test_fm();
  test_am();
  test_sync();
  test_mod_smooth();
  test_mod_extremes();
  test_timbre_bypass();
  test_folder();
  test_cheb_shapers();
  test_drive();
  test_timbre_smooth();
  test_timbre_route();
  test_timbre_extremes();
  test_fg();
  test_vactrol();
  test_gate_modes();
  test_resonance();
  test_resonance_peak();
  test_gate_switch();
  test_fg2_timbre();
  test_soft_clip();
  test_engine_pitch();
  test_engine_knobs();
  test_source_switch();
  test_rate_switch();
  test_engine_sync();
  test_engine_extremes();
  printf(fails ? "test_core: %d FAILED\n" : "test_core: all passed\n", fails);
  return fails ? 1 : 0;
}
