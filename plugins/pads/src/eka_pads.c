/* Eka Pads: 16-pad drum sampler + 8-scene step sequencer LV2 plugin for Zynthian. */
#include <lv2/core/lv2.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/forge.h>
#include <lv2/atom/util.h>
#include <lv2/midi/midi.h>
#include <lv2/patch/patch.h>
#include <lv2/state/state.h>
#include <lv2/time/time.h>
#include <lv2/urid/urid.h>
#include <lv2/worker/worker.h>

#include <fcntl.h>
#include <math.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "pads_core.h"
#include "sample.h"

#define EP_URI "http://dmtry3.github.io/zynthian-eka/pads"
#define EP_MAX_SECONDS 120.0
#define EP_RETIRED_MAX 32
#define EP_SHM_MAGIC   0x45504144u /* "EPAD" */
#define EP_SHM_VER     12u

/* Control input ports MUST come first (contiguous 0..31) so Zynthian's synthetic
 * patch:writable parameter indices never collide with Input ControlPorts! */
enum {
  P_PITCH = 0, P_DECAY, P_FILTER, P_LEVEL,        /* Page 1: Pad Sound */
  P_START, P_MODE, P_PUNCH, P_DRIVE,              /* Page 2: Pad Shape */
  P_PAN, P_VEL, P_CHOKE, P_WIDTH,                 /* Page 3: Pad Mix */
  P_SEQ_MODE, P_SEQ_SCENE, P_SEQ_SWING, P_MASTER, /* Page 4: Seq & Master */
  P_END,
  P_PAD_AUX1 = P_END,                             /* index 16: Selected pad Aux 1 send 0..100% */
  P_PAD_AUX2,                                     /* index 17: Selected pad Aux 2 send 0..100% */
  P_PAD_AUX3,                                     /* index 18: Selected pad Aux 3 send 0..100% */
  P_PAD_AUX4,                                     /* index 19: Selected pad Aux 4 send 0..100% */
  P_STEREO_WIDTH,                                 /* index 20: Stereo Width 0..200% */
  P_PAD_STEPS,                                    /* index 21: End / Steps 1..32 for selected pad */
  P_PAD_BEG,                                      /* index 22: Begin step 1..32 for selected pad */
  P_PAD_LFO1_RATE,                                /* index 23: Selected pad LFO 1 Rate 0.05..20 Hz */
  P_PAD_LFO2_RATE,                                /* index 24: Selected pad LFO 2 Rate 0.05..20 Hz */
  P_PAD_MOD1_AMT,                                 /* index 25: Selected pad Mod 1 Amount -100..+100% */
  P_PAD_MOD2_AMT,                                 /* index 26: Selected pad Mod 2 Amount -100..+100% */
  P_PAD_NOTE1,                                    /* index 27: Selected row col 1 MIDI note 0..127 */
  P_PAD_NOTE2,                                    /* index 28: Selected row col 2 MIDI note 0..127 */
  P_PAD_NOTE3,                                    /* index 29: Selected row col 3 MIDI note 0..127 */
  P_PAD_NOTE4,                                    /* index 30: Selected row col 4 MIDI note 0..127 */
  P_BUS_GLUE,                                     /* index 31: Master Bus Compressor / Glue 0..100% */
  P_MIDI_IN,                                      /* index 32 */
  P_OUT_L,                                        /* index 33 */
  P_OUT_R,                                        /* index 34 */
  P_AUX1_L,                                       /* index 35 */
  P_AUX1_R,                                       /* index 36 */
  P_AUX2_L,                                       /* index 37 */
  P_AUX2_R,                                       /* index 38 */
  P_AUX3_L,                                       /* index 39 */
  P_AUX3_R,                                       /* index 40 */
  P_AUX4_L,                                       /* index 41 */
  P_AUX4_R,                                       /* index 42 */
  P_NOTIFY,                                       /* index 43 */
  P_OUT_SEL_PAD,                                  /* index 44 */
  P_OUT_SEQ_STEP,                                 /* index 45 */
  P_OUT_SEQ_SCENE,                                /* index 46 */
  P_OUT_SEQ_PEND,                                 /* index 47 */
  P_OUT_LVL0,                                     /* index 48..63 (16 ports) */
  P_OUT_PLAYHEAD = P_OUT_LVL0 + EP_PADS           /* index 64 */
};

#define N_CTL     P_END
#define N_PAD_CTL 12

/* Packed seq_cmd (24-bit, exact in 32-bit float): (op << 20) | (a << 17) | (b << 13) | (c << 8) | val */
enum {
  CMD_SET_VEL = 1,    /* a=scene, b=pad, c=step(0..31), val=vel(0..127) */
  CMD_SET_ROLL,       /* a=scene, b=pad, c=step(0..31), val=roll(0..3) */
  CMD_CLEAR_PAD,      /* a=scene, b=pad */
  CMD_CLEAR_SCENE,    /* a=scene */
  CMD_COPY_SCENE,     /* a=src_scene, b=dst_scene */
  CMD_TRIG_PAD,       /* b=pad, val=vel(1..127) */
  CMD_SET_START,      /* a=scene, b=pad, val=start_step(0..31) */
  CMD_SET_END,        /* a=scene, b=pad, val=end_step(1..32) */
  CMD_SET_QUANT,      /* val=quantize(0..100%) */
  CMD_SET_RETRIG,     /* val=retrig_ms(0..80) */
  CMD_SET_MINVEL,     /* val=min_vel(0..40) */
  CMD_SET_XTALK,      /* val=xtalk(0..60%) */
  CMD_SET_LFO,        /* a=lfo(0..1), b=pad(0..15), c=0(wave)|1(rate_log 0..200), val */
  CMD_SET_MOD_SD,     /* a=slot(0..3), b=pad(0..15), c=src(0..5), val=dst(0..10) */
  CMD_SET_MOD_AMT     /* a=slot(0..3), b=pad(0..15), val=amt+100 (0..200) */
};

/* Shared memory snapshot read by the Zynthian Python widget */
typedef struct {
  uint32_t magic;
  uint32_t version;
  uint32_t seq;         /* incremented whenever pads/scenes/meta change */
  int32_t  sel_pad;
  int32_t  follow;
  int32_t  learn;
  int32_t  seq_mode;
  int32_t  seq_scene;
  int32_t  seq_pend;
  int32_t  seq_step;
  float    bpm;
  float    swing;
  int32_t  seq_quant;   /* 0..100 % */
  uint32_t hit_seq;     /* incremented on every MIDI pad trigger */
  int32_t  last_hit_pad;/* 0..15 pad index of latest MIDI hit */
  int32_t  retrig_ms;   /* 0..80 ms same-pad retrigger filter */
  int32_t  min_vel;     /* 0..40 min velocity threshold */
  int32_t  xtalk;       /* 0..60 % crosstalk suppression */
  /* v11 header fields (+48 bytes -> 120 bytes total) */
  int32_t  play_mode;   /* 0=Normal, 1=Chrom, 2=16Vel, 3=16Flt */
  int32_t  repeat_div;  /* 0=Off, 1=1/8, 2=1/16, 3=1/16T, 4=1/32 */
  int32_t  metro_mode;  /* 0=Off, 1=Rec, 2=On */
  int32_t  countin_en;  /* 0=Off, 1=1 Bar */
  int32_t  countin_rem; /* 0..16 remaining count-in steps */
  int32_t  undo_avail;  /* 0 or 1 */
  int32_t  fill_ret;    /* -1 or 0..7 */
  int32_t  chain_on;    /* 0 or 1 */
  int32_t  chain_len;   /* 1..8 */
  int32_t  chain_pos;   /* 0..7 */
  float    bus_glue;    /* 0..100 % */
  uint32_t chain_packed;/* 8 x 4-bit nibbles for chain[0..7] */
  /* v12 header fields (+24 bytes -> 144 bytes total) */
  int32_t  choke_mute;  /* bits 0..3: Choke Grp 1..4 Mute */
  int32_t  choke_solo;  /* bits 0..3: Choke Grp 1..4 Solo */
  float    perf_filter; /* -100..+100 % master DJ filter */
  float    perf_reso;   /* 0..100 % master DJ filter reso */
  int32_t  perf_fx_flags;/* bits 0..3: perf_fx, bit 4: break, bit 5: aux1_throw */
  float    bar_phase;   /* 0..16 in 16th-note steps */
  EpPadParams pads[EP_PADS];
  EpScene  scenes[EP_SCENES];
} EpShm;
_Static_assert(sizeof(EpPadParams) == 160, "EpPadParams size must match Python widget");
_Static_assert(sizeof(EpScene) == 2592, "EpScene size must match Python widget");
_Static_assert(sizeof(EpShm) == 23440, "EpShm size must match Python widget");

/* Worker messages */
enum { MSG_LOAD = 1, MSG_FREE };
typedef struct { uint32_t type; int pad; uint32_t seq; char path[EP_PATH_MAX]; } MsgLoad;
typedef struct { uint32_t type; EpSample* s; } MsgFree;
typedef struct { int pad; uint32_t seq; int err; EpSample* s; } MsgDone;

typedef struct {
  const float* ctl[N_CTL];
  const LV2_Atom_Sequence* in;
  LV2_Atom_Sequence* notify;
  float* out_l;
  float* out_r;
  float* aux_l[EP_AUX_BUSES];
  float* aux_r[EP_AUX_BUSES];
  float* out_sel_pad;
  float* out_seq_step;
  float* out_seq_scene;
  float* out_seq_pend;
  float* out_lvl[EP_PADS];
  float* out_playhead;
  const float* in_stereo_width;
  const float* in_pad_steps;
  const float* in_pad_beg;
  const float* in_pad_aux[EP_AUX_BUSES];
  const float* in_pad_lfo_rate[EP_LFOS];
  const float* in_pad_mod_amt[2];
  const float* in_pad_note[4];
  const float* in_bus_glue;
  float prev_pad_steps;
  float prev_pad_beg;
  float prev_pad_aux[EP_AUX_BUSES];
  float prev_pad_lfo_rate[EP_LFOS];
  float prev_pad_mod_amt[2];
  float prev_pad_note[4];
  int have_prev_steps;

  LV2_URID_Map* map;
  LV2_Worker_Schedule* sched;
  LV2_Atom_Forge forge;

  LV2_URID midi_Event, atom_Object, atom_Path, atom_String, atom_URID;
  LV2_URID atom_Int, atom_Float, atom_Double, atom_Long, atom_Chunk;
  LV2_URID patch_Set, patch_Get, patch_property, patch_value;
  LV2_URID time_Position, time_bpm, time_speed, time_bar, time_barBeat, time_bpb;

  LV2_URID p_sample[EP_PADS];
  LV2_URID p_sel_pad, p_follow, p_learn, p_mute_mask, p_solo_mask, p_pad_end;
  LV2_URID p_seq_bpm, p_seq_cmd, p_seq_len, p_seq_data, p_aux_routes;
  LV2_URID st_pad_params, st_scenes;

  EpSample* samples[EP_PADS];
  char paths[EP_PADS][EP_PATH_MAX];
  atomic_uint req_seq[EP_PADS];
  uint16_t pending_load_mask;
  uint32_t pending_load_seq[EP_PADS];

  struct { EpSample* s; int pad; uint32_t gen; } retired[EP_RETIRED_MAX];
  int nretired;

  int sel_pad;
  int follow;
  int learn;
  uint32_t hit_seq;
  int32_t last_hit_pad;
  int report_next;
  int seq_notify_dirty;
  uint32_t seq_notify_cd;
  char seq_buf[8192];
  char aux_routes[256];

  float prev_ctl[N_CTL];
  int have_prev_ctl;

  /* Shared memory for widget */
  char shm_path[128];
  int shm_fd;
  EpShm* shm;
  int shm_dirty;

  double sr;
  uint64_t sample_clock;
  uint64_t last_pad_hit_sample[EP_PADS];
  uint64_t last_any_hit_sample;
  int last_any_hit_pad;
  int last_any_hit_vel;
  EpCore core;
} EkaPads;

static void shm_publish(EkaPads* P) {
  if (!P->shm) return;
  EpShm* s = P->shm;
  s->sel_pad     = P->sel_pad;
  s->follow      = P->follow;
  s->learn       = P->learn;
  s->seq_mode    = P->core.seq.mode;
  s->seq_scene   = P->core.seq.scene;
  s->seq_pend    = P->core.seq.pend_scene;
  s->seq_step    = P->core.seq.cur_step;
  s->bpm         = (float)P->core.seq.bpm;
  s->swing       = P->core.seq.swing;
  s->seq_quant   = P->core.seq.quantize;
  s->hit_seq     = P->hit_seq;
  s->last_hit_pad = P->last_hit_pad;
  s->retrig_ms   = P->core.retrig_ms;
  s->min_vel     = P->core.min_vel;
  s->xtalk       = P->core.xtalk;
  s->play_mode   = P->core.play_mode;
  s->repeat_div  = P->core.repeat_div;
  s->metro_mode  = P->core.seq.metro_mode;
  s->countin_en  = P->core.seq.countin_en;
  s->countin_rem = P->core.seq.countin_rem;
  s->undo_avail  = P->core.seq.undo_scene_idx >= 0 ? 1 : 0;
  s->fill_ret    = P->core.seq.fill_ret_scene;
  s->chain_on    = P->core.seq.chain_on;
  s->chain_len   = P->core.seq.chain_len;
  s->chain_pos   = P->core.seq.chain_pos;
  s->bus_glue    = P->core.bus_glue;
  uint32_t cp = 0;
  for (int i = 0; i < EP_CHAIN_MAX; i++) {
    cp |= ((uint32_t)(P->core.seq.chain[i] & 0xF)) << (i * 4);
  }
  s->chain_packed = cp;
  s->choke_mute  = (int32_t)P->core.choke_mute_mask;
  s->choke_solo  = (int32_t)P->core.choke_solo_mask;
  s->perf_filter = P->core.perf_filter;
  s->perf_reso   = P->core.perf_reso;
  s->perf_fx_flags = (P->core.perf_fx & 0xF) |
                     (P->core.perf_break ? 0x10 : 0) |
                     (P->core.perf_aux1_throw ? 0x20 : 0);
  s->bar_phase   = (float)P->core.seq.bar_phase;
  if (P->shm_dirty) {
    memcpy(s->pads, P->core.pad, sizeof P->core.pad);
    memcpy(s->scenes, P->core.scenes, sizeof P->core.scenes);
    P->shm_dirty = 0;
    s->seq++;
  }
}

