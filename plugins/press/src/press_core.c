/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Eka Press: single-band live compressor core (pure C11, RT-safe). */
#include "press_core.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static inline float clampf(float x, float lo, float hi) {
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

static inline float sanitizef(float x) {
    if (!isfinite(x) || fabsf(x) < 1e-24f) return 0.0f;
    return x;
}

static inline float lin_to_db(float x) {
    if (x <= 1e-6f) return -120.0f;
    return 20.0f * log10f(x);
}

static inline float db_to_lin(float db) {
    /* 10^(db/20) == exp(db * ln(10)/20); avoids expensive general powf() */
    return expf(db * 0.11512925465f);
}

static inline float time_coeff(float ms, float sr) {
    if (ms <= 0.01f) return 0.0f;
    return expf(-1.0f / (ms * 0.001f * sr));
}

static void biquad_set_hpf(PressBiquad *b, float freq_hz, float sr) {
    float f = clampf(freq_hz, 20.0f, sr * 0.45f);
    float w0 = 2.0f * (float)M_PI * f / sr;
    float cs = cosf(w0);
    float sn = sinf(w0);
    float alpha = sn / 1.41421356237f; /* Butterworth Q = 0.7071 */
    float a0 = 1.0f + alpha;
    float inv_a0 = 1.0f / a0;

    b->b0 = ((1.0f + cs) * 0.5f) * inv_a0;
    b->b1 = -(1.0f + cs) * inv_a0;
    b->b2 = ((1.0f + cs) * 0.5f) * inv_a0;
    b->a1 = (-2.0f * cs) * inv_a0;
    b->a2 = (1.0f - alpha) * inv_a0;
}

static inline float biquad_run(PressBiquad *b, float x) {
    float y = b->b0 * x + b->z1;
    b->z1 = sanitizef(b->b1 * x - b->a1 * y + b->z2);
    b->z2 = sanitizef(b->b2 * x - b->a2 * y);
    return y;
}

void press_params_default(PressParams *p) {
    memset(p, 0, sizeof(*p));
    p->threshold_db = -18.0f;
    p->ratio        = 4.0f;
    p->attack_ms    = 10.0f;
    p->release_ms   = 150.0f;
    p->knee_db      = 6.0f;
    p->mode         = PRESS_MODE_CLEAN;

    p->sc_hpf_hz    = 20.0f;
    p->stereo_link  = 100.0f;
    p->auto_release = 0;
    p->sc_listen    = 0;

    p->drive_pct    = 0.0f;
    p->makeup_db    = 0.0f;
    p->auto_makeup  = 0;
    p->mix_pct      = 100.0f;
    p->soft_clip    = 1;
    p->output_db    = 0.0f;
    p->bypass       = 0;
}

void press_core_reset(PressCore *c) {
    c->hpf_l.z1 = c->hpf_l.z2 = 0.0f;
    c->hpf_r.z1 = c->hpf_r.z2 = 0.0f;
    c->rms_l = c->rms_r = 0.0f;
    c->env_l = c->env_r = 0.0f;
    c->slow_l = c->slow_r = 0.0f;
    c->dc_x_l = c->dc_y_l = 0.0f;
    c->dc_x_r = c->dc_y_r = 0.0f;
    c->sm_init = 0;
    c->meters.in_peak_db  = -60.0f;
    c->meters.out_peak_db = -60.0f;
    c->meters.sc_peak_db  = -60.0f;
    c->meters.gr_db       = 0.0f;
}

void press_core_init(PressCore *c, float sample_rate) {
    memset(c, 0, sizeof(*c));
    c->sample_rate = (sample_rate >= 8000.0f) ? sample_rate : 48000.0f;
    c->inv_sr = 1.0f / c->sample_rate;
    c->last_hpf_hz = 20.0f;
    biquad_set_hpf(&c->hpf_l, 20.0f, c->sample_rate);
    biquad_set_hpf(&c->hpf_r, 20.0f, c->sample_rate);
    press_core_reset(c);
}

float press_compute_gr_db(float level_db, float threshold_db, float ratio, float knee_db) {
    float r = clampf(ratio, 1.0f, 20.0f);
    float inv_r = (r >= 19.9f) ? 0.0f : (1.0f / r);
    float slope = 1.0f - inv_r; /* 0..1 */
    if (slope <= 1e-6f) return 0.0f;

    float over = level_db - threshold_db;
    float w = clampf(knee_db, 0.0f, 30.0f);

    if (w <= 0.05f) {
        return (over > 0.0f) ? (slope * over) : 0.0f;
    }

    float half_w = 0.5f * w;
    if (over <= -half_w) {
        return 0.0f;
    } else if (over >= half_w) {
        return slope * over;
    } else {
        float x = over + half_w;
        return slope * (x * x) / (2.0f * w);
    }
}

