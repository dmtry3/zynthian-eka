/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Minimal LV2 host test for eka_pulse.so: ports, patch:Set/Get, State save/restore, slot knob routing. */
#define _POSIX_C_SOURCE 200809L
#include <dlfcn.h>
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

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int fails = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static char* uris[512];
static int nuris = 0;
static LV2_URID map_uri(LV2_URID_Map_Handle h, const char* uri) {
  (void)h;
  for (int i = 0; i < nuris; i++) if (!strcmp(uris[i], uri)) return (LV2_URID)(i + 1);
  uris[nuris] = strdup(uri);
  return (LV2_URID)++nuris;
}
static LV2_URID_Map map = { NULL, map_uri };
#define U(s) map_uri(NULL, s)
#define P(name) U("http://dmtry3.github.io/zynthian-eka/pulse#" name)

enum {
  SLOT = 0, MIX, LOW_SPLIT, SCENE,
  MODE, PARAM1, PARAM2, DEPTH,
  PATTERN, STEP_RATE, STEPS, WARP,
  TRIG, THRESHOLD, ENV_DEPTH, SMOOTH,
  LIVE_PAD, PREV_SCENE, NEXT_SCENE, OUTPUT_DB,
  NCTL,
  IN_L = NCTL, IN_R, OUT_L, OUT_R, CONTROL, NOTIFY,
  PEAK, OUT_PEAK, LIM_GR, LOADS, BPM, SEL, ON_MASK,
  PHASE0, VAL0 = PHASE0 + 4, STEPS0 = VAL0 + 4, MODE0 = STEPS0 + 4,
  CUR0 = MODE0 + 4, NPORTS = CUR0 + 12
};

#define NFR 256
static float ctl[NCTL] = {
  4, 100, 0, 0,
  0, 0, 50, 100,
  2, 2, 8, 0,
  0, -24, 0, 10,
  0, 0, 0, 0
};
static float in_l[NFR], in_r[NFR], out_l[NFR], out_r[NFR], mons[NPORTS - PEAK];
static union { LV2_Atom_Sequence s; uint8_t b[16384]; } ctl_in, notify;
static const LV2_Descriptor* D;
static LV2_Handle inst;
static double phase = 0.0;

static float reported[512];
static int was_reported[512];

static void read_notify(void) {
  LV2_ATOM_SEQUENCE_FOREACH(&notify.s, ev) {
    const LV2_Atom_Object* o = (const LV2_Atom_Object*)&ev->body;
    if (o->body.otype != U(LV2_PATCH__Set)) continue;
    const LV2_Atom* prop = NULL; const LV2_Atom* val = NULL;
    lv2_atom_object_get(o, U(LV2_PATCH__property), &prop, U(LV2_PATCH__value), &val, 0);
    if (!prop || !val) continue;
    LV2_URID k = ((const LV2_Atom_URID*)prop)->body;
    if (k < 512) {
      reported[k] = (val->type == U(LV2_ATOM__Float)) ? ((const LV2_Atom_Float*)val)->body
                                                      : (float)((const LV2_Atom_Int*)val)->body;
      was_reported[k] = 1;
    }
  }
}

static LV2_Atom_Forge forge;
static LV2_Atom_Forge_Frame seq_frame;
static void ctl_begin(void) {
  lv2_atom_forge_set_buffer(&forge, ctl_in.b, sizeof ctl_in);
  lv2_atom_forge_sequence_head(&forge, &seq_frame, 0);
}
static void ctl_set(LV2_URID key, float v) {
  LV2_Atom_Forge_Frame f;
  lv2_atom_forge_frame_time(&forge, 0);
  lv2_atom_forge_object(&forge, &f, 0, U(LV2_PATCH__Set));
  lv2_atom_forge_key(&forge, U(LV2_PATCH__property));
  lv2_atom_forge_urid(&forge, key);
  lv2_atom_forge_key(&forge, U(LV2_PATCH__value));
  lv2_atom_forge_float(&forge, v);
  lv2_atom_forge_pop(&forge, &f);
}
static void ctl_get(void) {
  LV2_Atom_Forge_Frame f;
  lv2_atom_forge_frame_time(&forge, 0);
  lv2_atom_forge_object(&forge, &f, 0, U(LV2_PATCH__Get));
  lv2_atom_forge_pop(&forge, &f);
}

