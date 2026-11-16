/* A tiny LV2 host for the built plugin: URID map, patch:Set / patch:Get on the control port, State save and
 * restore. No jalv needed. Checks the selected-stream knobs: a knob turn goes into the selected stream only.
 * Usage: test_plugin <path to eka_grainflow.so> */
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

static int fails = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static char* uris[256];
static int nuris = 0;
static LV2_URID map_uri(LV2_URID_Map_Handle h, const char* uri) {
  (void)h;
  for (int i = 0; i < nuris; i++) if (!strcmp(uris[i], uri)) return (LV2_URID)(i + 1);
  uris[nuris] = strdup(uri);
  return (LV2_URID)++nuris;
}
static LV2_URID_Map map = { NULL, map_uri };
#define U(s) map_uri(NULL, s)
#define P(name) U("http://dmtry3.github.io/zynthian-eka/grainflow#" name)

/* ports, as in eka_grainflow.c */
enum { STREAM = 0, MIX, FEEDBACK, FREEZE, OFFSET, RATE, LENGTH, PITCH, REVERSE, FILTER, SPRAY, LEVEL, SCAN, FINE, INTERVAL, SPREAD, NCTL,
       IN_L = NCTL, IN_R, OUT_L, OUT_R, CONTROL, NOTIFY, PEAK, ACTIVE, LOADS, MIN_OFFSET, SEL, ON_MASK, CUR0,
       LFO0 = CUR0 + 8, ENV = LFO0 + 2, BPM, RATE_DIV, OFFSET_DIV, GRAIN0,
       FREEZE_PHASE = GRAIN0 + 16, RECORD_PEAK, CUR_FLOW0, NPORTS = CUR_FLOW0 + 4 };
#define NFR 256
static float ctl[NCTL] = { 1, 100, 0, 0, 100, 100, 500, 12, 0, 0, 0, 0, 0, 0, 0, 0 };
static float in_l[NFR], in_r[NFR], out_l[NFR], out_r[NFR], mons[NPORTS - PEAK];
static union { LV2_Atom_Sequence s; uint8_t b[8192]; } ctl_in, notify;
static const LV2_Descriptor* D;
static LV2_Handle inst;
static double phase = 0;

/* patch:Set values the plugin reported in the last run(s), by parameter URID */
static float reported[256];
static int was_reported[256];

static void read_notify(void) {
  LV2_ATOM_SEQUENCE_FOREACH(&notify.s, ev) {
    const LV2_Atom_Object* o = (const LV2_Atom_Object*)&ev->body;
    if (o->body.otype != U(LV2_PATCH__Set)) continue;
    const LV2_Atom* prop = NULL; const LV2_Atom* val = NULL;
    lv2_atom_object_get(o, U(LV2_PATCH__property), &prop, U(LV2_PATCH__value), &val, 0);
    if (!prop || !val) continue;
    LV2_URID k = ((const LV2_Atom_URID*)prop)->body;
    reported[k] = val->type == U(LV2_ATOM__Float) ? ((const LV2_Atom_Float*)val)->body
                                                  : (float)((const LV2_Atom_Int*)val)->body;
    was_reported[k] = 1;
  }
}

static LV2_Atom_Forge forge;
static LV2_Atom_Forge_Frame seq_frame;      /* stays open while events are added */
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
    for (int i = 0; i < NFR; i++) { in_l[i] = in_r[i] = 0.5f * (float)sin(phase); phase += 2 * M_PI * 440 / 48000; }
    notify.s.atom.size = sizeof notify - sizeof(LV2_Atom);
    D->run(inst, NFR);
    read_notify();
    ctl_begin();                                     /* control events are delivered once */
  }
}

/* ---------- state ---------- */
typedef struct { uint32_t key, type; size_t size; union { float v; double dv; int64_t lv; }; } Item;
static Item items[256];
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

/* Read our generated factory Turtle, including its RDF Double/Long State values.
 * Exercise the actual preset file rather than reproducing Init's settings in this host. */
