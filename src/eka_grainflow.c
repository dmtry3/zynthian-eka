/* Eka Grain Flow: LV2 wrapper around grain_fx.
 *
 * Four knob pages: Main (Stream, Mix, Feedback, Freeze) and Grain + Flow + Out, which act on the SELECTED stream.
 * The values of the three streams are LV2 parameters s1_offset .. s3_level (patch:Set, saved in State),
 * and the plugin is where they live:
 * - a turn of a Grain/Flow/Out knob goes into the selected stream and is reported back with patch:Set;
 * - choosing another stream (Stream knob or the widget) does not copy anything: the widget then moves the
 *   Grain/Flow/Out knobs to the values of that stream (a plugin cannot move its own input ports);
 * - after a preset is loaded (restore()) the output port "loads" counts up, so the widget does that again.
 * Outputs for the widget: input peak, active grains, grain1..16 (stream + seconds behind the head / 20,
 * -1 = none), the shortest Offset of the selected stream, and the plugin's own view of the selection:
 * sel (1..3), on_mask (bit per stream), cur_offset .. cur_level (values of the selected stream). The widget
 * moves the knobs from these, so it does not depend on the host passing patch:Set replies on. */
#include <lv2/atom/atom.h>
#include <lv2/atom/forge.h>
#include <lv2/atom/util.h>
#include <lv2/core/lv2.h>
#include <lv2/patch/patch.h>
#include <lv2/state/state.h>
#include <lv2/time/time.h>
#include <lv2/urid/urid.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "grain_fx.h"

#define GRAIN_URI "http://dmtry3.github.io/zynthian-eka/grainflow"

/* Control input ports come first: Zynthian numbers LV2 parameters from len(control ports)+1. */
enum { P_STREAM = 0, P_MIX, P_FEEDBACK, P_FREEZE,           /* page Main */
       P_FACE0,                                             /* Grain (4), Out (4), Flow (4); grouped by TTL */
       P_CTL_END = P_FACE0 + 12,
       P_IN_L = P_CTL_END, P_IN_R, P_OUT_L, P_OUT_R, P_CONTROL, P_NOTIFY,
       P_PEAK, P_ACTIVE, P_LOADS, P_MIN_OFFSET, P_SEL, P_ON_MASK, P_CUR0,
       P_LFO0 = P_CUR0 + 8,                                 /* lfo1, lfo2: their values now, -1..1 */
       P_ENV = P_LFO0 + GX_LFOS,                            /* the input level as the Env source sees it, 0..1 */
       P_BPM, P_RATE_DIV, P_OFFSET_DIV,                     /* tempo; note values of the selected stream (-1: no sync) */
       P_GRAIN0, P_FREEZE_PHASE = P_GRAIN0 + 16, P_RECORD_PEAK,
       P_CUR_FLOW0, P_END = P_CUR_FLOW0 + 4 };
#define N_GRAIN_PORTS 16

/* The original 8 values of a stream: Grain then Out. Flow maps to existing extra parameters. */
enum { V_OFFSET = 0, V_RATE, V_LENGTH, V_PITCH, V_REVERSE, V_FILTER, V_SPRAY, V_LEVEL, NV };
#define NFACES (NV + 4)
static const char* const v_names[NV] = { "offset", "rate", "length", "pitch", "reverse", "filter", "spray", "level" };
static const float v_min[NV] = { 1, GX_MIN_RATE_MS, GX_MIN_LENGTH_MS, -GX_MAX_PITCH, 0, -100, 0, -60 };
static const float v_max[NV] = { 8000, GX_MAX_RATE_MS, GX_MAX_LENGTH_MS, GX_MAX_PITCH, 100, 100, 100, 6 };
/* Init: streams pitched +12, 0, -12; only stream 1 is enabled. */
static const float v_def[GX_STREAMS][NV] = { { 100, 100, 500, 12, 0, 0, 0, 0 },
                                             { 250, 120, 400, 0, 0, 0, 0, -3 },
                                             { 400, 150, 600, -12, 0, 0, 0, -3 } };
static const float spread_def[GX_STREAMS] = { 0, 50, 50 };

/* Parameters (patch:Set / State): per stream its 8 values, then "on", "fine" (cents), "spread" (%) and
 * "interval" (set index), now controlled by Flow; then buffer, shape and sync. */
