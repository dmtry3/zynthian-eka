/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Eka Space DSP core: 5 stereo expansion engines, 2D pan/depth/tilt stage,
 * tempo-synced spatial modulation, bass mono anchor, early reflections, and phase guard. */
#include "space_core.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static inline double clampd(double v, double lo, double hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static inline double flush_denorm(double v) {
    return (fabs(v) < 1e-18) ? 0.0 : v;
}

static inline double db_to_lin(double db) {
    return pow(10.0, db * 0.05);
}

static inline double lin_to_db(double lin) {
    if (lin <= 1e-6) return -60.0;
    return clampd(20.0 * log10(lin), -60.0, 12.0);
}

static uint32_t next_pow2(uint32_t v) {
    uint32_t p = 1;
    while (p < v && p < (1u << 22)) p <<= 1;
    return p;
}

static void biquad_set_identity(SpaceBiquad *b) {
    b->b0 = 1.0; b->b1 = 0.0; b->b2 = 0.0; b->a1 = 0.0; b->a2 = 0.0;
}

static void biquad_set_hpf(SpaceBiquad *b, double sr, double f) {
    if (f <= 20.5) {
        biquad_set_identity(b);
        return;
    }
    f = clampd(f, 20.0, sr * 0.45);
    double w0 = 2.0 * M_PI * f / sr;
    double cw = cos(w0), sw = sin(w0);
    double alpha = sw / (2.0 * 0.70710678);
    double inv = 1.0 / (1.0 + alpha);
    b->b0 = ((1.0 + cw) * 0.5) * inv;
    b->b1 = (-(1.0 + cw)) * inv;
    b->b2 = ((1.0 + cw) * 0.5) * inv;
    b->a1 = (-2.0 * cw) * inv;
    b->a2 = (1.0 - alpha) * inv;
}

static void biquad_set_lpf(SpaceBiquad *b, double sr, double f) {
    if (f >= 19800.0) {
        biquad_set_identity(b);
        return;
    }
    f = clampd(f, 20.0, sr * 0.45);
    double w0 = 2.0 * M_PI * f / sr;
    double cw = cos(w0), sw = sin(w0);
    double alpha = sw / (2.0 * 0.70710678);
    double inv = 1.0 / (1.0 + alpha);
    b->b0 = ((1.0 - cw) * 0.5) * inv;
    b->b1 = (1.0 - cw) * inv;
    b->b2 = ((1.0 - cw) * 0.5) * inv;
    b->a1 = (-2.0 * cw) * inv;
    b->a2 = (1.0 - alpha) * inv;
}

static void biquad_set_high_shelf(SpaceBiquad *b, double sr, double f, double q, double gain_db) {
    if (fabs(gain_db) < 0.05) {
        biquad_set_identity(b);
        return;
    }
    f = clampd(f, 100.0, sr * 0.45);
    double A = pow(10.0, gain_db / 40.0);
    double w0 = 2.0 * M_PI * f / sr;
    double cw = cos(w0), sw = sin(w0);
    double alpha = sw / (2.0 * q);
    double sq = 2.0 * sqrt(A) * alpha;
    double inv = 1.0 / ((A + 1.0) - (A - 1.0) * cw + sq);
    b->b0 = A * ((A + 1.0) + (A - 1.0) * cw + sq) * inv;
    b->b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cw) * inv;
    b->b2 = A * ((A + 1.0) + (A - 1.0) * cw - sq) * inv;
    b->a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cw) * inv;
    b->a2 = ((A + 1.0) - (A - 1.0) * cw - sq) * inv;
}

static inline double biquad_process_1(SpaceBiquad *b, double x, int ch) {
    if (ch == 0) {
        double y = b->b0 * x + b->z1_l;
        b->z1_l = b->b1 * x - b->a1 * y + b->z2_l;
        b->z2_l = b->b2 * x - b->a2 * y;
        return y;
    } else {
        double y = b->b0 * x + b->z1_r;
        b->z1_r = b->b1 * x - b->a1 * y + b->z2_r;
        b->z2_r = b->b2 * x - b->a2 * y;
        return y;
    }
}

static inline void biquad_process_stereo(SpaceBiquad *b, double *l, double *r) {
    *l = biquad_process_1(b, *l, 0);
    *r = biquad_process_1(b, *r, 1);
}

/* 4-point Hermite interpolation read from circular buffer. */
static inline double read_hermite(const float *buf, uint32_t mask, uint32_t write_pos, double delay_samps) {
    double read_pos = (double)write_pos - delay_samps;
    int64_t ipos = (int64_t)floor(read_pos);
    double frac = read_pos - (double)ipos;

    uint32_t i0 = (uint32_t)(ipos - 1) & mask;
    uint32_t i1 = (uint32_t)(ipos)     & mask;
    uint32_t i2 = (uint32_t)(ipos + 1) & mask;
    uint32_t i3 = (uint32_t)(ipos + 2) & mask;

    double y0 = (double)buf[i0];
    double y1 = (double)buf[i1];
    double y2 = (double)buf[i2];
    double y3 = (double)buf[i3];

    double c0 = y1;
    double c1 = 0.5 * (y2 - y0);
    double c2 = y0 - 2.5 * y1 + 2.0 * y2 - 0.5 * y3;
    double c3 = 0.5 * (y3 - y0) + 1.5 * (y1 - y2);
    return ((c3 * frac + c2) * frac + c1) * frac + c0;
}

