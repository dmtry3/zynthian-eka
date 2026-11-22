/* Eka Pads: 16-pad drum sampler + 8-scene step sequencer + Room Space DSP core (pure C, no LV2). */
#ifndef EP_PADS_CORE_H
#define EP_PADS_CORE_H

#include <stdint.h>

#define EP_PADS       16
#define EP_VOICES     16
#define EP_CHOKE_MAX  4
#define EP_FADE_MS    2.0f
#define EP_SCENES     8
#define EP_STEPS      32
#define EP_STEPS_MIN  4
#define EP_STEPS_DEF  16

/* Sequencer modes */
enum {
  EP_SEQ_STOP = 0,
  EP_SEQ_PLAY,
  EP_SEQ_REC
};

/* Playback modes for each pad */
enum {
  EP_MODE_ONESHOT = 0,
  EP_MODE_GATE,
  EP_MODE_REV_ONESHOT,
  EP_MODE_REV_GATE,
  EP_NMODES
};

typedef struct {
  const float* data;      /* interleaved stereo float, frames + guard */
  int64_t frames;
  float rate;
} EpSource;

typedef struct {
  float pitch;            /* -24..+24 semitones */
  float decay;            /* 0..100 % (10 ms .. 10 s; >=99.5 = full sample) */
  float filter;           /* -100..+100 % (<0 = LP, 0 = open, >0 = HP) */
  float level;            /* 0..100 % */
  float start;            /* 0..95 % */
  float end;              /* 5..100 % (default 100) */
  int mode;               /* EP_MODE_* */
  float punch;            /* -100..+100 % (fast pitch envelope on attack) */
  float drive;            /* 0..100 % (0..50 soft saturation, 50..100 bitcrush/decimate) */
  float pan;              /* -100..+100 % */
  float vel_sens;         /* 0..100 % */
  int choke;              /* 0 = off, 1..4 = choke group */
  float send;             /* 0..100 % (Space room reverb send) */
  int mute;               /* 0 or 1 */
  int solo;               /* 0 or 1 */
  int midi_note;          /* 0..127 (default 36 + pad) */
} EpPadParams;

typedef struct {
  uint8_t vel;            /* 0 = off, 1..127 = trigger velocity */
  uint8_t roll;           /* bits 0..1: ratchet 0..3 (1x..4x); bits 2..7: signed micro-timing (-32..+31 / 64 step) */
} EpStep;

typedef struct {
  EpStep steps[EP_PADS][EP_STEPS];
  uint8_t pad_start[EP_PADS]; /* 0..31 (default 0) */
  uint8_t pad_end[EP_PADS];   /* 1..32 (default 16) */
} EpScene;

typedef struct {
  int active;
  int pad;                /* 0..15 */
  uint32_t gen;           /* source generation when started */
  double pos;             /* fractional frame position in source */
  double end_frame;
  double start_frame;
  int reverse;
  int gate_mode;
  int released;
  float vel_gain;
  float env;              /* amplitude envelope 0..1 */
  float decay_coef;       /* per-sample multiplier */
  int full_sample;        /* 1 if decay >= 99.5% */
  float fade;             /* anti-click fade out (1..0 when fading) */
  float fade_step;        /* >0 when fading out */
  float punch_env;        /* 1..0 fast pitch envelope */
  float punch_coef;
  /* TPT SVF state (stereo) */
  float ic1_l, ic2_l;
  float ic1_r, ic2_r;
  /* Bitcrush / sample-rate hold state */
  float hold_l, hold_r;
  float hold_phase;
  uint64_t age;
} EpVoice;

/* Lightweight static-buffer stereo Room Reverb (4 combs + 2 allpasses per channel) */
#define EP_REV_COMBS   4
#define EP_REV_APS     2
#define EP_REV_MAXBUF  4096

