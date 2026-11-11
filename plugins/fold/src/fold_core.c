/* Eka Fold engine, stage 1. SPDX-License-Identifier: GPL-3.0-or-later */
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
}

void fc_init(FoldCore* c, float sr) {
  memset(c, 0, sizeof *c);
  c->sr = sr > 1.0f ? sr : 48000.0f;
  for (int i = 0; i <= FC_SINE_N; i++)
    c->table[i] = (float)sin(2.0 * M_PI * (double)i / FC_SINE_N);
  for (int i = 0; i < FC_VOICES; i++) c->v[i].note = -1;
  c->smooth = 1.0f - expf(-1.0f / (FC_SMOOTH_MS * 0.001f * c->sr));
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

void fc_process(FoldCore* c, float* l, float* r, uint32_t n) {
  memset(l, 0, n * sizeof(float));
  const float k = c->smooth;
  for (int vi = 0; vi < FC_VOICES; vi++) {
    FcVoice* v = &c->v[vi];
    if (v->stage == FC_IDLE) continue;
    float inc = note_inc(c, v->note);                /* once per block: bend and tune come between blocks */
    float dm = mod_inc(c, inc);
    for (uint32_t s = 0; s < n; s++) {
      switch (v->stage) {
        case FC_ATTACK:
          v->env += c->att_inc;
          if (v->env >= 1.0f) { v->env = 1.0f; v->stage = FC_DECAY; }
          break;
        case FC_DECAY:
          v->env = c->sus + (v->env - c->sus) * c->dec_coef;
          if (v->env - c->sus < FC_SILENT) {
            v->env = c->sus;
            v->stage = FC_SUSTAIN;
          }
          break;
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
      l[s] += out * ((1.0f - am) + am * m) * v->env * v->amp;
    }
    if (v->stage == FC_IDLE) v->note = -1;
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
