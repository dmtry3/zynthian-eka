/* Eka Fold engine, stage 4. SPDX-License-Identifier: GPL-3.0-or-later */
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
  p->source = FC_SRC_SINE;
  p->harmonics = 0.5f;            /* VA: unison; FM2: 2:1, 16 cents wide (the quantizer); Wave: the middle */
  p->color = 0.5f;
  p->morph = 0.5f;
  p->fg_rise_ms[0] = 5.0f;
  p->fg_fall_ms[0] = 300.0f;
  p->fg_mode[0] = FC_FG_GATE;     /* plays like an envelope from the keyboard; Once is the Buchla "strike" */
  p->fg_rise_ms[1] = 300.0f;
  p->fg_fall_ms[1] = 800.0f;
  p->fg_mode[1] = FC_FG_GATE;
  p->gate_mode = FC_GATE_COMBO;
  p->gate_decay = 0.3f;
  p->gate_res = 0.0f;
  p->level = 0.8f;
  p->fg2_timbre = 0.0f;
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

/* Kaiser-windowed halfband low-pass of n taps (odd): zero at every second tap from the middle, cut at a quarter
 * of its input rate. */
static double bessel_i0(double x) {
  double sum = 1.0, term = 1.0;
  for (int k = 1; k < 40; k++) {
    term *= (x / (2.0 * k)) * (x / (2.0 * k));
    sum += term;
  }
  return sum;
}

static void make_halfband(float* h, int n) {
  const double beta = 8.0;
  const int mid = (n - 1) / 2;
  double sum = 0.0;
  double t[FC_HB_TAPS];
  for (int i = 0; i < n; i++) {
    const int m = i - mid;
    const double r = (double)m / mid;
    const double w = bessel_i0(beta * sqrt(1.0 - r * r)) / bessel_i0(beta);
    const double sinc = m == 0 ? 0.5 : (m % 2 == 0 ? 0.0 : sin(M_PI * m / 2.0) / (M_PI * m));
    t[i] = sinc * w;
    sum += t[i];
  }
  for (int i = 0; i < n; i++) h[i] = (float)(t[i] / sum);
}

/* Everything that depends on the rate (and not on the parameters). */
static void set_rate(FoldCore* c) {
  for (int i = 0; i < 2; i++) {
    FcRate* r = &c->rt[i];
    const int os = i ? FC_OS_HI : FC_OS;
    r->sr = c->sr_out * (float)os;
    r->step = 1.0f / (float)os;
    r->smooth = 1.0f - expf(-1.0f / (FC_SMOOTH_MS * 0.001f * r->sr));
    r->dc_r = 1.0f - 2.0f * (float)M_PI * FC_DC_HZ / r->sr;
    r->src_fade = 1.0f / (FC_SRC_FADE_MS * 0.001f * r->sr);
  }
  memset(c->hbuf, 0, sizeof c->hbuf);
  memset(c->dbuf, 0, sizeof c->dbuf);
  memset(c->qbuf, 0, sizeof c->qbuf);
  c->q_tail = 0;
}

void fc_init(FoldCore* c, float sr) {
  memset(c, 0, sizeof *c);
  c->sr_out = sr > 1.0f ? sr : 48000.0f;
  for (int i = 0; i <= FC_SINE_N; i++)
    c->table[i] = (float)sin(2.0 * M_PI * (double)i / FC_SINE_N);
  for (int i = 0; i < FC_VOICES; i++) c->v[i].note = -1;
  make_halfband(c->hb, FC_HB_TAPS);
  make_halfband(c->hb4, FC_HB4_TAPS);
  set_rate(c);
  FcParams p;
  fc_default_params(&p);
  fc_set_params(c, &p);
}

float fc_decay_ms(float decay) {
  return FC_DECAY_MIN_MS * powf(FC_DECAY_MAX_MS / FC_DECAY_MIN_MS, clampf(decay, 0.0f, 1.0f));
}

/* A segment of `ms` as a linear step per sample. */
static float seg_inc(float sr, float ms) {
  return 1.0f / (clampf(ms, FC_FG_MIN_MS, FC_FG_MAX_MS) * 0.001f * sr);
}

