/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Eka Press: single-band live compressor core (pure C11, RT-safe). */
#ifndef EKA_PRESS_CORE_H
#define EKA_PRESS_CORE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PRESS_MODE_CLEAN = 0,
    PRESS_MODE_OPTO  = 1,
    PRESS_MODE_PUNCH = 2,
    PRESS_MODE_GLUE  = 3,
    PRESS_MODE_COUNT = 4
} PressMode;

typedef struct {
    float threshold_db;   /* -60.0 .. 0.0 dB */
    float ratio;          /* 1.0 .. 20.0 (>= 19.9 => inf:1 limiter) */
    float attack_ms;      /* 0.1 .. 100.0 ms */
    float release_ms;     /* 10.0 .. 1500.0 ms */
    float knee_db;        /* 0.0 .. 18.0 dB */
    int   mode;           /* PressMode 0..3 */

    float sc_hpf_hz;      /* 20.0 .. 500.0 Hz (<= 20.5 => off) */
    float stereo_link;    /* 0.0 .. 100.0 % */
    int   auto_release;   /* 0 / 1 */
    int   sc_listen;      /* 0 / 1 */

    float drive_pct;      /* 0.0 .. 100.0 % */
    float makeup_db;      /* -12.0 .. +24.0 dB */
    int   auto_makeup;    /* 0 / 1 */
    float mix_pct;        /* 0.0 .. 100.0 % */
    int   soft_clip;      /* 0 / 1 */
    float output_db;      /* -24.0 .. +12.0 dB */
} PressParams;

typedef struct {
    float in_peak_db;     /* -60.0 .. +12.0 dBFS */
    float out_peak_db;    /* -60.0 .. +12.0 dBFS */
    float sc_peak_db;     /* -60.0 .. +12.0 dBFS */
    float gr_db;          /* 0.0 .. 36.0 dB (positive reduction amount) */
} PressMeters;

typedef struct {
    float b0, b1, b2, a1, a2;
    float z1, z2;
} PressBiquad;

typedef struct {
    float sample_rate;
    float inv_sr;

    /* Sidechain HPF biquads (L/R) */
    PressBiquad hpf_l;
    PressBiquad hpf_r;
    float last_hpf_hz;

    /* Detector RMS state (squared amplitude) */
    float rms_l;
    float rms_r;

    /* Gain reduction envelope state (dB, >= 0.0) */
    float env_l;
    float env_r;

    /* Slow release memory for Opto / Auto Release (dB, >= 0.0) */
    float slow_l;
    float slow_r;

    /* DC blocker after asymmetric Drive saturation */
    float dc_x_l, dc_y_l;
    float dc_x_r, dc_y_r;

    /* Smoothed control values */
    float sm_makeup_lin;
    float sm_output_lin;
    float sm_mix;
    float sm_drive;
    float sm_sc_listen;
    float sm_hpf_mix;
    int   sm_init;

    /* Cached block meters */
    PressMeters meters;
} PressCore;

void press_params_default(PressParams *p);
void press_core_init(PressCore *c, float sample_rate);
void press_core_reset(PressCore *c);

/* Compute static gain reduction (positive dB) for an input level in dB. */
float press_compute_gr_db(float level_db, float threshold_db, float ratio, float knee_db);

/* Process a stereo block in-place or out-of-place (RT-safe). */
void press_core_process(PressCore *c, const PressParams *p,
                        const float *in_l, const float *in_r,
                        float *out_l, float *out_r,
                        uint32_t nframes);

#ifdef __cplusplus
}
#endif

#endif /* EKA_PRESS_CORE_H */
