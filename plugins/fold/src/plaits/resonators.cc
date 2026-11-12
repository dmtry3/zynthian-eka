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
// The two resonators of Eka Fold's delay block, derived from plaits/dsp/physical_modelling/resonator.cc,
// modal_voice.cc, string.cc and string_voice.cc, with stmlib/dsp/filter.h (Svf, DCBlocker, OnePole::tan) and
// cosine_oscillator.h (github.com/pichenettes/eurorack and stmlib). Modified for Eka Fold, 2026-11-13:
// - no exciter of their own: the voice is their input; one sample at a time, any rate (f0 in cycles per sample;
//   the original's 48 kHz units kept where they set a sound: the modes' Q and their roll-off, the string's damping
//   ceiling and its DC pole in Hz);
// - Modal: the modes above 24 kHz are left out (not only attenuated); Position is a knob (the original: 0.015);
// - String: the line is a buffer of the voice (long enough for any note at 2x: no resampling trick), written
//   forwards (the original counts down; the same delays); one string per voice (the original keeps three and
//   rotates them); the filters' delay at the note computed and taken off the line (in tune within a cent).

#include "resonators.h"

#include <math.h>
#include <string.h>

namespace {

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

// stmlib OnePole::tan<FREQUENCY_FAST>: tan(pi f), tuned for the audio range
inline float tan_fast(float f) {
  const float pi = 3.14159265358979f;
  const float a = 3.260e-01f * pi * pi * pi;
  const float b = 1.823e-01f * pi * pi * pi * pi * pi;
  const float f2 = f * f;
  return f * (pi + f2 * (a + b * f2));
}

// stmlib Interpolate(table, index, size), the table holding size + 1 points: index 1.0 is its last point (the
// original read one past it there)
inline float interpolate(const float* table, float index, float size) {
  index *= size;
  int i = static_cast<int>(index);
  if (i > static_cast<int>(size) - 1) i = static_cast<int>(size) - 1;
  const float f = index - static_cast<float>(i);
  return table[i] + (table[i + 1] - table[i]) * f;
}

inline float semitones_to_ratio(float s) { return exp2f(s * (1.0f / 12.0f)); }

// Resonator::NthHarmonicCompensation
inline float nth_harmonic_compensation(int n, float stiffness) {
  float stretch = 1.0f;
  for (int i = 0; i < n - 1; ++i) {
    stretch += stiffness;
    stiffness *= stiffness < 0.0f ? 0.93f : 0.98f;
  }
  return 1.0f / stretch;
}

inline float rnd(uint32_t* r) {                 // stmlib Random::GetFloat: 0..1
  *r = *r * 1664525u + 1013904223u;
  return static_cast<float>(*r >> 8) * (1.0f / 16777216.0f);
}

}  // namespace

