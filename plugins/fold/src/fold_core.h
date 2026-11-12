/* Eka Fold engine, without LV2: tested in WSL as a plain program (test/test_core.c).
 * Stage 4: 4 voices; each voice has a Principal oscillator (Source: Sine, or one of the engines VA, FM2, Wave
 * of src/plaits/, with Harmonics, Color, Morph) and a Mod OSC
 * (triangle, sine, saw, square; Free / Keytrack / Ratio) that modulates it through FM, AM, Sync and Timbre with
 * one Mod Index; then the Timbre section: three shapers (folder, saw, M) blended by Order, Timbre = depth,
 * Symmetry = offset before them, Drive = soft saturation after them; a DC blocker; then the lopass gate:
 * a vactrol (fast to open, slow to close, slower the darker it gets) driven by function generator 1 and the
 * velocity, which sets a 2-pole low-pass (LP), the level (VCA) or both (Combo). Function generator 2 moves
 * Timbre (a mod-matrix cell from stage 4). A soft clip on the output.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef FOLD_CORE_H
#define FOLD_CORE_H

#include <stdint.h>

#include "plaits/engines.h"

#define FC_VOICES 4
#define FC_SINE_N 2048            /* table size (power of two); one more entry for interpolation */
#define FC_STEAL_MS 2.0f          /* a stolen voice fades out this fast before the new note starts */
#define FC_GAIN 0.25f             /* per voice: 4 voices at full velocity stay at 0 dBFS */
#define FC_SMOOTH_MS 5.0f         /* Mod Index and routing switches glide over this, not to click */

/* FM is linear and through zero: Mod Index 100 % swings the Principal frequency by FC_FM_DEPTH times itself.
 * Chosen by the aliasing test (test/alias_report.c, 2026-11-11): phase modulation aliased strongly with a saw or
 * square Mod OSC (a phase step is a step in the sound), linear FM stays clean to C6 at the plugin rate. */
#define FC_FM_DEPTH 4.0f

/* Timbre section. Folder: gain 1..FC_FOLD_GAIN into a triangle folder (Timbre 0 with Symmetry 0 = no change).
 * Saw and M: the sum of Chebyshev polynomials T_n / n (all n, or odd n), which turns a sine into exactly the
 * harmonics of a saw (square) up to n = N; N per voice from its note, below 0.45 of the rate, at most
 * FC_CHEB_MAX. Timbre crossfades the input into them. The Mod OSC adds up to FC_TIMBRE_MOD x Index to Timbre.
 * Drive: tanh saturation, k = 1 + FC_DRIVE_K x Drive, crossfaded in. */
#define FC_FOLD_GAIN 8.0f
#define FC_CHEB_MAX 32
#define FC_TIMBRE_MOD 1.0f
#define FC_DRIVE_K 4.0f
#define FC_DC_HZ 5.0f             /* DC blocker per voice (Symmetry, AM and the shapers make DC) */

/* Anti-aliasing: the voices run at 2x the plugin rate, with first-order ADAA on the folder, and a halfband
 * decimator on the mix. Chosen on 2026-11-11 from four modes (1x, ADAA, 2x, 2x + ADAA; test/alias_report.c,
 * fold_bench on the Pi 5): folder at Timbre 70 % on C5: -37 / -49 / -50 / -70 dB of aliasing; 4 voices with
 * everything on: 1.4 % of one Pi 5 core (4.3 % with the Saw shaper).
 * 2026-11-12 (stage 4a): a voice whose Source is an engine (VA, FM2, Wave: steps and corners, which the folder
 * and the shapers turn into aliases at -35..-45 dB on C5) runs at 4x instead: its own halfband takes it down to
 * 2x (FC_HB4_TAPS, -80 dB above 3/8 of the 4x rate) and joins the 2x mix, which is delayed by as much
 * (FC_HB4_DELAY). The rate is chosen at the note-on and kept to its end: a voice cannot move between the two
 * decimators while it sounds (the cut in one does not add up with the start in the other: a click), so a change
 * of Source on a held note crossfades at the note's rate, the next note takes the new one. */
#define FC_OS 2
#define FC_OS_HI 4
#define FC_CHUNK 64               /* output samples rendered at a time */
#define FC_HB_TAPS 63             /* halfband decimator 2x -> 1x */
#define FC_HB4_TAPS 25            /* halfband 4x -> 2x: 4 x FC_HB4_DELAY + 1 taps (the two ends are 0) */
#define FC_HB4_DELAY 6            /* its delay in 2x samples */

