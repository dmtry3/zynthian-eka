/* Eka Grain Flow core: a granular effect on live input. No LV2 here, so it can be tested on its own.
 * The input is written into a ring buffer; GX_STREAMS streams of grains read from it.
 * Nothing here allocates or locks after gx_init(). */
#ifndef GRAIN_FX_H
#define GRAIN_FX_H
#include <stdint.h>

#define GX_STREAMS 3
#ifndef GX_POOL
#define GX_POOL 48            /* grains sounding at once, all streams together */
#endif
#define GX_CHUNK 64           /* frames processed at a time (parameters are applied per chunk) */
#define GX_FADE 256           /* frames: fade-out of a cut grain, cross-fade when the recording resumes */
#define GX_WIN 1024           /* grain window table */
#define GX_MAX_BUFFER_S 8.0f  /* the longest Offset */
#define GX_MIN_LENGTH_MS 10.0f
#define GX_MAX_LENGTH_MS 8000.0f   /* and never longer than the buffer (buffer_s) */
#define GX_MIN_RATE_MS 5.0f
#define GX_MAX_RATE_MS 2000.0f
#define GX_MAX_PITCH 24.0f    /* semitones, both ways */
#define GX_NSHAPES 2          /* grain window: 0 soft (Hann), 1 sharp (Tukey, flat middle) */

/* Modulation: GX_LFOS LFOs for the whole effect; each one moves Pitch, Offset, Length and Level of any stream
 * by its own depth (-1..1). At depth 1: Pitch +-GX_MOD_PITCH semitones, Offset and Length x 2^(+-GX_MOD_OCT),
 * Level from full down to silence (a positive depth is full at the LFO's top, a negative one at its bottom).
 * Pitch acts on sounding grains too (their read speed follows the LFO); Offset and Length are taken when a
 * grain starts; Level acts on the stream. */
#define GX_LFOS 2
#define GX_MSRC (GX_LFOS + 1)  /* modulation sources: LFO 1, LFO 2, Env (the input's level, 0..1) */
#define GX_SRC_ENV GX_LFOS
#define GX_MOD_PITCH 2.0f
#define GX_MOD_PITCH_Q 24.0f /* quantized modes: full depth spans two octaves */
#define GX_MOD_OCT 2.0f
enum { GX_M_PITCH = 0, GX_M_OFFSET, GX_M_LENGTH, GX_M_LEVEL, GX_M_FILTER, GX_M_SCAN, GX_MDST };
enum { GX_LFO_SINE = 0, GX_LFO_TRI, GX_LFO_DRIFT, GX_LFO_SH, GX_NLFO_SHAPES };   /* drift = smooth random */
#define GX_LFO_MIN_HZ 0.02f
#define GX_LFO_MAX_HZ 20.0f
#define GX_LFO_RATE_OCT 3.0f /* other LFO -> rate: depth 1 gives x1/8..x8 */
/* Env: the input's level, 0 at ENV_FLOOR_DB and below, 1 at ENV_TOP_DB; fast up, slower down. Unlike the LFOs it
 * only goes one way: a positive depth raises Pitch / Offset / Length the louder you play, a negative one lowers
 * them; on Level a positive depth opens the stream as you play louder, a negative one ducks it. */
#define GX_ENV_FLOOR_DB -48.0f
#define GX_ENV_TOP_DB -6.0f

/* Intervals 0..8 add a random interval at birth; Q modes round Pitch + modulation to a note grid. */
#define GX_Q_FIRST 9          /* previous indices keep random intervals; new modes quantize Pitch */
#define GX_NINTERVALS 14
extern const char* const gx_interval_names[GX_NINTERVALS];
#define GX_MAX_SEMIS 36.0f    /* Pitch + interval, both ways */
#define GX_MAX_READ_S 8.0f    /* a grain reads at most this much of the buffer (a high grain is shortened) */

