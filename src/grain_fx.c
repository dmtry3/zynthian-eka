/* Eka Grain Flow core, see grain_fx.h.
 *
 * Positions are counted in frames written since the start (x->w), so they never wrap; the ring index is
 * pos & (frames - 1). Each chunk the input is written first, then grains are born and read.
 *
 * Where a grain may read:
 * - forward, recording: it must stay behind the write head while it plays, so it starts at least
 *   len * (inc - 1) frames back (pitch up reads faster than the input comes in), plus a margin
 *   for the interpolation;
 * - forward, frozen: the write head stands still, so the whole grain must fit behind it: len * inc;
 * - backwards: it reads older and older frames; the ring is long enough for the longest Offset plus
 *   the longest grain at +24 semitones, while the write head keeps going.
 * When Freeze comes on, recording continues for 20 ms while forward grains that would cross the stopped
 * head fade out. This also gives grains at zero Offset time to fade instead of cutting them in a few samples.
 * New grains already use frozen placement. When recording resumes, GX_FADE frames are cross-faded from the
 * last frame before the freeze, so grains reading across that seam do not click.
 * While frozen, each grain starts up to one Length further back than Offset, at random (see spawn()).
 * With LFOs on Pitch the read speed changes while a grain sounds: the room left to the head uses the highest
 * speed the LFOs can reach (pmax), so a grain never catches the head whatever the LFO does. */
#include "grain_fx.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define GUARD 3               /* frames a read stays behind the write head: the cubic reads 2 frames ahead */
#define MARGIN (GUARD + 2)    /* room given at a grain's start, a little more than GUARD (see stop_at_head) */
#define TUKEY 0.2f            /* sharp window: share of the grain taken by the two slopes */
#define LP_MIN 200.0f         /* Filter -1: low-pass at this frequency */
#define HP_MAX 5000.0f        /* Filter +1: high-pass at this frequency */
#define PEAK_FALL_S 0.3f      /* input peak meter: fall time */
#define FREEZE_FADE_S 0.02   /* finish endangered live grains before stopping the write head */

const char* const gx_interval_names[GX_NINTERVALS] = { "Off", "Oct", "Oct +-", "Fifth", "Major", "Minor", "Sus",
                                                       "Penta", "Wide", "Q Oct", "Q Fifth", "Q Major", "Q Minor", "Q Penta" };
static const signed char intervals[GX_NINTERVALS][8] = {   /* count, then semitones */
  { 1, 0 }, { 2, 0, 12 }, { 3, -12, 0, 12 }, { 3, 0, 7, 12 }, { 4, 0, 4, 7, 12 }, { 4, 0, 3, 7, 12 },
  { 4, 0, 5, 7, 12 }, { 6, 0, 2, 4, 7, 9, 12 }, { 6, -12, 0, 7, 12, 19, 24 },
  { 1, 0 }, { 1, 0 }, { 1, 0 }, { 1, 0 }, { 1, 0 } };

/* note values in beats: straight, triplet and dotted from 1/64 to 4 bars */
const float gx_div_beats[GX_NDIVS] = { 1 / 16.f, 1 / 12.f, 3 / 32.f, 1 / 8.f, 1 / 6.f, 3 / 16.f, 1 / 4.f, 1 / 3.f,
                                       3 / 8.f, 1 / 2.f, 2 / 3.f, 3 / 4.f, 1, 4 / 3.f, 3 / 2.f, 2, 3, 4, 8, 16 };
const char* const gx_div_names[GX_NDIVS] = { "1/64", "1/32T", "1/64.", "1/32", "1/16T", "1/32.", "1/16", "1/8T",
                                             "1/16.", "1/8", "1/4T", "1/8.", "1/4", "1/2T", "1/4.", "1/2", "1/2.",
                                             "1 bar", "2 bars", "4 bars" };

int gx_snap_div(float ms, float bpm) {
  double beats = ms * bpm / 60000.0, best = 1e9;
  int k = 0;
  if (beats <= 0) return 0;
  for (int i = 0; i < GX_NDIVS; i++) {
    double d = fabs(log(beats / gx_div_beats[i]));
    if (d < best) { best = d; k = i; }
  }
  return k;
}

static uint32_t rnd(GrainFx* x) {
  uint32_t r = x->rng;
  r ^= r << 13; r ^= r >> 17; r ^= r << 5;
  return x->rng = r;
}
static float rnd01(GrainFx* x) { return (float)(rnd(x) >> 8) * (1.0f / 16777216.0f); }

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

float gx_db(float db) { return db <= -59.9f ? 0.0f : powf(10.0f, db / 20.0f); }

void gx_default_params(GxParams* p) {
  memset(p, 0, sizeof *p);
  for (int s = 0; s < GX_STREAMS; s++) {
    GxStream* t = &p->s[s];
    t->on = s == 0;
    t->pitch = (1 - s) * 12.0f;
    t->offset_ms = 100.0f;
    t->rate_ms = 100.0f;
    t->length_ms = 500.0f;
    t->spread = 0.0f;
    t->gain = 1.0f;
  }
  p->mix = 0.5f;
  p->gain = 1.0f;
  p->buffer_s = 4.0f;
  p->env_release_ms = 300.0f;
  for (int l = 0; l < GX_LFOS; l++) p->lfo_div[l] = 12;
  p->lfo[0].rate = 0.3f; p->lfo[0].shape = GX_LFO_SINE;
  p->lfo[1].rate = 1.0f; p->lfo[1].shape = GX_LFO_DRIFT;
}

