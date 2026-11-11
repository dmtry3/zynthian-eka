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

static void reset(float sr) {
  fc_init(&core, sr);
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
  CHECK(peak(L, 0, SR / 2) <= FC_GAIN + 1e-4f);
  CHECK(fabsf(peak(L, SR / 4, SR / 2) - FC_GAIN * 0.7f) < 0.003f);       /* sustain 70 % */
  CHECK(!memcmp(L, R, sizeof(float) * SR / 2));                           /* mono for now: L = R */

  FcParams p;
  fc_default_params(&p);
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

  reset(44100);                                                           /* the rate Zynthian may use */
  fc_note_on(&core, 60, 100);
  render(&core, 0, 22050);
  CHECK(fabs(freq(L, 4410, 22050, 44100) - 261.626) < 0.2);
  CHECK(fabsf(peak(L, 11025, 22050) - FC_GAIN * 0.7f * 100.0f / 127.0f) < 0.003f);   /* velocity */
}

static void test_release(void) {
  reset(SR);
  fc_note_on(&core, 60, 127);
  render(&core, 0, SR / 4);
  fc_note_off(&core, 60);
  CHECK(fc_active_voices(&core) == 1);
  CHECK(fc_voice_note(&core, 0) == 60);
  render(&core, SR / 4, SR);                    /* release 400 ms to -60 dB: free (-80 dB) after ~530 ms */
  CHECK(fc_active_voices(&core) == 0);
  CHECK(fc_voice_note(&core, 0) == -1);
  CHECK(peak(L, SR, SR + SR / 4) == 0.0f);
  CHECK(max_step(L, 0, SR + SR / 4) < 0.02f);
  /* the voice ends at the right time: -30 dB 200 ms after note off (half of -60 dB in 400 ms), silent by 600 ms */
  const float mid = peak(L, SR / 4 + SR / 5, SR / 4 + SR / 5 + 480) / (FC_GAIN * 0.7f);
  CHECK(mid > 0.025f && mid < 0.04f);
  CHECK(peak(L, SR / 4 + SR * 6 / 10, SR + SR / 4) < 1e-4f);

  /* sustain 0: the voice ends during the decay, with the key still down */
  reset(SR);
  FcParams p;
  fc_default_params(&p);
  p.sustain = 0.0f;
  fc_set_params(&core, &p);
  fc_note_on(&core, 60, 127);
  render(&core, 0, SR);
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

  /* the same note again: its own voice, from where the envelope is (no click) */
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
  CHECK(core.v[0].stage == FC_RELEASE);
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
  /* a cut without the fade would give up to 0.175 (one voice at sustain); the 2 ms fade about 0.002 */
  CHECK(curve < 0.005f);
  write_wav("steal.wav", L, at);

  /* a voice in release is taken before a held one */
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
  CHECK(core.v[0].stage == FC_SUSTAIN);
  fc_set_sustain(&core, 0);
  CHECK(core.v[0].stage == FC_RELEASE);

  /* the key is still down when the pedal goes up: the note keeps sounding */
  reset(SR);
  fc_set_sustain(&core, 1);
  fc_note_on(&core, 60, 127);
  fc_set_sustain(&core, 0);
  render(&core, 0, SR / 2);
  CHECK(core.v[0].stage == FC_SUSTAIN);
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
  for (int i = 0; i < 4; i++) CHECK(core.v[i].stage == FC_RELEASE);
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
  fc_default_params(&p);
  p.pitch = 24; p.fine = 100; p.attack_ms = 0; p.decay_ms = 0; p.release_ms = 0;
  fc_set_params(&core, &p);
  fc_set_bend(&core, 2.0f);
  fc_note_on(&core, 127, 127);
  fc_note_on(&core, 0, 127);
  render(&core, 0, SR / 10);
  CHECK(all_finite(L, SR / 10));
  CHECK(peak(L, 0, SR / 10) <= 2 * FC_GAIN + 1e-4f);
  /* long envelope times */
  p.pitch = 0; p.attack_ms = 10000; p.decay_ms = 20000; p.release_ms = 20000;
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
  fc_default_params(&p);
  p.attack_ms = 1; p.sustain = 1.0f;
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
  CHECK(peak(L, 0, SR) <= FC_GAIN * 1.1f && all_finite(L, SR));
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
  fc_default_params(&p);
  p.attack_ms = 1; p.sustain = 1.0f;
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
  printf(fails ? "test_core: %d FAILED\n" : "test_core: all passed\n", fails);
  return fails ? 1 : 0;
}
