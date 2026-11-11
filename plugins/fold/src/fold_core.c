/* Eka Fold engine, stage 2. SPDX-License-Identifier: GPL-3.0-or-later */
#include "fold_core.h"

#include <math.h>
#include <string.h>
#if defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#endif

void fc_enable_ftz(void) {
#if defined(__aarch64__)
  uint64_t fpcr;
  __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
  __asm__ volatile("msr fpcr, %0" : : "r"(fpcr | (1u << 24)));          /* FZ */
#elif defined(__x86_64__) || defined(__i386__)
  _mm_setcsr(_mm_getcsr() | 0x8040);                                     /* FTZ | DAZ */
#endif
}

/* ---------- Mod Freq knob ---------- */
const float fc_ratios[FC_NRATIOS] = { 1.0f / 8, 1.0f / 6, 1.0f / 4, 1.0f / 3, 1.0f / 2, 2.0f / 3, 3.0f / 4,
  1.0f, 5.0f / 4, 4.0f / 3, 3.0f / 2, 5.0f / 3, 2.0f, 5.0f / 2, 3.0f, 7.0f / 2, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f,
  9.0f, 10.0f, 12.0f, 16.0f };

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

float fc_free_hz(float u) {
  return FC_FREE_MIN * powf(FC_FREE_MAX / FC_FREE_MIN, clampf(u, 0.0f, FC_FREQ_MAX) / FC_FREQ_MAX);
}
float fc_key_semis(float u) {
  return (clampf(u, 0.0f, FC_FREQ_MAX) / FC_FREQ_MAX * 2.0f - 1.0f) * FC_KEY_RANGE;
}
int fc_ratio_index(float u) {
  return (int)lrintf(clampf(u, 0.0f, FC_FREQ_MAX) * (float)(FC_NRATIOS - 1) / FC_FREQ_MAX);
}

void fc_default_params(FcParams* p) {
  p->pitch = 0.0f;
  p->fine = 0.0f;
  p->attack_ms = 5.0f;
  p->decay_ms = 300.0f;
  p->sustain = 0.7f;
  p->release_ms = 400.0f;
  p->mod_freq = 30.0f;            /* Ratio x1 */
  p->mod_index = 0.0f;            /* a plain sine until the Index is turned up */
  p->mod_shape = FC_MOD_SINE;
  p->mod_mode = FC_MODE_RATIO;
  p->route_fm = 1;
  p->route_am = 0;
  p->route_sync = 0;
  p->route_timbre = 0;
  p->timbre = 0.0f;               /* the Timbre section passes the Principal through until turned up */
  p->order = 0.0f;
  p->symmetry = 0.0f;
  p->drive = 0.0f;
}

/* Kaiser-windowed halfband low-pass for 2x -> 1x: zero at every second tap, cut at a quarter of the 2x rate. */
static double bessel_i0(double x) {
  double sum = 1.0, term = 1.0;
  for (int k = 1; k < 40; k++) {
    term *= (x / (2.0 * k)) * (x / (2.0 * k));
    sum += term;
  }
  return sum;
}

static void make_halfband(float* h) {
  const double beta = 8.0;
  const int mid = (FC_HB_TAPS - 1) / 2;
  double sum = 0.0;
  double t[FC_HB_TAPS];
  for (int i = 0; i < FC_HB_TAPS; i++) {
    const int m = i - mid;
    const double r = (double)m / mid;
    const double w = bessel_i0(beta * sqrt(1.0 - r * r)) / bessel_i0(beta);
    const double sinc = m == 0 ? 0.5 : (m % 2 == 0 ? 0.0 : sin(M_PI * m / 2.0) / (M_PI * m));
    t[i] = sinc * w;
    sum += t[i];
  }
  for (int i = 0; i < FC_HB_TAPS; i++) h[i] = (float)(t[i] / sum);
}

/* Everything that depends on the rate. */
static void set_rate(FoldCore* c) {
  c->sr = c->sr_out * (float)FC_OS;
  c->smooth = 1.0f - expf(-1.0f / (FC_SMOOTH_MS * 0.001f * c->sr));
  c->dc_r = 1.0f - 2.0f * (float)M_PI * FC_DC_HZ / c->sr;
  memset(c->hbuf, 0, sizeof c->hbuf);
}

