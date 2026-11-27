/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Eka Echo DSP core: Hermite stereo delay, 4 characters, 4-stage diffuser, ducking, freeze. */
#include "echo_core.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_LN2
#define M_LN2 0.69314718055994530942
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
    while (p < v && p < (1u << 24)) p <<= 1;
    return p;
}

static void biquad_set_identity(EchoBiquad *b) {
    b->b0 = 1.0; b->b1 = 0.0; b->b2 = 0.0; b->a1 = 0.0; b->a2 = 0.0;
}

static void biquad_set_hpf(EchoBiquad *b, double sr, double f, double q) {
    if (f <= 20.5) {
        biquad_set_identity(b);
        return;
    }
    f = clampd(f, 20.0, sr * 0.45);
    q = clampd(q, 0.5, 4.0);
    double w0 = 2.0 * M_PI * f / sr;
    double cw = cos(w0), sw = sin(w0);
    double alpha = sw / (2.0 * q);
    double inv = 1.0 / (1.0 + alpha);
    b->b0 = ((1.0 + cw) * 0.5) * inv;
    b->b1 = (-(1.0 + cw)) * inv;
    b->b2 = ((1.0 + cw) * 0.5) * inv;
    b->a1 = (-2.0 * cw) * inv;
    b->a2 = (1.0 - alpha) * inv;
}

static void biquad_set_lpf(EchoBiquad *b, double sr, double f, double q) {
    if (f >= 19800.0) {
        biquad_set_identity(b);
        return;
    }
    f = clampd(f, 100.0, sr * 0.45);
    q = clampd(q, 0.5, 4.0);
    double w0 = 2.0 * M_PI * f / sr;
    double cw = cos(w0), sw = sin(w0);
    double alpha = sw / (2.0 * q);
    double inv = 1.0 / (1.0 + alpha);
    b->b0 = ((1.0 - cw) * 0.5) * inv;
    b->b1 = (1.0 - cw) * inv;
    b->b2 = ((1.0 - cw) * 0.5) * inv;
    b->a1 = (-2.0 * cw) * inv;
    b->a2 = (1.0 - alpha) * inv;
}

/* Musical bell-shaped resonance envelope for Filter FX (0 at center, rich in mid sweep, tamed at extremes). */
static inline double filter_fx_res_env(double amt) {
    if (amt <= 1e-4) return 0.0;
    double t = clampd(amt / 0.18, 0.0, 1.0);
    double rise = t * t * (3.0 - 2.0 * t);
    return rise * (1.0 - 0.36 * amt * amt);
}

/* Cytomic trapezoidal ZDF SVF with passive analog saturation on the resonant state. */
static inline void svf_process(EchoSVF *s, double a1, double a2, double a3, double k,
                               int is_hpf, double *l, double *r) {
    double xl = *l;
    double v3l = xl - s->ic2_l;
    double v1l = a1 * s->ic1_l + a2 * v3l;
    double v2l = s->ic2_l + a2 * s->ic1_l + a3 * v3l;
    s->ic1_l = tanh((2.0 * v1l - s->ic1_l) * 0.45) * (1.0 / 0.45);
    s->ic2_l = 2.0 * v2l - s->ic2_l;
    *l = is_hpf ? (xl - k * v1l - v2l) : v2l;

    double xr = *r;
    double v3r = xr - s->ic2_r;
    double v1r = a1 * s->ic1_r + a2 * v3r;
    double v2r = s->ic2_r + a2 * s->ic1_r + a3 * v3r;
    s->ic1_r = tanh((2.0 * v1r - s->ic1_r) * 0.45) * (1.0 / 0.45);
    s->ic2_r = 2.0 * v2r - s->ic2_r;
    *r = is_hpf ? (xr - k * v1r - v2r) : v2r;
}

