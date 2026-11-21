/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Eka Q-4 DSP core: 64-bit TDF-II biquads, HPF/LPF, Tilt, Solo BPF, AutoGain, 24-bin spectrum. */
#include "q4_core.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

enum {
    ST_HPF1 = 0,
    ST_HPF2,
    ST_LPF1,
    ST_LPF2,
    ST_TILT_LO,
    ST_TILT_HI,
    ST_B1,
    ST_B1_PULTEC,
    ST_B2,
    ST_B3,
    ST_B4
};

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
    double db = 20.0 * log10(lin);
    return clampd(db, -60.0, 12.0);
}

static Q4BiquadCoeff coeff_identity(void) {
    Q4BiquadCoeff c = {1.0, 0.0, 0.0, 0.0, 0.0};
    return c;
}

static double clamp_freq(double f, double sr) {
    double max_f = sr * 0.46;
    if (max_f < 1000.0) max_f = 1000.0;
    return clampd(f, 15.0, max_f);
}

static Q4BiquadCoeff make_hpf(double sr, double f, double q) {
    f = clamp_freq(f, sr);
    q = clampd(q, 0.2, 20.0);
    double w0 = 2.0 * M_PI * f / sr;
    double cw = cos(w0);
    double sw = sin(w0);
    double alpha = sw / (2.0 * q);
    double a0 = 1.0 + alpha;
    double inv = 1.0 / a0;
    Q4BiquadCoeff c;
    c.b0 = ((1.0 + cw) * 0.5) * inv;
    c.b1 = (-(1.0 + cw)) * inv;
    c.b2 = ((1.0 + cw) * 0.5) * inv;
    c.a1 = (-2.0 * cw) * inv;
    c.a2 = (1.0 - alpha) * inv;
    return c;
}

static Q4BiquadCoeff make_lpf(double sr, double f, double q) {
    f = clamp_freq(f, sr);
    q = clampd(q, 0.2, 20.0);
    double w0 = 2.0 * M_PI * f / sr;
    double cw = cos(w0);
    double sw = sin(w0);
    double alpha = sw / (2.0 * q);
    double a0 = 1.0 + alpha;
    double inv = 1.0 / a0;
    Q4BiquadCoeff c;
    c.b0 = ((1.0 - cw) * 0.5) * inv;
    c.b1 = (1.0 - cw) * inv;
    c.b2 = ((1.0 - cw) * 0.5) * inv;
    c.a1 = (-2.0 * cw) * inv;
    c.a2 = (1.0 - alpha) * inv;
    return c;
}

static Q4BiquadCoeff make_peak(double sr, double f, double q, double gain_db) {
    if (fabs(gain_db) < 1e-4) return coeff_identity();
    f = clamp_freq(f, sr);
    q = clampd(q, 0.2, 20.0);
    double A = pow(10.0, gain_db / 40.0);
    double w0 = 2.0 * M_PI * f / sr;
    double cw = cos(w0);
    double sw = sin(w0);
    double alpha = sw / (2.0 * q);
    double a0 = 1.0 + alpha / A;
    double inv = 1.0 / a0;
    Q4BiquadCoeff c;
    c.b0 = (1.0 + alpha * A) * inv;
    c.b1 = (-2.0 * cw) * inv;
    c.b2 = (1.0 - alpha * A) * inv;
    c.a1 = (-2.0 * cw) * inv;
    c.a2 = (1.0 - alpha / A) * inv;
    return c;
}

static Q4BiquadCoeff make_low_shelf(double sr, double f, double q, double gain_db) {
    if (fabs(gain_db) < 1e-4) return coeff_identity();
    f = clamp_freq(f, sr);
    q = clampd(q, 0.2, 12.0);
    double A = pow(10.0, gain_db / 40.0);
    double w0 = 2.0 * M_PI * f / sr;
    double cw = cos(w0);
    double sw = sin(w0);
    double alpha = sw / (2.0 * q);
    double sq = 2.0 * sqrt(A) * alpha;
    double a0 = (A + 1.0) + (A - 1.0) * cw + sq;
    double inv = 1.0 / a0;
    Q4BiquadCoeff c;
    c.b0 = (A * ((A + 1.0) - (A - 1.0) * cw + sq)) * inv;
    c.b1 = (2.0 * A * ((A - 1.0) - (A + 1.0) * cw)) * inv;
    c.b2 = (A * ((A + 1.0) - (A - 1.0) * cw - sq)) * inv;
    c.a1 = (-2.0 * ((A - 1.0) + (A + 1.0) * cw)) * inv;
    c.a2 = ((A + 1.0) + (A - 1.0) * cw - sq) * inv;
    return c;
}

