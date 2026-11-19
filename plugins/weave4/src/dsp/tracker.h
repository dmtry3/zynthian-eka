/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef EKA_TRACKER_H
#define EKA_TRACKER_H
#include <stdint.h>
typedef struct EkaTracker EkaTracker;
EkaTracker *eka_tracker_create(unsigned rate);
void eka_tracker_destroy(EkaTracker *p);
void eka_tracker_reset(EkaTracker *p);
void eka_tracker_process(EkaTracker *p, const float *input, unsigned n, unsigned profile);
/* Source-time lookup: pitch shifters analyse past input, not the current sample. */
int eka_tracker_read(const EkaTracker *p, unsigned age, float *note, float *confidence, float *hz);
#endif