int gx_init(GrainFx* x, double sr) {
  memset(x, 0, sizeof *x);
  x->sr = sr;
  /* longest Offset + longest grain read at +24 semitones + the write head moving on during that grain */
  /* A grain starts at most 2 buffers back (Offset, plus Spray or the spread while frozen), reads at most
   * GX_MAX_READ_S (backwards: further back) and lasts at most GX_MAX_LENGTH_MS, while the head moves on. */
  double need = sr * (2.0 * GX_MAX_BUFFER_S + GX_MAX_READ_S + GX_MAX_LENGTH_MS / 1000.0)
                + 4 * GX_CHUNK + GX_FADE;
  uint64_t f = 1;
  while ((double)f < need) f <<= 1;
  x->frames = f;
  x->buf = (float*)calloc((size_t)f * 2, sizeof(float));
  if (!x->buf) return -1;
  for (int i = 0; i <= GX_WIN + 1; i++) {
    float u = (float)i / GX_WIN;
    if (u > 1.0f) u = 1.0f;
    x->win[0][i] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * u);
    float e = u < 0.5f ? u : 1.0f - u;              /* distance from the nearer end */
    x->win[1][i] = e >= TUKEY / 2 ? 1.0f : 0.5f - 0.5f * cosf((float)M_PI * e / (TUKEY / 2));
  }
  gx_reset(x);
  return 0;
}

void gx_free(GrainFx* x) {
  free(x->buf);
  x->buf = NULL;
}

void gx_reset(GrainFx* x) {
  if (x->buf) memset(x->buf, 0, (size_t)x->frames * 2 * sizeof(float));
  x->w = x->frames;            /* start one ring in, so positions behind the head are never negative */
  x->frozen = 0;
  x->freeze_loop = 0;
  x->hold_period = x->hold_phase = 0;
  x->hold_mix = x->record_peak = 0;
  x->freeze_tail = 0;
  x->xfade = 0;
  x->last_l = x->last_r = 0.0f;
  memset(x->g, 0, sizeof x->g);
  memset(x->svf, 0, sizeof x->svf);
  for (int s = 0; s < GX_STREAMS; s++) { x->next[s] = 0.0; x->fgain[s] = -1.0f; }
  memset(x->scan_pos, 0, sizeof x->scan_pos);
  memset(x->scan_offset, 0, sizeof x->scan_offset);
  memset(x->scan_active, 0, sizeof x->scan_active);
  x->rng = 0x2545F491u;
  x->dry = x->wet = -1.0f;
  x->in_peak = 0.0f;
  for (int l = 0; l < GX_LFOS; l++) { x->lfo_ph[l] = 0.0; x->lfo_v[l] = x->lfo_slew[l] = 0.0f; x->lfo_a[l] = x->lfo_b[l] = 0.0f; }
  for (int s = 0; s < GX_STREAMS; s++) { x->pm[s] = x->pmax[s] = x->om[s] = x->lm[s] = 0.0f; x->am[s] = 1.0f; }
  memset(x->fm, 0, sizeof x->fm);
  memset(x->sm, 0, sizeof x->sm);
  memset(x->scan_mod_on, 0, sizeof x->scan_mod_on);
  x->env = x->env_v = 0.0f;
  x->fbn = 0;
  x->dc_x[0] = x->dc_x[1] = x->dc_y[0] = x->dc_y[1] = 0.0f;
  x->bpm = 120.0f;
  x->beat = 0.0;
  for (int s = 0; s < GX_STREAMS; s++) x->last_k[s] = INT64_MIN;
}

void gx_set_tempo(GrainFx* x, float bpm, double beat) {
  if (bpm >= 20.0f && bpm <= 400.0f) x->bpm = bpm;
  if (beat >= 0.0) x->beat = beat;
}

int gx_active(const GrainFx* x) {
  int n = 0;
  for (int i = 0; i < GX_POOL; i++) n += x->g[i].active;
  return n;
}

int gx_grain_info(const GrainFx* x, int k, int* stream, float* seconds_back) {
  for (int i = 0; i < GX_POOL; i++) {
    if (!x->g[i].active) continue;
    if (k-- == 0) {
      *stream = x->g[i].stream;
      *seconds_back = (float)(((double)x->w - x->g[i].pos) / x->sr);
      return 1;
    }
  }
  return 0;
}