typedef struct {
  float comb_buf[2][EP_REV_COMBS][EP_REV_MAXBUF];
  int comb_len[2][EP_REV_COMBS];
  int comb_pos[2][EP_REV_COMBS];
  float comb_lp[2][EP_REV_COMBS];
  float ap_buf[2][EP_REV_APS][1024];
  int ap_len[2][EP_REV_APS];
  int ap_pos[2][EP_REV_APS];
  float energy;
  int tail_samples;
} EpReverb;

typedef struct {
  int mode;               /* EP_SEQ_STOP / PLAY / REC */
  int scene;              /* 0..7 currently playing scene */
  int pend_scene;         /* -1 or 0..7 queued for next bar */
  int cur_step;           /* -1 if stopped, 0..31 (for selected pad) */
  int num_steps;          /* 4..32 global/bar cycle length */
  int quantize;           /* 0..100 % quantize strength (0=free micro-timing, 100=hard grid) */
  double bpm;             /* 40..240 */
  float swing;            /* 50..75 % */
  double bar_phase;       /* 0..num_steps in 16th-note units */
  int last_step_fired;    /* -1 or 0..31 */
  int host_playing;       /* 1 if locked to host transport */
  /* Independent per-pad polyrhythmic lane state */
  double pad_phase[EP_PADS];
  int    pad_last_step[EP_PADS];
  int    pad_cur_step[EP_PADS];
  /* Ratchet (sub-step roll) pending counters */
  uint8_t roll_rem[EP_PADS];
  uint8_t roll_vel[EP_PADS];
  double roll_timer[EP_PADS];
  double roll_interval[EP_PADS];
  /* Prevent double trigger when Overdub quantizes a hit forward to the upcoming step */
  uint16_t skip_next_mask;
  int skip_step[EP_PADS];
} EpSeq;

typedef struct {
  double sr;
  EpSource src[EP_PADS];
  uint32_t gen[EP_PADS];
  EpPadParams pad[EP_PADS];
  EpVoice voices[EP_VOICES];
  uint64_t voice_seq;
  EpScene scenes[EP_SCENES];
  EpSeq seq;
  EpReverb rev;
  float master_vol;       /* 0..100 % */
  float stereo_width;     /* 0..200 % (0=mono, 100=normal, 200=wide) */
  int   retrig_ms;        /* 0..80 ms same-pad double-trigger mask (default 25) */
  int   min_vel;          /* 0..40 minimum MIDI velocity threshold (default 0) */
  int   xtalk;            /* 0..60 % adjacent-pad crosstalk suppression (default 0) */
  float activity[EP_PADS];/* 0..1 visual meter per pad */
  float sel_playhead;     /* -1 or 0..1 playhead of selected pad */
} EpCore;

void ep_pad_defaults(EpPadParams* p, int pad_index);
void ep_core_init(EpCore* c, double sr);
void ep_core_reset(EpCore* c);

uint32_t ep_core_set_source(EpCore* c, int pad, const EpSource* src);
int ep_core_gen_in_use(const EpCore* c, int pad, uint32_t gen);

void ep_core_trigger(EpCore* c, int pad, int vel);
void ep_core_release(EpCore* c, int pad);
void ep_core_all_off(EpCore* c);

/* Sequencer control */
void ep_seq_set_mode(EpCore* c, int mode);
void ep_seq_set_len(EpCore* c, int steps);
void ep_seq_set_pad_range(EpCore* c, int scene, int pad, int start_step, int end_step);
void ep_seq_set_quantize(EpCore* c, int q);
void ep_seq_request_scene(EpCore* c, int scene);
void ep_seq_sync_host(EpCore* c, double bpm, int playing, double bar_beat, int bpb);
void ep_seq_record_hit(EpCore* c, int pad, int vel);
void ep_seq_clear_pad(EpCore* c, int scene, int pad);
void ep_seq_clear_scene(EpCore* c, int scene);
void ep_seq_copy_scene(EpCore* c, int src_scene, int dst_scene);

/* Render `nframes` stereo audio. `sel_pad` (0..15) is used to update `c->sel_playhead`. */
void ep_core_process(EpCore* c, float* out_l, float* out_r, uint32_t nframes, int sel_pad);

#endif
