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
#define EP_SHM_VER     9u

/* Control input ports MUST come first (contiguous 0..26) so Zynthian's synthetic
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
  P_MIDI_IN,                                      /* index 27 */
  P_OUT_L,                                        /* index 28 */
  P_OUT_R,                                        /* index 29 */
  P_AUX1_L,                                       /* index 30 */
  P_AUX1_R,                                       /* index 31 */
  P_AUX2_L,                                       /* index 32 */
  P_AUX2_R,                                       /* index 33 */
  P_AUX3_L,                                       /* index 34 */
  P_AUX3_R,                                       /* index 35 */
  P_AUX4_L,                                       /* index 36 */
  P_AUX4_R,                                       /* index 37 */
  P_NOTIFY,                                       /* index 38 */
  P_OUT_SEL_PAD,                                  /* index 39 */
  P_OUT_SEQ_STEP,                                 /* index 40 */
  P_OUT_SEQ_SCENE,                                /* index 41 */
  P_OUT_SEQ_PEND,                                 /* index 42 */
  P_OUT_LVL0,                                     /* index 43..58 (16 ports) */
  P_OUT_PLAYHEAD = P_OUT_LVL0 + EP_PADS           /* index 59 */
};

#define N_CTL     P_END
#define N_PAD_CTL 12

/* Packed seq_cmd operations: (op << 21) | (a << 17) | (b << 13) | (c << 8) | val */
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
  EpPadParams pads[EP_PADS];
  EpScene  scenes[EP_SCENES];
} EpShm;
_Static_assert(sizeof(EpPadParams) == 144, "EpPadParams size must match Python widget");
_Static_assert(sizeof(EpScene) == 1056, "EpScene size must match Python widget");
_Static_assert(sizeof(EpShm) == 10824, "EpShm size must match Python widget");

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
  float prev_pad_steps;
  float prev_pad_beg;
  float prev_pad_aux[EP_AUX_BUSES];
  float prev_pad_lfo_rate[EP_LFOS];
  float prev_pad_mod_amt[2];
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
  LV2_URID p_seq_bpm, p_seq_cmd, p_seq_len, p_seq_data;
  LV2_URID st_pad_params, st_scenes;

  EpSample* samples[EP_PADS];
  char paths[EP_PADS][EP_PATH_MAX];
  atomic_uint req_seq[EP_PADS];

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
  s->sel_pad   = P->sel_pad;
  s->follow    = P->follow;
  s->learn     = P->learn;
  s->seq_mode  = P->core.seq.mode;
  s->seq_scene = P->core.seq.scene;
  s->seq_pend  = P->core.seq.pend_scene;
  s->seq_step  = P->core.seq.cur_step;
  s->bpm       = (float)P->core.seq.bpm;
  s->swing     = P->core.seq.swing;
  s->seq_quant = P->core.seq.quantize;
  s->hit_seq   = P->hit_seq;
  s->last_hit_pad = P->last_hit_pad;
  s->retrig_ms = P->core.retrig_ms;
  s->min_vel   = P->core.min_vel;
  s->xtalk     = P->core.xtalk;
  if (P->shm_dirty) {
    memcpy(s->pads, P->core.pad, sizeof P->core.pad);
    memcpy(s->scenes, P->core.scenes, sizeof P->core.scenes);
    P->shm_dirty = 0;
    s->seq++;
  }
}