/* Tempo sync: Rate and Offset snap to the nearest note value at the host's tempo. */
#define GX_NDIVS 20
extern const float gx_div_beats[GX_NDIVS];
extern const char* const gx_div_names[GX_NDIVS];
int gx_snap_div(float ms, float bpm);     /* index into gx_div_beats */

/* One stream of grains. Its values are taken when a grain is born and stay with that grain,
 * so turning a knob never clicks; only gain and filter act on the stream as it sounds. */
typedef struct {
  int   on;
  float offset_ms;   /* how far behind the write head the grain starts, 0..buffer */
  float rate_ms;     /* time between grain starts */
  float length_ms;   /* grain length */
  float pitch;       /* semitones */
  float fine;        /* cents, -50..50: a small detune for doubling and chorus */
  float reverse;     /* 0..1: share of grains played backwards */
  float filter;      /* -1..1: 0 off, < 0 low-pass coming down, > 0 high-pass going up */
  float spread;      /* 0..1: grain pan and stereo read separation (up to 18 ms) */
  float gain;        /* linear */
  float spray;       /* 0..1: random start time (+-Rate) and place (+-Length) of each grain */
  int   interval;    /* 0 off; 1..8 random intervals; GX_Q_FIRST..GX_NINTERVALS-1 quantized Pitch */
  float scan;        /* -2..2: source cursor speed; 0 off, +1 normal forward, -1 normal backward */
} GxStream;

typedef struct {
  GxStream s[GX_STREAMS];
  float mix;         /* 0 = dry only, 1 = grains only (equal power) */
  float gain;        /* linear, grains only */
  float feedback;    /* 0..0.95: grains written back into the buffer (soft-limited) */
  int   sync;        /* 1: Rate and Offset snap to note values at bpm; grains start on that grid */
  float buffer_s;    /* 1..GX_MAX_BUFFER_S: the longest Offset */
  int   freeze;      /* button / pedal: override Amount with full Freeze */
  float freeze_amount; /* 0..1: live recording through gradual retention to full Freeze */
  float shape;       /* 0..1: Hann through progressively shorter Tukey edges */
  struct { float rate; int shape; } lfo[GX_LFOS];   /* Hz, GX_LFO_* */
  int lfo_sync[GX_LFOS], lfo_div[GX_LFOS]; /* tempo-derived cycle length, gx_div_beats */
  float lfo_smooth[GX_LFOS]; /* 0..1: slew time up to half the current cycle */
  int   lfo_mode[GX_LFOS]; /* 0 bipolar (-1..+1), 1 positive (0..+1) */
  float lfo_rate_mod[GX_LFOS]; /* -1..1: other LFO modulates this LFO's rate */
  float env_attack_ms, env_release_ms, env_sensitivity_db; /* follower only; no audio gain */
  float mod[GX_MSRC][GX_STREAMS][GX_MDST];          /* depth -1..1 */
} GxParams;

typedef struct {
  int    active, stream;
  double pos, inc;   /* read position in frames written so far (never wraps) and step; inc < 0 = backwards */
  double stereo_lag; /* signed channel read offset: positive delays R, negative delays L */
  double rbase;      /* speed without the LFO (Pitch + Fine + interval when the grain started) */
  int    dir;        /* 1 forward, -1 backwards */
  int    pitch_locked; /* quantized Pitch is taken at birth and held for this grain */
  int    age, len;
  float  gl, gr;
  float  win_step;   /* envelope table step, captured at grain birth */
  int    fade, fade_n;   /* fade-out: frames left and its length; 0 = none */
} GxGrain;

typedef struct { float ic1, ic2; } GxSvf;   /* TPT state-variable filter, one channel */