static Q4BiquadCoeff make_high_shelf(double sr, double f, double q, double gain_db) {
    if (fabs(gain_db) < 1e-4) return coeff_identity();
    f = clamp_freq(f, sr);
    q = clampd(q, 0.2, 12.0);
    double A = pow(10.0, gain_db / 40.0);
    double w0 = 2.0 * M_PI * f / sr;
    double cw = cos(w0);
    double sw = sin(w0);
    double alpha = sw / (2.0 * q);
    double sq = 2.0 * sqrt(A) * alpha;
    double a0 = (A + 1.0) - (A - 1.0) * cw + sq;
    double inv = 1.0 / a0;
    Q4BiquadCoeff c;
    c.b0 = (A * ((A + 1.0) + (A - 1.0) * cw + sq)) * inv;
    c.b1 = (-2.0 * A * ((A - 1.0) + (A + 1.0) * cw)) * inv;
    c.b2 = (A * ((A + 1.0) + (A - 1.0) * cw - sq)) * inv;
    c.a1 = (2.0 * ((A - 1.0) - (A + 1.0) * cw)) * inv;
    c.a2 = ((A + 1.0) - (A - 1.0) * cw - sq) * inv;
    return c;
}

static Q4BiquadCoeff make_notch(double sr, double f, double q) {
    f = clamp_freq(f, sr);
    q = clampd(q, 0.2, 20.0);
    double w0 = 2.0 * M_PI * f / sr;
    double cw = cos(w0);
    double sw = sin(w0);
    double alpha = sw / (2.0 * q);
    double a0 = 1.0 + alpha;
    double inv = 1.0 / a0;
    Q4BiquadCoeff c;
    c.b0 = 1.0 * inv;
    c.b1 = (-2.0 * cw) * inv;
    c.b2 = 1.0 * inv;
    c.a1 = (-2.0 * cw) * inv;
    c.a2 = (1.0 - alpha) * inv;
    return c;
}

/* Constant 0 dB peak gain bandpass filter for solo audition & spectrum analyzer. */
static Q4BiquadCoeff make_bpf_peak0(double sr, double f, double q) {
    f = clamp_freq(f, sr);
    q = clampd(q, 0.5, 20.0);
    double w0 = 2.0 * M_PI * f / sr;
    double cw = cos(w0);
    double sw = sin(w0);
    double alpha = sw / (2.0 * q);
    double a0 = 1.0 + alpha;
    double inv = 1.0 / a0;
    Q4BiquadCoeff c;
    c.b0 = alpha * inv;
    c.b1 = 0.0;
    c.b2 = -alpha * inv;
    c.a1 = (-2.0 * cw) * inv;
    c.a2 = (1.0 - alpha) * inv;
    return c;
}

static double biquad_mag_sq(const Q4BiquadCoeff *c, double w) {
    double cw = cos(w);
    double c2w = cos(2.0 * w);
    double sw = sin(w);
    double s2w = sin(2.0 * w);

    double nr = c->b0 + c->b1 * cw + c->b2 * c2w;
    double ni = -(c->b1 * sw + c->b2 * s2w);
    double dr = 1.0 + c->a1 * cw + c->a2 * c2w;
    double di = -(c->a1 * sw + c->a2 * s2w);

    double num = nr * nr + ni * ni;
    double den = dr * dr + di * di;
    if (den < 1e-24) den = 1e-24;
    return num / den;
}

