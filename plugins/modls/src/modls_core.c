/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Eka Modls: 6-engine musical-to-extreme "Uber Mod" modulation DSP core. */
#include "modls_core.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MODLS_TWO_PI (2.0f * (float)M_PI)
#define MODLS_FLOOR_DB (-60.0f)

static inline float clampf(float x, float lo, float hi) {
    if (!isfinite(x)) return lo;
    return (x < lo) ? lo : ((x > hi) ? hi : x);
}

static inline float flush_denorm(float x) {
    if (!isfinite(x) || fabsf(x) < 1e-20f) return 0.0f;
    return x;
}

static inline float db_to_lin(float db) {
    return powf(10.0f, db * 0.05f);
}

static inline float lin_to_db(float lin) {
    if (lin <= 1e-6f) return MODLS_FLOOR_DB;
    float db = 20.0f * log10f(lin);
    return clampf(db, MODLS_FLOOR_DB, 12.0f);
}

static uint32_t next_pow2(uint32_t v) {
    uint32_t p = 1;
    while (p < v && p < (1u << 24)) p <<= 1;
    return p;
}

static int delay_alloc(ModlsDelayLine *dl, uint32_t min_samples) {
    uint32_t len = next_pow2(min_samples < 64 ? 64 : min_samples);
    dl->buf = (float *)calloc(len, sizeof(float));
    if (!dl->buf) return -1;
    dl->len = len;
    dl->mask = len - 1;
    dl->wpos = 0;
    return 0;
}

static void delay_clear(ModlsDelayLine *dl) {
    if (dl->buf && dl->len > 0) {
        memset(dl->buf, 0, dl->len * sizeof(float));
    }
    dl->wpos = 0;
}

static void delay_free(ModlsDelayLine *dl) {
    free(dl->buf);
    dl->buf = NULL;
    dl->len = 0;
    dl->mask = 0;
    dl->wpos = 0;
}

static inline void delay_write(ModlsDelayLine *dl, float x) {
    dl->buf[dl->wpos] = flush_denorm(x);
    dl->wpos = (dl->wpos + 1u) & dl->mask;
}

/* 4-point cubic Hermite fractional delay read (delay_samps >= 1.0f). */
static inline float delay_read_hermite(const ModlsDelayLine *dl, float delay_samps) {
    float max_d = (float)(dl->len - 4u);
    if (delay_samps < 1.0f) delay_samps = 1.0f;
    if (delay_samps > max_d) delay_samps = max_d;

    uint32_t id = (uint32_t)delay_samps;
    float frac = delay_samps - (float)id;
    /* wpos points to next write slot; most recent written sample is wpos - 1 */
    uint32_t idx0 = (dl->wpos - 1u - id) & dl->mask;
    uint32_t im1  = (idx0 + 1u) & dl->mask;
    uint32_t ip1  = (idx0 - 1u) & dl->mask;
    uint32_t ip2  = (idx0 - 2u) & dl->mask;

    float ym1 = dl->buf[im1];
    float y0  = dl->buf[idx0];
    float y1  = dl->buf[ip1];
    float y2  = dl->buf[ip2];

    float c0 = y0;
    float c1 = 0.5f * (y1 - ym1);
    float c2 = ym1 - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
    float c3 = 0.5f * (y2 - ym1) + 1.5f * (y0 - y1);
    return ((c3 * frac + c2) * frac + c1) * frac + c0;
}

/* Simple linear fractional read for internal diffuser modulation. */
static inline float delay_read_lin(const ModlsDelayLine *dl, float delay_samps) {
    float max_d = (float)(dl->len - 3u);
    if (delay_samps < 1.0f) delay_samps = 1.0f;
    if (delay_samps > max_d) delay_samps = max_d;
    uint32_t id = (uint32_t)delay_samps;
    float frac = delay_samps - (float)id;
    uint32_t i0 = (dl->wpos - 1u - id) & dl->mask;
    uint32_t i1 = (i0 - 1u) & dl->mask;
    return dl->buf[i0] + frac * (dl->buf[i1] - dl->buf[i0]);
}

/* Exact bilinear 1st-order all-pass stage: H(z) = (-a + z^-1) / (1 - a z^-1). */
static inline float ap1_coeff(float fc_hz, float sr) {
    float f = clampf(fc_hz, 25.0f, sr * 0.44f);
    float k = tanf((float)M_PI * f / sr);
    float a = (1.0f - k) / (1.0f + k);
    return clampf(a, -0.992f, 0.992f);
}

static inline float ap1_process(ModlsAP1 *s, float x, float a) {
    float y = -a * x + s->z1;
    s->z1 = flush_denorm(x + a * y);
    return y;
}

static inline float onepole_lp(ModlsOnePole *s, float x, float a) {
    s->z1 = flush_denorm(s->z1 + a * (x - s->z1));
    return s->z1;
}

static inline float onepole_hp(ModlsOnePole *s, float x, float a) {
    s->z1 = flush_denorm(s->z1 + a * (x - s->z1));
    return x - s->z1;
}

static inline float dc_block_process(ModlsDCBlock *s, float x, float r) {
    float y = x - s->x1 + r * s->y1;
    s->x1 = x;
    s->y1 = flush_denorm(y);
    return y;
}

static inline float soft_drive(float x, float drive_amt) {
    if (drive_amt <= 0.001f) {
        /* Gentle safety ceiling even at 0% drive */
        return (fabsf(x) > 1.2f) ? 1.5f * tanhf(x / 1.5f) : x;
    }
    float g = 1.0f + 4.5f * drive_amt * drive_amt;
    /* Subtle even-harmonic asymmetry at higher drive */
    float bias = 0.06f * drive_amt;
    float y = tanhf(g * (x + bias)) - tanhf(g * bias);
    float comp = 1.0f / (0.65f + 0.35f * g);
    return y * comp * 1.15f;
}

static inline float safety_limit(float x) {
    if (!isfinite(x)) return 0.0f;
    if (fabsf(x) <= 1.25f) return x;
    return 1.6f * tanhf(x / 1.6f);
}

static inline float rng_next_bipolar(uint32_t *st) {
    uint32_t x = *st;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *st = x;
    return ((float)(x & 0x00FFFFFFu) / 8388608.0f) - 1.0f;
}

/* Tempo-synced division snapping matching the widget (4 bars .. 1/64). */
static const float SYNC_BEATS[] = {
    16.0f, 8.0f, 4.0f, 3.0f, 2.0f, 1.5f, 4.0f / 3.0f, 1.0f,
    0.75f, 2.0f / 3.0f, 0.5f, 1.0f / 3.0f, 0.25f, 1.0f / 6.0f,
    0.125f, 0.0625f
};

static float snap_sync_rate(float hz, float bpm) {
    bpm = clampf(bpm, 20.0f, 400.0f);
    float q_sec = 60.0f / bpm;
    float log_t = logf(clampf(hz, 0.02f, 20.0f));
    float best_hz = hz;
    float best_err = 1e9f;
    const int n = (int)(sizeof(SYNC_BEATS) / sizeof(SYNC_BEATS[0]));
    for (int i = 0; i < n; ++i) {
        float cand = 1.0f / (q_sec * SYNC_BEATS[i]);
        if (cand < 0.015f || cand > 24.0f) continue;
        float err = fabsf(logf(cand) - log_t);
        if (err < best_err) {
            best_err = err;
            best_hz = cand;
        }
    }
    return best_hz;
}

