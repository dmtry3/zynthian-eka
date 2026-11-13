/* Eka Roll engine tests: Undo/Redo touch bounce, zynseq beat sync (zbeat port).
   Build and run: make test (or gcc ... test/test_transport.c, see Makefile). */
#include "../src/eka_roll.c"
#include <lv2/atom/forge.h>

static char uris[64][128];
static int nuris;
static LV2_URID map_uri(LV2_URID_Map_Handle h, const char* uri) {
  (void)h;
  for (int i = 0; i < nuris; i++) if (!strcmp(uris[i], uri)) return (LV2_URID)(i + 1);
  snprintf(uris[nuris], sizeof uris[0], "%s", uri);
  return (LV2_URID)(++nuris);
}

#define SR 48000.0
#define BLOCK 256
static LV2_URID_Map map = { NULL, map_uri };
static float ctl[N_IN], outv[N_OUT], zbeat = -1;
static uint8_t inbuf[4096], outbuf[65536];
static LV2_Atom_Forge forge;
static LV2_Atom_Forge_Frame seq_frame;
static Looper* L;
static int fails;

static void check(const char* name, int ok) {
  printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) fails++;
}

static void begin_block(void) {
  lv2_atom_forge_set_buffer(&forge, inbuf, sizeof inbuf);
  lv2_atom_forge_sequence_head(&forge, &seq_frame, 0);
}

/* time:Position as jalv sends it (bar 0-based, barBeat 0-based) */
static void position(float speed, long bar, float bar_beat) {
  LV2_Atom_Forge_Frame f;
  lv2_atom_forge_frame_time(&forge, 0);
  lv2_atom_forge_object(&forge, &f, 0, L->time_Position);
  lv2_atom_forge_key(&forge, L->time_speed); lv2_atom_forge_float(&forge, speed);
  lv2_atom_forge_key(&forge, L->time_barBeat); lv2_atom_forge_float(&forge, bar_beat);
  lv2_atom_forge_key(&forge, L->time_bar); lv2_atom_forge_long(&forge, bar);
  lv2_atom_forge_key(&forge, L->time_bpm); lv2_atom_forge_float(&forge, 120.0f);
  lv2_atom_forge_key(&forge, L->time_bpb); lv2_atom_forge_float(&forge, 4.0f);
  lv2_atom_forge_pop(&forge, &f);
}

static void end_block_run(void) {
  lv2_atom_forge_pop(&forge, &seq_frame);
  LV2_Atom_Sequence* out = (LV2_Atom_Sequence*)outbuf;
  out->atom.size = sizeof outbuf - sizeof(LV2_Atom);
  run(L, BLOCK);
}

static void run_secs(double s) {
  for (int i = 0; i < (int)(s * SR / BLOCK); i++) { begin_block(); end_block_run(); }
}

static double beat_in_bar(void) { return pmod(L->clk, bar_ticks(L)) / PPQN; }

/* a 1-bar loop with two layers, playing */
static void make_loop(void) {
  L->loop_len = 4 * PPQN; L->loop_anchor = 0; L->nlayers = 2; L->nredo = 0; L->bk_valid = 0;
  for (int i = 0; i < 2; i++) {
    L->layers[i].n = 1;
    L->layers[i].ev[0] = (Ev){ .tick = 0, .dur = 100, .st = 0x90, .d1 = 60 + i, .d2 = 100 };
  }
  L->state = ST_PLAY;
}

