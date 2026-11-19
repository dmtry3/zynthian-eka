/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef EKA_TONE_H
#define EKA_TONE_H
#include <stdint.h>
typedef struct {
    float high_g, low_g, target_high_g, target_low_g;
    float high_k1, high_k2, target_high_k1, target_high_k2;
    float low_k1, low_k2, target_low_k1, target_low_k2;
    float high_24, target_high_24, low_24, target_low_24;
    float high_s1, high_s2, high2_s1, high2_s2;
    float low_s1, low_s2, low2_s1, low2_s2;
    float mix, target_mix, smoothing;
    double rate;
    int started;
} EkaTone;
void eka_tone_reset(EkaTone *t, double rate);
void eka_tone_set(EkaTone *t, float highpass_hz, float lowpass_hz, int enabled);
void eka_tone_shape(EkaTone *t, float resonance_pct, int high_24, int low_24,
                    int high_res_on, int low_res_on);
void eka_tone_process(EkaTone *t, uint32_t n, float *audio);
#endif
