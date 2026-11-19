/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tone.h"
#include <math.h>
#include <string.h>
static const float K12_FLAT   = 1.4142135623730951f; /* 2nd-order Butterworth */
static const float K12_RES    = 0.30f;               /* ~+10.5 dB peak */
static const float K24_1_FLAT = 0.7653668647301795f; /* 4th-order Butterworth stage 1 */
static const float K24_2_FLAT = 1.8477590650225735f; /* 4th-order Butterworth stage 2 */
static const float K24_1_RES  = 0.30f;
static const float K24_2_RES  = 0.95f;               /* combined ~+10.9 dB peak */
static float coefficient(float hz, float fallback, float low, float high, double rate) {
    if (!isfinite(hz)) hz = fallback;
    if (hz < low) hz = low;
    if (hz > high) hz = high;
    if (hz > rate * .45) hz = (float)(rate * .45);
    return (float)tan(3.14159265358979323846 * hz / rate);
}
static void damping(int slope_24, float res_norm, float *k1, float *k2) {
    if (slope_24) {
        *k1 = K24_1_FLAT * powf(K24_1_RES / K24_1_FLAT, res_norm);
        *k2 = K24_2_FLAT * powf(K24_2_RES / K24_2_FLAT, res_norm);
    } else {
        *k1 = K12_FLAT * powf(K12_RES / K12_FLAT, res_norm);
        *k2 = K12_FLAT;
    }
}
static void snap_targets(EkaTone *t) {
    t->high_g = t->target_high_g; t->low_g = t->target_low_g;
    t->high_k1 = t->target_high_k1; t->high_k2 = t->target_high_k2;
    t->low_k1 = t->target_low_k1; t->low_k2 = t->target_low_k2;
    t->high_24 = t->target_high_24; t->low_24 = t->target_low_24;
}
void eka_tone_reset(EkaTone *t, double rate) {
    memset(t, 0, sizeof(*t));
    t->rate = rate;
    t->smoothing = (float)(1 - exp(-1 / (rate * .010)));
    t->high_k1 = t->target_high_k1 = K12_FLAT;
    t->high_k2 = t->target_high_k2 = K12_FLAT;
    t->low_k1 = t->target_low_k1 = K12_FLAT;
    t->low_k2 = t->target_low_k2 = K12_FLAT;
}
void eka_tone_set(EkaTone *t, float high, float low, int enabled) {
    t->target_high_g = coefficient(high, 20, 20, 4000, t->rate);
    t->target_low_g = coefficient(low, 20000, 200, 20000, t->rate);
    t->target_mix = enabled ? 1 : 0;
    if (!t->started) {
        t->high_g = t->target_high_g;
        t->low_g = t->target_low_g;
        t->mix = t->target_mix;
    }
}
void eka_tone_shape(EkaTone *t, float resonance_pct, int high_24, int low_24,
                    int high_res_on, int low_res_on) {
    float r = isfinite(resonance_pct) ? resonance_pct * .01f : 0;
    if (r < 0) r = 0;
    if (r > 1) r = 1;
    t->target_high_24 = high_24 ? 1.f : 0.f;
    t->target_low_24 = low_24 ? 1.f : 0.f;
    damping(high_24, high_res_on ? r : 0.f, &t->target_high_k1, &t->target_high_k2);
    damping(low_24, low_res_on ? r : 0.f, &t->target_low_k1, &t->target_low_k2);
    if (!t->started) {
        t->high_k1 = t->target_high_k1; t->high_k2 = t->target_high_k2;
        t->low_k1 = t->target_low_k1; t->low_k2 = t->target_low_k2;
        t->high_24 = t->target_high_24; t->low_24 = t->target_low_24;
    }
}
static float filter(float input, float g, float k, float *s1, float *s2, int high) {
    float v1 = (*s1 + g * (input - *s2)) / (1 + g * (g + k));
    float v2 = *s2 + g * v1;
    *s1 = 2 * v1 - *s1;
    *s2 = 2 * v2 - *s2;
    if (fabsf(*s1) < 1e-20f) *s1 = 0;
    if (fabsf(*s2) < 1e-20f) *s2 = 0;
    return high ? input - k * v1 - v2 : v2;
}
void eka_tone_process(EkaTone *t, uint32_t n, float *audio) {
    if (!n) return;
    t->started = 1;
    if (t->mix == 0 && t->target_mix == 0) {
        snap_targets(t);
        return; /* Exact bypass: no filtering, state work or audio rounding. */
    }
    for (uint32_t i = 0; i < n; ++i) {
        t->high_g += t->smoothing * (t->target_high_g - t->high_g);
        t->low_g += t->smoothing * (t->target_low_g - t->low_g);
        t->high_k1 += t->smoothing * (t->target_high_k1 - t->high_k1);
        t->high_k2 += t->smoothing * (t->target_high_k2 - t->high_k2);
        t->low_k1 += t->smoothing * (t->target_low_k1 - t->low_k1);
        t->low_k2 += t->smoothing * (t->target_low_k2 - t->low_k2);
        t->high_24 += t->smoothing * (t->target_high_24 - t->high_24);
        t->low_24 += t->smoothing * (t->target_low_24 - t->low_24);
        t->mix += t->smoothing * (t->target_mix - t->mix);
        if (fabsf(t->high_24 - t->target_high_24) < 1e-6f) t->high_24 = t->target_high_24;
        if (fabsf(t->low_24 - t->target_low_24) < 1e-6f) t->low_24 = t->target_low_24;
        if (fabsf(t->mix - t->target_mix) < 1e-6f) t->mix = t->target_mix;
        float input = isfinite(audio[i]) ? audio[i] : 0;
        float output = filter(input, t->high_g, t->high_k1, &t->high_s1, &t->high_s2, 1);
        if (t->high_24 > 0 || t->target_high_24 > 0) {
            float stage2 = filter(output, t->high_g, t->high_k2, &t->high2_s1, &t->high2_s2, 1);
            output = output + t->high_24 * (stage2 - output);
        } else {
            t->high2_s1 = t->high2_s2 = 0;
        }
        float lp1 = filter(output, t->low_g, t->low_k1, &t->low_s1, &t->low_s2, 0);
        if (t->low_24 > 0 || t->target_low_24 > 0) {
            float lp2 = filter(lp1, t->low_g, t->low_k2, &t->low2_s1, &t->low2_s2, 0);
            output = lp1 + t->low_24 * (lp2 - lp1);
        } else {
            t->low2_s1 = t->low2_s2 = 0;
            output = lp1;
        }
        audio[i] = input + t->mix * (output - input);
        if (t->mix == 0 && t->target_mix == 0) {
            t->high_s1 = t->high_s2 = t->high2_s1 = t->high2_s2 = 0;
            t->low_s1 = t->low_s2 = t->low2_s1 = t->low2_s2 = 0;
            snap_targets(t);
            break;
        }
    }
}
