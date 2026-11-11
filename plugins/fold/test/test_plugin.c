/* A tiny LV2 host for the built plugin: URID map, MIDI and patch:Set / patch:Get in an atom sequence,
 * the notify port, State save and restore, the output ports. No jalv needed.
 * Usage: test_plugin <path to eka_fold.so> */
#include <dlfcn.h>
#include <math.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/forge.h>
#include <lv2/atom/util.h>
#include <lv2/core/lv2.h>
#include <lv2/midi/midi.h>
#include <lv2/patch/patch.h>
#include <lv2/state/state.h>
#include <lv2/urid/urid.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

#define FOLD_URI "http://dmtry3.github.io/zynthian-eka/fold"
#define N 256
/* ports, as in lv2/eka_fold.ttl */
enum { P_PITCH, P_FINE, P_ATTACK, P_DECAY, P_SUSTAIN, P_RELEASE, P_MOD_FREQ, P_MOD_INDEX, P_MOD_SHAPE, P_MOD_MODE,
       P_MIDI_IN, P_OUT_L, P_OUT_R, P_NOTIFY, P_NOTE1, P_NOTE4 = P_NOTE1 + 3, P_PEAK, P_MOD_PHASE, P_COUNT };

static char* uris[64];
static int nuris = 0;
static LV2_URID map_uri(LV2_URID_Map_Handle h, const char* uri) {
  (void)h;
  for (int i = 0; i < nuris; i++) if (!strcmp(uris[i], uri)) return (LV2_URID)(i + 1);
  uris[nuris] = strdup(uri);
  return (LV2_URID)++nuris;
}
static LV2_URID_Map map = { NULL, map_uri };
#define U(s) map_uri(NULL, s)

/* ---------- input: an atom sequence the plugin reads ---------- */
static uint64_t seq_buf[1024];
static LV2_Atom_Sequence* seq = (LV2_Atom_Sequence*)seq_buf;
static LV2_Atom_Forge forge;
static LV2_Atom_Forge_Frame seq_frame;

static void seq_clear(void) {
  lv2_atom_forge_set_buffer(&forge, (uint8_t*)seq_buf, sizeof seq_buf);
  lv2_atom_forge_sequence_head(&forge, &seq_frame, 0);
}

static void seq_midi(int64_t frame, uint8_t a, uint8_t b, uint8_t c) {
  const uint8_t msg[3] = { a, b, c };
  lv2_atom_forge_frame_time(&forge, frame);
  lv2_atom_forge_atom(&forge, 3, U(LV2_MIDI__MidiEvent));
  lv2_atom_forge_write(&forge, msg, 3);
}

/* patch:Set of a parameter, with a float value like Zynthian sends */
static void seq_set(const char* sym, float value) {
  char uri[128];
  snprintf(uri, sizeof uri, FOLD_URI "#%s", sym);
  static LV2_Atom_Forge_Frame fr;                   /* static: the forge keeps a pointer to it until the pop */
  lv2_atom_forge_frame_time(&forge, 0);
  lv2_atom_forge_object(&forge, &fr, 0, U(LV2_PATCH__Set));
  lv2_atom_forge_key(&forge, U(LV2_PATCH__property));
  lv2_atom_forge_urid(&forge, U(uri));
  lv2_atom_forge_key(&forge, U(LV2_PATCH__value));
  lv2_atom_forge_float(&forge, value);
  lv2_atom_forge_pop(&forge, &fr);
}

static void seq_get(void) {
  static LV2_Atom_Forge_Frame fr;
  lv2_atom_forge_frame_time(&forge, 0);
  lv2_atom_forge_object(&forge, &fr, 0, U(LV2_PATCH__Get));
  lv2_atom_forge_pop(&forge, &fr);
}

/* ---------- output: the notify port ---------- */
static uint64_t notify_buf[1024];
static LV2_Atom_Sequence* notify = (LV2_Atom_Sequence*)notify_buf;

static void notify_reset(void) {
  notify->atom.type = 0;
  notify->atom.size = sizeof notify_buf - sizeof(LV2_Atom);
}

/* Value the plugin reported for a parameter on the notify port, or -1000. */
static int reported(const char* sym) {
  char uri[128];
  snprintf(uri, sizeof uri, FOLD_URI "#%s", sym);
  int v = -1000;
  LV2_ATOM_SEQUENCE_FOREACH(notify, ev) {
    const LV2_Atom_Object* o = (const LV2_Atom_Object*)&ev->body;
    if (o->atom.type != U(LV2_ATOM__Object) || o->body.otype != U(LV2_PATCH__Set)) continue;
    const LV2_Atom *prop = NULL, *val = NULL;
    lv2_atom_object_get(o, U(LV2_PATCH__property), &prop, U(LV2_PATCH__value), &val, 0);
    if (prop && ((const LV2_Atom_URID*)prop)->body == U(uri) && val && val->type == U(LV2_ATOM__Int))
      v = ((const LV2_Atom_Int*)val)->body;
  }
  return v;
}