static void compute_target_coeffs(const Q4Params *p, double sr, Q4BiquadCoeff out[Q4_NUM_STAGES]) {
    for (int i = 0; i < Q4_NUM_STAGES; ++i) out[i] = coeff_identity();

    double scale = clampd(p->scale_pct, 0.0, 200.0) * 0.01;

    /* HPF: bypassed at <= 20.5 Hz */
    if (p->hpf_hz > 20.5f) {
        if (p->slope_24) {
            out[ST_HPF1] = make_hpf(sr, p->hpf_hz, 0.54119610);
            out[ST_HPF2] = make_hpf(sr, p->hpf_hz, 1.30656296);
        } else {
            out[ST_HPF1] = make_hpf(sr, p->hpf_hz, 0.70710678);
        }
    }

    /* LPF: bypassed at >= 19900 Hz */
    if (p->lpf_hz < 19900.0f) {
        if (p->slope_24) {
            out[ST_LPF1] = make_lpf(sr, p->lpf_hz, 0.54119610);
            out[ST_LPF2] = make_lpf(sr, p->lpf_hz, 1.30656296);
        } else {
            out[ST_LPF1] = make_lpf(sr, p->lpf_hz, 0.70710678);
        }
    }

    /* Tilt around 1000 Hz: -tilt_db at DC, 0 dB at 1 kHz, +tilt_db at Nyquist */
    double tilt = clampd(p->tilt_db, -6.0, 6.0) * scale;
    if (fabs(tilt) > 1e-4) {
        out[ST_TILT_LO] = make_low_shelf(sr, 1000.0, 0.5, -tilt);
        out[ST_TILT_HI] = make_high_shelf(sr, 1000.0, 0.5, tilt);
    }

    /* Band 1: Low (Shelf / Bell / Pultec / Off) */
    {
        double f = clampd(p->bands[0].freq_hz, 20.0, 1000.0);
        double g = clampd(p->bands[0].gain_db, -18.0, 18.0) * scale;
        double q = clampd(p->bands[0].q, 0.2, 12.0);
        int t = p->bands[0].type;
        if (t == Q4_B1_SHELF) {
            out[ST_B1] = make_low_shelf(sr, f, q, g);
        } else if (t == Q4_B1_BELL) {
            out[ST_B1] = make_peak(sr, f, q, g);
        } else if (t == Q4_B1_PULTEC) {
            out[ST_B1] = make_low_shelf(sr, f, q, g);
            if (fabs(g) > 1e-4) {
                double dip_f = f * 1.75;
                out[ST_B1_PULTEC] = make_peak(sr, dip_f, 1.2, -0.35 * g);
            }
        }
    }

    /* Band 2: Low-Mid (Bell / Notch / Off) */
    {
        double f = clampd(p->bands[1].freq_hz, 60.0, 4000.0);
        double g = clampd(p->bands[1].gain_db, -18.0, 18.0) * scale;
        double q = clampd(p->bands[1].q, 0.2, 16.0);
        int t = p->bands[1].type;
        if (t == Q4_MID_BELL) {
            out[ST_B2] = make_peak(sr, f, q, g);
        } else if (t == Q4_MID_NOTCH && scale > 0.01) {
            out[ST_B2] = make_notch(sr, f, q);
        }
    }

    /* Band 3: High-Mid (Bell / Notch / Off) */
    {
        double f = clampd(p->bands[2].freq_hz, 250.0, 12000.0);
        double g = clampd(p->bands[2].gain_db, -18.0, 18.0) * scale;
        double q = clampd(p->bands[2].q, 0.2, 16.0);
        int t = p->bands[2].type;
        if (t == Q4_MID_BELL) {
            out[ST_B3] = make_peak(sr, f, q, g);
        } else if (t == Q4_MID_NOTCH && scale > 0.01) {
            out[ST_B3] = make_notch(sr, f, q);
        }
    }

    /* Band 4: High (Shelf / Bell / Air / Off) */
    {
        double f = clampd(p->bands[3].freq_hz, 800.0, 20000.0);
        double g = clampd(p->bands[3].gain_db, -18.0, 18.0) * scale;
        double q = clampd(p->bands[3].q, 0.2, 12.0);
        int t = p->bands[3].type;
        if (t == Q4_B4_SHELF) {
            out[ST_B4] = make_high_shelf(sr, f, q, g);
        } else if (t == Q4_B4_BELL) {
            out[ST_B4] = make_peak(sr, f, q, g);
        } else if (t == Q4_B4_AIR) {
            double air_f = f * 1.15;
            if (air_f > 19500.0) air_f = 19500.0;
            out[ST_B4] = make_high_shelf(sr, air_f, 0.55, g);
        }
    }
}

