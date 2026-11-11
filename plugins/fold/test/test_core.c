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

int main(void) {
  test_pitch();
  test_release();
  test_voices();
  test_steal();
  test_pedal();
  test_all_off();
  test_extremes();
  printf(fails ? "test_core: %d FAILED\n" : "test_core: all passed\n", fails);
  return fails ? 1 : 0;
}
