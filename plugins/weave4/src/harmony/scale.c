/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "scale.h"
#include <math.h>
#include <string.h>
static const int modes[EKA_SCALE_COUNT][7] = {
    {0,2,4,5,7,9,11}, /* Major / Ionian */
    {0,2,3,5,7,8,10}, /* Natural minor / Aeolian */
    {0,2,3,5,7,9,10}, /* Dorian */
    {0,1,3,5,7,8,10}, /* Phrygian */
    {0,2,4,6,7,9,11}, /* Lydian */
    {0,2,4,5,7,9,10}, /* Mixolydian */
    {0,1,3,5,6,8,10}, /* Locrian */
    {0,2,3,5,7,8,11}, /* Harmonic minor */
    {0,2,3,5,7,9,11}, /* Melodic minor (same ascending pattern both directions) */
};
void eka_scale_reset(EkaScale *p) { memset(p, 0, sizeof(*p)); }
static int note(unsigned tonic, unsigned scale, int degree) {
    int octave = degree / 7, step = degree % 7;
    if (step < 0) { step += 7; --octave; }
    return (int)tonic + 12 * octave + modes[scale][step];
}
int eka_scale_targets(EkaScale *p, unsigned tonic, unsigned scale, float input,
                      const int steps[4], float targets[4]) {
    for (unsigned v = 0; v < 4; ++v) targets[v] = -1;
    if (tonic > 11 || scale >= EKA_SCALE_COUNT || !isfinite(input) || input < 0 || input > 127) {
        eka_scale_reset(p); return 0;
    }
    int octave = (int)floorf((input - tonic) / 12);
    int best = 0; float distance = 1000;
    /* Three adjacent octaves, fixed 21 candidates; ties choose lower note. */
    for (int d = (octave - 1) * 7; d < (octave + 2) * 7; ++d) {
        float delta = fabsf(input - note(tonic, scale, d));
        if (delta < distance) { distance = delta; best = d; }
    }
    /* 0.1-semitone hysteresis around each midpoint prevents boundary chatter. */
    if (p->valid && p->tonic == tonic && p->scale == scale &&
        fabsf(input - note(tonic, scale, p->anchor)) <= distance + .2f)
        best = p->anchor;
    p->anchor = best; p->valid = 1; p->tonic = tonic; p->scale = scale;
    for (unsigned v = 0; v < 4; ++v) {
        int step = steps[v] < -7 ? -7 : steps[v] > 7 ? 7 : steps[v];
        targets[v] = (float)note(tonic, scale, best + step);
    }
    return 1;
}