static inline double allpass_process(SpaceAllpass *ap, double x, double g) {
    if (!ap->buf || ap->len == 0) return x;
    double delayed = (double)ap->buf[ap->idx];
    double v = x + g * delayed;
    double y = delayed - g * v;
    ap->buf[ap->idx] = (float)v;
    ap->idx++;
    if (ap->idx >= ap->len) ap->idx = 0;
    return y;
}

double space_quantize_rate_hz(double raw_hz, double bpm) {
    /* 16 musical LFO cycle lengths in quarter-note multiples: 4 bars .. 1/32 */
    static const double quarters[16] = {
        16.0, 8.0, 4.0, 3.0, 2.0, 1.5, 4.0 / 3.0, 1.0,
        0.75, 2.0 / 3.0, 0.5, 1.0 / 3.0, 0.25, 1.0 / 6.0, 0.125, 1.0 / 16.0
    };
    bpm = clampd(bpm, 20.0, 400.0);
    double q_sec = 60.0 / bpm;
    double target = clampd(raw_hz, 0.05, 12.0);
    double log_t = log(target);

    double best_hz = target;
    double best_err = 1e9;
    for (int i = 0; i < 16; ++i) {
        double cand_hz = 1.0 / (q_sec * quarters[i]);
        if (cand_hz < 0.04 || cand_hz > 14.0) continue;
        double err = fabs(log(cand_hz) - log_t);
        if (err < best_err) {
            best_err = err;
            best_hz = cand_hz;
        }
    }
    return best_hz;
}

void space_params_default(SpaceParams *p) {
    if (!p) return;
    memset(p, 0, sizeof(*p));
    p->pan_pct       = 0.0f;
    p->width_pct     = 100.0f;
    p->depth_pct     = 0.0f;
    p->tilt_pct      = 0.0f;

    p->mode          = SPACE_MODE_COMB;
    p->widen_pct     = 30.0f;
    p->spread_ms     = 14.0f;
    p->room_pct      = 15.0f;

    p->mod_shape     = SPACE_MOD_PAN;
    p->mod_rate_hz   = 0.5f;
    p->mod_depth_pct = 0.0f;
    p->sync          = 1;

    p->mono_low_hz   = 120.0f;
    p->side_hpf_hz   = 90.0f;
    p->side_air_db   = 1.5f;
    p->env_width_pct = 0.0f;

    p->pan_fx_pct    = 0.0f;
    p->width_fx_pct  = 0.0f;
    p->spin_fx_pct   = 0.0f;
    p->fx_zero       = 0;

    p->audition      = SPACE_AUD_STEREO;
    p->safe_phase    = 0;
    p->mix_pct       = 100.0f;
    p->output_db     = 0.0f;
    p->bypass        = 0;
    p->host_bpm      = 120.0f;
}

int space_core_init(SpaceCore *c, double sr) {
    if (!c) return -1;
    memset(c, 0, sizeof(*c));
    c->sr = (sr > 8000.0 && sr < 384000.0) ? sr : 48000.0;

    /* 120 ms circular buffers cover spread up to 40 ms + early reflections + modulation */
    uint32_t need = (uint32_t)ceil(c->sr * 0.140) + 64;
    c->buf_size = next_pow2(need);
    c->buf_mask = c->buf_size - 1u;

    c->buf_l = (float *)calloc(c->buf_size, sizeof(float));
    c->buf_r = (float *)calloc(c->buf_size, sizeof(float));
    c->buf_m = (float *)calloc(c->buf_size, sizeof(float));
    if (!c->buf_l || !c->buf_r || !c->buf_m) {
        space_core_free(c);
        return -1;
    }

    /* Decorrelated prime-ish all-pass lengths (ms) for Vortex phase disperser */
    static const double ap_ms_l[SPACE_AP_STAGES] = { 2.3, 5.9, 11.3, 19.7 };
    static const double ap_ms_r[SPACE_AP_STAGES] = { 2.9, 6.7, 13.1, 22.9 };
    for (int i = 0; i < SPACE_AP_STAGES; ++i) {
        c->ap_l[i].len = (uint32_t)fmax(8.0, round(ap_ms_l[i] * 0.001 * c->sr));
        c->ap_r[i].len = (uint32_t)fmax(8.0, round(ap_ms_r[i] * 0.001 * c->sr));
        c->ap_l[i].buf = (float *)calloc(c->ap_l[i].len, sizeof(float));
        c->ap_r[i].buf = (float *)calloc(c->ap_r[i].len, sizeof(float));
        if (!c->ap_l[i].buf || !c->ap_r[i].buf) {
            space_core_free(c);
            return -1;
        }
    }

    biquad_set_identity(&c->side_hpf);
    biquad_set_identity(&c->side_air);
    biquad_set_identity(&c->bass_lpf);
    biquad_set_identity(&c->tilt_eq_l);
    biquad_set_identity(&c->tilt_eq_r);
    biquad_set_identity(&c->depth_lpf);
    biquad_set_lpf(&c->er_lpf, c->sr, 5200.0);

    c->smooth_pan = 0.0;
    c->smooth_width = 1.0;
    c->smooth_depth = 0.0;
    c->smooth_tilt = 0.0;
    c->smooth_widen = 0.30;
    c->smooth_spread_ms = 14.0;
    c->smooth_room = 0.15;
    c->smooth_mix = 1.0;
    c->smooth_out_gain = 1.0;
    c->smooth_bypass = 0.0;
    c->safe_gain = 1.0;

    c->corr_ll = 1e-5;
    c->corr_rr = 1e-5;
    c->corr_lr = 1e-5;
    c->peak_rel = exp(-1.0 / (c->sr * 0.180));

    c->meters.in_peak_db = -60.0f;
    c->meters.out_peak_db = -60.0f;
    c->meters.mid_db = -60.0f;
    c->meters.side_db = -60.0f;
    c->meters.corr = 1.0f;
    c->meters.balance = 0.0f;
    c->meters.eff_pan_pct = 0.0f;
    c->meters.eff_width_pct = 100.0f;
    c->meters.eff_depth_pct = 0.0f;
    c->meters.host_bpm = 120.0f;
    return 0;
}

