/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef EKA_CHORDS_H
#define EKA_CHORDS_H
#define EKA_CHORD_COUNT 14
#define EKA_VOICING_LEAD 4
#define EKA_VOICING_COUNT 5
/* Returns the number of harmony notes, excluding the dry root. */
unsigned eka_chord_intervals(unsigned chord, unsigned voicing, float pitches[4]);
/* Relative-input voice leading. History is runtime state, not a scene bank. */
typedef struct {
    float previous[4], input[4];
    unsigned count, input_count;
    int valid, cached;
} EkaLeading;
void eka_leading_reset(EkaLeading *s);
void eka_leading_seed(EkaLeading *s, const float notes[4], unsigned count);
void eka_leading_apply(EkaLeading *s, const float notes[4], unsigned count, float out[4]);
#endif
