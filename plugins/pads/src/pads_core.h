/* Eka Pads: 16-pad drum sampler + 8-scene step sequencer + Bus Glue DSP core (pure C, no LV2). */
#ifndef EP_PADS_CORE_H
#define EP_PADS_CORE_H

#include <stdint.h>

#define EP_PADS       16
#define EP_VOICES     16
#define EP_CHOKE_MAX  4
#define EP_AUX_BUSES  4
#define EP_MOD_SLOTS  4
#define EP_LFOS       2
#define EP_FADE_MS    2.0f
#define EP_SCENES     8
#define EP_STEPS      32
#define EP_STEPS_MIN  4
#define EP_STEPS_DEF  16
#define EP_CHAIN_MAX  8

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
  EP_MODE_LOOP,
  EP_NMODES
};

/* Pad performance play modes (16 Levels / Chromatic) */
enum {
  EP_PLAY_NORMAL = 0,
  EP_PLAY_CHROM,
  EP_PLAY_16VEL,
  EP_PLAY_16FLT,
  EP_PLAY_NMODES
};

/* Note Repeat divisions */
enum {
  EP_RPT_OFF = 0,
  EP_RPT_8,
  EP_RPT_16,
  EP_RPT_16T,
  EP_RPT_32,
  EP_RPT_NMODES
};

/* Metronome modes */
enum {
  EP_METRO_OFF = 0,
  EP_METRO_REC,
  EP_METRO_ON
};

/* Pad link modes */
enum {
  EP_LINK_LAYER = 0,
  EP_LINK_RR
};

/* Modulation matrix sources, destinations, and LFO waves */
enum {
  EP_MSRC_OFF = 0,
  EP_MSRC_VEL,
  EP_MSRC_RAND,
  EP_MSRC_LFO1,
  EP_MSRC_LFO2,
  EP_MSRC_ENV,
  EP_MSRC_COUNT
};

enum {
  EP_MDST_NONE = 0,
  EP_MDST_PITCH,
  EP_MDST_DECAY,
  EP_MDST_FILTER,
  EP_MDST_LEVEL,
  EP_MDST_START,
  EP_MDST_DRIVE,
  EP_MDST_PAN,
  EP_MDST_WIDTH,
  EP_MDST_AUX1,
  EP_MDST_AUX2,
  EP_MDST_COUNT
};

enum {
  EP_LFO_SINE = 0,
  EP_LFO_TRI,
  EP_LFO_SAW,
  EP_LFO_SQR,
  EP_LFO_SH,
  EP_LFO_NWAVES
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
  float start;            /* 0..99 % */
  float end;              /* 1..100 % (default 100) */
  int mode;               /* EP_MODE_* */
  float punch;            /* -100..+100 % (fast pitch envelope on attack) */
  float drive;            /* 0..100 % (0..50 soft saturation, 50..100 bitcrush/decimate) */
  float pan;              /* -100..+100 % */
  float vel_sens;         /* 0..100 % */
  int choke;              /* 0 = off, 1..4 = choke group */
  float width;            /* -100..+100 % (-100=mono, 0=normal, +100=wide) */
  int mute;               /* 0 or 1 */
  int solo;               /* 0 or 1 */
  int midi_note;          /* 0..127 (default 36 + pad) */
  float aux[EP_AUX_BUSES];/* 0..100 % (Aux Send 1..4 stereo outs) */
  int   lfo_wave[EP_LFOS];      /* EP_LFO_* (+ bit 4 = BPM sync) */
  float lfo_rate[EP_LFOS];      /* 0.05..20.0 Hz */
  int   mod_src[EP_MOD_SLOTS];  /* EP_MSRC_* */
  int   mod_dst[EP_MOD_SLOTS];  /* EP_MDST_* */
  float mod_amt[EP_MOD_SLOTS];  /* -100..+100 % */
  /* v0.3.0 extensions (appended for seamless binary snapshot migration) */
  float attack;           /* 0..100 % (0 ms .. 500 ms exponential fade-in) */
  float reso;             /* 0..100 % (filter resonance; default 20% -> k=1.15) */
  int   link_pad;         /* 0 = off, 1..16 = linked target pad (1-based) */
  int   link_mode;        /* 0 = Layer, 1 = Round-Robin */
} EpPadParams;

