/* Copyright 2016 Emilie Gillet; adapted for Eka Fold, 2026. MIT license: see THIRD_PARTY.md.
 *
 * The C interface of the two resonators of Eka Fold's delay block (Modal, String), derived from the modal
 * resonator and the string of github.com/pichenettes/eurorack (plaits/dsp/physical_modelling, with stmlib's
 * filters). The C++ code (resonators.cc) is built like engines.cc; fold_core.c calls only what is here.
 *
 * What is different from the original: the voice excites them (no exciter of their own: the strike, the pluck or
 * the bow is the patch), one sample at a time at any rate, f0 in cycles per sample; their knobs are Structure,
 * Brightness, Damping (and Position for Modal: the original fixes it at 0.015), 0..1:
 *   Modal:  24 band-pass modes. Structure: from harmonic (a string, a bar) to stretched (a bell, a plate),
 *           Brightness: how many high modes ring, Damping: how long they ring, Position: where it is struck.
 *   String: a waveguide string. Structure: below 0.24 a curved bridge (a sitar's buzz), 0.24..0.26 a plain string,
 *           above 0.26 dispersion (a piano, a bar, a bell); Brightness: how fast the high partials die, Damping:
 *           how long it rings (above 0.95: for ever). The delay of its loop filters is taken off the line: in tune.
 * Both are updated every FR_BLOCK_SAMPLES (fr_*_block) and run per sample (fr_*_tick). */
#ifndef FOLD_RESONATORS_H
#define FOLD_RESONATORS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FR_MODES 24
#define FR_STRETCH_LEN 1024       /* the string's dispersion all-pass line (power of two) */
#define FR_STIFFNESS_SIZE 65
extern const float fr_stiffness[FR_STIFFNESS_SIZE];

typedef struct {
  float s1[FR_MODES], s2[FR_MODES];       /* the modes' state-variable filters */
  float g[FR_MODES], rg[FR_MODES], h[FR_MODES], a[FR_MODES];   /* their coefficients for this block */
  int n;                                  /* modes that sound (below 24 kHz) */
} FrModal;

void fr_modal_reset(FrModal* m);
/* f0: cycles per sample; sr: the rate (the modes above 24 kHz are left out). */
void fr_modal_block(FrModal* m, float f0, float sr, float structure, float brightness, float damping,
                    float position);
float fr_modal_tick(FrModal* m, float in);

typedef struct {
  float* line;                            /* the string: a buffer of the voice, len a power of two */
  int mask, wp;                           /* the last sample written (counts up: k samples ago is wp - k + 1) */
  float stretch[FR_STRETCH_LEN];
  int swp;
  float s1, s2, g, r, h;                  /* the damping low-pass */
  float dc_x, dc_y, dc_pole;
  float delay, delay_step;                /* the delay in samples, gliding over a block */
  float disp_noise, curved;               /* dispersion noise, curved bridge state */
  int bridge;                             /* 1: curved bridge, 0: dispersion */
  float noise_amount, noise_filter, bridge_curving, ap_gain, stretch_point;
  int ap_d;                               /* the dispersion all-pass length (0: none) */
  uint32_t rng;
} FrString;

void fr_string_init(FrString* s, float* line, int len);
void fr_string_reset(FrString* s);
/* f0: cycles per sample; n: the samples until the next block (the delay glides over them). */
void fr_string_block(FrString* s, float f0, float sr, float structure, float brightness, float damping, int n);
float fr_string_tick(FrString* s, float in);

#ifdef __cplusplus
}
#endif

#endif