static float eval_wave(int shape, double ph) {
    double p = ph - floor(ph);
    switch (shape) {
        case MODLS_SHAPE_SINE:
            return sinf((float)(p * 2.0 * M_PI));
        case MODLS_SHAPE_TRI: {
            float t = (float)(4.0 * fabs(p - 0.5) - 1.0);
            /* Slight soft rounding at turnaround prevents high-FB clicks */
            return t * (1.08f - 0.08f * t * t);
        }
        case MODLS_SHAPE_EXP: {
            float u = 0.5f - 0.5f * cosf((float)(p * 2.0 * M_PI));
            return 2.0f * powf(u, 2.2f) - 1.0f;
        }
        default:
            return sinf((float)(p * 2.0 * M_PI));
    }
}

static inline float process_diffuser(ModlsDelayLine *stages,
                                     const float *base_samps,
                                     float x,
                                     float coeff,
                                     float mod_samps) {
    float sig = x;
    for (int s = 0; s < MODLS_DIFF_STAGES; ++s) {
        float d_pos = base_samps[s] + ((s & 1) ? -mod_samps : mod_samps);
        float d = delay_read_lin(&stages[s], d_pos);
        float v = sig + coeff * d;
        delay_write(&stages[s], v);
        sig = d - coeff * v;
    }
    return sig;
}

void modls_params_default(ModlsParams *p) {
    if (!p) return;
    memset(p, 0, sizeof(*p));
    p->mode         = MODLS_MODE_CHORUS;
    p->depth_pct    = 45.0f;
    p->feedback_pct = 20.0f;
    p->mix_pct      = 50.0f;

    p->shape        = MODLS_SHAPE_SINE;
    p->rate_hz      = 0.65f;
    p->sync         = 0;
    p->stereo_deg   = 90.0f;

    p->delay_ms     = 8.0f;
    p->spread_pct   = 40.0f;
    p->diffuse_pct  = 15.0f;
    p->overmod_pct  = 0.0f;

    p->low_cut_hz   = 90.0f;
    p->high_cut_hz  = 12000.0f;
    p->drive_pct    = 10.0f;
    p->env_mod_pct  = 0.0f;

    p->rate_fx_pct  = 0.0f;
    p->warp_fx_pct  = 0.0f;
    p->comb_fx_pct  = 0.0f;
    p->fx_zero      = 0;

    p->width_pct    = 100.0f;
    p->lfo_freeze   = 0;
    p->output_db    = 0.0f;
    p->bypass       = 0;

    p->host_bpm     = 120.0f;
}

int modls_core_init(ModlsCore *c, double sample_rate) {
    if (!c) return -1;
    memset(c, 0, sizeof(*c));
    if (!isfinite(sample_rate) || sample_rate < 8000.0 || sample_rate > 384000.0) {
        sample_rate = 48000.0;
    }
    c->sample_rate = sample_rate;
    c->sr_f = (float)sample_rate;

    uint32_t main_samps = (uint32_t)ceil(0.65 * sample_rate) + 16u;
    uint32_t ref_samps  = (uint32_t)ceil(0.065 * sample_rate) + 16u;

    if (delay_alloc(&c->main_l, main_samps) != 0 ||
        delay_alloc(&c->main_r, main_samps) != 0 ||
        delay_alloc(&c->ref_l, ref_samps) != 0 ||
        delay_alloc(&c->ref_r, ref_samps) != 0) {
        modls_core_free(c);
        return -1;
    }

    static const float DIFF_MS_L[MODLS_DIFF_STAGES] = { 3.13f, 7.27f, 13.81f, 23.47f };
    static const float DIFF_MS_R[MODLS_DIFF_STAGES] = { 3.67f, 8.21f, 15.11f, 25.31f };

    for (int s = 0; s < MODLS_DIFF_STAGES; ++s) {
        c->diff_base_l[s] = DIFF_MS_L[s] * 0.001f * c->sr_f;
        c->diff_base_r[s] = DIFF_MS_R[s] * 0.001f * c->sr_f;
        uint32_t dl = (uint32_t)ceilf(c->diff_base_l[s] + 0.004f * c->sr_f) + 8u;
        uint32_t dr = (uint32_t)ceilf(c->diff_base_r[s] + 0.004f * c->sr_f) + 8u;
        if (delay_alloc(&c->diff_l[s], dl) != 0 || delay_alloc(&c->diff_r[s], dr) != 0) {
            modls_core_free(c);
            return -1;
        }
    }

    modls_core_reset(c);
    return 0;
}

void modls_core_reset(ModlsCore *c) {
    if (!c) return;
    delay_clear(&c->main_l);
    delay_clear(&c->main_r);
    delay_clear(&c->ref_l);
    delay_clear(&c->ref_r);
    for (int s = 0; s < MODLS_DIFF_STAGES; ++s) {
        delay_clear(&c->diff_l[s]);
        delay_clear(&c->diff_r[s]);
    }
    memset(c->phaser_l, 0, sizeof(c->phaser_l));
    memset(c->phaser_r, 0, sizeof(c->phaser_r));
    memset(c->barber_l, 0, sizeof(c->barber_l));
    memset(c->barber_r, 0, sizeof(c->barber_r));

    c->fb_l = 0.0f;
    c->fb_r = 0.0f;
    memset(&c->lp_fb_l, 0, sizeof(c->lp_fb_l));
    memset(&c->lp_fb_r, 0, sizeof(c->lp_fb_r));
    memset(&c->hp_fb_l, 0, sizeof(c->hp_fb_l));
    memset(&c->hp_fb_r, 0, sizeof(c->hp_fb_r));
    memset(&c->dc_l, 0, sizeof(c->dc_l));
    memset(&c->dc_r, 0, sizeof(c->dc_r));
    memset(&c->wet_lp_l, 0, sizeof(c->wet_lp_l));
    memset(&c->wet_lp_r, 0, sizeof(c->wet_lp_r));
    memset(&c->wet_hp_l, 0, sizeof(c->wet_hp_l));
    memset(&c->wet_hp_r, 0, sizeof(c->wet_hp_r));
    memset(&c->sub_lp_l, 0, sizeof(c->sub_lp_l));
    memset(&c->sub_lp_r, 0, sizeof(c->sub_lp_r));

    c->lfo_phase = 0.0;
    c->fast_phase = 0.0;
    c->barber_phase = 0.0;
    c->rng_state = 0x12345678u;
    c->sh_target_l = 0.0f;
    c->sh_target_r = 0.0f;
    c->sh_cur_l = 0.0f;
    c->sh_cur_r = 0.0f;
    c->prev_phase_l = 0.0;
    c->prev_phase_r = 0.0;
    c->chaos_x_l = 0.31f;
    c->chaos_y_l = -0.22f;
    c->chaos_x_r = -0.27f;
    c->chaos_y_r = 0.41f;

    c->env_fast = 0.0f;
    c->env_l = 0.0f;
    c->env_r = 0.0f;

    c->sm_depth = 0.45f;
    c->sm_feedback = 0.20f;
    c->sm_mix = 0.50f;
    c->sm_stereo = 90.0f;
    c->sm_delay_ms = 8.0f;
    c->sm_spread = 0.40f;
    c->sm_diffuse = 0.15f;
    c->sm_overmod = 0.0f;
    c->sm_drive = 0.10f;
    c->sm_env_mod = 0.0f;
    c->sm_rate_fx = 0.0f;
    c->sm_warp_fx = 0.0f;
    c->sm_comb_fx = 0.0f;
    c->sm_width = 1.0f;
    c->sm_out_gain = 1.0f;
    c->sm_bypass = 0.0f;
    c->sm_freeze = 0.0f;

    c->last_mode = MODLS_MODE_CHORUS;
    c->mode_fade = 1.0f;

    c->meter_in = 0.0f;
    c->meter_out = 0.0f;
    c->meter_fb = 0.0f;
    c->meters.in_peak_db = MODLS_FLOOR_DB;
    c->meters.out_peak_db = MODLS_FLOOR_DB;
    c->meters.lfo_l = 0.0f;
    c->meters.lfo_r = 0.0f;
    c->meters.env_lvl = 0.0f;
    c->meters.eff_mod = 0.0f;
    c->meters.fb_energy = 0.0f;
    c->meters.host_bpm = 120.0f;
}