static float semis(const GxStream* s) {          /* Pitch + Fine, without the LFOs */
  return clampf(s->pitch, -GX_MAX_PITCH, GX_MAX_PITCH) + clampf(s->fine, -50.0f, 50.0f) * 0.01f;
}
static double ratio(float st) { return pow(2.0, st / 12.0); }
static int quantized(const GxStream* s) { return s->interval >= GX_Q_FIRST && s->interval < GX_NINTERVALS; }
static float quantize_pitch(float pitch, int mode) {
  static const signed char scales[5][6] = {
    {1, 0}, {2, 0, 7}, {3, 0, 4, 7}, {3, 0, 3, 7}, {5, 0, 2, 4, 7, 9}
  };
  const signed char* notes = scales[mode - GX_Q_FIRST];
  pitch = clampf(pitch, -GX_MAX_SEMIS, GX_MAX_SEMIS);
  float best = 0, dist = 1e9f;
  for (int octave = -3; octave <= 3; octave++) {
    for (int k = 1; k <= notes[0]; k++) {
      float note = octave * 12 + notes[k];
      if (note < -GX_MAX_SEMIS || note > GX_MAX_SEMIS) continue;
      float d = fabsf(note - pitch);
      if (d < dist || (d == dist && fabsf(note) > fabsf(best))) { best = note; dist = d; }
    }
  }
  return best;
}
static float snapped_semis(const GxStream* s, float modulation) {
  float note = quantize_pitch(clampf(s->pitch, -GX_MAX_PITCH, GX_MAX_PITCH) + modulation, s->interval);
  return clampf(note + clampf(s->fine, -50, 50) * 0.01f, -GX_MAX_SEMIS, GX_MAX_SEMIS);
}
static float grain_semis(const GxStream* s, int iv) { return clampf(semis(s) + iv, -GX_MAX_SEMIS, GX_MAX_SEMIS); }
static double max_ratio(const GrainFx* x, const GxStream* s, int stream, int iv) {
  return ratio(quantized(s) ? snapped_semis(s, x->pmax[stream]) : grain_semis(s, iv) + x->pmax[stream]);
}
static int interval_max(const GxStream* s) {
  const signed char* t = intervals[s->interval >= 0 && s->interval < GX_NINTERVALS ? s->interval : 0];
  int m = t[1];
  for (int i = 2; i <= t[0]; i++) if (t[i] > m) m = t[i];
  return m;
}
/* A high grain reads fast: it is shortened so that it reads at most GX_MAX_READ_S. */
static double cap_len(const GrainFx* x, double len, double r) {
  double cap = GX_MAX_READ_S * x->sr / r;
  return len < cap ? len : cap;
}

/* Grain length in frames, with the LFOs' Length; never longer than the buffer. */
static double len_frames(const GrainFx* x, const GxParams* p, int i) {
  float cap = clampf(p->buffer_s, 1.0f, GX_MAX_BUFFER_S) * 1000.0f;
  if (cap > GX_MAX_LENGTH_MS) cap = GX_MAX_LENGTH_MS;
  return clampf(p->s[i].length_ms * exp2f(x->lm[i]), GX_MIN_LENGTH_MS, cap) * 0.001 * x->sr;
}

static double grain_length(const GrainFx* x, const GxParams* p, int s, int iv) {
  double fastest = max_ratio(x, &p->s[s], s, iv);
  double len = cap_len(x, len_frames(x, p, s), fastest);
  if (fabsf(p->s[s].scan) >= 0.00001f) {
    double cap = (clampf(p->buffer_s, 1, GX_MAX_BUFFER_S) * x->sr - 2 * MARGIN - FREEZE_FADE_S * x->sr) / fastest;
    if (len > cap) len = cap;
  }
  return len;
}

/* Room a forward grain needs to the write head, in frames (iv: the grain's interval). */
static double min_off(const GrainFx* x, const GxStream* s, int i, double len, int iv) {
  double r = max_ratio(x, s, i, iv);
  double f = x->frozen ? len * r : (r > 1.0 ? len * (r - 1.0) : 0.0);
  return f + MARGIN;
}

float gx_min_offset_ms(const GrainFx* x, const GxParams* p, int s) {
  const GxStream* st = &p->s[s];
  int iv = interval_max(st);
  double len = grain_length(x, p, s, iv);
  return (float)(min_off(x, st, s, len, iv) * 1000.0 / x->sr);
}

static float lfo_shape(GrainFx* x, int l, int shape) {
  double ph = x->lfo_ph[l];
  switch (shape) {
    case GX_LFO_TRI: return (float)(ph < 0.5 ? 4.0 * ph - 1.0 : 3.0 - 4.0 * ph);
    case GX_LFO_DRIFT: {
      float t = (float)ph, k = t * t * (3.0f - 2.0f * t);   /* smoothstep from the last value to the next */
      return x->lfo_a[l] + (x->lfo_b[l] - x->lfo_a[l]) * k;
    }
    case GX_LFO_SH: return x->lfo_b[l];
    default: return sinf(2.0f * (float)M_PI * (float)ph);
  }
}