/* ---------- state: a small key -> value store ---------- */
typedef struct { uint32_t key, type; size_t size; uint8_t value[64]; } Item;
static Item items[16];
static int nitems = 0;

static LV2_State_Status store(LV2_State_Handle h, uint32_t key, const void* value, size_t size,
                              uint32_t type, uint32_t flags) {
  (void)h; (void)flags;
  if (nitems >= 16 || size > 64) return LV2_STATE_ERR_UNKNOWN;
  items[nitems].key = key; items[nitems].type = type; items[nitems].size = size;
  memcpy(items[nitems].value, value, size);
  nitems++;
  return LV2_STATE_SUCCESS;
}

static const void* retrieve(LV2_State_Handle h, uint32_t key, size_t* size, uint32_t* type, uint32_t* flags) {
  (void)h;
  for (int i = 0; i < nitems; i++)
    if (items[i].key == key) { *size = items[i].size; *type = items[i].type; *flags = 0; return items[i].value; }
  return NULL;
}

static float ctl[P_COUNT];
static float out_l[N], out_r[N];

static float peak(const float* x, int a, int b) {
  float p = 0;
  for (int i = a; i < b; i++) p = fabsf(x[i]) > p ? fabsf(x[i]) : p;
  return p;
}

static const LV2_Descriptor* d;
static LV2_Handle h;

/* Runs blocks for `secs` and counts the rising zero crossings. */
static int crossings(double secs) {
  int cross = 0;
  float prev = 0.0f;                                                     /* across block boundaries too */
  for (int b = 0; b < (int)(secs * 48000 / N); b++) {
    notify_reset();
    d->run(h, N);
    for (int i = 0; i < N; i++) { cross += prev < 0.0f && out_l[i] >= 0.0f; prev = out_l[i]; }
  }
  return cross;
}

static void run_seq(void) {
  lv2_atom_forge_pop(&forge, &seq_frame);
  notify_reset();
  d->run(h, N);
  seq_clear();
}