void modls_core_free(ModlsCore *c) {
    if (!c) return;
    delay_free(&c->main_l);
    delay_free(&c->main_r);
    delay_free(&c->ref_l);
    delay_free(&c->ref_r);
    for (int s = 0; s < MODLS_DIFF_STAGES; ++s) {
        delay_free(&c->diff_l[s]);
        delay_free(&c->diff_r[s]);
    }
}

void modls_core_process(ModlsCore *c,
                        const ModlsParams *p,
                        const float *in_l,
                        const float *in_r,
                        float *out_l,
                        float *out_r,
                        uint32_t nframes) {
    if (!c || !p || !in_l || !in_r || !out_l || !out_r || nframes == 0) return;

    const float sr = c->sr_f;
    const float ms_to_samp = sr * 0.001f;

    int mode  = p->mode;
    if (mode < 0) mode = 0;
    if (mode >= MODLS_MODE_COUNT) mode = MODLS_MODE_COUNT - 1;

    int shape = p->shape;
    if (shape < 0) shape = 0;
    if (shape >= MODLS_SHAPE_COUNT) shape = MODLS_SHAPE_COUNT - 1;

    if (mode != c->last_mode) {
        c->last_mode = mode;
        c->mode_fade = 0.0f;
        /* Soften feedback state to avoid switching thump */
        c->fb_l *= 0.25f;
        c->fb_r *= 0.25f;
    }

    const float t_depth    = clampf(p->depth_pct, 0.0f, 100.0f) * 0.01f;
    const float t_feedback = clampf(p->feedback_pct, -100.0f, 100.0f) * 0.01f;
    const float t_mix      = clampf(p->mix_pct, 0.0f, 100.0f) * 0.01f;
    const float t_stereo   = clampf(p->stereo_deg, 0.0f, 180.0f);
    const float t_delay_ms = clampf(p->delay_ms, 0.3f, 120.0f);
    const float t_spread   = clampf(p->spread_pct, 0.0f, 100.0f) * 0.01f;
    const float t_diffuse  = clampf(p->diffuse_pct, 0.0f, 100.0f) * 0.01f;
    const float t_overmod  = clampf(p->overmod_pct, 0.0f, 100.0f) * 0.01f;
    const float t_drive    = clampf(p->drive_pct, 0.0f, 100.0f) * 0.01f;
    const float t_env_mod  = clampf(p->env_mod_pct, -100.0f, 100.0f) * 0.01f;
    const float t_rate_fx  = p->fx_zero ? 0.0f : (clampf(p->rate_fx_pct, -100.0f, 100.0f) * 0.01f);
    const float t_warp_fx  = p->fx_zero ? 0.0f : (clampf(p->warp_fx_pct, 0.0f, 100.0f) * 0.01f);
    const float t_comb_fx  = p->fx_zero ? 0.0f : (clampf(p->comb_fx_pct, -100.0f, 100.0f) * 0.01f);
    const float t_width    = clampf(p->width_pct, 0.0f, 200.0f) * 0.01f;
    const float t_out_gain = db_to_lin(clampf(p->output_db, -18.0f, 12.0f));
    const float t_bypass   = p->bypass ? 1.0f : 0.0f;
    const float t_freeze   = p->lfo_freeze ? 1.0f : 0.0f;

    const float host_bpm = clampf(p->host_bpm, 20.0f, 400.0f);
    float base_rate_hz = clampf(p->rate_hz, 0.02f, 20.0f);
    if (p->sync) {
        base_rate_hz = snap_sync_rate(base_rate_hz, host_bpm);
    }

    const float low_cut_hz  = clampf(p->low_cut_hz, 20.0f, 600.0f);
    const float high_cut_hz = clampf(p->high_cut_hz, 1000.0f, 20000.0f);

    const float lp_a = (high_cut_hz >= 19500.0f)
                     ? 1.0f
                     : clampf(1.0f - expf(-MODLS_TWO_PI * high_cut_hz / sr), 0.001f, 0.995f);
    const float hp_a = clampf(1.0f - expf(-MODLS_TWO_PI * low_cut_hz / sr), 0.0002f, 0.5f);
    const int   hp_wet_on = (low_cut_hz > 22.0f) ? 1 : 0;
    const float dc_r = clampf(1.0f - (MODLS_TWO_PI * 10.0f / sr), 0.985f, 0.9999f);

    const float sm_a       = 1.0f - expf(-1.0f / (0.012f * sr));
    const float sm_del_a   = 1.0f - expf(-1.0f / (0.028f * sr));
    const float mode_inc   = 1.0f / (0.015f * sr);
    const float env_atk    = 1.0f - expf(-1.0f / (0.005f * sr));
    const float env_rel_l  = 1.0f - expf(-1.0f / (0.110f * sr));
    const float env_rel_r  = 1.0f - expf(-1.0f / (0.145f * sr));
    const float sh_slew    = 1.0f - expf(-1.0f / (0.008f * sr));
    const float meter_rel  = expf(-1.0f / (0.18f * sr));

    /* Golden/prime tap offsets for 8-tap Uber mode */
    static const float UBER_TAPS_L[8] = { -0.46f, -0.31f, -0.19f, -0.07f,  0.06f,  0.18f,  0.33f,  0.49f };
    static const float UBER_TAPS_R[8] = { -0.49f, -0.34f, -0.21f, -0.05f,  0.08f,  0.22f,  0.37f,  0.47f };

    float block_in_pk = 0.0f;
    float block_out_pk = 0.0f;
    float last_lfo_l = 0.0f;
    float last_lfo_r = 0.0f;
    float last_eff_mod = 0.0f;

    for (uint32_t i = 0; i < nframes; ++i) {
        float xl = isfinite(in_l[i]) ? in_l[i] : 0.0f;
        float xr = isfinite(in_r[i]) ? in_r[i] : 0.0f;

        float in_abs = fmaxf(fabsf(xl), fabsf(xr));
        if (in_abs > block_in_pk) block_in_pk = in_abs;

        /* Parameter smoothing */
        c->sm_depth    += sm_a     * (t_depth    - c->sm_depth);
        c->sm_feedback += sm_a     * (t_feedback - c->sm_feedback);
        c->sm_mix      += sm_a     * (t_mix      - c->sm_mix);
        c->sm_stereo   += sm_a     * (t_stereo   - c->sm_stereo);
        c->sm_delay_ms += sm_del_a * (t_delay_ms - c->sm_delay_ms);
        c->sm_spread   += sm_a     * (t_spread   - c->sm_spread);
        c->sm_diffuse  += sm_a     * (t_diffuse  - c->sm_diffuse);
        c->sm_overmod  += sm_a     * (t_overmod  - c->sm_overmod);
        c->sm_drive    += sm_a     * (t_drive    - c->sm_drive);
        c->sm_env_mod  += sm_a     * (t_env_mod  - c->sm_env_mod);
        c->sm_rate_fx  += sm_a     * (t_rate_fx  - c->sm_rate_fx);
        c->sm_warp_fx  += sm_a     * (t_warp_fx  - c->sm_warp_fx);
        c->sm_comb_fx  += sm_a     * (t_comb_fx  - c->sm_comb_fx);
        c->sm_width    += sm_a     * (t_width    - c->sm_width);
        c->sm_out_gain += sm_a     * (t_out_gain - c->sm_out_gain);
        c->sm_bypass   += sm_a     * (t_bypass   - c->sm_bypass);
        c->sm_freeze   += sm_a     * (t_freeze   - c->sm_freeze);

        if (c->mode_fade < 1.0f) {
            c->mode_fade += mode_inc;
            if (c->mode_fade > 1.0f) c->mode_fade = 1.0f;
        }

        /* Envelope follower */
        float env_in_l = fabsf(xl);
        float env_in_r = fabsf(xr);
        c->env_l = flush_denorm(c->env_l + ((env_in_l > c->env_l) ? env_atk : env_rel_l) * (env_in_l - c->env_l));
        c->env_r = flush_denorm(c->env_r + ((env_in_r > c->env_r) ? env_atk : env_rel_r) * (env_in_r - c->env_r));
        float env_mono = 0.5f * (c->env_l + c->env_r);
        /* Normalized responsive 0..1 envelope */
        float env_norm   = clampf(env_mono / (env_mono + 0.18f), 0.0f, 1.0f);
        float env_norm_l = clampf(c->env_l / (c->env_l + 0.18f), 0.0f, 1.0f);
        float env_norm_r = clampf(c->env_r / (c->env_r + 0.18f), 0.0f, 1.0f);
        c->env_fast = env_norm;

        /* Effective Uber/Warp macros */
        float warp = c->sm_warp_fx;
        float over = clampf(c->sm_overmod + warp * 0.85f, 0.0f, 1.45f);
        float fb_eff = c->sm_feedback;
        if (warp > 0.001f) {
            float sgn = (fb_eff < -0.02f) ? -1.0f : 1.0f;
            float mag = clampf(fabsf(fb_eff) + warp * (0.94f - fabsf(fb_eff)), 0.0f, 0.96f);
            fb_eff = sgn * mag;
        } else {
            fb_eff = clampf(fb_eff, -0.96f, 0.96f);
        }
        float diff_eff = clampf(c->sm_diffuse + warp * 0.55f, 0.0f, 1.0f);

        /* Effective Rate (Rate FX exponential +/- 4 octaves, plus slight Env push) */
        float rate_mult = powf(2.0f, c->sm_rate_fx * 3.5f + c->sm_env_mod * env_norm * 0.7f);
        float eff_rate_hz = clampf(base_rate_hz * rate_mult, 0.005f, 80.0f);
        float run_scale = 1.0f - c->sm_freeze;

        /* Advance primary LFO, secondary OverMod LFO, and Barberpole phase */
        double ph_inc = (double)(eff_rate_hz * run_scale) / c->sample_rate;
        c->lfo_phase += ph_inc;
        if (c->lfo_phase >= 1.0) c->lfo_phase -= floor(c->lfo_phase);

        /* Secondary harmonic / audio-rate LFO driven by OverMod */
        float fast_hz = eff_rate_hz * 3.71f + over * over * 38.0f;
        if (fast_hz > 85.0f) fast_hz = 85.0f;
        c->fast_phase += (double)(fast_hz * run_scale) / c->sample_rate;
        if (c->fast_phase >= 1.0) c->fast_phase -= floor(c->fast_phase);

        /* Unidirectional Barberpole phase (direction follows feedback polarity) */
        float barber_dir = (fb_eff < -0.001f) ? -1.0f : 1.0f;
        double b_inc = (double)(eff_rate_hz * 0.45f * barber_dir * run_scale) / c->sample_rate;
        c->barber_phase += b_inc;
        c->barber_phase -= floor(c->barber_phase);

        double stereo_off = (double)(c->sm_stereo / 360.0f);
        double ph_l = c->lfo_phase;
        double ph_r = c->lfo_phase + stereo_off;
        ph_r -= floor(ph_r);

        /* Update S&H targets on phase wrap */
        if (ph_l < c->prev_phase_l) {
            c->sh_target_l = rng_next_bipolar(&c->rng_state);
            if (c->sm_stereo < 5.0f) c->sh_target_r = c->sh_target_l;
        }
        if (ph_r < c->prev_phase_r) {
            c->sh_target_r = (c->sm_stereo < 5.0f) ? c->sh_target_l : rng_next_bipolar(&c->rng_state);
        }
        c->prev_phase_l = ph_l;
        c->prev_phase_r = ph_r;
        c->sh_cur_l = flush_denorm(c->sh_cur_l + sh_slew * (c->sh_target_l - c->sh_cur_l));
        c->sh_cur_r = flush_denorm(c->sh_cur_r + sh_slew * (c->sh_target_r - c->sh_cur_r));

        /* Update bounded nonlinear chaotic drift oscillators (sample-rate independent) */
        float c_dt = clampf(eff_rate_hz * (MODLS_TWO_PI / sr) * run_scale, 0.0f, 0.08f);
        if (c_dt > 0.0f) {
            float drv_l = 0.85f * sinf((float)(c->lfo_phase * 2.0 * M_PI));
            float ax_l = -0.30f * c->chaos_y_l + c->chaos_x_l - c->chaos_x_l * c->chaos_x_l * c->chaos_x_l + drv_l;
            c->chaos_y_l = clampf(c->chaos_y_l + c_dt * ax_l * 2.2f, -1.8f, 1.8f);
            c->chaos_x_l = clampf(c->chaos_x_l + c_dt * c->chaos_y_l * 2.2f, -1.5f, 1.5f);

            float drv_r = 0.85f * sinf((float)((c->lfo_phase + stereo_off) * 2.0 * M_PI));
            float ax_r = -0.30f * c->chaos_y_r + c->chaos_x_r - c->chaos_x_r * c->chaos_x_r * c->chaos_x_r + drv_r;
            c->chaos_y_r = clampf(c->chaos_y_r + c_dt * ax_r * 2.2f, -1.8f, 1.8f);
            c->chaos_x_r = clampf(c->chaos_x_r + c_dt * c->chaos_y_r * 2.2f, -1.5f, 1.5f);
        }

        /* Evaluate primary LFO L/R in [-1..+1] */
        float lfo_l = 0.0f;
        float lfo_r = 0.0f;
        if (shape == MODLS_SHAPE_SH) {
            lfo_l = c->sh_cur_l;
            lfo_r = c->sh_cur_r;
        } else if (shape == MODLS_SHAPE_CHAOS) {
            lfo_l = clampf(c->chaos_x_l * 0.72f, -1.0f, 1.0f);
            float blend = clampf(c->sm_stereo / 90.0f, 0.0f, 1.0f);
            float ch_r = clampf(c->chaos_x_r * 0.72f, -1.0f, 1.0f);
            lfo_r = lfo_l + blend * (ch_r - lfo_l);
        } else if (shape == MODLS_SHAPE_ENV) {
            lfo_l = env_norm_l * 2.0f - 1.0f;
            float env_r_mod = env_norm_r * 2.0f - 1.0f;
            float inv_blend = clampf(c->sm_stereo / 180.0f, 0.0f, 1.0f);
            lfo_r = env_r_mod * (1.0f - 2.0f * inv_blend);
        } else {
            lfo_l = eval_wave(shape, ph_l);
            lfo_r = eval_wave(shape, ph_r);
        }

        /* Secondary fast shimmer / audio-rate FM from OverMod */
        float fast_l = sinf((float)(c->fast_phase * 2.0 * M_PI));
        float fast_r = sinf((float)((c->fast_phase + stereo_off * 0.73) * 2.0 * M_PI));
        float over_blend = clampf(over * 0.45f, 0.0f, 0.65f);
        float mod_l = clampf(lfo_l + over_blend * fast_l + (over > 0.5f ? 0.25f * (over - 0.5f) * c->chaos_x_l : 0.0f), -1.8f, 1.8f);
        float mod_r = clampf(lfo_r + over_blend * fast_r + (over > 0.5f ? 0.25f * (over - 0.5f) * c->chaos_x_r : 0.0f), -1.8f, 1.8f);

        /* Effective depth with Env Mod (-100..+100%) and OverMod expansion */
        float env_scale = 1.0f + c->sm_env_mod * (env_norm * 1.4f - 0.25f);
        if (c->sm_env_mod < 0.0f) {
            /* Negative Env Mod ducks modulation on transients, opens on tails */
            env_scale = clampf(1.0f + c->sm_env_mod * env_norm * 1.15f, 0.05f, 1.2f);
        } else {
            env_scale = clampf(env_scale, 0.1f, 2.2f);
        }
        float over_depth_mult = 1.0f + over * 1.1f + over * over * 1.9f; /* up to ~4.5x at extreme */
        float depth_eff = clampf(c->sm_depth * env_scale, 0.0f, 1.4f);
        float depth_uber = depth_eff * over_depth_mult;

        /* Keep reference delay line always fed for click-free TZF */
        delay_write(&c->ref_l, xl);
        delay_write(&c->ref_r, xr);

        /* Extract clean sub-bass (below low_cut) for bass clarity in Chorus/Tri-Mod/Uber */
        float sub_l = onepole_lp(&c->sub_lp_l, xl, hp_a);
        float sub_r = onepole_lp(&c->sub_lp_r, xr, hp_a);

        /* Condition previous feedback sample: Low Cut -> High Cut -> Drive -> DC Block */
        float fb_cond_l = onepole_hp(&c->hp_fb_l, c->fb_l, hp_a);
        float fb_cond_r = onepole_hp(&c->hp_fb_r, c->fb_r, hp_a);
        fb_cond_l = onepole_lp(&c->lp_fb_l, fb_cond_l, lp_a);
        fb_cond_r = onepole_lp(&c->lp_fb_r, fb_cond_r, lp_a);
        fb_cond_l = dc_block_process(&c->dc_l, soft_drive(fb_cond_l, c->sm_drive), dc_r);
        fb_cond_r = dc_block_process(&c->dc_r, soft_drive(fb_cond_r, c->sm_drive), dc_r);

        /* Cross-channel feedback blend controlled by Stereo (0..180 deg) */
        float xfade = clampf(c->sm_stereo / 180.0f, 0.0f, 1.0f) * 0.45f;
        float fb_in_l = (1.0f - xfade) * fb_cond_l + xfade * fb_cond_r;
        float fb_in_r = (1.0f - xfade) * fb_cond_r + xfade * fb_cond_l;

        float fb_abs = fmaxf(fabsf(fb_in_l), fabsf(fb_in_r));
        c->meter_fb = fmaxf(fb_abs, c->meter_fb * meter_rel);

        float dry_l = xl;
        float dry_r = xr;
        float wet_l = 0.0f;
        float wet_r = 0.0f;

        float diff_g = 0.68f * diff_eff;
        float diff_mod_samps = (0.05f + 0.35f * over) * fast_l * ms_to_samp;

        /* ==================== MODE DISPATCH ==================== */
        switch (mode) {
            case MODLS_MODE_CHORUS: {
                /* Mode 0: Dual-Tap Analog BBD Chorus */
                float in_d_l = soft_drive(xl + fb_eff * 0.85f * fb_in_l, c->sm_drive * 0.55f);
                float in_d_r = soft_drive(xr + fb_eff * 0.85f * fb_in_r, c->sm_drive * 0.55f);
                delay_write(&c->main_l, in_d_l);
                delay_write(&c->main_r, in_d_r);

                float center_ms = clampf(c->sm_delay_ms, 2.0f, 45.0f) + c->sm_comb_fx * 8.0f;
                if (center_ms < 1.5f) center_ms = 1.5f;
                float swing_ms = 5.2f * depth_uber;
                float tap_off  = 2.0f + c->sm_spread * 14.0f;

                /* Primary & quadrature secondary tap per channel */
                float lfo_q_l = eval_wave(shape == MODLS_SHAPE_SH || shape == MODLS_SHAPE_CHAOS || shape == MODLS_SHAPE_ENV
                                          ? MODLS_SHAPE_SINE : shape, ph_l + 0.25);
                float lfo_q_r = eval_wave(shape == MODLS_SHAPE_SH || shape == MODLS_SHAPE_CHAOS || shape == MODLS_SHAPE_ENV
                                          ? MODLS_SHAPE_SINE : shape, ph_r + 0.25);

                float d1_l = clampf(center_ms + mod_l * swing_ms, 0.35f, 580.0f);
                float d2_l = clampf(center_ms + tap_off + (0.65f * lfo_q_l + 0.35f * mod_l) * swing_ms * 0.85f, 0.35f, 580.0f);
                float d1_r = clampf(center_ms + mod_r * swing_ms, 0.35f, 580.0f);
                float d2_r = clampf(center_ms + tap_off + (0.65f * lfo_q_r + 0.35f * mod_r) * swing_ms * 0.85f, 0.35f, 580.0f);

                float t1_l = delay_read_hermite(&c->main_l, d1_l * ms_to_samp);
                float t2_l = delay_read_hermite(&c->main_l, d2_l * ms_to_samp);
                float t1_r = delay_read_hermite(&c->main_r, d1_r * ms_to_samp);
                float t2_r = delay_read_hermite(&c->main_r, d2_r * ms_to_samp);

                float sp_w = 0.25f + 0.45f * c->sm_spread;
                float norm = 1.0f / sqrtf(1.0f + sp_w * sp_w);
                wet_l = (t1_l + sp_w * t2_l) * norm;
                wet_r = (t1_r + sp_w * t2_r) * norm;

                if (diff_eff > 0.005f) {
                    float dl = process_diffuser(c->diff_l, c->diff_base_l, wet_l, diff_g, diff_mod_samps);
                    float dr = process_diffuser(c->diff_r, c->diff_base_r, wet_r, diff_g, -diff_mod_samps);
                    wet_l += diff_eff * (dl - wet_l);
                    wet_r += diff_eff * (dr - wet_r);
                }
                c->fb_l = wet_l;
                c->fb_r = wet_r;
                break;
            }

            case MODLS_MODE_TRIMOD: {
                /* Mode 1: 3-Phase Ensemble / Dimension Swirl (0 / 120 / 240 deg) */
                float in_d_l = soft_drive(xl + fb_eff * 0.82f * fb_in_l, c->sm_drive * 0.55f);
                float in_d_r = soft_drive(xr + fb_eff * 0.82f * fb_in_r, c->sm_drive * 0.55f);
                delay_write(&c->main_l, in_d_l);
                delay_write(&c->main_r, in_d_r);

                float center_ms = clampf(c->sm_delay_ms, 2.5f, 55.0f) + c->sm_comb_fx * 10.0f;
                if (center_ms < 2.0f) center_ms = 2.0f;
                float swing_ms = 4.5f * depth_uber;
                float spread_ms = 1.5f + c->sm_spread * 18.0f;

                float sum_l = 0.0f;
                float sum_r = 0.0f;
                static const float VOICE_OFF[3] = { -1.0f, 0.0f, 1.0f };
                for (int v = 0; v < 3; ++v) {
                    double voff = (double)v / 3.0;
                    float m_l = eval_wave(shape <= MODLS_SHAPE_EXP ? shape : MODLS_SHAPE_SINE, ph_l + voff);
                    float m_r = eval_wave(shape <= MODLS_SHAPE_EXP ? shape : MODLS_SHAPE_SINE, ph_r + voff);
                    if (shape >= MODLS_SHAPE_SH) {
                        m_l = 0.55f * m_l + 0.45f * mod_l;
                        m_r = 0.55f * m_r + 0.45f * mod_r;
                    }
                    /* Solina/Ensemble dual-speed shimmer layer */
                    float shim_l = sinf((float)((c->fast_phase + voff) * 2.0 * M_PI)) * (0.18f + 0.55f * over);
                    float shim_r = sinf((float)((c->fast_phase + voff + stereo_off) * 2.0 * M_PI)) * (0.18f + 0.55f * over);

                    float dl_ms = clampf(center_ms + VOICE_OFF[v] * spread_ms + (m_l + shim_l) * swing_ms, 0.35f, 580.0f);
                    float dr_ms = clampf(center_ms - VOICE_OFF[v] * spread_ms * 0.9f + (m_r + shim_r) * swing_ms, 0.35f, 580.0f);

                    sum_l += delay_read_hermite(&c->main_l, dl_ms * ms_to_samp);
                    sum_r += delay_read_hermite(&c->main_r, dr_ms * ms_to_samp);
                }
                const float inv_s3 = 0.57735027f;
                wet_l = sum_l * inv_s3;
                wet_r = sum_r * inv_s3;

                if (diff_eff > 0.005f) {
                    float dl = process_diffuser(c->diff_l, c->diff_base_l, wet_l, diff_g, diff_mod_samps);
                    float dr = process_diffuser(c->diff_r, c->diff_base_r, wet_r, diff_g, -diff_mod_samps);
                    wet_l += diff_eff * (dl - wet_l);
                    wet_r += diff_eff * (dr - wet_r);
                }
                c->fb_l = wet_l;
                c->fb_r = wet_r;
                break;
            }

            case MODLS_MODE_FLANGER: {
                /* Mode 2: True Through-Zero Tape Flanger (TZF) */
                float ref_ms = clampf(c->sm_delay_ms, 1.5f, 32.0f);
                /* Dry signal sits on matched reference line so wet can sweep PAST zero! */
                dry_l = delay_read_hermite(&c->ref_l, ref_ms * ms_to_samp);
                dry_r = delay_read_hermite(&c->ref_r, ref_ms * ms_to_samp);

                float fb_sign = (fb_eff < 0.0f) ? -1.0f : 1.0f;
                float fb_gain = clampf(fabsf(fb_eff), 0.0f, 0.95f) * fb_sign;

                float in_d_l = soft_drive(xl + fb_gain * fb_in_l, c->sm_drive * 0.65f);
                float in_d_r = soft_drive(xr + fb_gain * fb_in_r, c->sm_drive * 0.65f);
                delay_write(&c->main_l, in_d_l);
                delay_write(&c->main_r, in_d_r);

                /* Symmetric sweep across ref_ms -> crosses zero relative delay! */
                float max_swing = fminf(ref_ms * 0.95f, 12.0f) * clampf(depth_uber, 0.0f, 1.8f);
                float comb_ms = c->sm_comb_fx * fminf(ref_ms * 0.85f, 10.0f);

                float dl_ms = clampf(ref_ms + mod_l * max_swing + comb_ms, 0.20f, 580.0f);
                float dr_ms = clampf(ref_ms + mod_r * max_swing + comb_ms, 0.20f, 580.0f);

                float w1_l = delay_read_hermite(&c->main_l, dl_ms * ms_to_samp);
                float w1_r = delay_read_hermite(&c->main_r, dr_ms * ms_to_samp);

                /* Spread adds a counter-sweeping second flange voice */
                if (c->sm_spread > 0.02f) {
                    float d2_l = clampf(ref_ms - mod_l * max_swing * 0.55f + comb_ms * 0.5f, 0.20f, 580.0f);
                    float d2_r = clampf(ref_ms - mod_r * max_swing * 0.55f + comb_ms * 0.5f, 0.20f, 580.0f);
                    float w2_l = delay_read_hermite(&c->main_l, d2_l * ms_to_samp);
                    float w2_r = delay_read_hermite(&c->main_r, d2_r * ms_to_samp);
                    float g2 = 0.45f * c->sm_spread;
                    float n2 = 1.0f / (1.0f + g2);
                    w1_l = (w1_l + g2 * w2_l) * n2;
                    w1_r = (w1_r + g2 * w2_r) * n2;
                }

                /* Invert wet polarity when feedback is negative for authentic TZF null */
                wet_l = (fb_eff < -0.02f) ? -w1_l : w1_l;
                wet_r = (fb_eff < -0.02f) ? -w1_r : w1_r;

                if (diff_eff > 0.005f) {
                    float dl = process_diffuser(c->diff_l, c->diff_base_l, wet_l, diff_g, diff_mod_samps);
                    float dr = process_diffuser(c->diff_r, c->diff_base_r, wet_r, diff_g, -diff_mod_samps);
                    wet_l += diff_eff * (dl - wet_l);
                    wet_r += diff_eff * (dr - wet_r);
                }
                c->fb_l = w1_l;
                c->fb_r = w1_r;
                break;
            }

            case MODLS_MODE_PHASER: {
                /* Mode 3: 4..12-Stage Bilinear Stereo All-Pass Phaser */
                /* Map Delay (0.3..120 ms) inversely to center frequency (2600..180 Hz) */
                float log_del = logf(clampf(c->sm_delay_ms, 0.3f, 120.0f) / 0.3f) / logf(120.0f / 0.3f);
                float base_fc = 2600.0f * powf(180.0f / 2600.0f, log_del);

                float oct_span = 3.2f * clampf(depth_uber, 0.0f, 1.8f);
                float fc_l = clampf(base_fc * powf(2.0f, (mod_l * oct_span) + c->sm_comb_fx * 2.2f), 45.0f, sr * 0.42f);
                float fc_r = clampf(base_fc * powf(2.0f, (mod_r * oct_span) + c->sm_comb_fx * 2.2f), 45.0f, sr * 0.42f);

                float a_l = ap1_coeff(fc_l, sr);
                float a_r = ap1_coeff(fc_r, sr);

                float ph_fb = clampf(fb_eff, -0.93f, 0.93f);
                float sig_l = soft_drive(xl + ph_fb * fb_in_l, c->sm_drive * 0.6f);
                float sig_r = soft_drive(xr + ph_fb * fb_in_r, c->sm_drive * 0.6f);

                float tap4_l = 0.0f, tap6_l = 0.0f, tap8_l = 0.0f, tap12_l = 0.0f;
                float tap4_r = 0.0f, tap6_r = 0.0f, tap8_r = 0.0f, tap12_r = 0.0f;

                for (int s = 0; s < MODLS_PHASER_STAGES; ++s) {
                    /* Slight frequency stagger across stages controlled by OverMod for vocal formants */
                    float stag = 1.0f + (float)(s - 5) * 0.035f * over;
                    float al_s = (over > 0.05f) ? ap1_coeff(fc_l * stag, sr) : a_l;
                    float ar_s = (over > 0.05f) ? ap1_coeff(fc_r * stag, sr) : a_r;
                    sig_l = ap1_process(&c->phaser_l[s], sig_l, al_s);
                    sig_r = ap1_process(&c->phaser_r[s], sig_r, ar_s);
                    if (s == 3)  { tap4_l  = sig_l; tap4_r  = sig_r; }
                    if (s == 5)  { tap6_l  = sig_l; tap6_r  = sig_r; }
                    if (s == 7)  { tap8_l  = sig_l; tap8_r  = sig_r; }
                    if (s == 11) { tap12_l = sig_l; tap12_r = sig_r; }
                }

                /* Smooth morph across 4 -> 6 -> 8 -> 12 stages via Spread */
                float sp3 = c->sm_spread * 3.0f;
                if (sp3 < 1.0f) {
                    wet_l = tap4_l + sp3 * (tap6_l - tap4_l);
                    wet_r = tap4_r + sp3 * (tap6_r - tap4_r);
                } else if (sp3 < 2.0f) {
                    float t = sp3 - 1.0f;
                    wet_l = tap6_l + t * (tap8_l - tap6_l);
                    wet_r = tap6_r + t * (tap8_r - tap6_r);
                } else {
                    float t = sp3 - 2.0f;
                    wet_l = tap8_l + t * (tap12_l - tap8_l);
                    wet_r = tap8_r + t * (tap12_r - tap8_r);
                }

                if (diff_eff > 0.005f) {
                    float dl = process_diffuser(c->diff_l, c->diff_base_l, wet_l, diff_g, diff_mod_samps);
                    float dr = process_diffuser(c->diff_r, c->diff_base_r, wet_r, diff_g, -diff_mod_samps);
                    wet_l += diff_eff * (dl - wet_l);
                    wet_r += diff_eff * (dr - wet_r);
                }
                c->fb_l = wet_l;
                c->fb_r = wet_r;
                break;
            }

            case MODLS_MODE_BARBER: {
                /* Mode 4: Infinite Shepard / Barberpole 4-Voice Quadrature All-Pass */
                float log_del = logf(clampf(c->sm_delay_ms, 0.3f, 120.0f) / 0.3f) / logf(120.0f / 0.3f);
                float f_lo = 70.0f + 180.0f * (1.0f - log_del);
                float span_oct = 4.2f + 1.8f * clampf(depth_uber, 0.0f, 1.5f);
                float f_hi = fminf(f_lo * powf(2.0f, span_oct), sr * 0.40f);
                float ratio = f_hi / f_lo;

                float ph_fb = clampf(fabsf(fb_eff), 0.0f, 0.90f);
                float sig_in_l = soft_drive(xl + ph_fb * fb_in_l, c->sm_drive * 0.6f);
                float sig_in_r = soft_drive(xr + ph_fb * fb_in_r, c->sm_drive * 0.6f);

                /* OverMod adds FM flutter to the barberpole ramp; Comb FX shifts phase directly */
                double fm_off = (double)(c->sm_comb_fx * 0.5f + over * 0.08f * fast_l);
                double base_l = c->barber_phase + fm_off;
                double base_r = c->barber_phase + stereo_off + fm_off;

                float sum_l = 0.0f;
                float sum_r = 0.0f;
                for (int v = 0; v < MODLS_BARBER_VOICES; ++v) {
                    double pl = base_l + (double)v * 0.25;
                    double pr = base_r + (double)v * 0.25;
                    pl -= floor(pl);
                    pr -= floor(pr);

                    /* Constant-sum Hann window: sum(sin^2(pi * p_v)) == 2.0 identically */
                    float sl_w = sinf((float)(M_PI * pl));
                    float sr_w = sinf((float)(M_PI * pr));
                    float wl = sl_w * sl_w;
                    float wr = sr_w * sr_w;

                    float fl = f_lo * powf(ratio, (float)pl);
                    float fr = f_lo * powf(ratio, (float)pr);

                    float vl = sig_in_l;
                    float vr = sig_in_r;
                    for (int s = 0; s < MODLS_BARBER_STAGES; ++s) {
                        /* Spread separates inter-stage frequencies for wider hypnotic notches */
                        float mult = 1.0f + ((float)s - 1.5f) * 0.14f * c->sm_spread;
                        float al = ap1_coeff(fl * mult, sr);
                        float ar = ap1_coeff(fr * mult, sr);
                        vl = ap1_process(&c->barber_l[v][s], vl, al);
                        vr = ap1_process(&c->barber_r[v][s], vr, ar);
                    }
                    sum_l += wl * vl;
                    sum_r += wr * vr;
                }
                wet_l = 0.5f * sum_l;
                wet_r = 0.5f * sum_r;

                if (diff_eff > 0.005f) {
                    float dl = process_diffuser(c->diff_l, c->diff_base_l, wet_l, diff_g, diff_mod_samps);
                    float dr = process_diffuser(c->diff_r, c->diff_base_r, wet_r, diff_g, -diff_mod_samps);
                    wet_l += diff_eff * (dl - wet_l);
                    wet_r += diff_eff * (dr - wet_r);
                }
                c->fb_l = wet_l;
                c->fb_r = wet_r;
                break;
            }

            case MODLS_MODE_UBER:
            default: {
                /* Mode 5: 8-Tap Diffused Uber-Mod Cloud Engine */
                float in_fb_l = soft_drive(xl + fb_eff * 0.88f * fb_in_l, c->sm_drive * 0.75f);
                float in_fb_r = soft_drive(xr + fb_eff * 0.88f * fb_in_r, c->sm_drive * 0.75f);

                /* Pre-delay stereo 4-stage all-pass diffusion turns discrete taps into silk clouds */
                float u_diff_g = 0.20f + 0.50f * diff_eff;
                float d_in_l = process_diffuser(c->diff_l, c->diff_base_l, in_fb_l, u_diff_g, diff_mod_samps);
                float d_in_r = process_diffuser(c->diff_r, c->diff_base_r, in_fb_r, u_diff_g, -diff_mod_samps);
                float w_in_l = in_fb_l + diff_eff * (d_in_l - in_fb_l);
                float w_in_r = in_fb_r + diff_eff * (d_in_r - in_fb_r);

                delay_write(&c->main_l, w_in_l);
                delay_write(&c->main_r, w_in_r);

                float center_ms = clampf(c->sm_delay_ms, 2.0f, 120.0f) + c->sm_comb_fx * 28.0f;
                if (center_ms < 2.0f) center_ms = 2.0f;
                float spread_ms = 3.0f + c->sm_spread * c->sm_spread * 210.0f;
                float swing_ms  = 6.8f * depth_uber;

                float sum_l = 0.0f;
                float sum_r = 0.0f;
                for (int t = 0; t < 8; ++t) {
                    double toff = (double)t * 0.125;
                    float ml = sinf((float)((ph_l + toff) * 2.0 * M_PI));
                    float mr = sinf((float)((ph_r + toff) * 2.0 * M_PI));
                    if (shape != MODLS_SHAPE_SINE) {
                        ml = 0.55f * ml + 0.45f * mod_l;
                        mr = 0.55f * mr + 0.45f * mod_r;
                    }
                    float flt = sinf((float)((c->fast_phase + toff * 1.7) * 2.0 * M_PI)) * (0.25f * over);

                    float dl_ms = clampf(center_ms + UBER_TAPS_L[t] * spread_ms + (ml + flt) * swing_ms, 0.5f, 590.0f);
                    float dr_ms = clampf(center_ms + UBER_TAPS_R[t] * spread_ms + (mr - flt) * swing_ms, 0.5f, 590.0f);

                    float sl = delay_read_hermite(&c->main_l, dl_ms * ms_to_samp);
                    float sr_v = delay_read_hermite(&c->main_r, dr_ms * ms_to_samp);

                    /* Alternating gentle stereo cross-panning controlled by Stereo */
                    float pan_x = (t & 1) ? (0.30f * (c->sm_stereo / 180.0f)) : 0.0f;
                    sum_l += (1.0f - pan_x) * sl + pan_x * sr_v;
                    sum_r += (1.0f - pan_x) * sr_v + pan_x * sl;
                }
                const float inv_s8 = 0.35355339f; /* 1 / sqrt(8) */
                wet_l = sum_l * inv_s8;
                wet_r = sum_r * inv_s8;

                c->fb_l = wet_l;
                c->fb_r = wet_r;
                break;
            }
        }

        /* Shape wet signal with Low Cut + High Cut */
        float w_hp_l = hp_wet_on ? onepole_hp(&c->wet_hp_l, wet_l, hp_a) : wet_l;
        float w_hp_r = hp_wet_on ? onepole_hp(&c->wet_hp_r, wet_r, hp_a) : wet_r;
        float wet_shaped_l = (lp_a >= 0.999f) ? w_hp_l : onepole_lp(&c->wet_lp_l, w_hp_l, lp_a);
        float wet_shaped_r = (lp_a >= 0.999f) ? w_hp_r : onepole_lp(&c->wet_lp_r, w_hp_r, lp_a);

        /* Preserve solid mono-safe sub-bass below low_cut on delay-based modes */
        if (low_cut_hz > 26.0f && (mode == MODLS_MODE_CHORUS || mode == MODLS_MODE_TRIMOD || mode == MODLS_MODE_UBER)) {
            wet_shaped_l += sub_l;
            wet_shaped_r += sub_r;
        }

        /* Apply mode transition fade */
        wet_shaped_l *= c->mode_fade;
        wet_shaped_r *= c->mode_fade;

        /* Apply stereo Width (0..200%) via M/S on the wet signal */
        float wm = 0.5f * (wet_shaped_l + wet_shaped_r);
        float ws = 0.5f * (wet_shaped_l - wet_shaped_r) * c->sm_width;
        wet_shaped_l = wm + ws;
        wet_shaped_r = wm - ws;

        /* Dry/Wet mix */
        float m = c->sm_mix;
        float proc_l = (1.0f - m) * dry_l + m * wet_shaped_l;
        float proc_r = (1.0f - m) * dry_r + m * wet_shaped_r;

        /* Output trim + soft safety ceiling */
        proc_l = safety_limit(proc_l * c->sm_out_gain);
        proc_r = safety_limit(proc_r * c->sm_out_gain);

        /* Click-free Bypass crossfade */
        float yl = proc_l + c->sm_bypass * (xl - proc_l);
        float yr = proc_r + c->sm_bypass * (xr - proc_r);

        out_l[i] = flush_denorm(yl);
        out_r[i] = flush_denorm(yr);

        float out_abs = fmaxf(fabsf(out_l[i]), fabsf(out_r[i]));
        if (out_abs > block_out_pk) block_out_pk = out_abs;

        last_lfo_l = clampf(mod_l, -1.0f, 1.0f);
        last_lfo_r = clampf(mod_r, -1.0f, 1.0f);
        last_eff_mod = clampf(depth_eff * (0.5f + 0.5f * over), 0.0f, 1.0f);
    }

    float blk_rel = powf(meter_rel, (float)nframes);
    c->meter_in  = fmaxf(block_in_pk,  c->meter_in  * blk_rel);
    c->meter_out = fmaxf(block_out_pk, c->meter_out * blk_rel);

    c->meters.in_peak_db  = lin_to_db(c->meter_in);
    c->meters.out_peak_db = lin_to_db(c->meter_out);
    c->meters.lfo_l       = last_lfo_l;
    c->meters.lfo_r       = last_lfo_r;
    c->meters.env_lvl     = clampf(c->env_fast, 0.0f, 1.0f);
    c->meters.eff_mod     = last_eff_mod;
    c->meters.fb_energy   = clampf(c->meter_fb, 0.0f, 1.0f);
    c->meters.host_bpm    = host_bpm;
}