/* LFOs one chunk on, and what they do to each stream now. */
static void modulate(GrainFx* x, const GxParams* p, int n) {
  /* Both links use the previous chunk: mutual modulation has no evaluation-order bias. */
  float prev[GX_LFOS];
  for (int l = 0; l < GX_LFOS; l++) prev[l] = x->lfo_v[l];
  for (int l = 0; l < GX_LFOS; l++) {
    float depth = clampf(p->lfo_rate_mod[l], -1, 1);
    float hz = clampf(p->lfo[l].rate, GX_LFO_MIN_HZ, GX_LFO_MAX_HZ);
    float max_hz = GX_LFO_MAX_HZ;
    if (p->lfo_sync[l]) {
      int div = p->lfo_div[l];
      if (div < 0) div = 0;
      if (div >= GX_NDIVS) div = GX_NDIVS - 1;
      hz = x->bpm / (60.0f * gx_div_beats[div]);
      max_hz = 128.0f; /* includes 1/64 at 400 BPM */
    }
    hz = clampf(hz * exp2f(depth * prev[1 - l] * GX_LFO_RATE_OCT), GX_LFO_MIN_HZ, max_hz);
    x->lfo_ph[l] += hz * n / x->sr;
    while (x->lfo_ph[l] >= 1.0) {
      x->lfo_ph[l] -= 1.0;
      x->lfo_a[l] = x->lfo_b[l];
      x->lfo_b[l] = 2.0f * rnd01(x) - 1.0f;
    }
    float v = lfo_shape(x, l, p->lfo[l].shape);
    float smooth = clampf(p->lfo_smooth[l], 0, 1);
    if (smooth > 0) {
      float tau = 0.5f * smooth * smooth / hz;
      float follow = -expm1f(-(float)n / ((float)x->sr * tau));
      x->lfo_slew[l] += follow * (v - x->lfo_slew[l]);
    } else x->lfo_slew[l] = v;
    v = x->lfo_slew[l];
    x->lfo_v[l] = p->lfo_mode[l] ? 0.5f * (v + 1.0f) : v;
  }
  for (int s = 0; s < GX_STREAMS; s++) {
    float pm = 0, pmax = 0, om = 0, lm = 0, am = 1, fm = 0, sm = 0;
    x->scan_mod_on[s] = 0;
    float pitch_span = quantized(&p->s[s]) ? GX_MOD_PITCH_Q : GX_MOD_PITCH;
    for (int l = 0; l < GX_MSRC; l++) {
      const float* d = p->mod[l][s];
      float v = l == GX_SRC_ENV ? x->env_v : x->lfo_v[l];
      float dp = clampf(d[GX_M_PITCH], -1, 1), dl = clampf(d[GX_M_LEVEL], -1, 1);
      pm += dp * v * pitch_span;
      pmax += fabsf(dp) * pitch_span;
      om += clampf(d[GX_M_OFFSET], -1, 1) * v * GX_MOD_OCT;
      lm += clampf(d[GX_M_LENGTH], -1, 1) * v * GX_MOD_OCT;
      fm += clampf(d[GX_M_FILTER], -1, 1) * v;
      sm += clampf(d[GX_M_SCAN], -1, 1) * v * 2;
      if (fabsf(d[GX_M_SCAN]) > 0.00001f) x->scan_mod_on[s] = 1;
      if (l == GX_SRC_ENV) am *= dl > 0 ? 1.0f - dl * (1.0f - v) : 1.0f + dl * v;   /* open / duck */
      else am *= 1.0f - fabsf(dl) * (1.0f - (dl > 0 ? v : -v)) * 0.5f;
    }
    x->fm[s] += (1 - expf(-(float)n / ((float)x->sr * 0.01f))) * (fm - x->fm[s]);
    x->sm[s] = clampf(p->s[s].scan + sm, -2, 2);
    x->pm[s] = pm; x->pmax[s] = pmax; x->om[s] = om; x->lm[s] = lm; x->am[s] = am;
  }
}

/* Cubic (Hermite) read of one channel at a fractional frame. */
static inline float hermite(const float* b, uint64_t mask, double pos, int ch) {
  uint64_t i = (uint64_t)pos;
  float t = (float)(pos - (double)i);
  float y0 = b[((i - 1) & mask) * 2 + ch], y1 = b[(i & mask) * 2 + ch];
  float y2 = b[((i + 1) & mask) * 2 + ch], y3 = b[((i + 2) & mask) * 2 + ch];
  float c1 = 0.5f * (y2 - y0);
  float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
  float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
  return ((c3 * t + c2) * t + c1) * t + y1;
}

static float offset_ms(const GrainFx* x, const GxParams* p, int s) {
  float off = p->s[s].offset_ms;
  return p->sync ? gx_div_beats[gx_snap_div(off, x->bpm)] * 60000.0f / x->bpm : off;
}

static double wrap_offset(double off, double lo, double hi) {
  double span = hi - lo;
  if (span <= 0) return lo;
  double v = fmod(off - lo, span);
  return lo + (v < 0 ? v + span : v);
}

/* A new grain of stream s, its first frame at t in this chunk; head = frames written up to and including t
 * (or the frozen head). */
