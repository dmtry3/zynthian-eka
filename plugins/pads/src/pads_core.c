#include "pads_core.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static inline float clampf(float x, float lo, float hi) {
  return x < lo ? lo : (x > hi ? hi : x);
}

static inline float fast_tanh(float x) {
  if (x > 3.0f) return 1.0f;
  if (x < -3.0f) return -1.0f;
  float x2 = x * x;
  return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

/* ---------- Lightweight Room Reverb (Space) ---------- */

static void reverb_init(EpReverb* rv, double sr) {
  memset(rv, 0, sizeof *rv);
  /* Prime-like room delay lengths scaled from 48 kHz */
  static const int base_comb[EP_REV_COMBS] = { 1217, 1429, 1613, 1811 };
  static const int base_ap[EP_REV_APS] = { 241, 563 };
  double scale = sr / 48000.0;
  for (int ch = 0; ch < 2; ch++) {
    int spread = ch ? 23 : 0;
    for (int i = 0; i < EP_REV_COMBS; i++) {
      int len = (int)((base_comb[i] + spread) * scale);
      if (len < 64) len = 64;
      if (len >= EP_REV_MAXBUF) len = EP_REV_MAXBUF - 1;
      rv->comb_len[ch][i] = len;
    }
    for (int i = 0; i < EP_REV_APS; i++) {
      int len = (int)((base_ap[i] + spread) * scale);
      if (len < 32) len = 32;
      if (len >= 1024) len = 1023;
      rv->ap_len[ch][i] = len;
    }
  }
}

static void reverb_process(EpReverb* rv, double sr, const float* in_l, const float* in_r,
                           float* out_l, float* out_r, uint32_t nframes) {
  float in_e = 0.0f;
  for (uint32_t i = 0; i < nframes; i++) {
    in_e += fabsf(in_l[i]) + fabsf(in_r[i]);
  }
  if (in_e > 1e-7f) {
    rv->tail_samples = (int)(sr * 2.0);
  } else if (rv->tail_samples <= 0 && rv->energy < 1e-6f) {
    rv->energy = 0.0f;
    return;
  } else if (rv->tail_samples > 0) {
    rv->tail_samples -= (int)nframes;
    if (rv->tail_samples < 0) rv->tail_samples = 0;
  }

  const float fb = 0.78f;
  const float damp = 0.35f;
  const float wet = 0.32f;
  float out_e = 0.0f;

  for (uint32_t i = 0; i < nframes; i++) {
    float x[2] = { in_l[i] * 0.25f, in_r[i] * 0.25f };
    float y[2] = { 0.0f, 0.0f };

    for (int ch = 0; ch < 2; ch++) {
      float sum = 0.0f;
      for (int c = 0; c < EP_REV_COMBS; c++) {
        int p = rv->comb_pos[ch][c];
        float d = rv->comb_buf[ch][c][p];
        rv->comb_lp[ch][c] = d * (1.0f - damp) + rv->comb_lp[ch][c] * damp;
        rv->comb_buf[ch][c][p] = x[ch] + rv->comb_lp[ch][c] * fb;
        if (++p >= rv->comb_len[ch][c]) p = 0;
        rv->comb_pos[ch][c] = p;
        sum += d;
      }
      for (int a = 0; a < EP_REV_APS; a++) {
        int p = rv->ap_pos[ch][a];
        float d = rv->ap_buf[ch][a][p];
        float v = sum + d * 0.5f;
        rv->ap_buf[ch][a][p] = v;
        if (++p >= rv->ap_len[ch][a]) p = 0;
        rv->ap_pos[ch][a] = p;
        sum = d - v * 0.5f;
      }
      y[ch] = sum * wet;
      out_e += fabsf(y[ch]);
    }
    out_l[i] += y[0];
    out_r[i] += y[1];
  }
  rv->energy = out_e / (float)(nframes > 0 ? nframes : 1);
}

/* ---------- Pad Defaults & Core Init ---------- */

void ep_pad_defaults(EpPadParams* p, int pad_index) {
  memset(p, 0, sizeof *p);
  p->pitch = 0.0f;
  p->decay = 100.0f;
  p->filter = 0.0f;
  p->level = 80.0f;
  p->start = 0.0f;
  p->end = 100.0f;
  p->mode = EP_MODE_ONESHOT;
  p->punch = 0.0f;
  p->drive = 0.0f;
  p->pan = 0.0f;
  p->vel_sens = 75.0f;
  p->choke = 0;
  p->send = 15.0f;
  p->mute = 0;
  p->solo = 0;
  p->midi_note = 36 + (pad_index & 15);
}

static void init_scene_ranges(EpScene* sc) {
  for (int p = 0; p < EP_PADS; p++) {
    sc->pad_start[p] = 0;
    sc->pad_end[p] = EP_STEPS_DEF;
  }
}

void ep_core_init(EpCore* c, double sr) {
  memset(c, 0, sizeof *c);
  c->sr = sr > 1000.0 ? sr : 48000.0;
  c->master_vol = 85.0f;
  c->stereo_width = 100.0f;
  c->sel_playhead = -1.0f;
  for (int i = 0; i < EP_PADS; i++) {
    ep_pad_defaults(&c->pad[i], i);
    c->gen[i] = 1;
    c->seq.pad_last_step[i] = -1;
    c->seq.pad_cur_step[i] = -1;
    c->seq.skip_step[i] = -1;
  }
  for (int s = 0; s < EP_SCENES; s++) {
    init_scene_ranges(&c->scenes[s]);
  }
  c->seq.mode = EP_SEQ_STOP;
  c->seq.scene = 0;
  c->seq.pend_scene = -1;
  c->seq.cur_step = -1;
  c->seq.num_steps = EP_STEPS_DEF;
  c->seq.quantize = 1; /* 1/16 */
  c->seq.bpm = 120.0;
  c->seq.swing = 50.0f;
  c->seq.last_step_fired = -1;
  reverb_init(&c->rev, c->sr);
}

void ep_core_reset(EpCore* c) {
  memset(c->voices, 0, sizeof c->voices);
  memset(c->activity, 0, sizeof c->activity);
  c->sel_playhead = -1.0f;
  c->seq.bar_phase = 0.0;
  c->seq.cur_step = (c->seq.mode == EP_SEQ_STOP) ? -1 : 0;
  c->seq.last_step_fired = -1;
  c->seq.skip_next_mask = 0;
  for (int i = 0; i < EP_PADS; i++) {
    c->seq.pad_phase[i] = 0.0;
    c->seq.pad_last_step[i] = -1;
    c->seq.pad_cur_step[i] = -1;
    c->seq.skip_step[i] = -1;
  }
  memset(c->seq.roll_rem, 0, sizeof c->seq.roll_rem);
  reverb_init(&c->rev, c->sr);
}

uint32_t ep_core_set_source(EpCore* c, int pad, const EpSource* src) {
  if (pad < 0 || pad >= EP_PADS) return 0;
  uint32_t old_gen = c->gen[pad];
  float fstep = 1.0f / (float)(c->sr * EP_FADE_MS * 0.001 + 1.0);
  for (int i = 0; i < EP_VOICES; i++) {
    if (c->voices[i].active && c->voices[i].pad == pad) {
      if (c->voices[i].fade_step == 0.0f) {
        c->voices[i].fade = 1.0f;
        c->voices[i].fade_step = fstep;
      }
    }
  }
  c->gen[pad]++;
  if (src && src->data && src->frames > 0) {
    c->src[pad] = *src;
  } else {
    memset(&c->src[pad], 0, sizeof c->src[pad]);
  }
  return old_gen;
}

int ep_core_gen_in_use(const EpCore* c, int pad, uint32_t gen) {
  for (int i = 0; i < EP_VOICES; i++) {
    if (c->voices[i].active && c->voices[i].pad == pad && c->voices[i].gen == gen)
      return 1;
  }
  return 0;
}

void ep_core_all_off(EpCore* c) {
  for (int i = 0; i < EP_VOICES; i++) c->voices[i].active = 0;
}

static int any_solo(const EpCore* c) {
  for (int i = 0; i < EP_PADS; i++) {
    if (c->pad[i].solo) return 1;
  }
  return 0;
}

static int find_voice_slot(EpCore* c) {
  for (int i = 0; i < EP_VOICES; i++) {
    if (!c->voices[i].active) return i;
  }
  int best = 0;
  uint64_t oldest = UINT64_MAX;
  int found_fading = 0;
  for (int i = 0; i < EP_VOICES; i++) {
    int fading = c->voices[i].fade_step > 0.0f;
    if (fading && !found_fading) {
      found_fading = 1;
      best = i;
      oldest = c->voices[i].age;
    } else if (fading == found_fading && c->voices[i].age < oldest) {
      best = i;
      oldest = c->voices[i].age;
    }
  }
  return best;
}

void ep_core_trigger(EpCore* c, int pad, int vel) {
  if (pad < 0 || pad >= EP_PADS) return;
  const EpSource* s = &c->src[pad];
  const EpPadParams* p = &c->pad[pad];
  if (!s->data || s->frames <= 0) return;

  float fstep = 1.0f / (float)(c->sr * EP_FADE_MS * 0.001 + 1.0);
  for (int i = 0; i < EP_VOICES; i++) {
    EpVoice* v = &c->voices[i];
    if (!v->active) continue;
    int same_pad = (v->pad == pad);
    int same_choke = (p->choke > 0 && c->pad[v->pad].choke == p->choke);
    if (same_pad || same_choke) {
      if (v->fade_step == 0.0f) {
        v->fade = 1.0f;
        v->fade_step = fstep;
      }
    }
  }

  int slot = find_voice_slot(c);
  EpVoice* v = &c->voices[slot];
  memset(v, 0, sizeof *v);

  float st_frac = clampf(p->start * 0.01f, 0.0f, 0.95f);
  float en_frac = clampf(p->end * 0.01f, st_frac + 0.01f, 1.0f);
  if (en_frac > 1.0f) en_frac = 1.0f;

  v->active = 1;
  v->pad = pad;
  v->gen = c->gen[pad];
  v->start_frame = (double)st_frac * (double)(s->frames - 1);
  v->end_frame = (double)en_frac * (double)(s->frames - 1);
  if (v->end_frame <= v->start_frame) v->end_frame = (double)(s->frames - 1);

  v->reverse = (p->mode == EP_MODE_REV_ONESHOT || p->mode == EP_MODE_REV_GATE);
  v->gate_mode = (p->mode == EP_MODE_GATE || p->mode == EP_MODE_REV_GATE);
  v->pos = v->reverse ? v->end_frame : v->start_frame;

  float vel_norm = clampf((float)vel / 127.0f, 0.0f, 1.0f);
  float vs = clampf(p->vel_sens * 0.01f, 0.0f, 1.0f);
  v->vel_gain = (1.0f - vs) + vs * (vel_norm * vel_norm);

  v->env = 1.0f;
  v->full_sample = (p->decay >= 99.5f);
  if (!v->full_sample) {
    double dec_sec = 0.010 * pow(1000.0, (double)clampf(p->decay, 0.0f, 100.0f) / 100.0);
    v->decay_coef = (float)exp(-6.90775527898 / (dec_sec * c->sr));
  } else {
    v->decay_coef = 1.0f;
  }

  v->fade = 1.0f;
  v->fade_step = 0.0f;
  v->punch_env = 1.0f;
  v->punch_coef = (float)exp(-1.0 / (0.025 * c->sr));
  v->age = ++c->voice_seq;

  float act = 0.35f + 0.65f * vel_norm;
  if (act > c->activity[pad]) c->activity[pad] = act;
}

void ep_core_release(EpCore* c, int pad) {
  if (pad < 0 || pad >= EP_PADS) return;
  float fstep = 1.0f / (float)(c->sr * 0.012 + 1.0);
  for (int i = 0; i < EP_VOICES; i++) {
    EpVoice* v = &c->voices[i];
    if (v->active && v->pad == pad && v->gate_mode && !v->released) {
      v->released = 1;
      if (v->fade_step == 0.0f) {
        v->fade = 1.0f;
        v->fade_step = fstep;
      }
    }
  }
}

/* ---------- Step Sequencer ---------- */

static inline void get_pad_range(const EpScene* sc, int p, int* out_s, int* out_e) {
  int s = (int)sc->pad_start[p];
  int e = (int)sc->pad_end[p];
  if (e == 0) e = EP_STEPS_DEF;
  if (s < 0) s = 0;
  if (s >= EP_STEPS) s = EP_STEPS - 1;
  if (e <= s) e = s + 1;
  if (e > EP_STEPS) e = EP_STEPS;
  *out_s = s;
  *out_e = e;
}

void ep_seq_set_mode(EpCore* c, int mode) {
  if (mode < EP_SEQ_STOP) mode = EP_SEQ_STOP;
  if (mode > EP_SEQ_REC) mode = EP_SEQ_REC;
  if (mode == c->seq.mode) return;
  int was_stopped = (c->seq.mode == EP_SEQ_STOP);
  c->seq.mode = mode;
  if (mode == EP_SEQ_STOP) {
    c->seq.cur_step = -1;
    c->seq.last_step_fired = -1;
    c->seq.skip_next_mask = 0;
    for (int p = 0; p < EP_PADS; p++) {
      c->seq.pad_last_step[p] = -1;
      c->seq.pad_cur_step[p] = -1;
      c->seq.skip_step[p] = -1;
    }
    memset(c->seq.roll_rem, 0, sizeof c->seq.roll_rem);
    if (c->seq.pend_scene >= 0) {
      c->seq.scene = c->seq.pend_scene;
      c->seq.pend_scene = -1;
    }
  } else if (was_stopped) {
    if (!c->seq.host_playing) {
      c->seq.bar_phase = 0.0;
    }
    c->seq.last_step_fired = -1;
    const EpScene* sc = &c->scenes[c->seq.scene];
    for (int p = 0; p < EP_PADS; p++) {
      int ps, pe;
      get_pad_range(sc, p, &ps, &pe);
      double plen = (double)(pe - ps);
      c->seq.pad_phase[p] = c->seq.host_playing ? fmod(c->seq.bar_phase, plen) : 0.0;
      c->seq.pad_last_step[p] = -1;
      c->seq.pad_cur_step[p] = -1;
      c->seq.skip_step[p] = -1;
    }
  }
}

void ep_seq_set_len(EpCore* c, int steps) {
  if (steps < EP_STEPS_MIN) steps = EP_STEPS_MIN;
  if (steps > EP_STEPS) steps = EP_STEPS;
  c->seq.num_steps = steps;
  for (int p = 0; p < EP_PADS; p++) {
    ep_seq_set_pad_range(c, c->seq.scene, p, 0, steps);
  }
}

void ep_seq_set_pad_range(EpCore* c, int scene, int pad, int start_step, int end_step) {
  if (scene < 0 || scene >= EP_SCENES || pad < 0 || pad >= EP_PADS) return;
  if (start_step < 0) start_step = 0;
  if (start_step >= EP_STEPS) start_step = EP_STEPS - 1;
  if (end_step <= start_step) end_step = start_step + 1;
  if (end_step > EP_STEPS) end_step = EP_STEPS;
  c->scenes[scene].pad_start[pad] = (uint8_t)start_step;
  c->scenes[scene].pad_end[pad] = (uint8_t)end_step;
  if (scene == c->seq.scene) {
    int plen = end_step - start_step;
    double len = (double)plen;
    if (c->seq.pad_phase[pad] >= len) {
      c->seq.pad_phase[pad] = fmod(c->seq.pad_phase[pad], len);
    }
    if (c->seq.pad_last_step[pad] >= plen) {
      c->seq.pad_last_step[pad] = plen - 1;
    }
  }
}

void ep_seq_set_quantize(EpCore* c, int q) {
  if (q < 0) q = 0;
  if (q > 3) q = 3;
  c->seq.quantize = q;
}

void ep_seq_request_scene(EpCore* c, int scene) {
  if (scene < 0 || scene >= EP_SCENES) return;
  if (c->seq.mode == EP_SEQ_STOP) {
    c->seq.scene = scene;
    c->seq.pend_scene = -1;
  } else if (scene == c->seq.scene) {
    c->seq.pend_scene = -1;
  } else {
    c->seq.pend_scene = scene;
  }
}

void ep_seq_sync_host(EpCore* c, double bpm, int playing, double bar_beat, int bpb) {
  if (bpm >= 40.0 && bpm <= 240.0) c->seq.bpm = bpm;
  int was_host = c->seq.host_playing;
  c->seq.host_playing = playing ? 1 : 0;
  if (was_host && !c->seq.host_playing && c->seq.mode != EP_SEQ_STOP) {
    ep_seq_set_mode(c, EP_SEQ_STOP);
    return;
  }
  if (!was_host && c->seq.host_playing && c->seq.mode == EP_SEQ_STOP) {
    ep_seq_set_mode(c, EP_SEQ_PLAY);
  }
  int nsteps = c->seq.num_steps;
  if (nsteps < EP_STEPS_MIN || nsteps > EP_STEPS) nsteps = EP_STEPS_DEF;
  double len = (double)nsteps;
  if (playing && bpb > 0 && bar_beat >= 0.0) {
    /* Map host barBeat (0..bpb) onto 0..len 16th-note phase */
    double ph = fmod(bar_beat * 4.0, len);
    if (ph < 0.0) ph += len;
    /* Only hard-sync if drift > 0.15 steps so internal sample-accurate stepping stays jitter-free */
    double diff = fabs(ph - c->seq.bar_phase);
    if (diff > len * 0.5) diff = len - diff;
    if (diff > 0.15) {
      c->seq.bar_phase = ph;
    }
  }
}

void ep_seq_record_hit(EpCore* c, int pad, int vel) {
  if (c->seq.mode != EP_SEQ_REC || pad < 0 || pad >= EP_PADS || vel <= 0) return;
  EpScene* sc = &c->scenes[c->seq.scene];
  int ps, pe;
  get_pad_range(sc, pad, &ps, &pe);
  int plen = pe - ps;
  double ph = c->seq.pad_phase[pad];
  int rel = 0;
  if (c->seq.quantize == 0) {
    /* Quantize Off: stamp immediately onto current step without snapping forward */
    rel = (int)floor(ph) % plen;
  } else {
    int grid = (c->seq.quantize == 2) ? 2 : ((c->seq.quantize == 3) ? 4 : 1);
    rel = ((int)llround(ph / (double)grid) * grid) % plen;
  }
  if (rel < 0) rel += plen;
  int abs_step = ps + rel;
  sc->steps[pad][abs_step].vel = (uint8_t)(vel > 127 ? 127 : vel);
  /* If quantized forward to the next upcoming step, suppress its immediate auto-trigger once */
  int cur_rel = (int)floor(ph) % plen;
  if (rel != cur_rel) {
    c->seq.skip_step[pad] = abs_step;
    c->seq.skip_next_mask |= (uint16_t)(1u << pad);
  }
}

void ep_seq_clear_pad(EpCore* c, int scene, int pad) {
  if (scene < 0 || scene >= EP_SCENES || pad < 0 || pad >= EP_PADS) return;
  memset(c->scenes[scene].steps[pad], 0, sizeof c->scenes[scene].steps[pad]);
}

void ep_seq_clear_scene(EpCore* c, int scene) {
  if (scene < 0 || scene >= EP_SCENES) return;
  memset(&c->scenes[scene], 0, sizeof c->scenes[scene]);
  init_scene_ranges(&c->scenes[scene]);
}

void ep_seq_copy_scene(EpCore* c, int src_scene, int dst_scene) {
  if (src_scene < 0 || src_scene >= EP_SCENES || dst_scene < 0 || dst_scene >= EP_SCENES) return;
  if (src_scene == dst_scene) return;
  c->scenes[dst_scene] = c->scenes[src_scene];
}

/* Compute the swing-adjusted phase (0..16) at which step `s` fires */
static double step_trigger_phase(int s, float swing) {
  if ((s & 1) == 0) return (double)s;
  /* swing 50..75% -> odd step shifts from s to s + 0.5 */
  double shift = ((double)clampf(swing, 50.0f, 75.0f) - 50.0) / 50.0;
  return (double)s + shift;
}

static void fire_pad_step(EpCore* c, int p, int abs_step, double samples_per_step) {
  c->seq.pad_cur_step[p] = abs_step;
  const EpScene* sc = &c->scenes[c->seq.scene];
  int skip = (c->seq.skip_step[p] == abs_step && (c->seq.skip_next_mask & (1u << p)));
  if (c->seq.skip_step[p] == abs_step) {
    c->seq.skip_step[p] = -1;
    c->seq.skip_next_mask &= (uint16_t)~(1u << p);
  }
  EpStep st = sc->steps[p][abs_step];
  if (st.vel > 0) {
    if (!skip) {
      ep_core_trigger(c, p, st.vel);
    }
    if (st.roll > 0 && st.roll <= 3) {
      int sub = st.roll + 1; /* 2x, 3x, 4x */
      c->seq.roll_rem[p] = (uint8_t)(sub - 1);
      c->seq.roll_vel[p] = st.vel;
      c->seq.roll_interval[p] = samples_per_step / (double)sub;
      c->seq.roll_timer[p] = c->seq.roll_interval[p];
    } else {
      c->seq.roll_rem[p] = 0;
    }
  }
}

/* Band-limited windowed-sinc resampler (8-tap for <=1x, adaptive anti-aliased up to 40-tap for >1x). */
static inline void sample_read_hq(const EpSource* s, double pos, double step, float* out_l, float* out_r) {
  int64_t base = (int64_t)floor(pos);
  double frac = pos - (double)base;
  int64_t max_i = s->frames - 1;

  if (step <= 1.0001 && step >= 0.9999 && frac < 1e-6) {
    int64_t i = base < 0 ? 0 : (base > max_i ? max_i : base);
    *out_l = s->data[2 * i];
    *out_r = s->data[2 * i + 1];
    return;
  }

  double cutoff = (step > 1.02) ? (0.92 / step) : 1.0;
  int half = (step > 1.02) ? (int)ceil(4.0 * step) : 4;
  if (half < 4) half = 4;
  if (half > 20) half = 20;

  double inv_half = 1.0 / (double)half;
  double start_x = (double)(-half + 1) - frac;

  /* Trigonometric recurrences avoid calling sin/cos inside the tap loop */
  double d_sinc = M_PI * cutoff;
  double s_cos_d = cos(d_sinc), s_sin_d = sin(d_sinc);
  double u0 = M_PI * start_x * cutoff;
  double s_sin = sin(u0), s_cos = cos(u0);

  double d_win = M_PI * inv_half;
  double w_cos_d = cos(d_win), w_sin_d = sin(d_win);
  double v0 = M_PI * start_x * inv_half;
  double w_sin = sin(v0), w_cos = cos(v0);

  double sum_l = 0.0, sum_r = 0.0, sum_w = 0.0;
  int count = 2 * half;

  for (int t = 0; t < count; t++) {
    int k = -half + 1 + t;
    double x = (double)k - frac;
    double u = M_PI * x * cutoff;
    double sinc = (fabs(u) < 1e-6) ? 1.0 : (s_sin / u);
    double win = 0.5 * (1.0 + w_cos);
    double w = sinc * win;

    int64_t idx = base + k;
    if (idx < 0) idx = 0;
    else if (idx > max_i) idx = max_i;

    sum_l += w * (double)s->data[2 * idx];
    sum_r += w * (double)s->data[2 * idx + 1];
    sum_w += w;

    /* Advance sin/cos recurrences */
    double ns = s_sin * s_cos_d + s_cos * s_sin_d;
    double nc = s_cos * s_cos_d - s_sin * s_sin_d;
    s_sin = ns; s_cos = nc;

    double nws = w_sin * w_cos_d + w_cos * w_sin_d;
    double nwc = w_cos * w_cos_d - w_sin * w_sin_d;
    w_sin = nws; w_cos = nwc;
  }

  double inv_w = (fabs(sum_w) > 1e-9) ? (1.0 / sum_w) : 1.0;
  *out_l = (float)(sum_l * inv_w);
  *out_r = (float)(sum_r * inv_w);
}

/* Render a contiguous voice block without sequencer events inside it */
static void render_voices(EpCore* c, float* out_l, float* out_r,
                          float* send_l, float* send_r, uint32_t nframes, int sel_pad) {
  memset(out_l, 0, nframes * sizeof(float));
  memset(out_r, 0, nframes * sizeof(float));
  memset(send_l, 0, nframes * sizeof(float));
  memset(send_r, 0, nframes * sizeof(float));

  const int solo_active = any_solo(c);
  const float act_decay = (float)exp(-(double)nframes / (0.14 * c->sr));
  for (int i = 0; i < EP_PADS; i++) {
    c->activity[i] *= act_decay;
    if (c->activity[i] < 0.002f) c->activity[i] = 0.0f;
  }

  float sel_ph = -1.0f;
  uint64_t sel_newest = 0;

  for (int vi = 0; vi < EP_VOICES; vi++) {
    EpVoice* v = &c->voices[vi];
    if (!v->active) continue;

    const int pad = v->pad;
    const EpSource* s = &c->src[pad];
    const EpPadParams* p = &c->pad[pad];
    if (!s->data || s->frames <= 0 || v->gen != c->gen[pad]) {
      v->active = 0;
      continue;
    }

    int audible = solo_active ? p->solo : !p->mute;
    float pad_gain = audible ? (clampf(p->level * 0.01f, 0.0f, 1.5f) * v->vel_gain) : 0.0f;
    float send_amt = audible ? clampf(p->send * 0.01f, 0.0f, 1.0f) : 0.0f;

    float pan_norm = clampf(p->pan * 0.01f, -1.0f, 1.0f);
    float angle = (pan_norm + 1.0f) * (float)(0.25 * M_PI);
    float gain_l = pad_gain * cosf(angle) * 1.41421356f;
    float gain_r = pad_gain * sinf(angle) * 1.41421356f;

    double base_step = ((double)s->rate / c->sr) * pow(2.0, (double)clampf(p->pitch, -24.0f, 24.0f) / 12.0);
    float punch_st = clampf(p->punch, -100.0f, 100.0f) * 0.24f;

    float filt = clampf(p->filter, -100.0f, 100.0f);
    int filt_mode = 0;
    float g = 0.0f, k = 1.15f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;
    if (filt < -1.0f) {
      filt_mode = -1;
      float t = (100.0f + filt) / 100.0f;
      float fc = 80.0f * powf(250.0f, t);
      if (fc > 0.45f * (float)c->sr) fc = 0.45f * (float)c->sr;
      g = tanf((float)M_PI * fc / (float)c->sr);
      a1 = 1.0f / (1.0f + g * (g + k));
      a2 = g * a1;
      a3 = g * a2;
    } else if (filt > 1.0f) {
      filt_mode = 1;
      float t = filt / 100.0f;
      float fc = 20.0f * powf(400.0f, t);
      if (fc > 0.45f * (float)c->sr) fc = 0.45f * (float)c->sr;
      g = tanf((float)M_PI * fc / (float)c->sr);
      a1 = 1.0f / (1.0f + g * (g + k));
      a2 = g * a1;
      a3 = g * a2;
    }

    float drive = clampf(p->drive, 0.0f, 100.0f);
    float sat_gain = 1.0f + (drive < 50.0f ? drive : 50.0f) * 0.10f;
    float crush_amt = drive > 50.0f ? (drive - 50.0f) * 0.02f : 0.0f;
    float hold_inc = crush_amt > 0.0f ? 1.0f / (1.0f + crush_amt * 15.0f) : 1.0f;
    float q_steps = crush_amt > 0.0f ? (64.0f - crush_amt * 56.0f) : 0.0f;

    float peak_voice = 0.0f;

    for (uint32_t i = 0; i < nframes; i++) {
      if (v->reverse) {
        if (v->pos < v->start_frame) { v->active = 0; break; }
      } else {
        if (v->pos >= v->end_frame) { v->active = 0; break; }
      }

      double step = base_step;
      if (punch_st != 0.0f && v->punch_env > 0.001f) {
        step *= pow(2.0, (double)(punch_st * v->punch_env) / 12.0);
        v->punch_env *= v->punch_coef;
      }

      float l = 0.0f, r = 0.0f;
      sample_read_hq(s, v->pos, step, &l, &r);

      if (drive > 0.5f) {
        l = fast_tanh(l * sat_gain);
        r = fast_tanh(r * sat_gain);
        if (crush_amt > 0.0f) {
          v->hold_phase += hold_inc;
          if (v->hold_phase >= 1.0f) {
            v->hold_phase -= 1.0f;
            v->hold_l = floorf(l * q_steps + 0.5f) / q_steps;
            v->hold_r = floorf(r * q_steps + 0.5f) / q_steps;
          }
          l = v->hold_l;
          r = v->hold_r;
        }
      }

      if (filt_mode != 0) {
        float v3l = l - v->ic2_l;
        float v1l = a1 * v->ic1_l + a2 * v3l;
        float v2l = v->ic2_l + a2 * v->ic1_l + a3 * v3l;
        v->ic1_l = 2.0f * v1l - v->ic1_l;
        v->ic2_l = 2.0f * v2l - v->ic2_l;
        l = (filt_mode < 0) ? v2l : (l - k * v1l - v2l);

        float v3r = r - v->ic2_r;
        float v1r = a1 * v->ic1_r + a2 * v3r;
        float v2r = v->ic2_r + a2 * v->ic1_r + a3 * v3r;
        v->ic1_r = 2.0f * v1r - v->ic1_r;
        v->ic2_r = 2.0f * v2r - v->ic2_r;
        r = (filt_mode < 0) ? v2r : (r - k * v1r - v2r);
      }

      float amp = v->env * v->fade;
      float ol = l * amp * gain_l;
      float or_ = r * amp * gain_r;
      out_l[i] += ol;
      out_r[i] += or_;
      if (send_amt > 0.005f) {
        send_l[i] += ol * send_amt;
        send_r[i] += or_ * send_amt;
      }

      float m = fabsf(ol) > fabsf(or_) ? fabsf(ol) : fabsf(or_);
      if (m > peak_voice) peak_voice = m;

      v->pos += v->reverse ? -step : step;

      if (!v->full_sample) {
        v->env *= v->decay_coef;
        if (v->env < 0.0005f) { v->active = 0; break; }
      }
      if (v->fade_step > 0.0f) {
        v->fade -= v->fade_step;
        if (v->fade <= 0.0f) { v->active = 0; break; }
      }
    }

    if (peak_voice > c->activity[pad]) {
      c->activity[pad] = peak_voice > 1.0f ? 1.0f : peak_voice;
    }

    if (v->active && pad == sel_pad && v->age > sel_newest && s->frames > 1) {
      sel_newest = v->age;
      sel_ph = clampf((float)(v->pos / (double)(s->frames - 1)), 0.0f, 1.0f);
    }
  }

  c->sel_playhead = sel_ph;
  reverb_process(&c->rev, c->sr, send_l, send_r, out_l, out_r, nframes);

  float mg = clampf(c->master_vol * 0.01f, 0.0f, 1.5f);
  float sw = clampf(c->stereo_width * 0.01f, 0.0f, 2.0f);
  for (uint32_t i = 0; i < nframes; i++) {
    float l = out_l[i] * mg;
    float r = out_r[i] * mg;
    if (fabsf(sw - 1.0f) > 0.001f) {
      float mid  = 0.5f * (l + r);
      float side = 0.5f * (l - r) * sw;
      l = mid + side;
      r = mid - side;
    }
    if (l > 0.75f) l = 0.75f + 0.25f * fast_tanh((l - 0.75f) * 4.0f);
    else if (l < -0.75f) l = -0.75f + 0.25f * fast_tanh((l + 0.75f) * 4.0f);
    if (r > 0.75f) r = 0.75f + 0.25f * fast_tanh((r - 0.75f) * 4.0f);
    else if (r < -0.75f) r = -0.75f + 0.25f * fast_tanh((r + 0.75f) * 4.0f);
    out_l[i] = l;
    out_r[i] = r;
  }
}

void ep_core_process(EpCore* c, float* out_l, float* out_r, uint32_t nframes, int sel_pad) {
  float send_l[256], send_r[256];
  uint32_t done = 0;

  while (done < nframes) {
    uint32_t chunk = nframes - done;
    if (chunk > 256) chunk = 256;

    if (c->seq.mode != EP_SEQ_STOP) {
      double bpm = c->seq.bpm < 40.0 ? 40.0 : (c->seq.bpm > 240.0 ? 240.0 : c->seq.bpm);
      /* 4 16th-notes per quarter note */
      double steps_per_sample = (bpm / 60.0) * 4.0 / c->sr;
      double samples_per_step = 1.0 / steps_per_sample;

      /* Process sub-chunk sample-accurately up to the next per-pad step or roll event */
      uint32_t sub_pos = 0;
      while (sub_pos < chunk) {
        const EpScene* sc = &c->scenes[c->seq.scene];

        /* Fire initial step immediately for any pad starting from idle */
        for (int p = 0; p < EP_PADS; p++) {
          if (c->seq.pad_last_step[p] < 0 && c->seq.pad_phase[p] < 0.05) {
            int ps, pe;
            get_pad_range(sc, p, &ps, &pe);
            c->seq.pad_last_step[p] = 0;
            fire_pad_step(c, p, ps, samples_per_step);
          }
        }

        uint32_t rem = chunk - sub_pos;
        uint32_t adv = rem;
        int ev_kind = 0; /* 0 = none, 1 = pad step, 2 = roll */
        int ev_pad = -1;
        int ev_rel = 0;
        int ev_abs = 0;

        /* Find earliest upcoming step across all 16 polyrhythmic pad lanes */
        for (int p = 0; p < EP_PADS; p++) {
          int ps, pe;
          get_pad_range(sc, p, &ps, &pe);
          int plen = pe - ps;
          double len = (double)plen;
          int next_rel = (c->seq.pad_last_step[p] + 1) % plen;
          double target_ph = step_trigger_phase(next_rel, c->seq.swing);
          if (target_ph >= len) target_ph = fmod(target_ph, len);
          double dph = target_ph - c->seq.pad_phase[p];
          if (dph < -len * 0.5) dph += len;
          else if (dph < 0.0) dph = 0.0;
          uint32_t to_st = (uint32_t)ceil(dph * samples_per_step);
          if (to_st <= adv) {
            adv = to_st;
            ev_kind = 1;
            ev_pad = p;
            ev_rel = next_rel;
            ev_abs = ps + next_rel;
          }
        }

        /* Check ratchet timers */
        for (int p = 0; p < EP_PADS; p++) {
          if (c->seq.roll_rem[p] > 0) {
            uint32_t tr = c->seq.roll_timer[p] <= 0.0 ? 0 : (uint32_t)ceil(c->seq.roll_timer[p]);
            if (tr <= adv) {
              adv = tr;
              ev_kind = 2;
              ev_pad = p;
            }
          }
        }

        if (adv > 0) {
          render_voices(c, out_l + done + sub_pos, out_r + done + sub_pos,
                        send_l, send_r, adv, sel_pad);
          double dstep = (double)adv * steps_per_sample;
          double prev_bar = c->seq.bar_phase;
          c->seq.bar_phase = fmod(prev_bar + dstep, 16.0);
          /* Quantized scene switch at 16-step bar boundary */
          if (c->seq.pend_scene >= 0 && c->seq.bar_phase < prev_bar) {
            c->seq.scene = c->seq.pend_scene;
            c->seq.pend_scene = -1;
            for (int p = 0; p < EP_PADS; p++) {
              c->seq.pad_phase[p] = c->seq.bar_phase;
              c->seq.pad_last_step[p] = -1;
            }
            sc = &c->scenes[c->seq.scene];
          } else {
            for (int p = 0; p < EP_PADS; p++) {
              int ps, pe;
              get_pad_range(sc, p, &ps, &pe);
              c->seq.pad_phase[p] = fmod(c->seq.pad_phase[p] + dstep, (double)(pe - ps));
            }
          }
          for (int p = 0; p < EP_PADS; p++) {
            if (c->seq.roll_rem[p] > 0) c->seq.roll_timer[p] -= (double)adv;
          }
          sub_pos += adv;
        }

        if (ev_kind == 1 && ev_pad >= 0) {
          c->seq.pad_last_step[ev_pad] = ev_rel;
          fire_pad_step(c, ev_pad, ev_abs, samples_per_step);
        } else if (ev_kind == 2 && ev_pad >= 0) {
          c->seq.roll_rem[ev_pad]--;
          c->seq.roll_timer[ev_pad] += c->seq.roll_interval[ev_pad];
          ep_core_trigger(c, ev_pad, c->seq.roll_vel[ev_pad]);
        }
      }
      c->seq.cur_step = (sel_pad >= 0 && sel_pad < EP_PADS) ? c->seq.pad_cur_step[sel_pad] : -1;
    } else {
      render_voices(c, out_l + done, out_r + done, send_l, send_r, chunk, sel_pad);
    }

    done += chunk;
  }
}