void fc_set_params(FoldCore* c, const FcParams* p) {
  c->tune = p->pitch + p->fine * 0.01f;
  c->src_t = p->source >= 0 && p->source < FC_NSOURCES ? p->source : FC_SRC_SINE;
  c->harm_t = clampf(p->harmonics, 0.0f, 1.0f);
  c->color_t = clampf(p->color, 0.0f, 1.0f);
  c->morph_t = clampf(p->morph, 0.0f, 1.0f);
  for (int i = 0; i < 2; i++) {
    c->fg_rise_ms[i] = p->fg_rise_ms[i];
    c->fg_fall_ms[i] = p->fg_fall_ms[i];
    c->fg_mode[i] = p->fg_mode[i] >= 0 && p->fg_mode[i] < FC_NFG_MODES ? p->fg_mode[i] : FC_FG_GATE;
  }
  const int gm = p->gate_mode >= 0 && p->gate_mode < FC_NGATE_MODES ? p->gate_mode : FC_GATE_COMBO;
  c->wv_t = gm != FC_GATE_LP ? 1.0f : 0.0f;
  c->wl_t = gm != FC_GATE_VCA ? 1.0f : 0.0f;
  c->decay_ms = fc_decay_ms(p->gate_decay);
  for (int i = 0; i < 2; i++) {
    FcRate* r = &c->rt[i];
    for (int g = 0; g < 2; g++) {
      r->fg_rise_inc[g] = seg_inc(r->sr, c->fg_rise_ms[g]);
      r->fg_fall_inc[g] = seg_inc(r->sr, c->fg_fall_ms[g]);
    }
    r->vac_rise = 1.0f / (FC_VAC_RISE_MS * 0.001f * r->sr);
    r->vac_fall = 1.0f / (c->decay_ms * 0.001f * r->sr);
  }
  c->res_t = clampf(p->gate_res, 0.0f, 1.0f);
  c->lvl_t = clampf(p->level, 0.0f, 1.0f);
  c->fg2_tim = clampf(p->fg2_timbre, -1.0f, 1.0f);
  c->fmax = FC_LPG_FMAX < 0.45f * c->sr_out ? FC_LPG_FMAX : 0.45f * c->sr_out;
  c->mod_shape = p->mod_shape >= 0 && p->mod_shape < FC_NMOD_SHAPES ? p->mod_shape : FC_MOD_SINE;
  c->mod_mode = p->mod_mode >= 0 && p->mod_mode < FC_NMODES ? p->mod_mode : FC_MODE_RATIO;
  c->mod_hz = fc_free_hz(p->mod_freq);
  c->mod_semis = fc_key_semis(p->mod_freq);
  c->mod_ratio = fc_ratios[fc_ratio_index(p->mod_freq)];
  c->idx_t = clampf(p->mod_index, 0.0f, 1.0f);
  c->fm_t = p->route_fm ? 1.0f : 0.0f;
  c->am_t = p->route_am ? 1.0f : 0.0f;
  c->sync = p->route_sync != 0;
  c->engines_4x = p->engines_4x != 0;
  c->tim_route_t = p->route_timbre ? 1.0f : 0.0f;
  c->tim_t = clampf(p->timbre, 0.0f, 1.0f);
  c->ord_t = clampf(p->order, 0.0f, 1.0f);
  c->sym_t = clampf(p->symmetry, -1.0f, 1.0f);
  c->drv_t = clampf(p->drive, 0.0f, 1.0f);
}

static void trigger(FoldCore* c, FcVoice* v) {
  (void)c;
  for (int i = 0; i < 2; i++) v->fg[i].seg = FC_SEG_RISE;   /* from where it is: a retrigger does not jump */
}

static float velocity(int vel) { return FC_VEL_FLOOR + (1.0f - FC_VEL_FLOOR) * (float)vel / 127.0f; }