static void free_in_worker(EkaPads* P, EpSample* s) {
  if (!s) return;
  if (P->sched) {
    MsgFree fm = { MSG_FREE, s };
    P->sched->schedule_work(P->sched->handle, sizeof fm, &fm);
  } else {
    ep_sample_free(s);
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

  if (!ep_core_gen_in_use(&P->core, pad, old_gen)) {
    free_in_worker(P, old);
    return;
  }
  if (P->nretired >= EP_RETIRED_MAX) {
    ep_core_all_off(&P->core);
    for (int i = 0; i < P->nretired; i++) free_in_worker(P, P->retired[i].s);
    P->nretired = 0;
  }
  P->retired[P->nretired].s = old;
  P->retired[P->nretired].pad = pad;
  P->retired[P->nretired].gen = old_gen;
  P->nretired++;
}

static void collect_retired(EkaPads* P) {
  int w = 0;
  for (int i = 0; i < P->nretired; i++) {
    if (!ep_core_gen_in_use(&P->core, P->retired[i].pad, P->retired[i].gen)) {
      free_in_worker(P, P->retired[i].s);
    } else {
      P->retired[w++] = P->retired[i];
    }
  }
  P->nretired = w;
}

static void request_load(EkaPads* P, int pad, const char* path) {
  if (pad < 0 || pad >= EP_PADS) return;
  if (!path || !*path || strcmp(path, "_") == 0) {
    P->paths[pad][0] = '\0';
    atomic_fetch_add_explicit(&P->req_seq[pad], 1, memory_order_relaxed);
    swap_sample(P, pad, NULL);
    return;
  }
  size_t len = strlen(path);
  if (len >= EP_PATH_MAX) return;
  memcpy(P->paths[pad], path, len + 1);
  uint32_t seq = atomic_fetch_add_explicit(&P->req_seq[pad], 1, memory_order_relaxed) + 1;
  if (P->sched) {
    MsgLoad m;
    m.type = MSG_LOAD;
    m.pad = pad;
    m.seq = seq;
    memcpy(m.path, path, len + 1);
    P->sched->schedule_work(P->sched->handle, sizeof m, &m);
  }
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
        if (step.roll > 0) {
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
                fabsf(cur->aux[3] - def.aux[3]) > 0.2f);
    if (!diff) continue;
    n = snprintf(buf + pos, cap - pos,
                 "|P%x=%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%d,%.1f,%.1f,%.1f,%.1f,%d,%.1f,%d,%d,%d,%.1f,%.1f,%.1f,%.1f",
                 p & 0xF, cur->pitch, cur->decay, cur->filter, cur->level,
                 cur->start, cur->end, cur->mode, cur->punch, cur->drive,
                 cur->pan, cur->vel_sens, cur->choke, cur->width,
                 cur->mute, cur->solo, cur->midi_note,
                 cur->aux[0], cur->aux[1], cur->aux[2], cur->aux[3]);
    if (n < 0 || (size_t)n >= cap - pos) return pos;
    pos += (size_t)n;
  }
  if (c->retrig_ms != 25 || c->min_vel != 0 || c->xtalk != 0) {
    n = snprintf(buf + pos, cap - pos, "|F=%d,%d,%d", c->retrig_ms, c->min_vel, c->xtalk);
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
                 "|M%x=%d,%.2f,%d,%.2f,%d,%d,%.0f,%d,%d,%.0f,%d,%d,%.0f,%d,%d,%.0f",
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
  if (!str || str[0] != 'V' || str[1] != '1') return;
  for (int s = 0; s < EP_SCENES; s++) ep_seq_clear_scene(c, s);

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
      int vel = 0, roll = 0;
      if (*p == '.') { p++; vel = (int)strtol(p, (char**)&p, 10); }
      if (*p == '.') { p++; roll = (int)strtol(p, (char**)&p, 10); }
      if (st >= 0 && st < EP_STEPS && vel > 0) {
        c->scenes[sc].steps[pad][st].vel = (uint8_t)(vel > 127 ? 127 : vel);
        c->scenes[sc].steps[pad][st].roll = (uint8_t)(roll & 0xFF);
      }
    }
  }
  /* Parse optional |P<hex>=... per-pad parameter overrides */
  while (*p == '|') {
    p++;
    if (p[0] == 'P' && p[1] && p[2] == '=') {
      int pad = hex_nibble(p[1]);
      p += 3;
      if (pad >= 0 && pad < EP_PADS) {
        EpPadParams* d = &c->pad[pad];
        float v[20];
        int cnt = 0;
        while (cnt < 20 && *p && *p != '|') {
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
        }
      }
    } else if (p[0] == 'F' && p[1] == '=') {
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
    } else if (p[0] == 'M' && p[1] && p[2] == '=') {
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
          d->lfo_wave[0] = (int)lrintf(mv[0]) % EP_LFO_NWAVES;
          d->lfo_rate[0] = mv[1] < 0.05f ? 0.05f : (mv[1] > 20.0f ? 20.0f : mv[1]);
          d->lfo_wave[1] = (int)lrintf(mv[2]) % EP_LFO_NWAVES;
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
  int op  = (cmd >> 21) & 0xF;
  int a   = (cmd >> 17) & 0xF;
  int b   = (cmd >> 13) & 0xF;
  int c   = (cmd >> 8)  & 0x1F;
  int val = cmd & 0xFF;

  switch (op) {
    case CMD_SET_VEL:
      if (a < EP_SCENES && b < EP_PADS && c < EP_STEPS) {
        P->core.scenes[a].steps[b][c].vel = (uint8_t)(val > 127 ? 127 : val);
        P->shm_dirty = 1;
      }
      break;
    case CMD_SET_ROLL:
      if (a < EP_SCENES && b < EP_PADS && c < EP_STEPS) {
        uint8_t prev = P->core.scenes[a].steps[b][c].roll;
        P->core.scenes[a].steps[b][c].roll = (uint8_t)((prev & 0xFCu) | (uint8_t)(val > 3 ? 3 : val));
        P->shm_dirty = 1;
      }
      break;
    case CMD_CLEAR_PAD:
      ep_seq_clear_pad(&P->core, a, b);
      P->shm_dirty = 1;
      break;
    case CMD_CLEAR_SCENE:
      ep_seq_clear_scene(&P->core, a);
      P->shm_dirty = 1;
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
          P->core.pad[b].lfo_wave[a] = val % EP_LFO_NWAVES;
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
      if (a < EP_MOD_SLOTS && b < EP_PADS) {
        int v = val > 200 ? 200 : val;
        P->core.pad[b].mod_amt[a] = (float)(v - 100);
        P->shm_dirty = 1;
      }
      break;
    default:
      break;
  }
  if (op != CMD_TRIG_PAD) {
    P->seq_notify_dirty = 1;
  }
}

static void handle_patch_set(EkaPads* P, const LV2_Atom_Object* obj) {
  const LV2_Atom* prop = NULL;
  const LV2_Atom* val = NULL;
  lv2_atom_object_get(obj, P->patch_property, &prop, P->patch_value, &val, 0);
  if (!prop || prop->type != P->atom_URID || !val) return;
  LV2_URID u = ((const LV2_Atom_URID*)prop)->body;

  for (int i = 0; i < EP_PADS; i++) {
    if (u == P->p_sample[i] && (val->type == P->atom_Path || val->type == P->atom_String)) {
      const char* s = (const char*)LV2_ATOM_BODY_CONST(val);
      request_load(P, i, s);
      return;
    }
  }
  if (u == P->p_seq_data && (val->type == P->atom_String || val->type == P->atom_Path)) {
    const char* s = (const char*)LV2_ATOM_BODY_CONST(val);
    seq_decode(&P->core, s);
    P->shm_dirty = 1;
    return;
  }

  int iv = 0;
  if (!atom_to_int(P, val, &iv)) return;

  if (u == P->p_sel_pad) {
    if (iv >= 0 && iv < EP_PADS) P->sel_pad = iv;
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
    if (iv < 5) iv = 5;
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
      if (P->report_next >= EP_PADS + 7) {
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
        lv2_atom_forge_string(&P->forge, P->seq_buf, (uint32_t)slen);
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

  collect_retired(P);

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
    P->have_prev_steps = 1;
  } else {
    int sc = P->core.seq.scene;
    int pd = P->sel_pad;
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
                if (P->follow) P->sel_pad = p;
                ep_core_trigger(&P->core, p, vel);
                if (P->core.seq.mode == EP_SEQ_REC) {
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
          for (int p = 0; p < EP_PADS; p++) {
            if (P->core.pad[p].midi_note == note) {
              ep_core_release(&P->core, p);
            }
          }
        }
      } else if (ev->body.type == P->atom_Object) {
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
  if (type == MSG_LOAD && size == sizeof(MsgLoad)) {
    const MsgLoad* m = (const MsgLoad*)data;
    if (m->pad < 0 || m->pad >= EP_PADS) return LV2_WORKER_SUCCESS;
    if (m->seq != atomic_load_explicit(&P->req_seq[m->pad], memory_order_relaxed))
      return LV2_WORKER_SUCCESS;
    int err = EP_LOAD_OK;
    EpSample* s = ep_sample_load(m->path, EP_MAX_SECONDS, &err);
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
    free_in_worker(P, d->s);
    return LV2_WORKER_SUCCESS;
  }
  if (d->seq != atomic_load_explicit(&P->req_seq[d->pad], memory_order_relaxed)) {
    free_in_worker(P, d->s);
    return LV2_WORKER_SUCCESS;
  }
  if (d->err == EP_LOAD_OK && d->s) {
    swap_sample(P, d->pad, d->s);
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
  size_t slen = seq_encode(&P->core, P->seq_buf, sizeof P->seq_buf);
  if (slen > 0) {
    store(handle, P->p_seq_data, P->seq_buf, slen + 1, P->atom_String,
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

  const void* blob = retrieve(handle, P->st_pad_params, &sz, &type, &fl);
  if (blob && sz == sizeof P->core.pad) {
    memcpy(P->core.pad, blob, sz);
  } else if (blob && (sz == EP_PADS * 64 || sz == EP_PADS * 72 || sz == EP_PADS * 80)) {
    /* Legacy 64/72/80-byte EpPadParams snapshot migration */
    size_t stride = sz / EP_PADS;
    const uint8_t* src = (const uint8_t*)blob;
    for (int i = 0; i < EP_PADS; i++) {
      ep_pad_defaults(&P->core.pad[i], i);
      memcpy(&P->core.pad[i], src + i * stride, stride);
    }
  }
  const void* sd = retrieve(handle, P->p_seq_data, &sz, &type, &fl);
  if (sd && (type == P->atom_String || type == P->atom_Path) && sz > 0) {
    char tmp[sizeof P->seq_buf];
    size_t n = sz < sizeof(tmp) - 1 ? sz : sizeof(tmp) - 1;
    memcpy(tmp, sd, n);
    tmp[n] = '\0';
    seq_decode(&P->core, tmp);
  } else {
    const void* sc = retrieve(handle, P->st_scenes, &sz, &type, &fl);
    if (sc && sz == sizeof P->core.scenes) {
      memcpy(P->core.scenes, sc, sz);
    } else if (sc && sz == (size_t)(EP_SCENES * EP_PADS * 16 * sizeof(EpStep))) {
      /* Legacy 16-step snapshot migration */
      const EpStep* old = (const EpStep*)sc;
      for (int s = 0; s < EP_SCENES; s++) {
        ep_seq_clear_scene(&P->core, s);
        for (int p = 0; p < EP_PADS; p++) {
          memcpy(P->core.scenes[s].steps[p], old + (s * EP_PADS + p) * 16, 16 * sizeof(EpStep));
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

  for (int i = 0; i < EP_PADS; i++) {
    const void* pv = retrieve(handle, P->p_sample[i], &sz, &type, &fl);
    if (pv && (type == P->atom_Path || type == P->atom_String) && sz > 0 && sz < EP_PATH_MAX) {
      char buf[EP_PATH_MAX];
      memcpy(buf, pv, sz);
      buf[sz - 1] = '\0';
      if (buf[0] && strcmp(buf, "_") != 0) {
        int err = EP_LOAD_OK;
        EpSample* s = ep_sample_load(buf, EP_MAX_SECONDS, &err);
        snprintf(P->paths[i], EP_PATH_MAX, "%s", buf);
        if (err == EP_LOAD_OK && s) {
          swap_sample(P, i, s);
        }
      }
    }
  }
  P->shm_dirty = 1;
  shm_publish(P);
  P->report_next = 0;
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
