/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tone.h"
#include <math.h>
#include <string.h>
static float coefficient(float hz, float fallback, float low, float high, double rate) {
    if (!isfinite(hz)) hz = fallback;
    if (hz < low) hz = low;
    if (hz > high) hz = high;
    if (hz > rate * .45) hz = (float)(rate * .45);
    return (float)tan(3.14159265358979323846 * hz / rate);
}
void eka_tone_reset(EkaTone *t, double rate) {
    memset(t, 0, sizeof(*t));
    t->rate = rate;
    t->smoothing = (float)(1 - exp(-1 / (rate * .010)));
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
static float filter(float input, float g, float *s1, float *s2, int high) {
    const float k = 1.4142135623730951f;
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
        t->high_g = t->target_high_g; t->low_g = t->target_low_g;
        return; /* Exact bypass: no filtering, state work or audio rounding. */
    }
    for (uint32_t i = 0; i < n; ++i) {
        t->high_g += t->smoothing * (t->target_high_g - t->high_g);
        t->low_g += t->smoothing * (t->target_low_g - t->low_g);
        t->mix += t->smoothing * (t->target_mix - t->mix);
        if (fabsf(t->mix - t->target_mix) < 1e-6f) t->mix = t->target_mix;
        float input = isfinite(audio[i]) ? audio[i] : 0;
        float output = filter(input, t->high_g, &t->high_s1, &t->high_s2, 1);
        output = filter(output, t->low_g, &t->low_s1, &t->low_s2, 0);
        audio[i] = input + t->mix * (output - input);
        if (t->mix == 0 && t->target_mix == 0) {
            t->high_s1 = t->high_s2 = t->low_s1 = t->low_s2 = 0;
            t->high_g = t->target_high_g; t->low_g = t->target_low_g;
            break;
        }
    }
}
