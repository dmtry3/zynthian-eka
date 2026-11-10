/* Generative Arp: LV2 wrapper around arp_core.
 * Transport rolling: ticks follow time:Position (steps on the song grid).
 * Transport stopped: ticks come from an internal clock at the host tempo. */
#include <lv2/core/lv2.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/forge.h>
#include <lv2/atom/util.h>
#include <lv2/midi/midi.h>
#include <lv2/patch/patch.h>
#include <lv2/state/state.h>
#include <lv2/time/time.h>
#include <lv2/urid/urid.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "arp_core.h"

#define ARP_URI "urn:generativearp:arp"
#define JUMP_TICKS 4.0   /* position difference treated as a relocation */
#define REPORT_PER_RUN 16 /* patch:Set replies per run() after patch:Get, not to overflow midi_out */

/* Control input ports come first (0..P_END-1): Zynthian numbers LV2 parameters from len(control ports)+1,
 * so a control port with a higher index would be overwritten by a parameter in its list. */
enum { P_RATE = 0, P_GATE, P_ORDER, P_OCTAVES, P_LATCH,
       P_LEN0, P_KEY = P_LEN0 + ARP_NLANES, P_SCALE,     /* row lengths: prob, shift, vel, gate */
       P_MUT_NOTES, P_LOOP_LEN, P_MUT_STEPS, P_RESET,     /* mutation; any change of reset = reset */
       P_TRANSPOSE,                                       /* -2..+2 octaves */
       P_RPT, P_RPT_MODE,                                 /* repeats 1..5 / Rnd 3 / Rnd 5, ARP_RPT_* mode */
       P_SWING, P_FEEL, P_DRUNK, P_ACCENT,                /* groove */
       P_PASSING, P_PASS_STYLE,                           /* passing notes */
       P_END, P_MIDI_IN = P_END, P_MIDI_OUT, P_STEP_COUNT,     /* step_count: output, for the widget */
       P_DEV0 };   /* dev1..dev16 outputs: drift of each step for the widget, see pack_dev() */

/* Pattern step values are LV2 parameters (patch:Set on midi_in), not ports: Zynthian's jalv does not
 * register notOnGUI ports, but it does register parameters. Order of fields per step: */
enum { F_PROB, F_SHIFT, F_VEL, F_GATE, N_FIELDS };
static const char* const field_names[N_FIELDS] = { "prob", "shift", "vel", "gate" };
static const int field_defaults[N_FIELDS] = { 100, 0, 100, ARP_GATE_GLOBAL };
#define N_CTL (P_END - P_RATE)

typedef struct { LV2_Atom_Event ev; uint8_t msg[3]; } MidiOut;

typedef struct {
  const LV2_Atom_Sequence* in;
  LV2_Atom_Sequence* out;
  const float* ctl[N_CTL];
  float* step_count_out;
  float* dev_out[ARP_MAX_STEPS];
  uint32_t out_cap, nframes, cur_frame;
  LV2_URID_Map* map;
  LV2_URID map_urid_type;
  LV2_URID midi_Event, atom_Object, atom_Float, atom_Double, atom_Long, atom_Int;
  LV2_URID time_Position, time_bar, time_barBeat, time_bpm, time_speed, time_bpb;
  LV2_URID patch_Set, patch_Get, patch_property, patch_value;
  LV2_Atom_Forge forge;
  int report_next;       /* next parameter to report after patch:Get, -1 = nothing to report */
  LV2_URID param[ARP_MAX_STEPS][N_FIELDS];
  int stepv[ARP_MAX_STEPS][N_FIELDS];

  double sr, clk, bpm;   /* clk: current position in ticks */
  int bpb, rolling;
  int reset_prev;        /* last value of the reset port, -1 before the first run */
  ArpCore core;
} Arp;

static double tps(const Arp* A) { return A->bpm * ARP_PPQN / (60.0 * A->sr); }

