/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "timing.h"
#include <math.h>
double eka_division_ms(unsigned division, double bpm, double beats_per_bar, double beat_unit) {
    static const double quarters[] = {
        1.0/16, 1.0/12, 3.0/32, 1.0/8, 1.0/6, 3.0/16,
        1.0/4, 1.0/3, 3.0/8, 1.0/2, 2.0/3, 3.0/4,
        1, 4.0/3, 1.5, 2, 3
    };
    if (!isfinite(bpm) || bpm < 20 || bpm > 400) bpm = 120;
    if (!isfinite(beats_per_bar) || beats_per_bar < 1 || beats_per_bar > 32) beats_per_bar = 4;
    if (!isfinite(beat_unit) || beat_unit < 1 || beat_unit > 64) beat_unit = 4;
    if (division >= EKA_DIVISIONS) division = 12;
    if (division >= 17) return 60000 / bpm * beats_per_bar * (1u << (division - 17));
    return 60000 / bpm * beat_unit / 4 * quarters[division];
}
