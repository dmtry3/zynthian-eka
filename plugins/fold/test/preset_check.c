/* Plays every factory preset through the built plugin, the way Zynthian loads one: the control ports from the
 * preset, the LV2 parameters through State restore. Poly presets play a chord (held 1.5 s, then let go), Mono and
 * Legato ones a phrase of overlapping notes. Prints peak and loudness, fails on silence, NaN or a clipping peak,
 * and writes <out dir>/<preset id>.wav to listen to.
 * Usage: preset_check <eka_fold.so> <presets.ttl> [out dir]
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <dlfcn.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/forge.h>
#include <lv2/core/lv2.h>
#include <lv2/midi/midi.h>
#include <lv2/state/state.h>
#include <lv2/urid/urid.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SR 48000
#define NFR 256
#define NCTL 28                   /* control input ports, then midi_in, out_l, out_r, notify, 14 outputs */
#define NPORTS (NCTL + 4 + 14)
#define SECS 5
#define URI "http://dmtry3.github.io/zynthian-eka/fold"

static int fails = 0;

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

/* ---------- presets.ttl, as gen_presets.py writes it ---------- */
typedef struct {
  char id[64], label[96];
  float ctl[NCTL];
  int nctl;
  char ikey[128][48];
  int ival[128];
  int nint;
} Preset;
static Preset presets[64];
static int npresets = 0;

static void parse(const char* path) {
  FILE* f = fopen(path, "r");
  if (!f) { printf("FAIL: cannot read %s\n", path); exit(1); }
  char line[512], a[256], b[256];
  Preset* p = NULL;
  while (fgets(line, sizeof line, f)) {
    if (sscanf(line, "<" URI "#preset_%63[^>]>", a) == 1) {
      if (npresets == 64) break;
      p = &presets[npresets++];
      memset(p, 0, sizeof *p);
      snprintf(p->id, sizeof p->id, "%s", a);
    } else if (!p) {
      continue;
    } else if (sscanf(line, " rdfs:label \"%95[^\"]\"", a) == 1) {
      snprintf(p->label, sizeof p->label, "%s", a);
    } else if (sscanf(line, " [ lv2:symbol \"%63[^\"]\" ; pset:value %255[^ ] ]", a, b) == 2 && p->nctl < NCTL) {
      p->ctl[p->nctl++] = strtof(b, NULL);           /* in the ports' index order */
    } else if (sscanf(line, " <" URI "#%47[^>]> %255s", a, b) == 2 && p->nint < 128) {
      snprintf(p->ikey[p->nint], sizeof p->ikey[0], "%s", a);
      p->ival[p->nint++] = atoi(b);
    }
  }
  fclose(f);
}

static const Preset* cur;
static int32_t ivals[128];
static const void* retrieve(LV2_State_Handle h, uint32_t key, size_t* size, uint32_t* type, uint32_t* fl) {
  (void)h;
  char uri[160];
  for (int i = 0; i < cur->nint; i++) {
    snprintf(uri, sizeof uri, URI "#%s", cur->ikey[i]);
    if (U(uri) == key) {
      ivals[i] = cur->ival[i];
      *size = sizeof(int32_t);
      *type = U(LV2_ATOM__Int);
      *fl = LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE;
      return &ivals[i];
    }
  }
  return NULL;
}

/* ---------- the host ---------- */
static float ports[NPORTS], out_l[NFR], out_r[NFR];
static union { LV2_Atom_Sequence seq; uint8_t raw[8192]; } in_buf, notify_buf;
static LV2_Atom_Forge forge;
static LV2_Atom_Forge_Frame in_frame;
static const LV2_Descriptor* d;
static LV2_Handle inst;

static void in_begin(void) {
  lv2_atom_forge_set_buffer(&forge, in_buf.raw, sizeof in_buf);
  lv2_atom_forge_sequence_head(&forge, &in_frame, 0);
}

static void in_midi(uint8_t st, uint8_t d1, uint8_t d2) {
  const uint8_t m[3] = { st, d1, d2 };
  lv2_atom_forge_frame_time(&forge, 0);
  lv2_atom_forge_atom(&forge, 3, U(LV2_MIDI__MidiEvent));
  lv2_atom_forge_write(&forge, m, 3);
}

static void run_once(void) {
  lv2_atom_forge_pop(&forge, &in_frame);
  notify_buf.seq.atom.type = 0;
  notify_buf.seq.atom.size = sizeof notify_buf - sizeof(LV2_Atom);
  d->run(inst, NFR);
  in_begin();
}

