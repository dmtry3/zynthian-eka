/* Eka Fold: LV2 wrapper around fold_core.
 * Knob pages Osc (Source, Harmonics, Color, Morph), Mod, Timbre, Gate, Env, Delay. Pitch and Fine, the routing
 * switches FM / AM / Sync / Timbre, the function generator modes, the delay's Shaper and EQ, and the mod matrix (m_<source>_<destination>, -100..100)
 * are LV2 parameters
 * (patch:writable, notOnGUI) set in the widget and saved in LV2 State: jalv in Zynthian does not
 * register ports with notOnGUI, so they could not be hidden ports.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <lv2/atom/atom.h>
#include <lv2/atom/forge.h>
#include <lv2/atom/util.h>
#include <lv2/core/lv2.h>
#include <lv2/midi/midi.h>
#include <lv2/patch/patch.h>
#include <lv2/state/state.h>
#include <lv2/urid/urid.h>

#include "fold_core.h"

#define FOLD_URI "http://dmtry3.github.io/zynthian-eka/fold"
#define BEND_RANGE 2.0f                  /* semitones; a setting in a later stage */

/* Port indices: the same as lv2/eka_fold.ttl. Control inputs first (0..P_END-1): Zynthian numbers LV2
 * parameters from len(control ports)+1, so a control port with a higher index would be overwritten. */
enum {
  P_SOURCE, P_HARMONICS, P_COLOR, P_MORPH,
  P_MOD_FREQ, P_MOD_INDEX, P_MOD_SHAPE, P_MOD_MODE,
  P_TIMBRE, P_ORDER, P_SYMMETRY, P_DRIVE,
  P_GATE_MODE, P_GATE_DECAY, P_GATE_RES, P_LEVEL,
  P_FG1_RISE, P_FG1_FALL, P_FG2_RISE, P_FG2_FALL,
  P_DELAY_MODE, P_DELAY_TIME, P_DELAY_FB, P_DELAY_MIX,
  P_END,                                 /* number of control inputs */
  P_MIDI_IN = P_END, P_OUT_L, P_OUT_R, P_NOTIFY,
  P_NOTE0,                               /* note1..4 */
  P_PEAK = P_NOTE0 + FC_VOICES,
  P_MOD_PHASE, P_GATE_LEVEL, P_FG1, P_FG2, P_DELAY_LEVEL
};

/* Integer LV2 parameters set in the widget, saved in State. */
/* The mod matrix follows: cell (s, d) is IP_MATRIX + s x FC_NMDST + d, named m_<source>_<destination>. */
enum { IP_ROUTE_FM, IP_ROUTE_AM, IP_ROUTE_SYNC, IP_ROUTE_TIMBRE, IP_FG1_MODE, IP_FG2_MODE, IP_PITCH, IP_FINE,
       IP_DL_SHAPER, IP_DL_EQ_FREQ, IP_DL_EQ_GAIN, IP_MATRIX, N_IPARAMS = IP_MATRIX + FC_NCELLS };
static const char* const ip_names[IP_MATRIX] = { "route_fm", "route_am", "route_sync", "route_timbre",
  "fg1_mode", "fg2_mode", "pitch", "fine", "delay_shaper", "delay_eq_freq", "delay_eq_gain" };
static const int ip_range[IP_MATRIX + 1][3] = {    /* default, min, max */
  { 1, 0, 1 }, { 0, 0, 1 }, { 0, 0, 1 }, { 0, 0, 1 },
  { FC_FG_GATE, 0, FC_NFG_MODES - 1 }, { FC_FG_GATE, 0, FC_NFG_MODES - 1 },
  { 0, -24, 24 }, { 0, -100, 100 },               /* pitch (semitones), fine (cents) */
  { 30, 0, 100 }, { 50, 0, 100 }, { 0, -180, 180 },   /* delay: Shaper %, EQ Freq % (100 Hz..10 kHz), EQ Gain 0.1 dB */
  { 0, -100, 100 } };                              /* every matrix cell (%) */
static const int* range(int i) { return ip_range[i < IP_MATRIX ? i : IP_MATRIX]; }
#define REPORT_PER_RUN 16                /* patch:Set replies per run() after patch:Get */