extern "C" {

// ---------- Modal ----------
void fr_modal_reset(FrModal* m) {
  memset(m, 0, sizeof *m);
}

void fr_modal_block(FrModal* m, float f0, float sr, float structure, float brightness, float damping,
                    float position) {
  structure = clampf(structure, 0.0f, 1.0f);
  brightness = clampf(brightness, 0.0f, 1.0f);
  damping = clampf(damping, 0.0f, 1.0f);
  // the modes' amplitudes: stmlib CosineOscillator<COSINE_OSCILLATOR_APPROXIMATE>(position), x 0.25
  float amp[FR_MODES];
  {
    float freq = clampf(position, 0.0f, 1.0f), sign = 16.0f;
    freq -= 0.25f;
    if (freq < 0.0f) freq = -freq;
    else if (freq > 0.5f) freq -= 0.5f;
    else sign = -16.0f;
    const float k = sign * freq * (1.0f - 2.0f * freq);
    float y1 = k * 0.25f, y0 = 0.5f;
    for (int i = 0; i < FR_MODES; i++) {
      const float t = y0;
      y0 = k * y0 - y1;
      y1 = t;
      amp[i] = (t + 0.5f) * 0.25f;
    }
  }
  float stiffness = interpolate(fr_stiffness, structure, 64.0f);
  f0 *= nth_harmonic_compensation(3, stiffness);
  float harmonic = f0;
  float stretch = 1.0f;
  const float q_sqrt = semitones_to_ratio(damping * 79.7f);
  float q = 500.0f * q_sqrt * q_sqrt;
  brightness *= 1.0f - structure * 0.3f;
  brightness *= 1.0f - damping * 0.3f;
  const float q_loss = brightness * (2.0f - brightness) * 0.85f + 0.15f;
  const float to48 = sr / 48000.0f;              // the original's units for Q and the roll-off
  m->n = FR_MODES;
  for (int i = 0; i < FR_MODES; ++i) {
    const float f = harmonic * stretch;          // cycles per sample at sr
    const float f48 = f * to48;
    if (f48 >= 0.499f || f <= 0.0f) { m->n = i; break; }   // above 24 kHz: left out
    const float g = tan_fast(f);
    const float r = 1.0f / (1.0f + f48 * q);
    m->g[i] = g;
    m->rg[i] = r + g;
    m->h[i] = 1.0f / (1.0f + r * g + g * g);
    m->a[i] = amp[i] * (1.0f - f48 * 2.0f);
    stretch += stiffness;
    stiffness *= stiffness < 0.0f ? 0.93f : 0.98f;   // no folding into negative frequencies; a few more on top
    harmonic += f0;
    q *= q_loss;
  }
}

float fr_modal_tick(FrModal* m, float in) {
  float out = 0.0f;
  for (int i = 0; i < m->n; i++) {
    const float hp = (in - m->rg[i] * m->s1[i] - m->s2[i]) * m->h[i];
    const float bp = m->g[i] * hp + m->s1[i];
    m->s1[i] = m->g[i] * hp + bp;
    const float lp = m->g[i] * bp + m->s2[i];
    m->s2[i] = m->g[i] * bp + lp;
    out += m->a[i] * bp;
  }
  return out;
}

// ---------- String ----------
void fr_string_init(FrString* s, float* line, int len) {
  s->line = line;
  s->mask = len - 1;
  s->rng = 0x9E3779B9u;
  fr_string_reset(s);
}

void fr_string_reset(FrString* s) {
  // the line itself is cleared by the voice (only what was written)
  memset(s->stretch, 0, sizeof s->stretch);
  s->wp = s->swp = 0;
  s->s1 = s->s2 = 0.0f;
  s->g = 0.1f; s->r = 2.0f; s->h = 1.0f / (1.0f + s->r * s->g + s->g * s->g);
  s->dc_x = s->dc_y = 0.0f;
  s->dc_pole = 0.999f;
  s->delay = 100.0f;
  s->delay_step = 0.0f;
  s->disp_noise = s->curved = 0.0f;
}

void fr_string_block(FrString* s, float f0, float sr, float structure, float brightness, float damping, int n) {
  structure = clampf(structure, 0.0f, 1.0f);
  brightness = clampf(brightness, 0.0f, 1.0f);
  damping = clampf(damping, 0.0f, 1.0f);
  // StringVoice: Structure -> the non-linearity
  const float nl = structure < 0.24f ? (structure - 0.24f) * 4.166f
                 : structure > 0.26f ? (structure - 0.26f) * 1.35135f : 0.0f;
  s->bridge = nl <= 0.0f;
  const float amount = fabsf(nl);
  // String::ProcessInternal, per block
  float delay = 1.0f / (f0 > 1e-6f ? f0 : 1e-6f);
  delay = clampf(delay, 4.0f, static_cast<float>(s->mask) - 4.0f);
  float cutoff = fminf(12.0f + damping * damping * 60.0f + brightness * 24.0f, 84.0f);
  const float ceiling = fminf(0.499f, 24000.0f / sr);           // the original's 0.499 at 48 kHz
  float damping_f = fminf(f0 * semitones_to_ratio(cutoff), ceiling);
  if (damping >= 0.95f) {                                        // crossfade to infinite decay
    const float to_infinite = 20.0f * (damping - 0.95f);
    brightness += to_infinite * (1.0f - brightness);
    damping_f += to_infinite * (0.4999f - damping_f);
    cutoff += to_infinite * (128.0f - cutoff);
  }
  s->g = tan_fast(damping_f);
  s->r = 1.0f / 0.5f;
  s->h = 1.0f / (1.0f + s->r * s->g + s->g * s->g);
  s->dc_pole = 1.0f - 20.0f / sr;
  s->stretch_point = amount * (2.0f - amount) * 0.225f;
  s->ap_gain = -0.618f * amount / (0.15f + amount);
  s->ap_d = 0;
  if (!s->bridge) {
    const int d = static_cast<int>(delay * s->stretch_point);
    if (d >= 4 && d < FR_STRETCH_LEN - 1) s->ap_d = d;
  }
  // Eka Fold: the delay of everything else in the loop at the note, computed and taken off the line (the original
  // multiplies by lut_svf_shift for the low-pass alone and approximates the all-pass: on 2x and at the 24 kHz
  // ceiling that left C2 13 cents sharp, C7 25 flat, dispersion 8 % sharp): the low-pass (bilinear 2-pole:
  // H = 1 / (1 - (t/g)^2 + j r t/g), t = tan(w/2)), the DC blocker, the dispersion all-pass
  // (H = (-a + z^-D) / (1 - a z^-D)); the partials above stay stretched (that is the dispersion)
  float target = delay;
  if (f0 > 1e-6f) {
    const float w = 2.0f * 3.14159265f * f0;
    const float t = tanf(0.5f * w) / s->g;
    float ph = -atan2f(s->r * t, 1.0f - t * t);
    const float cw = cosf(w), sw = sinf(w);
    ph += atan2f(sw, 1.0f - cw) - atan2f(s->dc_pole * sw, 1.0f - s->dc_pole * cw);
    if (s->ap_d) {
      const float a = s->ap_gain, wd = w * static_cast<float>(s->ap_d);
      const float cd = cosf(wd), sd = sinf(wd);
      // numerator -a + e^(-j wd), denominator 1 - a e^(-j wd); the phase of the numerator unwrapped from -wd
      float ph_ap = atan2f(-sd, -a + cd) - atan2f(a * sd, 1.0f - a * cd);
      ph_ap -= 2.0f * 3.14159265f * floorf((ph_ap + wd) / (2.0f * 3.14159265f) + 0.5f);
      ph += ph_ap;
    }
    target = delay + ph / w;
    target = clampf(target, 4.0f, static_cast<float>(s->mask) - 4.0f);
  }
  s->delay_step = n > 0 ? (target - s->delay) / static_cast<float>(n) : 0.0f;
  if (n <= 0) s->delay = target;
  const float noise_sqrt = amount > 0.75f ? 4.0f * (amount - 0.75f) : 0.0f;
  s->noise_amount = noise_sqrt * noise_sqrt * 0.1f;
  s->noise_filter = 0.06f + 0.94f * brightness * brightness;
  s->bridge_curving = amount * amount * 0.01f;
}

float fr_string_tick(FrString* s, float in) {
  s->delay += s->delay_step;
  float delay = s->delay;
  const int M = s->mask;
  if (!s->bridge) {
    const float noise = rnd(&s->rng) - 0.5f;
    s->disp_noise += s->noise_filter * (noise - s->disp_noise);
    delay *= 1.0f + s->disp_noise * s->noise_amount;
  } else {
    delay *= 1.0f - s->curved * s->bridge_curving;
  }
  float v;
  if (s->ap_d) {                                                // the line, then the dispersion all-pass
    const int i = static_cast<int>(delay);
    const float f = delay - static_cast<float>(i);
    const float a = s->line[(s->wp + 1 - i) & M], b = s->line[(s->wp - i) & M];   // i, i + 1 samples ago
    v = a + (b - a) * f;
    const int SM = FR_STRETCH_LEN - 1;
    const float read = s->stretch[(s->swp + s->ap_d) & SM];
    const float write = v + s->ap_gain * read;
    s->stretch[s->swp] = write;
    s->swp = (s->swp - 1) & SM;
    v = -write * s->ap_gain + read;
  } else {
    const int i = static_cast<int>(delay);
    const float f = delay - static_cast<float>(i);
    const int t = s->wp + 1 - i;                                // i samples ago (wp: the last one written)
    const float xm1 = s->line[(t + 1) & M], x0 = s->line[t & M], x1 = s->line[(t - 1) & M],
                x2 = s->line[(t - 2) & M];
    const float c = (x1 - xm1) * 0.5f, w = c + (x0 - x1), a = w + (x0 - x1) + (x2 - x0) * 0.5f, bn = w + a;
    v = (((a * f) - bn) * f + c) * f + x0;
  }
  if (s->bridge) {
    const float value = fabsf(v) - 0.025f;
    const float sign = v > 0.0f ? 1.0f : -1.5f;
    s->curved = (fabsf(value) + value) * sign;
  }
  v += in;
  v = clampf(v, -20.0f, 20.0f);
  {                                                             // DC blocker
    const float old = s->dc_x;
    s->dc_x = v;
    s->dc_y = s->dc_y * s->dc_pole + v - old;
    v = s->dc_y;
  }
  {                                                             // the damping low-pass
    const float hp = (v - s->r * s->s1 - s->g * s->s1 - s->s2) * s->h;
    const float bp = s->g * hp + s->s1;
    s->s1 = s->g * hp + bp;
    const float lp = s->g * bp + s->s2;
    s->s2 = s->g * bp + lp;
    v = lp;
  }
  s->wp = (s->wp + 1) & M;                                      // the next sample's "now"
  s->line[s->wp] = v;
  return v;
}

}  // extern "C"