/* Function generators (after the Buchla 281): Rise and Fall are linear segments, FC_FG_MIN_MS..FC_FG_MAX_MS.
 * Once: a note rises and falls, the key does not matter. Gate: rises, holds while the key (or the sustain
 * pedal) is down, falls. Cycle: rises and falls again while the key is down (an LFO), then ends its fall. */
enum { FC_FG_ONCE, FC_FG_GATE, FC_FG_CYCLE, FC_NFG_MODES };
enum { FC_SEG_IDLE, FC_SEG_RISE, FC_SEG_HOLD, FC_SEG_FALL };
#define FC_FG_MIN_MS 1.0f
#define FC_FG_MAX_MS 10000.0f

/* Lopass gate. The vactrol follows FG1 x velocity: it opens with FC_VAC_RISE_MS and closes with Decay
 * (FC_DECAY_MIN_MS..FC_DECAY_MAX_MS, logarithmic), up to 1 + FC_VAC_SLOW times slower as it goes dark (the
 * "acoustic" tail of a Buchla lopass gate). Its state g (0..1) sets the cutoff FC_LPG_FMIN..FC_LPG_FMAX
 * (exponential) in LP and Combo, and the level (x g) in VCA and Combo. Velocity opens it to
 * FC_VEL_FLOOR + (1 - FC_VEL_FLOOR) x velocity / 127. The voice ends when FG1 has ended and g is below
 * FC_GATE_OFF; it fades out over the last FC_GATE_OFF of g. Resonance 0 is a Butterworth low-pass; VCA bypasses
 * the filter (no top loss). */
enum { FC_GATE_VCA, FC_GATE_LP, FC_GATE_COMBO, FC_NGATE_MODES };
#define FC_VAC_RISE_MS 2.0f
#define FC_VAC_SLOW 2.0f
#define FC_DECAY_MIN_MS 10.0f
#define FC_DECAY_MAX_MS 2000.0f
#define FC_LPG_FMIN 20.0f
#define FC_LPG_FMAX 20000.0f
#define FC_VEL_FLOOR 0.4f
#define FC_GATE_OFF 1e-3f         /* -60 dB */
/* The band state of the low-pass saturates softly at FC_RES_SAT (as in an analog filter): at full Resonance a
 * voice peaks at 0.4 instead of 3.3 (the output clipped, 2026-11-12, found on the device); the small-signal
 * lift stays about +20 dB, Resonance 0 is unchanged within 0.5 dB. */
#define FC_RES_SAT 2.0f
#define FC_COEF_EVERY 32          /* filter coefficients are updated every 32 samples (of the voice's rate) */
float fc_decay_ms(float decay);   /* Decay 0..1 -> ms */

/* Principal sources: the order of the scale points of Source in lv2/eka_fold.ttl and of the widget's tiles.
 * The engines are FE_* = source - 1. A change of Source crossfades a sounding voice over FC_SRC_FADE_MS. */
enum { FC_SRC_SINE, FC_SRC_VA, FC_SRC_FM2, FC_SRC_WAVE, FC_NSOURCES };
#define FC_SRC_FADE_MS 10.0f

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
  int source;                     /* FC_SRC_* */
  float harmonics, color, morph;  /* 0..1: the engine's knobs (Sine has none) */
  float fg_rise_ms[2], fg_fall_ms[2];
  int fg_mode[2];
  int gate_mode;
  float gate_decay, gate_res, level;   /* 0..1 */
  float fg2_timbre;               /* -1..1: FG2 -> Timbre */
  float mod_freq;                 /* units, see above */
  float mod_index;                /* 0..1 */
  int mod_shape, mod_mode;
  int route_fm, route_am, route_sync, route_timbre;
  float timbre, order, drive;     /* 0..1 */
  float symmetry;                 /* -1..1 */
} FcParams;

enum { FC_IDLE, FC_PLAY, FC_STEAL };

typedef struct {
  float lvl;                      /* 0..1 */
  int seg;                        /* FC_SEG_* */
} FcFg;