typedef struct {
  uint8_t vel;            /* 0 = off, 1..127 = trigger velocity */
  uint8_t roll;           /* bits 0..1: ratchet 0..3 (1x..4x); bits 2..7: signed micro-timing (-30..+30 / 64 step) */
  uint8_t chance;         /* 0 = 100% (default), 1..100 = trigger probability % */
  int8_t  pitch_off;      /* -24..+24 semitones p-lock offset */
  int8_t  filt_off;       /* -100..+100 % filter p-lock offset */
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
  int loop_mode;
  int released;
  float vel_gain;
  float atk_env;          /* 0..1 attack fade-in envelope */
  float atk_inc;          /* per-sample attack increment */
  float env;              /* amplitude envelope 0..1 */
  float decay_coef;       /* per-sample multiplier */
  int full_sample;        /* 1 if decay >= 99.5% */
  float fade;             /* anti-click fade out (1..0 when fading) */
  float fade_step;        /* >0 when fading out */
  float punch_env;        /* 1..0 fast pitch envelope */
  float punch_coef;
  /* Per-voice parameter-lock / 16-levels offsets */
  float plock_pitch;      /* semitones */
  float plock_filter;     /* -100..+100 % */
  /* TPT SVF state (stereo) */
  float ic1_l, ic2_l;
  float ic1_r, ic2_r;
  /* Bitcrush / sample-rate hold state */
  float hold_l, hold_r;
  float hold_phase;
  /* Per-voice stereo widener (dual-tap Haas + 2-stage allpass + sub-bass LP) */
  float wid_buf[1024];
  int   wid_pos;
  float wid_lp_l, wid_lp_r;
  float wid_ap1_x, wid_ap1_y;
  float wid_ap2_x, wid_ap2_y;
  /* Per-voice modulation states */
  float hit_vel;          /* 0..1 trigger velocity */
  float hit_rand;         /* -1..+1 random value per trigger */
  float env_fol;          /* 0..1 envelope follower */
  uint64_t age;
} EpVoice;

typedef struct {
  int mode;               /* EP_SEQ_STOP / PLAY / REC */
  int scene;              /* 0..7 currently playing scene */
  int pend_scene;         /* -1 or 0..7 queued for next bar */
  int cur_step;           /* -1 if stopped, 0..31 (for selected pad) */
  int num_steps;          /* 4..32 global/bar cycle length */
  int quantize;           /* 0..100 % quantize strength (0=free micro-timing, 100=hard grid) */
  double bpm;             /* 40..240 */
  float swing;            /* 50..75 % */
  double bar_phase;       /* 0..16 in 16th-note units */
  int last_step_fired;    /* -1 or 0..31 */
  int host_playing;       /* 1 if locked to host transport */
  /* Independent per-pad polyrhythmic lane state */
  double pad_phase[EP_PADS];
  int    pad_last_step[EP_PADS];
  int    pad_cur_step[EP_PADS];
  /* Ratchet (sub-step roll) pending counters */
  uint8_t roll_rem[EP_PADS];
  uint8_t roll_vel[EP_PADS];
  int8_t  roll_pitch[EP_PADS];
  int8_t  roll_filt[EP_PADS];
  double roll_timer[EP_PADS];
  double roll_interval[EP_PADS];
  /* Prevent double trigger when Overdub quantizes a hit forward to the upcoming step */
  uint16_t skip_next_mask;
  int skip_step[EP_PADS];
  /* Metronome, Count-in, Scene Chain, 1-bar Fill, Undo Overdub */
  int metro_mode;         /* EP_METRO_* */
  int countin_en;         /* 0 or 1 (1-bar count-in before Rec from Stop) */
  int countin_rem;        /* >0 while counting in (16..1 steps remaining) */
  double countin_phase;   /* 0..16 step phase during count-in */
  int last_metro_beat;    /* -1 or 0..3 */
  int fill_ret_scene;     /* -1 or 0..7 scene to return to after 1-bar fill */
  int chain_on;           /* 0 or 1 */
  int chain_len;          /* 1..EP_CHAIN_MAX */
  int chain_pos;          /* 0..EP_CHAIN_MAX-1 */
  uint8_t chain[EP_CHAIN_MAX];
  EpScene undo_scene;
  int undo_scene_idx;     /* -1 if no undo snapshot */
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
  float master_vol;       /* 0..100 % */
  float stereo_width;     /* 0..200 % (0=mono, 100=normal, 200=wide) */
  float bus_glue;         /* 0..100 % master bus compressor / glue */
  float comp_env;         /* stereo bus compressor detector state */
  int   retrig_ms;        /* 0..80 ms same-pad double-trigger mask (default 25) */
  int   min_vel;          /* 0..40 minimum MIDI velocity threshold (default 0) */
  int   xtalk;            /* 0..60 % adjacent-pad crosstalk suppression (default 0) */
  int   play_mode;        /* EP_PLAY_* (Normal / Chrom / 16Vel / 16Flt) */
  int   repeat_div;       /* EP_RPT_* (Off / 1/8 / 1/16 / 1/16T / 1/32) */
  /* Note Repeat held-pad state (16 slots) */
  uint16_t held_mask;
  uint8_t  held_vel[EP_PADS];
  int8_t   held_pad[EP_PADS];      /* actual sound pad triggered */
  float    held_pitch[EP_PADS];
  float    held_filt[EP_PADS];
  double   rpt_timer[EP_PADS];
  /* Round-robin step counter per pad */
  uint8_t  rr_pos[EP_PADS];
  /* Internal metronome click synth */
  float    click_phase;
  float    click_env;
  float    click_freq;
  uint32_t rng_state;
  double lfo_phase[EP_PADS][EP_LFOS];
  float  lfo_sh[EP_PADS][EP_LFOS];
  float  lfo_val[EP_PADS][EP_LFOS];
  float activity[EP_PADS];/* 0..1 visual meter per pad */
  float sel_playhead;     /* -1 or 0..1 playhead of selected pad */
} EpCore;