static double eval_auto_gain_db(const Q4BiquadCoeff coeffs[Q4_NUM_STAGES], double sr) {
    /* Average log-spaced energy across 12 perceptual frequencies (70 Hz .. 11 kHz). */
    static const double ref_f[12] = {
        70.0, 110.0, 180.0, 290.0, 470.0, 760.0,
        1200.0, 1950.0, 3100.0, 5000.0, 7800.0, 11000.0
    };
    double sum_db = 0.0;
    for (int i = 0; i < 12; ++i) {
        double w = 2.0 * M_PI * ref_f[i] / sr;
        if (w >= M_PI * 0.95) continue;
        double m2 = 1.0;
        /* Compensate EQ bands + tilt, not intentional HPF/LPF cuts. */
        for (int s = ST_TILT_LO; s < Q4_NUM_STAGES; ++s) {
            m2 *= biquad_mag_sq(&coeffs[s], w);
        }
        if (m2 < 1e-6) m2 = 1e-6;
        sum_db += 10.0 * log10(m2);
    }
    double avg_db = sum_db / 12.0;
    return clampd(-avg_db * 0.85, -12.0, 12.0);
}

static inline void stage_process_stereo(Q4BiquadStage *st, double *l, double *r, double alpha) {
    st->cur.b0 += alpha * (st->target.b0 - st->cur.b0);
    st->cur.b1 += alpha * (st->target.b1 - st->cur.b1);
    st->cur.b2 += alpha * (st->target.b2 - st->cur.b2);
    st->cur.a1 += alpha * (st->target.a1 - st->cur.a1);
    st->cur.a2 += alpha * (st->target.a2 - st->cur.a2);

    double xl = *l;
    double yl = st->cur.b0 * xl + st->z1_l;
    st->z1_l = st->cur.b1 * xl - st->cur.a1 * yl + st->z2_l;
    st->z2_l = st->cur.b2 * xl - st->cur.a2 * yl;

    double xr = *r;
    double yr = st->cur.b0 * xr + st->z1_r;
    st->z1_r = st->cur.b1 * xr - st->cur.a1 * yr + st->z2_r;
    st->z2_r = st->cur.b2 * xr - st->cur.a2 * yr;

    *l = yl;
    *r = yr;
}

void q4_params_default(Q4Params *p) {
    if (!p) return;
    memset(p, 0, sizeof(*p));
    p->bands[0].freq_hz = 100.0f;  p->bands[0].gain_db = 0.0f; p->bands[0].q = 0.71f; p->bands[0].type = Q4_B1_SHELF;
    p->bands[1].freq_hz = 400.0f;  p->bands[1].gain_db = 0.0f; p->bands[1].q = 1.00f; p->bands[1].type = Q4_MID_BELL;
    p->bands[2].freq_hz = 2200.0f; p->bands[2].gain_db = 0.0f; p->bands[2].q = 1.00f; p->bands[2].type = Q4_MID_BELL;
    p->bands[3].freq_hz = 8000.0f; p->bands[3].gain_db = 0.0f; p->bands[3].q = 0.71f; p->bands[3].type = Q4_B4_SHELF;
    p->hpf_hz = 20.0f;
    p->lpf_hz = 20000.0f;
    p->tilt_db = 0.0f;
    p->slope_24 = 0;
    p->scale_pct = 100.0f;
    p->solo_band = 0;
    p->auto_gain = 0;
    p->output_db = 0.0f;
    p->bypass = 0;
}