static int free_in_worker(EkaPads* P, EpSample* s) {
  if (!s) return 1;
  if (P->sched) {
    MsgFree fm = { MSG_FREE, s };
    return P->sched->schedule_work(P->sched->handle, sizeof fm, &fm) == LV2_WORKER_SUCCESS;
  } else {
    ep_sample_free(s);
    return 1;
  }
}

static void swap_sample(EkaPads* P, int pad, EpSample* s) {
  if (pad < 0 || pad >= EP_PADS) return;
  EpSample* old = P->samples[pad];
  EpSource src = { 0 };
  if (s) {
    src.data = s->data;
    src.frames = s->frames;
    src.rate = (float)s->rate;
  }
  uint32_t old_gen = ep_core_set_source(&P->core, pad, s ? &src : NULL);
  P->samples[pad] = s;
  if (!old) return;

  if (!ep_core_gen_in_use(&P->core, pad, old_gen) && free_in_worker(P, old)) {
    return;
  }
  if (P->nretired >= EP_RETIRED_MAX) {
    ep_core_all_off(&P->core);
    int w = 0;
    for (int i = 0; i < P->nretired; i++) {
      if (!free_in_worker(P, P->retired[i].s)) {
        P->retired[w++] = P->retired[i];
      }
    }
    P->nretired = w;
    if (P->nretired >= EP_RETIRED_MAX) {
      ep_sample_free(old);
      return;
    }
  }
  P->retired[P->nretired].s = old;
  P->retired[P->nretired].pad = pad;
  P->retired[P->nretired].gen = old_gen;
  P->nretired++;
}

static void collect_retired(EkaPads* P) {
  int w = 0;
  for (int i = 0; i < P->nretired; i++) {
    if (!ep_core_gen_in_use(&P->core, P->retired[i].pad, P->retired[i].gen) &&
        free_in_worker(P, P->retired[i].s)) {
      continue;
    }
    P->retired[w++] = P->retired[i];
  }
  P->nretired = w;
}

static void pump_pending_loads(EkaPads* P) {
  if (!P->sched || !P->pending_load_mask) return;
  for (int pad = 0; pad < EP_PADS; pad++) {
    if (!(P->pending_load_mask & (1u << pad))) continue;
    const char* path = P->paths[pad];
    size_t len = strlen(path);
    if (len == 0 || len >= EP_PATH_MAX) {
      P->pending_load_mask &= (uint16_t)~(1u << pad);
      continue;
    }
    MsgLoad m;
    m.type = MSG_LOAD;
    m.pad = pad;
    m.seq = P->pending_load_seq[pad];
    memcpy(m.path, path, len + 1);
    uint32_t msz = (uint32_t)(offsetof(MsgLoad, path) + len + 1);
    if (P->sched->schedule_work(P->sched->handle, msz, &m) == LV2_WORKER_SUCCESS) {
      P->pending_load_mask &= (uint16_t)~(1u << pad);
    } else {
      /* Host worker ringbuffer is full right now; keep pending bit set and retry next block */
      break;
    }
  }
}

static void request_load(EkaPads* P, int pad, const char* path) {
  if (pad < 0 || pad >= EP_PADS) return;
  if (!path || !*path || strcmp(path, "_") == 0) {
    P->paths[pad][0] = '\0';
    P->pending_load_mask &= (uint16_t)~(1u << pad);
    atomic_fetch_add_explicit(&P->req_seq[pad], 1, memory_order_relaxed);
    swap_sample(P, pad, NULL);
    return;
  }
  size_t len = strlen(path);
  if (len >= EP_PATH_MAX) return;
  /* Skip redundant reload if this exact sample is already loaded and idle */
  if (P->samples[pad] != NULL && !(P->pending_load_mask & (1u << pad)) &&
      strcmp(P->paths[pad], path) == 0) {
    return;
  }
  memcpy(P->paths[pad], path, len + 1);
  uint32_t seq = atomic_fetch_add_explicit(&P->req_seq[pad], 1, memory_order_relaxed) + 1;
  P->pending_load_seq[pad] = seq;
  P->pending_load_mask |= (uint16_t)(1u << pad);
  pump_pending_loads(P);
}

static int get_mute_mask(const EkaPads* P) {
  int m = 0;
  for (int i = 0; i < EP_PADS; i++) if (P->core.pad[i].mute) m |= (1 << i);
  return m;
}

static int get_solo_mask(const EkaPads* P) {
  int m = 0;
  for (int i = 0; i < EP_PADS; i++) if (P->core.pad[i].solo) m |= (1 << i);
  return m;
}

static int atom_to_int(const EkaPads* P, const LV2_Atom* a, int* out) {
  if (!a) return 0;
  if (a->type == P->atom_Int) { *out = ((const LV2_Atom_Int*)a)->body; return 1; }
  if (a->type == P->atom_Long) { *out = (int)((const LV2_Atom_Long*)a)->body; return 1; }
  if (a->type == P->atom_Float) { *out = (int)lrintf(((const LV2_Atom_Float*)a)->body); return 1; }
  if (a->type == P->atom_Double) { *out = (int)llround(((const LV2_Atom_Double*)a)->body); return 1; }
  return 0;
}

static int atom_to_double(const EkaPads* P, const LV2_Atom* a, double* out) {
  if (!a) return 0;
  if (a->type == P->atom_Float) { *out = (double)((const LV2_Atom_Float*)a)->body; return 1; }
  if (a->type == P->atom_Double) { *out = ((const LV2_Atom_Double*)a)->body; return 1; }
  if (a->type == P->atom_Int) { *out = (double)((const LV2_Atom_Int*)a)->body; return 1; }
  if (a->type == P->atom_Long) { *out = (double)((const LV2_Atom_Long*)a)->body; return 1; }
  return 0;
}

/* Compact ASCII serializer for sequencer scenes + per-pad Beg/End + Quantize (saved in Zynthian snapshots). */
static size_t seq_encode(const EpCore* c, char* buf, size_t cap) {
  if (!buf || cap < 16) return 0;
  size_t pos = 0;
  int n = snprintf(buf + pos, cap - pos, "V1,%d", c->seq.quantize);
  if (n < 0 || (size_t)n >= cap - pos) { buf[0] = '\0'; return 0; }
  pos += (size_t)n;

  for (int s = 0; s < EP_SCENES; s++) {
    const EpScene* sc = &c->scenes[s];
    for (int p = 0; p < EP_PADS; p++) {
      int ps = sc->pad_start[p];
      int pe = sc->pad_end[p] ? sc->pad_end[p] : EP_STEPS_DEF;
      int has = (ps != 0 || pe != EP_STEPS_DEF);
      if (!has) {
        for (int st = 0; st < EP_STEPS; st++) {
          if (sc->steps[p][st].vel > 0) { has = 1; break; }
        }
      }
      if (!has) continue;
      n = snprintf(buf + pos, cap - pos, ";%x%x:%d,%d", s & 0xF, p & 0xF, ps, pe);
      if (n < 0 || (size_t)n >= cap - pos) return pos;
      pos += (size_t)n;
      for (int st = 0; st < EP_STEPS; st++) {
        EpStep step = sc->steps[p][st];
        if (step.vel == 0) continue;
        int ch = (step.chance == 0 || step.chance > 100) ? 100 : (int)step.chance;
        if (step.pitch_off != 0 || step.filt_off != 0) {
          n = snprintf(buf + pos, cap - pos, ":%d.%d.%d.%d.%d.%d",
                       st, (int)step.vel, (int)step.roll, ch, (int)step.pitch_off, (int)step.filt_off);
        } else if (ch < 100) {
          n = snprintf(buf + pos, cap - pos, ":%d.%d.%d.%d", st, (int)step.vel, (int)step.roll, ch);
        } else if (step.roll > 0) {
          n = snprintf(buf + pos, cap - pos, ":%d.%d.%d", st, (int)step.vel, (int)step.roll);
        } else {
          n = snprintf(buf + pos, cap - pos, ":%d.%d", st, (int)step.vel);
        }
        if (n < 0 || (size_t)n >= cap - pos) return pos;
        pos += (size_t)n;
      }
    }
  }
  /* Append non-default per-pad parameters so Zynthian snapshots persist the full 16-pad kit mix */
  for (int p = 0; p < EP_PADS; p++) {
    EpPadParams def;
    ep_pad_defaults(&def, p);
    const EpPadParams* cur = &c->pad[p];
    int diff = (fabsf(cur->pitch - def.pitch) > 0.05f ||
                fabsf(cur->decay - def.decay) > 0.2f ||
                fabsf(cur->filter - def.filter) > 0.2f ||
                fabsf(cur->level - def.level) > 0.2f ||
                fabsf(cur->start - def.start) > 0.2f ||
                fabsf(cur->end - def.end) > 0.2f ||
                cur->mode != def.mode ||
                fabsf(cur->punch - def.punch) > 0.2f ||
                fabsf(cur->drive - def.drive) > 0.2f ||
                fabsf(cur->pan - def.pan) > 0.2f ||
                fabsf(cur->vel_sens - def.vel_sens) > 0.2f ||
                cur->choke != def.choke ||
                fabsf(cur->width - def.width) > 0.2f ||
                cur->mute != def.mute ||
                cur->solo != def.solo ||
                cur->midi_note != def.midi_note ||
                fabsf(cur->aux[0] - def.aux[0]) > 0.2f ||
                fabsf(cur->aux[1] - def.aux[1]) > 0.2f ||
                fabsf(cur->aux[2] - def.aux[2]) > 0.2f ||
                fabsf(cur->aux[3] - def.aux[3]) > 0.2f ||
                fabsf(cur->attack - def.attack) > 0.2f ||
                fabsf(cur->reso - def.reso) > 0.2f ||
                cur->link_pad != def.link_pad ||
                cur->link_mode != def.link_mode);
    if (!diff) continue;
    n = snprintf(buf + pos, cap - pos,
                 "|P%x:%.1f,%.1f,%.1f,%.1f,%.2f,%.2f,%d,%.1f,%.1f,%.1f,%.1f,%d,%.1f,%d,%d,%d,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%d,%d",
                 p & 0xF, cur->pitch, cur->decay, cur->filter, cur->level,
                 cur->start, cur->end, cur->mode, cur->punch, cur->drive,
                 cur->pan, cur->vel_sens, cur->choke, cur->width,
                 cur->mute, cur->solo, cur->midi_note,
                 cur->aux[0], cur->aux[1], cur->aux[2], cur->aux[3],
                 cur->attack, cur->reso, cur->link_pad, cur->link_mode);
    if (n < 0 || (size_t)n >= cap - pos) return pos;
    pos += (size_t)n;
  }
  if (c->retrig_ms != 25 || c->min_vel != 0 || c->xtalk != 0) {
    n = snprintf(buf + pos, cap - pos, "|F:%d,%d,%d", c->retrig_ms, c->min_vel, c->xtalk);
    if (n > 0 && (size_t)n < cap - pos) pos += (size_t)n;
  }
  if (c->seq.metro_mode != 0 || c->seq.countin_en != 0 || c->seq.chain_on != 0 || c->seq.chain_len != 4) {
    n = snprintf(buf + pos, cap - pos, "|E:%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d",
                 c->seq.metro_mode, c->seq.countin_en, c->seq.chain_on, c->seq.chain_len,
                 (int)c->seq.chain[0], (int)c->seq.chain[1], (int)c->seq.chain[2], (int)c->seq.chain[3],
                 (int)c->seq.chain[4], (int)c->seq.chain[5], (int)c->seq.chain[6], (int)c->seq.chain[7]);
    if (n > 0 && (size_t)n < cap - pos) pos += (size_t)n;
  }
  for (int p = 0; p < EP_PADS; p++) {
    EpPadParams def;
    ep_pad_defaults(&def, p);
    const EpPadParams* cur = &c->pad[p];
    int mdiff = (cur->lfo_wave[0] != def.lfo_wave[0] || fabsf(cur->lfo_rate[0] - def.lfo_rate[0]) > 0.02f ||
                 cur->lfo_wave[1] != def.lfo_wave[1] || fabsf(cur->lfo_rate[1] - def.lfo_rate[1]) > 0.02f);
    for (int s = 0; s < EP_MOD_SLOTS && !mdiff; s++) {
      if (cur->mod_src[s] != 0 || cur->mod_dst[s] != 0 || fabsf(cur->mod_amt[s]) > 0.2f) mdiff = 1;
    }
    if (!mdiff) continue;
    n = snprintf(buf + pos, cap - pos,
                 "|M%x:%d,%.2f,%d,%.2f,%d,%d,%.0f,%d,%d,%.0f,%d,%d,%.0f,%d,%d,%.0f",
                 p & 0xF, cur->lfo_wave[0], cur->lfo_rate[0], cur->lfo_wave[1], cur->lfo_rate[1],
                 cur->mod_src[0], cur->mod_dst[0], cur->mod_amt[0],
                 cur->mod_src[1], cur->mod_dst[1], cur->mod_amt[1],
                 cur->mod_src[2], cur->mod_dst[2], cur->mod_amt[2],
                 cur->mod_src[3], cur->mod_dst[3], cur->mod_amt[3]);
    if (n > 0 && (size_t)n < cap - pos) pos += (size_t)n;
  }
  return pos;
}

