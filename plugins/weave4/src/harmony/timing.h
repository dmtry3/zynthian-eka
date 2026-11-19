/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef EKA_TIMING_H
#define EKA_TIMING_H
#define EKA_DIVISIONS 20
double eka_division_ms(unsigned division, double bpm, double beats_per_bar, double beat_unit);
#endif