#define Q_ON NV
#define Q_FINE (NV + 1)
#define Q_SPREAD (NV + 2)
#define Q_INTERVAL (NV + 3)
/* After them: per LFO its rate (Hz) and shape, then the depth cells m<source>_s<stream>_<destination>
 * (-100..100 %); sources 1 and 2 are the LFOs, 3 is Env. */
enum { Q_STREAM0 = 0, Q_PER_STREAM = NV + 4, Q_BUFFER = Q_PER_STREAM * GX_STREAMS, Q_SHAPE, Q_SYNC,
       Q_LFO0, Q_CELL0 = Q_LFO0 + 2 * GX_LFOS,
       Q_RATE_MOD0 = Q_CELL0 + GX_MSRC * GX_STREAMS * 4,
       Q_FREEZE_AMOUNT = Q_RATE_MOD0 + GX_LFOS, Q_SCAN0,
       Q_LFO_MODE0 = Q_SCAN0 + GX_STREAMS, Q_ENV_ATTACK = Q_LFO_MODE0 + GX_LFOS,
       Q_ENV_RELEASE, Q_ENV_SENS, Q_SMOOTH0, Q_LFO_SYNC0 = Q_SMOOTH0 + GX_LFOS,
       Q_LFO_DIV0 = Q_LFO_SYNC0 + GX_LFOS, Q_CELL_EXTRA0 = Q_LFO_DIV0 + GX_LFOS,
       NQ = Q_CELL_EXTRA0 + GX_MSRC * GX_STREAMS * 2 };
static const char* const m_names[GX_MDST] = { "pitch", "offset", "length", "level", "filter", "scan" };
static const float lfo_rate_def[GX_LFOS] = { 0.3f, 1.0f };
static const int lfo_shape_def[GX_LFOS] = { GX_LFO_SINE, GX_LFO_DRIFT };
static int q_cell(int l, int s, int d) { return d < 4 ? Q_CELL0 + (l * GX_STREAMS + s) * 4 + d
               : Q_CELL_EXTRA0 + (l * GX_STREAMS + s) * 2 + d - 4; }
#define REPORT_PER_RUN 16

typedef struct {
  float* port[P_END];
  LV2_URID_Map* map;
  LV2_Atom_Forge forge;
  LV2_URID atom_Float, atom_Int, atom_Double, atom_Long, atom_Object, atom_URID;
  LV2_URID patch_Set, patch_Get, patch_property, patch_value;
  LV2_URID time_Position, time_bpm, time_speed, time_bar, time_barBeat, time_bpb;
  int bpb;
  LV2_URID q_urid[NQ];
  float q[NQ];                 /* parameter values */
  unsigned char dirty[NQ];     /* changed here: report to the host */
  int report_next;             /* after patch:Get: next parameter to report, -1 = none */
  int sel;                     /* selected stream 0..2 */
  float face[NFACES];          /* Grain/Out/Flow knob values seen last */
  int face_seen;
  float loads;
  GxParams params;
  GrainFx fx;
} Plugin;

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static int q_index(int s, int v) { return s * Q_PER_STREAM + v; }       /* v = Q_ON, Q_FINE too */
static int q_face(int s, int v) {
  if (v < NV) return q_index(s, v);
  static const int extra[] = { -1, Q_FINE, Q_INTERVAL, Q_SPREAD };  /* Scan, Fine, Intervals, Spread */
  return v == NV ? Q_SCAN0 + s : q_index(s, extra[v - NV]);
}