/* Smooth soft-clipping ceiling at -0.3 dBFS (~0.966). */
static inline float soft_clip_sample(float x) {
    const float ceil_lin = 0.96605f; /* -0.3 dBFS */
    const float knee_lin = 0.74989f; /* -2.5 dBFS */
    float ax = fabsf(x);
    if (ax <= knee_lin) return x;
    float span = ceil_lin - knee_lin;
    float excess = (ax - knee_lin) / span;
    float shaped = knee_lin + span * tanhf(excess);
    return (x >= 0.0f) ? shaped : -shaped;
}

/* Warm analog-style saturation with mild even-harmonic asymmetry and level compensation. */
static inline float apply_drive(float x, float drive, float *dc_x, float *dc_y, float dc_r) {
    if (drive <= 1e-4f) return x;
    /* Pre-gain 1x .. 3.8x */
    float pre = 1.0f + drive * 2.8f;
    float bias = 0.10f * drive;
    float tb = tanhf(bias);
    float s = tanhf(x * pre + bias) - tb;
    /* Compensate small-signal slope: d/dx tanh(pre*x + bias) at 0 = pre * sech^2(bias) */
    float norm = 1.0f / (pre * (1.0f - tb * tb));
    float sat = s * norm;
    /* Mild loudness compensation as signal compresses into saturation */
    float wet = sat * (1.0f + 0.22f * drive);
    /* 1st-order DC blocker */
    float y = wet - (*dc_x) + dc_r * (*dc_y);
    *dc_x = sanitizef(wet);
    *dc_y = sanitizef(y);
    return x + drive * (y - x);
}

