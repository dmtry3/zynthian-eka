// Copyright 2016 Emilie Gillet.
//
// Author: Emilie Gillet (emilie.o.gillet@gmail.com)
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
// See http://creativecommons.org/licenses/MIT/ for more information.
//
// -----------------------------------------------------------------------------
//
// The three Principal engines of Eka Fold, derived from plaits/dsp/engine/virtual_analog_engine.cc
// (VA_VARIANT 0), fm_engine.cc and wavetable_engine.cc, with plaits/dsp/oscillator/variable_shape_oscillator.h
// and wavetable_oscillator.h (github.com/pichenettes/eurorack). Modified for Eka Fold, 2026-11-12:
// - one sample at a time with a signed phase increment per sample (through-zero FM) at any rate, and an
//   external hard sync (see engines.h);
// - VA: the variable-shape oscillator is band-limited by polyBLEP / polyBLAMP residuals computed from the
//   phase (they hold for either direction of the phase) instead of the stateful BLEPs of the original;
// - FM2: rendered at the voice rate (2x the plugin rate in Eka Fold), not 4x oversampled; no sub output;
// - Wave: the slope of the integrated wave between two phases (signed), with the original low-pass and gain;
//   the table map fixed (no user waves).

#include "engines.h"

#include <math.h>

namespace {

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

inline float wrap(float ph) { return ph - floorf(ph); }

inline float sine(const FeBlock* b, float ph) {          // ph in cycles, any value
  const float x = wrap(ph) * static_cast<float>(b->sine_n);
  int i = static_cast<int>(x);
  if (i >= b->sine_n) i = b->sine_n - 1;
  const float f = x - static_cast<float>(i);
  return b->sine[i] + (b->sine[i + 1] - b->sine[i]) * f;
}

// Residual of a unit (height 2) step at phase 0, by the distance t (0..1) from it in phase.
inline float polyblep(float t, float dt) {
  if (t < dt) { t /= dt; return t + t - t * t - 1.0f; }
  if (t > 1.0f - dt) { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
  return 0.0f;
}

// Residual of a corner (a slope change of 1 per sample) at phase `at`.
inline float polyblamp(float ph, float at, float dt) {
  float d = ph - at;
  d -= floorf(d + 0.5f);
  if (d >= 0.0f && d < dt) { const float t = 1.0f - d / dt; return t * t * t * (1.0f / 6.0f); }
  if (d < 0.0f && d >= -dt) { const float t = 1.0f + d / dt; return t * t * t * (1.0f / 6.0f); }
  return 0.0f;
}

// ---------- VA ----------
// Variable shape: ws 0 = triangle (the up slope pw long), 0.5 = saw, 1 = square of width pw; -1..1.
// dt = |phase increment|; 0 = naive (no correction).
inline float va_osc(float ph, float dt, float ws, float pw) {
  if (dt >= 0.25f) pw = 0.5f;
  else pw = clampf(pw, 2.0f * dt, 1.0f - 2.0f * dt);
  const float sq = ws > 0.5f ? (ws - 0.5f) * 2.0f : 0.0f;
  const float ta = ws < 0.5f ? 1.0f - 2.0f * ws : 0.0f;
  const float square = ph < pw ? 0.0f : 1.0f;
  const float tri = ph < pw ? ph / pw : 1.0f - (ph - pw) / (1.0f - pw);
  float v = ph + (square - ph) * sq;
  v += (tri - v) * ta;
  if (dt > 0.0f) {
    float pa = ph - pw;
    if (pa < 0.0f) pa += 1.0f;
    v += 0.5f * sq * polyblep(pa, dt);                 // step +sq at pw
    v -= 0.5f * (1.0f - ta) * polyblep(ph, dt);        // step -(1 - ta) at the wrap
    const float corner = ta * (1.0f / pw + 1.0f / (1.0f - pw)) * dt;
    v += corner * (polyblamp(ph, 0.0f, dt) - polyblamp(ph, pw, dt));
  }
  return 2.0f * v - 1.0f;
}

// Interval of the second oscillator for Harmonics (VirtualAnalogEngine::ComputeDetuning).
const float kIntervals[5] = { 0.0f, 7.01f, 12.01f, 19.01f, 24.01f };

inline float squash(float x) { return x * x * (3.0f - 2.0f * x); }

float detuning(float h) {
  float d = clampf(2.05f * h - 1.025f, -1.0f, 1.0f);
  const float sign = d < 0.0f ? -1.0f : 1.0f;
  d = d * sign * 3.9999f;
  const int i = static_cast<int>(d);
  const float f = d - static_cast<float>(i);
  return (kIntervals[i] + (kIntervals[i + 1] - kIntervals[i]) * squash(squash(f))) * sign;
}

// ---------- FM2 ----------
inline float fm_carrier(const FeBlock* b, const FeState* s, float cph, float mph) {
  const float mod = sine(b, mph + s->p[3] * s->fb);           // modulator, with its own feedback
  return sine(b, cph + s->p[1] * mod);
}

// ---------- Wave ----------
inline float hermite(const int16_t* t, int i, float f, float* slope) {
  const float xm1 = t[i], x0 = t[i + 1], x1 = t[i + 2], x2 = t[i + 3];
  const float c = (x1 - xm1) * 0.5f;
  const float v = x0 - x1;
  const float w = c + v;
  const float a = w + v + (x2 - x0) * 0.5f;
  const float b_neg = w + a;
  if (slope) *slope = (3.0f * a * f - 2.0f * b_neg) * f + c;   // per table point
  return (((a * f) - b_neg) * f + c) * f + x0;
}

inline const int16_t* wave_at(int x, int y, int z) {
  if (z >= 4) z = 7 - z;                                       // Harmonics comes back through the banks
  int i = x + y * 8 + z * 64;
  if (z == 3) i = i * 101 % FE_NWAVES;                         // the 4th bank: the others shuffled
  return fe_waves + i * FE_WAVE_STRIDE;
}

// The integral of the blended wave at ph (table units); *slope = its slope per point, if asked.
float wave_integral(const FeState* s, float ph, float* slope) {
  const float p = wrap(ph) * static_cast<float>(FE_WAVE_LEN);
  int pi = static_cast<int>(p);
  if (pi >= FE_WAVE_LEN) pi = FE_WAVE_LEN - 1;
  const float pf = p - static_cast<float>(pi);
  const float x = s->p[0], y = s->p[1], z = s->p[2];
  const int xi = static_cast<int>(x), yi = static_cast<int>(y), zi = static_cast<int>(z);
  const float xf = x - static_cast<float>(xi), yf = y - static_cast<float>(yi), zf = z - static_cast<float>(zi);
  float out = 0.0f, out_slope = 0.0f;
  for (int k = 0; k < 8; k++) {
    const int dx = k & 1, dy = (k >> 1) & 1, dz = k >> 2;
    const float w = (dx ? xf : 1.0f - xf) * (dy ? yf : 1.0f - yf) * (dz ? zf : 1.0f - zf);
    if (w == 0.0f) continue;
    float sl = 0.0f;
    out += w * hermite(wave_at(xi + dx, yi + dy, zi + dz), pi, pf, slope ? &sl : nullptr);
    out_slope += w * sl;
  }
  if (slope) *slope = out_slope;
  return out;
}

// The wave itself at ph (from the slope of the integral), -1..1 x the gain.
inline float wave_naive(const FeState* s, const FeBlock* b, float ph) {
  float sl = 0.0f;
  wave_integral(s, ph, &sl);
  return sl * b->wave_gain * static_cast<float>(FE_WAVE_LEN);
}

inline void glide(FeState* s, const FeBlock* b) {
  if (s->fresh) {
    for (int i = 0; i < FE_NPARAMS; i++) s->p[i] = b->t[i];
    s->fresh = 0;
    return;
  }
  for (int i = 0; i < FE_NPARAMS; i++) s->p[i] += (b->t[i] - s->p[i]) * b->glide;
}

}  // namespace

extern "C" {

void fe_block(FeBlock* b, int engine, float harmonics, float color, float morph, float hz, float sr,
              const float* sine_table, int sine_n) {
  harmonics = clampf(harmonics, 0.0f, 1.0f);
  color = clampf(color, 0.0f, 1.0f);
  morph = clampf(morph, 0.0f, 1.0f);
  b->sine = sine_table;
  b->sine_n = sine_n;
  for (int i = 0; i < FE_NPARAMS; i++) b->t[i] = 0.0f;
  b->glide = 1.0f - expf(-1.0f / (0.002f * sr));               // 2 ms
  b->fb_coef = 1.0f - expf(-2.0f * static_cast<float>(M_PI) * 1530.0f / sr);   // 0.05 at 4 x 48 kHz
  const float f48 = clampf(hz / 48000.0f, 1e-6f, 0.5f);        // the original's units: cycles at 48 kHz
  b->wave_lp = 1.0f;
  b->wave_gain = 0.0f;
  switch (engine) {
    case FE_VA:
      b->t[0] = exp2f(detuning(harmonics) * (1.0f / 12.0f));
      b->t[1] = clampf(color * 1.5f, 0.0f, 1.0f);
      b->t[2] = clampf(0.5f + (color - 0.66f) * 1.4f, 0.5f, 0.99f);
      b->t[3] = clampf(morph * 1.5f, 0.0f, 1.0f);
      b->t[4] = clampf(0.5f + (morph - 0.66f) * 1.4f, 0.5f, 0.99f);
      break;
    case FE_FM2: {
      const float x = harmonics * 128.0f;                     // stmlib Interpolate(table, harmonics, 128)
      int i = static_cast<int>(x);
      if (i > FE_FM_QUANTIZER_SIZE - 2) i = FE_FM_QUANTIZER_SIZE - 2;
      const float semis = fe_fm_quantizer[i] + (fe_fm_quantizer[i + 1] - fe_fm_quantizer[i]) * (x - i);
      // a lower index for high modulators, against aliasing
      const float mod_note = 69.0f + 12.0f * log2f(hz > 1.0f ? hz / 440.0f : 1.0f / 440.0f) + semis;
      float taming = clampf(1.0f - (mod_note - 72.0f) * 0.025f, 0.0f, 1.0f);
      taming *= taming;
      const float fb = 2.0f * morph - 1.0f;
      b->t[0] = exp2f(semis * (1.0f / 12.0f));
      b->t[1] = 2.0f * color * color * taming;
      b->t[2] = fb < 0.0f ? 0.5f * fb * fb : 0.0f;            // carrier into the modulator's frequency
      b->t[3] = fb > 0.0f ? 0.25f * fb * fb : 0.0f;           // the modulator into itself
      break;
    }
    case FE_WAVE: {
      float c[3] = { color * 6.9999f, morph * 6.9999f, harmonics * 6.9999f };
      const float q = clampf(c[2] - 3.0f, 0.0f, 1.0f);       // the upper half: no blending
      for (int k = 0; k < 3; k++) {
        const float i = floorf(c[k]);
        float f = c[k] - i;
        const float snapped = clampf((f - 0.5f) * 16.0f, -0.5f, 0.5f) + 0.5f;
        f += q * (snapped - f);
        b->t[k] = i + f;
      }
      // the original glides per sample at 48 kHz; the same time at sr
      b->glide = clampf(2.0f * f48 * (4.0f - 3.0f * q), 0.01f, 0.1f) * (48000.0f / sr);
      b->wave_lp = clampf(static_cast<float>(FE_WAVE_LEN) * hz / sr, 0.0f, 1.0f);
      b->wave_gain = (0.95f - f48) / (static_cast<float>(FE_WAVE_LEN) * 1024.0f);
      break;
    }
    default:
      break;
  }
}

void fe_reset(FeState* s) {
  s->ph[0] = s->ph[1] = 0.0f;
  for (int i = 0; i < FE_NPARAMS; i++) s->p[i] = 0.0f;
  s->fb = 0.0f;
  s->w_prev = 0.0f;
  s->out = 0.0f;
  s->fresh = 1;
}

float fe_tick(FeState* s, const FeBlock* b, int engine, float pinc) {
  const int first = s->fresh;
  glide(s, b);
  switch (engine) {
    case FE_VA: {
      const float inc2 = pinc * s->p[0];
      s->ph[0] = wrap(s->ph[0] + pinc);                         // the phase after this sample, as fe_sync
      s->ph[1] = wrap(s->ph[1] + inc2);
      return 0.5f * (va_osc(s->ph[0], fabsf(pinc), s->p[1], s->p[2]) +
                     va_osc(s->ph[1], fabsf(inc2), s->p[3], s->p[4]));
    }
    case FE_FM2: {
      const float minc = clampf(pinc * s->p[0] * (1.0f + s->fb * s->p[2]), -0.5f, 0.5f);
      s->ph[1] = wrap(s->ph[1] + minc);
      s->ph[0] = wrap(s->ph[0] + pinc);
      const float y = fm_carrier(b, s, s->ph[0], s->ph[1]);
      s->fb += (y - s->fb) * b->fb_coef;
      return y;
    }
    case FE_WAVE: {
      s->ph[0] = wrap(s->ph[0] + pinc);
      const float w = wave_integral(s, s->ph[0], nullptr);
      float slope;
      if (first || fabsf(pinc) < 1e-6f) slope = wave_naive(s, b, s->ph[0]);   // no step to divide by
      else slope = (w - s->w_prev) * b->wave_gain / pinc;
      s->w_prev = w;
      if (first) s->out = slope;
      s->out += (slope - s->out) * b->wave_lp;
      return s->out;
    }
    default:
      return 0.0f;
  }
}

float fe_sync(FeState* s, const FeBlock* b, int engine, float pinc, float t, float* half) {
  if (s->fresh) return fe_tick(s, b, engine, pinc);           // a sync on the very first sample: nothing to cut
  glide(s, b);
  const float late = 1.0f - t;                                  // from the last sample to the restart
  float before = 0.0f, at0 = 0.0f, y = 0.0f;
  switch (engine) {
    case FE_VA: {
      const float inc2 = pinc * s->p[0];
      before = 0.5f * (va_osc(wrap(s->ph[0] + pinc * late), 0.0f, s->p[1], s->p[2]) +
                       va_osc(wrap(s->ph[1] + inc2 * late), 0.0f, s->p[3], s->p[4]));
      at0 = 0.5f * (va_osc(0.0f, 0.0f, s->p[1], s->p[2]) + va_osc(0.0f, 0.0f, s->p[3], s->p[4]));
      s->ph[0] = wrap(pinc * t);
      s->ph[1] = wrap(inc2 * t);
      y = 0.5f * (va_osc(s->ph[0], 0.0f, s->p[1], s->p[2]) + va_osc(s->ph[1], 0.0f, s->p[3], s->p[4]));
      break;
    }
    case FE_FM2: {
      const float minc = clampf(pinc * s->p[0] * (1.0f + s->fb * s->p[2]), -0.5f, 0.5f);
      before = fm_carrier(b, s, s->ph[0] + pinc * late, s->ph[1] + minc * late);
      at0 = fm_carrier(b, s, 0.0f, 0.0f);
      s->ph[0] = wrap(pinc * t);
      s->ph[1] = wrap(minc * t);
      y = fm_carrier(b, s, s->ph[0], s->ph[1]);
      s->fb += (y - s->fb) * b->fb_coef;
      break;
    }
    case FE_WAVE: {
      // the mean of the wave over the path of this sample: up to the restart, then from phase 0 (the step is
      // band-limited by the mean itself, as everywhere in this engine: no half step for the caller)
      const float end = wave_integral(s, s->ph[0] + pinc * late, nullptr);
      s->ph[0] = wrap(pinc * t);
      const float w = wave_integral(s, s->ph[0], nullptr);
      const float path = (end - s->w_prev) + (w - wave_integral(s, 0.0f, nullptr));
      const float slope = fabsf(pinc) < 1e-6f ? wave_naive(s, b, s->ph[0]) : path * b->wave_gain / pinc;
      s->w_prev = w;
      s->out += (slope - s->out) * b->wave_lp;
      *half = 0.0f;
      return s->out;
    }
    default:
      break;
  }
  *half = 0.5f * (at0 - before);
  return y;
}

}  // extern "C"
