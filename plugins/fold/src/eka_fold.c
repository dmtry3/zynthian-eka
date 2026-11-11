/* Eka Fold: LV2 wrapper around fold_core. Stage 0: 4-voice sine with an ADSR.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/core/lv2.h>
#include <lv2/midi/midi.h>
#include <lv2/urid/urid.h>

#include "fold_core.h"

#define FOLD_URI "http://dmtry3.github.io/zynthian-eka/fold"
#define BEND_RANGE 2.0f                  /* semitones; a setting in a later stage */

/* Port indices: the same as lv2/eka_fold.ttl. Control inputs first (Zynthian needs them at 0..N-1). */
enum {
  P_PITCH, P_FINE, P_ATTACK, P_DECAY, P_SUSTAIN, P_RELEASE,
  P_END,                                 /* number of control inputs */
  P_MIDI_IN = P_END, P_OUT_L, P_OUT_R,
  P_NOTE0,                               /* note1..4 */
  P_PEAK = P_NOTE0 + FC_VOICES
};

typedef struct {
  const float* ctl[P_END];
  const LV2_Atom_Sequence* in;
  float* out_l;
  float* out_r;
  float* note_out[FC_VOICES];
  float* peak_out;
  LV2_URID midi_Event;
  FoldCore core;
} Fold;

static LV2_Handle instantiate(const LV2_Descriptor* d, double rate, const char* bundle,
                              const LV2_Feature* const* f) {
  (void)d; (void)bundle;
  LV2_URID_Map* map = NULL;
  for (int i = 0; f[i]; i++)
    if (!strcmp(f[i]->URI, LV2_URID__map)) map = (LV2_URID_Map*)f[i]->data;
  if (!map) return NULL;
  Fold* S = (Fold*)calloc(1, sizeof(Fold));
  if (!S) return NULL;
  S->midi_Event = map->map(map->handle, LV2_MIDI__MidiEvent);
  fc_init(&S->core, (float)rate);
  return S;
}

static void connect_port(LV2_Handle h, uint32_t port, void* data) {
  Fold* S = (Fold*)h;
  if (port < P_END) S->ctl[port] = (const float*)data;
  else if (port == P_MIDI_IN) S->in = (const LV2_Atom_Sequence*)data;
  else if (port == P_OUT_L) S->out_l = (float*)data;
  else if (port == P_OUT_R) S->out_r = (float*)data;
  else if (port >= P_NOTE0 && port < P_NOTE0 + FC_VOICES) S->note_out[port - P_NOTE0] = (float*)data;
  else if (port == P_PEAK) S->peak_out = (float*)data;
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
  p.pitch = ctl(S, P_PITCH, p.pitch);
  p.fine = ctl(S, P_FINE, p.fine);
  p.attack_ms = ctl(S, P_ATTACK, p.attack_ms);
  p.decay_ms = ctl(S, P_DECAY, p.decay_ms);
  p.sustain = ctl(S, P_SUSTAIN, p.sustain * 100.0f) * 0.01f;
  p.release_ms = ctl(S, P_RELEASE, p.release_ms);
  fc_set_params(&S->core, &p);
}

static void handle_midi(Fold* S, const LV2_Atom_Event* ev) {
  const uint8_t* b = (const uint8_t*)LV2_ATOM_BODY_CONST(&ev->body);
  if (ev->body.size < 3) return;
  const uint8_t type = b[0] & 0xF0;
  if (type == 0x90 && b[2] > 0) fc_note_on(&S->core, b[1] & 0x7F, b[2]);
  else if (type == 0x80 || type == 0x90) fc_note_off(&S->core, b[1] & 0x7F);
  else if (type == 0xB0 && b[1] == 120) fc_all_sound_off(&S->core);       /* All Sound Off */
  else if (type == 0xB0 && b[1] == 123) fc_all_notes_off(&S->core);      /* All Notes Off */
  else if (type == 0xB0 && b[1] == 64) fc_set_sustain(&S->core, b[2] >= 64);   /* Sustain pedal */
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
  uint32_t done = 0;                                 /* audio rendered up to here: events are sample-accurate */
  if (S->in) {
    LV2_ATOM_SEQUENCE_FOREACH(S->in, ev) {
      uint32_t f = (uint32_t)ev->time.frames;
      if (f > n) f = n;
      if (f > done) { render(S, done, f); done = f; }
      if (ev->body.type == S->midi_Event) handle_midi(S, ev);
    }
  }
  render(S, done, n);

  float peak = 0.0f;
  for (uint32_t i = 0; i < n; i++) {
    const float a = fabsf(S->out_l[i]);
    peak = a > peak ? a : peak;
  }
  if (S->peak_out) *S->peak_out = peak > 1e-5f ? 20.0f * log10f(peak) : -100.0f;
  for (int i = 0; i < FC_VOICES; i++)
    if (S->note_out[i]) *S->note_out[i] = (float)fc_voice_note(&S->core, i);
}

static void cleanup(LV2_Handle h) { free(h); }

static const void* extension_data(const char* uri) { (void)uri; return NULL; }

static const LV2_Descriptor desc = { FOLD_URI, instantiate, connect_port, activate,
                                     run, deactivate, cleanup, extension_data };
LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t i) { return i == 0 ? &desc : NULL; }