typedef struct {
  const float* ctl[P_END];
  const LV2_Atom_Sequence* in;
  LV2_Atom_Sequence* notify;
  float* out_l;
  float* out_r;
  float* note_out[FC_VOICES];
  float* peak_out;
  float* mod_phase_out;
  float* gate_level_out;
  float* fg_out[2];
  float* delay_level_out;

  LV2_Atom_Forge forge;
  LV2_URID midi_Event, atom_Object, atom_URID, atom_Int, atom_Long, atom_Float, atom_Double;
  LV2_URID patch_Set, patch_Get, patch_property, patch_value;
  LV2_URID ip_urid[N_IPARAMS];
  int ip[N_IPARAMS];
  int report_next;                       /* next parameter to tell the host, -1 = none */
  FoldCore core;
} Fold;

static LV2_Handle instantiate(const LV2_Descriptor* d, double rate, const char* bundle,
                              const LV2_Feature* const* f) {
  (void)d; (void)bundle;
  LV2_URID_Map* m = NULL;
  for (int i = 0; f[i]; i++)
    if (!strcmp(f[i]->URI, LV2_URID__map)) m = (LV2_URID_Map*)f[i]->data;
  if (!m) return NULL;
  Fold* S = (Fold*)calloc(1, sizeof(Fold));
  if (!S) return NULL;
  S->midi_Event     = m->map(m->handle, LV2_MIDI__MidiEvent);
  S->atom_Object    = m->map(m->handle, LV2_ATOM__Object);
  S->atom_URID      = m->map(m->handle, LV2_ATOM__URID);
  S->atom_Int       = m->map(m->handle, LV2_ATOM__Int);
  S->atom_Long      = m->map(m->handle, LV2_ATOM__Long);
  S->atom_Float     = m->map(m->handle, LV2_ATOM__Float);
  S->atom_Double    = m->map(m->handle, LV2_ATOM__Double);
  S->patch_Set      = m->map(m->handle, LV2_PATCH__Set);
  S->patch_Get      = m->map(m->handle, LV2_PATCH__Get);
  S->patch_property = m->map(m->handle, LV2_PATCH__property);
  S->patch_value    = m->map(m->handle, LV2_PATCH__value);
  for (int i = 0; i < N_IPARAMS; i++) {
    char uri[96];
    strcpy(uri, FOLD_URI "#");
    if (i < IP_MATRIX) strcat(uri, ip_names[i]);
    else {
      strcat(uri, "m_");
      strcat(uri, fc_msrc_names[(i - IP_MATRIX) / FC_NMDST]);
      strcat(uri, "_");
      strcat(uri, fc_mdst_names[(i - IP_MATRIX) % FC_NMDST]);
    }
    S->ip_urid[i] = m->map(m->handle, uri);
    S->ip[i] = range(i)[0];
  }
  S->report_next = -1;
  lv2_atom_forge_init(&S->forge, m);
  fc_init(&S->core, (float)rate);
  return S;
}

static void connect_port(LV2_Handle h, uint32_t port, void* data) {
  Fold* S = (Fold*)h;
  if (port < P_END) S->ctl[port] = (const float*)data;
  else if (port == P_MIDI_IN) S->in = (const LV2_Atom_Sequence*)data;
  else if (port == P_OUT_L) S->out_l = (float*)data;
  else if (port == P_OUT_R) S->out_r = (float*)data;
  else if (port == P_NOTIFY) S->notify = (LV2_Atom_Sequence*)data;
  else if (port >= P_NOTE0 && port < P_NOTE0 + FC_VOICES) S->note_out[port - P_NOTE0] = (float*)data;
  else if (port == P_PEAK) S->peak_out = (float*)data;
  else if (port == P_MOD_PHASE) S->mod_phase_out = (float*)data;
  else if (port == P_GATE_LEVEL) S->gate_level_out = (float*)data;
  else if (port == P_FG1) S->fg_out[0] = (float*)data;
  else if (port == P_FG2) S->fg_out[1] = (float*)data;
  else if (port == P_DELAY_LEVEL) S->delay_level_out = (float*)data;
}

static void activate(LV2_Handle h) { (void)h; }
static void deactivate(LV2_Handle h) { (void)h; }

