/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Core DSP tests for Eka Pulse (no LV2). Renders WAV files into build_test/. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/pulse_fx.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int checks = 0, fails = 0;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void write_wav(const char* path, const float* l, const float* r, uint32_t n, uint32_t sr) {
  FILE* f = fopen(path, "wb");
  if (!f) return;
  uint32_t data_bytes = n * 4;
  uint32_t riff_size = 36 + data_bytes;
  uint16_t fmt = 1, ch = 2, align = 4, bits = 16;
  uint32_t fmt_sz = 16, byte_rate = sr * 4;
  fwrite("RIFF", 1, 4, f); fwrite(&riff_size, 4, 1, f); fwrite("WAVE", 1, 4, f);
  fwrite("fmt ", 1, 4, f); fwrite(&fmt_sz, 4, 1, f); fwrite(&fmt, 2, 1, f); fwrite(&ch, 2, 1, f);
  fwrite(&sr, 4, 1, f); fwrite(&byte_rate, 4, 1, f); fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f);
  fwrite("data", 1, 4, f); fwrite(&data_bytes, 4, 1, f);
  for (uint32_t i = 0; i < n; i++) {
    float fl = l[i] < -1.0f ? -1.0f : (l[i] > 1.0f ? 1.0f : l[i]);
    float fr = r[i] < -1.0f ? -1.0f : (r[i] > 1.0f ? 1.0f : r[i]);
    int16_t s[2] = { (int16_t)lrintf(fl * 32767.0f), (int16_t)lrintf(fr * 32767.0f) };
    fwrite(s, 2, 2, f);
  }
  fclose(f);
}