static void start_voice(FoldCore* c, FcVoice* v, int note, int vel) {
  v->note = note;
  v->vel = velocity(vel);
  v->phase = 0.0f;
  v->src = v->src_prev = c->src_t;
  v->xf = 1.0f;
  if (v->src != FC_SRC_SINE) fe_reset(&v->fe[v->src - 1]);
  v->hi = c->engines_4x && v->src != FC_SRC_SINE;    /* its rate for the whole note (see FC_OS_HI) */
  if (c->mod_mode != FC_MODE_FREE) v->mphase = 0.0f;   /* Free keeps running: the LFO does not restart */
  for (int i = 0; i < 2; i++) v->fg[i].lvl = 0.0f;
  v->vac = 0.0f;
  v->ic1 = v->ic2 = 0.0f;
  v->coef_count = 0;
  v->sg = 1.0f;
  v->wv = c->wv_t;
  v->wl = c->wl_t;
  v->lvl = c->lvl_t;
  v->res = c->res_t;
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
  v->stage = FC_PLAY;
  v->held = 1;
  v->pedal = 0;
  v->age = ++c->clock;
  trigger(c, v);
}

static void steal(FoldCore* c, FcVoice* v) {
  if (v->stage != FC_STEAL) v->steal_dec = v->sg / (FC_STEAL_MS * 0.001f * c->sr_out) + 1e-9f;
  v->stage = FC_STEAL;
}

void fc_note_on(FoldCore* c, int note, int vel) {
  if (vel <= 0) { fc_note_off(c, note); return; }
  /* the same note again: retrigger its voice from where it is (no click) */
  for (int i = 0; i < FC_VOICES; i++) {
    FcVoice* v = &c->v[i];
    if (v->stage == FC_PLAY && v->note == note) {
      v->vel = velocity(vel);
      v->held = 1;
      v->pedal = 0;
      v->age = ++c->clock;
      trigger(c, v);
      return;
    }
  }
  for (int i = 0; i < FC_VOICES; i++)
    if (c->v[i].stage == FC_IDLE) { start_voice(c, &c->v[i], note, vel); return; }
  /* no free voice: the oldest one let go (key and pedal up), else the oldest one; it fades out first */
  FcVoice* best = NULL;
  for (int pass = 0; pass < 2 && !best; pass++)
    for (int i = 0; i < FC_VOICES; i++) {
      FcVoice* v = &c->v[i];
      if (pass == 0 && (v->stage != FC_PLAY || v->held || v->pedal)) continue;
      if (!best || v->age < best->age) best = v;
    }
  steal(c, best);
  best->next_note = note;
  best->next_vel = vel;
  best->age = ++c->clock;
}

void fc_note_off(FoldCore* c, int note) {
  for (int i = 0; i < FC_VOICES; i++) {
    FcVoice* v = &c->v[i];
    if (v->stage == FC_STEAL && v->next_note == note) { v->next_note = -1; continue; }
    if (v->stage != FC_PLAY || v->note != note || !v->held) continue;
    v->held = 0;
    if (c->sustain_pedal) v->pedal = 1;
  }
}

void fc_set_sustain(FoldCore* c, int on) {
  c->sustain_pedal = on;
  if (on) return;
  for (int i = 0; i < FC_VOICES; i++) c->v[i].pedal = 0;
}

void fc_set_bend(FoldCore* c, float semitones) { c->bend = semitones; }

void fc_all_notes_off(FoldCore* c) {
  c->sustain_pedal = 0;
  for (int i = 0; i < FC_VOICES; i++) {
    FcVoice* v = &c->v[i];
    v->held = v->pedal = 0;
    if (v->stage == FC_STEAL) v->next_note = -1;
  }
}

void fc_all_sound_off(FoldCore* c) {
  c->sustain_pedal = 0;
  for (int i = 0; i < FC_VOICES; i++) {
    FcVoice* v = &c->v[i];
    v->held = v->pedal = 0;
    if (v->stage == FC_IDLE) continue;
    steal(c, v);
    v->next_note = -1;
  }
}

