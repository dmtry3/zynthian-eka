/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef EKA_TONE_H
#define EKA_TONE_H
#include <stdint.h>
typedef struct {
    float high_g, low_g, target_high_g, target_low_g;
    float high_s1, high_s2, low_s1, low_s2, mix, target_mix, smoothing;
    double rate;
    int started;
} EkaTone;
void eka_tone_reset(EkaTone *t, double rate);
void eka_tone_set(EkaTone *t, float highpass_hz, float lowpass_hz, int enabled);
void eka_tone_process(EkaTone *t, uint32_t n, float *audio);
#endif