int main(void) {
  const double sr = 48000.0;
  PulseFx fx;
  PxParams p;

  CHECK(px_init(&fx, sr) == 0);
  px_default_params(&p);

  /* 1. Step shapes */
  CHECK(fabsf(px_eval_shape(PX_SHP_HOLD, 0.5f, 0.0f) - 1.0f) < 1e-5f);
  CHECK(px_eval_shape(PX_SHP_DECAY, 0.1f, 0.0f) > px_eval_shape(PX_SHP_DECAY, 0.9f, 0.0f));
  CHECK(px_eval_shape(PX_SHP_RAMP, 0.9f, 0.0f) > px_eval_shape(PX_SHP_RAMP, 0.1f, 0.0f));
  CHECK(fabsf(px_eval_shape(PX_SHP_PULSE, 0.5f, 0.0f) - 1.0f) < 1e-4f);
  CHECK(px_eval_shape(PX_SHP_2X, 0.51f, 0.0f) > 0.9f);
  CHECK(px_eval_shape(PX_SHP_3X, 0.68f, 0.0f) > 0.9f);

  /* 2. Deterministic polyrhythmic Sync: 4 slots with 16, 5, 7, 8 steps at 120 BPM, 1/16 step = 0.25 beats */
  px_reset(&fx);
  px_set_tempo(&fx, 120.0f, 3.0); /* 3.0 beats = 12 steps of 1/16 */
  float in_l[256], in_r[256], out_l[256], out_r[256];
  for (int i = 0; i < 256; i++) in_l[i] = in_r[i] = 0.5f;
  px_process(&fx, &p, in_l, in_r, out_l, out_r, 1);
  CHECK(fx.st[0].last_step == 12 % 16);
  CHECK(fx.st[1].last_step == 12 % 5);
  CHECK(fx.st[2].last_step == 12 % 7);
  CHECK(fx.st[3].last_step == 12 % 8);

  /* 3. Gate cuts audio according to step levels (step 0 = 100%, step 1 = 0% Hold) */
  px_reset(&fx);
  px_default_params(&p);
  p.slot[PX_SLOT_GATE].steps = 2;
  p.slot[PX_SLOT_GATE].step_rate = 5; /* 1/4 = 0.5 s at 120 BPM */
  p.slot[PX_SLOT_GATE].levels[0] = 1.0f; p.slot[PX_SLOT_GATE].shapes[0] = PX_SHP_HOLD;
  p.slot[PX_SLOT_GATE].levels[1] = 0.0f; p.slot[PX_SLOT_GATE].shapes[1] = PX_SHP_HOLD;
  p.slot[PX_SLOT_GATE].smooth = 0.0f;
  p.slot[PX_SLOT_GATE].param1 = 0.0f; /* Floor = 0% */
  px_set_tempo(&fx, 120.0f, 0.0);

  uint32_t total = (uint32_t)sr; /* 1.0 s = 2 quarter-note steps */
  float* wl = (float*)calloc(total, sizeof(float));
  float* wr = (float*)calloc(total, sizeof(float));
  float* il = (float*)calloc(total, sizeof(float));
  float* ir = (float*)calloc(total, sizeof(float));
  for (uint32_t i = 0; i < total; i++) {
    float s = 0.5f * sinf(2.0f * (float)M_PI * 220.0f * (float)i / (float)sr);
    il[i] = ir[i] = s;
  }
  for (uint32_t off = 0; off < total; off += 256) {
    uint32_t n = (total - off < 256) ? (total - off) : 256;
    px_process(&fx, &p, il + off, ir + off, wl + off, wr + off, n);
  }
  float e_open = 0.0f, e_closed = 0.0f;
  for (uint32_t i = 4000; i < 20000; i++) e_open += wl[i] * wl[i];
  for (uint32_t i = 28000; i < 44000; i++) e_closed += wl[i] * wl[i];
  CHECK(e_open > 100.0f);
  CHECK(e_closed < 1e-4f);

  /* 4. Render 2 seconds of Trance Gate pattern to WAV for listening */
  px_reset(&fx);
  px_default_params(&p);
  px_set_tempo(&fx, 120.0f, 0.0);
  uint32_t n2 = (uint32_t)(sr * 2.0);
  float* wl2 = (float*)calloc(n2, sizeof(float));
  float* wr2 = (float*)calloc(n2, sizeof(float));
  float* il2 = (float*)calloc(n2, sizeof(float));
  float* ir2 = (float*)calloc(n2, sizeof(float));
  for (uint32_t i = 0; i < n2; i++) {
    float s = 0.4f * sinf(2.0f * (float)M_PI * 220.0f * (float)i / (float)sr)
            + 0.25f * sinf(2.0f * (float)M_PI * 330.0f * (float)i / (float)sr);
    il2[i] = ir2[i] = s;
  }
  for (uint32_t off = 0; off < n2; off += 256) {
    uint32_t n = (n2 - off < 256) ? (n2 - off) : 256;
    px_process(&fx, &p, il2 + off, ir2 + off, wl2 + off, wr2 + off, n);
  }
  write_wav("pulse_trance_gate.wav", wl2, wr2, n2, (uint32_t)sr);

  /* 5. Brickwall limiter + NaN/Inf immunity */
  px_reset(&fx);
  px_default_params(&p);
  p.output_db = 6.0f;
  for (int i = 0; i < 256; i++) { in_l[i] = 2.0f; in_r[i] = -2.0f; }
  in_l[10] = NAN; in_r[11] = INFINITY;
  px_process(&fx, &p, in_l, in_r, out_l, out_r, 256);
  int finite_ok = 1;
  float max_abs = 0.0f;
  for (int i = 0; i < 256; i++) {
    if (!isfinite(out_l[i]) || !isfinite(out_r[i])) finite_ok = 0;
    max_abs = fmaxf(max_abs, fmaxf(fabsf(out_l[i]), fabsf(out_r[i])));
  }
  CHECK(finite_ok);
  CHECK(max_abs <= 1.0001f);
  CHECK(fx.lim_gr > 0.1f);

  /* 6. Mix = 0 passes dry untouched */
  px_reset(&fx);
  px_default_params(&p);
  p.mix = 0.0f;
  p.output_db = 0.0f;
  for (int i = 0; i < 256; i++) { in_l[i] = 0.25f * sinf(0.1f * i); in_r[i] = -in_l[i]; }
  for (int b = 0; b < 20; b++) px_process(&fx, &p, in_l, in_r, out_l, out_r, 256);
  float worst = 0.0f;
  for (int i = 0; i < 256; i++) worst = fmaxf(worst, fabsf(out_l[i] - in_l[i]));
  CHECK(worst < 1e-4f);

  free(wl); free(wr); free(il); free(ir);
  free(wl2); free(wr2); free(il2); free(ir2);
  px_free(&fx);

  printf("%s: %d checks, %d failed (core)\n", fails ? "FAIL" : "OK", checks, fails);
  return fails ? 1 : 0;
}