void q4_core_init(Q4Core *c, double sr) {
    if (!c) return;
    memset(c, 0, sizeof(*c));
    c->sr = (sr > 8000.0 && sr < 384000.0) ? sr : 48000.0;

    for (int i = 0; i < Q4_NUM_STAGES; ++i) {
        c->stages[i].cur = coeff_identity();
        c->stages[i].target = coeff_identity();
    }
    c->solo_stage.cur = make_bpf_peak0(c->sr, 1000.0, 2.0);
    c->solo_stage.target = c->solo_stage.cur;

    c->cur_gain = 1.0;
    c->cur_solo_mix = 0.0;
    c->cur_bypass_mix = 0.0;

    /* 24 log-spaced spectrum analyzer bins from 28 Hz to 16 kHz. */
    const double f_min = 28.0;
    const double f_max = 16000.0;
    const double ratio = pow(f_max / f_min, 1.0 / (double)(Q4_SPEC_BINS - 1));
    for (int i = 0; i < Q4_SPEC_BINS; ++i) {
        double fc = f_min * pow(ratio, (double)i);
        c->spec[i].coeff = make_bpf_peak0(c->sr, fc, 4.2);
        c->spec[i].z1 = 0.0;
        c->spec[i].z2 = 0.0;
        c->spec[i].env = 0.0;
        c->meters.spec_db[i] = -60.0f;
    }

    c->spec_att = 1.0 - exp(-1.0 / (c->sr * 0.012));
    c->spec_rel = 1.0 - exp(-1.0 / (c->sr * 0.140));
    c->peak_rel = exp(-1.0 / (c->sr * 0.180));
    c->in_peak_lin = 0.0;
    c->out_peak_lin = 0.0;
    c->meters.in_peak_db = -60.0f;
    c->meters.out_peak_db = -60.0f;
}

void q4_core_reset(Q4Core *c) {
    if (!c) return;
    for (int i = 0; i < Q4_NUM_STAGES; ++i) {
        c->stages[i].z1_l = c->stages[i].z2_l = 0.0;
        c->stages[i].z1_r = c->stages[i].z2_r = 0.0;
    }
    c->solo_stage.z1_l = c->solo_stage.z2_l = 0.0;
    c->solo_stage.z1_r = c->solo_stage.z2_r = 0.0;
    for (int i = 0; i < Q4_SPEC_BINS; ++i) {
        c->spec[i].z1 = c->spec[i].z2 = 0.0;
        c->spec[i].env = 0.0;
        c->meters.spec_db[i] = -60.0f;
    }
    c->in_peak_lin = 0.0;
    c->out_peak_lin = 0.0;
    c->meters.in_peak_db = -60.0f;
    c->meters.out_peak_db = -60.0f;
}

