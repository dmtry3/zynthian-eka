/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Eka Prism: Dual-Chain Morphing & Modulated Multi-Model Filter DSP core. */
#ifndef EKA_PRISM_CORE_H
#define EKA_PRISM_CORE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    PRISM_MODEL_SVF     = 0,
    PRISM_MODEL_LADDER  = 1,
    PRISM_MODEL_SCREAM  = 2,
    PRISM_MODEL_COMB    = 3,
    PRISM_MODEL_FORMANT = 4,
    PRISM_MODEL_PHASER  = 5,
    PRISM_MODEL_COUNT   = 6
};

enum {
    PRISM_ROUTE_SERIAL   = 0,
    PRISM_ROUTE_PARALLEL = 1,
    PRISM_ROUTE_STEREO   = 2,
    PRISM_ROUTE_COUNT    = 3
};

enum {
    PRISM_LINK_FREE   = 0,
    PRISM_LINK_OFFSET = 1,
    PRISM_LINK_MIRROR = 2,
    PRISM_LINK_COUNT  = 3
};

enum {
    PRISM_LFO_SINE   = 0,
    PRISM_LFO_TRI    = 1,
    PRISM_LFO_SAW    = 2,
    PRISM_LFO_SQUARE = 3,
    PRISM_LFO_SH     = 4,
    PRISM_LFO_CHAOS  = 5,
    PRISM_LFO_STEP8  = 6,
    PRISM_LFO_COUNT  = 7
};

#define PRISM_PHASER_STAGES 6
#define PRISM_FORMANT_BANDS 3

typedef struct {
    /* Page 1: Filter A */
    int   model_a;        /* 0..5 */
    float cutoff_a_hz;    /* 20..18000 */
    float res_a_pct;      /* 0..100 */
    float morph_a_pct;    /* 0..100 */

    /* Page 2: Filter B */
    int   model_b;        /* 0..5 */
    float cutoff_b_hz;    /* 20..18000 */
    float res_b_pct;      /* 0..100 */
    float morph_b_pct;    /* 0..100 */

    /* Page 3: Routing */
    int   routing;        /* 0 Serial, 1 Parallel, 2 Stereo */
    int   link_mode;      /* 0 Free, 1 Offset, 2 Mirror */
    float ab_balance_pct; /* -100..+100 */
    int   invert_b;       /* 0 Norm, 1 Inv */

    /* Page 4: LFO */
    int   lfo_shape;      /* 0..6 */
    float lfo_rate_hz;    /* 0.05..40.0 */
    int   lfo_sync;       /* 0 Free, 1 Sync */
    float lfo_phase_deg;  /* 0..180 */

    /* Page 5: Mod Depth */
    float lfo_mod_pct;    /* -100..+100 */
    float morph_mod_pct;  /* -100..+100 */
    float env_mod_pct;    /* -100..+100 */
    float env_speed_pct;  /* 0..100 */

    /* Page 6: Master & FX */
    float drive_pct;      /* 0..100 */
    float mix_pct;        /* 0..100 */
    float output_db;      /* -18..+12 */
    int   bypass;         /* 0 Off, 1 On */

    /* Host tempo & MIDI note tracking */
    float host_bpm;       /* 20..400 */
    int   midi_note;      /* -1 if none, else 0..127 */
} PrismParams;

typedef struct {
    float in_peak_db;
    float out_peak_db;
    float eff_cut_a;      /* effective modulated cutoff A in Hz */
    float eff_cut_b;      /* effective modulated cutoff B in Hz */
    float eff_morph_a;    /* effective modulated morph A 0..100 */
    float eff_morph_b;    /* effective modulated morph B 0..100 */
    float lfo_a;          /* -1..+1 */
    float lfo_b;          /* -1..+1 */
    float env_lvl;        /* 0..1 */
    float host_bpm;
} PrismMeters;

typedef struct {
    /* Cytomic SVF state */
    float ic1eq;
    float ic2eq;

    /* 4-pole Ladder state */
    float lad_s[4];

    /* MS-20 / Sallen-Key Screamer state */
    float sk_s1;
    float sk_s2;

    /* Comb delay line */
    float   *comb_buf;
    uint32_t comb_len;
    uint32_t comb_mask;
    uint32_t comb_wpos;
    float    comb_lp;

    /* Formant 3 parallel resonator bands */
    float fm_ic1[PRISM_FORMANT_BANDS];
    float fm_ic2[PRISM_FORMANT_BANDS];

    /* 6-stage Phaser allpass cascade */
    float ph_z[PRISM_PHASER_STAGES];
    float ph_fb;
} PrismFilterChan;

typedef struct {
    float x1;
    float y1;
} PrismDCBlock;

typedef struct {
    double sample_rate;
    float  sr_f;

    /* Two independent stereo filter chains: chain_a[0..1] and chain_b[0..1] */
    PrismFilterChan chain_a[2];
    PrismFilterChan chain_b[2];

    PrismDCBlock dc_l;
    PrismDCBlock dc_r;

    /* LFO state */
    double   lfo_phase;
    double   prev_ph_a;
    double   prev_ph_b;
    uint32_t rng_state;
    float    sh_target_a;
    float    sh_target_b;
    float    sh_cur_a;
    float    sh_cur_b;
    float    chaos_x_a;
    float    chaos_y_a;
    float    chaos_x_b;
    float    chaos_y_b;

    /* Input Envelope Follower */
    float env_lvl;
    float env_sc_hp;

    /* Smoothed parameters */
    float sm_cut_a_oct;
    float sm_cut_b_oct;
    float sm_res_a;
    float sm_res_b;
    float sm_morph_a;
    float sm_morph_b;
    float sm_ab_bal;
    float sm_lfo_mod;
    float sm_morph_mod;
    float sm_env_mod;
    float sm_drive;
    float sm_mix;
    float sm_out_gain;
    float sm_bypass;

    /* Telemetry meters */
    float       meter_in;
    float       meter_out;
    PrismMeters meters;
} PrismCore;

void prism_params_default(PrismParams *p);
int  prism_core_init(PrismCore *c, double sample_rate);
void prism_core_reset(PrismCore *c);
void prism_core_free(PrismCore *c);
void prism_core_process(PrismCore *c,
                        const PrismParams *p,
                        const float *in_l,
                        const float *in_r,
                        float *out_l,
                        float *out_r,
                        uint32_t nframes);

#ifdef __cplusplus
}
#endif

#endif /* EKA_PRISM_CORE_H */