void ep_pad_defaults(EpPadParams* p, int pad_index);
void ep_core_init(EpCore* c, double sr);
void ep_core_reset(EpCore* c);

uint32_t ep_core_set_source(EpCore* c, int pad, const EpSource* src);
int ep_core_gen_in_use(const EpCore* c, int pad, uint32_t gen);

void ep_core_trigger(EpCore* c, int pad, int vel);
void ep_core_trigger_ex(EpCore* c, int pad, int vel, float pitch_off, float filt_off, int depth);
void ep_core_trigger_16level(EpCore* c, int sel_pad, int slot, int vel,
                             int* out_vel, float* out_pitch, float* out_filt);
void ep_core_hold_start(EpCore* c, int slot, int sound_pad, int vel, float pitch_off, float filt_off);
void ep_core_hold_stop(EpCore* c, int slot);
void ep_core_release(EpCore* c, int pad);
void ep_core_all_off(EpCore* c);

/* Sequencer control */
void ep_seq_set_mode(EpCore* c, int mode);
void ep_seq_set_len(EpCore* c, int steps);
void ep_seq_set_pad_range(EpCore* c, int scene, int pad, int start_step, int end_step);
void ep_seq_set_quantize(EpCore* c, int q);
void ep_seq_request_scene(EpCore* c, int scene);
void ep_seq_request_fill(EpCore* c, int fill_scene);
void ep_seq_sync_host(EpCore* c, double bpm, int playing, double bar_beat, int bpb);
void ep_seq_record_hit(EpCore* c, int pad, int vel);
void ep_seq_record_hit_ex(EpCore* c, int pad, int vel, int pitch_off, int filt_off);
void ep_seq_save_undo(EpCore* c, int scene);
int  ep_seq_undo(EpCore* c);
void ep_seq_clear_pad(EpCore* c, int scene, int pad);
void ep_seq_clear_scene(EpCore* c, int scene);
void ep_seq_copy_scene(EpCore* c, int src_scene, int dst_scene);
void ep_seq_rotate_pad(EpCore* c, int scene, int pad, int dir);
void ep_seq_euclid_pad(EpCore* c, int scene, int pad, int hits, int rot);

/* Render `nframes` stereo audio (+ optional Aux 1..4 stereo send buses). */
void ep_core_process_aux(EpCore* c, float* out_l, float* out_r,
                         float* const aux_l[EP_AUX_BUSES],
                         float* const aux_r[EP_AUX_BUSES],
                         uint32_t nframes, int sel_pad);
void ep_core_process(EpCore* c, float* out_l, float* out_r, uint32_t nframes, int sel_pad);

#endif