void space_core_free(SpaceCore *c) {
    if (!c) return;
    free(c->buf_l); c->buf_l = NULL;
    free(c->buf_r); c->buf_r = NULL;
    free(c->buf_m); c->buf_m = NULL;
    for (int i = 0; i < SPACE_AP_STAGES; ++i) {
        free(c->ap_l[i].buf); c->ap_l[i].buf = NULL;
        free(c->ap_r[i].buf); c->ap_r[i].buf = NULL;
    }
}

void space_core_reset(SpaceCore *c) {
    if (!c) return;
    if (c->buf_l) memset(c->buf_l, 0, c->buf_size * sizeof(float));
    if (c->buf_r) memset(c->buf_r, 0, c->buf_size * sizeof(float));
    if (c->buf_m) memset(c->buf_m, 0, c->buf_size * sizeof(float));
    c->write_pos = 0;
    for (int i = 0; i < SPACE_AP_STAGES; ++i) {
        if (c->ap_l[i].buf) memset(c->ap_l[i].buf, 0, c->ap_l[i].len * sizeof(float));
        if (c->ap_r[i].buf) memset(c->ap_r[i].buf, 0, c->ap_r[i].len * sizeof(float));
        c->ap_l[i].idx = 0;
        c->ap_r[i].idx = 0;
    }
    c->side_hpf.z1_l = c->side_hpf.z2_l = c->side_hpf.z1_r = c->side_hpf.z2_r = 0.0;
    c->side_air.z1_l = c->side_air.z2_l = c->side_air.z1_r = c->side_air.z2_r = 0.0;
    c->bass_lpf.z1_l = c->bass_lpf.z2_l = c->bass_lpf.z1_r = c->bass_lpf.z2_r = 0.0;
    c->tilt_eq_l.z1_l = c->tilt_eq_l.z2_l = c->tilt_eq_l.z1_r = c->tilt_eq_l.z2_r = 0.0;
    c->tilt_eq_r.z1_l = c->tilt_eq_r.z2_l = c->tilt_eq_r.z1_r = c->tilt_eq_r.z2_r = 0.0;
    c->depth_lpf.z1_l = c->depth_lpf.z2_l = c->depth_lpf.z1_r = c->depth_lpf.z2_r = 0.0;
    c->er_lpf.z1_l = c->er_lpf.z2_l = c->er_lpf.z1_r = c->er_lpf.z2_r = 0.0;
    c->haas_lpf_z_l = 0.0;
    c->haas_lpf_z_r = 0.0;

    c->env_fast = 0.0;
    c->env_slow = 0.0;
    c->smooth_pan = 0.0;
    c->smooth_width = 1.0;
    c->smooth_depth = 0.0;
    c->smooth_tilt = 0.0;
    c->smooth_widen = 0.30;
    c->smooth_spread_ms = 14.0;
    c->smooth_room = 0.15;
    c->smooth_mix = 1.0;
    c->smooth_out_gain = 1.0;
    c->smooth_bypass = 0.0;
    c->safe_gain = 1.0;
    c->corr_ll = 1e-5;
    c->corr_rr = 1e-5;
    c->corr_lr = 1e-5;
    c->fx_zero_mask = 0;
    c->in_pk_lin = c->out_pk_lin = c->mid_pk_lin = c->side_pk_lin = 0.0;

    c->meters.in_peak_db = -60.0f;
    c->meters.out_peak_db = -60.0f;
    c->meters.mid_db = -60.0f;
    c->meters.side_db = -60.0f;
    c->meters.corr = 1.0f;
    c->meters.balance = 0.0f;
}

