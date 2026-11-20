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

  /* 2. Transport stopped -> plugin stands still at phase 0, audio passes cleanly */
  px_reset(&fx);
  float in_l[256], in_r[256], out_l[256], out_r[256];
  for (int i = 0; i < 256; i++) in_l[i] = in_r[i] = 0.5f;
  px_process(&fx, &p, in_l, in_r, out_l, out_r, 256);
  CHECK(fx.st[3].phase == 0.0f && fx.st[3].last_step == 0);
  CHECK(fabsf(out_l[255] - 0.5f) < 1e-4f);

  /* 2b. Transport start at bar 3, beat 0 -> starts synchronously on beat 1 (step 0) */
  px_set_transport(&fx, 120.0f, 1, 1, 12.0, 4); /* bar 3, barBeat 0 */
  px_process(&fx, &p, in_l, in_r, out_l, out_r, 1);
  CHECK(fx.st[0].last_step == 0 && fx.st[1].last_step == 0 &&
        fx.st[2].last_step == 0 && fx.st[3].last_step == 0);

  /* 2c. Deterministic polyrhythmic Sync: 3.0 beats later = 12 steps of 1/16 */
  px_set_transport(&fx, 120.0f, 1, 1, 15.0, 4); /* +3.0 beats from origin 12.0 */
  px_process(&fx, &p, in_l, in_r, out_l, out_r, 1);
  CHECK(fx.st[0].last_step == 12 % 16);
  CHECK(fx.st[1].last_step == 12 % 5);
  CHECK(fx.st[2].last_step == 12 % 7);
  CHECK(fx.st[3].last_step == 12 % 8);

  /* 2d. Transport stop -> stands still again at phase 0 */
  px_set_transport(&fx, 120.0f, 0, 0, 0.0, 4);
  px_process(&fx, &p, in_l, in_r, out_l, out_r, 256);
  CHECK(fx.st[3].phase == 0.0f && fx.st[3].last_step == 0);

  /* 3. Gate cuts audio according to step levels (step 0 = 100%, step 1 = 0% Hold) */
  px_reset(&fx);
  px_default_params(&p);
  p.slot[PX_SLOT_GATE].steps = 2;
  p.slot[PX_SLOT_GATE].step_rate = 5; /* 1/4 = 0.5 s at 120 BPM */
  p.slot[PX_SLOT_GATE].levels[0] = 1.0f; p.slot[PX_SLOT_GATE].shapes[0] = PX_SHP_HOLD;
  p.slot[PX_SLOT_GATE].levels[1] = 0.0f; p.slot[PX_SLOT_GATE].shapes[1] = PX_SHP_HOLD;
  p.slot[PX_SLOT_GATE].smooth = 0.0f;
  p.slot[PX_SLOT_GATE].param1 = 0.0f; /* Floor = 0% */
  px_set_transport(&fx, 120.0f, 1, 1, 0.0, 4);

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
  px_set_transport(&fx, 120.0f, 1, 1, 0.0, 4);
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

  /* 7. M1: Low Split LR4 crossover preserves clean bass when Gate closes high band */
  px_reset(&fx);
  px_default_params(&p);
  p.low_split_hz = 250.0f;
  p.low_duck = 0;
  p.slot[PX_SLOT_GATE].on = 1;
  p.slot[PX_SLOT_GATE].steps = 1;
  p.slot[PX_SLOT_GATE].levels[0] = 0.0f; /* Gate fully closed on High band */
  p.slot[PX_SLOT_GATE].shapes[0] = PX_SHP_HOLD;
  p.slot[PX_SLOT_GATE].param1 = 0.0f;
  px_set_transport(&fx, 120.0f, 1, 1, 0.0, 4);
  {
    /* 60 Hz sub-bass should pass cleanly; 2000 Hz treble should be muted by closed Gate */
    float bass_e = 0.0f, hi_e = 0.0f;
    for (int b = 0; b < 40; b++) {
      for (int i = 0; i < 256; i++) {
        double t = (double)(b * 256 + i) / sr;
        in_l[i] = in_r[i] = 0.5f * sinf((float)(2.0 * M_PI * 60.0 * t));
      }
      px_process(&fx, &p, in_l, in_r, out_l, out_r, 256);
      if (b >= 30) for (int i = 0; i < 256; i++) bass_e += out_l[i] * out_l[i];
    }
    px_reset(&fx);
    px_set_transport(&fx, 120.0f, 1, 1, 0.0, 4);
    for (int b = 0; b < 40; b++) {
      for (int i = 0; i < 256; i++) {
        double t = (double)(b * 256 + i) / sr;
        in_l[i] = in_r[i] = 0.5f * sinf((float)(2.0 * M_PI * 2000.0 * t));
      }
      px_process(&fx, &p, in_l, in_r, out_l, out_r, 256);
      if (b >= 30) for (int i = 0; i < 256; i++) hi_e += out_l[i] * out_l[i];
    }
    CHECK(bass_e > 200.0f);
    CHECK(hi_e < 0.05f);
  }

  /* 8. M1: Low Duck applies Gate envelope to Low Band */
  px_reset(&fx);
  p.low_duck = 1;
  px_set_transport(&fx, 120.0f, 1, 1, 0.0, 4);
  {
    float ducked_e = 0.0f;
    for (int b = 0; b < 40; b++) {
      for (int i = 0; i < 256; i++) {
        double t = (double)(b * 256 + i) / sr;
        in_l[i] = in_r[i] = 0.5f * sinf((float)(2.0 * M_PI * 60.0 * t));
      }
      px_process(&fx, &p, in_l, in_r, out_l, out_r, 256);
      if (b >= 30) for (int i = 0; i < 256; i++) ducked_e += out_l[i] * out_l[i];
    }
    CHECK(ducked_e < 1e-3f);
  }

  /* 9. M1: All 5 Drive & Crush modes + all 7 Filter/Phaser modes stay finite, bounded, and active */
  for (int m = 0; m < 5; m++) {
    px_reset(&fx);
    px_default_params(&p);
    p.slot[PX_SLOT_GATE].on = 0;
    p.slot[PX_SLOT_DRIVE].on = 1;
    p.slot[PX_SLOT_DRIVE].mode = m;
    p.slot[PX_SLOT_DRIVE].param1 = 75.0f;
    p.slot[PX_SLOT_DRIVE].param2 = 60.0f;
    px_set_transport(&fx, 120.0f, 1, 1, 0.0, 4);
    int ok = 1; float e = 0.0f;
    for (int b = 0; b < 20; b++) {
      for (int i = 0; i < 256; i++) in_l[i] = in_r[i] = 0.4f * sinf(0.05f * (b * 256 + i));
      px_process(&fx, &p, in_l, in_r, out_l, out_r, 256);
      for (int i = 0; i < 256; i++) {
        if (!isfinite(out_l[i]) || !isfinite(out_r[i]) || fabsf(out_l[i]) > 1.001f) ok = 0;
        e += out_l[i] * out_l[i];
      }
    }
    CHECK(ok && e > 1.0f);
  }

  for (int m = 0; m < 7; m++) {
    px_reset(&fx);
    px_default_params(&p);
    p.slot[PX_SLOT_GATE].on = 0;
    p.slot[PX_SLOT_FILTER].on = 1;
    p.slot[PX_SLOT_FILTER].mode = m;
    p.slot[PX_SLOT_FILTER].param1 = 45.0f;
    p.slot[PX_SLOT_FILTER].param2 = 65.0f;
    px_set_transport(&fx, 120.0f, 1, 1, 0.0, 4);
    int ok = 1; float e = 0.0f;
    for (int b = 0; b < 20; b++) {
      for (int i = 0; i < 256; i++) {
        float t = (float)(b * 256 + i) / (float)sr;
        in_l[i] = in_r[i] = 0.25f * sinf(2.0f * (float)M_PI * 220.0f * t)
                          + 0.25f * sinf(2.0f * (float)M_PI * 2200.0f * t);
      }
      px_process(&fx, &p, in_l, in_r, out_l, out_r, 256);
      for (int i = 0; i < 256; i++) {
        if (!isfinite(out_l[i]) || !isfinite(out_r[i]) || fabsf(out_l[i]) > 1.001f) ok = 0;
        e += out_l[i] * out_l[i];
      }
    }
    CHECK(ok && e > 0.5f);
  }

  /* 10. Render 2s polyrhythmic 3-slot (Drive 5 + Filter 7 + Gate 8) with Low Split WAV */
  px_reset(&fx);
  px_default_params(&p);
  p.low_split_hz = 140.0f;
  p.slot[PX_SLOT_DRIVE].on = 1; p.slot[PX_SLOT_DRIVE].mode = PX_DRV_FOLD;
  p.slot[PX_SLOT_FILTER].on = 1; p.slot[PX_SLOT_FILTER].mode = PX_FLT_LP24;
  p.slot[PX_SLOT_GATE].on = 1;
  px_set_transport(&fx, 120.0f, 1, 1, 0.0, 4);
  for (uint32_t off = 0; off < n2; off += 256) {
    uint32_t n = (n2 - off < 256) ? (n2 - off) : 256;
    px_process(&fx, &p, il2 + off, ir2 + off, wl2 + off, wr2 + off, n);
  }
  write_wav("pulse_m1_poly3.wav", wl2, wr2, n2, (uint32_t)sr);

  /* 11. M2: All 4 TIME modes (Curve, Chop, Stutter, TapeStop) stay finite, bounded, and produce audio */
  for (int m = 0; m < 4; m++) {
    px_reset(&fx);
    px_default_params(&p);
    p.slot[PX_SLOT_GATE].on = 0;
    p.slot[PX_SLOT_TIME].on = 1;
    p.slot[PX_SLOT_TIME].mode = m;
    p.slot[PX_SLOT_TIME].param1 = 50.0f;
    p.slot[PX_SLOT_TIME].param2 = 40.0f;
    if (m == PX_TIME_CHOP) {
      /* Exercise all 6 Chop actions: THRU, 1/8, 1/16, 1/2x, REV, STOP */
      float lvls[6] = { 0.0f, 0.15f, 0.40f, 0.65f, 0.82f, 0.96f };
      p.slot[PX_SLOT_TIME].steps = 6;
      for (int k = 0; k < 6; k++) p.slot[PX_SLOT_TIME].levels[k] = lvls[k];
    }
    px_set_transport(&fx, 120.0f, 1, 1, 0.0, 4);
    int ok = 1; float e = 0.0f;
    for (int b = 0; b < 80; b++) {
      for (int i = 0; i < 256; i++) {
        float t = (float)(b * 256 + i) / (float)sr;
        in_l[i] = in_r[i] = 0.4f * sinf(2.0f * (float)M_PI * 330.0f * t);
      }
      px_process(&fx, &p, in_l, in_r, out_l, out_r, 256);
      for (int i = 0; i < 256; i++) {
        if (!isfinite(out_l[i]) || !isfinite(out_r[i]) || fabsf(out_l[i]) > 1.001f) ok = 0;
        e += out_l[i] * out_l[i];
      }
    }
    CHECK(ok && e > 5.0f);
  }

  /* 12. M2: Audio Trigger restarts slot on transient even when transport is stopped */
  px_reset(&fx);
  px_default_params(&p);
  p.slot[PX_SLOT_GATE].trig = PX_TRIG_AUDIO;
  p.slot[PX_SLOT_GATE].threshold_db = -18.0f;
  for (int i = 0; i < 256; i++) in_l[i] = in_r[i] = 0.0f;
  px_process(&fx, &p, in_l, in_r, out_l, out_r, 256);
  CHECK(fx.st[PX_SLOT_GATE].trig_active == 0 && fx.st[PX_SLOT_GATE].phase == 0.0f);
  for (int i = 0; i < 256; i++) in_l[i] = in_r[i] = 0.6f;
  px_process(&fx, &p, in_l, in_r, out_l, out_r, 256);
  CHECK(fx.st[PX_SLOT_GATE].trig_active == 1 && fx.st[PX_SLOT_GATE].phase > 0.0f);

  /* 13. M2: MIDI Trigger starts slot when transport is stopped */
  px_reset(&fx);
  px_default_params(&p);
  p.slot[PX_SLOT_GATE].trig = PX_TRIG_MIDI;
  for (int i = 0; i < 256; i++) in_l[i] = in_r[i] = 0.3f;
  px_process(&fx, &p, in_l, in_r, out_l, out_r, 256);
  CHECK(fx.st[PX_SLOT_GATE].trig_active == 0);
  px_midi_trigger(&fx);
  px_process(&fx, &p, in_l, in_r, out_l, out_r, 256);
  CHECK(fx.st[PX_SLOT_GATE].trig_active == 1 && fx.st[PX_SLOT_GATE].phase > 0.0f);

  /* 14. M2: Env -> Depth modulates Gate depth from input loudness */
  px_reset(&fx);
  px_default_params(&p);
  p.slot[PX_SLOT_GATE].steps = 1;
  p.slot[PX_SLOT_GATE].levels[0] = 0.0f; /* closed gate curve = 0 */
  p.slot[PX_SLOT_GATE].shapes[0] = PX_SHP_HOLD;
  p.slot[PX_SLOT_GATE].depth = 0.0f;     /* base depth 0 -> audio passes */
  p.slot[PX_SLOT_GATE].env_depth = 1.0f; /* loud signal pushes depth -> +1 (closes gate) */
  p.slot[PX_SLOT_GATE].threshold_db = -24.0f;
  p.slot[PX_SLOT_GATE].param1 = 0.0f;
  px_set_transport(&fx, 120.0f, 1, 1, 0.0, 4);
  for (int i = 0; i < 256; i++) in_l[i] = in_r[i] = 0.8f;
  for (int b = 0; b < 15; b++) px_process(&fx, &p, in_l, in_r, out_l, out_r, 256);
  CHECK(fabsf(out_l[255]) < 0.25f); /* strongly attenuated by Env -> Depth */

  /* 15. Render 2s WAV of M2 TIME (Chop) + FILTER + GATE */
  px_reset(&fx);
  px_default_params(&p);
  p.slot[PX_SLOT_TIME].on = 1; p.slot[PX_SLOT_TIME].mode = PX_TIME_CHOP;
  p.slot[PX_SLOT_TIME].steps = 6;
  p.slot[PX_SLOT_TIME].levels[0] = 0.0f;  p.slot[PX_SLOT_TIME].levels[1] = 0.40f;
  p.slot[PX_SLOT_TIME].levels[2] = 0.0f;  p.slot[PX_SLOT_TIME].levels[3] = 0.65f;
  p.slot[PX_SLOT_TIME].levels[4] = 0.82f; p.slot[PX_SLOT_TIME].levels[5] = 0.96f;
  p.slot[PX_SLOT_FILTER].on = 1;
  p.slot[PX_SLOT_GATE].on = 1;
  px_set_transport(&fx, 120.0f, 1, 1, 0.0, 4);
  for (uint32_t off = 0; off < n2; off += 256) {
    uint32_t n = (n2 - off < 256) ? (n2 - off) : 256;
    px_process(&fx, &p, il2 + off, ir2 + off, wl2 + off, wr2 + off, n);
  }
  write_wav("pulse_m2_time.wav", wl2, wr2, n2, (uint32_t)sr);

  free(wl); free(wr); free(il); free(ir);
  free(wl2); free(wr2); free(il2); free(ir2);
  px_free(&fx);

  printf("%s: %d checks, %d failed (core)\n", fails ? "FAIL" : "OK", checks, fails);
  return fails ? 1 : 0;
}
