/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Eka Space: psychoacoustic 3D stereo stage, 5 widener engines, binaural pan/tilt,
 * tempo-synced spatial modulation, bass mono anchor, early reflections, and phase guard. */
#ifndef EKA_SPACE_CORE_H
#define EKA_SPACE_CORE_H

#include <stdint.h>

#define SPACE_MAX_SPREAD_MS 40.0f
#define SPACE_MIN_SPREAD_MS 0.5f
#define SPACE_AP_STAGES     4
#define SPACE_ER_TAPS       4

typedef enum {
    SPACE_MODE_MS = 0,       /* Pure Mid/Side + Tilt + Side Air */
    SPACE_MODE_COMB,         /* Mono-safe complementary comb stereoizer */
    SPACE_MODE_HAAS,         /* Psychoacoustic Precedence ITD + head shadow */
    SPACE_MODE_DIMENSION,    /* BBD-style dual quadrature dimension expander */
    SPACE_MODE_VORTEX        /* 4-stage all-pass phase disperser + cross-feed */
} SpaceMode;

typedef enum {
    SPACE_MOD_PAN = 0,       /* Left-Right auto-pan */
    SPACE_MOD_BREATHE,       /* Stereo Width pulsing */
    SPACE_MOD_ORBIT,         /* 2D Pan + Depth circular orbit */
    SPACE_MOD_FIGURE8,       /* Lissajous Figure-8 (Pan f, Width/Depth 2f) */
    SPACE_MOD_DRIFT          /* Dual-sine organic spatial drift */
} SpaceModShape;

typedef enum {
    SPACE_AUD_STEREO = 0,
    SPACE_AUD_MONO,
    SPACE_AUD_MID,
    SPACE_AUD_SIDE
} SpaceAudition;

typedef struct {
    /* Page 1: Stage */
    float pan_pct;          /* -100..+100 % */
    float width_pct;        /* 0..300 % */
    float depth_pct;        /* 0..100 % */
    float tilt_pct;         /* -100..+100 % */

    /* Page 2: Widener */
    int   mode;             /* SpaceMode (0..4) */
    float widen_pct;        /* 0..100 % */
    float spread_ms;        /* 0.5..40.0 ms */
    float room_pct;         /* 0..100 % */

    /* Page 3: Motion */
    int   mod_shape;        /* SpaceModShape (0..4) */
    float mod_rate_hz;      /* 0.05..12.0 Hz */
    float mod_depth_pct;    /* 0..100 % */
    int   sync;             /* 0=Free, 1=Sync */

    /* Page 4: Bass & Air */
    float mono_low_hz;      /* 20..500 Hz */
    float side_hpf_hz;      /* 20..1000 Hz */
    float side_air_db;      /* -12..+12 dB */
    float env_width_pct;    /* -100..+100 % */

    /* Page 5: Live FX */
    float pan_fx_pct;       /* -100..+100 % */
    float width_fx_pct;     /* -100..+100 % */
    float spin_fx_pct;      /* 0..100 % */
    int   fx_zero;          /* 0/1 */

    /* Page 6: Master */
    int   audition;         /* SpaceAudition (0..3) */
    int   safe_phase;       /* 0/1 */
    float mix_pct;          /* 0..100 % */
    float output_db;        /* -18..+12 dB */
    int   bypass;           /* 0/1 */

    float host_bpm;         /* 20..400 BPM */
} SpaceParams;

typedef struct {
    double b0, b1, b2, a1, a2;
    double z1_l, z2_l;
    double z1_r, z2_r;
} SpaceBiquad;

typedef struct {
    float   *buf;
    uint32_t len;
    uint32_t idx;
} SpaceAllpass;

typedef struct {
    float in_peak_db;
    float out_peak_db;
    float mid_db;
    float side_db;
    float corr;             /* -1..+1 stereo correlation */
    float balance;          /* -1..+1 L/R energy balance */
    float eff_pan_pct;      /* -100..+100 % live modulated pan */
    float eff_width_pct;    /* 0..300 % live modulated width */
    float eff_depth_pct;    /* 0..100 % live modulated depth */
    float host_bpm;
} SpaceMeters;

typedef struct {
    double    sr;
    float    *buf_l;
    float    *buf_r;
    float    *buf_m;
    uint32_t  buf_size;
    uint32_t  buf_mask;
    uint32_t  write_pos;

    SpaceAllpass ap_l[SPACE_AP_STAGES];
    SpaceAllpass ap_r[SPACE_AP_STAGES];

    SpaceBiquad side_hpf;
    SpaceBiquad side_air;
    SpaceBiquad bass_lpf;   /* 1st/2nd order low-band extraction on Side */
    SpaceBiquad tilt_eq_l;
    SpaceBiquad tilt_eq_r;
    SpaceBiquad depth_lpf;
    SpaceBiquad er_lpf;

    double haas_lpf_z_l;
    double haas_lpf_z_r;

    double smooth_pan;
    double smooth_width;
    double smooth_depth;
    double smooth_tilt;
    double smooth_widen;
    double smooth_spread_ms;
    double smooth_room;
    double smooth_mix;
    double smooth_out_gain;
    double smooth_bypass;

    double lfo_phase;
    double drift_phase;
    double dim_phase;
    double spin_phase;

    double env_fast;
    double env_slow;
    double safe_gain;

    double corr_ll;
    double corr_rr;
    double corr_lr;

    int    fx_zero_mask;
    double fx_latch_pan;
    double fx_latch_width;
    double fx_latch_spin;

    double peak_rel;
    double in_pk_lin;
    double out_pk_lin;
    double mid_pk_lin;
    double side_pk_lin;

    SpaceMeters meters;
} SpaceCore;

void   space_params_default(SpaceParams *p);
int    space_core_init(SpaceCore *c, double sr);
void   space_core_free(SpaceCore *c);
void   space_core_reset(SpaceCore *c);
double space_quantize_rate_hz(double raw_hz, double bpm);
void   space_core_process(SpaceCore *c, const SpaceParams *p,
                          const float *in_l, const float *in_r,
                          float *out_l, float *out_r,
                          uint32_t nframes);

#endif /* EKA_SPACE_CORE_H */
