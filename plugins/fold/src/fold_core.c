/* Eka Fold engine, stage 0. SPDX-License-Identifier: GPL-3.0-or-later */
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

void fc_default_params(FcParams* p) {
  p->pitch = 0.0f;
  p->fine = 0.0f;
  p->attack_ms = 5.0f;
  p->decay_ms = 300.0f;
  p->sustain = 0.7f;
  p->release_ms = 400.0f;
}

void fc_init(FoldCore* c, float sr) {
  memset(c, 0, sizeof *c);
  c->sr = sr > 1.0f ? sr : 48000.0f;
  for (int i = 0; i <= FC_SINE_N; i++)
    c->table[i] = (float)sin(2.0 * M_PI * (double)i / FC_SINE_N);
  for (int i = 0; i < FC_VOICES; i++) c->v[i].note = -1;
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
  c->sus = p->sustain < 0.0f ? 0.0f : p->sustain > 1.0f ? 1.0f : p->sustain;
}

static void start_voice(FoldCore* c, FcVoice* v, int note, int vel) {
  v->note = note;
  v->amp = FC_GAIN * (float)vel / 127.0f;
  v->phase = 0.0f;
  v->env = 0.0f;
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

/* Table steps per sample for a note. */
static float phase_inc(const FoldCore* c, int note) {
  const float semis = (float)note - 69.0f + c->tune + c->bend;
  const float inc = 440.0f * exp2f(semis * (1.0f / 12.0f)) / c->sr * (float)FC_SINE_N;
  return inc < FC_SINE_N * 0.5f ? inc : FC_SINE_N * 0.5f;   /* not above Nyquist */
}

void fc_process(FoldCore* c, float* l, float* r, uint32_t n) {
  memset(l, 0, n * sizeof(float));
  for (int vi = 0; vi < FC_VOICES; vi++) {
    FcVoice* v = &c->v[vi];
    if (v->stage == FC_IDLE) continue;
    float inc = phase_inc(c, v->note);              /* once per block: bend and tune come between blocks */
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
              inc = phase_inc(c, v->note);
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
      const int i = (int)v->phase;
      const float f = v->phase - (float)i;
      const float y = c->table[i] + (c->table[i + 1] - c->table[i]) * f;
      l[s] += y * v->env * v->amp;
      v->phase += inc;
      if (v->phase >= (float)FC_SINE_N) v->phase -= (float)FC_SINE_N;
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