static int load_factory(const char* path, const char* id, const LV2_State_Interface* si) {
  static const char* names[] = { "stream", "mix", "feedback", "freeze", "offset", "rate",
                                "length", "pitch", "reverse", "filter", "spray", "level",
                                "scan", "fine", "interval", "spread" };
  FILE* f = fopen(path, "r");
  if (!f) return 0;
  char header[256], line[512];
  snprintf(header, sizeof header, "<http://dmtry3.github.io/zynthian-eka/grainflow#preset_%s>", id);
  int found = 0;
  nitems = 0;
  while (fgets(line, sizeof line, f)) {
    if (!found) { if (!strncmp(line, header, strlen(header))) found = 1; continue; }
    if (strstr(line, "    ] .")) break;
    char sym[96], number[64]; double value;
    if (sscanf(line, " [ lv2:symbol \"%95[^\"]\" ; pset:value %lf", sym, &value) == 2) {
      for (int i = 0; i < NCTL; i++) if (!strcmp(names[i], sym)) ctl[i] = (float)value;
    } else if (sscanf(line, " <http://dmtry3.github.io/zynthian-eka/grainflow#%95[^>]> %63s", sym, number) == 2) {
      char uri[256]; snprintf(uri, sizeof uri, "http://dmtry3.github.io/zynthian-eka/grainflow#%s", sym);
      Item* item = &items[nitems++];
      item->key = U(uri); item->size = 8;
      if (strchr(number, '.')) { item->type = U(LV2_ATOM__Double); item->dv = strtod(number, NULL); }
      else { item->type = U(LV2_ATOM__Long); item->lv = strtoll(number, NULL, 10); }
    }
  }
  fclose(f);
  if (found) si->restore(inst, retrieve, NULL, 0, NULL);
  return found;
}