static void spawn(GrainFx* x, const GxParams* p, int s, int t, uint64_t head) {
  int slot = -1;
  for (int i = 0; i < GX_POOL; i++) if (!x->g[i].active) { slot = i; break; }
  if (slot < 0) return;                              /* pool full: this grain is skipped */
  const GxStream* st = &p->s[s];
  const signed char* ivs = intervals[st->interval >= 0 && st->interval < GX_NINTERVALS ? st->interval : 0];
  int iv = ivs[1 + (int)(rnd01(x) * ivs[0]) % ivs[0]];
  int locked = quantized(st);
  double r = ratio(locked ? snapped_semis(st, x->pm[s]) : grain_semis(st, iv) + x->pm[s]);
  double len = grain_length(x, p, s, iv);
  int back = rnd01(x) < clampf(st->reverse, 0.0f, 1.0f);
  float buf_ms = clampf(p->buffer_s, 1.0f, GX_MAX_BUFFER_S) * 1000.0f;
  float off_ms = offset_ms(x, p, s);
  double base = clampf(off_ms, 0, buf_ms) * 0.001 * x->sr;
  double off = clampf(off_ms * exp2f(x->om[s]), 0.0f, buf_ms) * 0.001 * x->sr;
  if (x->scan_active[s]) {
    const double buffer = buf_ms * 0.001 * x->sr;
    const double fastest = max_ratio(x, st, s, iv);
    double lo = back ? MARGIN : min_off(x, st, s, len, iv);
    double hi = back ? buffer - len * fastest - MARGIN : buffer - MARGIN;
    if (x->frozen) hi -= x->freeze_tail;  /* leave room for the head's remaining capture transition */
    double speed = x->sm[s];
    double cursor = wrap_offset((double)head - x->scan_pos[s] - speed * t, lo, hi);
    x->scan_pos[s] = (double)head - cursor - speed * t;
    off = wrap_offset(cursor + off - base, lo, hi);
    off += clampf(st->spray, 0, 1) * (2 * rnd01(x) - 1) * len;
    off = wrap_offset(off, lo, hi);
  } else {
    off += clampf(st->spray, 0.0f, 1.0f) * (2.0f * rnd01(x) - 1.0f) * len;
    if (off < 0.0) off = 0.0;
    if (off > buf_ms * 0.001 * x->sr) off = buf_ms * 0.001 * x->sr;
    double lo = back ? MARGIN : min_off(x, st, s, len, iv);
    /* Frozen without Scan, random placement avoids cancellation between identical copies. */
    if (x->frozen && !back) off += rnd01(x) * len * r;
    else if (x->frozen) off += rnd01(x) * len;
    if (off < lo) off = lo;
  }
  GxGrain* g = &x->g[slot];
  g->active = 1;
  g->stream = s;
  g->pos = (double)head - off;
  g->dir = back ? -1 : 1;
  g->pitch_locked = locked;
  g->rbase = locked ? r : ratio(grain_semis(st, iv));
  g->inc = g->dir * r;
  g->age = -t;                                       /* reaches 0 at frame t of this chunk */
  g->len = (int)len;
  if (g->len < 2) g->len = 2;
  g->fade = g->fade_n = 0;
  /* Overlapping grains at the same pitch read the same delayed signal and add up in phase:
   * scale by the sum of the windows, so Rate and Length do not change the level much. */
  /* Broader range than the old Sharp window; retain at least 1 ms per edge. */
  float taper = 1.0f - 0.98f * clampf(p->shape, 0, 1);
  taper = fminf(1, fmaxf(taper, (float)(0.002 * x->sr / g->len)));
  g->win_step = (float)GX_WIN / (g->len * taper);
  float mean = 1.0f - taper * 0.5f;
  float sum = (float)(len / (clampf(st->rate_ms, GX_MIN_RATE_MS, GX_MAX_RATE_MS) * 0.001 * x->sr)) * mean;
  float norm = sum > 1.0f ? 1.0f / sum : 1.0f;
  float width = clampf(st->spread, 0.0f, 1.0f);
  float pan = 2.0f * rnd01(x) - 1.0f;
  /* At maximum width grains sit at the edges; intermediate widths remain gradual. */
  pan = width * (pan * (1.0f - width) + copysignf(width, pan));
  /* Edge placement and decorrelation reduce coherent summation; retain useful wet level. */
  norm *= 1.0f + 0.5f * width;
  g->gl = norm * (pan > 0.0f ? 1.0f - pan : 1.0f);
  g->gr = norm * (pan < 0.0f ? 1.0f + pan : 1.0f);
  /* Pan alone collapses towards mono when many identical grains overlap. Read the two
   * channels at slightly different positions as well. Keep the offset fixed throughout
   * the grain (no pitch wobble), and delay alternate sides randomly for a balanced image.
   * Only move backwards: neither channel can catch the write head. */
  g->stereo_lag = 0;
  if (width > 0) {
    double room = buf_ms * 0.001 * x->sr - off - MARGIN;
    if (back) room -= len * max_ratio(x, st, s, iv);
    if (x->frozen) room -= x->freeze_tail;
    double lag = width * (0.003 + 0.015 * rnd01(x)) * x->sr;
    lag = fmin(lag, fmax(0, room));
    g->stereo_lag = rnd01(x) < 0.5f ? lag : -lag;
  }
}

/* A forward grain that would catch the write head fades out before it. The head is at h0 at the start of the
 * chunk and moves on hs frames per frame (0 frozen, 1 recording). Happens when Freeze comes on, or when an LFO's
 * Pitch depth is turned up while grains sound (they started with less room). */
static void stop_at_head(GxGrain* g, double h0, double hs) {
  if (g->inc <= hs) return;
  double start = g->age < 0 ? -g->age : 0;            /* a grain born in this chunk starts later in it */
  double room = h0 + hs * start - GUARD - g->pos;
  int left = g->len - (g->age > 0 ? g->age : 0);
  double can_f = room / (g->inc - hs);
  if (can_f >= left) return;
  int can = (int)can_f;
  if (can <= 0) { g->active = 0; return; }
  if (g->fade && g->fade <= can) return;             /* already ends in time */
  g->fade = can < GX_FADE ? can : GX_FADE;
  if (!g->fade_n || g->fade_n < g->fade) g->fade_n = g->fade;   /* a running fade goes on from where it is */
}