void fc_init(FoldCore* c, float sr) {
  memset(c, 0, sizeof *c);
  c->sr_out = sr > 1.0f ? sr : 48000.0f;
  for (int i = 0; i <= FC_SINE_N; i++)
    c->table[i] = (float)sin(2.0 * M_PI * (double)i / FC_SINE_N);
  for (int i = 0; i < FC_VOICES; i++) c->v[i].note = -1;
  make_halfband(c->hb);
  set_rate(c);
  FcParams p;
  fc_default_params(&p);
  fc_set_params(c, &p);
}

/* Exponential segment: the time is how long it takes to fall by 60 dB (ln 1000 = 6.9 time constants). */
static float exp_coef(float sr, float ms) {
  if (ms < 0.1f) ms = 0.1f;
  return expf(-6.9078f / (ms * 0.001f * sr));
}

void fc_set_params(FoldCore* c, const FcParams* p) {
  c->tune = p->pitch + p->fine * 0.01f;
  c->att_inc = 1.0f / ((p->attack_ms < 0.5f ? 0.5f : p->attack_ms) * 0.001f * c->sr);
  c->dec_coef = exp_coef(c->sr, p->decay_ms);
  c->rel_coef = exp_coef(c->sr, p->release_ms);
  c->sus_coef = exp_coef(c->sr, 20.0f);
  c->sus = clampf(p->sustain, 0.0f, 1.0f);
  c->mod_shape = p->mod_shape >= 0 && p->mod_shape < FC_NMOD_SHAPES ? p->mod_shape : FC_MOD_SINE;
  c->mod_mode = p->mod_mode >= 0 && p->mod_mode < FC_NMODES ? p->mod_mode : FC_MODE_RATIO;
  c->mod_hz = fc_free_hz(p->mod_freq);
  c->mod_semis = fc_key_semis(p->mod_freq);
  c->mod_ratio = fc_ratios[fc_ratio_index(p->mod_freq)];
  c->idx_t = clampf(p->mod_index, 0.0f, 1.0f);
  c->fm_t = p->route_fm ? 1.0f : 0.0f;
  c->am_t = p->route_am ? 1.0f : 0.0f;
  c->sync = p->route_sync != 0;
  c->tim_route_t = p->route_timbre ? 1.0f : 0.0f;
  c->tim_t = clampf(p->timbre, 0.0f, 1.0f);
  c->ord_t = clampf(p->order, 0.0f, 1.0f);
  c->sym_t = clampf(p->symmetry, -1.0f, 1.0f);
  c->drv_t = clampf(p->drive, 0.0f, 1.0f);
}

static void start_voice(FoldCore* c, FcVoice* v, int note, int vel) {
  v->note = note;
  v->amp = FC_GAIN * (float)vel / 127.0f;
  v->phase = 0.0f;
  if (c->mod_mode != FC_MODE_FREE) v->mphase = 0.0f;   /* Free keeps running: the LFO does not restart */
  v->env = 0.0f;
  v->idx = c->idx_t;
  v->gfm = c->fm_t;
  v->gam = c->am_t;
  v->gtim = c->tim_route_t;
  v->tim = c->tim_t;
  v->ord = c->ord_t;
  v->sym = c->sym_t;
  v->drv = c->drv_t;
  v->au = v->aF = 0.0f;
  v->dcx = v->dcy = 0.0f;
  v->y_prev = 0.0f;
  v->stage = FC_ATTACK;
  v->held = 1;
  v->pedal = 0;
  v->age = ++c->clock;
}

