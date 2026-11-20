/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Eka Pulse DSP core implementation (M0: polyrhythmic Step Shaper + Gate & Space + Master Limiter). */
#include "pulse_fx.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#if defined(__SSE__) || defined(__x86_64__)
#include <xmmintrin.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define PX_LIM_CEILING 1.0f
#define PX_BYPASS_MS   7.0f
#define PX_PEAK_FALL_S 0.30f

/* 1/32, 1/16T, 1/16, 1/8T, 1/8, 1/4, 1/2, 1 bar */
const float px_rate_beats[PX_NRATES - 1] = {
  0.125f, 1.0f / 6.0f, 0.25f, 1.0f / 3.0f, 0.5f, 1.0f, 2.0f, 4.0f
};

static inline float clampf(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

static inline void px_enable_ftz(void) {
#if defined(__SSE__) || defined(__x86_64__)
  _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
#elif defined(__aarch64__)
  uint64_t fpcr;
  __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
  fpcr |= (1ULL << 24); /* FZ bit */
  __asm__ __volatile__("msr fpcr, %0" : : "r"(fpcr));
#endif
}

float px_db(float db) {
  return db <= -59.9f ? 0.0f : powf(10.0f, db / 20.0f);
}

float px_eval_shape(int shape, float u, float warp) {
  u = clampf(u, 0.0f, 0.999999f);
  float uw = u;
  if (fabsf(warp) > 1e-4f) {
    float exp_w = powf(2.0f, -clampf(warp, -1.0f, 1.0f) * 2.0f);
    uw = powf(u, exp_w);
  }
  switch (shape) {
    case PX_SHP_HOLD:
      return 1.0f;
    case PX_SHP_DECAY:
      return 1.0f - uw;
    case PX_SHP_RAMP:
      return uw;
    case PX_SHP_PULSE:
      return 1.0f - fabsf(2.0f * uw - 1.0f);
    case PX_SHP_2X: {
      float u2 = uw * 2.0f;
      u2 -= floorf(u2);
      return 1.0f - u2;
    }
    case PX_SHP_3X: {
      float u3 = uw * 3.0f;
      u3 -= floorf(u3);
      return 1.0f - u3;
    }
    default:
      return 1.0f;
  }
}

typedef struct {
  unsigned char lvl[PX_MAX_STEPS]; /* 0..100 */
  unsigned char shp[PX_MAX_STEPS]; /* 0..5 */
} PxPatternDef;

/* 24 built-in musical patterns (indices 1..24; index 0 is Custom). */
static const PxPatternDef k_patterns[PX_NPATTERNS - 1] = {
  /* 1: Sidechain (pumping ramps) */
  { { 100,100,100,100, 100,100,100,100, 100,100,100,100, 100,100,100,100 },
    { 2,2,2,2, 2,2,2,2, 2,2,2,2, 2,2,2,2 } },
  /* 2: Trance Gate */
  { { 100,0,100,100, 0,100,0,100, 100,0,100,0, 100,100,0,100 },
    { 1,0,1,0, 0,1,0,1, 1,0,1,0, 1,0,0,1 } },
  /* 3: Stutter 16 */
  { { 100,80,100,60, 100,80,100,60, 100,80,100,60, 100,80,100,90 },
    { 1,1,1,1, 1,1,1,1, 1,1,1,1, 1,1,4,4 } },
  /* 4: Euclid 3/8 */
  { { 100,0,0,100, 0,0,100,0, 100,0,0,100, 0,0,100,0 },
    { 1,0,0,1, 0,0,1,0, 1,0,0,1, 0,0,1,0 } },
  /* 5: Euclid 5/8 */
  { { 100,0,100,100, 0,100,0,100, 100,0,100,100, 0,100,0,100 },
    { 1,0,1,1, 0,1,0,1, 1,0,1,1, 0,1,0,1 } },
  /* 6: Euclid 7/16 */
  { { 100,0,100,0, 100,0,0,100, 0,100,0,100, 0,0,100,0 },
    { 1,0,1,0, 1,0,0,1, 0,1,0,1, 0,0,1,0 } },
  /* 7: Syncopated */
  { { 100,0,75,0, 0,95,0,65, 100,0,0,85, 0,90,60,0 },
    { 1,0,3,0, 0,1,0,3, 1,0,0,1, 0,1,4,0 } },
  /* 8: Offbeat Pump */
  { { 0,100,0,100, 0,100,0,100, 0,100,0,100, 0,100,0,100 },
    { 0,3,0,3, 0,3,0,3, 0,3,0,3, 0,3,0,3 } },
  /* 9: Swell */
  { { 12,25,38,50, 62,75,88,100, 12,25,38,50, 62,75,88,100 },
    { 2,2,2,2, 2,2,2,0, 2,2,2,2, 2,2,2,0 } },
  /* 10: Fade Out */
  { { 100,88,75,62, 50,38,25,12, 100,88,75,62, 50,38,25,12 },
    { 1,1,1,1, 1,1,1,1, 1,1,1,1, 1,1,1,1 } },
  /* 11: Bounce */
  { { 100,0,0,85, 0,75,70,95, 100,0,0,85, 0,75,90,100 },
    { 1,0,0,1, 0,1,4,5, 1,0,0,1, 0,1,4,5 } },
  /* 12: Triplet Roll */
  { { 100,0,100,0, 100,100,0,100, 100,0,100,0, 100,100,100,100 },
    { 5,0,5,0, 1,5,0,5, 5,0,5,0, 1,5,5,5 } },
  /* 13: Heartbeat */
  { { 100,70,0,0, 0,0,0,0, 100,70,0,0, 0,0,30,0 },
    { 1,1,0,0, 0,0,0,0, 1,1,0,0, 0,0,3,0 } },
  /* 14: Broken Beat */
  { { 100,0,0,90, 0,0,80,0, 0,100,0,0, 85,0,70,0 },
    { 0,0,0,1, 0,0,1,0, 0,0,0,0, 1,0,4,0 } },
  /* 15: Tremolo */
  { { 100,40,100,40, 100,40,100,40, 100,40,100,40, 100,40,100,40 },
    { 3,3,3,3, 3,3,3,3, 3,3,3,3, 3,3,3,3 } },
  /* 16: Chop & Hold */
  { { 100,100,0,0, 100,0,100,0, 100,100,0,0, 100,0,0,100 },
    { 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0 } },
  /* 17: Stairs Up */
  { { 15,25,35,45, 55,65,75,85, 95,100,100,90, 80,70,60,50 },
    { 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0 } },
  /* 18: Stairs Down */
  { { 100,90,80,70, 60,50,40,30, 20,10,20,35, 50,65,80,95 },
    { 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0 } },
  /* 19: Peak Center */
  { { 20,40,65,90, 100,90,65,40, 20,40,65,90, 100,90,65,40 },
    { 2,2,2,2, 3,1,1,1, 2,2,2,2, 3,1,1,1 } },
  /* 20: Tape Stop End */
  { { 0,0,0,0, 0,0,0,0, 0,0,0,0, 25,50,75,100 },
    { 0,0,0,0, 0,0,0,0, 0,0,0,0, 2,2,2,2 } },
  /* 21: Poly 5 */
  { { 100,0,70,100, 40,100,0,70, 100,40,100,0, 70,100,40,80 },
    { 1,0,3,1, 4,1,0,3, 1,4,1,0, 3,1,4,1 } },
  /* 22: Poly 7 */
  { { 100,50,0,90, 0,75,35,100, 50,0,90,0, 75,35,100,60 },
    { 1,2,0,1, 0,3,1,1, 2,0,1,0, 3,1,1,4 } },
  /* 23: Glitch */
  { { 100,0,80,100, 0,60,100,0, 90,100,0,70, 100,0,95,100 },
    { 4,0,5,0, 0,4,1,0, 5,0,0,4, 2,0,5,4 } },
  /* 24: Random (varied musical accent) */
  { { 95,20,70,0, 85,45,100,15, 60,0,90,35, 100,55,25,80 },
    { 1,3,2,0, 4,1,0,2, 3,0,5,1, 0,2,1,4 } },
};

void px_apply_pattern(int pattern_id, float levels_pct[PX_MAX_STEPS], int shapes[PX_MAX_STEPS]) {
  if (pattern_id < 1 || pattern_id >= PX_NPATTERNS) return;
  const PxPatternDef* pat = &k_patterns[pattern_id - 1];
  for (int i = 0; i < PX_MAX_STEPS; i++) {
    levels_pct[i] = (float)pat->lvl[i];
    shapes[i] = (int)pat->shp[i];
  }
}

void px_default_params(PxParams* p) {
  memset(p, 0, sizeof *p);
  static const int def_steps[PX_SLOTS] = { 16, 5, 7, 8 };
  static const int def_pat[PX_SLOTS] = { 20, 21, 22, 2 };
  static const float def_p1[PX_SLOTS] = { 50.0f, 50.0f, 50.0f, 0.0f };
  static const float def_p2[PX_SLOTS] = { 50.0f, 50.0f, 30.0f, 50.0f };
  for (int s = 0; s < PX_SLOTS; s++) {
    PxSlotParams* sp = &p->slot[s];
    sp->on = (s == PX_SLOT_GATE) ? 1 : 0;
    sp->mode = 0;
    sp->param1 = def_p1[s];
    sp->param2 = def_p2[s];
    sp->depth = 1.0f;
    sp->pattern = def_pat[s];
    sp->step_rate = 2; /* 1/16 */
    sp->steps = def_steps[s];
    sp->warp = 0.0f;
    sp->trig = PX_TRIG_SYNC;
    sp->threshold_db = -24.0f;
    sp->env_depth = 0.0f;
    sp->smooth = 0.10f;
    float lvl_pct[PX_MAX_STEPS];
    px_apply_pattern(sp->pattern, lvl_pct, sp->shapes);
    for (int k = 0; k < PX_MAX_STEPS; k++) sp->levels[k] = lvl_pct[k] * 0.01f;
  }
  p->mix = 1.0f;
  p->low_split_hz = 0.0f;
  p->low_duck = 0;
  p->free_hz = 4.0f;
  p->output_db = 0.0f;
  p->live_pad = 0;
}

int px_init(PulseFx* x, double sr) {
  if (!x || !isfinite(sr) || sr < 8000.0 || sr > 192000.0) return -1;
  memset(x, 0, sizeof *x);
  x->sr = sr;
  x->bpm = 120.0f;
  x->bpb = 4;
  x->rolling = 0;
  x->transport_beats = 0.0;
  x->origin_beats = 0.0;
  x->lim_release = expf(-1.0f / (0.060f * (float)sr));

  /* Allocate stereo ring buffer for TIME slot (~4+ seconds, power of 2). */
  uint32_t need = (uint32_t)(sr * 4.0) + 2048u;
  uint32_t frames = 4096u;
  while (frames < need && frames < (1u << 22)) frames <<= 1;
  x->time_buf = (float*)calloc((size_t)frames * 2u, sizeof(float));
  if (!x->time_buf) return -1;
  x->time_frames = frames;
  x->time_mask = frames - 1u;

  px_reset(x);
  return 0;
}

void px_reset(PulseFx* x) {
  if (!x) return;
  x->rolling = 0;
  x->transport_beats = 0.0;
  x->origin_beats = 0.0;
  x->midi_trig_pending = 0;
  x->env_val = 0.0f;
  x->in_peak = 0.0f;
  x->out_peak = 0.0f;
  x->lim_env = PX_LIM_CEILING;
  x->lim_gr = 0.0f;
  x->mix_cur = 1.0f;
  x->out_gain_cur = 1.0f;
  if (x->time_buf && x->time_frames) {
    memset(x->time_buf, 0, (size_t)x->time_frames * 2u * sizeof(float));
  }
  x->time_w = 0;
  x->time_rpos = 0.0;
  x->time_xfade_pos = 0.0;
  x->time_xfade_rem = 0;
  x->time_xfade_len = 128;
  x->time_anchor = 0.0;
  x->time_prev_step = -1;
  x->time_prev_act = -1;
  x->time_tape_delay = 0.0;
  x->time_motor_speed = 1.0f;
  memset(x->xover_lp, 0, sizeof x->xover_lp);
  memset(x->xover_hp, 0, sizeof x->xover_hp);
  memset(x->drv_dc_x, 0, sizeof x->drv_dc_x);
  memset(x->drv_dc_y, 0, sizeof x->drv_dc_y);
  memset(x->drv_tone, 0, sizeof x->drv_tone);
  memset(x->drv_fold_x1, 0, sizeof x->drv_fold_x1);
  memset(x->crush_phase, 0, sizeof x->crush_phase);
  memset(x->crush_hold, 0, sizeof x->crush_hold);
  memset(x->flt_svf, 0, sizeof x->flt_svf);
  memset(x->phaser_ap, 0, sizeof x->phaser_ap);
  memset(x->phaser_fb, 0, sizeof x->phaser_fb);
  x->pad_prev = PX_PAD_OFF;
  x->pad_anchor = 0.0;
  x->pad_rpos = 0.0;
  x->pad_tape_delay = 0.0;
  x->pad_phase = 0.0;
  x->pad_gain = 0.0f;
  memset(x->pad_svf, 0, sizeof x->pad_svf);
  memset(x->pad_crush_ph, 0, sizeof x->pad_crush_ph);
  memset(x->pad_crush_hold, 0, sizeof x->pad_crush_hold);
  x->xy_gain = 0.0f;
  memset(x->xy_svf, 0, sizeof x->xy_svf);
  for (int s = 0; s < PX_SLOTS; s++) {
    memset(&x->st[s], 0, sizeof x->st[s]);
    x->st[s].bypass_gain = 0.0f;
    x->st[s].last_step = 0;
    x->st[s].audio_armed = 1;
  }
}

void px_free(PulseFx* x) {
  if (!x) return;
  free(x->time_buf);
  x->time_buf = NULL;
  x->time_frames = 0;
  x->time_mask = 0;
}

void px_midi_trigger(PulseFx* x) {
  if (!x) return;
  x->midi_trig_pending = 1;
}

void px_set_transport(PulseFx* x, float bpm, int rolling, int has_pos, double beats, int bpb) {
  if (!x) return;
  if (isfinite(bpm) && bpm >= 20.0f && bpm <= 400.0f) x->bpm = bpm;
  if (bpb >= 1 && bpb <= 16) x->bpb = bpb;

  int was_rolling = x->rolling;
  x->rolling = rolling ? 1 : 0;

  if (!x->rolling) {
    if (was_rolling) {
      x->transport_beats = 0.0;
      x->origin_beats = 0.0;
    }
    return;
  }

  double bt = (double)(x->bpb > 0 ? x->bpb : 4);
  if (!was_rolling) {
    /* Transport just started: anchor origin at bar downbeat so all slots start on beat 1. */
    if (has_pos && isfinite(beats) && beats >= 0.0) {
      x->transport_beats = beats;
      x->origin_beats = floor(beats / bt) * bt;
    } else {
      x->transport_beats = 0.0;
      x->origin_beats = 0.0;
    }
  } else if (has_pos && isfinite(beats) && beats >= 0.0) {
    x->transport_beats = beats;
    if (beats < x->origin_beats) {
      x->origin_beats = floor(beats / bt) * bt;
    }
  }
}

static inline float soft_clip(float v) {
  /* Fast smooth saturation for Punch transients */
  if (v > 1.5f) return 1.0f;
  if (v < -1.5f) return -1.0f;
  return v - (v * v * v) * (4.0f / 27.0f);
}

/* TPT State-Variable Filter (Andrew Simper / Zavalishin): unconditionally stable under modulation. */
static inline void svf_tick(PxSvf* s, float v0, float g, float k,
                            float* lp, float* bp, float* hp) {
  float a1 = 1.0f / (1.0f + g * (g + k));
  float a2 = g * a1;
  float a3 = g * a2;
  float v3 = v0 - s->ic2;
  float v1 = a1 * s->ic1 + a2 * v3;
  float v2 = s->ic2 + a2 * s->ic1 + a3 * v3;
  s->ic1 = 2.0f * v1 - s->ic1;
  s->ic2 = 2.0f * v2 - s->ic2;
  *lp = v2;
  *bp = v1;
  *hp = v0 - k * v1 - v2;
}

/* 1st-order ADAA wavefolder on f(x) = sin(x), antiderivative F(x) = -cos(x). */
static inline float adaa_fold(float x, float* x1) {
  float prev = *x1;
  *x1 = x;
  float dx = x - prev;
  if (fabsf(dx) > 1e-4f) {
    return (cosf(prev) - cosf(x)) / dx;
  }
  return sinf(0.5f * (x + prev));
}

/* 4-point Hermite cubic interpolation from interleaved stereo ring buffer. */
static inline void time_read_hermite(const PulseFx* x, double pos, float* ol, float* or_) {
  double max_pos = (x->time_w > 2) ? (double)(x->time_w - 2) : 0.0;
  double min_pos = (x->time_w > (uint64_t)(x->time_frames - 4))
                     ? (double)(x->time_w - (uint64_t)(x->time_frames - 4))
                     : 0.0;
  if (pos > max_pos) pos = max_pos;
  if (pos < min_pos) pos = min_pos;

  int64_t ipos = (int64_t)floor(pos);
  float t = (float)(pos - (double)ipos);
  uint32_t m = x->time_mask;
  uint32_t i0 = (uint32_t)((ipos - 1) & m) * 2u;
  uint32_t i1 = (uint32_t)((ipos)     & m) * 2u;
  uint32_t i2 = (uint32_t)((ipos + 1) & m) * 2u;
  uint32_t i3 = (uint32_t)((ipos + 2) & m) * 2u;
  const float* b = x->time_buf;

  for (int ch = 0; ch < 2; ch++) {
    float y0 = b[i0 + ch], y1 = b[i1 + ch], y2 = b[i2 + ch], y3 = b[i3 + ch];
    float c0 = y1;
    float c1 = 0.5f * (y2 - y0);
    float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    float v = ((c3 * t + c2) * t + c1) * t + c0;
    if (ch == 0) *ol = v; else *or_ = v;
  }
}

static inline void time_jump(PulseFx* x, double new_pos, int xfade_len) {
  if (fabs(new_pos - x->time_rpos) > 4.0) {
    x->time_xfade_pos = x->time_rpos;
    x->time_xfade_len = xfade_len > 8 ? xfade_len : 8;
    x->time_xfade_rem = x->time_xfade_len;
  }
  x->time_rpos = new_pos;
}

static inline int chop_action_from_level(float lvl_01) {
  float pct = lvl_01 * 100.0f;
  if (pct < 0.5f)  return 0; /* THRU */
  if (pct <= 25.0f) return 1; /* 1/8 */
  if (pct <= 50.0f) return 2; /* 1/16 */
  if (pct <= 75.0f) return 3; /* 1/2x */
  if (pct <= 90.0f) return 4; /* REV */
  return 5;                   /* STOP */
}

/* Single-channel Drive & Crush sample processing. */
static inline float process_drive_ch(PulseFx* x, int ch, float in, int mode,
                                     float drv_amt, float crush_amt, float tone_coeff,
                                     float dc_coeff) {
  float y = in;
  if (mode == PX_DRV_TUBE || mode == PX_DRV_COMBO) {
    float g = 1.0f + 14.0f * drv_amt * drv_amt;
    float u = in * g;
    /* Asymmetric soft tube saturation + DC blocker */
    float s = tanhf(u + 0.14f * u * u) / tanhf(1.0f + 0.35f * (g - 1.0f));
    float dc_out = s - x->drv_dc_x[ch] + dc_coeff * x->drv_dc_y[ch];
    x->drv_dc_x[ch] = s;
    x->drv_dc_y[ch] = dc_out;
    y = dc_out;
  } else if (mode == PX_DRV_FOLD) {
    float g = 1.0f + 9.0f * drv_amt;
    y = adaa_fold(in * g, &x->drv_fold_x1[ch]);
  } else if (mode == PX_DRV_HARD) {
    float g = 1.0f + 18.0f * drv_amt * drv_amt;
    float u = in * g;
    if (u > 0.92f) u = 0.92f + 0.08f * tanhf((u - 0.92f) * 2.0f);
    else if (u < -0.92f) u = -0.92f + 0.08f * tanhf((u + 0.92f) * 2.0f);
    y = u * (1.0f / (1.0f + 0.25f * drv_amt));
  }

  if (mode == PX_DRV_CRUSH || mode == PX_DRV_COMBO) {
    float amt = (mode == PX_DRV_CRUSH) ? fmaxf(drv_amt, crush_amt) : crush_amt;
    /* Sample rate reduction: hold divider 1..28 */
    float step = 1.0f / (1.0f + 27.0f * amt * amt);
    x->crush_phase[ch] += step;
    if (x->crush_phase[ch] >= 1.0f) {
      x->crush_phase[ch] -= floorf(x->crush_phase[ch]);
      /* Bit crush: 16 bits down to 3 bits */
      float bits = 16.0f - 13.0f * amt;
      float levels = powf(2.0f, bits - 1.0f);
      float q = floorf(y * levels + 0.5f) / levels;
      x->crush_hold[ch] = q;
    }
    y = x->crush_hold[ch];
  } else {
    /* Post-drive tone LP for Tube / Fold / Hard */
    x->drv_tone[ch] += tone_coeff * (y - x->drv_tone[ch]);
    y = x->drv_tone[ch];
  }
  return y;
}

/* Single-channel Filter / Phaser processing. */
static inline float process_filter_ch(PulseFx* x, int ch, float in, int mode,
                                      float freq_hz, float res_norm, float curve_val,
                                      float sr_f) {
  float fc = clampf(freq_hz, 30.0f, 0.44f * sr_f);
  if (mode == PX_FLT_PHASER) {
    /* Stereo-offset 4-stage 1st-order allpass phaser */
    float fc_ch = ch == 0 ? fc : clampf(fc * 1.08f, 30.0f, 0.44f * sr_f);
    float w = tanf((float)M_PI * fc_ch / sr_f);
    float a = (1.0f - w) / (1.0f + w);
    float fb = (res_norm * 2.0f - 1.0f) * 0.80f; /* -0.80..+0.80 */
    float u = in + fb * x->phaser_fb[ch];
    u = clampf(u, -4.0f, 4.0f);
    for (int st = 0; st < 4; st++) {
      float ap_out = -a * u + x->phaser_ap[ch][st];
      x->phaser_ap[ch][st] = u + a * ap_out;
      u = ap_out;
    }
    x->phaser_fb[ch] = u;
    return 0.5f * (in + u);
  }

  float g = tanf((float)M_PI * fc / sr_f);
  float q = 0.6f * powf(16.0f, res_norm); /* 0.6 .. 9.6 */
  float k = 1.0f / q;
  float lp = 0.0f, bp = 0.0f, hp = 0.0f;

  if (mode == PX_FLT_LP24) {
    float k2 = 1.0f / (0.6f * powf(8.0f, res_norm));
    float lp1 = 0.0f, bp1 = 0.0f, hp1 = 0.0f;
    svf_tick(&x->flt_svf[ch][0], in, g, k2, &lp1, &bp1, &hp1);
    svf_tick(&x->flt_svf[ch][1], lp1, g, k2, &lp, &bp, &hp);
    return lp;
  }

  svf_tick(&x->flt_svf[ch][0], in, g, k, &lp, &bp, &hp);
  switch (mode) {
    case PX_FLT_LP12:  return lp;
    case PX_FLT_BP:    return bp;
    case PX_FLT_HP:    return hp;
    case PX_FLT_NOTCH: return lp + hp;
    case PX_FLT_MORPH: {
      float m = clampf(curve_val, 0.0f, 1.0f);
      if (m < 0.5f) {
        float t = m * 2.0f;
        return lp + t * (bp - lp);
      } else {
        float t = (m - 0.5f) * 2.0f;
        return bp + t * (hp - bp);
      }
    }
    default: return lp;
  }
}

void px_process(PulseFx* x, const PxParams* p,
                const float* in_l, const float* in_r,
                float* out_l, float* out_r, uint32_t nframes) {
  if (!x || !p || !in_l || !in_r || !out_l || !out_r || nframes == 0) return;
  px_enable_ftz();

  const double sr = x->sr;
  const double beats_per_sample = (double)x->bpm / (60.0 * sr);
  const float  bypass_step = 1.0f / (float)(PX_BYPASS_MS * 0.001 * sr);
  const float  peak_decay = expf(-1.0f / (PX_PEAK_FALL_S * (float)sr));
  const float  punch_decay = expf(-1.0f / (0.015f * (float)sr)); /* 15 ms punch decay */
  const float  param_coeff = 1.0f - expf(-1.0f / (0.005f * (float)sr));
  const float  env_att = 1.0f - expf(-1.0f / (0.002f * (float)sr)); /* 2 ms attack */
  const float  env_rel = 1.0f - expf(-1.0f / (0.080f * (float)sr)); /* 80 ms release */
  const int    audio_holdoff_n = (int)(0.050 * sr);                  /* 50 ms holdoff */

  const float  mix_target = clampf(p->mix, 0.0f, 1.0f);
  const float  out_gain_target = px_db(clampf(p->output_db, -24.0f, 6.0f));
  const float  free_hz = clampf(p->free_hz, 0.25f, 32.0f);

  /* Consume pending MIDI trigger for slots in MIDI mode */
  const int midi_trig = x->midi_trig_pending;
  x->midi_trig_pending = 0;

  /* Precompute per-slot stepping and smoothing coefficients for this block */
  int    slot_steps[PX_SLOTS];
  float  slot_step_beats[PX_SLOTS];
  int    slot_clock_mode[PX_SLOTS]; /* 0=Sync, 1=Free, 2=Audio, 3=MIDI */
  double slot_free_inc[PX_SLOTS];
  float  slot_smooth_coeff[PX_SLOTS];
  float  slot_punch_amt[PX_SLOTS];
  float  slot_thr_lin[PX_SLOTS];

  for (int s = 0; s < PX_SLOTS; s++) {
    const PxSlotParams* sp = &p->slot[s];
    int nst = sp->steps;
    if (nst < 1) nst = 1;
    if (nst > PX_MAX_STEPS) nst = PX_MAX_STEPS;
    slot_steps[s] = nst;

    int sr_idx = sp->step_rate;
    if (sr_idx < 0) sr_idx = 0;
    if (sr_idx >= PX_NRATES) sr_idx = PX_NRATES - 1;

    int cmode = sp->trig;
    if (sr_idx == PX_NRATES - 1 && cmode == PX_TRIG_SYNC) cmode = PX_TRIG_FREE;
    slot_clock_mode[s] = cmode;
    slot_step_beats[s] = (sr_idx < PX_NRATES - 1) ? px_rate_beats[sr_idx] : 0.25f;
    if (sr_idx == PX_NRATES - 1) {
      slot_free_inc[s] = (double)free_hz / sr;
    } else {
      slot_free_inc[s] = beats_per_sample / (double)slot_step_beats[s];
    }

    float sm = clampf(sp->smooth, -1.0f, 1.0f);
    float tau_s = (sm >= 0.0f) ? (0.0005f * powf(160.0f, sm)) : 0.0005f;
    slot_smooth_coeff[s] = 1.0f - expf(-1.0f / (tau_s * (float)sr));
    slot_punch_amt[s] = (s == PX_SLOT_GATE && sm < 0.0f) ? -sm : 0.0f;
    slot_thr_lin[s] = px_db(clampf(sp->threshold_db, -60.0f, 0.0f));

    if (midi_trig && cmode == PX_TRIG_MIDI) {
      x->st[s].trig_active = 1;
      x->st[s].free_pos = 0.0;
    }
  }

  const int rolling = x->rolling;
  double base_rel_beats = rolling ? (x->transport_beats - x->origin_beats) : 0.0;
  if (base_rel_beats < 0.0) base_rel_beats = 0.0;

  /* Low Split LR4 crossover setup (bypassed completely when < 60 Hz) */
  const int   xover_on = (p->low_split_hz >= 60.0f);
  const float xover_fc = clampf(p->low_split_hz, 60.0f, 500.0f);
  const float xover_g  = xover_on ? tanf((float)M_PI * xover_fc / (float)sr) : 0.0f;
  const float xover_k  = 1.41421356f; /* Butterworth Q = 1/sqrt(2) -> cascaded = LR4 */

  /* Time slot parameters */
  const PxSlotParams* tp = &p->slot[PX_SLOT_TIME];
  const int    time_mode = tp->mode;
  const float  time_p1   = clampf(tp->param1, 0.0f, 100.0f);
  const float  time_win  = clampf(tp->param2 * 0.01f, 0.0f, 1.0f);
  const int    time_xfade_len = (int)((2.0f + 8.0f * time_win) * 0.001f * (float)sr);
  const double samples_per_beat = (60.0 / (double)x->bpm) * sr;
  const double time_range_beats = (time_p1 < 25.0f) ? 0.5 : (time_p1 < 50.0f) ? 1.0 : (time_p1 < 75.0f) ? 2.0 : 4.0;
  const double time_max_del = (x->time_frames > 16) ? (double)(x->time_frames - 16) : 64.0;
  const double time_range_samp = fmin(time_range_beats * samples_per_beat, time_max_del);

  /* Drive & Crush parameters */
  const PxSlotParams* dp = &p->slot[PX_SLOT_DRIVE];
  const int   drv_mode  = dp->mode;
  const float drv_base  = clampf(dp->param1 * 0.01f, 0.0f, 1.0f);
  const float drv_p2    = clampf(dp->param2 * 0.01f, 0.0f, 1.0f);
  const float drv_tone_hz = 800.0f * powf(22.5f, drv_p2); /* 800 Hz .. 18 kHz */
  const float drv_tone_coeff = 1.0f - expf(-2.0f * (float)M_PI * drv_tone_hz / (float)sr);
  const float drv_dc_coeff = expf(-2.0f * (float)M_PI * 15.0f / (float)sr);

  /* Filter parameters */
  const PxSlotParams* fp = &p->slot[PX_SLOT_FILTER];
  const int   flt_mode   = fp->mode;
  const float flt_cut_hz = 40.0f * powf(450.0f, clampf(fp->param1 * 0.01f, 0.0f, 1.0f));
  const float flt_res    = clampf(fp->param2 * 0.01f, 0.0f, 1.0f);

  /* Gate slot parameters */
  const PxSlotParams* gp = &p->slot[PX_SLOT_GATE];
  const float gate_floor = clampf(gp->param1 * 0.01f, 0.0f, 1.0f);
  const float gate_width = clampf(gp->param2 * 0.01f, 0.0f, 1.0f);
  const int   gate_mode  = gp->mode;
  const int   low_duck   = p->low_duck ? 1 : 0;

  float in_pk = x->in_peak;
  float out_pk = x->out_peak;
  float env_v = x->env_val;
  float lim_env = x->lim_env;
  float lim_gr_max = 0.0f;

  for (uint32_t i = 0; i < nframes; i++) {
    float dl = in_l[i];
    float dr = in_r[i];
    if (!isfinite(dl)) dl = 0.0f;
    if (!isfinite(dr)) dr = 0.0f;

    float a_in = fmaxf(fabsf(dl), fabsf(dr));
    in_pk = (a_in > in_pk) ? a_in : in_pk * peak_decay;
    env_v += ((a_in > env_v) ? env_att : env_rel) * (a_in - env_v);

    double cur_beats = base_rel_beats + (double)i * beats_per_sample;
    float eff_depth[PX_SLOTS];

    /* Advance all 4 polyrhythmic Step Shapers + Audio/MIDI Triggers + Env -> Depth */
    for (int s = 0; s < PX_SLOTS; s++) {
      const PxSlotParams* sp = &p->slot[s];
      PxSlotState* st = &x->st[s];
      int nst = slot_steps[s];
      int cmode = slot_clock_mode[s];
      float thr = slot_thr_lin[s];

      /* Env -> Depth normalized above slot threshold */
      float env_norm = clampf((env_v - thr) / fmaxf(1.0f - thr, 0.05f), 0.0f, 1.0f);
      eff_depth[s] = clampf(sp->depth + env_norm * sp->env_depth, -1.0f, 1.0f);

      if (cmode == PX_TRIG_SYNC) {
        st->trig_active = 0;
        st->audio_armed = 1;
      } else if (cmode == PX_TRIG_AUDIO) {
        if (st->audio_holdoff > 0) st->audio_holdoff--;
        if (st->audio_armed && st->audio_holdoff == 0 && env_v >= thr && env_v > 1e-4f) {
          st->trig_active = 1;
          st->free_pos = 0.0;
          st->audio_armed = 0;
          st->audio_holdoff = audio_holdoff_n;
        } else if (env_v < thr * 0.5f) {
          st->audio_armed = 1;
        }
      }

      int slot_running = (cmode == PX_TRIG_FREE) ? 1
                       : (cmode == PX_TRIG_SYNC) ? rolling
                       : st->trig_active;

      if (!slot_running) {
        st->phase = 0.0f;
        st->last_step = 0;
        st->last_frac = 0.0f;
        st->free_pos = 0.0;
        st->curve_raw = 0.0f;
        st->curve_smooth = 0.0f;
        st->prev_raw = 0.0f;
        st->punch_env = 0.0f;
        if (s == PX_SLOT_TIME) {
          x->time_prev_step = -1;
          x->time_prev_act = -1;
          x->time_tape_delay = 0.0;
        }
        if (st->bypass_gain > 0.0f) {
          st->bypass_gain -= bypass_step;
          if (st->bypass_gain < 0.0f) st->bypass_gain = 0.0f;
        }
        continue;
      }

      double pos;
      if (cmode == PX_TRIG_SYNC) {
        double total = cur_beats / (double)slot_step_beats[s];
        pos = fmod(total, (double)nst);
        if (pos < 0.0) pos += (double)nst;
        st->free_pos = pos;
      } else {
        pos = st->free_pos;
        st->free_pos += slot_free_inc[s];
        if (st->free_pos >= (double)nst) {
          if (cmode == PX_TRIG_AUDIO || cmode == PX_TRIG_MIDI) {
            /* Audio & MIDI triggers run one full pattern cycle per trigger, then park */
            st->free_pos = 0.0;
            st->trig_active = 0;
          } else {
            st->free_pos = fmod(st->free_pos, (double)nst);
          }
        }
        if (st->free_pos < 0.0) st->free_pos = 0.0;
      }

      int idx = (int)pos;
      if (idx < 0) idx = 0;
      if (idx >= nst) idx = nst - 1;
      float frac = (float)(pos - (double)idx);

      st->phase = (float)(pos / (double)nst);
      st->last_step = idx;
      st->last_frac = frac;

      float lvl = clampf(sp->levels[idx], 0.0f, 1.0f);
      float raw = lvl * px_eval_shape(sp->shapes[idx], frac, sp->warp);
      st->curve_raw = raw;
      st->curve_smooth += slot_smooth_coeff[s] * (raw - st->curve_smooth);

      /* Punch transient detector on rising edge */
      float diff = raw - st->prev_raw;
      st->prev_raw = raw;
      if (slot_punch_amt[s] > 0.0f) {
        if (diff > 0.08f) {
          float trig = clampf(diff * 2.5f, 0.0f, 1.0f);
          if (trig > st->punch_env) st->punch_env = trig;
        }
        st->punch_env *= punch_decay;
      } else {
        st->punch_env = 0.0f;
      }

      /* Smooth bypass crossfade */
      float target_on = sp->on ? 1.0f : 0.0f;
      if (st->bypass_gain < target_on) {
        st->bypass_gain += bypass_step;
        if (st->bypass_gain > 1.0f) st->bypass_gain = 1.0f;
      } else if (st->bypass_gain > target_on) {
        st->bypass_gain -= bypass_step;
        if (st->bypass_gain < 0.0f) st->bypass_gain = 0.0f;
      }
    }

    /* Low Split LR4 Crossover */
    float low_l = 0.0f, low_r = 0.0f;
    float wl = dl, wr = dr;
    if (xover_on) {
      float lp1, bp1, hp1, lp2, bp2, hp2;
      /* Left */
      svf_tick(&x->xover_lp[0][0], dl, xover_g, xover_k, &lp1, &bp1, &hp1);
      svf_tick(&x->xover_lp[0][1], lp1, xover_g, xover_k, &low_l, &bp2, &hp2);
      svf_tick(&x->xover_hp[0][0], dl, xover_g, xover_k, &lp1, &bp1, &hp1);
      svf_tick(&x->xover_hp[0][1], hp1, xover_g, xover_k, &lp2, &bp2, &wl);
      /* Right */
      svf_tick(&x->xover_lp[1][0], dr, xover_g, xover_k, &lp1, &bp1, &hp1);
      svf_tick(&x->xover_lp[1][1], lp1, xover_g, xover_k, &low_r, &bp2, &hp2);
      svf_tick(&x->xover_hp[1][0], dr, xover_g, xover_k, &lp1, &bp1, &hp1);
      svf_tick(&x->xover_hp[1][1], hp1, xover_g, xover_k, &lp2, &bp2, &wr);
    }

    /* Slot 1: TIME (always record into ring buffer so enabling mid-bar has audio immediately) */
    if (x->time_buf) {
      uint32_t wi = (uint32_t)(x->time_w & x->time_mask) * 2u;
      x->time_buf[wi]     = wl;
      x->time_buf[wi + 1] = wr;
      x->time_w++;

      PxSlotState* tst = &x->st[PX_SLOT_TIME];
      if (tst->bypass_gain > 1e-6f && x->time_w > 4) {
        float t_depth = eff_depth[PX_SLOT_TIME];
        float ad = fabsf(t_depth);
        float c = clampf(tst->curve_smooth, 0.0f, 1.0f);
        double now_pos = (double)(x->time_w - 1);

        if (time_mode == PX_TIME_CURVE) {
          float shaped = (t_depth >= 0.0f) ? c : (1.0f - c);
          double del = (double)(shaped * ad) * time_range_samp;
          x->time_rpos = now_pos - del;
        } else if (time_mode == PX_TIME_CHOP) {
          int act = (ad < 0.05f) ? 0 : chop_action_from_level(tp->levels[tst->last_step]);
          if (tst->last_step != x->time_prev_step || act != x->time_prev_act) {
            x->time_prev_step = tst->last_step;
            x->time_prev_act = act;
            x->time_anchor = now_pos;
            if (act == 1 || act == 2) {
              double rep_len = fmin(((act == 1) ? 0.5 : 0.25) * samples_per_beat, time_max_del * 0.5);
              time_jump(x, now_pos - rep_len, time_xfade_len);
            } else if (act == 3 || act == 4 || act == 5) {
              time_jump(x, now_pos, time_xfade_len);
            } else {
              time_jump(x, now_pos, time_xfade_len);
            }
          }
          if (act == 0) {
            x->time_rpos = now_pos;
          } else if (act == 1 || act == 2) {
            double rep_len = fmin(((act == 1) ? 0.5 : 0.25) * samples_per_beat, time_max_del * 0.5);
            if (rep_len < 64.0) rep_len = 64.0;
            x->time_rpos += 1.0;
            if (x->time_rpos >= x->time_anchor) {
              time_jump(x, x->time_anchor - rep_len, time_xfade_len);
            }
          } else if (act == 3) {
            x->time_rpos += 0.5;
          } else if (act == 4) {
            x->time_rpos -= 1.0;
          } else { /* STOP */
            float spd = clampf(1.0f - tst->last_frac * 1.15f, 0.0f, 1.0f);
            x->time_rpos += (double)spd;
          }
        } else if (time_mode == PX_TIME_STUTTER) {
          float v = c * ad;
          int div = (v < 0.10f) ? 0 : (v < 0.35f) ? 1 : (v < 0.60f) ? 2 : (v < 0.85f) ? 3 : 4;
          if (div != x->time_prev_act || tst->last_step != x->time_prev_step) {
            x->time_prev_act = div;
            x->time_prev_step = tst->last_step;
            x->time_anchor = now_pos;
            if (div > 0) {
              static const double div_beats[4] = { 0.5, 0.25, 0.125, 0.0625 };
              double rep_len = fmin(div_beats[div - 1] * samples_per_beat, time_max_del * 0.5);
              time_jump(x, now_pos - rep_len, time_xfade_len);
            } else {
              time_jump(x, now_pos, time_xfade_len);
            }
          }
          if (div == 0) {
            x->time_rpos = now_pos;
          } else {
            static const double div_beats[4] = { 0.5, 0.25, 0.125, 0.0625 };
            double rep_len = fmin(div_beats[div - 1] * samples_per_beat, time_max_del * 0.5);
            if (rep_len < 32.0) rep_len = 32.0;
            x->time_rpos += 1.0;
            if (x->time_rpos >= x->time_anchor) {
              time_jump(x, x->time_anchor - rep_len, time_xfade_len);
            }
          }
        } else { /* PX_TIME_TAPESTOP */
          float shaped = (t_depth >= 0.0f) ? c : (1.0f - c);
          float target_spd = clampf(1.0f - shaped * ad, 0.0f, 1.0f);
          /* Param 1 (Range 0..100%) controls motor inertia: 20 ms .. 800 ms */
          float inertia_s = 0.020f * powf(40.0f, time_p1 * 0.01f);
          float m_coeff = 1.0f - expf(-1.0f / (inertia_s * (float)sr));
          x->time_motor_speed += m_coeff * (target_spd - x->time_motor_speed);
          if (x->time_motor_speed < 0.995f) {
            x->time_tape_delay += (1.0 - (double)x->time_motor_speed);
            if (x->time_tape_delay > time_max_del) x->time_tape_delay = time_max_del;
          } else if (x->time_tape_delay > 0.0) {
            x->time_tape_delay *= 0.996;
            if (x->time_tape_delay < 0.5) x->time_tape_delay = 0.0;
          }
          x->time_rpos = now_pos - x->time_tape_delay;
        }

        float tl = wl, tr = wr;
        time_read_hermite(x, x->time_rpos, &tl, &tr);
        if (x->time_xfade_rem > 0) {
          float xl = tl, xr = tr;
          time_read_hermite(x, x->time_xfade_pos, &xl, &xr);
          x->time_xfade_pos += 1.0;
          float a = (float)x->time_xfade_rem / (float)x->time_xfade_len;
          tl = tl + a * (xl - tl);
          tr = tr + a * (xr - tr);
          x->time_xfade_rem--;
        }
        float bg = tst->bypass_gain;
        wl = wl + bg * (tl - wl);
        wr = wr + bg * (tr - wr);
      } else {
        x->time_rpos = (double)(x->time_w - 1);
        x->time_prev_step = -1;
        x->time_prev_act = -1;
        x->time_tape_delay = 0.0;
      }
    }

    /* Slot 2: DRIVE & CRUSH */
    PxSlotState* dst = &x->st[PX_SLOT_DRIVE];
    if (dst->bypass_gain > 1e-6f) {
      float drv_depth = eff_depth[PX_SLOT_DRIVE];
      float c = clampf(dst->curve_smooth, 0.0f, 1.0f);
      float shaped = (drv_depth >= 0.0f) ? c : (1.0f - c);
      float ad = fabsf(drv_depth);
      float mod_drv = clampf(drv_base * (1.0f - ad) + drv_base * shaped * ad + (1.0f - drv_base) * 0.5f * shaped * ad, 0.0f, 1.0f);
      float mod_crush = clampf(drv_p2 * (1.0f - ad + ad * shaped), 0.0f, 1.0f);
      float dl_out = process_drive_ch(x, 0, wl, drv_mode, mod_drv, mod_crush, drv_tone_coeff, drv_dc_coeff);
      float dr_out = process_drive_ch(x, 1, wr, drv_mode, mod_drv, mod_crush, drv_tone_coeff, drv_dc_coeff);
      float bg = dst->bypass_gain;
      wl = wl + bg * (dl_out - wl);
      wr = wr + bg * (dr_out - wr);
    }

    /* Slot 3: FILTER (SVF + Phaser) */
    PxSlotState* fst = &x->st[PX_SLOT_FILTER];
    if (fst->bypass_gain > 1e-6f) {
      float flt_depth = eff_depth[PX_SLOT_FILTER];
      float c = clampf(fst->curve_smooth, 0.0f, 1.0f);
      float oct_shift = (c * 2.0f - 1.0f) * flt_depth * 3.5f; /* +-3.5 octaves */
      float fc = flt_cut_hz * powf(2.0f, oct_shift);
      float morph_pos = clampf(0.5f + (c - 0.5f) * flt_depth, 0.0f, 1.0f);
      float fl_out = process_filter_ch(x, 0, wl, flt_mode, fc, flt_res, morph_pos, (float)sr);
      float fr_out = process_filter_ch(x, 1, wr, flt_mode, fc, flt_res, morph_pos, (float)sr);
      float bg = fst->bypass_gain;
      wl = wl + bg * (fl_out - wl);
      wr = wr + bg * (fr_out - wr);
    }

    /* Slot 4: GATE & SPACE */
    PxSlotState* gst = &x->st[PX_SLOT_GATE];
    if (gst->bypass_gain > 1e-6f) {
      float gate_depth = eff_depth[PX_SLOT_GATE];
      float gl = wl;
      float gr = wr;
      float c = clampf(gst->curve_smooth, 0.0f, 1.0f);
      float ad = fabsf(gate_depth);
      float gate_env_for_low = 1.0f;

      if (gate_mode == PX_GATE_GATE) {
        float shaped = (gate_depth >= 0.0f) ? c : (1.0f - c);
        float target_g = gate_floor + (1.0f - gate_floor) * shaped;
        float gain = 1.0f + ad * (target_g - 1.0f);
        gate_env_for_low = gain;
        if (slot_punch_amt[PX_SLOT_GATE] > 0.0f && gst->punch_env > 1e-4f) {
          float boost = 1.0f + slot_punch_amt[PX_SLOT_GATE] * gst->punch_env * 1.0f;
          gl = soft_clip(gl * gain * boost);
          gr = soft_clip(gr * gain * boost);
        } else {
          gl *= gain;
          gr *= gain;
        }
      } else if (gate_mode == PX_GATE_PAN) {
        float pan = (c * 2.0f - 1.0f) * gate_depth; /* -1..+1 */
        float angle = (pan * 0.5f + 0.5f) * (float)(M_PI * 0.5);
        gl *= cosf(angle) * 1.41421356f;
        gr *= sinf(angle) * 1.41421356f;
      } else if (gate_mode == PX_GATE_WIDTH) {
        float mid = 0.5f * (gl + gr);
        float side = 0.5f * (gl - gr);
        float w_mod = gate_width * 2.0f * (1.0f - ad + ad * ((gate_depth >= 0.0f) ? c : (1.0f - c)));
        gl = mid + side * w_mod;
        gr = mid - side * w_mod;
      } else { /* PX_GATE_GATEPAN: ping-pong chopper */
        float shaped = (gate_depth >= 0.0f) ? c : (1.0f - c);
        float target_g = gate_floor + (1.0f - gate_floor) * shaped;
        float gain = 1.0f + ad * (target_g - 1.0f);
        gate_env_for_low = gain;
        float side_sign = (gst->last_step & 1) ? 1.0f : -1.0f;
        float pan = side_sign * gate_width * ad;
        float angle = (pan * 0.5f + 0.5f) * (float)(M_PI * 0.5);
        gl *= gain * cosf(angle) * 1.41421356f;
        gr *= gain * sinf(angle) * 1.41421356f;
      }

      float bg = gst->bypass_gain;
      wl = wl + bg * (gl - wl);
      wr = wr + bg * (gr - wr);

      if (xover_on && low_duck) {
        float lg = 1.0f + bg * (gate_env_for_low - 1.0f);
        low_l *= lg;
        low_r *= lg;
      }
    }

    /* Recombine Low Band + Main/High Band */
    if (xover_on) {
      wl += low_l;
      wr += low_r;
    }

    /* Live Performance Pad (momentary override on output, returns to grid on release) */
    {
      int pad = p->live_pad;
      if (pad < PX_PAD_OFF || pad > PX_PAD_BITCRUSH) pad = PX_PAD_OFF;
      float target_pg = (pad != PX_PAD_OFF) ? 1.0f : 0.0f;
      if (x->pad_gain < target_pg) {
        x->pad_gain += bypass_step;
        if (x->pad_gain > 1.0f) x->pad_gain = 1.0f;
      } else if (x->pad_gain > target_pg) {
        x->pad_gain -= bypass_step;
        if (x->pad_gain < 0.0f) x->pad_gain = 0.0f;
      }
      if (pad != x->pad_prev) {
        x->pad_prev = pad;
        x->pad_anchor = (x->time_w > 1) ? (double)(x->time_w - 1) : 0.0;
        x->pad_rpos = x->pad_anchor;
        x->pad_tape_delay = 0.0;
        x->pad_phase = 0.0;
        if (pad == PX_PAD_RPT16 || pad == PX_PAD_RPT8) {
          double rlen = fmin(((pad == PX_PAD_RPT16) ? 0.25 : 0.5) * samples_per_beat, time_max_del * 0.5);
          x->pad_rpos = x->pad_anchor - rlen;
        }
      }
      if (x->pad_gain > 1e-6f && pad != PX_PAD_OFF && x->time_buf && x->time_w > 4) {
        float pl = wl, pr = wr;
        double now_pos = (double)(x->time_w - 1);
        if (pad == PX_PAD_TAPESTOP) {
          /* Brake time controlled by Slot 1 Param 1 (Range): 0.25 beat .. 4.0 beats (default 50% = 1 beat) */
          double stop_beats = 0.25 * pow(16.0, (double)time_p1 * 0.01);
          x->pad_phase += 1.0 / fmax(samples_per_beat * stop_beats, 1000.0);
          float spd = clampf(1.0f - (float)x->pad_phase, 0.0f, 1.0f);
          x->pad_tape_delay += (1.0 - (double)spd);
          if (x->pad_tape_delay > time_max_del) x->pad_tape_delay = time_max_del;
          time_read_hermite(x, now_pos - x->pad_tape_delay, &pl, &pr);
        } else if (pad == PX_PAD_RPT16 || pad == PX_PAD_RPT8) {
          double rlen = fmin(((pad == PX_PAD_RPT16) ? 0.25 : 0.5) * samples_per_beat, time_max_del * 0.5);
          if (rlen < 64.0) rlen = 64.0;
          x->pad_rpos += 1.0;
          if (x->pad_rpos >= x->pad_anchor) x->pad_rpos = x->pad_anchor - rlen;
          time_read_hermite(x, x->pad_rpos, &pl, &pr);
        } else if (pad == PX_PAD_REVERSE) {
          x->pad_rpos -= 1.0;
          time_read_hermite(x, x->pad_rpos, &pl, &pr);
        } else if (pad == PX_PAD_HALF) {
          x->pad_rpos += 0.5;
          time_read_hermite(x, x->pad_rpos, &pl, &pr);
        } else if (pad == PX_PAD_GATECHOP) {
          x->pad_phase += 1.0 / fmax(samples_per_beat * 0.25, 256.0); /* 1/16th chop */
          double f = x->pad_phase - floor(x->pad_phase);
          float g = (f < 0.5) ? 1.0f : 0.0f;
          pl *= g; pr *= g;
        } else if (pad == PX_PAD_FILTDROP) {
          x->pad_phase += 1.0 / fmax(samples_per_beat * 4.0, 4000.0); /* 1 bar sweep */
          float t = clampf((float)x->pad_phase, 0.0f, 1.0f);
          float fc = 12000.0f * powf(120.0f / 12000.0f, t);
          float g = tanf((float)M_PI * clampf(fc, 40.0f, 0.44f * (float)sr) / (float)sr);
          float lp, bp, hp;
          svf_tick(&x->pad_svf[0], wl, g, 0.45f, &lp, &bp, &hp); pl = lp;
          svf_tick(&x->pad_svf[1], wr, g, 0.45f, &lp, &bp, &hp); pr = lp;
        } else if (pad == PX_PAD_BITCRUSH) {
          float step = 1.0f / 12.0f;
          for (int ch = 0; ch < 2; ch++) {
            float s = (ch == 0) ? wl : wr;
            x->pad_crush_ph[ch] += step;
            if (x->pad_crush_ph[ch] >= 1.0f) {
              x->pad_crush_ph[ch] -= 1.0f;
              x->pad_crush_hold[ch] = floorf(s * 16.0f + 0.5f) / 16.0f;
            }
          }
          pl = x->pad_crush_hold[0];
          pr = x->pad_crush_hold[1];
        }
        wl = wl + x->pad_gain * (pl - wl);
        wr = wr + x->pad_gain * (pr - wr);
      }
    }

    /* Live XY Pad macro filter (X: Cutoff 80..16000 Hz, Y: Resonance / Depth) */
    {
      float target_xy = p->xy_on ? 1.0f : 0.0f;
      if (x->xy_gain < target_xy) {
        x->xy_gain += bypass_step;
        if (x->xy_gain > 1.0f) x->xy_gain = 1.0f;
      } else if (x->xy_gain > target_xy) {
        x->xy_gain -= bypass_step;
        if (x->xy_gain < 0.0f) x->xy_gain = 0.0f;
      }
      if (x->xy_gain > 1e-6f) {
        float xx = clampf(p->xy_x, 0.0f, 1.0f);
        float yy = clampf(p->xy_y, 0.0f, 1.0f);
        float fc = 80.0f * powf(200.0f, xx);
        float g = tanf((float)M_PI * clampf(fc, 40.0f, 0.44f * (float)sr) / (float)sr);
        float k = 1.0f / (0.7f + 7.5f * yy);
        float lp, bp, hp;
        svf_tick(&x->xy_svf[0], wl, g, k, &lp, &bp, &hp);
        float xl = lp;
        svf_tick(&x->xy_svf[1], wr, g, k, &lp, &bp, &hp);
        float xr = lp;
        float amt = x->xy_gain * (0.25f + 0.75f * yy);
        wl = wl + amt * (xl - wl);
        wr = wr + amt * (xr - wr);
      }
    }

    /* Dry/Wet mix + Output Gain */
    x->mix_cur += param_coeff * (mix_target - x->mix_cur);
    x->out_gain_cur += param_coeff * (out_gain_target - x->out_gain_cur);

    float ol = (dl + x->mix_cur * (wl - dl)) * x->out_gain_cur;
    float or_ = (dr + x->mix_cur * (wr - dr)) * x->out_gain_cur;

    /* Stereo-linked zero-latency brickwall limiter (0 dBFS ceiling) */
    if (lim_env > PX_LIM_CEILING) {
      lim_env = PX_LIM_CEILING + x->lim_release * (lim_env - PX_LIM_CEILING);
      if (lim_env - PX_LIM_CEILING < 1e-6f) lim_env = PX_LIM_CEILING;
    }
    float pk = fmaxf(fabsf(ol), fabsf(or_));
    if (pk > lim_env) lim_env = pk;
    if (lim_env > PX_LIM_CEILING) {
      float g = PX_LIM_CEILING / lim_env;
      ol *= g;
      or_ *= g;
      float gr = 1.0f - g;
      if (gr > lim_gr_max) lim_gr_max = gr;
    }

    float a_out = fmaxf(fabsf(ol), fabsf(or_));
    out_pk = (a_out > out_pk) ? a_out : out_pk * peak_decay;

    out_l[i] = ol;
    out_r[i] = or_;
  }

  if (rolling) {
    x->transport_beats += (double)nframes * beats_per_sample;
  }

  x->in_peak = in_pk;
  x->out_peak = out_pk;
  x->env_val = env_v;
  x->lim_env = lim_env;
  x->lim_gr = lim_gr_max;
}