static int hex_nibble(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return 10 + (ch - 'a');
  if (ch >= 'A' && ch <= 'F') return 10 + (ch - 'A');
  return -1;
}

static void seq_decode(EpCore* c, const char* str) {
  if (!str) return;
  /* Legacy presets saved seq_data as atom:Path: lilv could prepend a bundle dir and percent-encode '|' as "%7C". */
  char clean[8192];
  const char* start = str;
  if (!(start[0] == 'V' && start[1] == '1')) {
    const char* found = strstr(str, "V1,");
    if (!found) found = strstr(str, "V1;");
    if (!found) found = strstr(str, "V1|");
    if (!found) found = strstr(str, "V1%7C");
    if (!found) found = strstr(str, "V1%7c");
    if (!found) return;
    start = found;
  }
  if (strstr(start, "%7C") || strstr(start, "%7c")) {
    size_t w = 0;
    for (const char* q = start; *q && w + 1 < sizeof clean; ) {
      if (q[0] == '%' && q[1] == '7' && (q[2] == 'C' || q[2] == 'c')) {
        clean[w++] = '|';
        q += 3;
      } else {
        clean[w++] = *q++;
      }
    }
    clean[w] = '\0';
    start = clean;
  }
  str = start;
  if (str[0] != 'V' || str[1] != '1') return;
  for (int s = 0; s < EP_SCENES; s++) ep_seq_clear_scene(c, s);
  for (int i = 0; i < EP_PADS; i++) ep_pad_defaults(&c->pad[i], i);
  c->retrig_ms = 25;
  c->min_vel = 0;
  c->xtalk = 0;

  const char* p = str + 2;
  if (*p == ',') {
    p++;
    int q = (int)strtol(p, (char**)&p, 10);
    ep_seq_set_quantize(c, q);
  }
  while (*p && *p != '|') {
    if (*p == ';') p++;
    if (!*p || *p == '|') break;
    int sc = hex_nibble(p[0]);
    int pad = p[1] ? hex_nibble(p[1]) : -1;
    if (sc < 0 || sc >= EP_SCENES || pad < 0 || pad >= EP_PADS || p[2] != ':') {
      while (*p && *p != ';' && *p != '|') p++;
      continue;
    }
    p += 3;
    int ps = (int)strtol(p, (char**)&p, 10);
    int pe = EP_STEPS_DEF;
    if (*p == ',') {
      p++;
      pe = (int)strtol(p, (char**)&p, 10);
    }
    ep_seq_set_pad_range(c, sc, pad, ps, pe);
    while (*p == ':') {
      p++;
      int st = (int)strtol(p, (char**)&p, 10);
      int vel = 0, roll = 0, chance = 0, po = 0, fo = 0;
      if (*p == '.') { p++; vel = (int)strtol(p, (char**)&p, 10); }
      if (*p == '.') { p++; roll = (int)strtol(p, (char**)&p, 10); }
      if (*p == '.') { p++; chance = (int)strtol(p, (char**)&p, 10); }
      if (*p == '.') { p++; po = (int)strtol(p, (char**)&p, 10); }
      if (*p == '.') { p++; fo = (int)strtol(p, (char**)&p, 10); }
      if (st >= 0 && st < EP_STEPS && vel > 0) {
        c->scenes[sc].steps[pad][st].vel = (uint8_t)(vel > 127 ? 127 : vel);
        c->scenes[sc].steps[pad][st].roll = (uint8_t)(roll & 0xFF);
        c->scenes[sc].steps[pad][st].chance = (uint8_t)((chance > 0 && chance < 100) ? chance : 0);
        if (po < -24) po = -24;
        if (po > 24) po = 24;
        if (fo < -100) fo = -100;
        if (fo > 100) fo = 100;
        c->scenes[sc].steps[pad][st].pitch_off = (int8_t)po;
        c->scenes[sc].steps[pad][st].filt_off = (int8_t)fo;
      }
    }
  }
  /* Parse optional |P<hex>:... / |F:... / |E:... / |M<hex>:... (also accepts legacy '=' separator) */
  while (*p == '|') {
    p++;
    if (p[0] == 'P' && p[1] && (p[2] == ':' || p[2] == '=')) {
      int pad = hex_nibble(p[1]);
      p += 3;
      if (pad >= 0 && pad < EP_PADS) {
        EpPadParams* d = &c->pad[pad];
        float v[24];
        int cnt = 0;
        while (cnt < 24 && *p && *p != '|') {
          v[cnt++] = strtof(p, (char**)&p);
          if (*p == ',') p++;
        }
        if (cnt >= 16) {
          d->pitch = v[0]; d->decay = v[1]; d->filter = v[2]; d->level = v[3];
          d->start = v[4]; d->end = v[5]; d->mode = (int)lrintf(v[6]);
          d->punch = v[7]; d->drive = v[8]; d->pan = v[9]; d->vel_sens = v[10];
          d->choke = (int)lrintf(v[11]); d->width = v[12];
          d->mute = v[13] >= 0.5f ? 1 : 0; d->solo = v[14] >= 0.5f ? 1 : 0;
          int mn = (int)lrintf(v[15]);
          if (mn >= 0 && mn <= 127) d->midi_note = mn;
          for (int a = 0; a < EP_AUX_BUSES; a++) {
            d->aux[a] = (cnt > 16 + a) ? v[16 + a] : 0.0f;
          }
          if (cnt > 20) d->attack = v[20] < 0.0f ? 0.0f : (v[20] > 100.0f ? 100.0f : v[20]);
          if (cnt > 21) d->reso   = v[21] < 0.0f ? 0.0f : (v[21] > 100.0f ? 100.0f : v[21]);
          if (cnt > 22) {
            int lp = (int)lrintf(v[22]);
            d->link_pad = (lp >= 0 && lp <= EP_PADS) ? lp : 0;
          }
          if (cnt > 23) d->link_mode = ((int)lrintf(v[23])) ? EP_LINK_RR : EP_LINK_LAYER;
        }
      }
    } else if (p[0] == 'E' && (p[1] == ':' || p[1] == '=')) {
      p += 2;
      int ev[12] = { 0 };
      int cnt = 0;
      while (cnt < 12 && *p && *p != '|') {
        ev[cnt++] = (int)strtol(p, (char**)&p, 10);
        if (*p == ',') p++;
      }
      if (cnt >= 1) c->seq.metro_mode = (ev[0] >= 0 && ev[0] <= 2) ? ev[0] : 0;
      if (cnt >= 2) c->seq.countin_en = ev[1] ? 1 : 0;
      if (cnt >= 3) c->seq.chain_on   = ev[2] ? 1 : 0;
      if (cnt >= 4) c->seq.chain_len  = ev[3] < 1 ? 1 : (ev[3] > EP_CHAIN_MAX ? EP_CHAIN_MAX : ev[3]);
      for (int k = 0; k < EP_CHAIN_MAX && 4 + k < cnt; k++) {
        c->seq.chain[k] = (uint8_t)(ev[4 + k] & 7);
      }
    } else if (p[0] == 'F' && (p[1] == ':' || p[1] == '=')) {
      p += 2;
      int fv[3] = { 25, 0, 0 };
      int cnt = 0;
      while (cnt < 3 && *p && *p != '|') {
        fv[cnt++] = (int)strtol(p, (char**)&p, 10);
        if (*p == ',') p++;
      }
      if (cnt >= 1) c->retrig_ms = fv[0] < 0 ? 0 : (fv[0] > 80 ? 80 : fv[0]);
      if (cnt >= 2) c->min_vel   = fv[1] < 0 ? 0 : (fv[1] > 40 ? 40 : fv[1]);
      if (cnt >= 3) c->xtalk     = fv[2] < 0 ? 0 : (fv[2] > 60 ? 60 : fv[2]);
    } else if (p[0] == 'M' && p[1] && (p[2] == ':' || p[2] == '=')) {
      int pad = hex_nibble(p[1]);
      p += 3;
      if (pad >= 0 && pad < EP_PADS) {
        EpPadParams* d = &c->pad[pad];
        float mv[16];
        int cnt = 0;
        while (cnt < 16 && *p && *p != '|') {
          mv[cnt++] = strtof(p, (char**)&p);
          if (*p == ',') p++;
        }
        if (cnt >= 16) {
          int w0 = (int)lrintf(mv[0]);
          int w1 = (int)lrintf(mv[2]);
          d->lfo_wave[0] = ((w0 & 7) % EP_LFO_NWAVES) | (w0 & 16);
          d->lfo_rate[0] = mv[1] < 0.05f ? 0.05f : (mv[1] > 20.0f ? 20.0f : mv[1]);
          d->lfo_wave[1] = ((w1 & 7) % EP_LFO_NWAVES) | (w1 & 16);
          d->lfo_rate[1] = mv[3] < 0.05f ? 0.05f : (mv[3] > 20.0f ? 20.0f : mv[3]);
          for (int s = 0; s < EP_MOD_SLOTS; s++) {
            int src = (int)lrintf(mv[4 + s * 3]);
            int dst = (int)lrintf(mv[5 + s * 3]);
            float amt = mv[6 + s * 3];
            d->mod_src[s] = (src >= 0 && src < EP_MSRC_COUNT) ? src : 0;
            d->mod_dst[s] = (dst >= 0 && dst < EP_MDST_COUNT) ? dst : 0;
            d->mod_amt[s] = amt < -100.0f ? -100.0f : (amt > 100.0f ? 100.0f : amt);
          }
        }
      }
    }
    while (*p && *p != '|') p++;
  }
}