static inline void biquad_process(EchoBiquad *b, double *l, double *r) {
    double xl = *l;
    double yl = b->b0 * xl + b->z1_l;
    b->z1_l = b->b1 * xl - b->a1 * yl + b->z2_l;
    b->z2_l = b->b2 * xl - b->a2 * yl;

    double xr = *r;
    double yr = b->b0 * xr + b->z1_r;
    b->z1_r = b->b1 * xr - b->a1 * yr + b->z2_r;
    b->z2_r = b->b2 * xr - b->a2 * yr;

    *l = yl;
    *r = yr;
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

static inline double allpass_process(EchoAllpass *ap, double x, double g) {
    if (!ap->buf || ap->len == 0) return x;
    double delayed = (double)ap->buf[ap->idx];
    double v = x + g * delayed;
    double y = delayed - g * v;
    ap->buf[ap->idx] = (float)v;
    ap->idx++;
    if (ap->idx >= ap->len) ap->idx = 0;
    return y;
}

static inline double soft_sat(double x, double drive, int asym) {
    if (drive <= 1e-4) {
        /* Gentle safety ceiling above +/-1.15 for self-oscillation */
        if (fabs(x) > 1.15) return 1.15 * tanh(x / 1.15);
        return x;
    }
    double g = 1.0 + drive * 3.2;
    double bias = asym ? (0.06 * drive) : 0.0;
    double y = tanh(x * g + bias) - tanh(bias);
    double norm = 1.0 / tanh(g);
    return y * norm;
}

double echo_quantize_ms(double raw_ms, double bpm) {
    /* 18 musical note divisions in quarter-note multiples: 1/64 .. 2 bars. */
    static const double quarters[18] = {
        1.0 / 16.0, 1.0 / 12.0, 3.0 / 32.0, 1.0 / 8.0, 1.0 / 6.0, 3.0 / 16.0,
        1.0 / 4.0,  1.0 / 3.0,  3.0 / 8.0,  1.0 / 2.0, 2.0 / 3.0, 3.0 / 4.0,
        1.0,        4.0 / 3.0,  1.5,        2.0,       3.0,       4.0
    };
    bpm = clampd(bpm, 20.0, 400.0);
    double q_ms = 60000.0 / bpm;
    double target = clampd(raw_ms, ECHO_MIN_MS, ECHO_MAX_MS);
    double log_t = log(target);

    double best_ms = target;
    double best_err = 1e9;
    for (int i = 0; i < 18; ++i) {
        double cand = q_ms * quarters[i];
        if (cand < ECHO_MIN_MS || cand > ECHO_MAX_MS) continue;
        double err = fabs(log(cand) - log_t);
        if (err < best_err) {
            best_err = err;
            best_ms = cand;
        }
    }
    return best_ms;
}

void echo_params_default(EchoParams *p) {
    if (!p) return;
    memset(p, 0, sizeof(*p));
    p->time_l_ms     = 375.0f;
    p->time_r_ms     = 500.0f;
    p->feedback_pct  = 42.0f;
    p->mix_pct       = 35.0f;
    p->mode          = ECHO_MODE_TAPE;
    p->routing       = ECHO_ROUTE_STEREO;
    p->sync          = 1;
    p->width_pct     = 100.0f;
    p->hpf_hz        = 80.0f;
    p->lpf_hz        = 8500.0f;
    p->drive_pct     = 15.0f;
    p->diffuse_pct   = 0.0f;
    p->mod_rate_hz   = 0.6f;
    p->mod_depth_pct = 18.0f;
    p->duck_pct      = 0.0f;
    p->duck_rel_ms   = 220.0f;
    p->freeze        = 0;
    p->trails        = 0;
    p->output_db     = 0.0f;
    p->bypass        = 0;
    p->time_fx_pct   = 0.0f;
    p->time_link     = 1;
    p->filter_fx_pct = 0.0f;
    p->wash_fx_pct   = 0.0f;
    p->fx_zero       = 0;
    p->time_smooth_pct = 50.0f;
    p->host_bpm      = 120.0f;
}

int echo_core_init(EchoCore *c, double sr) {
    if (!c) return -1;
    memset(c, 0, sizeof(*c));
    c->sr = (sr > 8000.0 && sr < 384000.0) ? sr : 48000.0;

    uint32_t need = (uint32_t)ceil(c->sr * 2.65) + 64;
    c->buf_size = next_pow2(need);
    c->buf_mask = c->buf_size - 1u;

    c->buf_l = (float *)calloc(c->buf_size, sizeof(float));
    c->buf_r = (float *)calloc(c->buf_size, sizeof(float));
    if (!c->buf_l || !c->buf_r) {
        echo_core_free(c);
        return -1;
    }

    /* Decorrelated prime-ish all-pass lengths (ms) for smooth L/R diffusion */
    static const double ap_ms_l[ECHO_AP_STAGES] = { 5.1, 11.7, 19.3, 31.1 };
    static const double ap_ms_r[ECHO_AP_STAGES] = { 5.7, 12.9, 21.1, 33.7 };
    for (int i = 0; i < ECHO_AP_STAGES; ++i) {
        c->ap_l[i].len = (uint32_t)fmax(16.0, round(ap_ms_l[i] * 0.001 * c->sr));
        c->ap_r[i].len = (uint32_t)fmax(16.0, round(ap_ms_r[i] * 0.001 * c->sr));
        c->ap_l[i].buf = (float *)calloc(c->ap_l[i].len, sizeof(float));
        c->ap_r[i].buf = (float *)calloc(c->ap_r[i].len, sizeof(float));
        if (!c->ap_l[i].buf || !c->ap_r[i].buf) {
            echo_core_free(c);
            return -1;
        }
    }

    biquad_set_identity(&c->hpf);
    biquad_set_identity(&c->lpf);
    biquad_set_lpf(&c->bbd_lpf, c->sr, 3600.0, 0.70710678);

    c->smooth_ms_l = 375.0;
    c->smooth_ms_r = 500.0;
    c->smooth_fb = 0.42;
    c->smooth_mix = 0.35;
    c->smooth_send = 1.0;
    c->smooth_freeze = 0.0;
    c->smooth_out_gain = 1.0;
    c->smooth_bypass = 0.0;
    c->smooth_link = 1.0;
    c->duck_gain = 1.0;
    c->peak_rel = exp(-1.0 / (c->sr * 0.180));

    c->meters.in_peak_db = -60.0f;
    c->meters.out_peak_db = -60.0f;
    c->meters.wet_l_db = -60.0f;
    c->meters.wet_r_db = -60.0f;
    c->meters.duck_db = 0.0f;
    c->meters.actual_ms_l = 375.0f;
    c->meters.actual_ms_r = 500.0f;
    c->meters.host_bpm = 120.0f;
    return 0;
}

void echo_core_free(EchoCore *c) {
    if (!c) return;
    free(c->buf_l); c->buf_l = NULL;
    free(c->buf_r); c->buf_r = NULL;
    for (int i = 0; i < ECHO_AP_STAGES; ++i) {
        free(c->ap_l[i].buf); c->ap_l[i].buf = NULL;
        free(c->ap_r[i].buf); c->ap_r[i].buf = NULL;
    }
}

void echo_core_reset(EchoCore *c) {
    if (!c) return;
    if (c->buf_l) memset(c->buf_l, 0, c->buf_size * sizeof(float));
    if (c->buf_r) memset(c->buf_r, 0, c->buf_size * sizeof(float));
    c->write_pos = 0;
    for (int i = 0; i < ECHO_AP_STAGES; ++i) {
        if (c->ap_l[i].buf) memset(c->ap_l[i].buf, 0, c->ap_l[i].len * sizeof(float));
        if (c->ap_r[i].buf) memset(c->ap_r[i].buf, 0, c->ap_r[i].len * sizeof(float));
        c->ap_l[i].idx = 0;
        c->ap_r[i].idx = 0;
    }
    c->hpf.z1_l = c->hpf.z2_l = c->hpf.z1_r = c->hpf.z2_r = 0.0;
    c->lpf.z1_l = c->lpf.z2_l = c->lpf.z1_r = c->lpf.z2_r = 0.0;
    c->bbd_lpf.z1_l = c->bbd_lpf.z2_l = c->bbd_lpf.z1_r = c->bbd_lpf.z2_r = 0.0;
    c->fx_svf.ic1_l = c->fx_svf.ic2_l = c->fx_svf.ic1_r = c->fx_svf.ic2_r = 0.0;
    c->dc_x_l = c->dc_y_l = c->dc_x_r = c->dc_y_r = 0.0;
    c->vel_ms_l = 0.0;
    c->vel_ms_r = 0.0;
    c->smooth_freeze = 0.0;
    c->fx_zero_mask = 0;
    c->tfx_s1 = 0.0;
    c->tfx_s2 = 0.0;
    c->smooth_link = 1.0;
    c->smooth_ffx = 0.0;
    c->smooth_wfx = 0.0;
    c->duck_env = 0.0;
    c->duck_gain = 1.0;
    c->in_pk_lin = c->out_pk_lin = c->wet_l_lin = c->wet_r_lin = 0.0;
    c->meters.in_peak_db = -60.0f;
    c->meters.out_peak_db = -60.0f;
    c->meters.wet_l_db = -60.0f;
    c->meters.wet_r_db = -60.0f;
    c->meters.duck_db = 0.0f;
}

void echo_core_process(EchoCore *c, const EchoParams *p,
                       const float *in_l, const float *in_r,
                       float *out_l, float *out_r,
                       uint32_t nframes) {
    if (!c || !p || !c->buf_l || !c->buf_r || !in_l || !in_r || !out_l || !out_r || nframes == 0) return;

    double bpm = clampd(p->host_bpm, 20.0, 400.0);
    double base_ms_l = clampd(p->time_l_ms, ECHO_MIN_MS, ECHO_MAX_MS);
    double base_ms_r = clampd(p->time_r_ms, ECHO_MIN_MS, ECHO_MAX_MS);
    if (p->sync) {
        base_ms_l = echo_quantize_ms(base_ms_l, bpm);
        base_ms_r = echo_quantize_ms(base_ms_r, bpm);
    }

    double raw_tfx = clampd(p->time_fx_pct, -100.0, 100.0);
    double raw_ffx = clampd(p->filter_fx_pct, -100.0, 100.0);
    double raw_wfx = clampd(p->wash_fx_pct, 0.0, 100.0);
    if (p->fx_zero) {
        c->fx_zero_mask    = 7;
        c->fx_latch_time   = raw_tfx;
        c->fx_latch_filter = raw_ffx;
        c->fx_latch_wash   = raw_wfx;
        raw_tfx = 0.0;
        raw_ffx = 0.0;
        raw_wfx = 0.0;
    } else if (c->fx_zero_mask) {
        if ((c->fx_zero_mask & 1) && fabs(raw_tfx - c->fx_latch_time) > 0.5)   c->fx_zero_mask &= ~1;
        if ((c->fx_zero_mask & 2) && fabs(raw_ffx - c->fx_latch_filter) > 0.5) c->fx_zero_mask &= ~2;
        if ((c->fx_zero_mask & 4) && fabs(raw_wfx - c->fx_latch_wash) > 0.5)   c->fx_zero_mask &= ~4;
        if (c->fx_zero_mask & 1) raw_tfx = 0.0;
        if (c->fx_zero_mask & 2) raw_ffx = 0.0;
        if (c->fx_zero_mask & 4) raw_wfx = 0.0;
    }

    /* Bipolar Time FX (-100..+100%, 0% = exact 1.0x original time):
     * -50% = 0.5x (+1 oct), -100% = 0.25x (+2 oct), +50% = 2.0x (-1 oct), +100% = 4.0x (-2 oct). */
    double target_tfx = (fabs(raw_tfx) < 0.75) ? 0.0 : raw_tfx;
    double time_mult = (target_tfx == 0.0) ? 1.0 : exp(target_tfx * (0.02 * M_LN2));
    double target_ms_l = clampd(base_ms_l * time_mult, ECHO_MIN_MS, ECHO_MAX_MS);
    double target_ms_r = p->time_link ? clampd(base_ms_r * time_mult, ECHO_MIN_MS, ECHO_MAX_MS) : base_ms_r;

    /* Smooth Wash FX across blocks; Filter FX is smoothed per-sample for zipper-free resonant sweeps */
    double blk_dt = (double)nframes / c->sr;
    double wfx_tau   = (raw_wfx < c->smooth_wfx) ? 0.065 : 0.035;
    double wfx_alpha = 1.0 - exp(-blk_dt / wfx_tau);
    c->smooth_wfx += wfx_alpha * (raw_wfx - c->smooth_wfx);
    if (raw_wfx < 0.5 && c->smooth_wfx < 0.05) c->smooth_wfx = 0.0;

    int freeze = p->freeze ? 1 : 0;
    int trails = p->trails ? 1 : 0;
    int mode = p->mode;
    if (mode < 0 || mode > ECHO_MODE_DIFF) mode = ECHO_MODE_CLEAN;
    int routing = p->routing;
    if (routing < 0 || routing > ECHO_ROUTE_CROSS) routing = ECHO_ROUTE_STEREO;

    double wash = c->smooth_wfx * 0.01;
    double target_fb = freeze ? 1.0 : (clampd(p->feedback_pct, 0.0, 110.0) * 0.01);
    if (!freeze && wash > 1e-4) {
        double w_fb = wash * (0.45 + 0.55 * wash);
        double boost_target = fmax(target_fb, 0.94);
        target_fb = clampd(target_fb + w_fb * (boost_target - target_fb), 0.0, 1.10);
    }
    double target_mix = clampd(p->mix_pct, 0.0, 100.0) * 0.01;
    double target_send = (freeze || trails) ? 0.0 : 1.0;
    double target_freeze = freeze ? 1.0 : 0.0;
    double target_out = db_to_lin(clampd(p->output_db, -18.0, 12.0));
    double target_byp = p->bypass ? 1.0 : 0.0;
    double width = clampd(p->width_pct, 0.0, 100.0) * 0.01;

    double eff_hpf = clampd(p->hpf_hz, 20.0, 1000.0);
    double eff_lpf = clampd(p->lpf_hz, 500.0, 20000.0);
    double ffx_blk = c->smooth_ffx * 0.01;
    double loop_hpf_q = 0.70710678;
    double loop_lpf_q = 0.70710678;
    /* Subtle, feedback-safe loop resonance complements the resonant wet SVF */
    double loop_res_gain = 0.52 * (1.0 - 0.65 * clampd(target_fb, 0.0, 1.0));
    if (ffx_blk < -0.005) {
        double amt = -ffx_blk;
        double fx_lpf = 20000.0 * pow(280.0 / 20000.0, amt);
        eff_lpf = fmin(eff_lpf, fx_lpf);
        loop_lpf_q += loop_res_gain * filter_fx_res_env(amt);
    } else if (ffx_blk > 0.005) {
        double amt = ffx_blk;
        double fx_hpf = 20.0 * pow(3200.0 / 20.0, amt);
        eff_hpf = fmax(eff_hpf, fx_hpf);
        loop_hpf_q += loop_res_gain * filter_fx_res_env(amt);
    }
    if (eff_hpf > eff_lpf * 0.85) {
        eff_hpf = eff_lpf * 0.85;
    }
    biquad_set_hpf(&c->hpf, c->sr, eff_hpf, loop_hpf_q);
    biquad_set_lpf(&c->lpf, c->sr, eff_lpf, loop_lpf_q);

    double drive = clampd(p->drive_pct, 0.0, 100.0) * 0.01;
    if (mode == ECHO_MODE_TAPE) drive = fmax(drive, 0.20);
    if (mode == ECHO_MODE_BBD)  drive = fmax(drive, 0.25);
    drive = clampd(drive + (wash * wash) * 0.14 * (1.0 - drive), 0.0, 1.0);

    double diff_amt = clampd(p->diffuse_pct, 0.0, 100.0) * 0.01;
    if (mode == ECHO_MODE_DIFF) diff_amt = clampd(0.55 + 0.45 * diff_amt, 0.0, 1.0);
    diff_amt = clampd(diff_amt + wash * 0.72 * (1.0 - diff_amt), 0.0, 1.0);
    double ap_g = 0.62 * diff_amt;

    double mod_rate = clampd(p->mod_rate_hz, 0.05, 8.0);
    double mod_depth = clampd(p->mod_depth_pct, 0.0, 100.0) * 0.01;
    double mod_ms_max = (mode == ECHO_MODE_TAPE) ? 6.5 : 4.5;
    double lfo_inc = 2.0 * M_PI * mod_rate / c->sr;
    double wow_inc = 2.0 * M_PI * (mod_rate * 0.37 + 0.19) / c->sr;

    double duck_amt = clampd(p->duck_pct, 0.0, 100.0) * 0.01;
    double duck_att = 1.0 - exp(-1.0 / (c->sr * 0.004));
    double duck_rel = 1.0 - exp(-1.0 / (c->sr * clampd(p->duck_rel_ms, 50.0, 1000.0) * 0.001));

    const double dt_ms = 1000.0 / c->sr;
    const double time_alpha = 1.0 - exp(-1.0 / (c->sr * 0.025)); /* 25 ms clean/diff */
    const double bbd_alpha  = 1.0 - exp(-1.0 / (c->sr * 0.055)); /* 55 ms BBD analog clock */
    const double tape_vel_alpha = 1.0 - exp(-1.0 / (c->sr * 0.028)); /* 28 ms tape motor inertia */
    const double ctrl_alpha = 1.0 - exp(-1.0 / (c->sr * 0.012)); /* 12 ms smooth controls */
    const double tsm = clampd(p->time_smooth_pct, 0.0, 100.0) * 0.01;
    const double tfx_tau = 0.004 * pow(100.0, tsm); /* 4 ms (0%) .. 40 ms (50%) .. 400 ms (100%) */
    const double tfx_alpha = 1.0 - exp(-1.0 / (c->sr * tfx_tau));
    const double ffx_smp_alpha = 1.0 - exp(-1.0 / (c->sr * 0.025)); /* 25 ms per-sample Filter FX glide */
    const double target_ffx = (fabs(raw_ffx) < 0.5) ? 0.0 : raw_ffx;
    const double target_link = p->time_link ? 1.0 : 0.0;
    const double dc_r = 0.9985;

    double svf_a1 = 0.0, svf_a2 = 0.0, svf_a3 = 0.0, svf_k = 1.41421356;
    double svf_wet = 0.0, svf_comp = 1.0;
    int svf_is_hpf = 0;

    double max_samps = (double)(c->buf_size - 8u);
    double in_pk = c->in_pk_lin;
    double out_pk = c->out_pk_lin;
    double wl_pk = c->wet_l_lin;
    double wr_pk = c->wet_r_lin;

    for (uint32_t i = 0; i < nframes; ++i) {
        double xl = isfinite(in_l[i]) ? (double)in_l[i] : 0.0;
        double xr = isfinite(in_r[i]) ? (double)in_r[i] : 0.0;

        double abs_in = fmax(fabs(xl), fabs(xr));
        in_pk = (abs_in > in_pk) ? abs_in : in_pk * c->peak_rel;

        /* 2-pole critically-damped S-curve smoothing for Time FX modulation */
        c->tfx_s1 += tfx_alpha * (target_tfx - c->tfx_s1);
        c->tfx_s2 += tfx_alpha * (c->tfx_s1 - c->tfx_s2);
        c->smooth_link += ctrl_alpha * (target_link - c->smooth_link);

        double eff_ms_l = base_ms_l;
        double eff_ms_r = base_ms_r;
        if (fabs(c->tfx_s2) >= 1e-4) {
            double ml = exp(c->tfx_s2 * (0.02 * M_LN2));
            eff_ms_l = clampd(base_ms_l * ml, ECHO_MIN_MS, ECHO_MAX_MS);
            if (c->smooth_link > 1e-4) {
                double mr = exp(c->tfx_s2 * c->smooth_link * (0.02 * M_LN2));
                eff_ms_r = clampd(base_ms_r * mr, ECHO_MIN_MS, ECHO_MAX_MS);
            }
        }

        /* Smooth delay times: musical inertia in Tape mode, bounded slew in BBD, reverse-safe in Clean/Diff */
        if (mode == ECHO_MODE_TAPE) {
            /* Critically-damped capstan motor: dD/dt in [-1.0, +0.58] => speed 0.42x..2.0x */
            double des_l = clampd((eff_ms_l - c->smooth_ms_l) / 120.0, -1.0, 0.58);
            double des_r = clampd((eff_ms_r - c->smooth_ms_r) / 120.0, -1.0, 0.58);
            c->vel_ms_l += tape_vel_alpha * (des_l - c->vel_ms_l);
            c->vel_ms_r += tape_vel_alpha * (des_r - c->vel_ms_r);
            c->smooth_ms_l = clampd(c->smooth_ms_l + c->vel_ms_l * dt_ms, ECHO_MIN_MS, ECHO_MAX_MS);
            c->smooth_ms_r = clampd(c->smooth_ms_r + c->vel_ms_r * dt_ms, ECHO_MIN_MS, ECHO_MAX_MS);
        } else if (mode == ECHO_MODE_BBD) {
            double step_l = clampd(bbd_alpha * (eff_ms_l - c->smooth_ms_l), -1.2 * dt_ms, 0.65 * dt_ms);
            double step_r = clampd(bbd_alpha * (eff_ms_r - c->smooth_ms_r), -1.2 * dt_ms, 0.65 * dt_ms);
            c->smooth_ms_l = clampd(c->smooth_ms_l + step_l, ECHO_MIN_MS, ECHO_MAX_MS);
            c->smooth_ms_r = clampd(c->smooth_ms_r + step_r, ECHO_MIN_MS, ECHO_MAX_MS);
            c->vel_ms_l = 0.0;
            c->vel_ms_r = 0.0;
        } else {
            double step_l = clampd(time_alpha * (eff_ms_l - c->smooth_ms_l), -2.5 * dt_ms, 0.72 * dt_ms);
            double step_r = clampd(time_alpha * (eff_ms_r - c->smooth_ms_r), -2.5 * dt_ms, 0.72 * dt_ms);
            c->smooth_ms_l = clampd(c->smooth_ms_l + step_l, ECHO_MIN_MS, ECHO_MAX_MS);
            c->smooth_ms_r = clampd(c->smooth_ms_r + step_r, ECHO_MIN_MS, ECHO_MAX_MS);
            c->vel_ms_l = 0.0;
            c->vel_ms_r = 0.0;
        }
        c->smooth_fb       += ctrl_alpha * (target_fb - c->smooth_fb);
        c->smooth_mix      += ctrl_alpha * (target_mix - c->smooth_mix);
        c->smooth_send     += ctrl_alpha * (target_send - c->smooth_send);
        c->smooth_freeze   += ctrl_alpha * (target_freeze - c->smooth_freeze);
        c->smooth_out_gain += ctrl_alpha * (target_out - c->smooth_out_gain);
        c->smooth_bypass   += ctrl_alpha * (target_byp - c->smooth_bypass);

        /* LFO modulation (quadrature L/R + tape wow) */
        c->lfo_phase += lfo_inc;
        if (c->lfo_phase >= 2.0 * M_PI) c->lfo_phase -= 2.0 * M_PI;
        c->wow_phase += wow_inc;
        if (c->wow_phase >= 2.0 * M_PI) c->wow_phase -= 2.0 * M_PI;

        double mod_l = sin(c->lfo_phase);
        double mod_r = cos(c->lfo_phase);
        if (mode == ECHO_MODE_TAPE) {
            double wow = 0.45 * sin(c->wow_phase);
            mod_l = 0.7 * mod_l + wow;
            mod_r = 0.7 * mod_r - wow;
        }
        double d_ms_l = c->smooth_ms_l + mod_l * mod_depth * mod_ms_max;
        double d_ms_r = c->smooth_ms_r + mod_r * mod_depth * mod_ms_max;
        double d_samps_l = clampd(d_ms_l * 0.001 * c->sr, 4.0, max_samps);
        double d_samps_r = clampd(d_ms_r * 0.001 * c->sr, 4.0, max_samps);

        /* Read delayed taps */
        double tap_l = read_hermite(c->buf_l, c->buf_mask, c->write_pos, d_samps_l);
        double tap_r = read_hermite(c->buf_r, c->buf_mask, c->write_pos, d_samps_r);

        /* Loop tone + character processing (smoothly bypassed while frozen so loop stays lossless) */
        double fb_l = tap_l;
        double fb_r = tap_r;
        if (c->smooth_freeze < 0.9999) {
            double proc_l = tap_l;
            double proc_r = tap_r;
            biquad_process(&c->hpf, &proc_l, &proc_r);
            biquad_process(&c->lpf, &proc_l, &proc_r);
            if (mode == ECHO_MODE_BBD) {
                biquad_process(&c->bbd_lpf, &proc_l, &proc_r);
            }
            if (ap_g > 0.01) {
                for (int s = 0; s < ECHO_AP_STAGES; ++s) {
                    proc_l = allpass_process(&c->ap_l[s], proc_l, ap_g);
                    proc_r = allpass_process(&c->ap_r[s], proc_r, ap_g);
                }
            }
            proc_l = soft_sat(proc_l, drive, mode == ECHO_MODE_TAPE);
            proc_r = soft_sat(proc_r, drive, mode == ECHO_MODE_TAPE);

            double fm = c->smooth_freeze;
            fb_l = proc_l + fm * (tap_l - proc_l);
            fb_r = proc_r + fm * (tap_r - proc_r);
        } else {
            /* Keep filter states clean while frozen so unfreezing is click-free */
            c->hpf.z1_l = c->hpf.z2_l = c->hpf.z1_r = c->hpf.z2_r = 0.0;
            c->lpf.z1_l = c->lpf.z2_l = c->lpf.z1_r = c->lpf.z2_r = 0.0;
            c->bbd_lpf.z1_l = c->bbd_lpf.z2_l = c->bbd_lpf.z1_r = c->bbd_lpf.z2_r = 0.0;
        }

        /* Gentle DC blocker always active in feedback loop */
        double dc_l = fb_l - c->dc_x_l + dc_r * c->dc_y_l;
        c->dc_x_l = fb_l; c->dc_y_l = dc_l; fb_l = dc_l;
        double dc_r_out = fb_r - c->dc_x_r + dc_r * c->dc_y_r;
        c->dc_x_r = fb_r; c->dc_y_r = dc_r_out; fb_r = dc_r_out;

        /* Feedback routing */
        double in_send_l = xl * c->smooth_send;
        double in_send_r = xr * c->smooth_send;
        double write_l = 0.0, write_r = 0.0;
        if (routing == ECHO_ROUTE_STEREO) {
            write_l = in_send_l + fb_l * c->smooth_fb;
            write_r = in_send_r + fb_r * c->smooth_fb;
        } else if (routing == ECHO_ROUTE_PINGPONG) {
            double mono_in = 0.5 * (in_send_l + in_send_r);
            double pp_lr = 0.70 + 0.30 * c->smooth_fb;
            write_l = mono_in + fb_r * c->smooth_fb;
            write_r = fb_l * pp_lr;
        } else { /* ECHO_ROUTE_CROSS */
            write_l = in_send_l + (0.62 * fb_l + 0.38 * fb_r) * c->smooth_fb;
            write_r = in_send_r + (0.62 * fb_r + 0.38 * fb_l) * c->smooth_fb;
        }

        /* Soft safety clamp before writing into delay line */
        if (fabs(write_l) > 2.0) write_l = 2.0 * tanh(write_l * 0.5);
        if (fabs(write_r) > 2.0) write_r = 2.0 * tanh(write_r * 0.5);

        c->buf_l[c->write_pos] = (float)write_l;
        c->buf_r[c->write_pos] = (float)write_r;
        c->write_pos = (c->write_pos + 1u) & c->buf_mask;

        /* Ducking envelope driven by input signal */
        double dk_k = (abs_in > c->duck_env) ? duck_att : duck_rel;
        c->duck_env += dk_k * (abs_in - c->duck_env);
        double duck_target = 1.0;
        if (duck_amt > 1e-4 && c->duck_env > 0.02) {
            double excess = clampd((c->duck_env - 0.02) / 0.35, 0.0, 1.0);
            double red_db = excess * duck_amt * 24.0;
            duck_target = db_to_lin(-red_db);
        }
        c->duck_gain += ctrl_alpha * (duck_target - c->duck_gain);

        /* Stereo width on wet signal */
        double mid = 0.5 * (tap_l + tap_r);
        double side = 0.5 * (tap_l - tap_r) * width;
        double wet_l = (mid + side) * c->duck_gain;
        double wet_r = (mid - side) * c->duck_gain;

        /* Musical resonant ZDF SVF for Filter FX on wet path (refresh coeffs every 8 samples) */
        c->smooth_ffx += ffx_smp_alpha * (target_ffx - c->smooth_ffx);
        if ((i & 7u) == 0u) {
            double ffx_cur = c->smooth_ffx * 0.01;
            double amt = fabs(ffx_cur);
            if (amt < 0.002) {
                svf_wet = 0.0;
                c->fx_svf.ic1_l = c->fx_svf.ic2_l = 0.0;
                c->fx_svf.ic1_r = c->fx_svf.ic2_r = 0.0;
            } else {
                svf_wet = clampd(amt / 0.04, 0.0, 1.0);
                svf_is_hpf = (ffx_cur > 0.0) ? 1 : 0;
                double f_cut = svf_is_hpf
                    ? (20.0 * pow(3200.0 / 20.0, amt))
                    : (20000.0 * pow(280.0 / 20000.0, amt));
                f_cut = clampd(f_cut, 20.0, c->sr * 0.42);
                double q = 0.70710678 + 2.9 * filter_fx_res_env(amt);
                double g = tan(M_PI * f_cut / c->sr);
                svf_k = 1.0 / q;
                svf_a1 = 1.0 / (1.0 + g * (g + svf_k));
                svf_a2 = g * svf_a1;
                svf_a3 = g * svf_a2;
                svf_comp = 1.0 / (1.0 + 0.12 * (q - 0.70710678));
            }
        }
        if (svf_wet > 1e-4) {
            double fl = wet_l, fr = wet_r;
            svf_process(&c->fx_svf, svf_a1, svf_a2, svf_a3, svf_k, svf_is_hpf, &fl, &fr);
            fl *= svf_comp;
            fr *= svf_comp;
            wet_l += svf_wet * (fl - wet_l);
            wet_r += svf_wet * (fr - wet_r);
        }

        wl_pk = (fabs(wet_l) > wl_pk) ? fabs(wet_l) : wl_pk * c->peak_rel;
        wr_pk = (fabs(wet_r) > wr_pk) ? fabs(wet_r) : wr_pk * c->peak_rel;

        /* Dry/wet mix + output trim */
        double m = c->smooth_mix;
        double dry_g = (m <= 0.5) ? 1.0 : (1.0 - (m - 0.5) * 2.0);
        double wet_g = (m <= 0.5) ? (m * 2.0) : 1.0;
        double yl = (xl * dry_g + wet_l * wet_g) * c->smooth_out_gain;
        double yr = (xr * dry_g + wet_r * wet_g) * c->smooth_out_gain;

        /* Click-free bypass crossfade */
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
    c->hpf.z1_l = flush_denorm(c->hpf.z1_l); c->hpf.z2_l = flush_denorm(c->hpf.z2_l);
    c->hpf.z1_r = flush_denorm(c->hpf.z1_r); c->hpf.z2_r = flush_denorm(c->hpf.z2_r);
    c->lpf.z1_l = flush_denorm(c->lpf.z1_l); c->lpf.z2_l = flush_denorm(c->lpf.z2_l);
    c->lpf.z1_r = flush_denorm(c->lpf.z1_r); c->lpf.z2_r = flush_denorm(c->lpf.z2_r);
    c->bbd_lpf.z1_l = flush_denorm(c->bbd_lpf.z1_l); c->bbd_lpf.z2_l = flush_denorm(c->bbd_lpf.z2_l);
    c->bbd_lpf.z1_r = flush_denorm(c->bbd_lpf.z1_r); c->bbd_lpf.z2_r = flush_denorm(c->bbd_lpf.z2_r);
    c->fx_svf.ic1_l = flush_denorm(c->fx_svf.ic1_l); c->fx_svf.ic2_l = flush_denorm(c->fx_svf.ic2_l);
    c->fx_svf.ic1_r = flush_denorm(c->fx_svf.ic1_r); c->fx_svf.ic2_r = flush_denorm(c->fx_svf.ic2_r);
    c->dc_x_l = flush_denorm(c->dc_x_l); c->dc_y_l = flush_denorm(c->dc_y_l);
    c->dc_x_r = flush_denorm(c->dc_x_r); c->dc_y_r = flush_denorm(c->dc_y_r);
    c->vel_ms_l = flush_denorm(c->vel_ms_l);
    c->vel_ms_r = flush_denorm(c->vel_ms_r);
    c->tfx_s1 = flush_denorm(c->tfx_s1);
    c->tfx_s2 = flush_denorm(c->tfx_s2);
    c->smooth_ffx = flush_denorm(c->smooth_ffx);
    c->smooth_wfx = flush_denorm(c->smooth_wfx);
    c->duck_env = flush_denorm(c->duck_env);

    c->in_pk_lin = flush_denorm(in_pk);
    c->out_pk_lin = flush_denorm(out_pk);
    c->wet_l_lin = flush_denorm(wl_pk);
    c->wet_r_lin = flush_denorm(wr_pk);

    c->meters.in_peak_db = (float)lin_to_db(c->in_pk_lin);
    c->meters.out_peak_db = (float)lin_to_db(c->out_pk_lin);
    c->meters.wet_l_db = (float)lin_to_db(c->wet_l_lin);
    c->meters.wet_r_db = (float)lin_to_db(c->wet_r_lin);
    double d_db = (c->duck_gain < 0.999) ? clampd(-20.0 * log10(fmax(c->duck_gain, 1e-4)), 0.0, 24.0) : 0.0;
    c->meters.duck_db = (float)d_db;
    c->meters.actual_ms_l = (float)target_ms_l;
    c->meters.actual_ms_r = (float)target_ms_r;
    c->meters.host_bpm = (float)bpm;
}