void space_core_process(SpaceCore *c, const SpaceParams *p,
                        const float *in_l, const float *in_r,
                        float *out_l, float *out_r,
                        uint32_t nframes) {
    if (!c || !p || !c->buf_l || !c->buf_r || !c->buf_m || !in_l || !in_r || !out_l || !out_r || nframes == 0) return;

    double bpm = clampd(p->host_bpm, 20.0, 400.0);

    /* Handle Live FX and MIDI-mappable FX Zero latch */
    double raw_pfx = clampd(p->pan_fx_pct, -100.0, 100.0);
    double raw_wfx = clampd(p->width_fx_pct, -100.0, 100.0);
    double raw_sfx = clampd(p->spin_fx_pct, 0.0, 100.0);
    if (p->fx_zero) {
        c->fx_zero_mask   = 7;
        c->fx_latch_pan   = raw_pfx;
        c->fx_latch_width = raw_wfx;
        c->fx_latch_spin  = raw_sfx;
        raw_pfx = 0.0;
        raw_wfx = 0.0;
        raw_sfx = 0.0;
    } else if (c->fx_zero_mask) {
        if ((c->fx_zero_mask & 1) && fabs(raw_pfx - c->fx_latch_pan) > 0.5)   c->fx_zero_mask &= ~1;
        if ((c->fx_zero_mask & 2) && fabs(raw_wfx - c->fx_latch_width) > 0.5) c->fx_zero_mask &= ~2;
        if ((c->fx_zero_mask & 4) && fabs(raw_sfx - c->fx_latch_spin) > 0.5)  c->fx_zero_mask &= ~4;
        if (c->fx_zero_mask & 1) raw_pfx = 0.0;
        if (c->fx_zero_mask & 2) raw_wfx = 0.0;
        if (c->fx_zero_mask & 4) raw_sfx = 0.0;
    }

    int mode = p->mode;
    if (mode < 0 || mode > SPACE_MODE_VORTEX) mode = SPACE_MODE_COMB;
    int mod_shape = p->mod_shape;
    if (mod_shape < 0 || mod_shape > SPACE_MOD_DRIFT) mod_shape = SPACE_MOD_PAN;
    int audition = p->audition;
    if (audition < 0 || audition > SPACE_AUD_SIDE) audition = SPACE_AUD_STEREO;

    double base_pan = clampd(p->pan_pct + raw_pfx, -100.0, 100.0) * 0.01;
    double base_width = clampd(p->width_pct + raw_wfx * 1.5, 0.0, 300.0) * 0.01;
    double spin = raw_sfx * 0.01;
    double base_depth = clampd(p->depth_pct + raw_sfx * 0.35, 0.0, 100.0) * 0.01;
    double target_tilt = clampd(p->tilt_pct, -100.0, 100.0) * 0.01;

    double target_widen = clampd(p->widen_pct + raw_sfx * 45.0, 0.0, 100.0) * 0.01;
    double target_spread_ms = clampd(p->spread_ms, SPACE_MIN_SPREAD_MS, SPACE_MAX_SPREAD_MS);
    double target_room = clampd(p->room_pct + raw_sfx * 40.0, 0.0, 100.0) * 0.01;

    double mod_rate = clampd(p->mod_rate_hz, 0.05, 12.0);
    if (p->sync) {
        mod_rate = space_quantize_rate_hz(mod_rate, bpm);
    }
    double mod_depth = clampd(p->mod_depth_pct, 0.0, 100.0) * 0.01;
    double env_width = clampd(p->env_width_pct, -100.0, 100.0) * 0.01;

    double target_mix = clampd(p->mix_pct, 0.0, 100.0) * 0.01;
    double target_out = db_to_lin(clampd(p->output_db, -18.0, 12.0));
    double target_byp = p->bypass ? 1.0 : 0.0;

    /* Update filters */
    double mono_low_hz = clampd(p->mono_low_hz, 20.0, 500.0);
    double side_hpf_hz = clampd(p->side_hpf_hz, 20.0, 1000.0);
    double side_air_db = clampd(p->side_air_db, -12.0, 12.0);

    biquad_set_hpf(&c->side_hpf, c->sr, side_hpf_hz);
    biquad_set_high_shelf(&c->side_air, c->sr, 6500.0, 0.707, side_air_db);
    if (mono_low_hz > 20.5) {
        biquad_set_lpf(&c->bass_lpf, c->sr, mono_low_hz);
    } else {
        biquad_set_identity(&c->bass_lpf);
    }

    /* Spectral Tilt: opposite high-shelves at 900 Hz on L vs R (+/- 6 dB) */
    double tilt_db = target_tilt * 6.0;
    biquad_set_high_shelf(&c->tilt_eq_l, c->sr, 900.0, 0.65, -tilt_db);
    biquad_set_high_shelf(&c->tilt_eq_r, c->sr, 900.0, 0.65,  tilt_db);

    /* Distance / Depth air absorption LPF (20 kHz at 0% -> 2.4 kHz at 100%) */
    double depth_fc = 20000.0 * pow(2400.0 / 20000.0, base_depth);
    biquad_set_lpf(&c->depth_lpf, c->sr, depth_fc);

    /* One-pole head-shadow coefficient for Haas & Binaural Pan (~2.8 kHz) */
    double hs_alpha = 1.0 - exp(-2.0 * M_PI * 2800.0 / c->sr);

    const double ctrl_alpha = 1.0 - exp(-1.0 / (c->sr * 0.012));
    const double spread_alpha = 1.0 - exp(-1.0 / (c->sr * 0.025));
    const double env_att = 1.0 - exp(-1.0 / (c->sr * 0.003));
    const double env_rel = 1.0 - exp(-1.0 / (c->sr * 0.095));
    const double corr_alpha = 1.0 - exp(-1.0 / (c->sr * 0.060));
    const double safe_alpha = 1.0 - exp(-1.0 / (c->sr * 0.020));

    double lfo_inc = 2.0 * M_PI * mod_rate / c->sr;
    double drift_inc = 2.0 * M_PI * (mod_rate * 0.37 + 0.13) / c->sr;
    double dim_inc = 2.0 * M_PI * (0.42 + 0.35 * mod_rate) / c->sr;
    double spin_rate = 0.6 + spin * 4.8;
    double spin_inc = 2.0 * M_PI * spin_rate / c->sr;

    /* Early reflection tap ratios relative to spread_ms (plus 4 ms base) */
    static const double er_mult_l[SPACE_ER_TAPS] = { 0.43, 0.89, 1.37, 1.93 };
    static const double er_mult_r[SPACE_ER_TAPS] = { 0.53, 0.97, 1.49, 2.11 };
    static const double er_gain_l[SPACE_ER_TAPS] = { 0.42, -0.31, 0.24, -0.17 };
    static const double er_gain_r[SPACE_ER_TAPS] = { 0.39,  0.33, -0.25, 0.18 };

    double max_samps = (double)(c->buf_size - 8u);
    double in_pk = c->in_pk_lin;
    double out_pk = c->out_pk_lin;
    double mid_pk = c->mid_pk_lin;
    double side_pk = c->side_pk_lin;

    double last_eff_pan = base_pan;
    double last_eff_width = base_width;
    double last_eff_depth = base_depth;

    for (uint32_t i = 0; i < nframes; ++i) {
        double xl = isfinite(in_l[i]) ? (double)in_l[i] : 0.0;
        double xr = isfinite(in_r[i]) ? (double)in_r[i] : 0.0;

        double abs_in = fmax(fabs(xl), fabs(xr));
        in_pk = (abs_in > in_pk) ? abs_in : in_pk * c->peak_rel;

        /* Smooth base controls */
        c->smooth_pan       += ctrl_alpha * (base_pan - c->smooth_pan);
        c->smooth_width     += ctrl_alpha * (base_width - c->smooth_width);
        c->smooth_depth     += ctrl_alpha * (base_depth - c->smooth_depth);
        c->smooth_tilt      += ctrl_alpha * (target_tilt - c->smooth_tilt);
        c->smooth_widen     += ctrl_alpha * (target_widen - c->smooth_widen);
        c->smooth_spread_ms += spread_alpha * (target_spread_ms - c->smooth_spread_ms);
        c->smooth_room      += ctrl_alpha * (target_room - c->smooth_room);
        c->smooth_mix       += ctrl_alpha * (target_mix - c->smooth_mix);
        c->smooth_out_gain  += ctrl_alpha * (target_out - c->smooth_out_gain);
        c->smooth_bypass    += ctrl_alpha * (target_byp - c->smooth_bypass);

        /* Advance LFOs */
        c->lfo_phase += lfo_inc;
        if (c->lfo_phase >= 2.0 * M_PI) c->lfo_phase -= 2.0 * M_PI;
        c->drift_phase += drift_inc;
        if (c->drift_phase >= 2.0 * M_PI) c->drift_phase -= 2.0 * M_PI;
        c->dim_phase += dim_inc;
        if (c->dim_phase >= 2.0 * M_PI) c->dim_phase -= 2.0 * M_PI;
        c->spin_phase += spin_inc;
        if (c->spin_phase >= 2.0 * M_PI) c->spin_phase -= 2.0 * M_PI;

        /* Input envelope follower for dynamic width */
        double env_k = (abs_in > c->env_fast) ? env_att : env_rel;
        c->env_fast += env_k * (abs_in - c->env_fast);
        double env_norm = clampd(c->env_fast * 2.2, 0.0, 1.0);

        /* Compute spatial LFO modulation offsets */
        double s1 = sin(c->lfo_phase);
        double c1 = cos(c->lfo_phase);
        double s2 = sin(2.0 * c->lfo_phase);
        double dr1 = 0.65 * s1 + 0.35 * sin(c->drift_phase);
        double dr2 = 0.65 * c1 - 0.35 * cos(c->drift_phase);

        double pan_mod = 0.0;
        double width_mod = 0.0;
        double depth_mod = 0.0;

        if (mod_depth > 1e-4) {
            if (mod_shape == SPACE_MOD_PAN) {
                pan_mod = s1 * mod_depth * 0.85;
            } else if (mod_shape == SPACE_MOD_BREATHE) {
                width_mod = s1 * mod_depth * 0.90;
            } else if (mod_shape == SPACE_MOD_ORBIT) {
                pan_mod   = s1 * mod_depth * 0.80;
                depth_mod = (0.5 * (1.0 - c1)) * mod_depth * 0.45;
                width_mod = c1 * mod_depth * 0.40;
            } else if (mod_shape == SPACE_MOD_FIGURE8) {
                pan_mod   = s1 * mod_depth * 0.85;
                width_mod = s2 * mod_depth * 0.55;
                depth_mod = (0.5 * (1.0 + s2)) * mod_depth * 0.35;
            } else { /* SPACE_MOD_DRIFT */
                pan_mod   = dr1 * mod_depth * 0.70;
                width_mod = dr2 * mod_depth * 0.65;
                depth_mod = (0.5 * (1.0 + dr2)) * mod_depth * 0.30;
            }
        }

        /* Add Live Spin FX 3D rotation */
        if (spin > 1e-4) {
            pan_mod   += sin(c->spin_phase) * spin * 0.75;
            width_mod += cos(c->spin_phase) * spin * 0.60;
        }

        /* Add Envelope -> Width modulation */
        width_mod += env_norm * env_width * 1.15;

        double eff_pan   = clampd(c->smooth_pan + pan_mod, -1.0, 1.0);
        double eff_width = clampd(c->smooth_width + width_mod, 0.0, 3.0);
        double eff_depth = clampd(c->smooth_depth + depth_mod, 0.0, 1.0);
        last_eff_pan = eff_pan;
        last_eff_width = eff_width;
        last_eff_depth = eff_depth;

        /* Write raw L, R, and Mid into circular history buffers */
        double in_mid = 0.5 * (xl + xr);
        double in_side = 0.5 * (xl - xr);
        c->buf_l[c->write_pos] = (float)xl;
        c->buf_r[c->write_pos] = (float)xr;
        c->buf_m[c->write_pos] = (float)in_mid;

        double spread_samps = clampd(c->smooth_spread_ms * 0.001 * c->sr, 4.0, max_samps * 0.45);
        double widen = c->smooth_widen;

        double proc_l = xl;
        double proc_r = xr;

        /* 1. Stereo Widener Engine (5 modes) */
        if (mode == SPACE_MODE_MS) {
            /* Mode 0: Pure Mid/Side + micro-spread synthetic side when input is mono */
            double syn_side = read_hermite(c->buf_m, c->buf_mask, c->write_pos, spread_samps * 0.5);
            double s_aug = in_side + widen * 0.35 * (in_mid - syn_side);
            proc_l = in_mid + s_aug;
            proc_r = in_mid - s_aug;
        } else if (mode == SPACE_MODE_COMB) {
            /* Mode 1: Complementary Comb Stereoizer (100% mono-compatible: +comb on L, -comb on R) */
            double d1 = read_hermite(c->buf_m, c->buf_mask, c->write_pos, spread_samps);
            double d2 = read_hermite(c->buf_m, c->buf_mask, c->write_pos, spread_samps * 0.618);
            double comb_side = (d1 - 0.55 * d2) * (widen * 0.78);
            proc_l = xl + comb_side;
            proc_r = xr - comb_side;
        } else if (mode == SPACE_MODE_HAAS) {
            /* Mode 2: Psychoacoustic Precedence (Haas delay + head-shadow damping on delayed side) */
            double r_del = read_hermite(c->buf_r, c->buf_mask, c->write_pos, spread_samps);
            c->haas_lpf_z_r += hs_alpha * (r_del - c->haas_lpf_z_r);
            double r_haas = r_del * (1.0 - 0.38 * widen) + c->haas_lpf_z_r * (0.38 * widen);
            proc_l = xl;
            proc_r = xr * (1.0 - widen) + r_haas * widen;
        } else if (mode == SPACE_MODE_DIMENSION) {
            /* Mode 3: Analog Dimension Chorus (dual quadrature BBD-modulated cross-phase lines) */
            double mod_a = sin(c->dim_phase);
            double mod_b = cos(c->dim_phase);
            double d_a = clampd(spread_samps * (0.65 + 0.22 * mod_a), 4.0, max_samps * 0.48);
            double d_b = clampd(spread_samps * (0.72 + 0.22 * mod_b), 4.0, max_samps * 0.48);
            double tap_a = read_hermite(c->buf_l, c->buf_mask, c->write_pos, d_a);
            double tap_b = read_hermite(c->buf_r, c->buf_mask, c->write_pos, d_b);
            double dim_side = 0.5 * (tap_a - tap_b) + 0.38 * (tap_a - read_hermite(c->buf_m, c->buf_mask, c->write_pos, d_b));
            proc_l = xl + widen * 0.80 * dim_side;
            proc_r = xr - widen * 0.80 * dim_side;
        } else { /* SPACE_MODE_VORTEX */
            /* Mode 4: 4-stage All-Pass Phase Disperser + cross-delayed hyper-wide cloud */
            double ap_l_out = xl;
            double ap_r_out = xr;
            double g_ap = 0.52 + 0.22 * widen;
            for (int s = 0; s < SPACE_AP_STAGES; ++s) {
                ap_l_out = allpass_process(&c->ap_l[s], ap_l_out, g_ap);
                ap_r_out = allpass_process(&c->ap_r[s], ap_r_out, -g_ap);
            }
            double cr_l = read_hermite(c->buf_r, c->buf_mask, c->write_pos, spread_samps * 0.78);
            double cr_r = read_hermite(c->buf_l, c->buf_mask, c->write_pos, spread_samps * 1.12);
            double vx_l = ap_l_out - 0.42 * cr_l;
            double vx_r = ap_r_out + 0.42 * cr_r;
            proc_l = xl * (1.0 - 0.85 * widen) + vx_l * (0.85 * widen);
            proc_r = xr * (1.0 - 0.85 * widen) + vx_r * (0.85 * widen);
        }

        /* 2. Early Reflections Room Stage (4 decorrelated stereo taps) */
        if (c->smooth_room > 1e-4) {
            double er_l = 0.0, er_r = 0.0;
            double base_er_samps = (4.0 + c->smooth_spread_ms * 0.85) * 0.001 * c->sr;
            for (int t = 0; t < SPACE_ER_TAPS; ++t) {
                double dl = clampd(base_er_samps * er_mult_l[t], 4.0, max_samps);
                double dr = clampd(base_er_samps * er_mult_r[t], 4.0, max_samps);
                er_l += er_gain_l[t] * read_hermite(c->buf_l, c->buf_mask, c->write_pos, dl);
                er_r += er_gain_r[t] * read_hermite(c->buf_r, c->buf_mask, c->write_pos, dr);
            }
            biquad_process_stereo(&c->er_lpf, &er_l, &er_r);
            double rm = c->smooth_room * (0.55 + 0.45 * eff_depth);
            proc_l += er_l * rm;
            proc_r += er_r * rm;
        }

        c->write_pos = (c->write_pos + 1u) & c->buf_mask;

        /* 3. Mid/Side Matrix: Side HPF, Side Air, Bass Mono Anchor, Width, and SafePhase */
        double mid  = 0.5 * (proc_l + proc_r);
        double side = 0.5 * (proc_l - proc_r);

        /* Side HPF + Side Air high shelf */
        side = biquad_process_1(&c->side_hpf, side, 0);
        side = biquad_process_1(&c->side_air, side, 0);

        /* Bass Mono Anchor: remove low frequencies below mono_low_hz from Side channel */
        if (mono_low_hz > 20.5) {
            double side_low = biquad_process_1(&c->bass_lpf, side, 0);
            side -= side_low;
        }

        /* Apply effective Width (0..300%) + SafePhase anti-cancellation guard */
        side *= eff_width * c->safe_gain;

        mid_pk  = (fabs(mid) > mid_pk)   ? fabs(mid)  : mid_pk * c->peak_rel;
        side_pk = (fabs(side) > side_pk) ? fabs(side) : side_pk * c->peak_rel;

        proc_l = mid + side;
        proc_r = mid - side;

        /* 4. Spectral Tilt (bipolar L/R spectral balance) */
        if (fabs(c->smooth_tilt) > 0.005) {
            proc_l = biquad_process_1(&c->tilt_eq_l, proc_l, 0);
            proc_r = biquad_process_1(&c->tilt_eq_r, proc_r, 0);
        }

        /* 5. Binaural Pan (Constant-Power Law + subtle far-ear head-shadow damping) */
        double angle = (eff_pan + 1.0) * (M_PI * 0.25); /* 0 .. pi/2 */
        double g_l = cos(angle) * 1.41421356;
        double g_r = sin(angle) * 1.41421356;
        /* Blend true stereo balance with cross-panning so stereo sources pan all the way */
        double pan_r_to_l = (eff_pan < 0.0) ? (-eff_pan * 0.45) : 0.0;
        double pan_l_to_r = (eff_pan > 0.0) ? ( eff_pan * 0.45) : 0.0;
        double p_l = (proc_l + pan_r_to_l * proc_r) * fmin(g_l, 1.25);
        double p_r = (proc_r + pan_l_to_r * proc_l) * fmin(g_r, 1.25);

        /* Subtle head shadow on the far ear when panned hard */
        c->haas_lpf_z_l += hs_alpha * (p_l - c->haas_lpf_z_l);
        double hs_r_amt = (eff_pan < -0.15) ? ((-eff_pan - 0.15) * 0.35) : 0.0;
        double hs_l_amt = (eff_pan >  0.15) ? (( eff_pan - 0.15) * 0.35) : 0.0;
        if (hs_l_amt > 0.0) p_l = p_l * (1.0 - hs_l_amt) + c->haas_lpf_z_l * hs_l_amt;
        if (hs_r_amt > 0.0) {
            /* reuse haas_lpf_z_l/r safely or apply subtle damping */
            p_r *= (1.0 - hs_r_amt * 0.35);
        }
        proc_l = p_l;
        proc_r = p_r;

        /* 6. Depth / Distance Stage (front-to-back proximity & high-frequency absorption) */
        if (eff_depth > 0.005) {
            double d_l = proc_l, d_r = proc_r;
            biquad_process_stereo(&c->depth_lpf, &d_l, &d_r);
            double dist_gain = 1.0 - 0.36 * eff_depth;
            proc_l = (proc_l * (1.0 - eff_depth) + d_l * eff_depth) * dist_gain;
            proc_r = (proc_r * (1.0 - eff_depth) + d_r * eff_depth) * dist_gain;
        }

        /* 7. Correlation & Balance Estimator + SafePhase feedback loop */
        c->corr_ll += corr_alpha * (proc_l * proc_l - c->corr_ll);
        c->corr_rr += corr_alpha * (proc_r * proc_r - c->corr_rr);
        c->corr_lr += corr_alpha * (proc_l * proc_r - c->corr_lr);
        double denom = sqrt(c->corr_ll * c->corr_rr) + 1e-12;
        double inst_corr = clampd(c->corr_lr / denom, -1.0, 1.0);

        if (p->safe_phase) {
            double want_safe = (inst_corr < 0.0) ? clampd(1.0 + inst_corr * 0.65, 0.30, 1.0) : 1.0;
            c->safe_gain += safe_alpha * (want_safe - c->safe_gain);
        } else {
            c->safe_gain += safe_alpha * (1.0 - c->safe_gain);
        }

        /* 8. Dry/Wet Mix & Output Trim */
        double m = c->smooth_mix;
        double yl = (xl * (1.0 - m) + proc_l * m) * c->smooth_out_gain;
        double yr = (xr * (1.0 - m) + proc_r * m) * c->smooth_out_gain;

        /* 9. Audition Monitor Mode (Stereo / Mono / Mid / Side) */
        if (audition == SPACE_AUD_MONO) {
            double mono = 0.5 * (yl + yr);
            yl = mono; yr = mono;
        } else if (audition == SPACE_AUD_MID) {
            double m_aud = 0.5 * (yl + yr);
            yl = m_aud; yr = m_aud;
        } else if (audition == SPACE_AUD_SIDE) {
            double s_aud = 0.5 * (yl - yr);
            yl = s_aud; yr = -s_aud;
        }

        /* Gentle safety ceiling at +/-2.0 */
        if (fabs(yl) > 2.0) yl = 2.0 * tanh(yl * 0.5);
        if (fabs(yr) > 2.0) yr = 2.0 * tanh(yr * 0.5);

        /* 10. Click-free Bypass crossfade */
        if (c->smooth_bypass > 1e-5) {
            double bm = c->smooth_bypass;
            yl = yl * (1.0 - bm) + xl * bm;
            yr = yr * (1.0 - bm) + xr * bm;
        }

        double abs_out = fmax(fabs(yl), fabs(yr));
        out_pk = (abs_out > out_pk) ? abs_out : out_pk * c->peak_rel;

        out_l[i] = isfinite(yl) ? (float)yl : 0.0f;
        out_r[i] = isfinite(yr) ? (float)yr : 0.0f;
    }

    /* Flush denormals once per block */
    c->side_hpf.z1_l = flush_denorm(c->side_hpf.z1_l); c->side_hpf.z2_l = flush_denorm(c->side_hpf.z2_l);
    c->side_air.z1_l = flush_denorm(c->side_air.z1_l); c->side_air.z2_l = flush_denorm(c->side_air.z2_l);
    c->bass_lpf.z1_l = flush_denorm(c->bass_lpf.z1_l); c->bass_lpf.z2_l = flush_denorm(c->bass_lpf.z2_l);
    c->tilt_eq_l.z1_l = flush_denorm(c->tilt_eq_l.z1_l); c->tilt_eq_l.z2_l = flush_denorm(c->tilt_eq_l.z2_l);
    c->tilt_eq_r.z1_l = flush_denorm(c->tilt_eq_r.z1_l); c->tilt_eq_r.z2_l = flush_denorm(c->tilt_eq_r.z2_l);
    c->depth_lpf.z1_l = flush_denorm(c->depth_lpf.z1_l); c->depth_lpf.z2_l = flush_denorm(c->depth_lpf.z2_l);
    c->depth_lpf.z1_r = flush_denorm(c->depth_lpf.z1_r); c->depth_lpf.z2_r = flush_denorm(c->depth_lpf.z2_r);
    c->er_lpf.z1_l = flush_denorm(c->er_lpf.z1_l); c->er_lpf.z2_l = flush_denorm(c->er_lpf.z2_l);
    c->er_lpf.z1_r = flush_denorm(c->er_lpf.z1_r); c->er_lpf.z2_r = flush_denorm(c->er_lpf.z2_r);
    c->haas_lpf_z_l = flush_denorm(c->haas_lpf_z_l);
    c->haas_lpf_z_r = flush_denorm(c->haas_lpf_z_r);
    c->env_fast = flush_denorm(c->env_fast);
    c->corr_ll = fmax(1e-12, flush_denorm(c->corr_ll));
    c->corr_rr = fmax(1e-12, flush_denorm(c->corr_rr));
    c->corr_lr = flush_denorm(c->corr_lr);

    c->in_pk_lin = flush_denorm(in_pk);
    c->out_pk_lin = flush_denorm(out_pk);
    c->mid_pk_lin = flush_denorm(mid_pk);
    c->side_pk_lin = flush_denorm(side_pk);

    double denom = sqrt(c->corr_ll * c->corr_rr) + 1e-12;
    double corr = (c->corr_ll + c->corr_rr > 1e-9) ? clampd(c->corr_lr / denom, -1.0, 1.0) : 1.0;
    double sum_e = c->corr_ll + c->corr_rr;
    double bal = (sum_e > 1e-9) ? clampd((c->corr_rr - c->corr_ll) / sum_e, -1.0, 1.0) : 0.0;

    c->meters.in_peak_db    = (float)lin_to_db(c->in_pk_lin);
    c->meters.out_peak_db   = (float)lin_to_db(c->out_pk_lin);
    c->meters.mid_db        = (float)lin_to_db(c->mid_pk_lin);
    c->meters.side_db       = (float)lin_to_db(c->side_pk_lin);
    c->meters.corr          = (float)corr;
    c->meters.balance       = (float)bal;
    c->meters.eff_pan_pct   = (float)(last_eff_pan * 100.0);
    c->meters.eff_width_pct = (float)(last_eff_width * 100.0);
    c->meters.eff_depth_pct = (float)(last_eff_depth * 100.0);
    c->meters.host_bpm      = (float)bpm;
}