static void handle_seq_cmd(EkaPads* P, int cmd) {
  int op  = (cmd >> 20) & 0xF;
  int a   = (cmd >> 17) & 0x7;
  int b   = (cmd >> 13) & 0xF;
  int c   = (cmd >> 8)  & 0x1F;
  int val = cmd & 0xFF;

  switch (op) {
    case CMD_SET_VEL:
      if (a < EP_SCENES && b < EP_PADS && c < EP_STEPS) {
        if (val < 128) {
          P->core.scenes[a].steps[b][c].vel = (uint8_t)val;
        } else if (val < 192) {
          /* 0x80..0xBF: p-lock pitch offset (-24..+24 st) */
          int po = (val & 0x3F) - 24;
          if (po < -24) po = -24;
          if (po > 24) po = 24;
          P->core.scenes[a].steps[b][c].pitch_off = (int8_t)po;
        } else {
          /* 0xC0..0xFF: p-lock micro-timing (-30..+30 / 64 step), preserving roll bits 0..1 */
          int m = (val & 0x3F) - 30;
          if (m < -30) m = -30;
          if (m > 30) m = 30;
          uint8_t prev = P->core.scenes[a].steps[b][c].roll;
          P->core.scenes[a].steps[b][c].roll = (uint8_t)(((m & 0x3F) << 2) | (prev & 3));
        }
        P->shm_dirty = 1;
      }
      break;
    case CMD_SET_ROLL:
      if (a < EP_SCENES && b < EP_PADS && c < EP_STEPS) {
        if (val & 0x80) {
          /* High bit set: val = 0x80 | chance (1..100%; 0 or >=100 -> 100% default stored as 0) */
          int ch = val & 0x7F;
          P->core.scenes[a].steps[b][c].chance = (uint8_t)((ch > 0 && ch < 100) ? ch : 0);
        } else if (val >= 4) {
          /* 4..127: p-lock filter offset (val=60 -> 0%, 10..110 -> -100..+100% in 2% steps) */
          int fo = (val - 60) * 2;
          if (fo < -100) fo = -100;
          if (fo > 100) fo = 100;
          P->core.scenes[a].steps[b][c].filt_off = (int8_t)fo;
        } else {
          uint8_t prev = P->core.scenes[a].steps[b][c].roll;
          P->core.scenes[a].steps[b][c].roll = (uint8_t)((prev & 0xFCu) | (uint8_t)(val & 3));
        }
        P->shm_dirty = 1;
      }
      break;
    case CMD_CLEAR_PAD:
      if (val == 0xFF) {
        request_load(P, b, NULL);
        P->shm_dirty = 1;
        P->report_next = 0;
        P->seq_notify_cd = 0;
      } else {
        ep_seq_clear_pad(&P->core, a, b);
        P->shm_dirty = 1;
      }
      break;
    case CMD_CLEAR_SCENE:
      if (val == 0xFF) {
        ep_core_all_off(&P->core);
        ep_seq_set_mode(&P->core, EP_SEQ_STOP);
        ep_core_reset(&P->core);
        for (int s = 0; s < EP_SCENES; s++) ep_seq_clear_scene(&P->core, s);
        for (int i = 0; i < EP_PADS; i++) {
          request_load(P, i, NULL);
          ep_pad_defaults(&P->core.pad[i], i);
          P->last_pad_hit_sample[i] = 0;
        }
        memset(P->core.lfo_phase, 0, sizeof P->core.lfo_phase);
        memset(P->core.lfo_sh, 0, sizeof P->core.lfo_sh);
        memset(P->core.lfo_val, 0, sizeof P->core.lfo_val);
        P->last_any_hit_sample = 0;
        P->last_any_hit_pad = -1;
        P->last_any_hit_vel = 0;
        P->core.retrig_ms = 25;
        P->core.min_vel = 0;
        P->core.xtalk = 0;
        P->core.master_vol = 85.0f;
        P->core.stereo_width = 100.0f;
        P->core.bus_glue = 0.0f;
        P->core.comp_env = 0.0f;
        P->core.play_mode = EP_PLAY_NORMAL;
        P->core.repeat_div = EP_RPT_OFF;
        P->core.held_mask = 0;
        P->core.choke_mute_mask = 0;
        P->core.choke_solo_mask = 0;
        for (int g = 0; g <= EP_CHOKE_MAX; g++) P->core.choke_env[g] = 1.0f;
        P->core.perf_filter = 0.0f;
        P->core.perf_reso = 20.0f;
        P->core.perf_filter_s = 0.0f;
        P->core.perf_reso_s = 20.0f;
        P->core.perf_fx = EP_FX_NONE;
        P->core.perf_fx_cur = EP_FX_NONE;
        P->core.perf_break = 0;
        P->core.perf_aux1_throw = 0;
        memset(P->core.rr_pos, 0, sizeof P->core.rr_pos);
        P->core.seq.scene = 0;
        P->core.seq.pend_scene = -1;
        P->core.seq.fill_ret_scene = -1;
        P->core.seq.metro_mode = EP_METRO_OFF;
        P->core.seq.countin_en = 0;
        P->core.seq.countin_rem = 0;
        P->core.seq.chain_on = 0;
        P->core.seq.chain_len = 4;
        P->core.seq.chain_pos = 0;
        P->core.seq.chain[0] = 0; P->core.seq.chain[1] = 0;
        P->core.seq.chain[2] = 1; P->core.seq.chain[3] = 2;
        P->core.seq.chain[4] = 0; P->core.seq.chain[5] = 1;
        P->core.seq.chain[6] = 2; P->core.seq.chain[7] = 3;
        P->core.seq.undo_scene_idx = -1;
        P->core.seq.num_steps = EP_STEPS_DEF;
        P->core.seq.bpm = 120.0;
        P->core.seq.swing = 50.0f;
        ep_seq_set_quantize(&P->core, 100);
        P->sel_pad = 0;
        P->follow = 1;
        P->learn = 0;
        P->aux_routes[0] = '\0';
        P->have_prev_ctl = 0;
        P->have_prev_steps = 0;
        P->shm_dirty = 1;
        P->report_next = 0;
        P->seq_notify_cd = 0;
      } else {
        ep_seq_clear_scene(&P->core, a);
        P->shm_dirty = 1;
      }
      break;
    case CMD_COPY_SCENE:
      ep_seq_copy_scene(&P->core, a, b);
      P->shm_dirty = 1;
      break;
    case CMD_TRIG_PAD:
      if (b < EP_PADS) {
        ep_core_trigger(&P->core, b, val > 0 ? (val > 127 ? 127 : val) : 100);
      }
      break;
    case CMD_SET_START:
      if (a < EP_SCENES && b < EP_PADS) {
        int cur_end = P->core.scenes[a].pad_end[b];
        if (cur_end == 0) cur_end = EP_STEPS_DEF;
        int ns = val < EP_STEPS ? val : EP_STEPS - 1;
        int ne = cur_end <= ns ? ns + 1 : cur_end;
        ep_seq_set_pad_range(&P->core, a, b, ns, ne);
        P->shm_dirty = 1;
      }
      break;
    case CMD_SET_END:
      if (a < EP_SCENES && b < EP_PADS) {
        int cur_start = P->core.scenes[a].pad_start[b];
        int ne = val < 1 ? 1 : (val > EP_STEPS ? EP_STEPS : val);
        int ns = cur_start >= ne ? ne - 1 : cur_start;
        ep_seq_set_pad_range(&P->core, a, b, ns, ne);
        P->shm_dirty = 1;
      }
      break;
    case CMD_SET_QUANT:
      ep_seq_set_quantize(&P->core, val);
      P->shm_dirty = 1;
      break;
    case CMD_SET_RETRIG:
      P->core.retrig_ms = val > 80 ? 80 : (val < 0 ? 0 : val);
      P->shm_dirty = 1;
      break;
    case CMD_SET_MINVEL:
      P->core.min_vel = val > 40 ? 40 : (val < 0 ? 0 : val);
      P->shm_dirty = 1;
      break;
    case CMD_SET_XTALK:
      P->core.xtalk = val > 60 ? 60 : (val < 0 ? 0 : val);
      P->shm_dirty = 1;
      break;
    case CMD_SET_LFO:
      if (a < EP_LFOS && b < EP_PADS) {
        if (c == 0) {
          /* Set wave 0..4 while preserving sync flag in bit 4 */
          P->core.pad[b].lfo_wave[a] = (P->core.pad[b].lfo_wave[a] & 16) | (val % EP_LFO_NWAVES);
        } else if (c == 2) {
          /* Toggle/set BPM Sync flag in bit 4 */
          if (val) P->core.pad[b].lfo_wave[a] |= 16;
          else     P->core.pad[b].lfo_wave[a] &= ~16;
        } else {
          /* Logarithmic mapping val 0..200 -> 0.05..20.0 Hz */
          double t = (double)(val > 200 ? 200 : val) / 200.0;
          P->core.pad[b].lfo_rate[a] = (float)(0.05 * pow(400.0, t));
        }
        P->shm_dirty = 1;
      }
      break;
    case CMD_SET_MOD_SD:
      if (a < EP_MOD_SLOTS && b < EP_PADS) {
        P->core.pad[b].mod_src[a] = (c < EP_MSRC_COUNT) ? c : 0;
        P->core.pad[b].mod_dst[a] = (val < EP_MDST_COUNT) ? val : 0;
        P->shm_dirty = 1;
      }
      break;
    case CMD_SET_MOD_AMT:
      if (a == 4) {
        if (c == 1) {
          /* Reset all 16 pad MIDI notes to default 36..51 */
          for (int i = 0; i < EP_PADS; i++) {
            P->core.pad[i].midi_note = 36 + i;
          }
          P->shm_dirty = 1;
        } else if (b < EP_PADS) {
          /* Set single pad's MIDI note 0..127 */
          P->core.pad[b].midi_note = val & 0x7F;
          P->shm_dirty = 1;
        }
      } else if (a == 5 && b < EP_PADS) {
        /* Pad extras: attack, reso, link, fine start/end (for Auto-Slice), choke, Copy/Swap, Touch Hold */
        if (c == 0) {
          P->core.pad[b].attack = (float)(val > 100 ? 100 : val);
          P->shm_dirty = 1;
        } else if (c == 1) {
          P->core.pad[b].reso = (float)(val > 100 ? 100 : val);
          P->shm_dirty = 1;
        } else if (c == 2) {
          P->core.pad[b].link_pad = (val <= EP_PADS) ? val : 0;
          P->shm_dirty = 1;
        } else if (c == 3) {
          P->core.pad[b].link_mode = val ? EP_LINK_RR : EP_LINK_LAYER;
          P->shm_dirty = 1;
        } else if (c == 4) {
          /* Fine start: 0..240 -> 0..100% (exact 1/4, 1/8, 1/16 fractions!) */
          float pct = ((float)(val > 240 ? 240 : val)) * (100.0f / 240.0f);
          if (pct > 99.0f) pct = 99.0f;
          P->core.pad[b].start = pct;
          if (P->core.pad[b].end <= pct + 0.2f) P->core.pad[b].end = pct + 0.5f > 100.0f ? 100.0f : pct + 0.5f;
          P->shm_dirty = 1;
        } else if (c == 5) {
          /* Fine end: 1..240 -> 0.416..100% */
          int vv = val < 1 ? 1 : (val > 240 ? 240 : val);
          float pct = ((float)vv) * (100.0f / 240.0f);
          if (pct < 1.0f) pct = 1.0f;
          if (pct > 100.0f) pct = 100.0f;
          P->core.pad[b].end = pct;
          P->shm_dirty = 1;
        } else if (c == 6) {
          P->core.pad[b].choke = val > EP_CHOKE_MAX ? EP_CHOKE_MAX : val;
          P->shm_dirty = 1;
        } else if (c == 7) {
          /* Copy Pad b -> dst (preserve dst midi_note) */
          int dst = val & 15;
          if (dst != b) {
            int keep_note = P->core.pad[dst].midi_note;
            P->core.pad[dst] = P->core.pad[b];
            P->core.pad[dst].midi_note = keep_note;
            request_load(P, dst, P->paths[b][0] ? P->paths[b] : NULL);
            P->shm_dirty = 1;
            P->report_next = 0;
          }
        } else if (c == 8) {
          /* Swap Pad b <-> dst (preserve both midi_notes) */
          int dst = val & 15;
          if (dst != b) {
            int nb = P->core.pad[b].midi_note;
            int nd = P->core.pad[dst].midi_note;
            EpPadParams tp = P->core.pad[b];
            P->core.pad[b] = P->core.pad[dst];
            P->core.pad[dst] = tp;
            P->core.pad[b].midi_note = nb;
            P->core.pad[dst].midi_note = nd;
            /* Swap sample pointers, paths & sources safely */
            ep_core_all_off(&P->core);
            EpSample* ts = P->samples[b];
            P->samples[b] = P->samples[dst];
            P->samples[dst] = ts;
            char tpath[EP_PATH_MAX];
            memcpy(tpath, P->paths[b], EP_PATH_MAX);
            memcpy(P->paths[b], P->paths[dst], EP_PATH_MAX);
            memcpy(P->paths[dst], tpath, EP_PATH_MAX);
            EpSource sb = { 0 }, sd = { 0 };
            if (P->samples[b])   { sb.data = P->samples[b]->data;   sb.frames = P->samples[b]->frames;   sb.rate = (float)P->samples[b]->rate; }
            if (P->samples[dst]) { sd.data = P->samples[dst]->data; sd.frames = P->samples[dst]->frames; sd.rate = (float)P->samples[dst]->rate; }
            ep_core_set_source(&P->core, b, P->samples[b] ? &sb : NULL);
            ep_core_set_source(&P->core, dst, P->samples[dst] ? &sd : NULL);
            P->shm_dirty = 1;
            P->report_next = 0;
          }
        } else if (c == 9) {
          ep_core_hold_start(&P->core, b, b, val > 0 ? val : 110, 0.0f, 0.0f);
        } else if (c == 10) {
          ep_core_hold_stop(&P->core, b);
          ep_core_release(&P->core, b);
        }
      } else if (a == 6) {
        /* Global performance & sequencer controls */
        if (c == 0) {
          P->core.play_mode = val % EP_PLAY_NMODES;
          P->core.held_mask = 0;
          P->shm_dirty = 1;
        } else if (c == 1) {
          P->core.repeat_div = val % EP_RPT_NMODES;
          P->core.held_mask = 0;
          P->shm_dirty = 1;
        } else if (c == 2) {
          P->core.seq.metro_mode = (val <= 2) ? val : 0;
          P->shm_dirty = 1;
        } else if (c == 3) {
          P->core.seq.countin_en = val ? 1 : 0;
          P->shm_dirty = 1;
        } else if (c == 4) {
          if (ep_seq_undo(&P->core)) {
            P->shm_dirty = 1;
          }
        } else if (c == 5) {
          ep_seq_request_fill(&P->core, val & 7);
          P->shm_dirty = 1;
        } else if (c == 6) {
          P->core.seq.chain_on = val ? 1 : 0;
          P->shm_dirty = 1;
        } else if (c == 7) {
          int cl = val < 1 ? 1 : (val > EP_CHAIN_MAX ? EP_CHAIN_MAX : val);
          P->core.seq.chain_len = cl;
          if (P->core.seq.chain_pos >= cl) P->core.seq.chain_pos = 0;
          P->shm_dirty = 1;
        } else if (c == 8) {
          P->core.seq.chain[b & 7] = (uint8_t)(val & 7);
          P->shm_dirty = 1;
        } else if (c == 9) {
          /* Touch trigger 16-level slot b (0..15) for sel_pad */
          int eff_vel = 110;
          float po = 0.0f, fo = 0.0f;
          ep_core_trigger_16level(&P->core, P->sel_pad, b, val > 0 ? val : 110, &eff_vel, &po, &fo);
          ep_core_hold_start(&P->core, b, P->sel_pad, eff_vel, po, fo);
          if (P->core.seq.mode == EP_SEQ_REC && P->core.seq.countin_rem == 0) {
            ep_seq_record_hit_ex(&P->core, P->sel_pad, eff_vel, (int)lrintf(po), (int)lrintf(fo));
            P->shm_dirty = 1;
            P->seq_notify_dirty = 1;
          }
        } else if (c == 10) {
          ep_core_hold_stop(&P->core, b);
          ep_core_release(&P->core, P->sel_pad);
        } else if (c == 11) {
          P->core.choke_mute_mask = (uint8_t)(val & 0x0F);
        } else if (c == 12) {
          P->core.choke_solo_mask = (uint8_t)(val & 0x0F);
        } else if (c == 13) {
          int fv = (val > 200 ? 200 : val) - 100;
          P->core.perf_filter = (float)fv;
        } else if (c == 14) {
          int rv = val > 100 ? 100 : val;
          P->core.perf_reso = (float)rv;
        } else if (c == 15) {
          P->core.perf_fx = (val < EP_FX_NMODES) ? val : EP_FX_NONE;
        } else if (c == 16) {
          P->core.perf_break = (val & 1) ? 1 : 0;
          P->core.perf_aux1_throw = (val & 2) ? 1 : 0;
        }
      } else if (a == 7 && b < EP_PADS) {
        /* Pattern generators on active scene for pad b */
        int sc = P->core.seq.scene;
        if (val == 0xFE) {
          ep_seq_rotate_pad(&P->core, sc, b, -1);
          P->shm_dirty = 1;
        } else if (val == 0xFF) {
          ep_seq_rotate_pad(&P->core, sc, b, +1);
          P->shm_dirty = 1;
        } else if (val <= EP_STEPS) {
          ep_seq_euclid_pad(&P->core, sc, b, val, c);
          P->shm_dirty = 1;
        }
      } else if (a < EP_MOD_SLOTS && b < EP_PADS) {
        int v = val > 200 ? 200 : val;
        P->core.pad[b].mod_amt[a] = (float)(v - 100);
        P->shm_dirty = 1;
      }
      break;
    default:
      break;
  }
  int is_transient = (op == CMD_TRIG_PAD) ||
                     (op == CMD_SET_MOD_AMT && a == 5 && (c == 9 || c == 10)) ||
                     (op == CMD_SET_MOD_AMT && a == 6 && (c == 0 || c == 1 || c == 5 || (c >= 9 && c <= 16)));
  if (op != 0 && !is_transient) {
    P->seq_notify_dirty = 1;
  }
}

