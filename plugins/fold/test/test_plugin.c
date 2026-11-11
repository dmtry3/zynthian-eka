/* A tiny LV2 host for the built plugin: URID map, MIDI in an atom sequence, the output ports. No jalv needed.
 * Usage: test_plugin <path to eka_fold.so> */
#include <dlfcn.h>
#include <math.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/core/lv2.h>
#include <lv2/midi/midi.h>
#include <lv2/urid/urid.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

#define FOLD_URI "http://dmtry3.github.io/zynthian-eka/fold"
#define N 256
/* ports, as in lv2/eka_fold.ttl */
enum { P_PITCH, P_FINE, P_ATTACK, P_DECAY, P_SUSTAIN, P_RELEASE, P_MIDI_IN, P_OUT_L, P_OUT_R,
       P_NOTE1, P_NOTE4 = P_NOTE1 + 3, P_PEAK, P_COUNT };

static char* uris[64];
static int nuris = 0;
static LV2_URID map_uri(LV2_URID_Map_Handle h, const char* uri) {
  (void)h;
  for (int i = 0; i < nuris; i++) if (!strcmp(uris[i], uri)) return (LV2_URID)(i + 1);
  uris[nuris] = strdup(uri);
  return (LV2_URID)++nuris;
}
static LV2_URID_Map map = { NULL, map_uri };

/* ---------- MIDI input: an atom sequence the plugin reads ---------- */
static uint64_t seq_buf[1024];
static LV2_Atom_Sequence* seq = (LV2_Atom_Sequence*)seq_buf;

static void seq_clear(void) {
  seq->atom.type = map_uri(NULL, LV2_ATOM__Sequence);
  seq->atom.size = sizeof(LV2_Atom_Sequence_Body);
  seq->body.unit = 0;
  seq->body.pad = 0;
}

static void seq_midi(int64_t frame, uint8_t a, uint8_t b, uint8_t c) {
  LV2_Atom_Event* ev = (LV2_Atom_Event*)((uint8_t*)&seq->body + seq->atom.size);
  ev->time.frames = frame;
  ev->body.type = map_uri(NULL, LV2_MIDI__MidiEvent);
  ev->body.size = 3;
  uint8_t* d = (uint8_t*)(ev + 1);
  d[0] = a; d[1] = b; d[2] = c;
  seq->atom.size += lv2_atom_pad_size(sizeof(LV2_Atom_Event) + 3);
}

static float ctl[P_COUNT];
static float out_l[N], out_r[N];

static float peak(const float* x, int a, int b) {
  float p = 0;
  for (int i = a; i < b; i++) p = fabsf(x[i]) > p ? fabsf(x[i]) : p;
  return p;
}

int main(int argc, char** argv) {
  if (argc < 2) { printf("usage: test_plugin eka_fold.so\n"); return 2; }
  void* lib = dlopen(argv[1], RTLD_NOW);
  if (!lib) { printf("FAIL dlopen: %s\n", dlerror()); return 1; }
  LV2_Descriptor_Function df = (LV2_Descriptor_Function)dlsym(lib, "lv2_descriptor");
  CHECK(df != NULL);
  const LV2_Descriptor* d = df(0);
  CHECK(d && !strcmp(d->URI, FOLD_URI));
  CHECK(df(1) == NULL);

  const LV2_Feature none = { "urn:none", NULL };
  const LV2_Feature* no_map[] = { &none, NULL };
  CHECK(d->instantiate(d, 48000, "", no_map) == NULL);                   /* urid:map is required */

  const LV2_Feature map_f = { LV2_URID__map, &map };
  const LV2_Feature* feats[] = { &map_f, NULL };
  LV2_Handle h = d->instantiate(d, 48000, "", feats);
  CHECK(h != NULL);
  if (!h) return 1;
  ctl[P_PITCH] = 0; ctl[P_FINE] = 0; ctl[P_ATTACK] = 5; ctl[P_DECAY] = 300; ctl[P_SUSTAIN] = 70; ctl[P_RELEASE] = 400;
  for (int p = 0; p < P_COUNT; p++) {
    if (p == P_MIDI_IN) d->connect_port(h, p, seq);
    else if (p == P_OUT_L) d->connect_port(h, p, out_l);
    else if (p == P_OUT_R) d->connect_port(h, p, out_r);
    else d->connect_port(h, p, &ctl[p]);
  }
  d->activate(h);

  /* silence without notes */
  seq_clear();
  d->run(h, N);
  CHECK(peak(out_l, 0, N) == 0.0f);
  CHECK(ctl[P_PEAK] == -100.0f);
  CHECK(ctl[P_NOTE1] == -1.0f && ctl[P_NOTE4] == -1.0f);

  /* a note on at frame 100: sample-accurate */
  seq_clear();
  seq_midi(100, 0x90, 60, 127);
  d->run(h, N);
  CHECK(peak(out_l, 0, 101) == 0.0f);
  CHECK(peak(out_l, 101, N) > 0.0f);
  CHECK(!memcmp(out_l, out_r, sizeof out_l));
  CHECK(ctl[P_NOTE1] == 60.0f);
  CHECK(ctl[P_PEAK] > -100.0f && ctl[P_PEAK] <= 0.0f);

  /* a chord on another MIDI channel, then 2 s: peak at sustain, 4 notes shown */
  seq_clear();
  seq_midi(0, 0x93, 64, 127);
  seq_midi(0, 0x93, 67, 127);
  seq_midi(0, 0x93, 72, 127);
  d->run(h, N);
  int shown = 0;
  for (int i = 0; i < 4; i++) shown += ctl[P_NOTE1 + i] >= 0.0f;
  CHECK(shown == 4);
  seq_clear();
  for (int i = 0; i < 400; i++) d->run(h, N);
  CHECK(ctl[P_PEAK] < 0.0f);

  /* the Pitch knob: an octave up, the frequency doubles (zero crossings over 256 samples of a single note) */
  seq_clear();
  seq_midi(0, 0xB0, 123, 0);                                            /* All Notes Off */
  d->run(h, N);
  for (int i = 0; i < 200; i++) d->run(h, N);
  CHECK(ctl[P_NOTE1] == -1.0f);
  ctl[P_PITCH] = 12;
  seq_clear();
  seq_midi(0, 0x90, 69, 127);
  d->run(h, N);
  seq_clear();
  int cross = 0;
  float prev = 0.0f;                                                     /* across block boundaries too */
  for (int b = 0; b < 375; b++) {                                        /* 2 s */
    d->run(h, N);
    for (int i = 0; i < N; i++) { cross += prev < 0.0f && out_l[i] >= 0.0f; prev = out_l[i]; }
  }
  printf("A4 + 12: %d crossings in 2 s\n", cross);
  CHECK(cross >= 1758 && cross <= 1762);

  /* odd values from a host: NaN knobs, a short MIDI event */
  ctl[P_ATTACK] = NAN;
  ctl[P_PITCH] = 0;
  seq_clear();
  seq_midi(0, 0xE0, 0x7F, 0x7F);                                        /* bend fully up */
  d->run(h, N);
  for (int i = 0; i < N; i++) CHECK(isfinite(out_l[i]));
  seq_clear();
  seq_midi(0, 0xB0, 120, 0);                                            /* All Sound Off */
  d->run(h, N);
  d->run(h, N);
  CHECK(peak(out_l, 0, N) == 0.0f);

  d->deactivate(h);
  d->cleanup(h);
  dlclose(lib);
  printf(fails ? "test_plugin: %d FAILED\n" : "test_plugin: all passed\n", fails);
  return fails ? 1 : 0;
}