int main(void) {
  LV2_URID_Map* mp = &map;
  const LV2_Feature fmap = { LV2_URID__map, mp };
  const LV2_Feature* feats[] = { &fmap, NULL };
  L = (Looper*)instantiate(&desc, SR, "/tmp", feats);
  if (!L) { printf("FAIL: instantiate\n"); return 1; }
  lv2_atom_forge_init(&forge, mp);
  connect_port(L, 0, inbuf);
  connect_port(L, 1, outbuf);
  for (int i = 0; i < N_IN; i++) connect_port(L, 2 + i, &ctl[i]);
  for (int i = 0; i < N_OUT; i++) connect_port(L, OUT_BASE + i, &outv[i]);
  connect_port(L, ZBEAT_PORT, &zbeat);
  ctl[I_LAYER] = 1; ctl[I_LAUNCH] = 2; ctl[I_QSTR] = 100; ctl[I_QTARGET] = 2; ctl[I_ELAYER] = 1;
  ctl[I_SLOT] = 1; ctl[I_QMASK] = 255;

  /* transport rolls from bar 0 */
  begin_block(); position(1, 0, 0); end_block_run();
  run_secs(0.5);
  check("transport rolling", outv[O_RUN] == 1.0f);

  /* ---- Undo after an edit: one tap with a touch bounce undoes only the edit ---- */
  make_loop();
  backup_make(L);                                     /* as Quantize does */
  ctl[C_UNDO] = 1; run_secs(0.05);
  ctl[C_UNDO] = 0; run_secs(0.05);                    /* bounce 50 ms later */
  check("undo + bounce: edit undone, layers kept", L->nlayers == 2 && !L->bk_valid);
  run_secs(0.4);
  ctl[C_UNDO] = 1; run_secs(0.05);
  check("next undo (0.45 s later) removes the last layer", L->nlayers == 1 && L->nredo == 1);
  run_secs(0.4);
  ctl[C_REDO] = 1; run_secs(0.05);
  ctl[C_REDO] = 0; run_secs(0.05);
  check("redo + bounce: one layer back", L->nlayers == 2 && L->nredo == 0);

  /* ---- zynseq sync ---- */
  zbeat = 3; run_secs(0.02);
  check("first zbeat value is only remembered", outv[O_RUN] == 1.0f && L->zb_prev == 3);

  /* zynseq stops, JACK keeps rolling -> the loop pauses */
  zbeat = 0; run_secs(0.02);
  double c0 = L->clk;
  run_secs(0.7);
  check("zynseq stopped: loop paused (run = 0, clock frozen)", outv[O_RUN] == 0.0f && L->clk == c0);

  /* zynseq restarts a scene (no JACK start): beat 1 -> clock jumps to the bar start */
  zbeat = 1; run_secs(0.02);
  check("zynseq restart: running again from beat 1", outv[O_RUN] == 1.0f && beat_in_bar() < 0.1);

  /* whole-beat offset: zynseq says beat 3 when Roll is on beat 1 -> fixed after two beats agree */
  run_secs(0.5 - 0.02 - 0.0);                         /* now at beat 2 start (index 1) */
  zbeat = 4; run_secs(0.02);                          /* zynseq: beat 4 (2 beats ahead) */
  check("one mismatching beat: no jump yet", beat_in_bar() > 0.9 && beat_in_bar() < 1.2);
  run_secs(0.48);
  zbeat = 1; run_secs(0.02);                          /* zynseq: next bar, beat 1; Roll at beat 3 */
  check("two mismatching beats in a row: clock moved 2 beats", beat_in_bar() < 0.2);
  run_secs(0.48);
  zbeat = 2; run_secs(0.02);
  check("in step afterwards: no more jumps", beat_in_bar() > 0.9 && beat_in_bar() < 1.2);

  /* small latency of the widget (0.4 beat late) is not a mismatch */
  run_secs(0.18);
  zbeat = 3; run_secs(0.02);
  check("a late beat (0.4) does not move the clock", beat_in_bar() > 1.35 && beat_in_bar() < 1.5);

  /* normal stop/start through JACK clears the zynseq pause */
  zbeat = 0; run_secs(0.02);
  begin_block(); position(0, 0, 0); end_block_run();
  run_secs(0.2);
  begin_block(); position(1, 0, 0); end_block_run();
  run_secs(0.02);
  check("JACK start resumes even before the widget writes a beat", outv[O_RUN] == 1.0f);

  cleanup(L);
  printf("%d failed\n", fails);
  return fails ? 1 : 0;
}