static void run_blocks(int n) {
  for (int b = 0; b < n; b++) {
    for (int i = 0; i < NFR; i++) {
      in_l[i] = in_r[i] = 0.5f * (float)sin(phase);
      phase += 2.0 * M_PI * 440.0 / 48000.0;
    }
    notify.s.atom.size = sizeof notify - sizeof(LV2_Atom);
    D->run(inst, NFR);
    read_notify();
    ctl_begin();
  }
}

typedef struct { uint32_t key, type; size_t size; union { float v; double dv; int64_t lv; }; } Item;
static Item items[512];
static int nitems = 0;
static LV2_State_Status store(LV2_State_Handle h, uint32_t key, const void* v, size_t size, uint32_t type,
                              uint32_t flags) {
  (void)h; (void)flags;
  items[nitems].key = key; items[nitems].type = type; items[nitems].size = size;
  memcpy(&items[nitems].v, v, size < sizeof(double) ? size : sizeof(double));
  nitems++;
  return LV2_STATE_SUCCESS;
}
static const void* retrieve(LV2_State_Handle h, uint32_t key, size_t* size, uint32_t* type, uint32_t* flags) {
  (void)h;
  for (int i = 0; i < nitems; i++)
    if (items[i].key == key) { *size = items[i].size; *type = items[i].type; *flags = 0; return &items[i].v; }
  return NULL;
}
static float saved(LV2_URID key) {
  for (int i = 0; i < nitems; i++)
    if (items[i].key == key) {
      if (items[i].type == U(LV2_ATOM__Int)) { int32_t v; memcpy(&v, &items[i].v, 4); return (float)v; }
      return items[i].v;
    }
  return NAN;
}