static float ctl(const Fold* S, int port, float def) {
  const float* p = S->ctl[port];
  return p && isfinite(*p) ? *p : def;
}

static void read_params(Fold* S) {
  FcParams p;
  fc_default_params(&p);
  p.pitch = (float)S->ip[IP_PITCH];
  p.fine = (float)S->ip[IP_FINE];
  p.source = (int)lrintf(ctl(S, P_SOURCE, (float)p.source));
  p.harmonics = ctl(S, P_HARMONICS, p.harmonics * 100.0f) * 0.01f;
  p.color = ctl(S, P_COLOR, p.color * 100.0f) * 0.01f;
  p.morph = ctl(S, P_MORPH, p.morph * 100.0f) * 0.01f;
  p.fg_rise_ms[0] = ctl(S, P_FG1_RISE, p.fg_rise_ms[0]);
  p.fg_fall_ms[0] = ctl(S, P_FG1_FALL, p.fg_fall_ms[0]);
  p.fg_rise_ms[1] = ctl(S, P_FG2_RISE, p.fg_rise_ms[1]);
  p.fg_fall_ms[1] = ctl(S, P_FG2_FALL, p.fg_fall_ms[1]);
  p.fg_mode[0] = S->ip[IP_FG1_MODE];
  p.fg_mode[1] = S->ip[IP_FG2_MODE];
  for (int i = 0; i < FC_NCELLS; i++) p.matrix[i / FC_NMDST][i % FC_NMDST] = (float)S->ip[IP_MATRIX + i] * 0.01f;
  p.gate_mode = (int)lrintf(ctl(S, P_GATE_MODE, (float)p.gate_mode));
  p.gate_decay = ctl(S, P_GATE_DECAY, p.gate_decay * 100.0f) * 0.01f;
  p.gate_res = ctl(S, P_GATE_RES, p.gate_res * 100.0f) * 0.01f;
  p.level = ctl(S, P_LEVEL, p.level * 100.0f) * 0.01f;
  p.mod_freq = ctl(S, P_MOD_FREQ, p.mod_freq);
  p.mod_index = ctl(S, P_MOD_INDEX, p.mod_index * 100.0f) * 0.01f;
  p.mod_shape = (int)lrintf(ctl(S, P_MOD_SHAPE, (float)p.mod_shape));
  p.mod_mode = (int)lrintf(ctl(S, P_MOD_MODE, (float)p.mod_mode));
  p.route_fm = S->ip[IP_ROUTE_FM];
  p.route_am = S->ip[IP_ROUTE_AM];
  p.route_sync = S->ip[IP_ROUTE_SYNC];
  p.route_timbre = S->ip[IP_ROUTE_TIMBRE];
  p.timbre = ctl(S, P_TIMBRE, p.timbre * 100.0f) * 0.01f;
  p.order = ctl(S, P_ORDER, p.order * 100.0f) * 0.01f;
  p.symmetry = ctl(S, P_SYMMETRY, p.symmetry * 100.0f) * 0.01f;
  p.drive = ctl(S, P_DRIVE, p.drive * 100.0f) * 0.01f;
  p.delay_mode = (int)lrintf(ctl(S, P_DELAY_MODE, (float)p.delay_mode));
  p.delay_time = ctl(S, P_DELAY_TIME, p.delay_time);
  p.delay_fb = ctl(S, P_DELAY_FB, p.delay_fb * 100.0f) * 0.01f;
  p.delay_mix = ctl(S, P_DELAY_MIX, p.delay_mix * 100.0f) * 0.01f;
  p.delay_shaper = (float)S->ip[IP_DL_SHAPER] * 0.01f;
  p.delay_eq_freq = (float)S->ip[IP_DL_EQ_FREQ] * 0.01f;
  p.delay_eq_db = (float)S->ip[IP_DL_EQ_GAIN] * 0.1f;
  fc_set_params(&S->core, &p);
}

