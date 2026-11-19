/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef EKA_DELAY_H
#define EKA_DELAY_H
#include <stdint.h>
#include "tone.h"
#define EKA_DELAY_MAX_MS 3000.0
typedef struct EkaDelay EkaDelay;
EkaDelay *eka_delay_create(double rate);
void eka_delay_destroy(EkaDelay *d);
void eka_delay_reset(EkaDelay *d);
void eka_delay_set(EkaDelay *d, double milliseconds, float feedback);
void eka_delay_process(EkaDelay *d, uint32_t n, float *audio);
void eka_delay_process_tone(EkaDelay *d, EkaTone *t, uint32_t n, float *audio);
double eka_delay_milliseconds(const EkaDelay *d);
#endif
