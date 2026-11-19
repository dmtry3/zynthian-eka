/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef EKA_CHARACTER_H
#define EKA_CHARACTER_H
#include <stdint.h>
typedef struct {
    double phase, rate;
    float fine, amount, speed, target_fine, target_amount, target_speed;
    float from_pitch, to_pitch, from_time, to_time, cents, milliseconds;
    uint32_t seed;
    int started;
} EkaCharacter;
void eka_character_reset(EkaCharacter *c, double rate, unsigned voice);
void eka_character_set(EkaCharacter *c, float fine, float amount, float speed);
void eka_character_advance(EkaCharacter *c, unsigned samples);
#endif