static void write_wav(const char* path, const float* x, uint32_t n) {
  FILE* f = fopen(path, "wb");
  if (!f) return;
  const uint32_t bytes = n * 2, sr = SR, br = SR * 2, riff = 36 + bytes, fmt_len = 16;
  const uint16_t fmt = 1, ch = 1, align = 2, bits = 16;
  fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
  fwrite(&fmt_len, 4, 1, f); fwrite(&fmt, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&sr, 4, 1, f);
  fwrite(&br, 4, 1, f); fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f);
  fwrite("data", 1, 4, f); fwrite(&bytes, 4, 1, f);
  for (uint32_t i = 0; i < n; i++) {
    float v = x[i] < -1.0f ? -1.0f : x[i] > 1.0f ? 1.0f : x[i];
    const int16_t s = (int16_t)lrintf(v * 32767.0f);
    fwrite(&s, 2, 1, f);
  }
  fclose(f);
}

static float wav[SR * SECS];

int main(int argc, char** argv) {
  if (argc < 3) { printf("usage: preset_check eka_fold.so presets.ttl [out dir]\n"); return 2; }
  void* lib = dlopen(argv[1], RTLD_NOW);
  if (!lib) { printf("FAIL dlopen: %s\n", dlerror()); return 1; }
  d = ((LV2_Descriptor_Function)dlsym(lib, "lv2_descriptor"))(0);
  parse(argv[2]);
  if (npresets == 0) { printf("FAIL: no presets in %s\n", argv[2]); return 1; }
  lv2_atom_forge_init(&forge, &map);
  const LV2_Feature map_f = { LV2_URID__map, &map };
  const LV2_Feature* feats[] = { &map_f, NULL };
  printf("%-20s %8s %8s  %s\n", "preset", "peak dB", "RMS dB", "");
  for (int pi = 0; pi < npresets; pi++) {
    const Preset* p = &presets[pi];
    if (p->nctl != NCTL) { printf("FAIL %s: %d ports, not %d\n", p->id, p->nctl, NCTL); fails++; continue; }
    inst = d->instantiate(d, SR, "", feats);              /* a fresh instance: nothing left from the last one */
    for (int i = 0; i < NPORTS; i++) {
      if (i == NCTL) d->connect_port(inst, i, &in_buf);
      else if (i == NCTL + 1) d->connect_port(inst, i, out_l);
      else if (i == NCTL + 2) d->connect_port(inst, i, out_r);
      else if (i == NCTL + 3) d->connect_port(inst, i, &notify_buf);
      else d->connect_port(inst, i, &ports[i]);
    }
    for (int i = 0; i < NCTL; i++) ports[i] = p->ctl[i];
    cur = p;
    const LV2_State_Interface* si = (const LV2_State_Interface*)d->extension_data(LV2_STATE__interface);
    si->restore(inst, retrieve, NULL, 0, NULL);
    d->activate(inst);
    in_begin();
    const int mono = (int)p->ctl[24] != 0;              /* play_mode */
    const int chord[4] = { 48, 55, 60, 64 };
    const int phrase[4] = { 36, 43, 41, 48 };
    const int nblocks = SR * SECS / NFR;
    float pk = 0.0f;
    double sum = 0.0;
    int nan = 0;
    for (int b = 0; b < nblocks; b++) {
      const float t = (float)b * NFR / SR;
      if (!mono) {
        if (b == 0) for (int k = 0; k < 4; k++) in_midi(0x90, (uint8_t)chord[k], 100);
        if (b == (int)(1.5f * SR / NFR)) for (int k = 0; k < 4; k++) in_midi(0x80, (uint8_t)chord[k], 0);
      } else {
        for (int k = 0; k < 4; k++) {                    /* 0.4 s apart, each held 0.5 s: they overlap */
          if (b == (int)(0.4f * k * SR / NFR)) in_midi(0x90, (uint8_t)phrase[k], 100);
          if (b == (int)((0.4f * k + 0.5f) * SR / NFR)) in_midi(0x80, (uint8_t)phrase[k], 0);
        }
      }
      run_once();
      for (int i = 0; i < NFR; i++) {
        const float v = out_l[i];
        if (!isfinite(v)) nan = 1;
        pk = fabsf(v) > pk ? fabsf(v) : pk;
        if (t < 2.0f) sum += (double)v * v;
        wav[b * NFR + i] = v;
      }
    }
    const double rms = sqrt(sum / (2.0 * SR));
    const double pdb = 20.0 * log10(pk > 1e-9f ? pk : 1e-9), rdb = 20.0 * log10(rms > 1e-9 ? rms : 1e-9);
    const char* bad = nan ? "NaN" : pk < 0.01f ? "silent" : pk > 0.995f ? "clips" : "";
    printf("%-20s %8.1f %8.1f  %s\n", p->id, pdb, rdb, bad);
    if (*bad) fails++;
    if (argc > 3) {
      char path[600];
      snprintf(path, sizeof path, "%s/%s.wav", argv[3], p->id);
      write_wav(path, wav, SR * SECS);
    }
    d->deactivate(inst);
    d->cleanup(inst);
  }
  printf(fails ? "preset_check: %d FAILED\n" : "preset_check: all %d presets play\n", fails ? fails : npresets);
  return fails ? 1 : 0;
}