int main(int argc, char** argv) {
  if (argc < 2) { printf("usage: test_plugin eka_pulse.so\n"); return 2; }
  void* lib = dlopen(argv[1], RTLD_NOW);
  if (!lib) { printf("FAIL dlopen: %s\n", dlerror()); return 1; }
  LV2_Descriptor_Function df = (LV2_Descriptor_Function)dlsym(lib, "lv2_descriptor");
  D = df(0);
  CHECK(D && !strcmp(D->URI, "http://dmtry3.github.io/zynthian-eka/pulse"));
  CHECK(df(1) == NULL);

  LV2_Feature fmap = { LV2_URID__map, &map };
  const LV2_Feature* feats[] = { &fmap, NULL };
  inst = D->instantiate(D, 48000, "", feats);
  CHECK(inst != NULL);
  lv2_atom_forge_init(&forge, &map);
  for (int i = 0; i < NCTL; i++) D->connect_port(inst, i, &ctl[i]);
  D->connect_port(inst, IN_L, in_l); D->connect_port(inst, IN_R, in_r);
  D->connect_port(inst, OUT_L, out_l); D->connect_port(inst, OUT_R, out_r);
  D->connect_port(inst, CONTROL, &ctl_in); D->connect_port(inst, NOTIFY, &notify);
  for (int i = PEAK; i < NPORTS; i++) D->connect_port(inst, i, &mons[i - PEAK]);
  D->activate(inst);

  const LV2_State_Interface* si = (const LV2_State_Interface*)D->extension_data(LV2_STATE__interface);
  CHECK(si != NULL);

  ctl_begin();
  run_blocks(50);
  CHECK(mons[SEL - PEAK] == 4);
  CHECK(mons[ON_MASK - PEAK] == 8); /* slot 4 (Gate) on by default */
  CHECK(mons[PEAK - PEAK] > 0.4f && mons[PEAK - PEAK] < 0.6f);
  CHECK(mons[STEPS0 + 0 - PEAK] == 16 && mons[STEPS0 + 1 - PEAK] == 5 &&
        mons[STEPS0 + 2 - PEAK] == 7 && mons[STEPS0 + 3 - PEAK] == 8);

  /* Turning Steps knob edits slot 4 only */
  memset(was_reported, 0, sizeof was_reported);
  ctl[STEPS] = 12;
  run_blocks(2);
  CHECK(was_reported[P("s4_steps")] && reported[P("s4_steps")] == 12);
  CHECK(mons[STEPS0 + 3 - PEAK] == 12);

  /* Switching to slot 3 does NOT overwrite slot 3 with stale slot-4 knobs */
  memset(was_reported, 0, sizeof was_reported);
  ctl[SLOT] = 3;
  run_blocks(2);
  CHECK(mons[SEL - PEAK] == 3);
  CHECK(mons[STEPS0 + 2 - PEAK] == 7);
  CHECK(mons[CUR0 + 6 - PEAK] == 7); /* cur_steps == 7 */
  CHECK(!was_reported[P("s3_steps")]);

  /* Editing a step level via patch:Set switches pattern to Custom (0) */
  ctl_set(P("s4_lvl2"), 42.0f);
  run_blocks(2);
  nitems = 0; si->save(inst, store, NULL, 0, NULL);
  CHECK(saved(P("s4_lvl2")) == 42.0f);
  CHECK(saved(P("s4_pattern")) == 0.0f);
  CHECK(saved(P("s4_steps")) == 12.0f);
  CHECK(saved(P("s3_steps")) == 7.0f);

  /* Selecting a built-in pattern updates step levels and reports them */
  memset(was_reported, 0, sizeof was_reported);
  ctl_set(P("s4_pattern"), 1); /* Sidechain */
  run_blocks(4);
  CHECK(was_reported[P("s4_lvl2")] && reported[P("s4_lvl2")] == 100.0f);
  CHECK(was_reported[P("s4_shp1")] && reported[P("s4_shp1")] == 2.0f);

  /* Host tempo via time:Position */
  {
    LV2_Atom_Forge_Frame f;
    ctl_begin();
    lv2_atom_forge_frame_time(&forge, 0);
    lv2_atom_forge_object(&forge, &f, 0, U(LV2_TIME__Position));
    lv2_atom_forge_key(&forge, U(LV2_TIME__beatsPerMinute));
    lv2_atom_forge_float(&forge, 135.0f);
    lv2_atom_forge_key(&forge, U(LV2_TIME__speed));
    lv2_atom_forge_float(&forge, 1.0f);
    lv2_atom_forge_key(&forge, U(LV2_TIME__bar));
    lv2_atom_forge_int(&forge, 0);
    lv2_atom_forge_key(&forge, U(LV2_TIME__barBeat));
    lv2_atom_forge_float(&forge, 0.0f);
    lv2_atom_forge_pop(&forge, &f);
    run_blocks(1);
  }
  CHECK(mons[BPM - PEAK] == 135.0f);

  /* patch:Get reports all parameters over several runs */
  memset(was_reported, 0, sizeof was_reported);
  ctl_begin(); ctl_get();
  run_blocks(20);
  CHECK(was_reported[P("s1_on")] && was_reported[P("s4_lvl16")] && was_reported[P("free_hz")]);

  /* State restore increments loads and restores saved values without stale knob overwrite */
  float loads = mons[LOADS - PEAK];
  for (int i = 0; i < nitems; i++) if (items[i].key == P("s3_steps")) items[i].v = 11.0f;
  /* Store as int32 since s3_steps is atom:Int */
  for (int i = 0; i < nitems; i++) if (items[i].key == P("s3_steps")) { int32_t v = 11; memcpy(&items[i].v, &v, 4); }
  ctl[STEPS] = 3; /* stale */
  si->restore(inst, retrieve, NULL, 0, NULL);
  run_blocks(2);
  CHECK(mons[LOADS - PEAK] == loads + 1);
  CHECK(mons[STEPS0 + 2 - PEAK] == 11);

  if (D->deactivate) D->deactivate(inst);
  D->cleanup(inst);
  dlclose(lib);
  for (int i = 0; i < nuris; i++) free(uris[i]);
  printf("%s: %d checks, %d failed (plugin)\n", fails ? "FAIL" : "OK", checks, fails);
  return fails ? 1 : 0;
}
