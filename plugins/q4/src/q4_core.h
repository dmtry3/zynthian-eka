/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Eka Q-4: 4-band parametric EQ + HPF/LPF + Tilt + Solo + 24-band spectrum analyzer. */
#ifndef EKA_Q4_CORE_H
#define EKA_Q4_CORE_H

#include <stdint.h>

#define Q4_BANDS        4
#define Q4_SPEC_BINS    24
#define Q4_NUM_STAGES   11  /* HPF1, HPF2, LPF1, LPF2, TiltL, TiltH, B1, B1_pultec, B2, B3, B4 */

typedef enum {
    Q4_B1_SHELF = 0,
    Q4_B1_BELL,
    Q4_B1_PULTEC,
    Q4_B1_OFF
} Q4Band1Type;

typedef enum {
    Q4_MID_BELL = 0,
    Q4_MID_NOTCH,
    Q4_MID_OFF
} Q4MidType;

typedef enum {
    Q4_B4_SHELF = 0,
    Q4_B4_BELL,
    Q4_B4_AIR,
    Q4_B4_OFF
} Q4Band4Type;

typedef struct {
    float freq_hz;
    float gain_db;
    float q;
    int   type;
} Q4BandParams;

typedef struct {
    Q4BandParams bands[Q4_BANDS];
    float hpf_hz;
    float lpf_hz;
    float tilt_db;
    int   slope_24;     /* 0 = 12 dB/oct, 1 = 24 dB/oct */
    float scale_pct;    /* 0..200 % */
    int   solo_band;    /* 0 = Off, 1..4 = Band 1..4 */
    int   auto_gain;    /* 0/1 */
    float output_db;    /* -18..+18 dB */
} Q4Params;

typedef struct {
    double b0, b1, b2;
    double a1, a2;
} Q4BiquadCoeff;

typedef struct {
    Q4BiquadCoeff cur;
    Q4BiquadCoeff target;
    double z1_l, z2_l;
    double z1_r, z2_r;
} Q4BiquadStage;

typedef struct {
    Q4BiquadCoeff coeff;
    double z1, z2;
    double env;
} Q4SpecBin;

typedef struct {
    float in_peak_db;
    float out_peak_db;
    float spec_db[Q4_SPEC_BINS];
} Q4Meters;

typedef struct {
    double sr;
    Q4BiquadStage stages[Q4_NUM_STAGES];
    Q4BiquadStage solo_stage;

    double cur_gain;
    double cur_solo_mix;

    Q4SpecBin spec[Q4_SPEC_BINS];
    double spec_att;
    double spec_rel;

    double peak_rel;
    double in_peak_lin;
    double out_peak_lin;
    Q4Meters meters;
} Q4Core;

void q4_params_default(Q4Params *p);
void q4_core_init(Q4Core *c, double sr);
void q4_core_reset(Q4Core *c);
void q4_core_process(Q4Core *c, const Q4Params *p,
                     const float *in_l, const float *in_r,
                     float *out_l, float *out_r,
                     uint32_t nframes);

/* Analytical magnitude response (dB) at frequency f_hz for the current params (excluding solo). */
double q4_eval_response_db(const Q4Params *p, double sr, double f_hz);

#endif /* EKA_Q4_CORE_H */