void fc_note_on(FoldCore* c, int note, int vel) {
  if (vel <= 0) { fc_note_off(c, note); return; }
  /* the same note again: retrigger its voice from where its envelope is now (no click) */
  for (int i = 0; i < FC_VOICES; i++) {
    FcVoice* v = &c->v[i];
    if (v->stage != FC_IDLE && v->stage != FC_STEAL && v->note == note) {
      v->amp = FC_GAIN * (float)vel / 127.0f;
      v->stage = FC_ATTACK;
      v->held = 1;
      v->pedal = 0;
      v->age = ++c->clock;
      return;
    }
  }
  for (int i = 0; i < FC_VOICES; i++)
    if (c->v[i].stage == FC_IDLE) { start_voice(c, &c->v[i], note, vel); return; }
  /* no free voice: the oldest one in release, else the oldest one; it fades out first */
  FcVoice* best = NULL;
  for (int pass = 0; pass < 2 && !best; pass++)
    for (int i = 0; i < FC_VOICES; i++) {
      FcVoice* v = &c->v[i];
      if (pass == 0 && v->stage != FC_RELEASE) continue;
      if (!best || v->age < best->age) best = v;
    }
  if (best->stage == FC_STEAL) {                     /* already fading: only the coming note changes */
    best->next_note = note;
    best->next_vel = vel;
    best->age = ++c->clock;
    return;
  }
  best->steal_dec = best->env / (FC_STEAL_MS * 0.001f * c->sr) + 1e-9f;
  best->stage = FC_STEAL;
  best->next_note = note;
  best->next_vel = vel;
  best->age = ++c->clock;
}

void fc_note_off(FoldCore* c, int note) {
  for (int i = 0; i < FC_VOICES; i++) {
    FcVoice* v = &c->v[i];
    if (v->stage == FC_STEAL && v->next_note == note) { v->next_note = -1; continue; }
    if (v->stage == FC_IDLE || v->stage == FC_STEAL || v->note != note || !v->held) continue;
    v->held = 0;
    if (c->sustain_pedal) v->pedal = 1;
    else v->stage = FC_RELEASE;
  }
}

void fc_set_sustain(FoldCore* c, int on) {
  c->sustain_pedal = on;
  if (on) return;
  for (int i = 0; i < FC_VOICES; i++) {
    FcVoice* v = &c->v[i];
    if (v->pedal && v->stage != FC_IDLE && v->stage != FC_STEAL) v->stage = FC_RELEASE;
    v->pedal = 0;
  }
}

void fc_set_bend(FoldCore* c, float semitones) { c->bend = semitones; }

void fc_all_notes_off(FoldCore* c) {
  c->sustain_pedal = 0;
  for (int i = 0; i < FC_VOICES; i++) {
    FcVoice* v = &c->v[i];
    v->held = v->pedal = 0;
    if (v->stage == FC_STEAL) v->next_note = -1;
    else if (v->stage != FC_IDLE) v->stage = FC_RELEASE;
  }
}

void fc_all_sound_off(FoldCore* c) {
  c->sustain_pedal = 0;
  for (int i = 0; i < FC_VOICES; i++) {
    FcVoice* v = &c->v[i];
    v->held = v->pedal = 0;
    if (v->stage == FC_IDLE) continue;
    if (v->stage != FC_STEAL) v->steal_dec = v->env / (FC_STEAL_MS * 0.001f * c->sr) + 1e-9f;
    v->stage = FC_STEAL;
    v->next_note = -1;
  }
}

/* ---------- oscillators ---------- */
static inline float sine(const FoldCore* c, float ph) {   /* ph in cycles, any value */
  ph -= floorf(ph);
  const float x = ph * (float)FC_SINE_N;
  int i = (int)x;
  if (i >= FC_SINE_N) i = FC_SINE_N - 1;
  const float f = x - (float)i;
  return c->table[i] + (c->table[i + 1] - c->table[i]) * f;
}