static void emit(Arp* A, uint32_t frame, uint8_t st, uint8_t d1, uint8_t d2) {
  MidiOut e;
  memset(&e, 0, sizeof e);
  if (frame >= A->nframes) frame = A->nframes ? A->nframes - 1 : 0;
  e.ev.time.frames = frame;
  e.ev.body.type = A->midi_Event;
  e.ev.body.size = 3;
  e.msg[0] = st; e.msg[1] = d1; e.msg[2] = d2;
  lv2_atom_sequence_append_event(A->out, A->out_cap, &e.ev);
}

/* arp_core callback: convert tick to a frame of the current block */
static void core_emit(void* user, double tick, uint8_t st, uint8_t d1, uint8_t d2) {
  Arp* A = (Arp*)user;
  double d = (tick - A->clk) / tps(A);
  uint32_t f = A->cur_frame + (d > 1e-6 ? (uint32_t)ceil(d - 1e-6) : 0);
  emit(A, f, st, d1, d2);
}

static void advance_to(Arp* A, uint32_t target) {
  if (target <= A->cur_frame) return;
  double end = A->clk + (double)(target - A->cur_frame) * tps(A);
  arp_process(&A->core, A->clk, end, core_emit, A);
  A->clk = end;
  A->cur_frame = target;
}

static double numv(const Arp* A, const LV2_Atom* a, double def) {
  if (!a) return def;
  if (a->type == A->atom_Float)  return ((const LV2_Atom_Float*)a)->body;
  if (a->type == A->atom_Double) return ((const LV2_Atom_Double*)a)->body;
  if (a->type == A->atom_Long)   return (double)((const LV2_Atom_Long*)a)->body;
  if (a->type == A->atom_Int)    return ((const LV2_Atom_Int*)a)->body;
  return def;
}

static void handle_pos(Arp* A, const LV2_Atom_Object* obj) {
  const LV2_Atom *bpm = NULL, *speed = NULL, *bb = NULL, *bar = NULL, *bpb = NULL;
  lv2_atom_object_get(obj, A->time_bpm, &bpm, A->time_speed, &speed, A->time_barBeat, &bb,
                      A->time_bar, &bar, A->time_bpb, &bpb, 0);
  if (bpm) { double v = numv(A, bpm, A->bpm); if (v >= 20 && v <= 400) A->bpm = v; }
  if (bpb) { int v = (int)(numv(A, bpb, 4) + 0.5); if (v >= 1 && v <= 16) A->bpb = v; }
  if (speed) A->rolling = numv(A, speed, 0) != 0;
  /* Stopped: the position is frozen, keep the internal clock running. */
  if (!A->rolling || !bar || !bb) return;
  double ht = (numv(A, bar, 0) * A->bpb + numv(A, bb, 0)) * ARP_PPQN;
  if (fabs(ht - A->clk) > JUMP_TICKS) arp_silence(&A->core, A->clk, core_emit, A);
  A->clk = ht;
}

static void apply_step(Arp* A, int i) {
  const int* v = A->stepv[i];
  arp_set_step(&A->core, i, v[F_PROB], v[F_SHIFT], v[F_VEL], v[F_GATE]);
}

static void handle_patch_set(Arp* A, const LV2_Atom_Object* obj) {
  const LV2_Atom *prop = NULL, *val = NULL;
  lv2_atom_object_get(obj, A->patch_property, &prop, A->patch_value, &val, 0);
  if (!prop || prop->type != A->map_urid_type || !val) return;
  const LV2_URID key = ((const LV2_Atom_URID*)prop)->body;
  for (int i = 0; i < ARP_MAX_STEPS; i++)
    for (int f = 0; f < N_FIELDS; f++)
      if (A->param[i][f] == key) {
        A->stepv[i][f] = (int)lrint(numv(A, val, field_defaults[f]));
        apply_step(A, i);
        arp_clear_mutation(&A->core, i, f);   /* an edited step starts again from the drawn value */
        return;
      }
}

