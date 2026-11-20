/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Eka Pulse: LV2 wrapper around pulse_fx.
 *
 * 5 encoder pages (20 input control ports):
 *   Main (Slot, Mix, Low Split, Scene)
 *   FX (Mode, Param 1, Param 2, Depth)            -- selected slot
 *   Curve (Pattern, Step Rate, Steps, Warp)       -- selected slot
 *   Trigger (Trig, Threshold, Env->Depth, Smooth) -- selected slot
 *   Perform (Live Pad, Prev Scene, Next Scene, Output dB)
 *
 * Per-slot settings and 16-step levels/shapes are LV2 patch:writable parameters (pprops:notOnGUI).
 * Turning an FX/Curve/Trigger knob edits the selected slot and reports patch:Set back on notify.
 * Changing Slot does not overwrite the new slot with stale knob values; the widget syncs the
 * knobs from output monitors sel and cur_* (same mechanism as Eka Grain Flow). */
#include <lv2/atom/atom.h>
#include <lv2/atom/forge.h>
#include <lv2/atom/util.h>
#include <lv2/core/lv2.h>
#include <lv2/midi/midi.h>
#include <lv2/patch/patch.h>
#include <lv2/state/state.h>
#include <lv2/time/time.h>
#include <lv2/urid/urid.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pulse_fx.h"

#define PULSE_URI "http://dmtry3.github.io/zynthian-eka/pulse"

/* Port indices MUST match scripts/gen_ttl.py. Control input ports come first (0..19). */
enum {
  P_SLOT = 0, P_MIX, P_LOW_SPLIT, P_SCENE,           /* Main */
  P_FACE0,                                           /* FX (4), Curve (4), Trigger (4) */
  P_LIVE_PAD = P_FACE0 + 12, P_PREV_SCENE, P_NEXT_SCENE, P_OUTPUT_DB, /* Perform */
  P_CTL_END,
  P_IN_L = P_CTL_END, P_IN_R, P_OUT_L, P_OUT_R,
  P_CONTROL, P_NOTIFY,
  P_PEAK, P_OUT_PEAK, P_LIM_GR, P_LOADS, P_BPM, P_SEL, P_ON_MASK, P_CUR_SCENE,
  P_PHASE0,
  P_VAL0 = P_PHASE0 + PX_SLOTS,
  P_STEPS0 = P_VAL0 + PX_SLOTS,
  P_MODE0 = P_STEPS0 + PX_SLOTS,
  P_CUR0 = P_MODE0 + PX_SLOTS,
  P_END = P_CUR0 + 12
};

/* The 12 face parameters per slot (order matches FX, Curve, Trigger pages). */
enum {
  F_MODE = 0, F_PARAM1, F_PARAM2, F_DEPTH,
  F_PATTERN, F_STEP_RATE, F_STEPS, F_WARP,
  F_TRIG, F_THRESHOLD, F_ENV_DEPTH, F_SMOOTH,
  NFACES
};

static const char* const face_names[NFACES] = {
  "mode", "param1", "param2", "depth",
  "pattern", "step_rate", "steps", "warp",
  "trig", "threshold", "env_depth", "smooth"
};
static const float face_min[NFACES] = { 0, 0, 0, -100, 0, 0, 1, -100, 0, -60, -100, -100 };
static const float face_max[NFACES] = { 6, 100, 100, 100, PX_NPATTERNS - 1, PX_NRATES - 1, PX_MAX_STEPS, 100, PX_NTRIGS - 1, 0, 100, 100 };
static const int   face_is_int[NFACES] = { 1, 0, 0, 0, 1, 1, 1, 0, 1, 0, 0, 0 };

static const float slot_face_def[PX_SLOTS][NFACES] = {
  { 0, 50, 50, 100, 20, 2, 16, 0, 0, -24, 0, 10 }, /* 1: Time */
  { 0, 50, 50, 100, 21, 2,  5, 0, 0, -24, 0, 10 }, /* 2: Drive */
  { 0, 50, 30, 100, 22, 2,  7, 0, 0, -24, 0, 10 }, /* 3: Filter */
  { 0,  0, 50, 100,  2, 2,  8, 0, 0, -24, 0, 10 }, /* 4: Gate */
};