static void q_name(int i, char* out, size_t n) {
  if (i == Q_BUFFER) snprintf(out, n, "buffer");
  else if (i == Q_SHAPE) snprintf(out, n, "shape");
  else if (i == Q_SYNC) snprintf(out, n, "sync");
  else if (i == Q_FREEZE_AMOUNT) snprintf(out, n, "freeze_amount");
  else if (i >= Q_CELL_EXTRA0) {
    int k = i - Q_CELL_EXTRA0;
    snprintf(out, n, "m%d_s%d_%s", k / (GX_STREAMS * 2) + 1, k / 2 % GX_STREAMS + 1, m_names[4 + k % 2]);
  }
  else if (i >= Q_LFO_DIV0) snprintf(out, n, "lfo%d_div", i - Q_LFO_DIV0 + 1);
  else if (i >= Q_LFO_SYNC0) snprintf(out, n, "lfo%d_sync", i - Q_LFO_SYNC0 + 1);
  else if (i >= Q_SMOOTH0) snprintf(out, n, "lfo%d_smooth", i - Q_SMOOTH0 + 1);
  else if (i == Q_ENV_ATTACK) snprintf(out, n, "env_attack");
  else if (i == Q_ENV_RELEASE) snprintf(out, n, "env_release");
  else if (i == Q_ENV_SENS) snprintf(out, n, "env_sensitivity");
  else if (i >= Q_LFO_MODE0) snprintf(out, n, "lfo%d_mode", i - Q_LFO_MODE0 + 1);
  else if (i >= Q_SCAN0) snprintf(out, n, "s%d_scan", i - Q_SCAN0 + 1);
  else if (i >= Q_RATE_MOD0) snprintf(out, n, "lfo%d_rate_mod", i - Q_RATE_MOD0 + 1);
  else if (i >= Q_CELL0) {
    int k = i - Q_CELL0;
    snprintf(out, n, "m%d_s%d_%s", k / (GX_STREAMS * 4) + 1, k / 4 % GX_STREAMS + 1, m_names[k % 4]);
  } else if (i >= Q_LFO0) snprintf(out, n, "lfo%d_%s", (i - Q_LFO0) / 2 + 1, (i - Q_LFO0) % 2 ? "shape" : "rate");
  else {
    int s = i / Q_PER_STREAM, v = i % Q_PER_STREAM;
    snprintf(out, n, "s%d_%s", s + 1, v == Q_ON ? "on" : v == Q_FINE ? "fine" : v == Q_SPREAD ? "spread"
                                     : v == Q_INTERVAL ? "interval" : v_names[v]);
  }
}

static int q_is_lfo_rate(int i) { return i >= Q_LFO0 && i < Q_CELL0 && (i - Q_LFO0) % 2 == 0; }
static int q_is_int(int i) {
  if (i == Q_SHAPE || i == Q_FREEZE_AMOUNT || (i >= Q_SCAN0 && i < Q_LFO_MODE0)) return 0;
  if (i >= Q_CELL_EXTRA0) return 1;
  if (i >= Q_LFO_SYNC0) return 1;
  if (i >= Q_ENV_ATTACK) return 0;
  if (i >= Q_LFO_MODE0) return 1;
  return i >= Q_BUFFER ? !q_is_lfo_rate(i) : (i % Q_PER_STREAM == Q_ON || i % Q_PER_STREAM == Q_INTERVAL);
}

static float q_clamp(int i, float v) {
  if (i >= Q_CELL_EXTRA0) return clampf(roundf(v), -100, 100);
  if (i >= Q_LFO_DIV0) return clampf(roundf(v), 0, GX_NDIVS - 1);
  if (i >= Q_LFO_SYNC0) return v >= 0.5f;
  if (i >= Q_SMOOTH0) return clampf(v, 0, 100);
  if (i == Q_BUFFER) return clampf(roundf(v), 1, GX_MAX_BUFFER_S);
  if (i == Q_SHAPE) return clampf(v, 0, 1);
  if (i == Q_SYNC) return v >= 0.5f ? 1.0f : 0.0f;
  if (i == Q_FREEZE_AMOUNT) return clampf(v, 0, 100);
  if (i == Q_ENV_ATTACK) return clampf(v, 0, 1000);
  if (i == Q_ENV_RELEASE) return clampf(v, 10, 5000);
  if (i == Q_ENV_SENS) return clampf(v, -24, 24);
  if (i >= Q_LFO_MODE0) return clampf(roundf(v), 0, 1);
  if (i >= Q_SCAN0) return clampf(v, -200, 200);
  if (i >= Q_CELL0) return clampf(roundf(v), -100, 100);
  if (q_is_lfo_rate(i)) return clampf(v, GX_LFO_MIN_HZ, GX_LFO_MAX_HZ);
  if (i >= Q_LFO0) return clampf(roundf(v), 0, GX_NLFO_SHAPES - 1);
  int k = i % Q_PER_STREAM;
  if (k == Q_ON) return v >= 0.5f ? 1.0f : 0.0f;
  if (k == Q_FINE) return clampf(v, -50, 50);
  if (k == Q_SPREAD) return clampf(v, 0, 100);
  if (k == Q_INTERVAL) return clampf(roundf(v), 0, GX_NINTERVALS - 1);
  return clampf(v, v_min[k], v_max[k]);
}

