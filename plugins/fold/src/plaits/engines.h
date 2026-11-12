/* Copyright 2016 Emilie Gillet; adapted for Eka Fold, 2026. MIT license: see THIRD_PARTY.md.
 *
 * The C interface of the three Principal engines of Eka Fold (VA, FM2, Wave), derived from the virtual
 * analog, 2-operator FM and wavetable engines of github.com/pichenettes/eurorack (plaits/dsp/engine). The
 * C++ code (engines.cc) is built with g++ -fno-exceptions -fno-rtti, without the STL; fold_core.c calls only
 * what is here.
 *
 * What is different from the original: the engines render one sample at a time at any rate, with the phase
 * increment given for each sample and signed (fold_core's FM is linear and through zero), and they can be
 * hard-synced from outside (fold_core's Mod OSC). Their knobs are Harmonics, Color (the original Timbre:
 * Timbre in Eka Fold is the shaper depth) and Morph, 0..1:
 *   VA:   two variable-shape oscillators (triangle - saw - square - narrow pulse), mixed. Harmonics: the
 *         interval of the second one (-24..+24 semitones through the fifths and octaves), Color: the shape of
 *         the first, Morph: of the second.
 *   FM2:  a sine carrier phase-modulated by a sine. Harmonics: the ratio (quantized), Color: the index,
 *         Morph: feedback (below 50 % the carrier into the modulator's frequency, above it the modulator
 *         into itself).
 *   Wave: 4 banks of 8 x 8 waves. Harmonics: the bank (its upper half steps through them without
 *         blending), Color: the column, Morph: the row.
 * Every engine gives -1..1. */
#ifndef FOLD_ENGINES_H
#define FOLD_ENGINES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { FE_VA, FE_FM2, FE_WAVE, FE_NENGINES };

#define FE_NPARAMS 6
#define FE_FM_QUANTIZER_SIZE 130
#define FE_WAVE_LEN 128
#define FE_WAVE_STRIDE (FE_WAVE_LEN + 4)
#define FE_NWAVES 192
#define FE_WAVES_SIZE (FE_NWAVES * FE_WAVE_STRIDE)

extern const float fe_fm_quantizer[FE_FM_QUANTIZER_SIZE];
extern const int16_t fe_waves[FE_WAVES_SIZE];

/* State of one engine in one voice. */
typedef struct {
  float ph[2];                    /* VA: the two oscillators; FM2: carrier, modulator; Wave: ph[0] */
  float p[FE_NPARAMS];            /* the knobs decoded, gliding to FeBlock.t */
  float fb;                       /* FM2: its output, low-passed (feedback) */
  float w_prev;                   /* Wave: the integral at the last phase */
  float out;                      /* Wave: the last output (low-passed slope) */
  int fresh;                      /* the parameters jump to their targets on the first sample */
} FeState;

/* What one voice's engine needs for a block of samples: the knobs decoded. */
typedef struct {
  const float* sine;              /* fold_core's sine table, sine_n + 1 entries over one cycle */
  int sine_n;
  float t[FE_NPARAMS];            /* targets of FeState.p */
  float glide;                    /* one-pole step per sample towards them */
  float fb_coef;                  /* FM2: the feedback low-pass, per sample */
  float wave_lp;                  /* Wave: the slope low-pass, per sample */
  float wave_gain;                /* Wave: 1 / (128 x 1024 x the increment), without the increment */
} FeBlock;

/* Decode the knobs (0..1) for a voice at hz, rendered at sr. */
void fe_block(FeBlock* b, int engine, float harmonics, float color, float morph, float hz, float sr,
              const float* sine, int sine_n);
/* Start from phase 0 (a new note). */
void fe_reset(FeState* s);
/* The next sample; pinc = phase increment in cycles per sample, signed. Band-limited. */
float fe_tick(FeState* s, const FeBlock* b, int engine, float pinc);
/* The next sample with a hard sync: the engine restarted at phase 0, t (0..1) of this sample ago. Returns the
 * sample after the restart; *half = half the step (the value at the restart - the value it would have had),
 * which the caller spreads over this sample and the one before (fold_core: one sample late); Wave
 * band-limits its step itself (*half = 0). */
float fe_sync(FeState* s, const FeBlock* b, int engine, float pinc, float t, float* half);

#ifdef __cplusplus
}
#endif

#endif
