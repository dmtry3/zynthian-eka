/* Eka Fold engine, without LV2: tested in WSL as a plain program (test/test_core.c).
 * Stage 1: 4 voices; each voice has a Principal oscillator (sine; more sources at stage 4) and a Mod OSC
 * (triangle, sine, saw, square; Free / Keytrack / Ratio) that modulates it through FM, AM and Sync with one
 * Mod Index. A plain ADSR per voice (replaced by FG1 and the lopass gate at stage 3).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef FOLD_CORE_H
#define FOLD_CORE_H

#include <stdint.h>

#define FC_VOICES 4
#define FC_SINE_N 2048            /* table size (power of two); one more entry for interpolation */
#define FC_STEAL_MS 2.0f          /* a stolen voice fades out this fast before the new note starts */
#define FC_SILENT 1e-4f           /* -80 dB: a voice in release below this is free */
#define FC_GAIN 0.25f             /* per voice: 4 voices at full velocity stay at 0 dBFS */
#define FC_SMOOTH_MS 5.0f         /* Mod Index and routing switches glide over this, not to click */

/* FM is linear and through zero: Mod Index 100 % swings the Principal frequency by FC_FM_DEPTH times itself.
 * Chosen by the aliasing test (test/alias_report.c, 2026-11-11): phase modulation aliased strongly with a saw or
 * square Mod OSC (a phase step is a step in the sound), linear FM stays clean to C6 at the plugin rate. */
#define FC_FM_DEPTH 4.0f

/* Mod OSC shapes and modes: the order of the scale points in lv2/eka_fold.ttl and of the widget's lists. */
enum { FC_MOD_TRI, FC_MOD_SINE, FC_MOD_SAW, FC_MOD_SQUARE, FC_NMOD_SHAPES };
enum { FC_MODE_FREE, FC_MODE_KEY, FC_MODE_RATIO, FC_NMODES };

/* Mod Freq knob, 0..FC_FREQ_MAX units, read by the mode:
 * Free: Hz, FC_FREE_MIN..FC_FREE_MAX, logarithmic; Keytrack: semitones from the note, -48..+48;
 * Ratio: one of FC_NRATIOS ratios to the Principal frequency. The widget has the same formulas. */
#define FC_FREQ_MAX 100.0f
#define FC_FREE_MIN 0.01f
#define FC_FREE_MAX 10000.0f
#define FC_KEY_RANGE 48.0f
#define FC_NRATIOS 25
extern const float fc_ratios[FC_NRATIOS];
float fc_free_hz(float units);
float fc_key_semis(float units);
int fc_ratio_index(float units);

typedef struct {
  float pitch;                    /* semitones */
  float fine;                     /* cents */
  float attack_ms, decay_ms;
  float sustain;                  /* 0..1 */
  float release_ms;
  float mod_freq;                 /* units, see above */
  float mod_index;                /* 0..1 */
  int mod_shape, mod_mode;
  int route_fm, route_am, route_sync;
} FcParams;

enum { FC_IDLE, FC_ATTACK, FC_DECAY, FC_SUSTAIN, FC_RELEASE, FC_STEAL };

typedef struct {
  int stage;
  int note;
  float amp;                      /* from velocity */
  float phase;                    /* Principal, cycles 0..1 */
  float mphase;                   /* Mod OSC, cycles 0..1 */
  float env;
  float idx, gfm, gam;            /* Mod Index and the FM / AM switches, smoothed */
  float y_prev;                   /* Principal, one sample late: the sync step is corrected on both sides */
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
  float smooth;                   /* one-pole coefficient for FC_SMOOTH_MS */
  int mod_shape, mod_mode;
  float mod_hz, mod_semis, mod_ratio;   /* Mod Freq read for each mode */
  float idx_t, fm_t, am_t;        /* targets of the smoothed values */
  int sync;
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
/* Mod OSC phase 0..1 of the newest sounding voice, for the widget; -1 = none. */
float fc_mod_phase(const FoldCore* c);
/* Flush denormals to zero in this thread (call at the start of run()). */
void fc_enable_ftz(void);

#endif