static void svf_coefs(float f, double sr, float* g, int* hp) {
  float fc;
  if (f > 0.0f) { *hp = 1; fc = 20.0f * powf(HP_MAX / 20.0f, f); }
  else { *hp = 0; fc = 20000.0f * powf(LP_MIN / 20000.0f, -f); }
  fc = clampf(fc, 10.0f, (float)(0.45 * sr));
  *g = tanf((float)M_PI * fc / (float)sr);
}

static inline float svf(GxSvf* z, float v0, float g, int hp) {
  const float k = 1.41421356f;                       /* Q = 0.707: no resonance */
  float a1 = 1.0f / (1.0f + g * (g + k)), a2 = g * a1, a3 = g * a2;
  float v3 = v0 - z->ic2;
  float v1 = a1 * z->ic1 + a2 * v3;
  float v2 = z->ic2 + a2 * z->ic1 + a3 * v3;
  z->ic1 = 2.0f * v1 - z->ic1;
  z->ic2 = 2.0f * v2 - z->ic2;
  if (fabsf(z->ic1) < 1e-20f) z->ic1 = 0.0f;         /* no denormals in silence */
  if (fabsf(z->ic2) < 1e-20f) z->ic2 = 0.0f;
  return hp ? v0 - k * v1 - v2 : v2;
}

/* Linear below 0.7, then bends smoothly to at most 1. */
static inline float softclip(float v) {
  float a = fabsf(v);
  if (a <= 0.7f) return v;
  float y = 0.7f + 0.3f * tanhf((a - 0.7f) / 0.3f);
  return v < 0 ? -y : y;
}