typedef struct {
  double   sr;
  float*   buf;          /* interleaved stereo ring */
  uint64_t frames;       /* ring frames, a power of two */
  uint64_t w;            /* frames written: the write head */
  int      frozen;
  int      freeze_loop;  /* intermediate Amount: gradual renewal of the whole tape */
  uint64_t hold_period, hold_phase;
  float    hold_mix;     /* smoothed retention for intermediate Amount */
  float    record_peak;  /* peak of the signal actually written, for the level profile */
  int      freeze_tail;  /* frames of recording left to fade grains safely when Freeze is pressed */
  int      xfade;        /* frames left of the cross-fade after a freeze */
  float    xl, xr;       /* the step that cross-fade removes */
  float    last_l, last_r;
  GxGrain  g[GX_POOL];
  double   next[GX_STREAMS];   /* frames until the next grain of each stream */
  double   scan_pos[GX_STREAMS], scan_offset[GX_STREAMS]; /* cursor at chunk start; base Offset last seen */
  int      scan_active[GX_STREAMS];
  uint32_t rng;
  float    win[GX_NSHAPES][GX_WIN + 2];
  GxSvf    svf[GX_STREAMS][2];
  float    fgain[GX_STREAMS];  /* stream gains in use, ramped */
  float    dry, wet;           /* < 0: not set yet */
  float    in_peak;            /* input peak, decaying, for the widget */
  double   lfo_ph[GX_LFOS];    /* 0..1 */
  float    lfo_slew[GX_LFOS]; /* bipolar smoothing state, before polarity conversion */
  float    lfo_v[GX_LFOS];     /* -1..1, this chunk */
  float    lfo_a[GX_LFOS], lfo_b[GX_LFOS];   /* random values: the last and the next (drift, S&H) */
  float    pm[GX_STREAMS], pmax[GX_STREAMS]; /* Pitch modulation and reach; Q modes apply it at birth */
  float    om[GX_STREAMS], lm[GX_STREAMS];   /* Offset and Length from the LFOs, octaves */
  float    fm[GX_STREAMS], sm[GX_STREAMS]; /* filter offset and effective scan velocity */
  int      scan_mod_on[GX_STREAMS];
  float    am[GX_STREAMS];                   /* Level from the LFOs, 0..1 */
  float    env, env_v;                       /* input level follower (linear) and its 0..1 value */
  float    fbl[GX_CHUNK], fbr[GX_CHUNK];     /* last chunk's grains, written back with Feedback */
  int      fbn;
  float    dc_x[2], dc_y[2];                 /* DC blocker in the feedback path */
  float    bpm;                              /* host tempo */
  double   beat;                             /* beat clock: the host's while it rolls, else our own */
  int64_t  last_k[GX_STREAMS];               /* sync: last grid step a grain started on */
  float    sl[GX_STREAMS][GX_CHUNK], sr_[GX_STREAMS][GX_CHUNK];
} GrainFx;

/* 0 = ok, -1 = no memory. */
int  gx_init(GrainFx* x, double sr);
void gx_free(GrainFx* x);
/* Silence: empty buffer, no grains. */
void gx_reset(GrainFx* x);
/* out may be the same buffers as in. */
void gx_process(GrainFx* x, const GxParams* p, const float* in_l, const float* in_r,
                float* out_l, float* out_r, uint32_t n);
void gx_default_params(GxParams* p);
/* Host tempo (ignored outside 20..400) and song position in beats (< 0: not rolling, keep our own clock). */
void gx_set_tempo(GrainFx* x, float bpm, double beat);
int  gx_active(const GrainFx* x);
/* The k-th active grain (k = 0..): its stream and how far behind the write head it reads now, in seconds.
 * 0 = no such grain. */
int  gx_grain_info(const GrainFx* x, int k, int* stream, float* seconds_back);
/* The shortest Offset (ms) a forward grain of stream s gets now: pitch up (with the LFOs' reach) needs room to
 * the write head. */
float gx_min_offset_ms(const GrainFx* x, const GxParams* p, int s);
/* Linear stream gain for -60..+6 dB (-60 = silent). */
float gx_db(float db);

#endif