/* Per-slot layout inside q[] (45 params per slot):
 *   0       : on
 *   1..12   : 12 faces
 *   13..28  : lvl1..lvl16 (0..100)
 *   29..44  : shp1..shp16 (0..5)
 */
#define Q_SLOT_ON   0
#define Q_SLOT_FACE 1
#define Q_SLOT_LVL  (Q_SLOT_FACE + NFACES)
#define Q_SLOT_SHP  (Q_SLOT_LVL + PX_MAX_STEPS)
#define Q_PER_SLOT  (Q_SLOT_SHP + PX_MAX_STEPS)

enum {
  Q_LOW_DUCK = Q_PER_SLOT * PX_SLOTS,
  Q_FREE_HZ,
  NQ_SCENE,                    /* 0..NQ_SCENE-1 are stored per Scene A..D */
  Q_XY_X = NQ_SCENE,
  Q_XY_Y,
  Q_XY_ON,
  Q_STORE_SCENE,
  NQ
};

#define NSCENES 4
#define REPORT_PER_RUN 16

typedef struct {
  float* port[P_END];
  LV2_URID_Map* map;
  LV2_Atom_Forge forge;
  LV2_URID atom_Float, atom_Int, atom_Double, atom_Long, atom_Object, atom_Blank, atom_URID, atom_Chunk;
  LV2_URID midi_Event;
  LV2_URID patch_Set, patch_Get, patch_property, patch_value;
  LV2_URID time_Position, time_bpm, time_speed, time_bar, time_barBeat, time_bpb;
  LV2_URID urid_scenes_blob, urid_cur_scene;
  int bpb;
  int rolling;
  LV2_URID q_urid[NQ];
  float q[NQ];
  float scenes[NSCENES][NQ_SCENE];
  int cur_scene;
  int scene_seen;
  int prev_pedal_hi;
  int next_pedal_hi;
  unsigned char dirty[NQ];
  int report_next;
  int sel;                     /* selected slot 0..3 */
  float face[NFACES];
  int face_seen;
  float loads;
  PxParams params;
  PulseFx fx;
} Plugin;