/* One step of a function generator; gate = the key or the pedal is down. */
static inline void fg_step(FcFg* g, int mode, int gate, float rise, float fall) {
  switch (g->seg) {
    case FC_SEG_RISE:
      if (mode == FC_FG_GATE && !gate) { g->seg = FC_SEG_FALL; break; }   /* let go while rising */
      g->lvl += rise;
      if (g->lvl >= 1.0f) { g->lvl = 1.0f; g->seg = mode == FC_FG_GATE ? FC_SEG_HOLD : FC_SEG_FALL; }
      break;
    case FC_SEG_HOLD:
      if (!gate || mode != FC_FG_GATE) g->seg = FC_SEG_FALL;
      break;
    case FC_SEG_FALL:
      g->lvl -= fall;
      if (g->lvl <= 0.0f) { g->lvl = 0.0f; g->seg = mode == FC_FG_CYCLE && gate ? FC_SEG_RISE : FC_SEG_IDLE; }
      break;
    default:
      break;
  }
}

/* Low-pass coefficients (TPT state-variable filter) for the vactrol state. */
static void lpg_coefs(const FoldCore* c, const FcRate* r, FcVoice* v) {
  const float ctl = 1.0f - v->wl + v->wl * v->vac;               /* LP share follows g, the rest stays open */
  float fc = FC_LPG_FMIN * powf(c->fmax / FC_LPG_FMIN, ctl);
  const float g = tanf((float)M_PI * fc / r->sr);
  const float k = 1.41421356f * (1.0f - 0.96f * v->res);         /* Butterworth at 0, Q ~ 18 at 100 % */
  v->a1 = 1.0f / (1.0f + g * (g + k));
  v->a2 = g * v->a1;
  v->a3 = g * v->a2;
}

/* ---------- oscillators ---------- *//* ---------- oscillators ---------- */
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

/* Chebyshev weights of the Saw / M blend for a voice at inc (cycles per plugin sample): Saw 1/n, M 1/n for odd n,
 * each scaled to a peak of 1 (its value at u = 1, the sum of the weights), then mixed by ws : wm. Returns N. */
static int shaper_coefs(float inc, float ws, float wm, float* a) {
  /* below 0.45 of the plugin rate, not of the voice's one: harmonics between 24 and 28 kHz would pass the
   * halfband in part and fold back */
  int n = inc > 0.0f ? (int)(0.45f / inc) : FC_CHEB_MAX;
  n = n < 1 ? 1 : n > FC_CHEB_MAX ? FC_CHEB_MAX : n;
  float hs = 0.0f, hm = 0.0f;
  for (int k = 1; k <= n; k++) { hs += 1.0f / (float)k; if (k & 1) hm += 1.0f / (float)k; }
  const float sum = ws + wm > 1e-6f ? ws + wm : 1.0f;
  a[0] = 0.0f;
  for (int k = 1; k <= n; k++)
    a[k] = (ws / hs + ((k & 1) ? wm / hm : 0.0f)) / ((float)k * sum);
  return n;
}

/* Principal cycles per sample (at sr) for a note. */
static float note_inc(const FoldCore* c, float sr, int note) {
  const float semis = (float)note - 69.0f + c->tune + c->bend;
  const float inc = 440.0f * exp2f(semis * (1.0f / 12.0f)) / sr;
  return inc < 0.5f ? inc : 0.5f;                    /* not above Nyquist */
}

/* Mod OSC cycles per sample for a voice playing at inc. */
static float mod_inc(const FoldCore* c, float sr, float inc) {
  float m;
  if (c->mod_mode == FC_MODE_FREE) m = c->mod_hz / sr;
  else if (c->mod_mode == FC_MODE_KEY) m = inc * exp2f(c->mod_semis * (1.0f / 12.0f));
  else m = inc * c->mod_ratio;
  return m < 0.49f ? m : 0.49f;
}

/* A change of Source starts a crossfade (a new one waits until the last has ended); the voice keeps its rate. */
static void voice_source(const FoldCore* c, FcVoice* v) {
  if (v->xf >= 1.0f && v->src != c->src_t) {
    v->src_prev = v->src;
    v->src = c->src_t;
    v->xf = 0.0f;
    if (v->src != FC_SRC_SINE) fe_reset(&v->fe[v->src - 1]);
  }
}