static void handle_midi(Arp* A, const LV2_Atom_Event* ev) {
  const uint8_t* b = (const uint8_t*)LV2_ATOM_BODY_CONST(&ev->body);
  uint32_t sz = ev->body.size;
  if (sz < 1) return;
  uint8_t type = sz >= 3 ? (b[0] & 0xF0) : 0, ch = b[0] & 0x0F;
  if (type == 0x90 && b[2] > 0) {
    int first = arp_note_on(&A->core, ch, b[1], b[2]);
    if (first && !A->rolling) {
      /* Free-running: start on a step right now instead of waiting for the next one. */
      const double step = A->core.step_ticks;
      A->clk = ceil(A->clk / step) * step;
    }
    return;
  }
  if (type == 0x80 || type == 0x90) { arp_note_off(&A->core, ch, b[1]); return; }
  if (type == 0xB0 && (b[1] == 123 || b[1] == 120)) arp_release_all(&A->core);
  lv2_atom_sequence_append_event(A->out, A->out_cap, ev);   /* everything else passes through */
}

static LV2_Handle instantiate(const LV2_Descriptor* d, double rate, const char* path,
                              const LV2_Feature* const* f) {
  (void)d; (void)path;
  Arp* A = (Arp*)calloc(1, sizeof(Arp));
  if (!A) return NULL;
  for (int i = 0; f[i]; i++)
    if (!strcmp(f[i]->URI, LV2_URID__map)) A->map = (LV2_URID_Map*)f[i]->data;
  if (!A->map) { free(A); return NULL; }
  LV2_URID_Map* m = A->map;
  A->midi_Event    = m->map(m->handle, LV2_MIDI__MidiEvent);
  A->atom_Object   = m->map(m->handle, LV2_ATOM__Object);
  A->atom_Float    = m->map(m->handle, LV2_ATOM__Float);
  A->atom_Double   = m->map(m->handle, LV2_ATOM__Double);
  A->atom_Long     = m->map(m->handle, LV2_ATOM__Long);
  A->atom_Int      = m->map(m->handle, LV2_ATOM__Int);
  A->time_Position = m->map(m->handle, LV2_TIME__Position);
  A->time_bar      = m->map(m->handle, LV2_TIME__bar);
  A->time_barBeat  = m->map(m->handle, LV2_TIME__barBeat);
  A->time_bpm      = m->map(m->handle, LV2_TIME__beatsPerMinute);
  A->time_speed    = m->map(m->handle, LV2_TIME__speed);
  A->time_bpb      = m->map(m->handle, LV2_TIME__beatsPerBar);
  A->map_urid_type = m->map(m->handle, LV2_ATOM__URID);
  A->patch_Set      = m->map(m->handle, LV2_PATCH__Set);
  A->patch_Get      = m->map(m->handle, LV2_PATCH__Get);
  A->patch_property = m->map(m->handle, LV2_PATCH__property);
  A->patch_value    = m->map(m->handle, LV2_PATCH__value);
  for (int i = 0; i < ARP_MAX_STEPS; i++)
    for (int f = 0; f < N_FIELDS; f++) {
      char uri[64];
      snprintf(uri, sizeof uri, ARP_URI "#%s%d", field_names[f], i + 1);
      A->param[i][f] = m->map(m->handle, uri);
      A->stepv[i][f] = field_defaults[f];
    }
  A->sr = rate; A->bpm = 120.0; A->bpb = 4;
  arp_init(&A->core);
  arp_seed(&A->core, (uint32_t)(uintptr_t)A ^ 0xA5A5A5A5u);
  arp_seed_mutation(&A->core, (uint32_t)time(NULL) ^ ((uint32_t)(uintptr_t)A * 2654435761u));
  arp_reset_mutation(&A->core);   /* a different loop after every start */
  A->reset_prev = -1;
  A->report_next = -1;
  lv2_atom_forge_init(&A->forge, m);
  return A;
}

static void connect_port(LV2_Handle h, uint32_t port, void* data) {
  Arp* A = (Arp*)h;
  if (port == P_MIDI_IN) A->in = (const LV2_Atom_Sequence*)data;
  else if (port == P_MIDI_OUT) A->out = (LV2_Atom_Sequence*)data;
  else if (port < P_END) A->ctl[port - P_RATE] = (const float*)data;
  else if (port == P_STEP_COUNT) A->step_count_out = (float*)data;
  else if (port >= P_DEV0 && port < P_DEV0 + ARP_MAX_STEPS) A->dev_out[port - P_DEV0] = (float*)data;
}

