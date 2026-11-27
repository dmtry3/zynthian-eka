/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Eka Prism: Dual-Chain Morphing & Modulated Multi-Model Filter DSP implementation. */
#include "prism_core.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline float sanitizef(float v) {
    return (isfinite(v) && fabsf(v) < 1.0e15f) ? v : 0.0f;
}

static inline float flush_denorm(float v) {
    return (fabsf(v) < 1.0e-20f) ? 0.0f : v;
}

/* Fast rational Pade tanh approximation */
static inline float fast_tanh(float x) {
    if (x > 4.5f) return 1.0f;
    if (x < -4.5f) return -1.0f;
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

/* Warm asymmetric tube/diode saturator for MS-20 & Pre-Drive */
static inline float asym_sat(float x, float asym) {
    float xp = x + asym * 0.18f * x * x;
    return fast_tanh(xp) - fast_tanh(asym * 0.05f);
}

static inline uint32_t next_pow2(uint32_t v) {
    uint32_t p = 1;
    while (p < v) p <<= 1;
    return p;
}

static inline float rng_bipolar(uint32_t *st) {
    uint32_t x = *st;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *st = x ? x : 0x9E3779B9u;
    return ((float)(x & 0x00FFFFFFu) / 8388608.0f) - 1.0f;
}

static inline float dc_block_tick(PrismDCBlock *dc, float x) {
    float y = x - dc->x1 + 0.9965f * dc->y1;
    dc->x1 = x;
    dc->y1 = flush_denorm(y);
    return y;
}

/* Tempo divisions in quarter-note beats matching widget */
static const float kSyncBeats[16] = {
    16.0f, 8.0f, 4.0f, 3.0f, 2.0f, 1.5f, 4.0f / 3.0f, 1.0f,
    0.75f, 2.0f / 3.0f, 0.5f, 1.0f / 3.0f, 0.25f, 1.0f / 6.0f, 0.125f, 1.0f / 16.0f
};

static float snap_rate_to_bpm(float rate_hz, float bpm) {
    bpm = clampf(bpm, 20.0f, 400.0f);
    float q_sec = 60.0f / bpm;
    float log_t = logf(clampf(rate_hz, 0.05f, 40.0f));
    float best_hz = rate_hz;
    float best_err = 1.0e9f;
    for (int i = 0; i < 16; ++i) {
        float cand = 1.0f / (q_sec * kSyncBeats[i]);
        if (cand < 0.03f || cand > 45.0f) continue;
        float err = fabsf(logf(cand) - log_t);
        if (err < best_err) {
            best_err = err;
            best_hz = cand;
        }
    }
    return best_hz;
}

/* 8-step rhythmic gate/groove pattern for PRISM_LFO_STEP8 (-1..+1) */
static const float kStep8Pattern[8] = {
    1.0f, -0.45f, 0.55f, -0.85f, 0.80f, -0.20f, 0.35f, -1.0f
};

/* Formant table: 5 vowels (A, E, I, O, U) x 3 formants (Hz) + relative gains */
static const float kVowelFreqs[5][3] = {
    { 730.0f, 1090.0f, 2440.0f }, /* A */
    { 530.0f, 1840.0f, 2480.0f }, /* E */
    { 270.0f, 2290.0f, 3010.0f }, /* I */
    { 570.0f,  840.0f, 2410.0f }, /* O */
    { 300.0f,  870.0f, 2240.0f }  /* U */
};
static const float kVowelGains[3] = { 1.00f, 0.72f, 0.48f };

/* Model 0: Cytomic SVF with continuous LP -> BP -> HP -> Notch morph */
static float tick_svf(PrismFilterChan *ch, float x, float g, float res_norm, float morph_norm) {
    /* Q from 0.55 up to self-oscillation ~28 */
    float k = 1.85f * (1.0f - 0.965f * powf(res_norm, 0.75f));
    float a1 = 1.0f / (1.0f + g * (g + k));
    float a2 = g * a1;
    float a3 = g * a2;

    /* Nonlinear saturation inside feedback state for analog warmth at high resonance */
    float s1 = ch->ic1eq;
    float s2 = ch->ic2eq;
    if (res_norm > 0.65f) {
        s1 = fast_tanh(s1 * 0.85f) * 1.176f;
    }

    float v3 = x - s2;
    float v1 = a1 * s1 + a2 * v3;
    float v2 = s2 + a2 * s1 + a3 * v3;
    ch->ic1eq = flush_denorm(2.0f * v1 - s1);
    ch->ic2eq = flush_denorm(2.0f * v2 - s2);

    float lp = v2;
    float bp = v1;
    float hp = x - k * v1 - v2;
    float notch = lp + hp;

    /* Continuous 4-zone morph: 0.0=LP, 0.33=BP, 0.67=HP, 1.0=Notch */
    float m3 = morph_norm * 3.0f;
    if (m3 <= 1.0f) {
        return lp * (1.0f - m3) + bp * m3;
    } else if (m3 <= 2.0f) {
        float t = m3 - 1.0f;
        /* Boost BP slightly to match perceived loudness */
        return (bp * 1.25f) * (1.0f - t) + hp * t;
    } else {
        float t = m3 - 2.0f;
        return hp * (1.0f - t) + notch * t;
    }
}

/* Model 1: 4-Pole Transistor Ladder (24 dB/oct) with bass compensation and pole morphing */
static float tick_ladder(PrismFilterChan *ch, float x, float g, float res_norm, float morph_norm) {
    /* Map g = tan(pi*fc/sr) to one-pole coefficient alpha = g / (1 + g) */
    float alpha = g / (1.0f + g);
    float k = 4.15f * res_norm * res_norm * (0.45f + 0.55f * res_norm);

    /* Bass compensation: mix a portion of input back into feedback so low end stays fat */
    float fb = ch->lad_s[3] - 0.32f * res_norm * x;
    float u = fast_tanh(x - k * fast_tanh(fb * 0.9f));

    float v0 = u;
    for (int p = 0; p < 4; ++p) {
        float s = ch->lad_s[p];
        float y = s + alpha * (v0 - s);
        /* Gentle per-stage transistor saturation */
        y = fast_tanh(y * 1.05f) * 0.952f;
        ch->lad_s[p] = flush_denorm(y);
        v0 = y;
    }

    /* Multimode pole combination via morph_norm:
       0.00 = 24 dB Lowpass (s3)
       0.33 = 12 dB Lowpass (s1)
       0.67 = 4-pole Bandpass (2*(s1 - s2))
       1.00 = 2-pole Highpass (u - 2*s0 + s1) */
    float lp24 = ch->lad_s[3] * (1.0f + 0.45f * res_norm);
    float lp12 = ch->lad_s[1] * (1.0f + 0.30f * res_norm);
    float bp   = (ch->lad_s[1] - ch->lad_s[3]) * 2.1f;
    float hp   = (u - 2.0f * ch->lad_s[0] + ch->lad_s[1]) * 1.15f;

    float m3 = morph_norm * 3.0f;
    if (m3 <= 1.0f) {
        return lp24 * (1.0f - m3) + lp12 * m3;
    } else if (m3 <= 2.0f) {
        float t = m3 - 1.0f;
        return lp12 * (1.0f - t) + bp * t;
    } else {
        float t = m3 - 2.0f;
        return bp * (1.0f - t) + hp * t;
    }
}

/* Model 2: MS-20 / Steiner Sallen-Key Screamer with diode feedback clipping */
static float tick_scream(PrismFilterChan *ch, float x, float g, float res_norm, float morph_norm) {
    /* Morph controls input topology (LP -> BP -> HP) plus asymmetric diode bite */
    float m2 = morph_norm * 2.0f;
    float in_lp = clampf(1.0f - m2, 0.0f, 1.0f);
    float in_bp = 1.0f - fabsf(m2 - 1.0f);
    float in_hp = clampf(m2 - 1.0f, 0.0f, 1.0f);

    /* Resonance up to K ~ 2.0 triggers aggressive self-oscillation */
    float k = 1.98f * powf(res_norm, 0.65f);
    float g1 = 1.0f / (1.0f + g);

    /* Asymmetric diode clipper in the feedback path */
    float bp_est = g1 * (ch->sk_s1 - ch->sk_s2);
    float fb_clip = asym_sat(k * bp_est * 1.65f, 0.45f + 0.4f * morph_norm) * 0.72f;

    /* Drive input slightly harder as resonance increases for signature MS-20 scream */
    float xin = asym_sat(x * (1.1f + 0.8f * res_norm), 0.35f);

    float u0 = xin * in_lp + fb_clip;
    float v1 = g1 * (ch->sk_s1 + g * (u0 + xin * in_hp));
    ch->sk_s1 = flush_denorm(2.0f * v1 - ch->sk_s1);

    float u2 = v1 + xin * in_bp * 0.85f;
    float v2 = g1 * (ch->sk_s2 + g * (u2 - fb_clip * 0.5f));
    ch->sk_s2 = flush_denorm(2.0f * v2 - ch->sk_s2);

    float lp = v2;
    float bp = (v1 - v2) * 1.85f;
    float hp = xin - v1 - bp * 0.35f;

    return (lp * in_lp + bp * in_bp + hp * in_hp) * 1.1f;
}

/* Model 3: Tuned Comb Resonator (Karplus-Strong / Positive-Negative Comb) */
static float tick_comb(PrismFilterChan *ch, float x, float cutoff_hz, float sr, float res_norm, float morph_norm) {
    if (!ch->comb_buf || ch->comb_len < 8) return x;

    /* Delay length in samples from cutoff_hz (20 Hz .. 8000 Hz) */
    float fc = clampf(cutoff_hz, 20.0f, 8000.0f);
    float delay_samp = clampf(sr / fc, 2.0f, (float)(ch->comb_len - 4));

    uint32_t w = ch->comb_wpos;
    uint32_t mask = ch->comb_mask;

    /* Hermite 4-point fractional delay read */
    float rpos = (float)w - delay_samp;
    while (rpos < 0.0f) rpos += (float)ch->comb_len;
    uint32_t ri = (uint32_t)rpos;
    float frac = rpos - (float)ri;

    float ym1 = ch->comb_buf[(ri - 1u) & mask];
    float y0  = ch->comb_buf[ri & mask];
    float y1  = ch->comb_buf[(ri + 1u) & mask];
    float y2  = ch->comb_buf[(ri + 2u) & mask];

    float c0 = y0;
    float c1 = 0.5f * (y1 - ym1);
    float c2 = ym1 - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
    float c3 = 0.5f * (y2 - ym1) + 1.5f * (y0 - y1);
    float d_out = ((c3 * frac + c2) * frac + c1) * frac + c0;

    /* Morph controls polarity & damping:
       morph < 0.5: negative comb (hollow square/clarinet odd harmonics)
       morph > 0.5: positive comb (bright string/flange all harmonics) */
    float polarity = (morph_norm - 0.5f) * 2.0f; /* -1..+1 */
    float sign = (polarity >= 0.0f) ? 1.0f : -1.0f;
    float damp = 0.15f + 0.65f * (1.0f - fabsf(polarity));

    ch->comb_lp = flush_denorm(ch->comb_lp + (1.0f - damp) * (d_out - ch->comb_lp));
    float fb_gain = sign * (0.18f + 0.805f * powf(res_norm, 0.55f));
    float loop = fast_tanh(x + fb_gain * ch->comb_lp);

    ch->comb_buf[w & mask] = flush_denorm(loop);
    ch->comb_wpos = (w + 1u) & mask;

    return x * 0.45f + d_out * 0.85f;
}

/* Model 4: Formant Vowel Filter (3 parallel resonant formant bands A-E-I-O-U) */
static float tick_formant(PrismFilterChan *ch, float x, float cutoff_hz, float sr, float res_norm, float morph_norm) {
    /* Morph selects vowel A -> E -> I -> O -> U (0..4) */
    float vpos = clampf(morph_norm, 0.0f, 1.0f) * 4.0f;
    int v0 = (int)vpos;
    if (v0 > 3) v0 = 3;
    int v1 = v0 + 1;
    float vf = vpos - (float)v0;

    /* Cutoff shifts overall formant pitch (1000 Hz = 1.0x reference vocal tract) */
    float shift = powf(clampf(cutoff_hz, 80.0f, 12000.0f) / 1000.0f, 0.55f);
    shift = clampf(shift, 0.35f, 2.8f);

    /* Resonance sharpens formant Q (from Q=3.5 to Q=24) */
    float q = 3.5f + 20.5f * res_norm * res_norm;
    float k = 1.0f / q;

    float sum = 0.0f;
    for (int b = 0; b < PRISM_FORMANT_BANDS; ++b) {
        float f_hz = (kVowelFreqs[v0][b] * (1.0f - vf) + kVowelFreqs[v1][b] * vf) * shift;
        f_hz = clampf(f_hz, 50.0f, sr * 0.44f);
        float g = tanf((float)M_PI * f_hz / sr);
        float a1 = 1.0f / (1.0f + g * (g + k));
        float a2 = g * a1;
        float a3 = g * a2;

        float s1 = ch->fm_ic1[b];
        float s2 = ch->fm_ic2[b];
        float v3 = x - s2;
        float bp = a1 * s1 + a2 * v3;
        float lp = s2 + a2 * s1 + a3 * v3;
        ch->fm_ic1[b] = flush_denorm(2.0f * bp - s1);
        ch->fm_ic2[b] = flush_denorm(2.0f * lp - s2);

        sum += bp * kVowelGains[b];
    }
    /* Normalize output gain across Q settings */
    float norm = 1.15f + 0.55f * (1.0f - res_norm);
    return fast_tanh(sum * norm);
}

/* Model 5: 6-Stage Allpass Phaser with spread & notch/peak morph */
static float tick_phaser(PrismFilterChan *ch, float x, float cutoff_hz, float sr, float res_norm, float morph_norm) {
    float fb_gain = 0.90f * res_norm * ((morph_norm >= 0.5f) ? 1.0f : -1.0f);
    float u = fast_tanh(x + fb_gain * ch->ph_fb);

    /* Morph also spreads the 6 allpass poles around cutoff_hz */
    float spread = 0.65f + 0.75f * fabsf(morph_norm - 0.5f) * 2.0f;
    float base_f = clampf(cutoff_hz, 40.0f, sr * 0.42f);

    float v = u;
    for (int s = 0; s < PRISM_PHASER_STAGES; ++s) {
        float ratio = powf(spread, (float)(s - 2) * 0.45f);
        float f_s = clampf(base_f * ratio, 30.0f, sr * 0.45f);
        float g = tanf((float)M_PI * f_s / sr);
        float a = (1.0f - g) / (1.0f + g);
        float y = -a * v + ch->ph_z[s];
        ch->ph_z[s] = flush_denorm(v + a * y);
        v = y;
    }
    ch->ph_fb = flush_denorm(v);
    return 0.55f * (x + v);
}

static float process_filter_model(PrismFilterChan *ch,
                                  int model,
                                  float x,
                                  float cutoff_hz,
                                  float g,
                                  float sr,
                                  float res_norm,
                                  float morph_norm) {
    switch (model) {
        case PRISM_MODEL_SVF:
        default:
            return tick_svf(ch, x, g, res_norm, morph_norm);
        case PRISM_MODEL_LADDER:
            return tick_ladder(ch, x, g, res_norm, morph_norm);
        case PRISM_MODEL_SCREAM:
            return tick_scream(ch, x, g, res_norm, morph_norm);
        case PRISM_MODEL_COMB:
            return tick_comb(ch, x, cutoff_hz, sr, res_norm, morph_norm);
        case PRISM_MODEL_FORMANT:
            return tick_formant(ch, x, cutoff_hz, sr, res_norm, morph_norm);
        case PRISM_MODEL_PHASER:
            return tick_phaser(ch, x, cutoff_hz, sr, res_norm, morph_norm);
    }
}

static float eval_lfo_wave(int shape,
                           double ph,
                           float sh_val,
                           float chaos_val) {
    switch (shape) {
        case PRISM_LFO_SINE:
        default:
            return sinf((float)(2.0 * M_PI * ph));
        case PRISM_LFO_TRI: {
            float p = (float)ph;
            return (p < 0.5f) ? (4.0f * p - 1.0f) : (3.0f - 4.0f * p);
        }
        case PRISM_LFO_SAW:
            return 2.0f * (float)ph - 1.0f;
        case PRISM_LFO_SQUARE:
            return (ph < 0.5) ? 1.0f : -1.0f;
        case PRISM_LFO_SH:
            return sh_val;
        case PRISM_LFO_CHAOS:
            return chaos_val;
        case PRISM_LFO_STEP8: {
            double s = ph * 8.0;
            int idx = (int)s & 7;
            int nxt = (idx + 1) & 7;
            float frac = (float)(s - (int)s);
            /* Smooth edges in final 18% of step so rhythmic gating never clicks */
            float t = (frac > 0.82f) ? ((frac - 0.82f) / 0.18f) : 0.0f;
            return kStep8Pattern[idx] * (1.0f - t) + kStep8Pattern[nxt] * t;
        }
    }
}

void prism_params_default(PrismParams *p) {
    if (!p) return;
    memset(p, 0, sizeof(*p));

    p->model_a        = PRISM_MODEL_SVF;
    p->cutoff_a_hz    = 650.0f;
    p->res_a_pct      = 35.0f;
    p->morph_a_pct    = 0.0f;

    p->model_b        = PRISM_MODEL_LADDER;
    p->cutoff_b_hz    = 1800.0f;
    p->res_b_pct      = 40.0f;
    p->morph_b_pct    = 25.0f;

    p->routing        = PRISM_ROUTE_SERIAL;
    p->link_mode      = PRISM_LINK_FREE;
    p->ab_balance_pct = 0.0f;
    p->invert_b       = 0;

    p->lfo_shape      = PRISM_LFO_SINE;
    p->lfo_rate_hz    = 0.80f;
    p->lfo_sync       = 0;
    p->lfo_phase_deg  = 90.0f;

    p->lfo_mod_pct    = 25.0f;
    p->morph_mod_pct  = 0.0f;
    p->env_mod_pct    = 0.0f;
    p->env_speed_pct  = 50.0f;

    p->drive_pct      = 15.0f;
    p->mix_pct        = 100.0f;
    p->output_db      = 0.0f;
    p->bypass         = 0;

    p->host_bpm       = 120.0f;
    p->midi_note      = -1;
}

int prism_core_init(PrismCore *c, double sample_rate) {
    if (!c) return -1;
    memset(c, 0, sizeof(*c));
    if (sample_rate < 8000.0 || sample_rate > 384000.0) sample_rate = 48000.0;
    c->sample_rate = sample_rate;
    c->sr_f = (float)sample_rate;

    /* Comb buffer for 20 Hz fundamental (~60 ms) */
    uint32_t comb_len = next_pow2((uint32_t)(sample_rate * 0.065) + 16u);
    for (int ch = 0; ch < 2; ++ch) {
        c->chain_a[ch].comb_buf = (float *)calloc(comb_len, sizeof(float));
        c->chain_b[ch].comb_buf = (float *)calloc(comb_len, sizeof(float));
        if (!c->chain_a[ch].comb_buf || !c->chain_b[ch].comb_buf) {
            prism_core_free(c);
            return -1;
        }
        c->chain_a[ch].comb_len  = comb_len;
        c->chain_a[ch].comb_mask = comb_len - 1u;
        c->chain_b[ch].comb_len  = comb_len;
        c->chain_b[ch].comb_mask = comb_len - 1u;
    }

    prism_core_reset(c);
    return 0;
}

static void reset_filter_chan(PrismFilterChan *fc) {
    fc->ic1eq = 0.0f;
    fc->ic2eq = 0.0f;
    for (int i = 0; i < 4; ++i) fc->lad_s[i] = 0.0f;
    fc->sk_s1 = 0.0f;
    fc->sk_s2 = 0.0f;
    if (fc->comb_buf && fc->comb_len > 0) {
        memset(fc->comb_buf, 0, fc->comb_len * sizeof(float));
    }
    fc->comb_wpos = 0;
    fc->comb_lp = 0.0f;
    for (int i = 0; i < PRISM_FORMANT_BANDS; ++i) {
        fc->fm_ic1[i] = 0.0f;
        fc->fm_ic2[i] = 0.0f;
    }
    for (int i = 0; i < PRISM_PHASER_STAGES; ++i) {
        fc->ph_z[i] = 0.0f;
    }
    fc->ph_fb = 0.0f;
}

void prism_core_reset(PrismCore *c) {
    if (!c) return;
    for (int ch = 0; ch < 2; ++ch) {
        reset_filter_chan(&c->chain_a[ch]);
        reset_filter_chan(&c->chain_b[ch]);
    }
    memset(&c->dc_l, 0, sizeof(c->dc_l));
    memset(&c->dc_r, 0, sizeof(c->dc_r));

    c->lfo_phase   = 0.0;
    c->prev_ph_a   = 0.0;
    c->prev_ph_b   = 0.25;
    c->rng_state   = 0x6D2B79F5u;
    c->sh_target_a = 0.0f;
    c->sh_target_b = 0.0f;
    c->sh_cur_a    = 0.0f;
    c->sh_cur_b    = 0.0f;
    c->chaos_x_a   = 0.35f;
    c->chaos_y_a   = -0.25f;
    c->chaos_x_b   = -0.35f;
    c->chaos_y_b   = 0.25f;

    c->env_lvl     = 0.0f;
    c->env_sc_hp   = 0.0f;

    c->sm_cut_a_oct = log2f(650.0f);
    c->sm_cut_b_oct = log2f(1800.0f);
    c->sm_res_a     = 0.35f;
    c->sm_res_b     = 0.40f;
    c->sm_morph_a   = 0.0f;
    c->sm_morph_b   = 0.25f;
    c->sm_ab_bal    = 0.0f;
    c->sm_lfo_mod   = 0.25f;
    c->sm_morph_mod = 0.0f;
    c->sm_env_mod   = 0.0f;
    c->sm_drive     = 0.15f;
    c->sm_mix       = 1.0f;
    c->sm_out_gain  = 1.0f;
    c->sm_bypass    = 0.0f;

    c->meter_in  = 0.0f;
    c->meter_out = 0.0f;
    c->meters.in_peak_db  = -60.0f;
    c->meters.out_peak_db = -60.0f;
    c->meters.eff_cut_a   = 650.0f;
    c->meters.eff_cut_b   = 1800.0f;
    c->meters.eff_morph_a = 0.0f;
    c->meters.eff_morph_b = 25.0f;
    c->meters.lfo_a       = 0.0f;
    c->meters.lfo_b       = 0.0f;
    c->meters.env_lvl     = 0.0f;
    c->meters.host_bpm    = 120.0f;
}

void prism_core_free(PrismCore *c) {
    if (!c) return;
    for (int ch = 0; ch < 2; ++ch) {
        free(c->chain_a[ch].comb_buf);
        c->chain_a[ch].comb_buf = NULL;
        free(c->chain_b[ch].comb_buf);
        c->chain_b[ch].comb_buf = NULL;
    }
}

void prism_core_process(PrismCore *c,
                        const PrismParams *p,
                        const float *in_l,
                        const float *in_r,
                        float *out_l,
                        float *out_r,
                        uint32_t nframes) {
    if (!c || !p || !in_l || !in_r || !out_l || !out_r) return;

    const float sr = c->sr_f;
    const float nyq_limit = fminf(18000.0f, sr * 0.44f);

    int model_a   = (p->model_a >= 0 && p->model_a < PRISM_MODEL_COUNT) ? p->model_a : 0;
    int model_b   = (p->model_b >= 0 && p->model_b < PRISM_MODEL_COUNT) ? p->model_b : 0;
    int routing   = (p->routing >= 0 && p->routing < PRISM_ROUTE_COUNT) ? p->routing : 0;
    int link_mode = (p->link_mode >= 0 && p->link_mode < PRISM_LINK_COUNT) ? p->link_mode : 0;
    int lfo_shape = (p->lfo_shape >= 0 && p->lfo_shape < PRISM_LFO_COUNT) ? p->lfo_shape : 0;
    float sign_b  = p->invert_b ? -1.0f : 1.0f;

    float tgt_cut_a = clampf(p->cutoff_a_hz, 20.0f, nyq_limit);
    float tgt_cut_b = clampf(p->cutoff_b_hz, 20.0f, nyq_limit);

    /* Optional MIDI note tracking shifts Cutoff A & B relative to C4 (261.63 Hz) */
    if (p->midi_note >= 0 && p->midi_note <= 127) {
        float note_mul = exp2f(((float)p->midi_note - 60.0f) / 12.0f);
        tgt_cut_a = clampf(tgt_cut_a * note_mul, 20.0f, nyq_limit);
        tgt_cut_b = clampf(tgt_cut_b * note_mul, 20.0f, nyq_limit);
    }

    float tgt_cut_a_oct = log2f(tgt_cut_a);
    float tgt_cut_b_oct = log2f(tgt_cut_b);
    float tgt_res_a     = clampf(p->res_a_pct, 0.0f, 100.0f) * 0.01f;
    float tgt_res_b     = clampf(p->res_b_pct, 0.0f, 100.0f) * 0.01f;
    float tgt_morph_a   = clampf(p->morph_a_pct, 0.0f, 100.0f) * 0.01f;
    float tgt_morph_b   = clampf(p->morph_b_pct, 0.0f, 100.0f) * 0.01f;
    float tgt_ab_bal    = clampf(p->ab_balance_pct, -100.0f, 100.0f) * 0.01f;
    float tgt_lfo_mod   = clampf(p->lfo_mod_pct, -100.0f, 100.0f) * 0.01f;
    float tgt_morph_mod = clampf(p->morph_mod_pct, -100.0f, 100.0f) * 0.01f;
    float tgt_env_mod   = clampf(p->env_mod_pct, -100.0f, 100.0f) * 0.01f;
    float tgt_drive     = clampf(p->drive_pct, 0.0f, 100.0f) * 0.01f;
    float tgt_mix       = clampf(p->mix_pct, 0.0f, 100.0f) * 0.01f;
    float tgt_out_gain  = powf(10.0f, clampf(p->output_db, -18.0f, 12.0f) / 20.0f);
    float tgt_bypass    = p->bypass ? 1.0f : 0.0f;

    float host_bpm = clampf(p->host_bpm, 20.0f, 400.0f);
    float rate_hz  = clampf(p->lfo_rate_hz, 0.05f, 40.0f);
    if (p->lfo_sync) {
        rate_hz = snap_rate_to_bpm(rate_hz, host_bpm);
    }
    double lfo_inc = (double)rate_hz / (double)sr;
    double phase_off_b = (double)clampf(p->lfo_phase_deg, 0.0f, 180.0f) / 360.0;

    /* Envelope Follower attack/release from env_speed_pct (0=slow groove, 100=snappy transient) */
    float spd = clampf(p->env_speed_pct, 0.0f, 100.0f) * 0.01f;
    float att_ms = 35.0f * powf(0.06f, spd);  /* 35 ms .. 2.1 ms */
    float rel_ms = 420.0f * powf(0.07f, spd); /* 420 ms .. 29 ms */
    float env_att = 1.0f - expf(-1000.0f / (att_ms * sr));
    float env_rel = 1.0f - expf(-1000.0f / (rel_ms * sr));

    /* Parameter smoothing coefficient (~6 ms) */
    float sm_a = 1.0f - expf(-1.0f / (0.006f * sr));
    float sh_slew = 1.0f - expf(-1.0f / (0.004f * sr));

    float peak_in  = 0.0f;
    float peak_out = 0.0f;
    float last_cut_a = tgt_cut_a;
    float last_cut_b = tgt_cut_b;
    float last_morph_a = tgt_morph_a;
    float last_morph_b = tgt_morph_b;
    float last_lfo_a = 0.0f;
    float last_lfo_b = 0.0f;

    /* Cached tanf coefficients updated every 8 samples (or per sample if audio-rate LFO > 15 Hz) */
    int coeff_step = (rate_hz > 15.0f) ? 1 : 8;
    float g_a = tanf((float)M_PI * tgt_cut_a / sr);
    float g_b = tanf((float)M_PI * tgt_cut_b / sr);
    float cur_cut_a = tgt_cut_a;
    float cur_cut_b = tgt_cut_b;

    for (uint32_t i = 0; i < nframes; ++i) {
        float xl = sanitizef(in_l[i]);
        float xr = sanitizef(in_r[i]);

        float abs_in = fmaxf(fabsf(xl), fabsf(xr));
        if (abs_in > peak_in) peak_in = abs_in;

        /* Parameter smoothing */
        c->sm_cut_a_oct += sm_a * (tgt_cut_a_oct - c->sm_cut_a_oct);
        c->sm_cut_b_oct += sm_a * (tgt_cut_b_oct - c->sm_cut_b_oct);
        c->sm_res_a     += sm_a * (tgt_res_a     - c->sm_res_a);
        c->sm_res_b     += sm_a * (tgt_res_b     - c->sm_res_b);
        c->sm_morph_a   += sm_a * (tgt_morph_a   - c->sm_morph_a);
        c->sm_morph_b   += sm_a * (tgt_morph_b   - c->sm_morph_b);
        c->sm_ab_bal    += sm_a * (tgt_ab_bal    - c->sm_ab_bal);
        c->sm_lfo_mod   += sm_a * (tgt_lfo_mod   - c->sm_lfo_mod);
        c->sm_morph_mod += sm_a * (tgt_morph_mod - c->sm_morph_mod);
        c->sm_env_mod   += sm_a * (tgt_env_mod   - c->sm_env_mod);
        c->sm_drive     += sm_a * (tgt_drive     - c->sm_drive);
        c->sm_mix       += sm_a * (tgt_mix       - c->sm_mix);
        c->sm_out_gain  += sm_a * (tgt_out_gain  - c->sm_out_gain);
        c->sm_bypass    += sm_a * (tgt_bypass    - c->sm_bypass);

        /* Input Envelope Follower with gentle transient focus */
        float det = clampf(abs_in * 1.85f, 0.0f, 1.0f);
        float ec = (det > c->env_lvl) ? env_att : env_rel;
        c->env_lvl = flush_denorm(c->env_lvl + ec * (det - c->env_lvl));

        /* Advance LFO phase */
        double ph_a = c->lfo_phase;
        double ph_b = ph_a + phase_off_b;
        if (ph_b >= 1.0) ph_b -= 1.0;

        /* Trigger S&H / Chaos updates on wrap or sub-cycle */
        if (ph_a < c->prev_ph_a) {
            c->sh_target_a = rng_bipolar(&c->rng_state);
        }
        if (ph_b < c->prev_ph_b) {
            c->sh_target_b = rng_bipolar(&c->rng_state);
        }
        c->prev_ph_a = ph_a;
        c->prev_ph_b = ph_b;

        c->sh_cur_a += sh_slew * (c->sh_target_a - c->sh_cur_a);
        c->sh_cur_b += sh_slew * (c->sh_target_b - c->sh_cur_b);

        /* Smooth Lorenz/coupled random walk for Chaos LFO */
        float dt = (float)lfo_inc * 3.8f;
        c->chaos_x_a = clampf(c->chaos_x_a + dt * (c->sh_target_a - c->chaos_x_a * 0.45f), -1.0f, 1.0f);
        c->chaos_y_a = clampf(c->chaos_y_a + dt * (c->chaos_x_a - c->chaos_y_a), -1.0f, 1.0f);
        c->chaos_x_b = clampf(c->chaos_x_b + dt * (c->sh_target_b - c->chaos_x_b * 0.45f), -1.0f, 1.0f);
        c->chaos_y_b = clampf(c->chaos_y_b + dt * (c->chaos_x_b - c->chaos_y_b), -1.0f, 1.0f);

        c->lfo_phase += lfo_inc;
        while (c->lfo_phase >= 1.0) c->lfo_phase -= 1.0;

        float lfo_a = eval_lfo_wave(lfo_shape, ph_a, c->sh_cur_a, c->chaos_y_a * 1.35f);
        float lfo_b = eval_lfo_wave(lfo_shape, ph_b, c->sh_cur_b, c->chaos_y_b * 1.35f);
        lfo_a = clampf(lfo_a, -1.0f, 1.0f);
        lfo_b = clampf(lfo_b, -1.0f, 1.0f);
        last_lfo_a = lfo_a;
        last_lfo_b = lfo_b;

        /* Cutoff Link / Offset / Mirror calculation in octaves */
        float base_a_oct = c->sm_cut_a_oct;
        float base_b_oct = c->sm_cut_b_oct;
        if (link_mode == PRISM_LINK_OFFSET) {
            /* Filter B knob acts as relative offset around 1000 Hz (log2(1000) = 9.96578) */
            base_b_oct = base_a_oct + (c->sm_cut_b_oct - 9.965784f);
        } else if (link_mode == PRISM_LINK_MIRROR) {
            /* Filter B moves in opposite direction around 1000 Hz pivot */
            base_b_oct = 2.0f * 9.965784f - base_a_oct + (c->sm_cut_b_oct - 9.965784f) * 0.5f;
        }

        /* Pitch modulation in octaves (up to +/- 4.0 octaves for LFO, +/- 4.5 octaves for Env) */
        float env_oct_a = c->sm_env_mod * c->env_lvl * 4.5f;
        float env_oct_b = (link_mode == PRISM_LINK_MIRROR) ? -env_oct_a : env_oct_a;

        float mod_oct_a = base_a_oct + c->sm_lfo_mod * lfo_a * 3.8f + env_oct_a;
        float mod_oct_b = base_b_oct + c->sm_lfo_mod * lfo_b * 3.8f + env_oct_b;

        if ((i & (uint32_t)(coeff_step - 1)) == 0) {
            cur_cut_a = clampf(exp2f(mod_oct_a), 20.0f, nyq_limit);
            cur_cut_b = clampf(exp2f(mod_oct_b), 20.0f, nyq_limit);
            g_a = tanf((float)M_PI * cur_cut_a / sr);
            g_b = tanf((float)M_PI * cur_cut_b / sr);
            last_cut_a = cur_cut_a;
            last_cut_b = cur_cut_b;
        }

        /* Morph modulation from LFO + Envelope */
        float eff_morph_a = clampf(c->sm_morph_a + c->sm_morph_mod * (0.65f * lfo_a + 0.55f * (c->env_lvl - 0.3f)), 0.0f, 1.0f);
        float eff_morph_b = clampf(c->sm_morph_b + c->sm_morph_mod * (0.65f * lfo_b + 0.55f * (c->env_lvl - 0.3f)), 0.0f, 1.0f);
        last_morph_a = eff_morph_a;
        last_morph_b = eff_morph_b;

        /* Analog Pre-Drive saturation stage (compensated for gain so driving adds harmonics, not just volume) */
        float drv = c->sm_drive;
        float drv_gain = 1.0f + drv * 4.5f;
        float drv_comp = 1.0f / (1.0f + drv * 1.55f);
        float dl = (drv > 0.001f) ? (asym_sat(xl * drv_gain, 0.25f * drv) * drv_comp) : xl;
        float dr = (drv > 0.001f) ? (asym_sat(xr * drv_gain, 0.25f * drv) * drv_comp) : xr;

        /* Balance gains for A and B (-1..+1): equal-power crossfade / blend */
        float bal = c->sm_ab_bal;
        float gain_a = clampf(1.0f - fmaxf(0.0f, bal), 0.0f, 1.0f);
        float gain_b = clampf(1.0f + fminf(0.0f, bal), 0.0f, 1.0f);

        float wet_l = 0.0f;
        float wet_r = 0.0f;

        if (routing == PRISM_ROUTE_SERIAL) {
            /* Serial: Input -> Filter A -> (crossfade by A/B Balance) -> Filter B */
            float al = process_filter_model(&c->chain_a[0], model_a, dl, cur_cut_a, g_a, sr, c->sm_res_a, eff_morph_a);
            float ar = process_filter_model(&c->chain_a[1], model_a, dr, cur_cut_a, g_a, sr, c->sm_res_a, eff_morph_a);

            /* Feed Filter B with blend of A and dry-driven input according to gain_a */
            float in_bl = al * gain_a + dl * (1.0f - gain_a);
            float in_br = ar * gain_a + dr * (1.0f - gain_a);

            float bl = process_filter_model(&c->chain_b[0], model_b, in_bl, cur_cut_b, g_b, sr, c->sm_res_b, eff_morph_b) * sign_b;
            float br = process_filter_model(&c->chain_b[1], model_b, in_br, cur_cut_b, g_b, sr, c->sm_res_b, eff_morph_b) * sign_b;

            wet_l = al * (1.0f - gain_b) + bl * gain_b;
            wet_r = ar * (1.0f - gain_b) + br * gain_b;
        } else if (routing == PRISM_ROUTE_PARALLEL) {
            /* Parallel: Filter A + Filter B summed (with phase invert B option for deep notch cancellation) */
            float al = process_filter_model(&c->chain_a[0], model_a, dl, cur_cut_a, g_a, sr, c->sm_res_a, eff_morph_a);
            float ar = process_filter_model(&c->chain_a[1], model_a, dr, cur_cut_a, g_a, sr, c->sm_res_a, eff_morph_a);
            float bl = process_filter_model(&c->chain_b[0], model_b, dl, cur_cut_b, g_b, sr, c->sm_res_b, eff_morph_b) * sign_b;
            float br = process_filter_model(&c->chain_b[1], model_b, dr, cur_cut_b, g_b, sr, c->sm_res_b, eff_morph_b) * sign_b;

            float norm = 0.72f;
            wet_l = (al * gain_a + bl * gain_b) * norm;
            wet_r = (ar * gain_a + br * gain_b) * norm;
        } else {
            /* Stereo Split: Left -> Filter A, Right -> Filter B */
            float al = process_filter_model(&c->chain_a[0], model_a, dl, cur_cut_a, g_a, sr, c->sm_res_a, eff_morph_a);
            float br = process_filter_model(&c->chain_b[1], model_b, dr, cur_cut_b, g_b, sr, c->sm_res_b, eff_morph_b) * sign_b;
            wet_l = al * gain_a;
            wet_r = br * gain_b;
        }

        /* DC blocking and soft post-limiting */
        wet_l = fast_tanh(dc_block_tick(&c->dc_l, sanitizef(wet_l)) * 0.92f) * 1.08f;
        wet_r = fast_tanh(dc_block_tick(&c->dc_r, sanitizef(wet_r)) * 0.92f) * 1.08f;

        /* Dry/Wet Mix + Output Gain + Click-free Bypass */
        float mix = c->sm_mix;
        float proc_l = (xl * (1.0f - mix) + wet_l * mix) * c->sm_out_gain;
        float proc_r = (xr * (1.0f - mix) + wet_r * mix) * c->sm_out_gain;

        float byp = c->sm_bypass;
        float yl = proc_l * (1.0f - byp) + xl * byp;
        float yr = proc_r * (1.0f - byp) + xr * byp;

        out_l[i] = yl;
        out_r[i] = yr;

        float abs_out = fmaxf(fabsf(yl), fabsf(yr));
        if (abs_out > peak_out) peak_out = abs_out;
    }

    /* Update peak hold meters */
    c->meter_in  = fmaxf(peak_in,  c->meter_in  * 0.85f);
    c->meter_out = fmaxf(peak_out, c->meter_out * 0.85f);

    c->meters.in_peak_db  = (c->meter_in  > 1.0e-3f) ? clampf(20.0f * log10f(c->meter_in),  -60.0f, 12.0f) : -60.0f;
    c->meters.out_peak_db = (c->meter_out > 1.0e-3f) ? clampf(20.0f * log10f(c->meter_out), -60.0f, 12.0f) : -60.0f;
    c->meters.eff_cut_a   = last_cut_a;
    c->meters.eff_cut_b   = last_cut_b;
    c->meters.eff_morph_a = last_morph_a * 100.0f;
    c->meters.eff_morph_b = last_morph_b * 100.0f;
    c->meters.lfo_a       = last_lfo_a;
    c->meters.lfo_b       = last_lfo_b;
    c->meters.env_lvl     = clampf(c->env_lvl, 0.0f, 1.0f);
    c->meters.host_bpm    = host_bpm;
}