static void chunk(GrainFx* x, const GxParams* p, const float* il, const float* ir, float* ol, float* orr, int n) {
  const uint64_t mask = x->frames - 1;
  float* b = x->buf;

  /* input peak for the widget */
  float pk = 0.0f;
  for (int i = 0; i < n; i++) {
    float a = fabsf(il[i]), c = fabsf(ir[i]);
    if (a > pk) pk = a;
    if (c > pk) pk = c;
  }
  x->in_peak *= expf(-(float)n / (PEAK_FALL_S * (float)x->sr));
  if (pk > x->in_peak) x->in_peak = pk;
  /* Independent envelope follower: changing sensitivity never changes audible input gain. */
  float attack = clampf(p->env_attack_ms, 0, 1000);
  float release = clampf(p->env_release_ms, 10, 5000);
  float ca = attack > 0 ? expf(-1.0f / (attack * 0.001f * (float)x->sr)) : 0;
  float cr = expf(-1.0f / (release * 0.001f * (float)x->sr));
  for (int i = 0; i < n; i++) {
    float v = fmaxf(fabsf(il[i]), fabsf(ir[i]));
    float c = v > x->env ? ca : cr;
    x->env = v + c * (x->env - v);
  }
  x->env_v = clampf((20.0f * log10f(x->env + 1e-9f) + clampf(p->env_sensitivity_db, -24, 24) - GX_ENV_FLOOR_DB) / (GX_ENV_TOP_DB - GX_ENV_FLOOR_DB), 0.0f, 1.0f);

  /* The button overrides the continuous Amount; releasing it returns to Amount. */
  const float amount = p->freeze ? 1.0f : clampf(p->freeze_amount, 0, 1);
  if (amount >= 1 && !x->frozen) {
    x->frozen = 1;
    x->freeze_tail = (int)(FREEZE_FADE_S * x->sr);
    if (x->freeze_tail < 1) x->freeze_tail = 1;
    for (int i = 0; i < GX_POOL; i++) {
      GxGrain* g = &x->g[i];
      if (!g->active || g->dir < 0) continue;
      double speed = g->pitch_locked ? g->rbase : g->rbase * ratio(x->pmax[g->stream]);
      double room = (double)x->w - GUARD - g->pos;
      if (room >= (g->len - g->age) * speed) continue;
      if (!g->fade_n) g->fade = g->fade_n = x->freeze_tail;
    }
  } else if (amount < 1 && x->frozen) {
    x->frozen = 0;
    x->freeze_tail = 0;
    x->xfade = GX_FADE;
    x->xl = x->last_l - il[0];
    x->xr = x->last_r - ir[0];
    x->freeze_loop = 0;
    x->hold_mix = 0;
  }
  if (amount > 0 && amount < 1 && !x->freeze_loop) {
    x->freeze_loop = 1;
    x->hold_period = (uint64_t)(clampf(p->buffer_s, 1, GX_MAX_BUFFER_S) * x->sr);
    x->hold_phase = 0;
    x->hold_mix = 0;
  }

  /* write */
  const uint64_t w0 = x->w;
  const float fb = clampf(p->feedback, 0.0f, 0.95f);
  const int tail = x->freeze_tail;
  int written = !x->frozen ? n : (tail < n ? tail : n);
  const float hold_step = (float)(1.0 / (FREEZE_FADE_S * x->sr));
  const double hold_edge = FREEZE_FADE_S * x->sr;
  x->record_peak *= expf(-(float)n / (PEAK_FALL_S * (float)x->sr));
  if (written > 0) {
    for (int i = 0; i < written; i++) {
      float l = il[i], r = ir[i];
      if (fb > 0.0f) {
        /* Feedback: last chunk's grains (one chunk later), without DC, soft-limited so the loop can't run away */
        float fl = i < x->fbn ? x->fbl[i] : 0.0f, fr = i < x->fbn ? x->fbr[i] : 0.0f;
        float yl = fl - x->dc_x[0] + 0.995f * x->dc_y[0], yr = fr - x->dc_x[1] + 0.995f * x->dc_y[1];
        x->dc_x[0] = fl; x->dc_y[0] = yl; x->dc_x[1] = fr; x->dc_y[1] = yr;
        l = softclip(l + fb * yl);
        r = softclip(r + fb * yr);
      }
      if (x->xfade > 0) {
        float k = (float)x->xfade / GX_FADE;
        l += x->xl * k; r += x->xr * k;
        x->xfade--;
      }
      uint64_t j = ((w0 + i) & mask) * 2;
      if (x->freeze_loop) {
        /* Retain the whole tape. A short live crossfade at its seam avoids a hard loop cut.
         * Copy one slot per frame from the previous turn; no snapshot allocation or bulk copy. */
        if (!x->frozen) x->hold_mix += clampf(amount - x->hold_mix, -hold_step, hold_step);
        double at = (double)x->hold_phase;
        float u = clampf((float)(fmin(at, x->hold_period - at) / hold_edge), 0, 1);
        float keep = u * u * (3 - 2 * u) * x->hold_mix;
        uint64_t old = ((w0 + i - x->hold_period) & mask) * 2;
        l += keep * (b[old] - l);
        r += keep * (b[old + 1] - r);
        if (++x->hold_phase >= x->hold_period) x->hold_phase = 0;
        if (amount <= 0 && x->hold_mix <= 0) x->freeze_loop = 0;
      }
      b[j] = l; b[j + 1] = r;
      float peak = fmaxf(fabsf(l), fabsf(r));
      if (peak > x->record_peak) x->record_peak = peak;
    }
    x->w = w0 + written;
    if (x->frozen) {
      x->freeze_tail -= written;
      if (!x->freeze_tail) { x->freeze_loop = 0; x->hold_mix = 0; }
    }
    uint64_t j = ((x->w - 1) & mask) * 2;
    x->last_l = b[j]; x->last_r = b[j + 1];
  }

  modulate(x, p, n);

  for (int s = 0; s < GX_STREAMS; s++) {
    const GxStream* st = &p->s[s];
    if (!st->on || (fabsf(st->scan) < 0.00001f && !x->scan_mod_on[s])) { x->scan_active[s] = 0; continue; }
    double off = clampf(offset_ms(x, p, s), 0, clampf(p->buffer_s, 1, GX_MAX_BUFFER_S) * 1000) * 0.001 * x->sr;
    if (!x->scan_active[s]) x->scan_pos[s] = (double)w0 - off;
    else x->scan_pos[s] -= off - x->scan_offset[s];
    x->scan_offset[s] = off;
    x->scan_active[s] = 1;
  }

  /* births */
  const double bpf = x->bpm / 60.0 / x->sr;           /* beats per frame */
  for (int s = 0; s < GX_STREAMS; s++) {
    const GxStream* st = &p->s[s];
    if (!st->on) { x->next[s] = 0.0; x->last_k[s] = INT64_MIN; continue; }
    if (p->sync) {
      /* a grain on every step of the note-value grid that falls in this chunk */
      double step = gx_div_beats[gx_snap_div(st->rate_ms, x->bpm)];
      int64_t k0 = (int64_t)floor(x->beat / step), k1 = (int64_t)floor((x->beat + n * bpf) / step);
      if (x->last_k[s] == INT64_MIN || x->last_k[s] > k1 || x->last_k[s] < k0 - 1) x->last_k[s] = k0;
      for (int64_t k = x->last_k[s] + 1; k <= k1; k++) {
        int t = (int)((k * step - x->beat) / bpf);
        if (t < 0) t = 0;
        if (t >= n) t = n - 1;
        spawn(x, p, s, t, x->frozen ? x->w : w0 + t + 1);
      }
      x->last_k[s] = k1;
      x->next[s] = 0.0;
      continue;
    }
    x->last_k[s] = INT64_MIN;
    double rate = clampf(st->rate_ms, GX_MIN_RATE_MS, GX_MAX_RATE_MS) * 0.001 * x->sr;
    if (x->next[s] > rate) x->next[s] = rate;        /* Rate turned down: do not wait for the old interval */
    float spray = clampf(st->spray, 0.0f, 1.0f);
    while (x->next[s] < n) {
      int t = x->next[s] > 0.0 ? (int)x->next[s] : 0;
      spawn(x, p, s, t, x->frozen ? x->w : w0 + t + 1);
      x->next[s] += rate * (1.0 + 0.9 * spray * (2.0 * rnd01(x) - 1.0));   /* Spray: the time */
    }
    x->next[s] -= n;
  }
  x->beat += n * bpf;
  for (int s = 0; s < GX_STREAMS; s++)
    if (x->scan_active[s]) x->scan_pos[s] += x->sm[s] * n;

  /* grains into the stream buses */
  for (int s = 0; s < GX_STREAMS; s++) {
    memset(x->sl[s], 0, sizeof(float) * n);
    memset(x->sr_[s], 0, sizeof(float) * n);
  }
  const float* win = x->win[0];
  for (int k = 0; k < GX_POOL; k++) {
    GxGrain* g = &x->g[k];
    if (!g->active) continue;
    /* Stepped notes stay stable for the grain's lifetime. Older continuous grains keep their original
     * modulation span even if Intervals changes to a quantized mode while they sound. */
    float pm = x->pm[g->stream];
    if (quantized(&p->s[g->stream])) pm *= GX_MOD_PITCH / GX_MOD_PITCH_Q;
    g->inc = g->dir * g->rbase * (g->pitch_locked ? 1.0 : exp2(pm / 12.0));
    if (x->frozen && !tail) stop_at_head(g, (double)x->w, 0.0);
    else stop_at_head(g, (double)w0, 1.0);
    if (tail && written < n) stop_at_head(g, (double)x->w, 0.0);  /* head stops partway through this chunk */
    if (!g->active) continue;
    float* bl = x->sl[g->stream];
    float* br = x->sr_[g->stream];
    int i = g->age < 0 ? -g->age : 0;
    if (g->age < 0) g->age = 0;
    const double lag_l = fmin(0, g->stereo_lag), lag_r = -fmax(0, g->stereo_lag);
    for (; i < n; i++) {
      if (g->age >= g->len || (g->fade_n && g->fade <= 0)) { g->active = 0; break; }
      /* The rising half of Hann also supplies the falling edge; clamp to its plateau. */
      int edge = g->age < g->len - g->age ? g->age : g->len - g->age;
      float u = fminf(edge * g->win_step, GX_WIN * 0.5f);
      int wi = (int)u;
      float a = win[wi] + (win[wi + 1] - win[wi]) * (u - wi);
      if (g->fade_n) { a *= (float)g->fade / g->fade_n; g->fade--; }
      bl[i] += a * g->gl * hermite(b, mask, g->pos + lag_l, 0);
      br[i] += a * g->gr * hermite(b, mask, g->pos + lag_r, 1);
      g->pos += g->inc;
      g->age++;
    }
    if (g->active && g->age >= g->len) g->active = 0;
  }

  /* filter, stream gain (ramped), sum; mix (equal power, ramped) */
  float dry = cosf(clampf(p->mix, 0.0f, 1.0f) * (float)M_PI * 0.5f);
  float wet = sinf(clampf(p->mix, 0.0f, 1.0f) * (float)M_PI * 0.5f) * p->gain;
  if (x->dry < 0.0f) { x->dry = dry; x->wet = wet; }
  float wl[GX_CHUNK], wr[GX_CHUNK];
  memset(wl, 0, sizeof(float) * n);
  memset(wr, 0, sizeof(float) * n);
  for (int s = 0; s < GX_STREAMS; s++) {
    float gt = p->s[s].on ? p->s[s].gain * x->am[s] : 0.0f;
    if (x->fgain[s] < 0.0f) x->fgain[s] = gt;
    float g0 = x->fgain[s], dg = (gt - g0) / n;
    x->fgain[s] = gt;
    if (g0 == 0.0f && gt == 0.0f) continue;          /* silent stream: its filter state may rest */
    float fg; int hp;
    float base_filter = clampf(p->s[s].filter, -1, 1);
    /* Modulate within the selected filter type to avoid switching SVF topology at zero. */
    float filter = base_filter > 0 ? clampf(base_filter + x->fm[s], 1e-8f, 1)
                                   : clampf(base_filter + x->fm[s], -1, 0);
    svf_coefs(filter, x->sr, &fg, &hp);
    for (int i = 0; i < n; i++) {
      float gg = g0 + dg * (i + 1);
      wl[i] += gg * svf(&x->svf[s][0], x->sl[s][i], fg, hp);
      wr[i] += gg * svf(&x->svf[s][1], x->sr_[s][i], fg, hp);
    }
  }
  memcpy(x->fbl, wl, sizeof(float) * n);              /* for Feedback in the next chunk */
  memcpy(x->fbr, wr, sizeof(float) * n);
  x->fbn = n;
  float d0 = x->dry, dd = (dry - d0) / n, e0 = x->wet, de = (wet - e0) / n;
  x->dry = dry; x->wet = wet;
  for (int i = 0; i < n; i++) {
    float d = d0 + dd * (i + 1), e = e0 + de * (i + 1);
    float l = d * il[i] + e * wl[i], r = d * ir[i] + e * wr[i];
    ol[i] = isfinite(l) ? l : 0.0f;
    orr[i] = isfinite(r) ? r : 0.0f;
  }
}

void gx_process(GrainFx* x, const GxParams* p, const float* in_l, const float* in_r,
                float* out_l, float* out_r, uint32_t n) {
  float il[GX_CHUNK], ir[GX_CHUNK];
  for (uint32_t o = 0; o < n; o += GX_CHUNK) {
    int m = (int)(n - o < GX_CHUNK ? n - o : GX_CHUNK);
    memcpy(il, in_l + o, sizeof(float) * m);           /* out may be the same buffers as in */
    memcpy(ir, in_r + o, sizeof(float) * m);
    chunk(x, p, il, ir, out_l + o, out_r + o, m);
  }
}