static int ctl_int(const Arp* A, int port, int def) {
  const float* p = A->ctl[port - P_RATE];
  return p ? (int)lrintf(*p) : def;
}

static void activate(LV2_Handle h) { (void)h; }
static void deactivate(LV2_Handle h) { (void)h; }

/* After patch:Get: tell the host the step values as patch:Set on midi_out, a few per run.
 * jalv prints them, so Zynthian's controllers (and the widget) follow a loaded preset. */
static void report_params(Arp* A, uint32_t n) {
  for (int k = 0; k < REPORT_PER_RUN && A->report_next >= 0; k++) {
    const int i = A->report_next / N_FIELDS, f = A->report_next % N_FIELDS;
    union { LV2_Atom_Event ev; uint8_t raw[128]; } buf;
    buf.ev.time.frames = n ? n - 1 : 0;
    lv2_atom_forge_set_buffer(&A->forge, (uint8_t*)&buf.ev.body, sizeof buf - sizeof buf.ev.time);
    LV2_Atom_Forge_Frame fr;
    lv2_atom_forge_object(&A->forge, &fr, 0, A->patch_Set);
    lv2_atom_forge_key(&A->forge, A->patch_property);
    lv2_atom_forge_urid(&A->forge, A->param[i][f]);
    lv2_atom_forge_key(&A->forge, A->patch_value);
    lv2_atom_forge_float(&A->forge, (float)A->stepv[i][f]);
    lv2_atom_forge_pop(&A->forge, &fr);
    if (!lv2_atom_sequence_append_event(A->out, A->out_cap, &buf.ev)) return;   /* full: next run */
    if (++A->report_next >= ARP_MAX_STEPS * N_FIELDS) A->report_next = -1;
  }
}

/* All four drifts of a step in one number: (prob+3) + 7*(shift+3) + 49*(vel+3) + 343*(gate+4). */
static float pack_dev(const int8_t* d) {
  return (float)((d[ARP_LANE_PROB] + ARP_MUT_MAX) + 7 * (d[ARP_LANE_SHIFT] + ARP_MUT_MAX) +
                 49 * (d[ARP_LANE_VEL] + ARP_MUT_MAX) + 343 * (d[ARP_LANE_GATE] + ARP_MUT_MAX_GATE));
}

static void run(LV2_Handle h, uint32_t n) {
  Arp* A = (Arp*)h;
  A->out_cap = A->out->atom.size;
  lv2_atom_sequence_clear(A->out);
  A->out->atom.type = A->in->atom.type;
  A->nframes = n; A->cur_frame = 0;
  arp_set_params(&A->core, ctl_int(A, P_RATE, ARP_RATE_16), ctl_int(A, P_GATE, 50),
                 ctl_int(A, P_ORDER, ARP_UP), ctl_int(A, P_OCTAVES, 1));
  arp_set_latch(&A->core, ctl_int(A, P_LATCH, 0) != 0);
  for (int r = 0; r < ARP_NLANES; r++) arp_set_lane_length(&A->core, r, ctl_int(A, P_LEN0 + r, ARP_MAX_STEPS));
  arp_set_scale(&A->core, ctl_int(A, P_KEY, 0), ctl_int(A, P_SCALE, ARP_SCALE_CHROMATIC));
  arp_set_loop(&A->core, ctl_int(A, P_MUT_NOTES, 0), ctl_int(A, P_LOOP_LEN, 8));
  arp_set_mut_steps(&A->core, ctl_int(A, P_MUT_STEPS, 0));
  arp_set_transpose(&A->core, ctl_int(A, P_TRANSPOSE, 0));
  arp_set_repeats(&A->core, ctl_int(A, P_RPT, 1), ctl_int(A, P_RPT_MODE, ARP_RPT_SAME));
  arp_set_passing(&A->core, ctl_int(A, P_PASSING, 0), ctl_int(A, P_PASS_STYLE, ARP_PASS_NEIGHBOR));
  arp_set_groove(&A->core, ctl_int(A, P_SWING, 50), ctl_int(A, P_FEEL, ARP_FEEL_PAIRS),
                 ctl_int(A, P_DRUNK, 0), ctl_int(A, P_ACCENT, 0));
  const int rs = ctl_int(A, P_RESET, 0) != 0;
  if (A->reset_prev >= 0 && rs != A->reset_prev) arp_reset_mutation(&A->core);
  A->reset_prev = rs;

  LV2_ATOM_SEQUENCE_FOREACH(A->in, ev) {
    uint32_t f = (uint32_t)ev->time.frames;
    if (f >= n) f = n ? n - 1 : 0;
    if (f < A->cur_frame) f = A->cur_frame;
    advance_to(A, f);
    if (ev->body.type == A->midi_Event) handle_midi(A, ev);
    else if (ev->body.type == A->atom_Object) {
      const LV2_Atom_Object* o = (const LV2_Atom_Object*)&ev->body;
      if (o->body.otype == A->time_Position) handle_pos(A, o);
      else if (o->body.otype == A->patch_Set) handle_patch_set(A, o);
      else if (o->body.otype == A->patch_Get) A->report_next = 0;   /* host asks for all parameters */
    }
  }
  advance_to(A, n);
  report_params(A, n);
  if (A->step_count_out) *A->step_count_out = (float)(A->core.step_count & 0xFFFFFFu);
  for (int i = 0; i < ARP_MAX_STEPS; i++)
    if (A->dev_out[i]) *A->dev_out[i] = pack_dev(A->core.dev[i]);
}

