/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "character.h"
#include <math.h>
#include <string.h>
static float bounded(float value, float low, float high, float fallback) {
    return isfinite(value) ? fminf(high, fmaxf(low, value)) : fallback;
}
static float random_signed(EkaCharacter *c) {
    uint32_t x = c->seed;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    c->seed = x;
    return (float)(x >> 8) * (2.0f / 16777215.0f) - 1;
}
void eka_character_reset(EkaCharacter *c, double rate, unsigned voice) {
    memset(c, 0, sizeof(*c));
    c->rate = rate;
    c->seed = 0x6d2b79f5u ^ ((voice + 1u) * 0x9e3779b9u);
    c->speed = c->target_speed = .2f;
    c->to_pitch = random_signed(c);
    c->to_time = random_signed(c);
}
void eka_character_set(EkaCharacter *c, float fine, float amount, float speed) {
    c->target_fine = bounded(fine, -50, 50, 0);
    c->target_amount = bounded(amount, 0, 100, 0) / 100;
    c->target_speed = bounded(speed, .05f, 1, .2f);
    if (!c->started) {
        c->fine = c->target_fine;
        c->speed = c->target_speed;
        c->cents = c->fine;
    }
}
void eka_character_advance(EkaCharacter *c, unsigned samples) {
    c->started = 1;
    if (c->amount == 0 && c->target_amount == 0 && c->fine == c->target_fine) {
        c->cents = c->fine; c->milliseconds = 0; return;
    }
    float smooth = (float)(1 - exp(-(double)samples / (c->rate * .05)));
    c->fine += smooth * (c->target_fine - c->fine);
    c->amount += smooth * (c->target_amount - c->amount);
    c->speed += smooth * (c->target_speed - c->speed);
    if (fabsf(c->fine - c->target_fine) < 1e-5f) c->fine = c->target_fine;
    if (fabsf(c->amount - c->target_amount) < 1e-7f) c->amount = c->target_amount;
    c->phase += samples * c->speed / c->rate;
    if (c->phase >= 1) {
        c->phase -= 1;
        c->from_pitch = c->to_pitch; c->from_time = c->to_time;
        c->to_pitch = random_signed(c); c->to_time = random_signed(c);
    }
    double w = c->phase * c->phase * (3 - 2 * c->phase);
    c->cents = c->fine + c->amount * 8 * (c->from_pitch + w * (c->to_pitch - c->from_pitch));
    c->milliseconds = c->amount * 6 * (1 + c->from_time + w * (c->to_time - c->from_time));
}