/* Residual of a unit (height 2) step at phase 0, spread over the samples on both sides (polyBLEP). */
static inline float polyblep(float t, float dt) {
  if (t < dt) { t /= dt; return t + t - t * t - 1.0f; }
  if (t > 1.0f - dt) { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
  return 0.0f;
}

/* Residual of a corner (a slope change of 1 per sample) at phase `at`, spread over the samples on both
 * sides (polyBLAMP, the integral of polyBLEP). */
static inline float polyblamp(float ph, float at, float dt) {
  float d = ph - at;
  d -= floorf(d + 0.5f);                             /* -0.5..0.5 */
  if (d >= 0.0f && d < dt) { const float t = 1.0f - d / dt; return t * t * t * (1.0f / 6.0f); }
  if (d < 0.0f && d >= -dt) { const float t = 1.0f + d / dt; return t * t * t * (1.0f / 6.0f); }
  return 0.0f;
}

/* Mod OSC, -1..1, phase ph 0..1, dt = cycles per sample (> 0, < 0.5). Saw and square are band-limited
 * (polyBLEP at the steps), the triangle too (polyBLAMP at the corners): FM magnifies the aliases of the Mod OSC,
 * a triangle's folded 23rd harmonic gave sidebands at -25 dB on C7. */
static inline float mod_wave(const FoldCore* c, int shape, float ph, float dt) {
  switch (shape) {
    case FC_MOD_TRI: {
      const float y = ph < 0.25f ? 4.0f * ph : ph < 0.75f ? 2.0f - 4.0f * ph : 4.0f * ph - 4.0f;
      const float s = 8.0f * dt;                      /* slope change per sample at each corner */
      return y - s * polyblamp(ph, 0.25f, dt) + s * polyblamp(ph, 0.75f, dt);
    }
    case FC_MOD_SAW: return 2.0f * ph - 1.0f - polyblep(ph, dt);
    case FC_MOD_SQUARE: {
      float p2 = ph + 0.5f;
      if (p2 >= 1.0f) p2 -= 1.0f;
      return (ph < 0.5f ? 1.0f : -1.0f) + polyblep(ph, dt) - polyblep(p2, dt);
    }
    default: return sine(c, ph);
  }
}

/* ---------- Timbre section ---------- */
/* Triangle folder: u inside -1..1 passes, beyond it folds back (period 4). */
static inline float fold(float u) {
  float s = (u + 1.0f) * 0.25f;
  s -= floorf(s);
  return s < 0.5f ? 4.0f * s - 1.0f : 3.0f - 4.0f * s;
}

/* Its antiderivative (periodic, 0 at u = -1). */
static inline float fold_int(float u) {
  float s = (u + 1.0f) * 0.25f;
  s -= floorf(s);
  return s < 0.5f ? 8.0f * s * s - 4.0f * s : 12.0f * s - 8.0f * s * s - 4.0f;
}

/* sum a[k] T_k(u), k = 1..n (Clenshaw). */
static inline float chebyshev(const float* a, int n, float u) {
  float b1 = 0.0f, b2 = 0.0f;
  const float u2 = 2.0f * u;
  for (int k = n; k >= 1; k--) {
    const float b0 = a[k] + u2 * b1 - b2;
    b2 = b1;
    b1 = b0;
  }
  return u * b1 - b2;
}

static inline float fast_tanh(float x) {           /* rational, exact at +-3 (= 1), clamped beyond */
  if (x > 3.0f) return 1.0f;
  if (x < -3.0f) return -1.0f;
  const float x2 = x * x;
  return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

/* Chebyshev weights of the Saw / M blend for a voice: Saw 1/n, M 1/n for odd n, each scaled to a peak of 1
 * (its value at u = 1, the sum of the weights), then mixed by ws : wm. Returns N. */
static int shaper_coefs(float inc, float ws, float wm, float* a) {
  /* below 0.45 of the plugin rate, not of the 2x one: harmonics between 24 and 28 kHz would pass the halfband
   * in part and fold back */
  int n = inc > 0.0f ? (int)(0.45f / ((float)FC_OS * inc)) : FC_CHEB_MAX;
  n = n < 1 ? 1 : n > FC_CHEB_MAX ? FC_CHEB_MAX : n;
  float hs = 0.0f, hm = 0.0f;
  for (int k = 1; k <= n; k++) { hs += 1.0f / (float)k; if (k & 1) hm += 1.0f / (float)k; }
  const float sum = ws + wm > 1e-6f ? ws + wm : 1.0f;
  a[0] = 0.0f;
  for (int k = 1; k <= n; k++)
    a[k] = (ws / hs + ((k & 1) ? wm / hm : 0.0f)) / ((float)k * sum);
  return n;
}

/* Principal cycles per sample for a note. */
static float note_inc(const FoldCore* c, int note) {
  const float semis = (float)note - 69.0f + c->tune + c->bend;
  const float inc = 440.0f * exp2f(semis * (1.0f / 12.0f)) / c->sr;
  return inc < 0.5f ? inc : 0.5f;                    /* not above Nyquist */
}

/* Mod OSC cycles per sample for a voice playing at inc. */
static float mod_inc(const FoldCore* c, float inc) {
  float m;
  if (c->mod_mode == FC_MODE_FREE) m = c->mod_hz / c->sr;
  else if (c->mod_mode == FC_MODE_KEY) m = inc * exp2f(c->mod_semis * (1.0f / 12.0f));
  else m = inc * c->mod_ratio;
  return m < 0.49f ? m : 0.49f;
}

/* n samples of all voices at the voices' rate into l (overwritten). */
static void render_voices(FoldCore* c, float* l, uint32_t n) {
  memset(l, 0, n * sizeof(float));
  const float k = c->smooth;
  float a[FC_CHEB_MAX + 1];
  for (int vi = 0; vi < FC_VOICES; vi++) {
    FcVoice* v = &c->v[vi];
    if (v->stage == FC_IDLE) continue;
    float inc = note_inc(c, v->note);                /* once per block: bend and tune come between blocks */
    float dm = mod_inc(c, inc);
    /* the Saw / M weights for this block (Order changes slowly; the crossfade with the folder is per sample) */
    float o = v->ord;
    int ncheb = shaper_coefs(inc, o < 0.5f ? 2.0f * o : 2.0f - 2.0f * o, o < 0.5f ? 0.0f : 2.0f * o - 1.0f, a);
    for (uint32_t s = 0; s < n; s++) {
      switch (v->stage) {
        case FC_ATTACK:
          v->env += c->att_inc;
          if (v->env >= 1.0f) { v->env = 1.0f; v->stage = FC_DECAY; }
          break;
        case FC_DECAY: {
          const float e = c->sus + (v->env - c->sus) * c->dec_coef;
          /* e == env: the step is below the float resolution near the Sustain level (a long Decay, or the 2x
           * rate), the decay would stall just above it */
          if (e - c->sus < FC_SILENT || e == v->env) {
            v->env = c->sus;
            v->stage = FC_SUSTAIN;
          } else {
            v->env = e;
          }
          break;
        }
        case FC_SUSTAIN:
          v->env = c->sus + (v->env - c->sus) * c->sus_coef;   /* follows the Sustain knob, without a click */
          break;
        case FC_RELEASE:
          v->env *= c->rel_coef;
          break;
        case FC_STEAL:
          v->env -= v->steal_dec;
          if (v->env <= 0.0f) {
            v->env = 0.0f;
            if (v->next_note >= 0) {
              start_voice(c, v, v->next_note, v->next_vel);
              inc = note_inc(c, v->note);
              dm = mod_inc(c, inc);
              o = v->ord;
              ncheb = shaper_coefs(inc, o < 0.5f ? 2.0f * o : 2.0f - 2.0f * o, o < 0.5f ? 0.0f : 2.0f * o - 1.0f, a);
            } else {
              v->stage = FC_IDLE;
            }
          }
          break;
      }
      if (v->stage == FC_IDLE) break;
      if ((v->stage == FC_RELEASE || (v->stage == FC_SUSTAIN && c->sus <= 0.0f)) && v->env < FC_SILENT) {
        v->stage = FC_IDLE;
        v->env = 0.0f;
        break;
      }
      v->idx += (c->idx_t - v->idx) * k;
      v->gfm += (c->fm_t - v->gfm) * k;
      v->gam += (c->am_t - v->gam) * k;
      v->gtim += (c->tim_route_t - v->gtim) * k;
      v->tim += (c->tim_t - v->tim) * k;
      v->ord += (c->ord_t - v->ord) * k;
      v->sym += (c->sym_t - v->sym) * k;
      v->drv += (c->drv_t - v->drv) * k;

      /* Mod OSC */
      const float m = mod_wave(c, c->mod_shape, v->mphase, dm);
      v->mphase += dm;
      const int wrapped = v->mphase >= 1.0f;
      if (wrapped) v->mphase -= 1.0f;

      /* Principal: FM (linear, through zero), then Sync, then AM */
      const float pinc = inc * (1.0f + FC_FM_DEPTH * v->gfm * v->idx * m);
      const float old = v->phase;
      v->phase += pinc;
      v->phase -= floorf(v->phase);
      float y = sine(c, v->phase);
      if (wrapped && c->sync) {
        /* the Mod OSC began a cycle t samples ago: the Principal restarts there; the step is band-limited
         * on the late sample (y_prev, before the step) and this one (after it) */
        const float t = v->mphase / dm;                  /* 0..1 */
        const float before = sine(c, old + pinc * (1.0f - t));
        v->phase = pinc * t;
        v->phase -= floorf(v->phase);
        y = sine(c, v->phase);
        const float h = -0.5f * before;                  /* half the step: from `before` to sin(0) = 0 */
        v->y_prev += h * t * t;
        y -= h * (1.0f - t) * (1.0f - t);
      }
      const float out = v->y_prev;                     /* one sample late */
      v->y_prev = y;
      const float am = v->gam * v->idx;
      const float x = out * ((1.0f - am) + am * m);    /* AM before the shapers: it changes the timbre too */

      /* Timbre section: folder, Saw / M (Chebyshev) blended by Order; Timbre = depth, Symmetry = offset */
      float t = v->tim + FC_TIMBRE_MOD * v->gtim * v->idx * m;
      t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
      const float wf = v->ord < 0.5f ? 1.0f - 2.0f * v->ord : 0.0f;   /* the folder's share */
      float sh = 0.0f;
      const float u = (1.0f + (FC_FOLD_GAIN - 1.0f) * t) * x + v->sym;
      {                                                /* first-order ADAA: the mean of fold() since the last sample */
        const float F = fold_int(u);                   /* (kept up to date at Order >= 50 too: no jump coming back) */
        const float d = u - v->au;
        const float f = fabsf(d) > 1e-4f ? (F - v->aF) / d : fold(0.5f * (u + v->au));
        v->au = u;
        v->aF = F;
        sh = wf * f;
      }
      if (wf < 1.0f) {
        float uc = x + v->sym;
        uc = uc < -1.0f ? -1.0f : uc > 1.0f ? 1.0f : uc;
        const float ch = t > 0.0f ? chebyshev(a, ncheb, uc) : 0.0f;
        sh += (1.0f - wf) * ((1.0f - t) * x + t * ch);
      }
      if (v->drv > 1e-4f) {
        const float kd = 1.0f + FC_DRIVE_K * v->drv;
        sh = (1.0f - v->drv) * sh + v->drv * fast_tanh(kd * sh) / fast_tanh(kd);
      }
      /* DC blocker before the envelope: silence stays exact silence */
      const float dc = sh - v->dcx + c->dc_r * v->dcy;
      v->dcx = sh;
      v->dcy = dc;
      l[s] += dc * v->env * v->amp;
    }
    if (v->stage == FC_IDLE) v->note = -1;
  }
}

void fc_process(FoldCore* c, float* l, float* r, uint32_t n) {
  /* 2x: render a chunk after the history, then the halfband taps for every second sample */
  const int hist = FC_HB_TAPS - 1;
  float* in = c->hbuf + hist;
  for (uint32_t done = 0; done < n; done += FC_CHUNK) {
    const uint32_t k = n - done < FC_CHUNK ? n - done : FC_CHUNK;
    render_voices(c, in, FC_OS * k);
    for (uint32_t j = 0; j < k; j++) {
      const float* x = in + 2 * j + 1;                 /* newest input of this output sample */
      float acc = c->hb[(FC_HB_TAPS - 1) / 2] * x[-(FC_HB_TAPS - 1) / 2];
      for (int i = 0; i < FC_HB_TAPS; i += 2) acc += c->hb[i] * x[-i];   /* the other non-zero taps are even */
      l[done + j] = acc;
    }
    memmove(c->hbuf, in + 2 * k - hist, hist * sizeof(float));
  }
  memcpy(r, l, n * sizeof(float));
}

int fc_voice_note(const FoldCore* c, int i) {
  if (i < 0 || i >= FC_VOICES) return -1;
  const FcVoice* v = &c->v[i];
  if (v->stage == FC_IDLE) return -1;
  if (v->stage == FC_STEAL) return v->next_note;
  return v->note;
}

int fc_active_voices(const FoldCore* c) {
  int k = 0;
  for (int i = 0; i < FC_VOICES; i++) k += c->v[i].stage != FC_IDLE;
  return k;
}

float fc_mod_phase(const FoldCore* c) {
  const FcVoice* best = NULL;
  for (int i = 0; i < FC_VOICES; i++) {
    const FcVoice* v = &c->v[i];
    if (v->stage != FC_IDLE && v->stage != FC_STEAL && (!best || v->age > best->age)) best = v;
  }
  return best ? best->mphase : -1.0f;
}
