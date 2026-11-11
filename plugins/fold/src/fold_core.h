/* Eka Fold engine, without LV2: tested in WSL as a plain program (test/test_core.c).
 * Stage 0: 4 voices, a sine from a table, a plain ADSR per voice (replaced by FG1 and the lopass gate at stage 3).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef FOLD_CORE_H
#define FOLD_CORE_H

#include <stdint.h>

#define FC_VOICES 4
#define FC_SINE_N 2048            /* table size (power of two); one more entry for interpolation */
#define FC_STEAL_MS 2.0f          /* a stolen voice fades out this fast before the new note starts */
#define FC_SILENT 1e-4f           /* -80 dB: a voice in release below this is free */
#define FC_GAIN 0.25f             /* per voice: 4 voices at full velocity stay at 0 dBFS */

typedef struct {
  float pitch;                    /* semitones */
  float fine;                     /* cents */
  float attack_ms, decay_ms;
  float sustain;                  /* 0..1 */
  float release_ms;
} FcParams;

enum { FC_IDLE, FC_ATTACK, FC_DECAY, FC_SUSTAIN, FC_RELEASE, FC_STEAL };

typedef struct {
  int stage;
  int note;
  float amp;                      /* from velocity */
  float phase;                    /* 0..FC_SINE_N */
  float env;
  uint32_t age;                   /* note-on order: lower = older */
  int held;                       /* the key is down */
  int pedal;                      /* released while the sustain pedal was down */
  float steal_dec;                /* FC_STEAL: env step per sample */
  int next_note, next_vel;        /* FC_STEAL: the note that starts when the fade ends */
} FcVoice;

typedef struct {
  float sr;
  FcVoice v[FC_VOICES];
  uint32_t clock;
  int sustain_pedal;
  float bend;                     /* semitones */
  float tune;                     /* semitones: pitch + fine */
  float att_inc, dec_coef, rel_coef, sus_coef, sus;
  float table[FC_SINE_N + 1];
} FoldCore;

void fc_init(FoldCore* c, float sr);
void fc_default_params(FcParams* p);
void fc_set_params(FoldCore* c, const FcParams* p);
void fc_note_on(FoldCore* c, int note, int vel);
void fc_note_off(FoldCore* c, int note);
void fc_set_sustain(FoldCore* c, int on);
void fc_set_bend(FoldCore* c, float semitones);
void fc_all_notes_off(FoldCore* c);    /* every voice to release (CC 123) */
void fc_all_sound_off(FoldCore* c);    /* every voice fades out in FC_STEAL_MS (CC 120) */
/* Renders n samples into l and r (overwrites them). */
void fc_process(FoldCore* c, float* l, float* r, uint32_t n);
/* MIDI note of voice i for the widget (the coming note while a voice is stolen), -1 = idle. */
int fc_voice_note(const FoldCore* c, int i);
int fc_active_voices(const FoldCore* c);
/* Flush denormals to zero in this thread (call at the start of run()). */
void fc_enable_ftz(void);

#endif
