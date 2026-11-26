/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Eka Modls: 6-engine musical-to-extreme "Uber Mod" modulation DSP core. */
#ifndef EKA_MODLS_CORE_H
#define EKA_MODLS_CORE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    MODLS_MODE_CHORUS  = 0,
    MODLS_MODE_TRIMOD  = 1,
    MODLS_MODE_FLANGER = 2,
    MODLS_MODE_PHASER  = 3,
    MODLS_MODE_BARBER  = 4,
    MODLS_MODE_UBER    = 5,
    MODLS_MODE_COUNT   = 6
};

enum {
    MODLS_SHAPE_SINE  = 0,
    MODLS_SHAPE_TRI   = 1,
    MODLS_SHAPE_EXP   = 2,
    MODLS_SHAPE_SH    = 3,
    MODLS_SHAPE_CHAOS = 4,
    MODLS_SHAPE_ENV   = 5,
    MODLS_SHAPE_COUNT = 6
};

#define MODLS_DIFF_STAGES    4
#define MODLS_PHASER_STAGES 12
#define MODLS_BARBER_VOICES  4
#define MODLS_BARBER_STAGES  4

typedef struct {
    /* Page 1: Engine */
    int   mode;          /* 0..5 */
    float depth_pct;     /* 0..100 */
    float feedback_pct;  /* -100..+100 */
    float mix_pct;       /* 0..100 */

    /* Page 2: LFO */
    int   shape;         /* 0..5 */
    float rate_hz;       /* 0.02..20.0 */
    int   sync;          /* 0 Free, 1 Sync */
    float stereo_deg;    /* 0..180 */

    /* Page 3: Uber */
    float delay_ms;      /* 0.3..120.0 */
    float spread_pct;    /* 0..100 */
    float diffuse_pct;   /* 0..100 */
    float overmod_pct;   /* 0..100 */

    /* Page 4: Tone */
    float low_cut_hz;    /* 20..600 */
    float high_cut_hz;   /* 1000..20000 */
    float drive_pct;     /* 0..100 */
    float env_mod_pct;   /* -100..+100 */

    /* Page 5: Live FX */
    float rate_fx_pct;   /* -100..+100 */
    float warp_fx_pct;   /* 0..100 */
    float comb_fx_pct;   /* -100..+100 */
    int   fx_zero;       /* 0 Off, 1 Zero */

    /* Page 6: Master */
    float width_pct;     /* 0..200 */
    int   lfo_freeze;    /* 0 Run, 1 Freeze */
    float output_db;     /* -18..+12 */
    int   bypass;        /* 0 Off, 1 On */

    /* Host tempo */
    float host_bpm;      /* 20..400 */
} ModlsParams;

typedef struct {
    float in_peak_db;
    float out_peak_db;
    float lfo_l;
    float lfo_r;
    float env_lvl;
    float eff_mod;
    float fb_energy;
    float host_bpm;
} ModlsMeters;

typedef struct {
    float z1;
} ModlsAP1;

typedef struct {
    float z1;
} ModlsOnePole;

typedef struct {
    float x1;
    float y1;
} ModlsDCBlock;

typedef struct {
    float   *buf;
    uint32_t len;
    uint32_t mask;
    uint32_t wpos;
} ModlsDelayLine;

typedef struct {
    double sample_rate;
    float  sr_f;

    /* Main stereo delay lines (~650 ms) */
    ModlsDelayLine main_l;
    ModlsDelayLine main_r;

    /* TZF dry reference delay lines (~65 ms) */
    ModlsDelayLine ref_l;
    ModlsDelayLine ref_r;

    /* 4-stage stereo all-pass diffuser delay lines */
    ModlsDelayLine diff_l[MODLS_DIFF_STAGES];
    ModlsDelayLine diff_r[MODLS_DIFF_STAGES];
    float          diff_base_l[MODLS_DIFF_STAGES];
    float          diff_base_r[MODLS_DIFF_STAGES];

    /* 12-stage stereo phaser all-pass cascade */
    ModlsAP1 phaser_l[MODLS_PHASER_STAGES];
    ModlsAP1 phaser_r[MODLS_PHASER_STAGES];

    /* 4-voice x 4-stage Barberpole Shepard all-pass banks */
    ModlsAP1 barber_l[MODLS_BARBER_VOICES][MODLS_BARBER_STAGES];
    ModlsAP1 barber_r[MODLS_BARBER_VOICES][MODLS_BARBER_STAGES];

    /* Feedback / tone loop filters and states */
    float        fb_l;
    float        fb_r;
    ModlsOnePole lp_fb_l;
    ModlsOnePole lp_fb_r;
    ModlsOnePole hp_fb_l;
    ModlsOnePole hp_fb_r;
    ModlsDCBlock dc_l;
    ModlsDCBlock dc_r;
    ModlsOnePole wet_lp_l;
    ModlsOnePole wet_lp_r;
    ModlsOnePole wet_hp_l;
    ModlsOnePole wet_hp_r;
    ModlsOnePole sub_lp_l;
    ModlsOnePole sub_lp_r;

    /* LFO, secondary fast LFO, Barberpole phase, S&H, Chaos */
    double   lfo_phase;
    double   fast_phase;
    double   barber_phase;
    uint32_t rng_state;
    float    sh_target_l;
    float    sh_target_r;
    float    sh_cur_l;
    float    sh_cur_r;
    double   prev_phase_l;
    double   prev_phase_r;
    float    chaos_x_l;
    float    chaos_y_l;
    float    chaos_x_r;
    float    chaos_y_r;

    /* Input envelope follower */
    float env_fast;
    float env_l;
    float env_r;

    /* Smoothed parameters */
    float sm_depth;
    float sm_feedback;
    float sm_mix;
    float sm_stereo;
    float sm_delay_ms;
    float sm_spread;
    float sm_diffuse;
    float sm_overmod;
    float sm_drive;
    float sm_env_mod;
    float sm_rate_fx;
    float sm_warp_fx;
    float sm_comb_fx;
    float sm_width;
    float sm_out_gain;
    float sm_bypass;
    float sm_freeze;

    /* Click-free mode transition ramp */
    int   last_mode;
    float mode_fade;

    /* Telemetry meters */
    float       meter_in;
    float       meter_out;
    float       meter_fb;
    ModlsMeters meters;
} ModlsCore;

void modls_params_default(ModlsParams *p);
int  modls_core_init(ModlsCore *c, double sample_rate);
void modls_core_reset(ModlsCore *c);
void modls_core_free(ModlsCore *c);
void modls_core_process(ModlsCore *c,
                        const ModlsParams *p,
                        const float *in_l,
                        const float *in_r,
                        float *out_l,
                        float *out_r,
                        uint32_t nframes);

#ifdef __cplusplus
}
#endif

#endif /* EKA_MODLS_CORE_H */
