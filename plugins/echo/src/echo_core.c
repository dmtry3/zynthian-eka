/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Eka Echo DSP core: Hermite stereo delay, 4 characters, 4-stage diffuser, ducking, freeze. */
#include "echo_core.h"

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
    while (p < v && p < (1u << 24)) p <<= 1;
    return p;
}

static void biquad_set_identity(EchoBiquad *b) {
    b->b0 = 1.0; b->b1 = 0.0; b->b2 = 0.0; b->a1 = 0.0; b->a2 = 0.0;
}

static void biquad_set_hpf(EchoBiquad *b, double sr, double f) {
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

static void biquad_set_lpf(EchoBiquad *b, double sr, double f) {
    if (f >= 19800.0) {
        biquad_set_identity(b);
        return;
    }
    f = clampd(f, 100.0, sr * 0.45);
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
    biquad_set_lpf(&c->bbd_lpf, c->sr, 3600.0);

    c->smooth_ms_l = 375.0;
    c->smooth_ms_r = 500.0;
    c->smooth_fb = 0.42;
    c->smooth_mix = 0.35;
    c->smooth_send = 1.0;
    c->smooth_out_gain = 1.0;
    c->smooth_bypass = 0.0;
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
    c->dc_x_l = c->dc_y_l = c->dc_x_r = c->dc_y_r = 0.0;
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
    double target_ms_l = clampd(p->time_l_ms, ECHO_MIN_MS, ECHO_MAX_MS);
    double target_ms_r = clampd(p->time_r_ms, ECHO_MIN_MS, ECHO_MAX_MS);
    if (p->sync) {
        target_ms_l = echo_quantize_ms(target_ms_l, bpm);
        target_ms_r = echo_quantize_ms(target_ms_r, bpm);
    }

    int freeze = p->freeze ? 1 : 0;
    int trails = p->trails ? 1 : 0;
    int mode = p->mode;
    if (mode < 0 || mode > ECHO_MODE_DIFF) mode = ECHO_MODE_CLEAN;
    int routing = p->routing;
    if (routing < 0 || routing > ECHO_ROUTE_CROSS) routing = ECHO_ROUTE_STEREO;

    double target_fb = freeze ? 1.0 : (clampd(p->feedback_pct, 0.0, 110.0) * 0.01);
    double target_mix = clampd(p->mix_pct, 0.0, 100.0) * 0.01;
    double target_send = (freeze || trails) ? 0.0 : 1.0;
    double target_out = db_to_lin(clampd(p->output_db, -18.0, 12.0));
    double target_byp = p->bypass ? 1.0 : 0.0;
    double width = clampd(p->width_pct, 0.0, 100.0) * 0.01;

    biquad_set_hpf(&c->hpf, c->sr, p->hpf_hz);
    biquad_set_lpf(&c->lpf, c->sr, p->lpf_hz);

    double drive = clampd(p->drive_pct, 0.0, 100.0) * 0.01;
    if (mode == ECHO_MODE_TAPE) drive = fmax(drive, 0.20);
    if (mode == ECHO_MODE_BBD)  drive = fmax(drive, 0.25);

    double diff_amt = clampd(p->diffuse_pct, 0.0, 100.0) * 0.01;
    if (mode == ECHO_MODE_DIFF) diff_amt = clampd(0.55 + 0.45 * diff_amt, 0.0, 1.0);
    double ap_g = 0.62 * diff_amt;

    double mod_rate = clampd(p->mod_rate_hz, 0.05, 8.0);
    double mod_depth = clampd(p->mod_depth_pct, 0.0, 100.0) * 0.01;
    double mod_ms_max = (mode == ECHO_MODE_TAPE) ? 6.5 : 4.5;
    double lfo_inc = 2.0 * M_PI * mod_rate / c->sr;
    double wow_inc = 2.0 * M_PI * (mod_rate * 0.37 + 0.19) / c->sr;

    double duck_amt = clampd(p->duck_pct, 0.0, 100.0) * 0.01;
    double duck_att = 1.0 - exp(-1.0 / (c->sr * 0.004));
    double duck_rel = 1.0 - exp(-1.0 / (c->sr * clampd(p->duck_rel_ms, 50.0, 1000.0) * 0.001));

    const double time_alpha = 1.0 - exp(-1.0 / (c->sr * 0.025)); /* 25 ms smooth tape repitch */
    const double ctrl_alpha = 1.0 - exp(-1.0 / (c->sr * 0.012)); /* 12 ms smooth controls */
    const double dc_r = 0.995;

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

        /* Smooth parameters */
        c->smooth_ms_l     += time_alpha * (target_ms_l - c->smooth_ms_l);
        c->smooth_ms_r     += time_alpha * (target_ms_r - c->smooth_ms_r);
        c->smooth_fb       += ctrl_alpha * (target_fb - c->smooth_fb);
        c->smooth_mix      += ctrl_alpha * (target_mix - c->smooth_mix);
        c->smooth_send     += ctrl_alpha * (target_send - c->smooth_send);
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

        /* Loop tone + character processing (skipped while frozen so loop stays lossless) */
        double fb_l = tap_l;
        double fb_r = tap_r;
        if (!freeze) {
            biquad_process(&c->hpf, &fb_l, &fb_r);
            biquad_process(&c->lpf, &fb_l, &fb_r);
            if (mode == ECHO_MODE_BBD) {
                biquad_process(&c->bbd_lpf, &fb_l, &fb_r);
            }
            if (ap_g > 0.01) {
                for (int s = 0; s < ECHO_AP_STAGES; ++s) {
                    fb_l = allpass_process(&c->ap_l[s], fb_l, ap_g);
                    fb_r = allpass_process(&c->ap_r[s], fb_r, ap_g);
                }
            }
            fb_l = soft_sat(fb_l, drive, mode == ECHO_MODE_TAPE);
            fb_r = soft_sat(fb_r, drive, mode == ECHO_MODE_TAPE);

            /* DC blocker in feedback loop */
            double dc_l = fb_l - c->dc_x_l + dc_r * c->dc_y_l;
            c->dc_x_l = fb_l; c->dc_y_l = dc_l; fb_l = dc_l;
            double dc_r_out = fb_r - c->dc_x_r + dc_r * c->dc_y_r;
            c->dc_x_r = fb_r; c->dc_y_r = dc_r_out; fb_r = dc_r_out;
        }

        /* Feedback routing */
        double in_send_l = xl * c->smooth_send;
        double in_send_r = xr * c->smooth_send;
        double write_l = 0.0, write_r = 0.0;
        if (routing == ECHO_ROUTE_STEREO) {
            write_l = in_send_l + fb_l * c->smooth_fb;
            write_r = in_send_r + fb_r * c->smooth_fb;
        } else if (routing == ECHO_ROUTE_PINGPONG) {
            double mono_in = 0.5 * (in_send_l + in_send_r);
            write_l = mono_in + fb_r * c->smooth_fb;
            write_r = fb_l * c->smooth_fb;
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
    c->dc_x_l = flush_denorm(c->dc_x_l); c->dc_y_l = flush_denorm(c->dc_y_l);
    c->dc_x_r = flush_denorm(c->dc_x_r); c->dc_y_r = flush_denorm(c->dc_y_r);
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