static void q_defaults(Plugin* P) {
  for (int s = 0; s < GX_STREAMS; s++) {
    for (int v = 0; v < NV; v++) P->q[q_index(s, v)] = v_def[s][v];
    P->q[q_index(s, Q_ON)] = s == 0;
    P->q[q_index(s, Q_FINE)] = 0;
    P->q[q_index(s, Q_SPREAD)] = spread_def[s];
    P->q[q_index(s, Q_INTERVAL)] = 0;
  }
  P->q[Q_SYNC] = 0;
  P->q[Q_BUFFER] = 4;
  P->q[Q_SHAPE] = 0;
  for (int l = 0; l < GX_LFOS; l++) { P->q[Q_LFO0 + 2 * l] = lfo_rate_def[l]; P->q[Q_LFO0 + 2 * l + 1] = lfo_shape_def[l]; }
  for (int i = Q_CELL0; i < NQ; i++) P->q[i] = 0;
  for (int l = 0; l < GX_LFOS; l++) P->q[Q_LFO_MODE0 + l] = 0;
  P->q[Q_ENV_RELEASE] = 300;
  for (int l = 0; l < GX_LFOS; l++) P->q[Q_LFO_DIV0 + l] = 12;
}

static void apply_params(Plugin* P) {
  GxParams* p = &P->params;
  for (int s = 0; s < GX_STREAMS; s++) {
    const float* v = &P->q[q_index(s, 0)];
    GxStream* t = &p->s[s];
    t->on = v[Q_ON] >= 0.5f;
    t->fine = v[Q_FINE];
    t->offset_ms = v[V_OFFSET];
    t->rate_ms = v[V_RATE];
    t->length_ms = v[V_LENGTH];
    t->pitch = v[V_PITCH];
    t->reverse = v[V_REVERSE] * 0.01f;
    t->filter = v[V_FILTER] * 0.01f;
    t->spray = v[V_SPRAY] * 0.01f;
    t->spread = v[Q_SPREAD] * 0.01f;
    t->interval = (int)v[Q_INTERVAL];
    t->gain = gx_db(v[V_LEVEL]);
    t->scan = P->q[Q_SCAN0 + s] * 0.01f;
  }
  p->buffer_s = P->q[Q_BUFFER];
  p->sync = P->q[Q_SYNC] >= 0.5f;
  p->shape = P->q[Q_SHAPE];
  for (int l = 0; l < GX_LFOS; l++) {
    p->lfo[l].rate = P->q[Q_LFO0 + 2 * l];
    p->lfo[l].shape = (int)P->q[Q_LFO0 + 2 * l + 1];
    p->lfo_mode[l] = (int)P->q[Q_LFO_MODE0 + l];
    p->lfo_smooth[l] = P->q[Q_SMOOTH0 + l] * 0.01f;
    p->lfo_sync[l] = (int)P->q[Q_LFO_SYNC0 + l];
    p->lfo_div[l] = (int)P->q[Q_LFO_DIV0 + l];
    p->lfo_rate_mod[l] = P->q[Q_RATE_MOD0 + l] * 0.01f;
  }
  for (int l = 0; l < GX_MSRC; l++)
    for (int s = 0; s < GX_STREAMS; s++)
      for (int d = 0; d < GX_MDST; d++) p->mod[l][s][d] = P->q[q_cell(l, s, d)] * 0.01f;
  p->mix = clampf(*P->port[P_MIX], 0, 100) * 0.01f;
  p->env_attack_ms = P->q[Q_ENV_ATTACK];
  p->env_release_ms = P->q[Q_ENV_RELEASE];
  p->env_sensitivity_db = P->q[Q_ENV_SENS];
  p->gain = 1.0f;
  p->feedback = clampf(*P->port[P_FEEDBACK], 0, 95) * 0.01f;
  p->freeze = *P->port[P_FREEZE] >= 0.5f;
  p->freeze_amount = P->q[Q_FREEZE_AMOUNT] * 0.01f;
}