int main(int argc, char** argv) {
  if (argc < 2) { printf("usage: test_plugin eka_fold.so\n"); return 2; }
  void* lib = dlopen(argv[1], RTLD_NOW);
  if (!lib) { printf("FAIL dlopen: %s\n", dlerror()); return 1; }
  LV2_Descriptor_Function df = (LV2_Descriptor_Function)dlsym(lib, "lv2_descriptor");
  CHECK(df != NULL);
  d = df(0);
  CHECK(d && !strcmp(d->URI, FOLD_URI));
  CHECK(df(1) == NULL);
  lv2_atom_forge_init(&forge, &map);

  const LV2_Feature none = { "urn:none", NULL };
  const LV2_Feature* no_map[] = { &none, NULL };
  CHECK(d->instantiate(d, 48000, "", no_map) == NULL);                   /* urid:map is required */

  const LV2_Feature map_f = { LV2_URID__map, &map };
  const LV2_Feature* feats[] = { &map_f, NULL };
  h = d->instantiate(d, 48000, "", feats);
  CHECK(h != NULL);
  if (!h) return 1;
  /* the knob defaults of the .ttl */
  ctl[P_PITCH] = 0; ctl[P_FINE] = 0; ctl[P_ATTACK] = 5; ctl[P_DECAY] = 300; ctl[P_SUSTAIN] = 70; ctl[P_RELEASE] = 400;
  ctl[P_MOD_FREQ] = 30; ctl[P_MOD_INDEX] = 0; ctl[P_MOD_SHAPE] = 1; ctl[P_MOD_MODE] = 2;
  for (int p = 0; p < P_COUNT; p++) {
    if (p == P_MIDI_IN) d->connect_port(h, p, seq);
    else if (p == P_NOTIFY) d->connect_port(h, p, notify);
    else if (p == P_OUT_L) d->connect_port(h, p, out_l);
    else if (p == P_OUT_R) d->connect_port(h, p, out_r);
    else d->connect_port(h, p, &ctl[p]);
  }
  d->activate(h);
  const LV2_State_Interface* si = (const LV2_State_Interface*)d->extension_data(LV2_STATE__interface);
  CHECK(si && si->save && si->restore);

  /* silence without notes */
  seq_clear();
  run_seq();
  CHECK(peak(out_l, 0, N) == 0.0f);
  CHECK(ctl[P_PEAK] == -100.0f);
  CHECK(ctl[P_NOTE1] == -1.0f && ctl[P_NOTE4] == -1.0f);
  CHECK(ctl[P_MOD_PHASE] == -1.0f);

  /* a note on at frame 100: sample-accurate (the Principal comes one sample late: the sync correction) */
  seq_midi(100, 0x90, 60, 127);
  run_seq();
  CHECK(peak(out_l, 0, 101) == 0.0f);
  CHECK(peak(out_l, 101, N) > 0.0f);
  CHECK(!memcmp(out_l, out_r, sizeof out_l));
  CHECK(ctl[P_NOTE1] == 60.0f);
  CHECK(ctl[P_PEAK] > -100.0f && ctl[P_PEAK] <= 0.0f);
  CHECK(ctl[P_MOD_PHASE] >= 0.0f && ctl[P_MOD_PHASE] < 1.0f);

  /* a chord on another MIDI channel, then 2 s: peak at sustain, 4 notes shown */
  seq_midi(0, 0x93, 64, 127);
  seq_midi(0, 0x93, 67, 127);
  seq_midi(0, 0x93, 72, 127);
  run_seq();
  int shown = 0;
  for (int i = 0; i < 4; i++) shown += ctl[P_NOTE1 + i] >= 0.0f;
  CHECK(shown == 4);
  for (int i = 0; i < 400; i++) run_seq();
  CHECK(ctl[P_PEAK] < 0.0f);

  /* the Pitch knob: an octave up, the frequency doubles */
  seq_midi(0, 0xB0, 123, 0);                                            /* All Notes Off */
  run_seq();
  for (int i = 0; i < 200; i++) run_seq();
  CHECK(ctl[P_NOTE1] == -1.0f);
  ctl[P_PITCH] = 12;
  seq_midi(0, 0x90, 69, 127);
  run_seq();
  int cross = crossings(2.0);
  printf("A4 + 12: %d crossings in 2 s\n", cross);
  CHECK(cross >= 1758 && cross <= 1762);

  /* patch:Get: the plugin reports the routing switches (defaults FM on, AM and Sync off) */
  seq_get();
  run_seq();
  CHECK(reported("route_fm") == 1 && reported("route_am") == 0 && reported("route_sync") == 0);

  /* AM on through patch:Set (FM off), Index 100 %, ratio 1: ring modulation sin^2, never below zero.
   * A new note: Ratio mode restarts both phases together (switching while a note sounds glides through some
   * FM for 5 ms, which shifts the phases: sin * cos then, which does cross zero) */
  ctl[P_PITCH] = 0;
  ctl[P_MOD_INDEX] = 100;
  seq_set("route_fm", 0.0f);
  seq_set("route_am", 1.0f);
  seq_midi(0, 0xB0, 120, 0);                                            /* All Sound Off */
  run_seq();
  run_seq();
  seq_midi(0, 0x90, 69, 127);
  run_seq();
  cross = crossings(1.0);
  printf("ring at ratio 1: %d crossings in 1 s (440 without)\n", cross);
  CHECK(cross < 5);
  /* out of range values are clamped */
  seq_set("route_sync", 5.0f);
  run_seq();
  seq_get();
  run_seq();
  CHECK(reported("route_sync") == 1 && reported("route_am") == 1 && reported("route_fm") == 0);

  /* State: save, change, restore; the restored values are reported on the next run */
  nitems = 0;
  CHECK(si->save(h, store, NULL, 0, NULL) == LV2_STATE_SUCCESS);
  CHECK(nitems == 3);
  seq_set("route_am", 0.0f);
  seq_set("route_sync", 0.0f);
  seq_set("route_fm", 1.0f);
  run_seq();
  CHECK(si->restore(h, retrieve, NULL, 0, NULL) == LV2_STATE_SUCCESS);
  run_seq();
  CHECK(reported("route_sync") == 1 && reported("route_am") == 1 && reported("route_fm") == 0);
  /* a preset without these values (older version): defaults */
  nitems = 0;
  CHECK(si->restore(h, retrieve, NULL, 0, NULL) == LV2_STATE_SUCCESS);
  run_seq();
  CHECK(reported("route_fm") == 1 && reported("route_am") == 0 && reported("route_sync") == 0);
  /* FM on (the default again), Index 100 %: the sound is no longer a plain sine at 440 */
  for (int i = 0; i < 10; i++) run_seq();
  cross = crossings(1.0);
  printf("FM ratio 1, Index 100 %%: %d crossings in 1 s\n", cross);
  CHECK(cross > 600);

  /* odd values from a host: NaN knobs, a short MIDI event, out of range Mod knobs */
  ctl[P_ATTACK] = NAN;
  ctl[P_MOD_SHAPE] = 9; ctl[P_MOD_MODE] = -3; ctl[P_MOD_FREQ] = 1000; ctl[P_MOD_INDEX] = NAN;
  seq_midi(0, 0xE0, 0x7F, 0x7F);                                        /* bend fully up */
  run_seq();
  for (int b = 0; b < 20; b++) {
    run_seq();
    for (int i = 0; i < N; i++) CHECK(isfinite(out_l[i]));
  }
  seq_midi(0, 0xB0, 120, 0);                                            /* All Sound Off */
  run_seq();
  run_seq();
  CHECK(peak(out_l, 0, N) == 0.0f);
  CHECK(ctl[P_MOD_PHASE] == -1.0f);

  /* no notify port connected (a host may leave it): runs anyway */
  d->connect_port(h, P_NOTIFY, NULL);
  seq_get();
  run_seq();

  d->deactivate(h);
  d->cleanup(h);
  dlclose(lib);
  printf(fails ? "test_plugin: %d FAILED\n" : "test_plugin: all passed\n", fails);
  return fails ? 1 : 0;
}
