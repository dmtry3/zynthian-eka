/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Eka Pulse DSP core: polyrhythmic multi-effect shaper.
 * No LV2 dependencies here so it can be unit-tested and offline-rendered in WSL.
 * Nothing allocates, locks, or does I/O after px_init(). */
#ifndef PULSE_FX_H
#define PULSE_FX_H

#include <stdint.h>

#define PX_SLOTS 4
#define PX_MAX_STEPS 16
#define PX_NSHAPES 6
#define PX_NRATES 9
#define PX_NTRIGS 4
#define PX_NPATTERNS 25 /* 0 = Custom, 1..24 = built-in patterns */

enum {
  PX_SLOT_TIME = 0,
  PX_SLOT_DRIVE = 1,
  PX_SLOT_FILTER = 2,
  PX_SLOT_GATE = 3
};

enum {
  PX_SHP_HOLD = 0,
  PX_SHP_DECAY = 1,
  PX_SHP_RAMP = 2,
  PX_SHP_PULSE = 3,
  PX_SHP_2X = 4,
  PX_SHP_3X = 5
};

enum {
  PX_TRIG_SYNC = 0,
  PX_TRIG_FREE = 1,
  PX_TRIG_AUDIO = 2,
  PX_TRIG_MIDI = 3
};

enum {
  PX_GATE_GATE = 0,
  PX_GATE_PAN = 1,
  PX_GATE_WIDTH = 2,
  PX_GATE_GATEPAN = 3
};

/* Beats per single step for step_rate 0..7 (8 = Free Hz). */
extern const float px_rate_beats[PX_NRATES - 1];

typedef struct {
  int   on;
  int   mode;
  float param1;                  /* 0..100 */
  float param2;                  /* 0..100 */
  float depth;                   /* -1..+1 */
  int   pattern;                 /* 0..24 */
  int   step_rate;               /* 0..8 (8 = Free) */
  int   steps;                   /* 1..16 */
  float warp;                    /* -1..+1 */
  int   trig;                    /* PX_TRIG_* */
  float threshold_db;            /* -60..0 */
  float env_depth;               /* -1..+1 */
  float smooth;                  /* -1..+1 (<0 = Punch in Gate, >=0 = Smooth) */
  float levels[PX_MAX_STEPS];    /* 0..1 */
  int   shapes[PX_MAX_STEPS];    /* 0..5 */
} PxSlotParams;

typedef struct {
  PxSlotParams slot[PX_SLOTS];
  float mix;                     /* 0..1 */
  float low_split_hz;            /* 0 = Off, 60..500 */
  int   low_duck;                /* 0..1 */
  float free_hz;                 /* 0.25..32.0 steps/sec when step_rate == Free */
  float output_db;               /* -24..+6 */
  int   live_pad;                /* 0..8 */
} PxParams;

typedef struct {
  double free_pos;               /* step position in Free/stopped-transport mode (0..steps) */
  float  phase;                  /* 0..1 normalized loop phase for UI */
  float  curve_raw;              /* 0..1 unsmoothed curve */
  float  curve_smooth;           /* 0..1 smoothed curve */
  float  prev_raw;               /* previous sample raw curve for Punch edge detect */
  float  punch_env;              /* 0..1 transient punch envelope */
  float  bypass_gain;            /* 0..1 click-free slot enable crossfade */
  int    last_step;              /* last integer step index (for ping-pong) */
} PxSlotState;

typedef struct {
  double      sr;
  float       bpm;
  int         bpb;               /* beats per bar (default 4) */
  int         rolling;           /* 1 while host transport is running, 0 when stopped */
  double      transport_beats;   /* current song position in beats while rolling */
  double      origin_beats;      /* bar downbeat where transport started */
  PxSlotState st[PX_SLOTS];
  float       in_peak;
  float       out_peak;
  float       lim_env;
  float       lim_release;
  float       lim_gr;            /* 0..1 gain reduction indicator (1 - g) */
  float       mix_cur;
  float       out_gain_cur;
} PulseFx;

float px_db(float db);
float px_eval_shape(int shape, float u, float warp);
void  px_apply_pattern(int pattern_id, float levels_pct[PX_MAX_STEPS], int shapes[PX_MAX_STEPS]);
void  px_default_params(PxParams* p);

int   px_init(PulseFx* x, double sr);
void  px_reset(PulseFx* x);
void  px_free(PulseFx* x);
void  px_set_transport(PulseFx* x, float bpm, int rolling, int has_pos, double beats, int bpb);
void  px_process(PulseFx* x, const PxParams* p,
                 const float* in_l, const float* in_r,
                 float* out_l, float* out_r, uint32_t nframes);

#endif
