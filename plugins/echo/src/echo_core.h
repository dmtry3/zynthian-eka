/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Eka Echo: stereo / ping-pong / diffused delay with BPM sync, ducking, freeze, trails, bypass. */
#ifndef EKA_ECHO_CORE_H
#define EKA_ECHO_CORE_H

#include <stdint.h>

#define ECHO_MAX_MS      2500.0f
#define ECHO_MIN_MS      5.0f
#define ECHO_AP_STAGES   4

typedef enum {
    ECHO_MODE_CLEAN = 0,
    ECHO_MODE_TAPE,
    ECHO_MODE_BBD,
    ECHO_MODE_DIFF
} EchoMode;

typedef enum {
    ECHO_ROUTE_STEREO = 0,
    ECHO_ROUTE_PINGPONG,
    ECHO_ROUTE_CROSS
} EchoRouting;

typedef struct {
    float time_l_ms;
    float time_r_ms;
    float feedback_pct;   /* 0..110 % */
    float mix_pct;        /* 0..100 % */

    int   mode;           /* EchoMode */
    int   routing;        /* EchoRouting */
    int   sync;           /* 0=Free, 1=Sync */
    float width_pct;      /* 0..100 % */

    float hpf_hz;         /* 20..1000 Hz */
    float lpf_hz;         /* 500..20000 Hz */
    float drive_pct;      /* 0..100 % */
    float diffuse_pct;    /* 0..100 % */

    float mod_rate_hz;    /* 0.05..8 Hz */
    float mod_depth_pct;  /* 0..100 % */
    float duck_pct;       /* 0..100 % */
    float duck_rel_ms;    /* 50..1000 ms */

    int   freeze;         /* 0/1 */
    int   trails;         /* 0=Send On, 1=Mute In */
    float output_db;      /* -18..+12 dB */
    int   bypass;         /* 0/1 */

    float time_fx_pct;    /* 25..400 %, default 100 % */
    int   time_link;      /* 0=Unlinked (L only), 1=Linked (L & R) */
    float filter_fx_pct;  /* -100..+100 %, default 0 % */
    float wash_fx_pct;    /* 0..100 %, default 0 % */

    float host_bpm;       /* 20..400 BPM */
} EchoParams;

typedef struct {
    double b0, b1, b2, a1, a2;
    double z1_l, z2_l;
    double z1_r, z2_r;
} EchoBiquad;

typedef struct {
    float   *buf;
    uint32_t len;
    uint32_t idx;
} EchoAllpass;

typedef struct {
    float in_peak_db;
    float out_peak_db;
    float wet_l_db;
    float wet_r_db;
    float duck_db;
    float actual_ms_l;
    float actual_ms_r;
    float host_bpm;
} EchoMeters;

typedef struct {
    double    sr;
    float    *buf_l;
    float    *buf_r;
    uint32_t  buf_mask;
    uint32_t  buf_size;
    uint32_t  write_pos;

    EchoAllpass ap_l[ECHO_AP_STAGES];
    EchoAllpass ap_r[ECHO_AP_STAGES];

    EchoBiquad hpf;
    EchoBiquad lpf;
    EchoBiquad bbd_lpf;

    double dc_x_l, dc_y_l;
    double dc_x_r, dc_y_r;

    double smooth_ms_l;
    double smooth_ms_r;
    double vel_ms_l;
    double vel_ms_r;
    double smooth_fb;
    double smooth_mix;
    double smooth_send;
    double smooth_out_gain;
    double smooth_bypass;

    double lfo_phase;
    double wow_phase;

    double duck_env;
    double duck_gain;

    double peak_rel;
    double in_pk_lin;
    double out_pk_lin;
    double wet_l_lin;
    double wet_r_lin;

    EchoMeters meters;
} EchoCore;

void   echo_params_default(EchoParams *p);
int    echo_core_init(EchoCore *c, double sr);
void   echo_core_free(EchoCore *c);
void   echo_core_reset(EchoCore *c);
double echo_quantize_ms(double raw_ms, double bpm);
void   echo_core_process(EchoCore *c, const EchoParams *p,
                         const float *in_l, const float *in_r,
                         float *out_l, float *out_r,
                         uint32_t nframes);

#endif /* EKA_ECHO_CORE_H */