static float clampf(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

static int q_slot_idx(int s, int off) {
  return s * Q_PER_SLOT + off;
}

static void q_name(int i, char* out, size_t n) {
  if (i == Q_LOW_DUCK)    { snprintf(out, n, "low_duck");    return; }
  if (i == Q_FREE_HZ)     { snprintf(out, n, "free_hz");     return; }
  if (i == Q_XY_X)        { snprintf(out, n, "xy_x");        return; }
  if (i == Q_XY_Y)        { snprintf(out, n, "xy_y");        return; }
  if (i == Q_XY_ON)       { snprintf(out, n, "xy_on");       return; }
  if (i == Q_STORE_SCENE) { snprintf(out, n, "store_scene"); return; }
  int s = i / Q_PER_SLOT;
  int k = i % Q_PER_SLOT;
  if (k == Q_SLOT_ON) {
    snprintf(out, n, "s%d_on", s + 1);
  } else if (k < Q_SLOT_LVL) {
    snprintf(out, n, "s%d_%s", s + 1, face_names[k - Q_SLOT_FACE]);
  } else if (k < Q_SLOT_SHP) {
    snprintf(out, n, "s%d_lvl%d", s + 1, k - Q_SLOT_LVL + 1);
  } else {
    snprintf(out, n, "s%d_shp%d", s + 1, k - Q_SLOT_SHP + 1);
  }
}

static int q_is_int(int i) {
  if (i == Q_LOW_DUCK || i == Q_XY_ON || i == Q_STORE_SCENE) return 1;
  if (i == Q_FREE_HZ || i == Q_XY_X || i == Q_XY_Y) return 0;
  int k = i % Q_PER_SLOT;
  if (k == Q_SLOT_ON) return 1;
  if (k < Q_SLOT_LVL) return face_is_int[k - Q_SLOT_FACE];
  if (k < Q_SLOT_SHP) return 0; /* lvl */
  return 1; /* shp */
}

static float q_clamp(int i, float v) {
  if (!isfinite(v)) v = 0.0f;
  if (i == Q_LOW_DUCK || i == Q_XY_ON) return v >= 0.5f ? 1.0f : 0.0f;
  if (i == Q_FREE_HZ)     return clampf(v, 0.25f, 32.0f);
  if (i == Q_XY_X || i == Q_XY_Y) return clampf(v, 0.0f, 100.0f);
  if (i == Q_STORE_SCENE) return clampf(roundf(v), 0.0f, (float)NSCENES);
  int k = i % Q_PER_SLOT;
  if (k == Q_SLOT_ON) return v >= 0.5f ? 1.0f : 0.0f;
  if (k < Q_SLOT_LVL) {
    int f = k - Q_SLOT_FACE;
    float c = clampf(v, face_min[f], face_max[f]);
    return face_is_int[f] ? roundf(c) : c;
  }
  if (k < Q_SLOT_SHP) return clampf(v, 0.0f, 100.0f);
  return clampf(roundf(v), 0.0f, (float)(PX_NSHAPES - 1));
}

static void apply_slot_pattern(Plugin* P, int s, int pat, int mark_dirty) {
  if (pat < 1 || pat >= PX_NPATTERNS) return;
  float lvls[PX_MAX_STEPS];
  int   shps[PX_MAX_STEPS];
  px_apply_pattern(pat, lvls, shps);
  for (int k = 0; k < PX_MAX_STEPS; k++) {
    int il = q_slot_idx(s, Q_SLOT_LVL + k);
    int is = q_slot_idx(s, Q_SLOT_SHP + k);
    P->q[il] = lvls[k];
    P->q[is] = (float)shps[k];
    if (mark_dirty) {
      P->dirty[il] = 1;
      P->dirty[is] = 1;
    }
  }
}

static void q_defaults(Plugin* P) {
  for (int s = 0; s < PX_SLOTS; s++) {
    P->q[q_slot_idx(s, Q_SLOT_ON)] = (s == PX_SLOT_GATE) ? 1.0f : 0.0f;
    for (int f = 0; f < NFACES; f++) {
      P->q[q_slot_idx(s, Q_SLOT_FACE + f)] = slot_face_def[s][f];
    }
    apply_slot_pattern(P, s, (int)slot_face_def[s][F_PATTERN], 0);
  }
  P->q[Q_LOW_DUCK] = 0.0f;
  P->q[Q_FREE_HZ] = 4.0f;
  P->q[Q_XY_X] = 50.0f;
  P->q[Q_XY_Y] = 50.0f;
  P->q[Q_XY_ON] = 0.0f;
  P->q[Q_STORE_SCENE] = 0.0f;
  for (int sc = 0; sc < NSCENES; sc++) {
    memcpy(P->scenes[sc], P->q, sizeof(float) * NQ_SCENE);
  }
  P->cur_scene = 0;
  P->scene_seen = 0;
}

static void switch_scene(Plugin* P, int target) {
  if (target < 0) target = 0;
  if (target >= NSCENES) target = NSCENES - 1;
  if (target == P->cur_scene) return;
  /* Save current scene state, load target scene state */
  memcpy(P->scenes[P->cur_scene], P->q, sizeof(float) * NQ_SCENE);
  P->cur_scene = target;
  memcpy(P->q, P->scenes[target], sizeof(float) * NQ_SCENE);
  P->face_seen = 0;
  P->loads += 1.0f;
  P->report_next = 0;
}

static void apply_params(Plugin* P) {
  PxParams* p = &P->params;
  for (int s = 0; s < PX_SLOTS; s++) {
    const float* base = &P->q[q_slot_idx(s, 0)];
    PxSlotParams* sp = &p->slot[s];
    sp->on           = base[Q_SLOT_ON] >= 0.5f;
    sp->mode         = (int)base[Q_SLOT_FACE + F_MODE];
    sp->param1       = base[Q_SLOT_FACE + F_PARAM1];
    sp->param2       = base[Q_SLOT_FACE + F_PARAM2];
    sp->depth        = base[Q_SLOT_FACE + F_DEPTH] * 0.01f;
    sp->pattern      = (int)base[Q_SLOT_FACE + F_PATTERN];
    sp->step_rate    = (int)base[Q_SLOT_FACE + F_STEP_RATE];
    sp->steps        = (int)base[Q_SLOT_FACE + F_STEPS];
    sp->warp         = base[Q_SLOT_FACE + F_WARP] * 0.01f;
    sp->trig         = (int)base[Q_SLOT_FACE + F_TRIG];
    sp->threshold_db = base[Q_SLOT_FACE + F_THRESHOLD];
    sp->env_depth    = base[Q_SLOT_FACE + F_ENV_DEPTH] * 0.01f;
    sp->smooth       = base[Q_SLOT_FACE + F_SMOOTH] * 0.01f;
    for (int k = 0; k < PX_MAX_STEPS; k++) {
      sp->levels[k] = base[Q_SLOT_LVL + k] * 0.01f;
      sp->shapes[k] = (int)base[Q_SLOT_SHP + k];
    }
  }
  p->mix          = P->port[P_MIX] ? clampf(*P->port[P_MIX], 0.0f, 100.0f) * 0.01f : 1.0f;
  p->low_split_hz = P->port[P_LOW_SPLIT] ? clampf(*P->port[P_LOW_SPLIT], 0.0f, 500.0f) : 0.0f;
  p->low_duck     = P->q[Q_LOW_DUCK] >= 0.5f;
  p->free_hz      = P->q[Q_FREE_HZ];
  p->output_db    = P->port[P_OUTPUT_DB] ? clampf(*P->port[P_OUTPUT_DB], -24.0f, 6.0f) : 0.0f;
  p->live_pad     = P->port[P_LIVE_PAD] ? (int)lrintf(clampf(*P->port[P_LIVE_PAD], 0.0f, 8.0f)) : 0;
  p->xy_x         = P->q[Q_XY_X] * 0.01f;
  p->xy_y         = P->q[Q_XY_Y] * 0.01f;
  p->xy_on        = P->q[Q_XY_ON] >= 0.5f;
}

static LV2_Handle instantiate(const LV2_Descriptor* d, double rate, const char* bundle,
                              const LV2_Feature* const* f) {
  (void)d; (void)bundle;
  Plugin* P = (Plugin*)calloc(1, sizeof(Plugin));
  if (!P) return NULL;
  for (int i = 0; f && f[i]; i++) {
    if (!strcmp(f[i]->URI, LV2_URID__map)) P->map = (LV2_URID_Map*)f[i]->data;
  }
  if (!P->map || px_init(&P->fx, rate)) { free(P); return NULL; }

  LV2_URID_Map* m = P->map;
  P->atom_Float     = m->map(m->handle, LV2_ATOM__Float);
  P->atom_Int       = m->map(m->handle, LV2_ATOM__Int);
  P->atom_Double    = m->map(m->handle, LV2_ATOM__Double);
  P->atom_Long      = m->map(m->handle, LV2_ATOM__Long);
  P->atom_Object    = m->map(m->handle, LV2_ATOM__Object);
  P->atom_Blank     = m->map(m->handle, LV2_ATOM__Blank);
  P->atom_URID      = m->map(m->handle, LV2_ATOM__URID);
  P->atom_Chunk     = m->map(m->handle, LV2_ATOM__Chunk);
  P->midi_Event     = m->map(m->handle, LV2_MIDI__MidiEvent);
  P->patch_Set      = m->map(m->handle, LV2_PATCH__Set);
  P->patch_Get      = m->map(m->handle, LV2_PATCH__Get);
  P->patch_property = m->map(m->handle, LV2_PATCH__property);
  P->patch_value    = m->map(m->handle, LV2_PATCH__value);
  P->time_Position  = m->map(m->handle, LV2_TIME__Position);
  P->time_bpm       = m->map(m->handle, LV2_TIME__beatsPerMinute);
  P->time_speed     = m->map(m->handle, LV2_TIME__speed);
  P->time_bar       = m->map(m->handle, LV2_TIME__bar);
  P->time_barBeat   = m->map(m->handle, LV2_TIME__barBeat);
  P->time_bpb       = m->map(m->handle, LV2_TIME__beatsPerBar);
  P->urid_scenes_blob = m->map(m->handle, PULSE_URI "#scenes_blob");
  P->urid_cur_scene   = m->map(m->handle, PULSE_URI "#cur_scene");
  P->bpb            = 4;

  for (int i = 0; i < NQ; i++) {
    char name[32], uri[128];
    q_name(i, name, sizeof name);
    snprintf(uri, sizeof uri, PULSE_URI "#%s", name);
    P->q_urid[i] = m->map(m->handle, uri);
  }
  lv2_atom_forge_init(&P->forge, m);
  q_defaults(P);
  P->sel = PX_SLOT_GATE;
  P->report_next = -1;
  return P;
}

static void connect_port(LV2_Handle h, uint32_t port, void* data) {
  Plugin* P = (Plugin*)h;
  if (port < P_END) P->port[port] = (float*)data;
}

static void activate(LV2_Handle h) {
  Plugin* P = (Plugin*)h;
  px_reset(&P->fx);
  P->rolling = 0;
  P->face_seen = 0;
}

static int q_by_urid(Plugin* P, LV2_URID u) {
  for (int i = 0; i < NQ; i++) if (P->q_urid[i] == u) return i;
  return -1;
}

static int atom_number(Plugin* P, const LV2_Atom* a, float* out) {
  if (a->type == P->atom_Float)  { *out = ((const LV2_Atom_Float*)a)->body; return 1; }
  if (a->type == P->atom_Int)    { *out = (float)((const LV2_Atom_Int*)a)->body; return 1; }
  if (a->type == P->atom_Double) { *out = (float)((const LV2_Atom_Double*)a)->body; return 1; }
  if (a->type == P->atom_Long)   { *out = (float)((const LV2_Atom_Long*)a)->body; return 1; }
  return 0;
}

static void read_events(Plugin* P) {
  const LV2_Atom_Sequence* seq = (const LV2_Atom_Sequence*)P->port[P_CONTROL];
  if (!seq) return;
  LV2_ATOM_SEQUENCE_FOREACH(seq, ev) {
    if (ev->body.type == P->midi_Event) {
      if (ev->body.size >= 3) {
        const uint8_t* msg = (const uint8_t*)LV2_ATOM_BODY_CONST(&ev->body);
        if ((msg[0] & 0xF0) == 0x90 && msg[2] > 0) px_midi_trigger(&P->fx);
      }
      continue;
    }
    if (ev->body.type != P->atom_Object && ev->body.type != P->atom_Blank) continue;
    const LV2_Atom_Object* obj = (const LV2_Atom_Object*)&ev->body;
    if (obj->body.otype == P->patch_Get) { P->report_next = 0; continue; }
    if (obj->body.otype == P->time_Position) {
      const LV2_Atom *bpm = NULL, *speed = NULL, *bar = NULL, *bb = NULL, *bpb = NULL;
      lv2_atom_object_get(obj, P->time_bpm, &bpm, P->time_speed, &speed, P->time_bar, &bar,
                          P->time_barBeat, &bb, P->time_bpb, &bpb, 0);
      float v;
      if (bpb && atom_number(P, bpb, &v) && v >= 1.0f && v <= 16.0f) P->bpb = (int)lrintf(v);
      float tempo = 0.0f;
      if (bpm && atom_number(P, bpm, &v) && v >= 20.0f && v <= 400.0f) tempo = v;
      if (speed && atom_number(P, speed, &v)) P->rolling = (v != 0.0f);
      float br = 0.0f, beat = 0.0f;
      int has_pos = bar && bb && atom_number(P, bar, &br) && atom_number(P, bb, &beat);
      double beats = has_pos ? (double)br * (double)P->bpb + (double)beat : 0.0;
      px_set_transport(&P->fx, tempo, P->rolling, has_pos, beats, P->bpb);
      continue;
    }
    if (obj->body.otype != P->patch_Set) continue;
    const LV2_Atom* prop = NULL;
    const LV2_Atom* val = NULL;
    lv2_atom_object_get(obj, P->patch_property, &prop, P->patch_value, &val, 0);
    if (!prop || !val || prop->type != P->atom_URID) continue;
    int i = q_by_urid(P, ((const LV2_Atom_URID*)prop)->body);
    float v;
    if (i >= 0 && atom_number(P, val, &v)) {
      float nv = q_clamp(i, v);
      if (i == Q_STORE_SCENE) {
        int dst = (int)nv;
        if (dst >= 1 && dst <= NSCENES) {
          memcpy(P->scenes[dst - 1], P->q, sizeof(float) * NQ_SCENE);
        }
        P->q[Q_STORE_SCENE] = 0.0f;
        P->dirty[Q_STORE_SCENE] = 1;
        continue;
      }
      if (nv != P->q[i]) {
        P->q[i] = nv;
        if (i < Q_PER_SLOT * PX_SLOTS) {
          int s = i / Q_PER_SLOT;
          int k = i % Q_PER_SLOT;
          if (k == Q_SLOT_FACE + F_PATTERN && (int)nv >= 1) {
            apply_slot_pattern(P, s, (int)nv, 1);
          } else if (k >= Q_SLOT_LVL) {
            int ip = q_slot_idx(s, Q_SLOT_FACE + F_PATTERN);
            if (P->q[ip] != 0.0f) {
              P->q[ip] = 0.0f;
              P->dirty[ip] = 1;
            }
          }
        }
      }
    }
  }
}

static void forge_set(Plugin* P, int i) {
  LV2_Atom_Forge_Frame of;
  lv2_atom_forge_frame_time(&P->forge, 0);
  lv2_atom_forge_object(&P->forge, &of, 0, P->patch_Set);
  lv2_atom_forge_key(&P->forge, P->patch_property);
  lv2_atom_forge_urid(&P->forge, P->q_urid[i]);
  lv2_atom_forge_key(&P->forge, P->patch_value);
  if (q_is_int(i)) lv2_atom_forge_int(&P->forge, (int32_t)P->q[i]);
  else lv2_atom_forge_float(&P->forge, P->q[i]);
  lv2_atom_forge_pop(&P->forge, &of);
}

static void write_notify(Plugin* P) {
  LV2_Atom_Sequence* seq = (LV2_Atom_Sequence*)P->port[P_NOTIFY];
  if (!seq) return;
  const uint32_t cap = seq->atom.size;
  lv2_atom_forge_set_buffer(&P->forge, (uint8_t*)seq, cap);
  LV2_Atom_Forge_Frame frame;
  lv2_atom_forge_sequence_head(&P->forge, &frame, 0);
  int sent = 0;
  for (int i = 0; i < NQ && sent < REPORT_PER_RUN; i++) {
    if (P->dirty[i]) {
      P->dirty[i] = 0;
      forge_set(P, i);
      sent++;
    }
  }
  for (; sent < REPORT_PER_RUN && P->report_next >= 0; sent++) {
    forge_set(P, P->report_next);
    P->report_next = (P->report_next + 1 < NQ) ? P->report_next + 1 : -1;
  }
  lv2_atom_forge_pop(&P->forge, &frame);
}

static void read_faces(Plugin* P) {
  int sel = P->port[P_SLOT] ? (int)lrintf(clampf(*P->port[P_SLOT], 1.0f, (float)PX_SLOTS)) - 1 : PX_SLOT_GATE;
  float face[NFACES];
  for (int f = 0; f < NFACES; f++) {
    face[f] = P->port[P_FACE0 + f] ? *P->port[P_FACE0 + f] : P->q[q_slot_idx(sel, Q_SLOT_FACE + f)];
  }
  if (!P->face_seen || sel != P->sel) {
    P->sel = sel;
    memcpy(P->face, face, sizeof face);
    P->face_seen = 1;
    return;
  }
  for (int f = 0; f < NFACES; f++) {
    if (face[f] == P->face[f]) continue;
    P->face[f] = face[f];
    int i = q_slot_idx(sel, Q_SLOT_FACE + f);
    float nv = q_clamp(i, face[f]);
    if (nv != P->q[i]) {
      P->q[i] = nv;
      P->dirty[i] = 1;
      if (f == F_PATTERN && (int)nv >= 1) {
        apply_slot_pattern(P, sel, (int)nv, 1);
      }
    }
  }
}

static void read_scenes(Plugin* P) {
  int prev_hi = P->port[P_PREV_SCENE] ? (*P->port[P_PREV_SCENE] >= 0.5f) : 0;
  int next_hi = P->port[P_NEXT_SCENE] ? (*P->port[P_NEXT_SCENE] >= 0.5f) : 0;
  if (prev_hi && !P->prev_pedal_hi) {
    switch_scene(P, (P->cur_scene + NSCENES - 1) % NSCENES);
    P->scene_seen = 0; /* wait for host/widget to update P_SCENE */
  } else if (next_hi && !P->next_pedal_hi) {
    switch_scene(P, (P->cur_scene + 1) % NSCENES);
    P->scene_seen = 0;
  }
  P->prev_pedal_hi = prev_hi;
  P->next_pedal_hi = next_hi;

  if (P->port[P_SCENE]) {
    int req = (int)lrintf(clampf(*P->port[P_SCENE], 0.0f, (float)(NSCENES - 1)));
    if (!P->scene_seen) {
      if (req == P->cur_scene) P->scene_seen = 1;
    } else if (req != P->cur_scene) {
      switch_scene(P, req);
    }
  }
}

static void run(LV2_Handle h, uint32_t n) {
  Plugin* P = (Plugin*)h;
  read_events(P);
  read_scenes(P);
  read_faces(P);
  apply_params(P);
  px_process(&P->fx, &P->params, P->port[P_IN_L], P->port[P_IN_R], P->port[P_OUT_L], P->port[P_OUT_R], n);
  write_notify(P);

  if (P->port[P_PEAK])      *P->port[P_PEAK]      = P->fx.in_peak;
  if (P->port[P_OUT_PEAK])  *P->port[P_OUT_PEAK]  = P->fx.out_peak;
  if (P->port[P_LIM_GR])    *P->port[P_LIM_GR]    = P->fx.lim_gr;
  if (P->port[P_LOADS])     *P->port[P_LOADS]     = P->loads;
  if (P->port[P_BPM])       *P->port[P_BPM]       = P->fx.bpm;
  if (P->port[P_SEL])       *P->port[P_SEL]       = (float)(P->sel + 1);
  if (P->port[P_CUR_SCENE]) *P->port[P_CUR_SCENE] = (float)P->cur_scene;

  int mask = 0;
  for (int s = 0; s < PX_SLOTS; s++) {
    if (P->q[q_slot_idx(s, Q_SLOT_ON)] >= 0.5f) mask |= 1 << s;
    if (P->port[P_PHASE0 + s]) *P->port[P_PHASE0 + s] = P->fx.st[s].phase;
    if (P->port[P_VAL0 + s])   *P->port[P_VAL0 + s]   = P->fx.st[s].curve_smooth;
    if (P->port[P_STEPS0 + s]) *P->port[P_STEPS0 + s] = P->q[q_slot_idx(s, Q_SLOT_FACE + F_STEPS)];
    if (P->port[P_MODE0 + s])  *P->port[P_MODE0 + s]  = P->q[q_slot_idx(s, Q_SLOT_FACE + F_MODE)];
  }
  if (P->port[P_ON_MASK]) *P->port[P_ON_MASK] = (float)mask;

  for (int f = 0; f < NFACES; f++) {
    if (P->port[P_CUR0 + f]) *P->port[P_CUR0 + f] = P->q[q_slot_idx(P->sel, Q_SLOT_FACE + f)];
  }
}

static void cleanup(LV2_Handle h) {
  Plugin* P = (Plugin*)h;
  px_free(&P->fx);
  free(P);
}

static LV2_State_Status save(LV2_Handle h, LV2_State_Store_Function store, LV2_State_Handle sh,
                             uint32_t flags, const LV2_Feature* const* f) {
  (void)flags; (void)f;
  Plugin* P = (Plugin*)h;
  /* Keep current scene up to date before saving */
  memcpy(P->scenes[P->cur_scene], P->q, sizeof(float) * NQ_SCENE);
  for (int i = 0; i < NQ; i++) {
    if (q_is_int(i)) {
      int32_t v = (int32_t)P->q[i];
      store(sh, P->q_urid[i], &v, sizeof v, P->atom_Int, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
    } else {
      store(sh, P->q_urid[i], &P->q[i], sizeof(float), P->atom_Float, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
    }
  }
  int32_t cs = (int32_t)P->cur_scene;
  store(sh, P->urid_cur_scene, &cs, sizeof cs, P->atom_Int, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
  store(sh, P->urid_scenes_blob, P->scenes, sizeof P->scenes, P->atom_Chunk,
        LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
  return LV2_STATE_SUCCESS;
}

static LV2_State_Status restore(LV2_Handle h, LV2_State_Retrieve_Function retrieve, LV2_State_Handle sh,
                                uint32_t flags, const LV2_Feature* const* f) {
  (void)flags; (void)f;
  Plugin* P = (Plugin*)h;
  q_defaults(P);
  unsigned char has_lvl[PX_SLOTS] = { 0 };
  for (int i = 0; i < NQ; i++) {
    size_t size; uint32_t type, fl;
    const void* d = retrieve(sh, P->q_urid[i], &size, &type, &fl);
    if (!d) continue;
    if (type == P->atom_Float && size == sizeof(float))        P->q[i] = q_clamp(i, *(const float*)d);
    else if (type == P->atom_Int && size == sizeof(int32_t))   P->q[i] = q_clamp(i, (float)*(const int32_t*)d);
    else if (type == P->atom_Double && size == sizeof(double)) P->q[i] = q_clamp(i, (float)*(const double*)d);
    else if (type == P->atom_Long && size == sizeof(int64_t))  P->q[i] = q_clamp(i, (float)*(const int64_t*)d);
    if (i < Q_PER_SLOT * PX_SLOTS && (i % Q_PER_SLOT) >= Q_SLOT_LVL) {
      has_lvl[i / Q_PER_SLOT] = 1;
    }
  }
  /* Factory presets that set a built-in pattern without explicit step levels expand from the pattern table. */
  for (int s = 0; s < PX_SLOTS; s++) {
    int pat = (int)P->q[q_slot_idx(s, Q_SLOT_FACE + F_PATTERN)];
    if (!has_lvl[s] && pat >= 1 && pat < PX_NPATTERNS) {
      apply_slot_pattern(P, s, pat, 0);
    }
  }
  /* Initialize all 4 scenes from restored q[], then overwrite if scenes_blob is present */
  for (int sc = 0; sc < NSCENES; sc++) memcpy(P->scenes[sc], P->q, sizeof(float) * NQ_SCENE);
  size_t bsize; uint32_t btype, bfl;
  const void* blob = retrieve(sh, P->urid_scenes_blob, &bsize, &btype, &bfl);
  if (blob && bsize == sizeof P->scenes) {
    memcpy(P->scenes, blob, sizeof P->scenes);
  }
  const void* csd = retrieve(sh, P->urid_cur_scene, &bsize, &btype, &bfl);
  if (csd) {
    int sc = 0;
    if (btype == P->atom_Int && bsize == sizeof(int32_t)) sc = *(const int32_t*)csd;
    else if (btype == P->atom_Long && bsize == sizeof(int64_t)) sc = (int)*(const int64_t*)csd;
    if (sc >= 0 && sc < NSCENES) P->cur_scene = sc;
  }
  P->scene_seen = 0;
  P->face_seen = 0;
  P->loads += 1.0f;
  P->report_next = 0;
  return LV2_STATE_SUCCESS;
}

static const void* extension_data(const char* uri) {
  static const LV2_State_Interface state = { save, restore };
  if (!strcmp(uri, LV2_STATE__interface)) return &state;
  return NULL;
}

static const LV2_Descriptor descriptor = {
  PULSE_URI, instantiate, connect_port, activate, run, NULL, cleanup, extension_data
};

LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index) {
  return index == 0 ? &descriptor : NULL;
}
