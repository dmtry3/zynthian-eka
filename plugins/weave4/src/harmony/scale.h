/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef EKA_SCALE_H
#define EKA_SCALE_H
#define EKA_SCALE_COUNT 9
typedef struct { int anchor, valid; unsigned tonic, scale; } EkaScale;
void eka_scale_reset(EkaScale *p);
/* Signed degree steps: 0 unison, 2 third, 4 fifth, 7 octave. */
int eka_scale_targets(EkaScale *p, unsigned tonic, unsigned scale, float input,
                      const int steps[4], float targets[4]);
#endif