/* The engine blocks of a voice for the coming samples (its Source and, while it fades, the old one). */
static void voice_blocks(const FoldCore* c, const FcRate* r, const FcVoice* v, float inc, FeBlock* blk) {
  const float hz = inc * r->sr;
  if (v->src != FC_SRC_SINE)
    fe_block(&blk[0], v->src - 1, c->harm_t, c->color_t, c->morph_t, hz, r->sr, c->table, FC_SINE_N);
  if (v->xf < 1.0f && v->src_prev != FC_SRC_SINE)
    fe_block(&blk[1], v->src_prev - 1, c->harm_t, c->color_t, c->morph_t, hz, r->sr, c->table, FC_SINE_N);
}

/* One sample of a Source of a voice at the phase increment pinc. With a sync (t >= 0: the Mod OSC began a cycle
 * t samples ago), the source restarts there and *half = half its step, for the caller's band-limiting. */
static inline float principal(const FoldCore* c, FcVoice* v, int src, const FeBlock* b, float pinc, float t,
                              float* half) {
  if (src != FC_SRC_SINE) {
    FeState* s = &v->fe[src - 1];
    return t < 0.0f ? fe_tick(s, b, src - 1, pinc) : fe_sync(s, b, src - 1, pinc, t, half);
  }
  const float old = v->phase;
  v->phase += pinc;
  v->phase -= floorf(v->phase);
  if (t < 0.0f) return sine(c, v->phase);
  const float before = sine(c, old + pinc * (1.0f - t));
  v->phase = pinc * t;
  v->phase -= floorf(v->phase);
  *half = -0.5f * before;                            /* from `before` to sin(0) = 0 */
  return sine(c, v->phase);
}

