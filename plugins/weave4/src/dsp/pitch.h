/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef EKA_PITCH_H
#define EKA_PITCH_H
#include <stdint.h>
typedef struct EkaPitch EkaPitch;
EkaPitch *eka_pitch_create(double rate);
void eka_pitch_destroy(EkaPitch *p);
void eka_pitch_reset(EkaPitch *p);
void eka_pitch_schedule(EkaPitch *p, unsigned voice, unsigned voices);
void eka_pitch_set(EkaPitch *p, float semitones, float dry_db, float wet_db);
void eka_pitch_align(EkaPitch *p, unsigned total_latency);
void eka_pitch_dry_alignment(EkaPitch *p, int aligned);
void eka_pitch_voice_alignment(EkaPitch *p, int aligned);
/* Separate wet/dry outputs let the caller pan only the harmony voice. */
void eka_pitch_process(EkaPitch *p, uint32_t n, const float *input, float *wet, float *dry);
void eka_pitch_character(EkaPitch *p, float fine, float amount, float speed);
float eka_pitch_fine_actual(const EkaPitch *p);
float eka_pitch_time_actual(const EkaPitch *p);
unsigned eka_pitch_tracking_age(const EkaPitch *p);
unsigned eka_pitch_latency(const EkaPitch *p);
#endif
