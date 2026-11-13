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
static int qmask_ui = 255;           /* what the widget writes to shared memory (UiIn) */
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
  L->uiin->zbeat = (int)zbeat; L->uiin->qmask = qmask_ui; L->uiin->magic = UIIN_MAGIC;
  LV2_Atom_Sequence* out = (LV2_Atom_Sequence*)outbuf;
  out->atom.size = sizeof outbuf - sizeof(LV2_Atom);
  run(L, BLOCK);
}

static void run_secs(double s) {
  for (int i = 0; i < (int)(s * SR / BLOCK); i++) { begin_block(); end_block_run(); }
}

/* the widget while zynseq is stopped: 0 and -2 alternating every 0.5 s */
static void hb(double secs) {
  for (double t = 0; t < secs; t += 0.5) {
    zbeat = (zbeat == 0) ? -2 : 0;
    run_secs(secs - t < 0.5 ? secs - t : 0.5);
  }
}

static void connect_all(void) {
  lv2_atom_forge_init(&forge, &map);
  connect_port(L, 0, inbuf);
  connect_port(L, 1, outbuf);
  for (int i = 0; i < N_IN; i++) connect_port(L, 2 + i, &ctl[i]);
  for (int i = 0; i < N_OUT; i++) connect_port(L, OUT_BASE + i, &outv[i]);
}

static double beat_in_bar(void) { return pmod(L->clk, bar_ticks(L)) / PPQN; }

/* a 1-bar loop with two layers, playing */
static void make_loop(void) {
  L->loop_len = 4 * PPQN; L->loop_anchor = 0; L->nlayers = 2;
  hist_clear(L);
  push(L, L->und, &L->nund, (HEnt){ H_LAYER, 0, -1 });          /* layer 2 was an overdub */
  for (int i = 0; i < 2; i++) {
    L->layers[i].n = 1;
    L->layers[i].ev[0] = (Ev){ .tick = 0, .dur = 100, .st = 0x90, .d1 = 60 + i, .d2 = 100 };
  }
  L->state = ST_PLAY;
}