/* n samples of voice v at its rate r, added to l. */
static void render_voice(FoldCore* c, FcVoice* v, const FcRate* r, float* l, uint32_t n) {
  const float k = r->smooth;
  float a[FC_CHEB_MAX + 1];
  FeBlock blk[2];
  {
    float inc = note_inc(c, r->sr, v->note);         /* once per block: bend and tune come between blocks */
    float dm = mod_inc(c, r->sr, inc);
    voice_blocks(c, r, v, inc, blk);
    /* the Saw / M weights for this block (Order changes slowly; the crossfade with the folder is per sample) */
    float o = v->ord;
    int ncheb = shaper_coefs(inc / r->step, o < 0.5f ? 2.0f * o : 2.0f - 2.0f * o,
                             o < 0.5f ? 0.0f : 2.0f * o - 1.0f, a);
    for (uint32_t s = 0; s < n; s++) {
      if (v->stage == FC_STEAL) {
        v->sg -= v->steal_dec * r->step;
        if (v->sg <= 0.0f) {
          if (v->next_note >= 0 && (c->engines_4x && c->src_t != FC_SRC_SINE) != v->hi) {
            v->sg = 0.0f;                                /* the new note needs the other rate: from the next chunk */
            break;
          }
          if (v->next_note >= 0) {
            start_voice(c, v, v->next_note, v->next_vel);
            inc = note_inc(c, r->sr, v->note);
            dm = mod_inc(c, r->sr, inc);
            voice_blocks(c, r, v, inc, blk);
            o = v->ord;
            ncheb = shaper_coefs(inc / r->step, o < 0.5f ? 2.0f * o : 2.0f - 2.0f * o,
                                 o < 0.5f ? 0.0f : 2.0f * o - 1.0f, a);
          } else {
            v->stage = FC_IDLE;
            break;
          }
        }
      }
      /* function generators and the vactrol */
      const int gate = v->held || v->pedal;
      for (int i = 0; i < 2; i++) fg_step(&v->fg[i], c->fg_mode[i], gate, r->fg_rise_inc[i], r->fg_fall_inc[i]);
      const float drive_in = v->fg[0].lvl * v->vel;
      if (drive_in > v->vac) v->vac += (drive_in - v->vac) * r->vac_rise;
      else v->vac += (drive_in - v->vac) * r->vac_fall / (1.0f + FC_VAC_SLOW * (1.0f - v->vac));
      if (v->stage == FC_PLAY && v->fg[0].seg == FC_SEG_IDLE && v->vac < FC_GATE_OFF) {
        v->stage = FC_IDLE;
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
      v->wv += (c->wv_t - v->wv) * k;
      v->wl += (c->wl_t - v->wl) * k;
      v->lvl += (c->lvl_t - v->lvl) * k;
      v->res += (c->res_t - v->res) * k;

      /* Mod OSC */
      const float m = mod_wave(c, c->mod_shape, v->mphase, dm);
      v->mphase += dm;
      const int wrapped = v->mphase >= 1.0f;
      if (wrapped) v->mphase -= 1.0f;

      /* Principal: FM (linear, through zero), then Sync, then AM */
      const float pinc = inc * (1.0f + FC_FM_DEPTH * v->gfm * v->idx * m);
      /* with Sync, the Mod OSC began a cycle ts samples ago: the Principal restarts there; the step is
       * band-limited on the late sample (y_prev, before the step) and this one (after it) */
      const float ts = wrapped && c->sync ? v->mphase / dm : -1.0f;   /* 0..1 */
      float half = 0.0f;
      float y = principal(c, v, v->src, &blk[0], pinc, ts, &half);
      if (v->xf < 1.0f) {                              /* Source changed: crossfade from the old one */
        float h2 = 0.0f;
        const float y2 = principal(c, v, v->src_prev, &blk[1], pinc, ts, &h2);
        y = v->xf * y + (1.0f - v->xf) * y2;
        half = v->xf * half + (1.0f - v->xf) * h2;
        v->xf += r->src_fade;
        if (v->xf > 1.0f) v->xf = 1.0f;
      }
      if (ts >= 0.0f) {
        v->y_prev += half * ts * ts;
        y -= half * (1.0f - ts) * (1.0f - ts);
      }
      const float out = v->y_prev;                     /* one sample late */
      v->y_prev = y;
      const float am = v->gam * v->idx;
      const float x = out * ((1.0f - am) + am * m);    /* AM before the shapers: it changes the timbre too */

      /* Timbre section: folder, Saw / M (Chebyshev) blended by Order; Timbre = depth, Symmetry = offset */
      float t = v->tim + FC_TIMBRE_MOD * v->gtim * v->idx * m + c->fg2_tim * v->fg[1].lvl;
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
      /* DC blocker before the gate: silence stays exact silence */
      const float dc = sh - v->dcx + r->dc_r * v->dcy;
      v->dcx = sh;
      v->dcy = dc;

      /* lopass gate: the low-pass (LP share), then the level (VCA share), the fade at the end */
      if (--v->coef_count <= 0) { lpg_coefs(c, r, v); v->coef_count = FC_COEF_EVERY; }
      const float v3 = dc - v->ic2;
      const float v1 = v->a1 * v->ic1 + v->a2 * v3;
      const float v2 = v->ic2 + v->a2 * v->ic1 + v->a3 * v3;
      v->ic1 = 2.0f * v1 - v->ic1;
      v->ic1 = FC_RES_SAT * fast_tanh(v->ic1 * (1.0f / FC_RES_SAT));   /* resonance limits itself */
      v->ic2 = 2.0f * v2 - v->ic2;
      const float filtered = dc + v->wl * (v2 - dc);
      float fade = (v->vac - FC_GATE_OFF) * (1.0f / FC_GATE_OFF);
      fade = fade < 0.0f ? 0.0f : fade > 1.0f ? 1.0f : fade;
      const float gain = (1.0f - v->wv + v->wv * v->vac) * fade * v->lvl * v->sg * FC_GAIN;
      l[s] += filtered * gain;
    }
    if (v->stage == FC_IDLE) v->note = -1;
  }
}

void fc_process(FoldCore* c, float* l, float* r, uint32_t n) {
  /* per chunk: the 2x voices (FC_HB4_DELAY late) and the 4x voices taken down to 2x make the 2x mix, then the
   * halfband taps for every second sample of it */
  const int hist = FC_HB_TAPS - 1, qhist = FC_HB4_TAPS - 1, mid4 = (FC_HB4_TAPS - 1) / 2;
  float* in = c->hbuf + hist;
  float* din = c->dbuf + FC_HB4_DELAY;
  float* qin = c->qbuf + qhist;
  for (uint32_t done = 0; done < n; done += FC_CHUNK) {
    const uint32_t k = n - done < FC_CHUNK ? n - done : FC_CHUNK;
    const uint32_t k2 = FC_OS * k;
    memset(din, 0, k2 * sizeof(float));
    memset(qin, 0, FC_OS_HI * k * sizeof(float));
    int any_hi = 0;
    for (int vi = 0; vi < FC_VOICES; vi++) {
      FcVoice* v = &c->v[vi];
      if (v->stage == FC_IDLE) continue;
      if (v->stage == FC_STEAL && v->sg <= 0.0f && v->next_note >= 0) start_voice(c, v, v->next_note, v->next_vel);
      voice_source(c, v);
      if (v->hi) render_voice(c, v, &c->rt[1], qin, FC_OS_HI * k);
      else render_voice(c, v, &c->rt[0], din, k2);
      any_hi |= v->hi;
    }
    for (uint32_t j = 0; j < k2; j++) in[j] = c->dbuf[j];   /* FC_HB4_DELAY behind */
    if (any_hi || c->q_tail > 0) {
      /* 4x -> 2x, as late as the 2x voices: output j is centred on 4x sample 2j - mid4 (a voice's 4x sample 2q
       * comes half a 2x sample before its 2x sample q would, the one-sample-late Principal is half as late) */
      for (uint32_t j = 0; j < k2; j++) {
        const float* x = qin + 2 * j;
        float acc = c->hb4[mid4] * x[-mid4];
        for (int i = 1; i < FC_HB4_TAPS; i += 2) acc += c->hb4[i] * x[-i];   /* the other non-zero taps are odd */
        in[j] += acc;
      }
      memmove(c->qbuf, qin + FC_OS_HI * k - qhist, qhist * sizeof(float));
      if (any_hi) c->q_tail = qhist;                 /* 4x samples of silence until its history is silent */
      else c->q_tail = c->q_tail > (int)(FC_OS_HI * k) ? c->q_tail - (int)(FC_OS_HI * k) : 0;
    }
    memmove(c->dbuf, din + k2 - FC_HB4_DELAY, FC_HB4_DELAY * sizeof(float));
    for (uint32_t j = 0; j < k; j++) {
      const float* x = in + 2 * j + 1;                 /* newest input of this output sample */
      float acc = c->hb[(FC_HB_TAPS - 1) / 2] * x[-(FC_HB_TAPS - 1) / 2];
      for (int i = 0; i < FC_HB_TAPS; i += 2) acc += c->hb[i] * x[-i];   /* the other non-zero taps are even */
      /* soft clip, transparent below about -6 dBFS (TZ 6: a safe output at extreme settings) */
      const float a2 = acc * acc, a8 = a2 * a2 * a2 * a2;
      l[done + j] = acc / sqrtf(sqrtf(sqrtf(1.0f + a8)));
    }
    memmove(c->hbuf, in + k2 - hist, hist * sizeof(float));
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

static const FcVoice* newest(const FoldCore* c) {
  const FcVoice* best = NULL;
  for (int i = 0; i < FC_VOICES; i++) {
    const FcVoice* v = &c->v[i];
    if (v->stage == FC_PLAY && (!best || v->age > best->age)) best = v;
  }
  return best;
}

float fc_mod_phase(const FoldCore* c) {
  const FcVoice* v = newest(c);
  return v ? v->mphase : -1.0f;
}

float fc_gate_level(const FoldCore* c) {
  const FcVoice* v = newest(c);
  return v ? v->vac : -1.0f;
}

float fc_fg_pos(const FoldCore* c, int i) {
  const FcVoice* v = newest(c);
  if (!v || i < 0 || i > 1) return -1.0f;
  const FcFg* g = &v->fg[i];
  switch (g->seg) {
    case FC_SEG_RISE: return g->lvl < 0.999f ? g->lvl : 0.999f;
    case FC_SEG_HOLD: return 1.5f;
    case FC_SEG_FALL: return 3.0f - g->lvl;
    default: return 3.0f;
  }
}