typedef struct {
  int stage;
  int note;
  float vel;                      /* vactrol drive from the velocity, FC_VEL_FLOOR..1 */
  FcFg fg[2];
  float vac;                      /* vactrol state g, 0..1 */
  float wv, wl, lvl, res;         /* gate mode (VCA, LP shares), Level, Resonance: smoothed */
  float ic1, ic2;                 /* low-pass state (TPT state-variable filter) */
  float a1, a2, a3;               /* its coefficients, every FC_COEF_EVERY samples */
  int coef_count;
  int hi;                         /* the note runs at FC_OS_HI (an engine at its note-on), else at FC_OS */
  float sg;                       /* steal fade, 1 = none */
  float phase;                    /* Principal, cycles 0..1 (Sine) */
  int src, src_prev;              /* Source, and the one it fades from while xf < 1 */
  float xf;                       /* crossfade 0..1 */
  FeState fe[FE_NENGINES];        /* the engines' states (each one keeps its own phase) */
  float mphase;                   /* Mod OSC, cycles 0..1 */
  float idx, gfm, gam, gtim;      /* Mod Index and the FM / AM / Timbre switches, smoothed */
  float tim, ord, sym, drv;       /* Timbre knobs, smoothed */
  float au, aF;                   /* ADAA: last folder input and its antiderivative */
  float dcx, dcy;                 /* DC blocker */
  float y_prev;                   /* Principal, one sample late: the sync step is corrected on both sides */
  uint32_t age;                   /* note-on order: lower = older */
  int held;                       /* the key is down */
  int pedal;                      /* released while the sustain pedal was down */
  float steal_dec;                /* FC_STEAL: sg step per output sample */
  int next_note, next_vel;        /* FC_STEAL: the note that starts when the fade ends */
} FcVoice;

/* What depends on the voices' rate: one for FC_OS, one for FC_OS_HI. */
typedef struct {
  float sr;                       /* the plugin rate x FC_OS or FC_OS_HI */
  float step;                     /* plugin samples per voice sample: 1 / os */
  float smooth;                   /* one-pole coefficient for FC_SMOOTH_MS */
  float dc_r;                     /* DC blocker pole */
  float src_fade;                 /* Source crossfade step per sample */
  float fg_rise_inc[2], fg_fall_inc[2];   /* per sample */
  float vac_rise, vac_fall;       /* vactrol: one-pole steps per sample (fall before the slow-down) */
} FcRate;

typedef struct {
  float sr_out;                   /* the plugin rate */
  FcRate rt[2];                   /* rt[0]: FC_OS, rt[1]: FC_OS_HI */
  FcVoice v[FC_VOICES];
  uint32_t clock;
  int sustain_pedal;
  float bend;                     /* semitones */
  float tune;                     /* semitones: pitch + fine */
  int src_t;                      /* Source */
  float harm_t, color_t, morph_t; /* engine knobs */
  float fg_rise_ms[2], fg_fall_ms[2], decay_ms;
  int fg_mode[2];
  float wv_t, wl_t, lvl_t, res_t, fg2_tim;
  float fmax;                     /* the highest cutoff */
  int mod_shape, mod_mode;
  float mod_hz, mod_semis, mod_ratio;   /* Mod Freq read for each mode */
  float idx_t, fm_t, am_t, tim_route_t;   /* targets of the smoothed values */
  float tim_t, ord_t, sym_t, drv_t;
  float hb[FC_HB_TAPS];           /* halfband taps 2x -> 1x */
  float hb4[FC_HB4_TAPS];         /* halfband taps 4x -> 2x */
  float hbuf[FC_HB_TAPS - 1 + FC_OS * FC_CHUNK];        /* 2x mix: history, then the chunk */
  float dbuf[FC_HB4_DELAY + FC_OS * FC_CHUNK];          /* the 2x voices: FC_HB4_DELAY behind, then the chunk */
  float qbuf[FC_HB4_TAPS - 1 + FC_OS_HI * FC_CHUNK];    /* the 4x voices: history, then the chunk */
  int q_tail;                     /* 4x samples still to flush through hb4 (0: its history is silence) */
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
void fc_all_notes_off(FoldCore* c);    /* every key and the pedal up (CC 123) */
void fc_all_sound_off(FoldCore* c);    /* every voice fades out in FC_STEAL_MS (CC 120) */
/* Renders n samples into l and r (overwrites them). */
void fc_process(FoldCore* c, float* l, float* r, uint32_t n);
/* MIDI note of voice i for the widget (the coming note while a voice is stolen), -1 = idle. */
int fc_voice_note(const FoldCore* c, int i);
int fc_active_voices(const FoldCore* c);
/* For the widget, from the newest sounding voice (-1 = none): the Mod OSC phase 0..1; the vactrol state g 0..1;
 * where function generator i is: 0..1 rising (= its level), 1.5 holding, 2..3 falling (2 + 1 - level),
 * 3 ended. */
float fc_mod_phase(const FoldCore* c);
float fc_gate_level(const FoldCore* c);
float fc_fg_pos(const FoldCore* c, int i);
/* Flush denormals to zero in this thread (call at the start of run()). */
void fc_enable_ftz(void);

#endif