static LV2_Handle instantiate(const LV2_Descriptor* d, double rate, const char* bundle,
                              const LV2_Feature* const* f) {
  (void)d; (void)bundle;
  Plugin* P = (Plugin*)calloc(1, sizeof(Plugin));
  if (!P) return NULL;
  for (int i = 0; f[i]; i++)
    if (!strcmp(f[i]->URI, LV2_URID__map)) P->map = (LV2_URID_Map*)f[i]->data;
  if (!P->map || gx_init(&P->fx, rate)) { free(P); return NULL; }
  LV2_URID_Map* m = P->map;
  P->atom_Float = m->map(m->handle, LV2_ATOM__Float);
  P->atom_Int = m->map(m->handle, LV2_ATOM__Int);
  P->atom_Double = m->map(m->handle, LV2_ATOM__Double);
  P->atom_Long = m->map(m->handle, LV2_ATOM__Long);
  P->atom_Object = m->map(m->handle, LV2_ATOM__Object);
  P->atom_URID = m->map(m->handle, LV2_ATOM__URID);
  P->patch_Set = m->map(m->handle, LV2_PATCH__Set);
  P->patch_Get = m->map(m->handle, LV2_PATCH__Get);
  P->patch_property = m->map(m->handle, LV2_PATCH__property);
  P->patch_value = m->map(m->handle, LV2_PATCH__value);
  P->time_Position = m->map(m->handle, LV2_TIME__Position);
  P->time_bpm = m->map(m->handle, LV2_TIME__beatsPerMinute);
  P->time_speed = m->map(m->handle, LV2_TIME__speed);
  P->time_bar = m->map(m->handle, LV2_TIME__bar);
  P->time_barBeat = m->map(m->handle, LV2_TIME__barBeat);
  P->time_bpb = m->map(m->handle, LV2_TIME__beatsPerBar);
  P->bpb = 4;
  for (int i = 0; i < NQ; i++) {
    char name[32], uri[128];
    q_name(i, name, sizeof name);
    snprintf(uri, sizeof uri, GRAIN_URI "#%s", name);
    P->q_urid[i] = m->map(m->handle, uri);
  }
  lv2_atom_forge_init(&P->forge, m);
  q_defaults(P);
  P->report_next = -1;
  return P;
}

static void connect_port(LV2_Handle h, uint32_t port, void* data) {
  Plugin* P = (Plugin*)h;
  if (port < P_END) P->port[port] = (float*)data;
}

static void activate(LV2_Handle h) {
  Plugin* P = (Plugin*)h;
  gx_reset(&P->fx);
  P->face_seen = 0;
}

static int q_by_urid(Plugin* P, LV2_URID u) {
  for (int i = 0; i < NQ; i++) if (P->q_urid[i] == u) return i;
  return -1;
}

static int atom_number(Plugin* P, const LV2_Atom* a, float* out) {
  if (a->type == P->atom_Float) { *out = ((const LV2_Atom_Float*)a)->body; return 1; }
  if (a->type == P->atom_Int) { *out = (float)((const LV2_Atom_Int*)a)->body; return 1; }
  if (a->type == P->atom_Double) { *out = (float)((const LV2_Atom_Double*)a)->body; return 1; }
  if (a->type == P->atom_Long) { *out = (float)((const LV2_Atom_Long*)a)->body; return 1; }
  return 0;
}