/* ---------- LV2 parameters (patch:Set / patch:Get) ---------- */
/* A number of any atom type (Zynthian sends floats), or `def`. */
static int atom_int(const Fold* S, const LV2_Atom* a, int def) {
  if (a->type == S->atom_Int) return ((const LV2_Atom_Int*)a)->body;
  if (a->type == S->atom_Long) return (int)((const LV2_Atom_Long*)a)->body;
  if (a->type == S->atom_Float) return (int)lrintf(((const LV2_Atom_Float*)a)->body);
  if (a->type == S->atom_Double) return (int)lrint(((const LV2_Atom_Double*)a)->body);
  return def;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void handle_patch_set(Fold* S, const LV2_Atom_Object* obj) {
  const LV2_Atom *prop = NULL, *val = NULL;
  lv2_atom_object_get(obj, S->patch_property, &prop, S->patch_value, &val, 0);
  if (!prop || prop->type != S->atom_URID || !val) return;
  const LV2_URID key = ((const LV2_Atom_URID*)prop)->body;
  for (int i = 0; i < N_IPARAMS; i++)
    if (key == S->ip_urid[i]) {
      S->ip[i] = clampi(atom_int(S, val, S->ip[i]), range(i)[1], range(i)[2]);
      return;
    }
}

/* patch:Set of a parameter on the notify port: jalv prints them, Zynthian's controllers follow. */
static int report_param(Fold* S, int i) {
  LV2_Atom_Forge* F = &S->forge;
  LV2_Atom_Forge_Frame fr;
  if (!lv2_atom_forge_frame_time(F, 0)) return 0;
  if (!lv2_atom_forge_object(F, &fr, 0, S->patch_Set)) return 0;
  lv2_atom_forge_key(F, S->patch_property);
  lv2_atom_forge_urid(F, S->ip_urid[i]);
  lv2_atom_forge_key(F, S->patch_value);
  if (!lv2_atom_forge_int(F, S->ip[i])) return 0;
  lv2_atom_forge_pop(F, &fr);
  return 1;
}

/* After patch:Get or a restore: every parameter, REPORT_PER_RUN per run. */
static void report_params(Fold* S) {
  for (int k = 0; k < REPORT_PER_RUN && S->report_next >= 0; k++) {
    if (!report_param(S, S->report_next)) return;   /* buffer full: go on in the next run */
    if (++S->report_next >= N_IPARAMS) S->report_next = -1;
  }
}

/* ---------- MIDI and audio ---------- */
static void handle_midi(Fold* S, const LV2_Atom_Event* ev) {
  const uint8_t* b = (const uint8_t*)LV2_ATOM_BODY_CONST(&ev->body);
  if (ev->body.size >= 2 && (b[0] & 0xF0) == 0xD0) {                     /* Channel pressure: 2 bytes */
    fc_set_aftertouch(&S->core, (float)(b[1] & 0x7F) / 127.0f);
    return;
  }
  if (ev->body.size < 3) return;
  const uint8_t type = b[0] & 0xF0;
  if (type == 0x90 && b[2] > 0) fc_note_on(&S->core, b[1] & 0x7F, b[2]);
  else if (type == 0x80 || type == 0x90) fc_note_off(&S->core, b[1] & 0x7F);
  else if (type == 0xB0 && b[1] == 120) fc_all_sound_off(&S->core);       /* All Sound Off */
  else if (type == 0xB0 && b[1] == 123) fc_all_notes_off(&S->core);      /* All Notes Off */
  else if (type == 0xB0 && b[1] == 64) fc_set_sustain(&S->core, b[2] >= 64);   /* Sustain pedal */
  else if (type == 0xB0 && b[1] == 1) fc_set_wheel(&S->core, (float)(b[2] & 0x7F) / 127.0f);   /* Mod wheel */
  else if (type == 0xA0) fc_set_poly_aftertouch(&S->core, b[1] & 0x7F, (float)(b[2] & 0x7F) / 127.0f);
  else if (type == 0xE0) {                                                /* Pitch Bend, 14 bit */
    const int v = ((b[2] & 0x7F) << 7 | (b[1] & 0x7F)) - 8192;
    const float bend = v < 0 ? (float)v / 8192.0f : (float)v / 8191.0f;
    fc_set_bend(&S->core, bend * BEND_RANGE);
  }
}

static void render(Fold* S, uint32_t from, uint32_t to) {
  if (to > from) fc_process(&S->core, S->out_l + from, S->out_r + from, to - from);
}

static void run(LV2_Handle h, uint32_t n) {
  Fold* S = (Fold*)h;
  fc_enable_ftz();
  read_params(S);
  LV2_Atom_Forge_Frame seq;
  if (S->notify) {
    lv2_atom_forge_set_buffer(&S->forge, (uint8_t*)S->notify, S->notify->atom.size);
    lv2_atom_forge_sequence_head(&S->forge, &seq, 0);
  }
  uint32_t done = 0;                                 /* audio rendered up to here: events are sample-accurate */
  if (S->in) {
    LV2_ATOM_SEQUENCE_FOREACH(S->in, ev) {
      uint32_t f = (uint32_t)ev->time.frames;
      if (f > n) f = n;
      if (f > done) { render(S, done, f); done = f; }
      if (ev->body.type == S->midi_Event) handle_midi(S, ev);
      else if (ev->body.type == S->atom_Object) {
        const LV2_Atom_Object* o = (const LV2_Atom_Object*)&ev->body;
        if (o->body.otype == S->patch_Set) { handle_patch_set(S, o); read_params(S); }
        else if (o->body.otype == S->patch_Get) S->report_next = 0;
      }
    }
  }
  render(S, done, n);
  if (S->notify) {
    if (S->report_next >= 0) report_params(S);
    lv2_atom_forge_pop(&S->forge, &seq);
  }

  float peak = 0.0f;
  for (uint32_t i = 0; i < n; i++) {
    const float a = fabsf(S->out_l[i]);
    peak = a > peak ? a : peak;
  }
  if (S->peak_out) *S->peak_out = peak > 1e-5f ? 20.0f * log10f(peak) : -100.0f;
  for (int i = 0; i < FC_VOICES; i++)
    if (S->note_out[i]) *S->note_out[i] = (float)fc_voice_note(&S->core, i);
  if (S->mod_phase_out) *S->mod_phase_out = fc_mod_phase(&S->core);
  if (S->gate_level_out) *S->gate_level_out = fc_gate_level(&S->core);
  for (int i = 0; i < 2; i++)
    if (S->fg_out[i]) *S->fg_out[i] = fc_fg_pos(&S->core, i);
  if (S->delay_level_out) *S->delay_level_out = fc_delay_level(&S->core);
}

static void cleanup(LV2_Handle h) { free(h); }

/* ---------- state: the routing switches go into snapshots and presets ---------- */
static LV2_State_Status save(LV2_Handle h, LV2_State_Store_Function store, LV2_State_Handle sh,
                             uint32_t flags, const LV2_Feature* const* features) {
  (void)flags; (void)features;
  Fold* S = (Fold*)h;
  for (int i = 0; i < N_IPARAMS; i++) {
    const int32_t v = S->ip[i];
    store(sh, S->ip_urid[i], &v, sizeof v, S->atom_Int, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
  }
  return LV2_STATE_SUCCESS;
}

/* Not called concurrently with run(). Missing values (older presets): defaults. */
static LV2_State_Status restore(LV2_Handle h, LV2_State_Retrieve_Function retrieve, LV2_State_Handle sh,
                                uint32_t flags, const LV2_Feature* const* features) {
  (void)flags; (void)features;
  Fold* S = (Fold*)h;
  for (int i = 0; i < N_IPARAMS; i++) {
    size_t size = 0; uint32_t type = 0, vflags = 0;
    const void* v = retrieve(sh, S->ip_urid[i], &size, &type, &vflags);
    S->ip[i] = range(i)[0];
    if (v && type == S->atom_Int && size >= sizeof(int32_t))
      S->ip[i] = clampi(*(const int32_t*)v, range(i)[1], range(i)[2]);
  }
  S->report_next = 0;                                /* tell the host the restored values */
  return LV2_STATE_SUCCESS;
}

static const void* extension_data(const char* uri) {
  static const LV2_State_Interface state = { save, restore };
  if (!strcmp(uri, LV2_STATE__interface)) return &state;
  return NULL;
}

static const LV2_Descriptor desc = { FOLD_URI, instantiate, connect_port, activate,
                                     run, deactivate, cleanup, extension_data };
LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t i) { return i == 0 ? &desc : NULL; }