static void snapshot_prev_controls(EkaPads* P) {
  for (int i = 0; i < N_CTL; i++) {
    if (P->ctl[i]) P->prev_ctl[i] = *P->ctl[i];
  }
  if (P->in_pad_steps) P->prev_pad_steps = *P->in_pad_steps;
  if (P->in_pad_beg)   P->prev_pad_beg   = *P->in_pad_beg;
  for (int a = 0; a < EP_AUX_BUSES; a++) {
    if (P->in_pad_aux[a]) P->prev_pad_aux[a] = *P->in_pad_aux[a];
  }
  for (int k = 0; k < EP_LFOS; k++) {
    if (P->in_pad_lfo_rate[k]) P->prev_pad_lfo_rate[k] = *P->in_pad_lfo_rate[k];
  }
  for (int k = 0; k < 2; k++) {
    if (P->in_pad_mod_amt[k]) P->prev_pad_mod_amt[k] = *P->in_pad_mod_amt[k];
  }
  for (int k = 0; k < 4; k++) {
    if (P->in_pad_note[k]) P->prev_pad_note[k] = *P->in_pad_note[k];
  }
  P->have_prev_ctl = 1;
  P->have_prev_steps = 1;
}

static void handle_patch_set(EkaPads* P, const LV2_Atom_Object* obj) {
  const LV2_Atom* prop = NULL;
  const LV2_Atom* val = NULL;
  lv2_atom_object_get(obj, P->patch_property, &prop, P->patch_value, &val, 0);
  if (!prop || prop->type != P->atom_URID || !val) return;
  LV2_URID u = ((const LV2_Atom_URID*)prop)->body;

  for (int i = 0; i < EP_PADS; i++) {
    if (u == P->p_sample[i] && (val->type == P->atom_Path || val->type == P->atom_String)) {
      char buf[EP_PATH_MAX];
      size_t n = val->size < EP_PATH_MAX - 1 ? val->size : EP_PATH_MAX - 1;
      if (n > 0) memcpy(buf, LV2_ATOM_BODY_CONST(val), n);
      buf[n] = '\0';
      request_load(P, i, buf);
      return;
    }
  }
  if (u == P->p_seq_data && (val->type == P->atom_String || val->type == P->atom_Path)) {
    const char* s = (const char*)LV2_ATOM_BODY_CONST(val);
    seq_decode(&P->core, s);
    P->have_prev_ctl = 0;
    P->have_prev_steps = 0;
    P->shm_dirty = 1;
    return;
  }
  if (u == P->p_aux_routes && (val->type == P->atom_String || val->type == P->atom_Path)) {
    const char* s = (const char*)LV2_ATOM_BODY_CONST(val);
    snprintf(P->aux_routes, sizeof P->aux_routes, "%s", s ? s : "");
    return;
  }

  int iv = 0;
  if (!atom_to_int(P, val, &iv)) return;

  if (u == P->p_sel_pad) {
    if (iv >= 0 && iv < EP_PADS && iv != P->sel_pad) {
      P->sel_pad = iv;
      snapshot_prev_controls(P);
    }
  } else if (u == P->p_follow) {
    P->follow = iv ? 1 : 0;
  } else if (u == P->p_learn) {
    P->learn = iv ? 1 : 0;
  } else if (u == P->p_mute_mask) {
    for (int i = 0; i < EP_PADS; i++) P->core.pad[i].mute = (iv >> i) & 1;
    P->shm_dirty = 1;
    P->seq_notify_dirty = 1;
  } else if (u == P->p_solo_mask) {
    for (int i = 0; i < EP_PADS; i++) P->core.pad[i].solo = (iv >> i) & 1;
    P->shm_dirty = 1;
    P->seq_notify_dirty = 1;
  } else if (u == P->p_pad_end) {
    if (iv < 1) iv = 1;
    if (iv > 100) iv = 100;
    P->core.pad[P->sel_pad].end = (float)iv;
    P->shm_dirty = 1;
    P->seq_notify_dirty = 1;
  } else if (u == P->p_seq_bpm) {
    if (iv < 40) iv = 40;
    if (iv > 240) iv = 240;
    P->core.seq.bpm = (double)iv;
  } else if (u == P->p_seq_cmd) {
    handle_seq_cmd(P, iv);
  }
}

static void handle_time_position(EkaPads* P, const LV2_Atom_Object* obj) {
  const LV2_Atom *a_bpm = NULL, *a_speed = NULL, *a_beat = NULL, *a_bpb = NULL;
  lv2_atom_object_get(obj,
                      P->time_bpm, &a_bpm,
                      P->time_speed, &a_speed,
                      P->time_barBeat, &a_beat,
                      P->time_bpb, &a_bpb, 0);
  double bpm = P->core.seq.bpm;
  double speed = 0.0;
  double beat = -1.0;
  double bpb = 4.0;
  atom_to_double(P, a_bpm, &bpm);
  atom_to_double(P, a_speed, &speed);
  atom_to_double(P, a_beat, &beat);
  atom_to_double(P, a_bpb, &bpb);
  int prev_mode = P->core.seq.mode;
  ep_seq_sync_host(&P->core, bpm, speed > 0.01, beat, (int)lrint(bpb));
  if (P->core.seq.mode != prev_mode) {
    P->shm_dirty = 1;
  }
}

static void apply_pad_control(EpPadParams* p, int idx, float v) {
  switch (idx) {
    case P_PITCH:  p->pitch = v; break;
    case P_DECAY:  p->decay = v; break;
    case P_FILTER: p->filter = v; break;
    case P_LEVEL:  p->level = v; break;
    case P_START:  p->start = v; break;
    case P_MODE:   p->mode = (int)lrintf(v); if (p->mode < 0) p->mode = 0; if (p->mode >= EP_NMODES) p->mode = EP_NMODES - 1; break;
    case P_PUNCH:  p->punch = v; break;
    case P_DRIVE:  p->drive = v; break;
    case P_PAN:    p->pan = v; break;
    case P_VEL:    p->vel_sens = v; break;
    case P_CHOKE:  p->choke = (int)lrintf(v); if (p->choke < 0) p->choke = 0; if (p->choke > EP_CHOKE_MAX) p->choke = EP_CHOKE_MAX; break;
    case P_WIDTH:  p->width = v; break;
    default: break;
  }
}

static void forge_notify_reports(EkaPads* P) {
  if (!P->notify) return;
  const uint32_t cap = P->notify->atom.size;
  lv2_atom_forge_set_buffer(&P->forge, (uint8_t*)P->notify, cap);
  LV2_Atom_Forge_Frame seq_frame;
  lv2_atom_forge_sequence_head(&P->forge, &seq_frame, 0);

  if (P->report_next >= 0) {
    int budget = 8;
    while (P->report_next >= 0 && budget-- > 0) {
      LV2_Atom_Forge_Frame obj;
      if (P->report_next < EP_PADS) {
        int i = P->report_next;
        lv2_atom_forge_frame_time(&P->forge, 0);
        lv2_atom_forge_object(&P->forge, &obj, 0, P->patch_Set);
        lv2_atom_forge_key(&P->forge, P->patch_property);
        lv2_atom_forge_urid(&P->forge, P->p_sample[i]);
        lv2_atom_forge_key(&P->forge, P->patch_value);
        const char* p = P->paths[i][0] ? P->paths[i] : "_";
        lv2_atom_forge_path(&P->forge, p, (uint32_t)strlen(p));
        lv2_atom_forge_pop(&P->forge, &obj);
      } else {
        int k = P->report_next - EP_PADS;
        LV2_URID u = 0;
        int val = 0;
        if (k == 0) { u = P->p_sel_pad; val = P->sel_pad; }
        else if (k == 1) { u = P->p_follow; val = P->follow; }
        else if (k == 2) { u = P->p_learn; val = P->learn; }
        else if (k == 3) { u = P->p_mute_mask; val = get_mute_mask(P); }
        else if (k == 4) { u = P->p_solo_mask; val = get_solo_mask(P); }
        else if (k == 5) { u = P->p_pad_end; val = (int)lrintf(P->core.pad[P->sel_pad].end); }
        else if (k == 6) { u = P->p_seq_bpm; val = (int)lrint(P->core.seq.bpm); }
        else if (k == 7) {
          const char* ar = P->aux_routes[0] ? P->aux_routes : "_";
          lv2_atom_forge_frame_time(&P->forge, 0);
          lv2_atom_forge_object(&P->forge, &obj, 0, P->patch_Set);
          lv2_atom_forge_key(&P->forge, P->patch_property);
          lv2_atom_forge_urid(&P->forge, P->p_aux_routes);
          lv2_atom_forge_key(&P->forge, P->patch_value);
          lv2_atom_forge_path(&P->forge, ar, (uint32_t)strlen(ar));
          lv2_atom_forge_pop(&P->forge, &obj);
          P->report_next++;
          if (P->report_next >= EP_PADS + 8) {
            P->report_next = -1;
            P->seq_notify_dirty = 1;
          }
          continue;
        }
        else { P->report_next = -1; break; }
        lv2_atom_forge_frame_time(&P->forge, 0);
        lv2_atom_forge_object(&P->forge, &obj, 0, P->patch_Set);
        lv2_atom_forge_key(&P->forge, P->patch_property);
        lv2_atom_forge_urid(&P->forge, u);
        lv2_atom_forge_key(&P->forge, P->patch_value);
        lv2_atom_forge_int(&P->forge, val);
        lv2_atom_forge_pop(&P->forge, &obj);
      }
      P->report_next++;
      if (P->report_next >= EP_PADS + 8) {
        P->report_next = -1;
        P->seq_notify_dirty = 1;
      }
    }
  } else if (P->seq_notify_dirty) {
    if (P->seq_notify_cd > 0) {
      P->seq_notify_cd--;
    } else {
      P->seq_notify_dirty = 0;
      P->seq_notify_cd = 16; /* throttle ~85 ms at 256-sample blocks */
      size_t slen = seq_encode(&P->core, P->seq_buf, sizeof P->seq_buf);
      if (slen > 0 && slen + 64 < cap) {
        LV2_Atom_Forge_Frame obj;
        lv2_atom_forge_frame_time(&P->forge, 0);
        lv2_atom_forge_object(&P->forge, &obj, 0, P->patch_Set);
        lv2_atom_forge_key(&P->forge, P->patch_property);
        lv2_atom_forge_urid(&P->forge, P->p_seq_data);
        lv2_atom_forge_key(&P->forge, P->patch_value);
        lv2_atom_forge_path(&P->forge, P->seq_buf, (uint32_t)slen);
        lv2_atom_forge_pop(&P->forge, &obj);
      }
    }
  }
  lv2_atom_forge_pop(&P->forge, &seq_frame);
}

/* ---------- LV2 callbacks ---------- */

