/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "delay.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

struct EkaDelay {
    float *ring;
    unsigned capacity, write, fade_length, fade_position;
    double rate, current, destination, requested;
    float feedback, target_feedback, smoothing;
    int started, fading;
};
EkaDelay *eka_delay_create(double rate) {
    if (!isfinite(rate) || rate < 8000 || rate > 192000) return NULL;
    EkaDelay *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->rate = rate;
    d->capacity = (unsigned)ceil(rate * EKA_DELAY_MAX_MS / 1000) + 2;
    d->ring = calloc(d->capacity, sizeof(float));
    if (!d->ring) { free(d); return NULL; }
    d->fade_length = (unsigned)(rate * .020);
    d->smoothing = (float)(1 - exp(-1 / (rate * .010)));
    eka_delay_reset(d);
    return d;
}
void eka_delay_destroy(EkaDelay *d) {
    if (d) { free(d->ring); free(d); }
}
void eka_delay_reset(EkaDelay *d) {
    memset(d->ring, 0, d->capacity * sizeof(float));
    d->write = d->fade_position = 0;
    d->current = d->destination = d->requested = 0;
    d->feedback = d->target_feedback = 0;
    d->started = d->fading = 0;
}
void eka_delay_set(EkaDelay *d, double milliseconds, float feedback) {
    if (!isfinite(milliseconds)) milliseconds = 0;
    if (!isfinite(feedback)) feedback = 0;
    if (milliseconds < 0) milliseconds = 0;
    if (milliseconds > EKA_DELAY_MAX_MS) milliseconds = EKA_DELAY_MAX_MS;
    if (feedback < 0) feedback = 0;
    if (feedback > .90f) feedback = .90f;
    d->requested = milliseconds * d->rate / 1000;
    d->target_feedback = feedback;
    if (!d->started) {
        d->current = d->destination = d->requested;
        d->feedback = feedback;
    }
}
static float tap(const EkaDelay *d, double samples, float input) {
    if (samples < 1e-9) return input;
    /* Subsample delays below one sample blend the latest stored sample and input. */
    if (samples < 1) {
        unsigned previous = d->write ? d->write - 1 : d->capacity - 1;
        return input + (float)samples * (d->ring[previous] - input);
    }
    double position = d->write - samples;
    if (position < 0) position += d->capacity;
    unsigned first = (unsigned)position;
    unsigned second = first + 1 == d->capacity ? 0 : first + 1;
    return d->ring[first] + (float)(position - first) * (d->ring[second] - d->ring[first]);
}
void eka_delay_process_tone(EkaDelay *d, EkaTone *t, uint32_t n, float *audio) {
    for (uint32_t i = 0; i < n; ++i) {
        float input = isfinite(audio[i]) ? audio[i] : 0;
        if (!d->fading && fabs(d->requested - d->current) > 1e-6) {
            d->destination = d->requested;
            d->fade_position = 0;
            d->fading = 1;
        }
        float delayed = tap(d, d->current, input);
        if (d->fading) {
            float weight = (float)d->fade_position / d->fade_length;
            delayed += weight * (tap(d, d->destination, input) - delayed);
            if (++d->fade_position >= d->fade_length) {
                d->current = d->destination;
                d->fading = 0;
            }
        }
        d->feedback += d->smoothing * (d->target_feedback - d->feedback);
        if (fabsf(d->target_feedback - d->feedback) < 1e-6f)
            d->feedback = d->target_feedback;
        /* Zero-delay bypass has no recursive feedback or gain increase. */
        float feedback = !d->fading && d->current < 1e-9 ? 0 : d->feedback;
        float out = delayed;
        if (t) eka_tone_process(t, 1, &out);
        float stored = input + feedback * out;
        if (stored > 1.5f) stored = 1.5f + 1.5f * tanhf((stored - 1.5f) / 1.5f);
        else if (stored < -1.5f) stored = -1.5f - 1.5f * tanhf((-stored - 1.5f) / 1.5f);
        if (fabsf(stored) < 1e-20f) stored = 0;
        d->ring[d->write] = stored;
        if (++d->write == d->capacity) d->write = 0;
        audio[i] = out;
        d->started = 1;
    }
}
void eka_delay_process(EkaDelay *d, uint32_t n, float *audio) {
    eka_delay_process_tone(d, NULL, n, audio);
}
double eka_delay_milliseconds(const EkaDelay *d) {
    return d->requested * 1000 / d->rate;
}
