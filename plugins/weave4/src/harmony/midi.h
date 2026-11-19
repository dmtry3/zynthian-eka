/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef EKA_MIDI_H
#define EKA_MIDI_H
#include <stdint.h>
typedef struct {
    uint8_t pressed[16][128], sustained[16][128], pedal[16];
    int target[4];
    unsigned count;
} EkaMidi;
void eka_midi_reset(EkaMidi *p);
void eka_midi_event(EkaMidi *p, const uint8_t *data, unsigned size);
void eka_midi_select(EkaMidi *p, unsigned channel);
#endif