void press_core_process(PressCore *c, const PressParams *p,
                        const float *in_l, const float *in_r,
                        float *out_l, float *out_r,
                        uint32_t nframes) {
    if (!c || !p || !in_l || !in_r || !out_l || !out_r || nframes == 0) return;

    const float sr = c->sample_rate;
    const float thresh = clampf(p->threshold_db, -60.0f, 0.0f);
    const float ratio  = clampf(p->ratio, 1.0f, 20.0f);
    const float atk_ms = clampf(p->attack_ms, 0.1f, 100.0f);
    const float rel_ms = clampf(p->release_ms, 10.0f, 1500.0f);
    const float knee   = clampf(p->knee_db, 0.0f, 18.0f);
    const int   mode   = (p->mode >= 0 && p->mode < PRESS_MODE_COUNT) ? p->mode : PRESS_MODE_CLEAN;

    const float hpf_hz   = clampf(p->sc_hpf_hz, 20.0f, 500.0f);
    const float link     = clampf(p->stereo_link, 0.0f, 100.0f) * 0.01f;
    const int   auto_rel = p->auto_release ? 1 : 0;
    const float sc_lstn_t = p->sc_listen ? 1.0f : 0.0f;

    const float drive_t  = clampf(p->drive_pct, 0.0f, 100.0f) * 0.01f;
    const float mix_t    = clampf(p->mix_pct, 0.0f, 100.0f) * 0.01f;
    const int   sclip    = p->soft_clip ? 1 : 0;
    const float out_t    = db_to_lin(clampf(p->output_db, -24.0f, 12.0f));
    const float bypass_t = p->bypass ? 1.0f : 0.0f;

    /* Update sidechain HPF coefficients if changed */
    if (fabsf(hpf_hz - c->last_hpf_hz) > 0.25f) {
        c->last_hpf_hz = hpf_hz;
        biquad_set_hpf(&c->hpf_l, hpf_hz, sr);
        biquad_set_hpf(&c->hpf_r, hpf_hz, sr);
    }
    const float hpf_mix_t = (hpf_hz > 20.5f) ? 1.0f : 0.0f;

    /* Auto makeup: conservative estimate based on threshold & ratio */
    float auto_mk_db = 0.0f;
    if (p->auto_makeup) {
        float inv_r = (ratio >= 19.9f) ? 0.0f : (1.0f / ratio);
        float slope = 1.0f - inv_r;
        auto_mk_db = clampf(-0.42f * thresh * slope, 0.0f, 18.0f);
    }
    const float makeup_t = db_to_lin(clampf(p->makeup_db + auto_mk_db, -24.0f, 36.0f));

    /* Initialize smoothers on first block to avoid startup ramp */
    if (!c->sm_init) {
        c->sm_makeup_lin = makeup_t;
        c->sm_output_lin = out_t;
        c->sm_mix        = mix_t;
        c->sm_drive      = drive_t;
        c->sm_sc_listen  = sc_lstn_t;
        c->sm_hpf_mix    = hpf_mix_t;
        c->sm_bypass     = bypass_t;
        c->sm_init       = 1;
    }

    /* Mode-specific detector & ballistic tuning */
    float rms_weight = 0.30f;
    float rms_ms     = 10.0f;
    float eff_atk_ms = atk_ms;
    float eff_rel_ms = rel_ms;
    float eff_knee   = knee;
    float fet_color  = 0.0f;

    switch (mode) {
        case PRESS_MODE_CLEAN:
            rms_weight = 0.30f;
            rms_ms     = 8.0f;
            break;
        case PRESS_MODE_OPTO:
            rms_weight = 0.80f;
            rms_ms     = 22.0f;
            eff_atk_ms = 6.0f + atk_ms * 1.15f;
            eff_rel_ms = rel_ms * 1.25f;
            eff_knee   = clampf(knee + 6.0f, 0.0f, 24.0f);
            break;
        case PRESS_MODE_PUNCH:
            rms_weight = 0.05f;
            rms_ms     = 4.0f;
            eff_atk_ms = clampf(atk_ms * 0.55f, 0.08f, 100.0f);
            eff_rel_ms = clampf(rel_ms * 0.75f, 8.0f, 1500.0f);
            eff_knee   = knee * 0.65f;
            fet_color  = 0.14f;
            break;
        case PRESS_MODE_GLUE:
            rms_weight = 0.55f;
            rms_ms     = 15.0f;
            eff_atk_ms = clampf(atk_ms * 1.2f, 0.2f, 120.0f);
            eff_rel_ms = rel_ms * 1.15f;
            eff_knee   = clampf(knee + 3.0f, 0.0f, 21.0f);
            break;
    }

    const float rms_c    = time_coeff(rms_ms, sr);
    const float atk_c    = time_coeff(eff_atk_ms, sr);
    const float rel_c    = time_coeff(eff_rel_ms, sr);
    const float fast_rel = time_coeff(clampf(eff_rel_ms * 0.28f, 15.0f, 120.0f), sr);
    const float slow_atk = time_coeff(80.0f, sr);
    const float slow_rel = time_coeff(clampf(eff_rel_ms * 2.5f, 250.0f, 3000.0f), sr);
    const float sm_c     = 1.0f - time_coeff(8.0f, sr);
    const float dc_r     = 1.0f - (2.0f * (float)M_PI * 10.0f * c->inv_sr);

    float in_pk  = 0.0f;
    float out_pk = 0.0f;
    float sc_pk  = 0.0f;
    float gr_pk  = 0.0f;

    for (uint32_t i = 0; i < nframes; ++i) {
        /* Smooth parameters (~8 ms) */
        c->sm_makeup_lin += sm_c * (makeup_t  - c->sm_makeup_lin);
        c->sm_output_lin += sm_c * (out_t     - c->sm_output_lin);
        c->sm_mix        += sm_c * (mix_t     - c->sm_mix);
        c->sm_drive      += sm_c * (drive_t   - c->sm_drive);
        c->sm_sc_listen  += sm_c * (sc_lstn_t - c->sm_sc_listen);
        c->sm_hpf_mix    += sm_c * (hpf_mix_t - c->sm_hpf_mix);
        c->sm_bypass     += sm_c * (bypass_t  - c->sm_bypass);

        float xl = sanitizef(in_l[i]);
        float xr = sanitizef(in_r[i]);

        float al = fabsf(xl);
        float ar = fabsf(xr);
        float ip = (al > ar) ? al : ar;
        if (ip > in_pk) in_pk = ip;

        /* Sidechain HPF */
        float fl = biquad_run(&c->hpf_l, xl);
        float fr = biquad_run(&c->hpf_r, xr);
        float scl = xl + c->sm_hpf_mix * (fl - xl);
        float scr = xr + c->sm_hpf_mix * (fr - xr);

        float ascl = fabsf(scl);
        float ascr = fabsf(scr);
        float sp = (ascl > ascr) ? ascl : ascr;
        if (sp > sc_pk) sc_pk = sp;

        /* Hybrid Peak + RMS detector */
        c->rms_l = sanitizef(rms_c * c->rms_l + (1.0f - rms_c) * (scl * scl));
        c->rms_r = sanitizef(rms_c * c->rms_r + (1.0f - rms_c) * (scr * scr));

        float det_l = (1.0f - rms_weight) * ascl + rms_weight * sqrtf(c->rms_l * 2.0f);
        float det_r = (1.0f - rms_weight) * ascr + rms_weight * sqrtf(c->rms_r * 2.0f);

        /* Stereo link */
        float det_max = (det_l > det_r) ? det_l : det_r;
        float lvl_l = det_l + link * (det_max - det_l);
        float lvl_r = det_r + link * (det_max - det_r);

        /* Target gain reduction (positive dB) */
        float gr_t_l = press_compute_gr_db(lin_to_db(lvl_l), thresh, ratio, eff_knee);
        float gr_t_r = press_compute_gr_db(lin_to_db(lvl_r), thresh, ratio, eff_knee);

        /* Slow memory for Opto program-dependent release or Auto Release */
        float s_c_l = (gr_t_l > c->slow_l) ? slow_atk : slow_rel;
        float s_c_r = (gr_t_r > c->slow_r) ? slow_atk : slow_rel;
        c->slow_l = sanitizef(s_c_l * c->slow_l + (1.0f - s_c_l) * gr_t_l);
        c->slow_r = sanitizef(s_c_r * c->slow_r + (1.0f - s_c_r) * gr_t_r);

        /* Envelope ballistics L */
        if (gr_t_l > c->env_l) {
            c->env_l = atk_c * c->env_l + (1.0f - atk_c) * gr_t_l;
        } else {
            float rc = rel_c;
            if (auto_rel || mode == PRESS_MODE_OPTO) {
                float sustain = clampf(c->slow_l / 12.0f, 0.0f, 1.0f);
                rc = fast_rel + sustain * (slow_rel - fast_rel);
                if (!auto_rel && mode == PRESS_MODE_OPTO) {
                    /* Blend optical tail with user release */
                    rc = 0.5f * (rel_c + rc);
                }
            }
            c->env_l = sanitizef(rc * c->env_l + (1.0f - rc) * gr_t_l);
        }

        /* Envelope ballistics R */
        if (gr_t_r > c->env_r) {
            c->env_r = atk_c * c->env_r + (1.0f - atk_c) * gr_t_r;
        } else {
            float rc = rel_c;
            if (auto_rel || mode == PRESS_MODE_OPTO) {
                float sustain = clampf(c->slow_r / 12.0f, 0.0f, 1.0f);
                rc = fast_rel + sustain * (slow_rel - fast_rel);
                if (!auto_rel && mode == PRESS_MODE_OPTO) {
                    rc = 0.5f * (rel_c + rc);
                }
            }
            c->env_r = sanitizef(rc * c->env_r + (1.0f - rc) * gr_t_r);
        }

        float gr_now = (c->env_l > c->env_r) ? c->env_l : c->env_r;
        float gr_meter = (p->bypass && c->sm_bypass >= 0.9999f)
                             ? 0.0f
                             : gr_now * (1.0f - c->sm_bypass);
        if (gr_meter > gr_pk) gr_pk = gr_meter;

        /* Apply gain reduction + makeup */
        float gl = db_to_lin(-c->env_l) * c->sm_makeup_lin;
        float gr = db_to_lin(-c->env_r) * c->sm_makeup_lin;

        float wl = xl * gl;
        float wr = xr * gr;

        /* Subtle FET harmonic density proportional to gain reduction in Punch mode */
        float eff_drive = c->sm_drive;
        if (fet_color > 0.0f) {
            eff_drive = clampf(eff_drive + fet_color * clampf(gr_now / 12.0f, 0.0f, 1.0f), 0.0f, 1.0f);
        }

        if (eff_drive > 1e-4f) {
            wl = apply_drive(wl, eff_drive, &c->dc_x_l, &c->dc_y_l, dc_r);
            wr = apply_drive(wr, eff_drive, &c->dc_x_r, &c->dc_y_r, dc_r);
        }

        /* Parallel mix (Dry/Wet) */
        float yl = xl + c->sm_mix * (wl - xl);
        float yr = xr + c->sm_mix * (wr - xr);

        /* Output trim */
        yl *= c->sm_output_lin;
        yr *= c->sm_output_lin;

        /* Sidechain audition crossfade */
        if (c->sm_sc_listen > 1e-4f) {
            yl = yl + c->sm_sc_listen * (scl - yl);
            yr = yr + c->sm_sc_listen * (scr - yr);
        }

        /* Soft clip ceiling */
        if (sclip) {
            yl = soft_clip_sample(yl);
            yr = soft_clip_sample(yr);
        }

        /* Clickless true bypass crossfade */
        if (p->bypass && c->sm_bypass >= 0.9999f) {
            yl = xl;
            yr = xr;
        } else if (c->sm_bypass > 1e-4f) {
            yl = yl + c->sm_bypass * (xl - yl);
            yr = yr + c->sm_bypass * (xr - yr);
        }

        yl = sanitizef(yl);
        yr = sanitizef(yr);
        out_l[i] = yl;
        out_r[i] = yr;

        float ayl = fabsf(yl);
        float ayr = fabsf(yr);
        float op = (ayl > ayr) ? ayl : ayr;
        if (op > out_pk) out_pk = op;
    }

    c->meters.in_peak_db  = clampf(lin_to_db(in_pk),  -60.0f, 12.0f);
    c->meters.out_peak_db = clampf(lin_to_db(out_pk), -60.0f, 12.0f);
    c->meters.sc_peak_db  = clampf(lin_to_db(sc_pk),  -60.0f, 12.0f);
    c->meters.gr_db       = clampf(gr_pk, 0.0f, 36.0f);
}