static void read_events(Plugin* P) {
  const LV2_Atom_Sequence* seq = (const LV2_Atom_Sequence*)P->port[P_CONTROL];
  if (!seq) return;
  LV2_ATOM_SEQUENCE_FOREACH(seq, ev) {
    if (ev->body.type != P->atom_Object) continue;
    const LV2_Atom_Object* obj = (const LV2_Atom_Object*)&ev->body;
    if (obj->body.otype == P->patch_Get) { P->report_next = 0; continue; }
    if (obj->body.otype == P->time_Position) {
      /* host transport (as in Eka Mist): tempo always; the song position in beats while it rolls */
      const LV2_Atom *bpm = NULL, *speed = NULL, *bar = NULL, *bb = NULL, *bpb = NULL;
      lv2_atom_object_get(obj, P->time_bpm, &bpm, P->time_speed, &speed, P->time_bar, &bar, P->time_barBeat, &bb,
                          P->time_bpb, &bpb, 0);
      float v;
      if (bpb && atom_number(P, bpb, &v) && v >= 1 && v <= 16) P->bpb = (int)lrintf(v);
      float tempo = 0, sp = 0, br = 0, beat = 0;
      if (bpm) atom_number(P, bpm, &tempo);
      if (speed) atom_number(P, speed, &sp);
      int rolling = sp != 0.0f && bar && bb && atom_number(P, bar, &br) && atom_number(P, bb, &beat);
      gx_set_tempo(&P->fx, tempo, rolling ? (double)br * P->bpb + beat : -1.0);
      continue;
    }
    if (obj->body.otype != P->patch_Set) continue;
    const LV2_Atom* prop = NULL; const LV2_Atom* val = NULL;
    lv2_atom_object_get(obj, P->patch_property, &prop, P->patch_value, &val, 0);
    if (!prop || !val || prop->type != P->atom_URID) continue;
    int i = q_by_urid(P, ((const LV2_Atom_URID*)prop)->body);
    float v;
    if (i >= 0 && atom_number(P, val, &v)) P->q[i] = q_clamp(i, v);
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
  for (int i = 0; i < NQ && sent < REPORT_PER_RUN; i++)
    if (P->dirty[i]) { P->dirty[i] = 0; forge_set(P, i); sent++; }
  for (; sent < REPORT_PER_RUN && P->report_next >= 0; sent++) {
    forge_set(P, P->report_next);
    P->report_next = P->report_next + 1 < NQ ? P->report_next + 1 : -1;
  }
  lv2_atom_forge_pop(&P->forge, &frame);
}

/* Grain/Flow/Out knobs -> the selected stream. A new selection only takes the knob values as seen, so a knob
 * that still shows the previous stream does not overwrite this one. */
static void read_faces(Plugin* P) {
  int sel = (int)lrintf(clampf(*P->port[P_STREAM], 1, GX_STREAMS)) - 1;
  float face[NFACES];
  for (int v = 0; v < NFACES; v++)
    face[v] = P->port[P_FACE0 + v] ? *P->port[P_FACE0 + v] : P->q[q_face(sel, v)];
  if (!P->face_seen || sel != P->sel) {
    P->sel = sel;
    memcpy(P->face, face, sizeof face);
    P->face_seen = 1;
    return;
  }
  for (int v = 0; v < NFACES; v++) {
    if (face[v] == P->face[v]) continue;
    P->face[v] = face[v];
    int i = q_face(sel, v);
    float nv = q_clamp(i, face[v]);
    if (nv != P->q[i]) { P->q[i] = nv; P->dirty[i] = 1; }
  }
}

static void run(LV2_Handle h, uint32_t n) {
  Plugin* P = (Plugin*)h;
  read_events(P);
  read_faces(P);
  apply_params(P);
  gx_process(&P->fx, &P->params, P->port[P_IN_L], P->port[P_IN_R], P->port[P_OUT_L], P->port[P_OUT_R], n);
  write_notify(P);

  if (P->port[P_PEAK]) *P->port[P_PEAK] = P->fx.in_peak;
  if (P->port[P_ACTIVE]) *P->port[P_ACTIVE] = (float)gx_active(&P->fx);
  if (P->port[P_LOADS]) *P->port[P_LOADS] = P->loads;
  if (P->port[P_MIN_OFFSET])
    *P->port[P_MIN_OFFSET] = gx_min_offset_ms(&P->fx, &P->params, P->sel);
  for (int l = 0; l < GX_LFOS; l++)
    if (P->port[P_LFO0 + l]) *P->port[P_LFO0 + l] = P->fx.lfo_v[l];
  if (P->port[P_ENV]) *P->port[P_ENV] = P->fx.env_v;
  if (P->port[P_FREEZE_PHASE]) *P->port[P_FREEZE_PHASE] = P->fx.freeze_loop && !P->fx.frozen
      ? (float)((double)P->fx.hold_phase / P->fx.hold_period) : -1.0f;
  if (P->port[P_RECORD_PEAK]) *P->port[P_RECORD_PEAK] = P->fx.record_peak;
  if (P->port[P_BPM]) *P->port[P_BPM] = P->fx.bpm;
  const GxStream* cs = &P->params.s[P->sel];
  if (P->port[P_RATE_DIV]) *P->port[P_RATE_DIV] = P->params.sync ? (float)gx_snap_div(cs->rate_ms, P->fx.bpm) : -1.0f;
  if (P->port[P_OFFSET_DIV])
    *P->port[P_OFFSET_DIV] = P->params.sync ? (float)gx_snap_div(cs->offset_ms, P->fx.bpm) : -1.0f;
  if (P->port[P_SEL]) *P->port[P_SEL] = (float)(P->sel + 1);
  int mask = 0;
  for (int t = 0; t < GX_STREAMS; t++) if (P->q[q_index(t, Q_ON)] >= 0.5f) mask |= 1 << t;
  if (P->port[P_ON_MASK]) *P->port[P_ON_MASK] = (float)mask;
  for (int v = 0; v < NV; v++)
    if (P->port[P_CUR0 + v]) *P->port[P_CUR0 + v] = P->q[q_index(P->sel, v)];
  for (int v = 0; v < 4; v++)
    if (P->port[P_CUR_FLOW0 + v]) *P->port[P_CUR_FLOW0 + v] = P->q[q_face(P->sel, NV + v)];
  int k = 0, s;
  float back;
  for (; k < N_GRAIN_PORTS && gx_grain_info(&P->fx, k, &s, &back); k++)
    if (P->port[P_GRAIN0 + k]) *P->port[P_GRAIN0 + k] = (float)s + clampf(back / 20.0f, 0.0f, 0.999f);
  for (; k < N_GRAIN_PORTS; k++)
    if (P->port[P_GRAIN0 + k]) *P->port[P_GRAIN0 + k] = -1.0f;
}

static void cleanup(LV2_Handle h) {
  Plugin* P = (Plugin*)h;
  gx_free(&P->fx);
  free(P);
}

static LV2_State_Status save(LV2_Handle h, LV2_State_Store_Function store, LV2_State_Handle sh,
                             uint32_t flags, const LV2_Feature* const* f) {
  (void)flags; (void)f;
  Plugin* P = (Plugin*)h;
  for (int i = 0; i < NQ; i++) {
    if (q_is_int(i)) {
      int32_t v = (int32_t)P->q[i];
      store(sh, P->q_urid[i], &v, sizeof v, P->atom_Int, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
    } else {
      store(sh, P->q_urid[i], &P->q[i], sizeof(float), P->atom_Float, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
    }
  }
  return LV2_STATE_SUCCESS;
}

static LV2_State_Status restore(LV2_Handle h, LV2_State_Retrieve_Function retrieve, LV2_State_Handle sh,
                                uint32_t flags, const LV2_Feature* const* f) {
  (void)flags; (void)f;
  Plugin* P = (Plugin*)h;
  q_defaults(P);                                     /* values missing in an older preset */
  for (int i = 0; i < NQ; i++) {
    size_t size; uint32_t type, fl;
    const void* d = retrieve(sh, P->q_urid[i], &size, &type, &fl);
    if (!d) continue;
    /* presets written as Turtle numbers arrive as Double / Long (sratom), saved states as Float / Int */
    if (type == P->atom_Float && size == sizeof(float)) P->q[i] = q_clamp(i, *(const float*)d);
    else if (type == P->atom_Int && size == sizeof(int32_t)) P->q[i] = q_clamp(i, (float)*(const int32_t*)d);
    else if (type == P->atom_Double && size == sizeof(double)) P->q[i] = q_clamp(i, (float)*(const double*)d);
    else if (type == P->atom_Long && size == sizeof(int64_t)) P->q[i] = q_clamp(i, (float)*(const int64_t*)d);
  }
  P->face_seen = 0;                                  /* the knobs still show the old values: do not copy them */
  memset(P->fx.scan_active, 0, sizeof P->fx.scan_active);
  P->loads += 1.0f;
  P->report_next = 0;
  return LV2_STATE_SUCCESS;
}

static const void* extension_data(const char* uri) {
  static const LV2_State_Interface state = { save, restore };
  if (!strcmp(uri, LV2_STATE__interface)) return &state;
  return NULL;
}

static const LV2_Descriptor descriptor = { GRAIN_URI, instantiate, connect_port, activate, run, NULL, cleanup,
                                           extension_data };

LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index) {
  return index == 0 ? &descriptor : NULL;
}