static LV2_Handle instantiate(const LV2_Descriptor* d, double sr, const char* bundle_path,
                              const LV2_Feature* const* features) {
  (void)d; (void)bundle_path;
  EkaPads* P = (EkaPads*)calloc(1, sizeof(EkaPads));
  if (!P) return NULL;
  P->sr = sr;
  P->sample_clock = (uint64_t)(sr > 1000.0 ? sr * 10.0 : 480000.0);
  P->last_any_hit_pad = -1;
  P->follow = 1;
  P->report_next = -1;
  P->shm_fd = -1;
  P->shm_dirty = 1;
  ep_core_init(&P->core, sr);

  for (int i = 0; features && features[i]; i++) {
    if (!strcmp(features[i]->URI, LV2_URID__map))
      P->map = (LV2_URID_Map*)features[i]->data;
    else if (!strcmp(features[i]->URI, LV2_WORKER__schedule))
      P->sched = (LV2_Worker_Schedule*)features[i]->data;
  }
  if (!P->map) { free(P); return NULL; }

  lv2_atom_forge_init(&P->forge, P->map);
  P->midi_Event     = P->map->map(P->map->handle, LV2_MIDI__MidiEvent);
  P->atom_Object    = P->map->map(P->map->handle, LV2_ATOM__Object);
  P->atom_Path      = P->map->map(P->map->handle, LV2_ATOM__Path);
  P->atom_String    = P->map->map(P->map->handle, LV2_ATOM__String);
  P->atom_URID      = P->map->map(P->map->handle, LV2_ATOM__URID);
  P->atom_Int       = P->map->map(P->map->handle, LV2_ATOM__Int);
  P->atom_Float     = P->map->map(P->map->handle, LV2_ATOM__Float);
  P->atom_Double    = P->map->map(P->map->handle, LV2_ATOM__Double);
  P->atom_Long      = P->map->map(P->map->handle, LV2_ATOM__Long);
  P->atom_Chunk     = P->map->map(P->map->handle, LV2_ATOM__Chunk);
  P->patch_Set      = P->map->map(P->map->handle, LV2_PATCH__Set);
  P->patch_Get      = P->map->map(P->map->handle, LV2_PATCH__Get);
  P->patch_property = P->map->map(P->map->handle, LV2_PATCH__property);
  P->patch_value    = P->map->map(P->map->handle, LV2_PATCH__value);
  P->time_Position  = P->map->map(P->map->handle, LV2_TIME__Position);
  P->time_bpm       = P->map->map(P->map->handle, LV2_TIME__beatsPerMinute);
  P->time_speed     = P->map->map(P->map->handle, LV2_TIME__speed);
  P->time_bar       = P->map->map(P->map->handle, LV2_TIME__bar);
  P->time_barBeat   = P->map->map(P->map->handle, LV2_TIME__barBeat);
  P->time_bpb       = P->map->map(P->map->handle, LV2_TIME__beatsPerBar);

  char uri[128];
  for (int i = 0; i < EP_PADS; i++) {
    snprintf(uri, sizeof uri, "%s#sample%d", EP_URI, i + 1);
    P->p_sample[i] = P->map->map(P->map->handle, uri);
  }
  P->p_sel_pad     = P->map->map(P->map->handle, EP_URI "#sel_pad");
  P->p_follow      = P->map->map(P->map->handle, EP_URI "#follow");
  P->p_learn       = P->map->map(P->map->handle, EP_URI "#learn");
  P->p_mute_mask   = P->map->map(P->map->handle, EP_URI "#mute_mask");
  P->p_solo_mask   = P->map->map(P->map->handle, EP_URI "#solo_mask");
  P->p_pad_end     = P->map->map(P->map->handle, EP_URI "#pad_end");
  P->p_seq_bpm     = P->map->map(P->map->handle, EP_URI "#seq_bpm");
  P->p_seq_cmd     = P->map->map(P->map->handle, EP_URI "#seq_cmd");
  P->p_seq_len     = P->map->map(P->map->handle, EP_URI "#seq_len");
  P->p_seq_data    = P->map->map(P->map->handle, EP_URI "#seq_data");
  P->p_aux_routes  = P->map->map(P->map->handle, EP_URI "#aux_routes");
  P->st_pad_params = P->map->map(P->map->handle, EP_URI "#state_pads");
  P->st_scenes     = P->map->map(P->map->handle, EP_URI "#state_scenes");

  /* Shared memory snapshot for Zynthian widget (same pattern as Eka Roll) */
  snprintf(P->shm_path, sizeof P->shm_path, "/dev/shm/eka_pads_%d", (int)getpid());
  P->shm_fd = open(P->shm_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
  if (P->shm_fd >= 0 && ftruncate(P->shm_fd, (off_t)sizeof(EpShm)) == 0) {
    void* mp = mmap(NULL, sizeof(EpShm), PROT_READ | PROT_WRITE, MAP_SHARED, P->shm_fd, 0);
    if (mp != MAP_FAILED) {
      P->shm = (EpShm*)mp;
      memset(P->shm, 0, sizeof(EpShm));
      P->shm->magic = EP_SHM_MAGIC;
      P->shm->version = EP_SHM_VER;
      shm_publish(P);
    }
  }

  return (LV2_Handle)P;
}

static void connect_port(LV2_Handle h, uint32_t port, void* data) {
  EkaPads* P = (EkaPads*)h;
  if (port < N_CTL) { P->ctl[port] = (const float*)data; return; }
  switch (port) {
    case P_MIDI_IN:       P->in = (const LV2_Atom_Sequence*)data; break;
    case P_OUT_L:         P->out_l = (float*)data; break;
    case P_OUT_R:         P->out_r = (float*)data; break;
    case P_NOTIFY:        P->notify = (LV2_Atom_Sequence*)data; break;
    case P_OUT_SEL_PAD:   P->out_sel_pad = (float*)data; break;
    case P_OUT_SEQ_STEP:  P->out_seq_step = (float*)data; break;
    case P_OUT_SEQ_SCENE: P->out_seq_scene = (float*)data; break;
    case P_OUT_SEQ_PEND:  P->out_seq_pend = (float*)data; break;
    case P_OUT_PLAYHEAD:  P->out_playhead = (float*)data; break;
    case P_STEREO_WIDTH:  P->in_stereo_width = (const float*)data; break;
    case P_PAD_STEPS:     P->in_pad_steps = (const float*)data; break;
    case P_PAD_BEG:       P->in_pad_beg = (const float*)data; break;
    case P_AUX1_L:        P->aux_l[0] = (float*)data; break;
    case P_AUX1_R:        P->aux_r[0] = (float*)data; break;
    case P_AUX2_L:        P->aux_l[1] = (float*)data; break;
    case P_AUX2_R:        P->aux_r[1] = (float*)data; break;
    case P_AUX3_L:        P->aux_l[2] = (float*)data; break;
    case P_AUX3_R:        P->aux_r[2] = (float*)data; break;
    case P_AUX4_L:        P->aux_l[3] = (float*)data; break;
    case P_AUX4_R:        P->aux_r[3] = (float*)data; break;
    case P_PAD_AUX1:      P->in_pad_aux[0] = (const float*)data; break;
    case P_PAD_AUX2:      P->in_pad_aux[1] = (const float*)data; break;
    case P_PAD_AUX3:      P->in_pad_aux[2] = (const float*)data; break;
    case P_PAD_AUX4:      P->in_pad_aux[3] = (const float*)data; break;
    case P_PAD_LFO1_RATE: P->in_pad_lfo_rate[0] = (const float*)data; break;
    case P_PAD_LFO2_RATE: P->in_pad_lfo_rate[1] = (const float*)data; break;
    case P_PAD_MOD1_AMT:  P->in_pad_mod_amt[0] = (const float*)data; break;
    case P_PAD_MOD2_AMT:  P->in_pad_mod_amt[1] = (const float*)data; break;
    case P_PAD_NOTE1:     P->in_pad_note[0] = (const float*)data; break;
    case P_PAD_NOTE2:     P->in_pad_note[1] = (const float*)data; break;
    case P_PAD_NOTE3:     P->in_pad_note[2] = (const float*)data; break;
    case P_PAD_NOTE4:     P->in_pad_note[3] = (const float*)data; break;
    case P_BUS_GLUE:      P->in_bus_glue = (const float*)data; break;
    default:
      if (port >= P_OUT_LVL0 && port < P_OUT_LVL0 + EP_PADS)
        P->out_lvl[port - P_OUT_LVL0] = (float*)data;
      break;
  }
}

static void activate(LV2_Handle h) {
  EkaPads* P = (EkaPads*)h;
  ep_core_reset(&P->core);
  P->have_prev_ctl = 0;
  P->have_prev_steps = 0;
  P->shm_dirty = 1;
}

static void run(LV2_Handle h, uint32_t nframes) {
  EkaPads* P = (EkaPads*)h;
  if (!P->out_l || !P->out_r) return;

  pump_pending_loads(P);
  collect_retired(P);

  /* Pass 1: process patch:Set / patch:Get / time:Position BEFORE reading control ports
   * so switching sel_pad or loading seq_data never lets stale encoder values overwrite the old pad! */
  if (P->in) {
    LV2_ATOM_SEQUENCE_FOREACH(P->in, ev) {
      if (ev->body.type == P->atom_Object) {
        const LV2_Atom_Object* obj = (const LV2_Atom_Object*)&ev->body;
        if (obj->body.otype == P->patch_Set) {
          handle_patch_set(P, obj);
        } else if (obj->body.otype == P->patch_Get) {
          P->report_next = 0;
        } else if (obj->body.otype == P->time_Position) {
          handle_time_position(P, obj);
        }
      }
    }
  }

  /* Sync control inputs: only apply pad knobs (0..11) when they actually move */
  if (!P->have_prev_ctl) {
    for (int i = 0; i < N_CTL; i++) P->prev_ctl[i] = P->ctl[i] ? *P->ctl[i] : 0.0f;
    P->have_prev_ctl = 1;
  } else {
    for (int i = 0; i < N_PAD_CTL; i++) {
      if (!P->ctl[i]) continue;
      float v = *P->ctl[i];
      if (fabsf(v - P->prev_ctl[i]) > 1e-4f) {
        P->prev_ctl[i] = v;
        apply_pad_control(&P->core.pad[P->sel_pad], i, v);
        P->shm_dirty = 1;
        P->seq_notify_dirty = 1;
      }
    }
    if (P->ctl[P_SEQ_MODE]) {
      float v = *P->ctl[P_SEQ_MODE];
      if (fabsf(v - P->prev_ctl[P_SEQ_MODE]) > 1e-4f) {
        P->prev_ctl[P_SEQ_MODE] = v;
        ep_seq_set_mode(&P->core, (int)lrintf(v));
      }
    }
    if (P->ctl[P_SEQ_SCENE]) {
      float v = *P->ctl[P_SEQ_SCENE];
      if (fabsf(v - P->prev_ctl[P_SEQ_SCENE]) > 1e-4f) {
        P->prev_ctl[P_SEQ_SCENE] = v;
        ep_seq_request_scene(&P->core, (int)lrintf(v) - 1);
      }
    }
  }
  if (P->ctl[P_SEQ_SWING]) P->core.seq.swing = *P->ctl[P_SEQ_SWING];
  if (P->ctl[P_MASTER])    P->core.master_vol = *P->ctl[P_MASTER];
  if (P->in_stereo_width)  P->core.stereo_width = *P->in_stereo_width;
  if (P->in_bus_glue)      P->core.bus_glue = *P->in_bus_glue;

  /* Encoder control for selected pad's Steps (End), Beg Step, and Aux 1..4 Sends */
  if (!P->have_prev_steps) {
    if (P->in_pad_steps) P->prev_pad_steps = *P->in_pad_steps;
    if (P->in_pad_beg)   P->prev_pad_beg   = *P->in_pad_beg;
    for (int a = 0; a < EP_AUX_BUSES; a++) {
      if (P->in_pad_aux[a]) P->prev_pad_aux[a] = *P->in_pad_aux[a];
    }
    for (int k = 0; k < EP_LFOS; k++) {
      if (P->in_pad_lfo_rate[k]) P->prev_pad_lfo_rate[k] = *P->in_pad_lfo_rate[k];
    }
    for (int k = 0; k < 2; k++) {
      if (P->in_pad_mod_amt[k]) P->prev_pad_mod_amt[k] = *P->in_pad_mod_amt[k];
    }
    for (int k = 0; k < 4; k++) {
      if (P->in_pad_note[k]) P->prev_pad_note[k] = *P->in_pad_note[k];
    }
    P->have_prev_steps = 1;
  } else {
    int sc = P->core.seq.scene;
    int pd = P->sel_pad;
    int row = (pd >> 2) & 3;
    for (int col = 0; col < 4; col++) {
      if (P->in_pad_note[col]) {
        float v = *P->in_pad_note[col];
        if (fabsf(v - P->prev_pad_note[col]) > 0.4f) {
          P->prev_pad_note[col] = v;
          int mn = (int)lrintf(v);
          if (mn < 0) mn = 0;
          if (mn > 127) mn = 127;
          int tp = row * 4 + col;
          if (P->core.pad[tp].midi_note != mn) {
            P->core.pad[tp].midi_note = mn;
            P->shm_dirty = 1;
            P->seq_notify_dirty = 1;
          }
        }
      }
    }
    for (int k = 0; k < EP_LFOS; k++) {
      if (P->in_pad_lfo_rate[k]) {
        float v = *P->in_pad_lfo_rate[k];
        if (fabsf(v - P->prev_pad_lfo_rate[k]) > 1e-3f) {
          P->prev_pad_lfo_rate[k] = v;
          if (v < 0.05f) v = 0.05f;
          if (v > 20.0f) v = 20.0f;
          P->core.pad[pd].lfo_rate[k] = v;
          P->shm_dirty = 1;
          P->seq_notify_dirty = 1;
        }
      }
    }
    for (int k = 0; k < 2; k++) {
      if (P->in_pad_mod_amt[k]) {
        float v = *P->in_pad_mod_amt[k];
        if (fabsf(v - P->prev_pad_mod_amt[k]) > 1e-3f) {
          P->prev_pad_mod_amt[k] = v;
          if (v < -100.0f) v = -100.0f;
          if (v > 100.0f) v = 100.0f;
          P->core.pad[pd].mod_amt[k] = v;
          P->shm_dirty = 1;
          P->seq_notify_dirty = 1;
        }
      }
    }
    for (int a = 0; a < EP_AUX_BUSES; a++) {
      if (P->in_pad_aux[a]) {
        float v = *P->in_pad_aux[a];
        if (fabsf(v - P->prev_pad_aux[a]) > 1e-4f) {
          P->prev_pad_aux[a] = v;
          if (v < 0.0f) v = 0.0f;
          if (v > 100.0f) v = 100.0f;
          P->core.pad[pd].aux[a] = v;
          P->shm_dirty = 1;
          P->seq_notify_dirty = 1;
        }
      }
    }
    if (P->in_pad_steps) {
      float v = *P->in_pad_steps;
      if (fabsf(v - P->prev_pad_steps) > 0.4f) {
        P->prev_pad_steps = v;
        int ne = (int)lrintf(v);
        if (ne < 1) ne = 1;
        if (ne > EP_STEPS) ne = EP_STEPS;
        int cs = P->core.scenes[sc].pad_start[pd];
        int ns = cs >= ne ? ne - 1 : cs;
        ep_seq_set_pad_range(&P->core, sc, pd, ns, ne);
        P->shm_dirty = 1;
        P->seq_notify_dirty = 1;
      }
    }
    if (P->in_pad_beg) {
      float v = *P->in_pad_beg;
      if (fabsf(v - P->prev_pad_beg) > 0.4f) {
        P->prev_pad_beg = v;
        int ns = (int)lrintf(v) - 1;
        if (ns < 0) ns = 0;
        if (ns >= EP_STEPS) ns = EP_STEPS - 1;
        int ce = P->core.scenes[sc].pad_end[pd];
        if (ce == 0) ce = EP_STEPS_DEF;
        int ne = ce <= ns ? ns + 1 : ce;
        ep_seq_set_pad_range(&P->core, sc, pd, ns, ne);
        P->shm_dirty = 1;
        P->seq_notify_dirty = 1;
      }
    }
  }

  uint32_t pos = 0;
  if (P->in) {
    LV2_ATOM_SEQUENCE_FOREACH(P->in, ev) {
      uint32_t t = (uint32_t)(ev->time.frames < 0 ? 0 : ev->time.frames);
      if (t > nframes) t = nframes;

      if (ev->body.type == P->midi_Event && ev->body.size >= 1) {
        const uint8_t* msg = (const uint8_t*)(ev + 1);
        if (msg[0] == 0xFA || msg[0] == 0xFB) {
          /* MIDI Real-Time Start (0xFA) / Continue (0xFB) */
          if (t > pos) {
            float* al[EP_AUX_BUSES];
            float* ar[EP_AUX_BUSES];
            for (int a = 0; a < EP_AUX_BUSES; a++) {
              al[a] = P->aux_l[a] ? P->aux_l[a] + pos : NULL;
              ar[a] = P->aux_r[a] ? P->aux_r[a] + pos : NULL;
            }
            ep_core_process_aux(&P->core, P->out_l + pos, P->out_r + pos, al, ar, t - pos, P->sel_pad);
            pos = t;
          }
          if (msg[0] == 0xFA) {
            P->core.seq.bar_phase = 0.0;
            for (int p = 0; p < EP_PADS; p++) {
              P->core.seq.pad_phase[p] = 0.0;
              P->core.seq.pad_last_step[p] = -1;
              P->core.seq.pad_cur_step[p] = -1;
              P->core.seq.roll_rem[p] = 0;
            }
          }
          if (P->core.seq.mode == EP_SEQ_STOP) {
            ep_seq_set_mode(&P->core, EP_SEQ_PLAY);
            P->shm_dirty = 1;
          }
          continue;
        }
        if (msg[0] == 0xFC) {
          /* MIDI Real-Time Stop */
          if (P->core.seq.mode != EP_SEQ_STOP) {
            ep_seq_set_mode(&P->core, EP_SEQ_STOP);
            P->shm_dirty = 1;
          }
          continue;
        }
        if (ev->body.size < 3) continue;
        if (t > pos) {
          float* al[EP_AUX_BUSES];
          float* ar[EP_AUX_BUSES];
          for (int a = 0; a < EP_AUX_BUSES; a++) {
            al[a] = P->aux_l[a] ? P->aux_l[a] + pos : NULL;
            ar[a] = P->aux_r[a] ? P->aux_r[a] + pos : NULL;
          }
          ep_core_process_aux(&P->core, P->out_l + pos, P->out_r + pos, al, ar, t - pos, P->sel_pad);
          pos = t;
        }
        uint8_t status = msg[0] & 0xF0;
        uint8_t note = msg[1] & 0x7F;
        uint8_t vel = msg[2] & 0x7F;

        if (status == 0x90 && vel > 0) {
          if (P->learn) {
            P->core.pad[P->sel_pad].midi_note = note;
            P->learn = 0;
            P->shm_dirty = 1;
            P->seq_notify_dirty = 1;
            ep_core_trigger(&P->core, P->sel_pad, vel);
          } else if (P->core.play_mode != EP_PLAY_NORMAL) {
            int matched_slot = -1;
            for (int p = 0; p < EP_PADS; p++) {
              if (P->core.pad[p].midi_note == note) { matched_slot = p; break; }
            }
            if (matched_slot >= 0) {
              uint64_t ev_sample = P->sample_clock + (uint64_t)t;
              uint64_t retrig_samples = (uint64_t)((double)P->core.retrig_ms * 0.001 * P->sr);
              int skip = (P->core.min_vel > 0 && (int)vel < P->core.min_vel) ||
                         (retrig_samples > 0 && (ev_sample - P->last_pad_hit_sample[matched_slot]) < retrig_samples);
              if (!skip) {
                P->last_pad_hit_sample[matched_slot] = ev_sample;
                int eff_vel = vel;
                float po = 0.0f, fo = 0.0f;
                ep_core_trigger_16level(&P->core, P->sel_pad, matched_slot, vel, &eff_vel, &po, &fo);
                ep_core_hold_start(&P->core, matched_slot, P->sel_pad, eff_vel, po, fo);
                if (P->core.seq.mode == EP_SEQ_REC && P->core.seq.countin_rem == 0) {
                  ep_seq_record_hit_ex(&P->core, P->sel_pad, eff_vel, (int)lrintf(po), (int)lrintf(fo));
                  P->shm_dirty = 1;
                  P->seq_notify_dirty = 1;
                }
              }
            } else if (P->core.play_mode == EP_PLAY_CHROM) {
              /* External MIDI keyboard plays sel_pad chromatically relative to its MIDI note */
              int base_note = P->core.pad[P->sel_pad].midi_note;
              float po = (float)((int)note - base_note);
              if (po < -24.0f) po = -24.0f;
              if (po > 24.0f) po = 24.0f;
              ep_core_trigger_ex(&P->core, P->sel_pad, vel, po, 0.0f, 0);
              ep_core_hold_start(&P->core, P->sel_pad, P->sel_pad, vel, po, 0.0f);
              if (P->core.seq.mode == EP_SEQ_REC && P->core.seq.countin_rem == 0) {
                ep_seq_record_hit_ex(&P->core, P->sel_pad, vel, (int)lrintf(po), 0);
                P->shm_dirty = 1;
                P->seq_notify_dirty = 1;
              }
            } else if (note >= 52 && note < 57) {
              ep_seq_request_scene(&P->core, note - 52);
            }
          } else {
            int matched = 0;
            uint64_t ev_sample = P->sample_clock + (uint64_t)t;
            uint64_t retrig_samples = (uint64_t)((double)P->core.retrig_ms * 0.001 * P->sr);
            uint64_t xtalk_samples  = (uint64_t)(0.015 * P->sr);
            for (int p = 0; p < EP_PADS; p++) {
              if (P->core.pad[p].midi_note == note) {
                matched = 1;
                if (P->core.min_vel > 0 && (int)vel < P->core.min_vel) {
                  continue;
                }
                if (retrig_samples > 0 &&
                    (ev_sample - P->last_pad_hit_sample[p]) < retrig_samples) {
                  continue;
                }
                if (P->core.xtalk > 0 &&
                    P->last_any_hit_pad >= 0 && P->last_any_hit_pad != p &&
                    (ev_sample - P->last_any_hit_sample) < xtalk_samples) {
                  int thresh = (P->last_any_hit_vel * P->core.xtalk + 50) / 100;
                  if ((int)vel < thresh) continue;
                }
                P->last_pad_hit_sample[p] = ev_sample;
                if ((int)vel >= P->last_any_hit_vel ||
                    (ev_sample - P->last_any_hit_sample) >= xtalk_samples) {
                  P->last_any_hit_sample = ev_sample;
                  P->last_any_hit_pad = p;
                  P->last_any_hit_vel = vel;
                }
                P->last_hit_pad = p;
                P->hit_seq++;
                if (P->follow && P->sel_pad != p) {
                  P->sel_pad = p;
                  snapshot_prev_controls(P);
                }
                ep_core_trigger(&P->core, p, vel);
                ep_core_hold_start(&P->core, p, p, vel, 0.0f, 0.0f);
                if (P->core.seq.mode == EP_SEQ_REC && P->core.seq.countin_rem == 0) {
                  ep_seq_record_hit(&P->core, p, vel);
                  P->shm_dirty = 1;
                  P->seq_notify_dirty = 1;
                }
              }
            }
            /* Unassigned Bank B notes 52..56 switch Scenes A..E on M-VAVE SMC-PAD */
            if (!matched && note >= 52 && note < 57) {
              ep_seq_request_scene(&P->core, note - 52);
            }
          }
        } else if (status == 0x80 || (status == 0x90 && vel == 0)) {
          if (P->core.play_mode != EP_PLAY_NORMAL) {
            int matched_slot = -1;
            for (int p = 0; p < EP_PADS; p++) {
              if (P->core.pad[p].midi_note == note) { matched_slot = p; break; }
            }
            if (matched_slot >= 0) ep_core_hold_stop(&P->core, matched_slot);
            else ep_core_hold_stop(&P->core, P->sel_pad);
            ep_core_release(&P->core, P->sel_pad);
          } else {
            for (int p = 0; p < EP_PADS; p++) {
              if (P->core.pad[p].midi_note == note) {
                ep_core_hold_stop(&P->core, p);
                ep_core_release(&P->core, p);
              }
            }
          }
        }
      }
    }
  }

  if (pos < nframes) {
    float* al[EP_AUX_BUSES];
    float* ar[EP_AUX_BUSES];
    for (int a = 0; a < EP_AUX_BUSES; a++) {
      al[a] = P->aux_l[a] ? P->aux_l[a] + pos : NULL;
      ar[a] = P->aux_r[a] ? P->aux_r[a] + pos : NULL;
    }
    ep_core_process_aux(&P->core, P->out_l + pos, P->out_r + pos, al, ar, nframes - pos, P->sel_pad);
  }
  P->sample_clock += (uint64_t)nframes;

  if (P->out_sel_pad)   *P->out_sel_pad   = (float)P->sel_pad;
  if (P->out_seq_step)  *P->out_seq_step  = (float)P->core.seq.cur_step;
  if (P->out_seq_scene) *P->out_seq_scene = (float)P->core.seq.scene;
  if (P->out_seq_pend)  *P->out_seq_pend  = (float)P->core.seq.pend_scene;
  if (P->out_playhead)  *P->out_playhead  = P->core.sel_playhead;
  for (int i = 0; i < EP_PADS; i++) {
    if (P->out_lvl[i]) *P->out_lvl[i] = P->core.activity[i];
  }

  shm_publish(P);
  forge_notify_reports(P);
}

static void cleanup(LV2_Handle h) {
  EkaPads* P = (EkaPads*)h;
  if (P->shm) munmap(P->shm, sizeof(EpShm));
  if (P->shm_fd >= 0) close(P->shm_fd);
  if (P->shm_path[0]) unlink(P->shm_path);
  for (int i = 0; i < EP_PADS; i++) ep_sample_free(P->samples[i]);
  for (int i = 0; i < P->nretired; i++) ep_sample_free(P->retired[i].s);
  free(P);
}

/* ---------- LV2 Worker ---------- */

static LV2_Worker_Status work(LV2_Handle h, LV2_Worker_Respond_Function respond,
                              LV2_Worker_Respond_Handle handle, uint32_t size, const void* data) {
  EkaPads* P = (EkaPads*)h;
  if (size < sizeof(uint32_t)) return LV2_WORKER_ERR_UNKNOWN;
  uint32_t type = *(const uint32_t*)data;
  if (type == MSG_FREE && size == sizeof(MsgFree)) {
    const MsgFree* m = (const MsgFree*)data;
    ep_sample_free(m->s);
    return LV2_WORKER_SUCCESS;
  }
  if (type == MSG_LOAD && size > offsetof(MsgLoad, path) && size <= sizeof(MsgLoad)) {
    const MsgLoad* m = (const MsgLoad*)data;
    if (m->pad < 0 || m->pad >= EP_PADS) return LV2_WORKER_SUCCESS;
    if (m->seq != atomic_load_explicit(&P->req_seq[m->pad], memory_order_relaxed))
      return LV2_WORKER_SUCCESS;
    char path_buf[EP_PATH_MAX];
    size_t plen = size - offsetof(MsgLoad, path);
    if (plen >= EP_PATH_MAX) plen = EP_PATH_MAX - 1;
    memcpy(path_buf, m->path, plen);
    path_buf[plen] = '\0';
    int err = EP_LOAD_OK;
    EpSample* s = ep_sample_load(path_buf, EP_MAX_SECONDS, &err);
    MsgDone done = { m->pad, m->seq, err, s };
    respond(handle, sizeof done, &done);
    return LV2_WORKER_SUCCESS;
  }
  return LV2_WORKER_ERR_UNKNOWN;
}

static LV2_Worker_Status work_response(LV2_Handle h, uint32_t size, const void* data) {
  EkaPads* P = (EkaPads*)h;
  if (size != sizeof(MsgDone)) return LV2_WORKER_ERR_UNKNOWN;
  const MsgDone* d = (const MsgDone*)data;
  if (d->pad < 0 || d->pad >= EP_PADS) {
    if (d->s && !free_in_worker(P, d->s)) ep_sample_free(d->s);
    return LV2_WORKER_SUCCESS;
  }
  if (d->seq != atomic_load_explicit(&P->req_seq[d->pad], memory_order_relaxed)) {
    if (d->s && !free_in_worker(P, d->s)) ep_sample_free(d->s);
    return LV2_WORKER_SUCCESS;
  }
  if (d->err == EP_LOAD_OK && d->s) {
    swap_sample(P, d->pad, d->s);
  } else {
    P->paths[d->pad][0] = '\0';
    swap_sample(P, d->pad, NULL);
    P->report_next = 0;
  }
  return LV2_WORKER_SUCCESS;
}

static const LV2_Worker_Interface worker_iface = { work, work_response, NULL };

/* ---------- LV2 State ---------- */

static LV2_State_Status state_save(LV2_Handle h, LV2_State_Store_Function store,
                                   LV2_State_Handle handle, uint32_t flags,
                                   const LV2_Feature* const* features) {
  (void)flags; (void)features;
  EkaPads* P = (EkaPads*)h;
  for (int i = 0; i < EP_PADS; i++) {
    if (P->paths[i][0]) {
      store(handle, P->p_sample[i], P->paths[i], strlen(P->paths[i]) + 1,
            P->atom_Path, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
    }
  }
  int32_t bpm_i = (int32_t)lrint(P->core.seq.bpm);
  int32_t len_i = (int32_t)P->core.seq.num_steps;
  store(handle, P->p_sel_pad, &P->sel_pad, sizeof(int32_t), P->atom_Int,
        LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
  store(handle, P->p_follow, &P->follow, sizeof(int32_t), P->atom_Int,
        LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
  store(handle, P->p_seq_bpm, &bpm_i, sizeof(int32_t), P->atom_Int,
        LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
  store(handle, P->p_seq_len, &len_i, sizeof(int32_t), P->atom_Int,
        LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
  store(handle, P->st_pad_params, P->core.pad, sizeof P->core.pad, P->atom_Chunk,
        LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
  store(handle, P->st_scenes, P->core.scenes, sizeof P->core.scenes, P->atom_Chunk,
        LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
  /* Store seq_data and aux_routes as atom:String (NOT atom:Path!) so lilv never mangles
   * '|' / ':' characters as relative filesystem URIs in .ttl preset files. */
  char sbuf[sizeof P->seq_buf];
  size_t slen = seq_encode(&P->core, sbuf, sizeof sbuf);
  if (slen > 0) {
    store(handle, P->p_seq_data, sbuf, slen + 1, P->atom_String,
          LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
  }
  if (P->aux_routes[0] && strcmp(P->aux_routes, "_") != 0) {
    store(handle, P->p_aux_routes, P->aux_routes, strlen(P->aux_routes) + 1, P->atom_String,
          LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
  }
  return LV2_STATE_SUCCESS;
}

static LV2_State_Status state_restore(LV2_Handle h, LV2_State_Retrieve_Function retrieve,
                                      LV2_State_Handle handle, uint32_t flags,
                                      const LV2_Feature* const* features) {
  (void)flags; (void)features;
  EkaPads* P = (EkaPads*)h;
  size_t sz; uint32_t type, fl;

  /* Start from clean defaults so nothing from a previously loaded kit leaks through */
  P->pending_load_mask = 0;
  ep_core_all_off(&P->core);
  for (int s = 0; s < EP_SCENES; s++) ep_seq_clear_scene(&P->core, s);
  for (int i = 0; i < EP_PADS; i++) ep_pad_defaults(&P->core.pad[i], i);
  P->core.retrig_ms = 25;
  P->core.min_vel = 0;
  P->core.xtalk = 0;
  P->core.play_mode = EP_PLAY_NORMAL;
  P->core.repeat_div = EP_RPT_OFF;
  P->core.choke_mute_mask = 0;
  P->core.choke_solo_mask = 0;
  for (int g = 0; g <= EP_CHOKE_MAX; g++) P->core.choke_env[g] = 1.0f;
  P->core.perf_filter = 0.0f;
  P->core.perf_reso = 20.0f;
  P->core.perf_filter_s = 0.0f;
  P->core.perf_reso_s = 20.0f;
  P->core.perf_fx = EP_FX_NONE;
  P->core.perf_fx_cur = EP_FX_NONE;
  P->core.perf_break = 0;
  P->core.perf_aux1_throw = 0;
  P->core.seq.metro_mode = EP_METRO_OFF;
  P->core.seq.countin_en = 0;
  P->core.seq.countin_rem = 0;
  P->core.seq.fill_ret_scene = -1;
  P->core.seq.chain_on = 0;
  P->core.seq.chain_len = 4;
  P->core.seq.chain_pos = 0;
  P->core.seq.chain[0] = 0; P->core.seq.chain[1] = 0;
  P->core.seq.chain[2] = 1; P->core.seq.chain[3] = 2;
  P->core.seq.chain[4] = 0; P->core.seq.chain[5] = 1;
  P->core.seq.chain[6] = 2; P->core.seq.chain[7] = 3;
  P->core.seq.undo_scene_idx = -1;
  ep_seq_set_quantize(&P->core, 100);
  P->core.seq.bpm = 120.0;
  P->core.seq.num_steps = EP_STEPS_DEF;
  P->sel_pad = 0;
  P->follow = 1;
  P->learn = 0;

  /* 1. Decode ASCII seq_data first (restores quantize, retrig/min_vel/xtalk, and fallback pads/scenes) */
  const void* sd = retrieve(handle, P->p_seq_data, &sz, &type, &fl);
  if (sd && (type == P->atom_String || type == P->atom_Path) && sz > 0) {
    char tmp[sizeof P->seq_buf];
    size_t n = sz < sizeof(tmp) - 1 ? sz : sizeof(tmp) - 1;
    memcpy(tmp, sd, n);
    tmp[n] = '\0';
    seq_decode(&P->core, tmp);
  }

  /* 2. Exact binary chunks always take precedence over ASCII seq_data (lossless & recovers legacy presets) */
  const void* blob = retrieve(handle, P->st_pad_params, &sz, &type, &fl);
  if (blob && sz == sizeof P->core.pad) {
    memcpy(P->core.pad, blob, sz);
  } else if (blob && (sz == EP_PADS * 64 || sz == EP_PADS * 72 || sz == EP_PADS * 80 || sz == EP_PADS * 144)) {
    /* Legacy 64/72/80/144-byte EpPadParams snapshot migration */
    size_t stride = sz / EP_PADS;
    const uint8_t* src = (const uint8_t*)blob;
    for (int i = 0; i < EP_PADS; i++) {
      ep_pad_defaults(&P->core.pad[i], i);
      memcpy(&P->core.pad[i], src + i * stride, stride);
    }
  }

  const void* sc = retrieve(handle, P->st_scenes, &sz, &type, &fl);
  if (sc && sz == sizeof P->core.scenes) {
    memcpy(P->core.scenes, sc, sz);
  } else if (sc && sz == (size_t)(EP_SCENES * (EP_PADS * EP_STEPS * 3 + EP_PADS * 2))) {
    /* Legacy v0.2.x 32-step 3-byte EpStep snapshot migration */
    const uint8_t* raw = (const uint8_t*)sc;
    size_t old_sc_sz = EP_PADS * EP_STEPS * 3 + EP_PADS * 2;
    for (int s = 0; s < EP_SCENES; s++) {
      ep_seq_clear_scene(&P->core, s);
      const uint8_t* sbase = raw + s * old_sc_sz;
      for (int p = 0; p < EP_PADS; p++) {
        for (int st = 0; st < EP_STEPS; st++) {
          size_t off = (size_t)(p * EP_STEPS + st) * 3;
          P->core.scenes[s].steps[p][st].vel = sbase[off];
          P->core.scenes[s].steps[p][st].roll = sbase[off + 1];
          P->core.scenes[s].steps[p][st].chance = sbase[off + 2];
          P->core.scenes[s].steps[p][st].pitch_off = 0;
          P->core.scenes[s].steps[p][st].filt_off = 0;
        }
        P->core.scenes[s].pad_start[p] = sbase[EP_PADS * EP_STEPS * 3 + p];
        P->core.scenes[s].pad_end[p] = sbase[EP_PADS * EP_STEPS * 3 + EP_PADS + p];
      }
    }
  } else if (sc && sz == (size_t)(EP_SCENES * (EP_PADS * EP_STEPS * 2 + EP_PADS * 2))) {
    /* Legacy 32-step 2-byte EpStep snapshot migration */
    const uint8_t* raw = (const uint8_t*)sc;
    size_t old_sc_sz = EP_PADS * EP_STEPS * 2 + EP_PADS * 2;
    for (int s = 0; s < EP_SCENES; s++) {
      ep_seq_clear_scene(&P->core, s);
      const uint8_t* sbase = raw + s * old_sc_sz;
      for (int p = 0; p < EP_PADS; p++) {
        for (int st = 0; st < EP_STEPS; st++) {
          size_t off = (size_t)(p * EP_STEPS + st) * 2;
          P->core.scenes[s].steps[p][st].vel = sbase[off];
          P->core.scenes[s].steps[p][st].roll = sbase[off + 1];
          P->core.scenes[s].steps[p][st].chance = 0;
        }
        P->core.scenes[s].pad_start[p] = sbase[EP_PADS * EP_STEPS * 2 + p];
        P->core.scenes[s].pad_end[p] = sbase[EP_PADS * EP_STEPS * 2 + EP_PADS + p];
      }
    }
  } else if (sc && sz == (size_t)(EP_SCENES * EP_PADS * 16 * 2)) {
    /* Legacy 16-step 2-byte EpStep snapshot migration */
    const uint8_t* raw = (const uint8_t*)sc;
    for (int s = 0; s < EP_SCENES; s++) {
      ep_seq_clear_scene(&P->core, s);
      for (int p = 0; p < EP_PADS; p++) {
        for (int st = 0; st < 16; st++) {
          size_t off = (size_t)((s * EP_PADS + p) * 16 + st) * 2;
          P->core.scenes[s].steps[p][st].vel = raw[off];
          P->core.scenes[s].steps[p][st].roll = raw[off + 1];
          P->core.scenes[s].steps[p][st].chance = 0;
        }
      }
    }
  }

  const void* sp = retrieve(handle, P->p_sel_pad, &sz, &type, &fl);
  if (sp && sz == sizeof(int32_t)) {
    int v = *(const int32_t*)sp;
    if (v >= 0 && v < EP_PADS) P->sel_pad = v;
  }
  const void* fw = retrieve(handle, P->p_follow, &sz, &type, &fl);
  if (fw && sz == sizeof(int32_t)) {
    P->follow = (*(const int32_t*)fw) ? 1 : 0;
  }
  const void* bv = retrieve(handle, P->p_seq_bpm, &sz, &type, &fl);
  if (bv && sz == sizeof(int32_t)) {
    int b = *(const int32_t*)bv;
    if (b >= 40 && b <= 240) P->core.seq.bpm = (double)b;
  }
  const void* lv = retrieve(handle, P->p_seq_len, &sz, &type, &fl);
  if (lv && sz == sizeof(int32_t)) {
    int l = *(const int32_t*)lv;
    if (l >= EP_STEPS_MIN && l <= EP_STEPS) P->core.seq.num_steps = l;
  }

  const void* ar = retrieve(handle, P->p_aux_routes, &sz, &type, &fl);
  if (ar && (type == P->atom_String || type == P->atom_Path) && sz > 0) {
    size_t n = sz < sizeof(P->aux_routes) - 1 ? sz : sizeof(P->aux_routes) - 1;
    memcpy(P->aux_routes, ar, n);
    P->aux_routes[n] = '\0';
  } else {
    P->aux_routes[0] = '\0';
  }

  for (int i = 0; i < EP_PADS; i++) {
    const void* pv = retrieve(handle, P->p_sample[i], &sz, &type, &fl);
    if (pv && (type == P->atom_Path || type == P->atom_String) && sz > 0) {
      char buf[EP_PATH_MAX];
      size_t n = sz < EP_PATH_MAX - 1 ? sz : EP_PATH_MAX - 1;
      memcpy(buf, pv, n);
      buf[n] = '\0';
      if (buf[0] && strcmp(buf, "_") != 0) {
        int err = EP_LOAD_OK;
        EpSample* s = ep_sample_load(buf, EP_MAX_SECONDS, &err);
        snprintf(P->paths[i], EP_PATH_MAX, "%s", buf);
        atomic_fetch_add_explicit(&P->req_seq[i], 1, memory_order_relaxed);
        swap_sample(P, i, (err == EP_LOAD_OK && s) ? s : NULL);
        continue;
      }
    }
    P->paths[i][0] = '\0';
    atomic_fetch_add_explicit(&P->req_seq[i], 1, memory_order_relaxed);
    swap_sample(P, i, NULL);
  }
  P->have_prev_ctl = 0;
  P->have_prev_steps = 0;
  P->shm_dirty = 1;
  shm_publish(P);
  P->report_next = 0;
  P->seq_notify_cd = 0;
  return LV2_STATE_SUCCESS;
}

static const LV2_State_Interface state_iface = { state_save, state_restore };

static const void* extension_data(const char* uri) {
  if (!strcmp(uri, LV2_WORKER__interface)) return &worker_iface;
  if (!strcmp(uri, LV2_STATE__interface))  return &state_iface;
  return NULL;
}

static const LV2_Descriptor descriptor = {
  EP_URI,
  instantiate,
  connect_port,
  activate,
  run,
  NULL,
  cleanup,
  extension_data
};

LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index) {
  return index == 0 ? &descriptor : NULL;
}