void q4_core_process(Q4Core *c, const Q4Params *p,
                     const float *in_l, const float *in_r,
                     float *out_l, float *out_r,
                     uint32_t nframes) {
    if (!c || !p || !in_l || !in_r || !out_l || !out_r || nframes == 0) return;

    Q4BiquadCoeff targets[Q4_NUM_STAGES];
    compute_target_coeffs(p, c->sr, targets);
    for (int s = 0; s < Q4_NUM_STAGES; ++s) {
        c->stages[s].target = targets[s];
    }

    int solo_idx = p->solo_band;
    double target_solo = 0.0;
    if (solo_idx >= 1 && solo_idx <= Q4_BANDS) {
        const Q4BandParams *bp = &p->bands[solo_idx - 1];
        double sq = clampd(bp->q, 0.8, 12.0);
        c->solo_stage.target = make_bpf_peak0(c->sr, bp->freq_hz, sq);
        target_solo = 1.0;
    }

    double out_db = clampd(p->output_db, -18.0, 18.0);
    if (p->auto_gain) {
        out_db += eval_auto_gain_db(targets, c->sr);
    }
    double target_gain = db_to_lin(out_db);

    const double coeff_alpha = 1.0 - exp(-1.0 / (c->sr * 0.008)); /* 8 ms smooth */
    const double gain_alpha  = 1.0 - exp(-1.0 / (c->sr * 0.015)); /* 15 ms smooth */
    const double solo_alpha  = 1.0 - exp(-1.0 / (c->sr * 0.010)); /* 10 ms crossfade */
    const double target_byp  = p->bypass ? 1.0 : 0.0;

    double in_pk = c->in_peak_lin;
    double out_pk = c->out_peak_lin;

    for (uint32_t i = 0; i < nframes; ++i) {
        double xl = isfinite(in_l[i]) ? (double)in_l[i] : 0.0;
        double xr = isfinite(in_r[i]) ? (double)in_r[i] : 0.0;

        double abs_in = fmax(fabs(xl), fabs(xr));
        in_pk = (abs_in > in_pk) ? abs_in : in_pk * c->peak_rel;

        double yl = xl;
        double yr = xr;
        for (int s = 0; s < Q4_NUM_STAGES; ++s) {
            stage_process_stereo(&c->stages[s], &yl, &yr, coeff_alpha);
        }

        c->cur_gain += gain_alpha * (target_gain - c->cur_gain);
        yl *= c->cur_gain;
        yr *= c->cur_gain;

        /* Solo audition bandpass (crossfaded smoothly) */
        c->cur_solo_mix += solo_alpha * (target_solo - c->cur_solo_mix);
        if (c->cur_solo_mix > 1e-4) {
            double sl = xl;
            double sr_s = xr;
            stage_process_stereo(&c->solo_stage, &sl, &sr_s, coeff_alpha);
            double sm = c->cur_solo_mix;
            yl = yl * (1.0 - sm) + sl * sm;
            yr = yr * (1.0 - sm) + sr_s * sm;
        }

        /* True click-free bypass crossfade */
        c->cur_bypass_mix += solo_alpha * (target_byp - c->cur_bypass_mix);
        if (c->cur_bypass_mix > 1e-5) {
            double bm = c->cur_bypass_mix;
            yl = yl * (1.0 - bm) + xl * bm;
            yr = yr * (1.0 - bm) + xr * bm;
        }

        double abs_out = fmax(fabs(yl), fabs(yr));
        out_pk = (abs_out > out_pk) ? abs_out : out_pk * c->peak_rel;

        /* Feed post-EQ mono signal to 24-bin spectrum analyzer */
        double mono = 0.5 * (yl + yr);
        for (int b = 0; b < Q4_SPEC_BINS; ++b) {
            Q4SpecBin *sb = &c->spec[b];
            double y = sb->coeff.b0 * mono + sb->z1;
            sb->z1 = sb->coeff.b1 * mono - sb->coeff.a1 * y + sb->z2;
            sb->z2 = sb->coeff.b2 * mono - sb->coeff.a2 * y;
            double a = fabs(y) * 1.41421356; /* peak-normalized for sine */
            double k = (a > sb->env) ? c->spec_att : c->spec_rel;
            sb->env += k * (a - sb->env);
        }

        out_l[i] = isfinite(yl) ? (float)yl : 0.0f;
        out_r[i] = isfinite(yr) ? (float)yr : 0.0f;
    }

    /* Flush denormals once per block */
    for (int s = 0; s < Q4_NUM_STAGES; ++s) {
        c->stages[s].z1_l = flush_denorm(c->stages[s].z1_l);
        c->stages[s].z2_l = flush_denorm(c->stages[s].z2_l);
        c->stages[s].z1_r = flush_denorm(c->stages[s].z1_r);
        c->stages[s].z2_r = flush_denorm(c->stages[s].z2_r);
    }
    c->solo_stage.z1_l = flush_denorm(c->solo_stage.z1_l);
    c->solo_stage.z2_l = flush_denorm(c->solo_stage.z2_l);
    c->solo_stage.z1_r = flush_denorm(c->solo_stage.z1_r);
    c->solo_stage.z2_r = flush_denorm(c->solo_stage.z2_r);

    c->in_peak_lin = flush_denorm(in_pk);
    c->out_peak_lin = flush_denorm(out_pk);
    c->meters.in_peak_db = (float)lin_to_db(c->in_peak_lin);
    c->meters.out_peak_db = (float)lin_to_db(c->out_peak_lin);

    for (int b = 0; b < Q4_SPEC_BINS; ++b) {
        c->spec[b].z1 = flush_denorm(c->spec[b].z1);
        c->spec[b].z2 = flush_denorm(c->spec[b].z2);
        c->spec[b].env = flush_denorm(c->spec[b].env);
        c->meters.spec_db[b] = (float)clampd(lin_to_db(c->spec[b].env), -60.0, 0.0);
    }
}

double q4_eval_response_db(const Q4Params *p, double sr, double f_hz) {
    if (!p || sr <= 0.0 || f_hz <= 0.0) return 0.0;
    Q4BiquadCoeff coeffs[Q4_NUM_STAGES];
    compute_target_coeffs(p, sr, coeffs);
    double w = 2.0 * M_PI * f_hz / sr;
    double m2 = 1.0;
    for (int s = 0; s < Q4_NUM_STAGES; ++s) {
        m2 *= biquad_mag_sq(&coeffs[s], w);
    }
    if (m2 < 1e-12) m2 = 1e-12;
    return 10.0 * log10(m2);
}