static void cleanup(LV2_Handle h) { free(h); }
/* LV2 State: the pattern steps (LV2 parameters) go into presets; control ports are saved by the host. */
static LV2_State_Status save(LV2_Handle h, LV2_State_Store_Function store, LV2_State_Handle sh,
                             uint32_t flags, const LV2_Feature* const* features) {
  (void)flags; (void)features;
  Arp* A = (Arp*)h;
  for (int i = 0; i < ARP_MAX_STEPS; i++)
    for (int f = 0; f < N_FIELDS; f++) {
      const int32_t v = A->stepv[i][f];
      store(sh, A->param[i][f], &v, sizeof v, A->atom_Int, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
    }
  return LV2_STATE_SUCCESS;
}

static LV2_State_Status restore(LV2_Handle h, LV2_State_Retrieve_Function retrieve, LV2_State_Handle sh,
                                uint32_t flags, const LV2_Feature* const* features) {
  (void)flags; (void)features;
  Arp* A = (Arp*)h;
  for (int i = 0; i < ARP_MAX_STEPS; i++) {
    for (int f = 0; f < N_FIELDS; f++) {
      size_t size = 0; uint32_t type = 0, vflags = 0;
      const void* v = retrieve(sh, A->param[i][f], &size, &type, &vflags);
      int val = field_defaults[f];                       /* missing in the preset: default */
      if (v && type == A->atom_Int && size >= sizeof(int32_t)) val = *(const int32_t*)v;
      else if (v && type == A->atom_Float && size >= sizeof(float)) val = (int)lrintf(*(const float*)v);
      else if (v && type == A->atom_Long && size >= sizeof(int64_t)) val = (int)*(const int64_t*)v;
      else if (v && type == A->atom_Double && size >= sizeof(double)) val = (int)lrint(*(const double*)v);
      A->stepv[i][f] = val;
      arp_clear_mutation(&A->core, i, f);                /* a loaded pattern starts without drift */
    }
    apply_step(A, i);
  }
  A->report_next = 0;                                    /* tell the host the new values */
  return LV2_STATE_SUCCESS;
}

static const void* extension_data(const char* uri) {
  static const LV2_State_Interface state = { save, restore };
  return strcmp(uri, LV2_STATE__interface) ? NULL : &state;
}

static const LV2_Descriptor desc = { ARP_URI, instantiate, connect_port, activate,
                                     run, deactivate, cleanup, extension_data };
LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t i) { return i == 0 ? &desc : NULL; }