int main(int argc, char** argv) {
  if (argc < 2) { printf("usage: test_plugin eka_grainflow.so\n"); return 2; }
  void* lib = dlopen(argv[1], RTLD_NOW);
  if (!lib) { printf("FAIL dlopen: %s\n", dlerror()); return 1; }
  LV2_Descriptor_Function df = (LV2_Descriptor_Function)dlsym(lib, "lv2_descriptor");
  D = df(0);
  CHECK(D && !strcmp(D->URI, "http://dmtry3.github.io/zynthian-eka/grainflow"));
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
  run_blocks(200);                                   /* ~1 s */
  float e = 0;
  for (int i = 0; i < NFR; i++) e += out_l[i] * out_l[i];
  CHECK(e > 1.0f);                                   /* stream 1 is on by default: grains sound */
  CHECK(mons[ACTIVE - PEAK] >= 1);
  CHECK(mons[PEAK - PEAK] > 0.4f && mons[PEAK - PEAK] < 0.6f);
  CHECK(mons[GRAIN0 - PEAK] >= 0.0f && mons[GRAIN0 - PEAK] < 1.0f);   /* stream 0, some seconds back */
  nitems = 0; si->save(inst, store, NULL, 0, NULL);
  CHECK(saved(P("s1_pitch")) == 12 && saved(P("s2_pitch")) == 0 && saved(P("s3_pitch")) == -12);

  /* a knob turn goes into stream 1 and is reported */
  ctl[PITCH] = 7;
  run_blocks(2);
  CHECK(was_reported[P("s1_pitch")] && reported[P("s1_pitch")] == 7);
  CHECK(fabsf(mons[MIN_OFFSET - PEAK] - (500 * (powf(2, 7 / 12.0f) - 1) + 3 / 48.0f)) < 1.0f);

  /* select stream 2: the knobs still show stream 1, nothing is copied */
  memset(was_reported, 0, sizeof was_reported);
  ctl[STREAM] = 2;
  run_blocks(2);
  CHECK(!was_reported[P("s2_pitch")] && !was_reported[P("s2_offset")]);
  /* the plugin shows stream 2's values for the widget to copy into the knobs */
  CHECK(mons[SEL - PEAK] == 2 && mons[CUR0 + 3 - PEAK] == 0 && mons[CUR0 + 0 - PEAK] == 250);
  CHECK(mons[ON_MASK - PEAK] == 1);
  /* the widget moves the knobs to stream 2 (pitch 0 by default); then a turn changes stream 2 only */
  ctl[OFFSET] = 250; ctl[RATE] = 120; ctl[LENGTH] = 400; ctl[PITCH] = 0; ctl[SPRAY] = 0; ctl[LEVEL] = -3;
  run_blocks(1);
  ctl[LENGTH] = 900;
  run_blocks(2);
  CHECK(was_reported[P("s2_length")] && reported[P("s2_length")] == 900);
  nitems = 0;
  si->save(inst, store, NULL, 0, NULL);
  CHECK(saved(P("s1_pitch")) == 7 && saved(P("s1_length")) == 500);
  CHECK(saved(P("s2_length")) == 900 && saved(P("s2_pitch")) == 0);
  CHECK(saved(P("s3_pitch")) == -12 && saved(P("s1_on")) == 1 && saved(P("s2_on")) == 0);
  CHECK(saved(P("buffer")) == 4 && saved(P("s1_fine")) == 0);
  CHECK(fabsf(saved(P("lfo1_rate")) - 0.3f) < 1e-6f && saved(P("lfo2_shape")) == 2 && saved(P("m2_s3_level")) == 0);
  CHECK(saved(P("lfo1_rate_mod")) == 0 && saved(P("lfo2_rate_mod")) == 0);
  CHECK(saved(P("freeze_amount")) == 0);
  CHECK(saved(P("s1_scan")) == 0 && saved(P("s3_scan")) == 0);

  /* the widget turns stream 3 on with patch:Set; out-of-range values are clamped */
  ctl_set(P("s3_on"), 1);
  ctl_set(P("s3_offset"), 99999);
  ctl_set(P("s2_fine"), -80);
  ctl_set(P("m1_s1_pitch"), 25);
  ctl_set(P("lfo1_rate"), 40);
  ctl_set(P("lfo1_rate_mod"), 150);
  ctl_set(P("lfo2_rate_mod"), -65);
  ctl_set(P("lfo2_mode"), 1);
  ctl_set(P("shape"), 0.37f);
  ctl_set(P("lfo1_sync"), 1);
  ctl_set(P("lfo1_div"), 17);
  ctl_set(P("lfo2_div"), 50);
  ctl_set(P("lfo1_smooth"), 75);
  ctl_set(P("lfo2_smooth"), 200);
  ctl_set(P("m1_s2_filter"), -65);
  ctl_set(P("m3_s3_scan"), 135);
  ctl_set(P("env_attack"), 150);
  ctl_set(P("env_release"), 1200);
  ctl_set(P("env_sensitivity"), 8);
  ctl_set(P("s1_scan"), 500); ctl_set(P("s2_scan"), -25);
  run_blocks(1);
  nitems = 0;
  si->save(inst, store, NULL, 0, NULL);
  CHECK(saved(P("s3_on")) == 1 && saved(P("s3_offset")) == 8000 && saved(P("s2_fine")) == -50);
  CHECK(saved(P("m1_s1_pitch")) == 25 && saved(P("lfo1_rate")) == 20);
  CHECK(saved(P("lfo1_rate_mod")) == 100 && saved(P("lfo2_rate_mod")) == -65);
  CHECK(saved(P("s1_scan")) == 200 && saved(P("s2_scan")) == -25);
  CHECK(saved(P("lfo1_mode")) == 0 && saved(P("lfo2_mode")) == 1);
  CHECK(fabsf(saved(P("shape")) - 0.37f) < 1e-6f);
  CHECK(saved(P("m1_s2_filter")) == -65 && saved(P("m3_s3_scan")) == 100);
  CHECK(saved(P("lfo1_sync")) == 1 && saved(P("lfo2_sync")) == 0 && saved(P("lfo1_div")) == 17 && saved(P("lfo2_div")) == 19);
  CHECK(saved(P("lfo1_smooth")) == 75 && saved(P("lfo2_smooth")) == 100);
  CHECK(saved(P("env_attack")) == 150 && saved(P("env_release")) == 1200 && saved(P("env_sensitivity")) == 8);
  CHECK(mons[LFO0 - PEAK] >= -1.0f && mons[LFO0 - PEAK] <= 1.0f);
  CHECK(saved(P("s2_spread")) == 50 && saved(P("s1_interval")) == 0 && saved(P("sync")) == 0 && saved(P("m3_s3_level")) == 0);
  CHECK(mons[ENV - PEAK] > 0.5f && mons[BPM - PEAK] == 120 && mons[RATE_DIV - PEAK] == -1);

  /* host tempo and Sync: the selected stream's Rate shows as a note value */
  {
    LV2_Atom_Forge_Frame f;
    ctl_begin();
    ctl_set(P("sync"), 1);
    ctl_set(P("s2_interval"), 4);
    lv2_atom_forge_frame_time(&forge, 0);
    lv2_atom_forge_object(&forge, &f, 0, U(LV2_TIME__Position));
    lv2_atom_forge_key(&forge, U(LV2_TIME__beatsPerMinute));
    lv2_atom_forge_float(&forge, 90.0f);
    lv2_atom_forge_key(&forge, U(LV2_TIME__speed));
    lv2_atom_forge_float(&forge, 0.0f);
    lv2_atom_forge_pop(&forge, &f);
    run_blocks(1);
  }
  CHECK(mons[BPM - PEAK] == 90);
  /* stream 2 is selected, Rate 120 ms at 90 BPM = 0.18 beats: nearest 3/16 (1/32.) = index 5 */
  CHECK(mons[RATE_DIV - PEAK] == 5);
  nitems = 0;
  si->save(inst, store, NULL, 0, NULL);
  CHECK(saved(P("sync")) == 1 && saved(P("s2_interval")) == 4);
  ctl_set(P("sync"), 0);
  run_blocks(1);

  /* patch:Get: all parameters are reported (a few per run) */
  memset(was_reported, 0, sizeof was_reported);
  ctl_begin(); ctl_get();
  run_blocks(10);
  CHECK(was_reported[P("s1_offset")] && was_reported[P("shape")] && reported[P("s2_length")] == 900);

  /* restore: values come back, "loads" counts up, the knobs (still stream 2's) do not overwrite anything */
  float loads = mons[LOADS - PEAK];
  for (int i = 0; i < nitems; i++) if (items[i].key == P("s2_length")) items[i].v = 333;
  ctl[LENGTH] = 777;                                 /* a stale knob value */
  si->restore(inst, retrieve, NULL, 0, NULL);
  run_blocks(2);
  CHECK(mons[LOADS - PEAK] == loads + 1);
  nitems = 0;
  si->save(inst, store, NULL, 0, NULL);
  CHECK(saved(P("s2_length")) == 333);
  CHECK(saved(P("s1_scan")) == 200 && saved(P("s2_scan")) == -25);
  CHECK(saved(P("lfo1_rate_mod")) == 100 && saved(P("lfo2_rate_mod")) == -65);

  /* Older snapshots have no rate links or Scan: missing parameters must return to off. */
  for (int i = 0; i < nitems; ) {
    if (items[i].key == P("lfo1_rate_mod") || items[i].key == P("lfo2_rate_mod") ||
        items[i].key == P("s1_scan") || items[i].key == P("s2_scan") || items[i].key == P("s3_scan"))
      items[i] = items[--nitems];
    else i++;
  }
  si->restore(inst, retrieve, NULL, 0, NULL);
  run_blocks(2);
  nitems = 0;
  si->save(inst, store, NULL, 0, NULL);
  CHECK(saved(P("lfo1_rate_mod")) == 0 && saved(P("lfo2_rate_mod")) == 0);
  CHECK(saved(P("s1_scan")) == 0 && saved(P("s2_scan")) == 0 && saved(P("s3_scan")) == 0);

  /* Freeze, Mix 0, silence: output = input */
  ctl[MIX] = 0; ctl[FREEZE] = 1;
  run_blocks(3);
  float worst = 0;
  for (int i = 0; i < NFR; i++) worst = fmaxf(worst, fabsf(out_l[i] - in_l[i]));
  CHECK(worst < 1e-5f);

  /* Partial capture parameters survive State and activate a moving tape instead of the stopped head. */
  ctl[FREEZE] = 0; run_blocks(1);
  ctl_set(P("freeze_amount"), 40);
  run_blocks(10);
  CHECK(mons[FREEZE_PHASE - PEAK] >= 0 && mons[FREEZE_PHASE - PEAK] < 1);
  nitems = 0; si->save(inst, store, NULL, 0, NULL);
  CHECK(saved(P("freeze_amount")) == 40);
  si->restore(inst, retrieve, NULL, 0, NULL);
  run_blocks(1);
  nitems = 0; si->save(inst, store, NULL, 0, NULL);
  CHECK(saved(P("freeze_amount")) == 40);

  if (argc > 2) {
    static const char* previous[] = { "tidal_memory", "breathing_light", "velvet_drift", "clockwork_glass", "slow_motion", "living_cloud", "freeze_pad", "pulse", "sparks", "afterglow" };
    for (unsigned k = 0; k < sizeof previous / sizeof *previous; k++) {
      CHECK(load_factory(argv[2], previous[k], si));
      run_blocks(200);
      /* A capture with no enabled streams must not keep Init silent. */
      ctl_set(P("s1_on"), 0); ctl_set(P("s2_on"), 0); ctl_set(P("s3_on"), 0);
      ctl[FREEZE] = 1; run_blocks(200);
      CHECK(load_factory(argv[2], "init", si));
      run_blocks(200);
      CHECK(mons[ON_MASK - PEAK] == 2 && mons[ACTIVE - PEAK] >= 1);
      CHECK(mons[SEL - PEAK] == 1 && mons[CUR0 - PEAK] == 100 && mons[CUR0 + 2 - PEAK] == 500);
      CHECK(mons[CUR0 + 3 - PEAK] == 12);
      nitems = 0; si->save(inst, store, NULL, 0, NULL);
      CHECK(saved(P("s1_scan")) == 0 && saved(P("s2_scan")) == 0 && saved(P("s3_scan")) == 0);
      float wet = 0;
      for (int i = 0; i < NFR; i++) {
        float v = out_l[i] - in_l[i] * sqrtf(0.5f);
        wet += v * v;
      }
      CHECK(wet > 0.1f);
    }
  }

  if (argc > 2) {
    CHECK(load_factory(argv[2], "velvet_drift", si)); run_blocks(2);
    nitems = 0; si->save(inst, store, NULL, 0, NULL);
    CHECK(saved(P("lfo1_smooth")) == 85 && saved(P("env_release")) == 900 && saved(P("env_sensitivity")) == 6);
    CHECK(load_factory(argv[2], "clockwork_glass", si)); run_blocks(2);
    nitems = 0; si->save(inst, store, NULL, 0, NULL);
    CHECK(saved(P("lfo1_sync")) == 1 && saved(P("lfo1_div")) == 9 && saved(P("lfo2_div")) == 17);
    CHECK(fabsf(saved(P("shape")) - 0.65f) < 1e-6f && saved(P("lfo2_mode")) == 1);
  }

  /* Flow encoder ports route to the selected stream, including host-driven monitor synchronization. */
  if (argc > 2) {
    CHECK(load_factory(argv[2], "init", si));
    run_blocks(2);
    ctl[SCAN] = 30; ctl[FINE] = 11; ctl[INTERVAL] = 4; ctl[SPREAD] = 60;
    run_blocks(2);
    nitems = 0; si->save(inst, store, NULL, 0, NULL);
    CHECK(saved(P("s1_scan")) == 30 && saved(P("s1_fine")) == 11 &&
          saved(P("s1_interval")) == 4 && saved(P("s1_spread")) == 60);
    CHECK(mons[CUR_FLOW0 - PEAK] == 30 && mons[CUR_FLOW0 + 1 - PEAK] == 11 &&
          mons[CUR_FLOW0 + 2 - PEAK] == 4 && mons[CUR_FLOW0 + 3 - PEAK] == 60);
    ctl[STREAM] = 2; run_blocks(2);
    CHECK(mons[CUR_FLOW0 - PEAK] == 0 && mons[CUR_FLOW0 + 1 - PEAK] == 0 &&
          mons[CUR_FLOW0 + 2 - PEAK] == 0 && mons[CUR_FLOW0 + 3 - PEAK] == 50);
    for (int i = 0; i < 8; i++) ctl[OFFSET + i] = mons[CUR0 + i - PEAK];
    for (int i = 0; i < 4; i++) ctl[SCAN + i] = mons[CUR_FLOW0 + i - PEAK];
    run_blocks(1);
    ctl[SCAN] = -20; ctl[FINE] = -7; ctl[INTERVAL] = 5; ctl[SPREAD] = 95;
    run_blocks(2);
    ctl[STREAM] = 1; run_blocks(2);  /* stale stream-2 encoders must not overwrite stream 1 */
    nitems = 0; si->save(inst, store, NULL, 0, NULL);
    CHECK(saved(P("s1_scan")) == 30 && saved(P("s1_fine")) == 11 &&
          saved(P("s1_interval")) == 4 && saved(P("s1_spread")) == 60);
    CHECK(saved(P("s2_scan")) == -20 && saved(P("s2_fine")) == -7 &&
          saved(P("s2_interval")) == 5 && saved(P("s2_spread")) == 95);
    CHECK(load_factory(argv[2], "slow_motion", si));
    run_blocks(2);
    CHECK(mons[CUR_FLOW0 - PEAK] == 25 && mons[CUR_FLOW0 + 1 - PEAK] == 0 &&
          mons[CUR_FLOW0 + 2 - PEAK] == 0 && mons[CUR_FLOW0 + 3 - PEAK] == 70);
    ctl[INTERVAL] = 99; run_blocks(2);
    nitems = 0; si->save(inst, store, NULL, 0, NULL);
    CHECK(saved(P("s1_interval")) == 13 && mons[CUR_FLOW0 + 2 - PEAK] == 13);
    CHECK(load_factory(argv[2], "stepped_sky", si));
    run_blocks(2);
    CHECK(mons[CUR_FLOW0 + 2 - PEAK] == 9);
  }

  D->deactivate ? D->deactivate(inst) : (void)0;
  D->cleanup(inst);
  printf("%s: %d checks, %d failed (plugin)\n", fails ? "FAIL" : "OK", checks, fails);
  return fails ? 1 : 0;
}