int main(void) {
  const LV2_Feature fmap = { LV2_URID__map, &map };
  const LV2_Feature* feats[] = { &fmap, NULL };
  L = (Looper*)instantiate(&desc, SR, "/tmp", feats);
  if (!L) { printf("FAIL: instantiate\n"); return 1; }
  connect_all();
  ctl[I_LAYER] = 1; ctl[I_LAUNCH] = 2; ctl[I_QSTR] = 100; ctl[I_QTARGET] = 2; ctl[I_ELAYER] = 1;
  ctl[I_SLOT] = 1; ctl[I_QMASK] = 255;

  /* transport rolls from bar 0 */
  begin_block(); position(1, 0, 0); end_block_run();
  run_secs(0.5);
  check("transport rolling", outv[O_RUN] == 1.0f);

  /* ---- Undo/Redo knob gesture: turned and back = Undo, left 0.5 s = Redo ---- */
  make_loop();
  run_cmd(L, C_HALF); run_cmd(L, C_DOUBLE);                       /* two edits to undo */
  ctl[C_UNDO] = 1; run_secs(0.1);
  check("knob turned: nothing yet", L->nund == 3);
  ctl[C_UNDO] = 0; run_secs(0.05);
  check("turned and back: Undo (Double)", L->nund == 2 && L->msg_code == 30 + E_DOUBLE);
  ctl[C_UNDO] = 1; run_secs(0.03); ctl[C_UNDO] = 0; run_secs(0.05);
  check("a bounce right after is ignored", L->nund == 2);
  run_secs(0.4);
  ctl[C_UNDO] = 1; run_secs(0.45);
  check("held 0.45 s: nothing yet", L->nred == 1);
  run_secs(0.1);
  check("held 0.5 s: Redo (Double)", L->nred == 0 && L->msg_code == 40 + E_DOUBLE);
  run_secs(0.5);
  ctl[C_UNDO] = 0; run_secs(0.05);
  check("turning back after the Redo does nothing", L->nund == 3);
  run_secs(0.4);
  ctl[C_UNDO] = 1; ctl[C_PLAY] = 1; run_secs(0.7);    /* two ports at once = snapshot restore */
  check("undo set by a snapshot restore is not a gesture", L->nund == 3 && L->state == ST_PLAY);
  ctl[C_UNDO] = 0; ctl[C_PLAY] = 0; run_secs(0.9);
  L->uiin->redo++; run_secs(0.05);
  check("redo counter in shared memory (widget) reaches the engine", L->msg_code == 26);

  /* ---- Undo/Redo scenarios ---- */
#define UNDO() do { run_secs(0.35); run_cmd(L, C_UNDO); } while (0)
#define REDO() do { run_secs(0.35); run_cmd(L, C_REDO); } while (0)
  make_loop();
  L->layers[0].ev[0].tick = 37; L->layers[1].ev[0].tick = 517;
  ctl[I_QGRID] = 3; qmask_ui = 255; run_secs(0.02);
  run_cmd(L, C_QAPPLY);
  check("quantize: both layers on the grid", L->layers[0].ev[0].tick == 0 && L->layers[1].ev[0].tick == 480);
  UNDO();
  check("undo quantize: notes back, message 'Undo: Quantize'",
        L->layers[0].ev[0].tick == 37 && L->layers[1].ev[0].tick == 517 && L->msg_code == 30 + E_QUANT);
  REDO();
  check("redo quantize: on the grid again, 'Redo: Quantize'",
        L->layers[0].ev[0].tick == 0 && L->layers[1].ev[0].tick == 480 && L->msg_code == 40 + E_QUANT);
  UNDO();
  check("undo it again", L->layers[0].ev[0].tick == 37 && L->layers[1].ev[0].tick == 517);
  UNDO();
  check("next undo hides overdub layer 2 ('Undo: layer 2')", L->nlayers == 1 && L->msg_code == 52);
  UNDO();
  check("nothing more to undo (the first layer stays)", L->nlayers == 1 && L->msg_code == 25);
  REDO();
  check("redo: layer 2 back ('Redo: layer 2')", L->nlayers == 2 && L->msg_code == 62 && L->layers[1].ev[0].tick == 517);
  REDO();
  check("redo: quantize again", L->layers[0].ev[0].tick == 0 && L->layers[1].ev[0].tick == 480 && L->msg_code == 40);
  REDO();
  check("nothing more to redo", L->msg_code == 26);

  /* a new edit forgets what could be redone */
  UNDO();
  run_cmd(L, C_HALF);
  REDO();
  check("after a new edit, Redo has nothing", L->msg_code == 26);
  UNDO();
  check("undo Half", L->loop_len == 4 * PPQN && L->msg_code == 30 + E_HALF);

  /* overdub: undo while recording drops the take, redo brings it back */
  run_cmd(L, C_REC);
  check("overdub started (layer 3)", L->state == ST_OVERDUB && L->nlayers == 3);
  L->layers[2].n = 1; L->layers[2].ev[0] = (Ev){ .tick = 960, .dur = 100, .st = 0x90, .d1 = 70, .d2 = 90 };
  UNDO();
  check("undo during overdub: take dropped, playing ('Undo: overdub')",
        L->state == ST_PLAY && L->nlayers == 2 && L->msg_code == 20);
  REDO();
  check("redo: the take is back as layer 3", L->nlayers == 3 && L->layers[2].n == 1 && L->msg_code == 63);
  UNDO();
  check("undo: layer 3 hidden again", L->nlayers == 2 && L->msg_code == 53);
  run_cmd(L, C_REC); run_cmd(L, C_REC);                 /* empty overdub: not kept, not in the history */
  REDO();
  check("a new (empty) overdub forgets the hidden layer", L->nlayers == 2 && L->msg_code == 26);

  /* an edit after hiding a layer: undo the edit keeps the hidden layer for redo */
  run_cmd(L, C_REC); L->layers[2].n = 1; run_cmd(L, C_REC);      /* overdub layer 3 with one note */
  UNDO();                                                         /* hide layer 3 */
  ctl[I_TRANSP] = 5; ctl[I_ELAYER] = 1; run_secs(0.02);
  run_cmd(L, C_EAPPLY);
  check("transpose is an edit", L->layers[0].transp == 5);
  UNDO();
  check("undo transpose", L->layers[0].transp == 0 && L->msg_code == 30 + E_TRANSP);
  REDO();
  check("redo transpose", L->layers[0].transp == 5 && L->msg_code == 40 + E_TRANSP);
  ctl[I_TRANSP] = 0;

  /* mutes are not undone */
  L->layers[0].muted = 1;
  UNDO();
  check("undo of an edit keeps the current mute", L->layers[0].muted == 1 && L->layers[0].transp == 0);
  L->layers[0].muted = 0;

  /* many edits: the oldest are forgotten, the newest undo and redo fine */
  make_loop();
  for (int k = 0; k < 12; k++) run_cmd(L, (k & 1) ? C_DOUBLE : C_HALF);
  int undone = 0;
  for (int k = 0; k < 14; k++) { UNDO(); if (L->msg_code == 30 + E_HALF || L->msg_code == 30 + E_DOUBLE) undone++; }
  /* edits alternate Half / Double: after undoing k of 12 the loop is as after edit 12 - k */
  check("12 edits: at least 6 can be undone, back to the right state",
        undone >= 6 && L->loop_len == (((12 - undone) & 1) ? 2 * PPQN : 4 * PPQN));
  int redone = 0;
  for (int k = 0; k < 14; k++) { REDO(); if (L->msg_code >= 40 && L->msg_code < 44) redone++; }
  check("and redone the same number of times", redone == undone);

  /* clear / load / delete forget the history */
  make_loop(); run_cmd(L, C_HALF);
  L->state = ST_PLAY; ctl[I_LAYER] = 1;
  run_cmd(L, C_LDEL); run_cmd(L, C_LDEL);
  UNDO();
  check("after deleting a layer there is nothing to undo", L->msg_code == 25);

  /* ---- Quantize: only the layers in qmask (the device log had qmask = 253: layer 1 on, layer 2 off) ---- */
  make_loop();
  L->layers[0].ev[0].tick = 37; L->layers[1].ev[0].tick = 517;          /* both off the 1/16 grid */
  hist_clear(L);
  ctl[I_QGRID] = 3; qmask_ui = 253; run_secs(0.05);
  ctl[C_QAPPLY] = 1; run_secs(0.05);
  check("quantize qmask 253: layer 1 moved, layer 2 untouched",
        L->layers[0].ev[0].tick == 0 && L->layers[1].ev[0].tick == 517);
  ctl[C_QAPPLY] = 0; run_secs(0.05);
  qmask_ui = 255; ctl[I_QGRID] = 0;

  /* ---- zynseq sync ---- */
  zbeat = 3; run_secs(0.02);
  check("first zbeat value is only remembered", outv[O_RUN] == 1.0f && L->zb_prev == 3);

  /* zynseq stops, JACK keeps rolling: the widget alternates 0 / -2 -> the loop pauses */
  zbeat = 0; run_secs(0.02);
  double c0 = L->clk;
  hb(1.0);
  check("zynseq stopped: loop paused (run = 0, clock frozen)", outv[O_RUN] == 0.0f && L->clk == c0);

  /* zynseq restarts a scene (no JACK start): beat 1 -> clock jumps to the bar start */
  zbeat = 1; run_secs(0.02);
  check("zynseq restart: running again from beat 1", outv[O_RUN] == 1.0f && beat_in_bar() < 0.1);

  /* whole-beat offset: zynseq says beat 4 when Roll is on beat 2 -> fixed after two beats agree */
  run_secs(0.48);                                     /* now at beat 2 start (index 1) */
  zbeat = 4; run_secs(0.02);
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

  /* the widget goes away while paused: after 1.5 s without news, JACK decides again */
  zbeat = 0; hb(1.0);
  check("paused again", outv[O_RUN] == 0.0f);
  run_secs(2.0);
  check("no heartbeat for 2 s: running on JACK", outv[O_RUN] == 1.0f);

  /* normal JACK stop/start while zynseq reports 'stopped': the start is not cut by an old 'stopped' */
  hb(0.6);
  begin_block(); position(0, 0, 0); end_block_run();
  hb(0.4);
  begin_block(); position(1, 0, 0); end_block_run();
  run_secs(0.1);
  check("JACK start resumes even before the widget writes a beat", outv[O_RUN] == 1.0f);
  zbeat = 1; run_secs(0.3);
  check("zynseq playing: keeps running", outv[O_RUN] == 1.0f);

  /* Zynthian restart with JACK still rolling: new instance, zbeat restored as 0, widget alternates -> pause
     after the start grace; a 0 that never changes (no widget) does not pause */
  cleanup(L);
  L = (Looper*)instantiate(&desc, SR, "/tmp", feats);
  connect_all();
  zbeat = 0;
  begin_block(); position(1, 3, 2.5f); end_block_run();
  run_secs(2.0);
  check("restart, stale 0 from the snapshot: running (JACK)", outv[O_RUN] == 1.0f);
  hb(1.2);
  check("restart, widget says zynseq stopped: paused", outv[O_RUN] == 0.0f);

  cleanup(L);
  printf("%d failed\n", fails);
  return fails ? 1 : 0;
}
