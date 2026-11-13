/* Eka Bloom: transport behaviour of the LV2 wrapper.
   Stop = silent (once the transport has run), every start plays from step 1, zynseq stop / restart while
   JACK keeps rolling (beat from the widget in shared memory). Run by ./scripts/test.sh. */
#include "../src/eka_bloom.c"

static char uris[512][128];
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
static uint8_t inbuf[4096], outbuf[65536];
static LV2_Atom_Forge forge;
static LV2_Atom_Forge_Frame seq_frame;
static Arp* A;
static int fails, ons, offs, zbeat = -1;

static void check(const char* name, int ok) {
  printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) fails++;
}

static void begin_block(void) {
  lv2_atom_forge_set_buffer(&forge, inbuf, sizeof inbuf);
  lv2_atom_forge_sequence_head(&forge, &seq_frame, 0);
}

static void note_on(uint8_t note) {
  const uint8_t msg[3] = { 0x90, note, 100 };
  lv2_atom_forge_frame_time(&forge, 0);
  lv2_atom_forge_atom(&forge, 3, A->midi_Event);
  lv2_atom_forge_write(&forge, msg, 3);
}

/* time:Position as jalv sends it (bar 0-based, barBeat 0-based) */
static void position(float speed, long bar, float bar_beat) {
  LV2_Atom_Forge_Frame f;
  lv2_atom_forge_frame_time(&forge, 0);
  lv2_atom_forge_object(&forge, &f, 0, A->time_Position);
  lv2_atom_forge_key(&forge, A->time_speed); lv2_atom_forge_float(&forge, speed);
  lv2_atom_forge_key(&forge, A->time_barBeat); lv2_atom_forge_float(&forge, bar_beat);
  lv2_atom_forge_key(&forge, A->time_bar); lv2_atom_forge_long(&forge, bar);
  lv2_atom_forge_key(&forge, A->time_bpm); lv2_atom_forge_float(&forge, 120.0f);
  lv2_atom_forge_key(&forge, A->time_bpb); lv2_atom_forge_float(&forge, 4.0f);
  lv2_atom_forge_pop(&forge, &f);
}

static void end_block_run(void) {
  lv2_atom_forge_pop(&forge, &seq_frame);
  if (A->viz) { A->viz->ui.zbeat = zbeat; A->viz->ui.magic = UIIN_MAGIC; }
  LV2_Atom_Sequence* out = (LV2_Atom_Sequence*)outbuf;
  out->atom.size = sizeof outbuf - sizeof(LV2_Atom);
  run(A, BLOCK);
  LV2_ATOM_SEQUENCE_FOREACH(out, ev) {
    if (ev->body.type != A->midi_Event) continue;
    const uint8_t* b = (const uint8_t*)LV2_ATOM_BODY_CONST(&ev->body);
    if ((b[0] & 0xF0) == 0x90 && b[2] > 0) ons++;
    else if ((b[0] & 0xF0) == 0x80 || (b[0] & 0xF0) == 0x90) offs++;
  }
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

int main(void) {
  const LV2_Feature fmap = { LV2_URID__map, &map };
  const LV2_Feature* feats[] = { &fmap, NULL };
  A = (Arp*)instantiate(NULL, SR, "/tmp", feats);
  if (!A) { printf("FAIL: instantiate\n"); return 1; }
  lv2_atom_forge_init(&forge, &map);
  connect_port(A, P_MIDI_IN, inbuf);
  connect_port(A, P_MIDI_OUT, outbuf);

  /* a held chord, the transport never started: free running (arpeggiate without the sequencer) */
  begin_block(); note_on(60); note_on(64); note_on(67); end_block_run();
  ons = 0; run_secs(1.0);
  check("transport never started: plays on its own clock", ons >= 6);

  /* transport starts at bar 3 (0-based 2), beat 1 */
  begin_block(); position(1, 2, 0); end_block_run();
  check("start: the pattern from step 1", A->core.step_count == 1);
  ons = 0; run_secs(1.0);
  check("rolling: plays", ons >= 6);

  /* stop in the middle of a bar: silent from now on, the sounding note is released */
  run_secs(0.37);
  begin_block(); position(0, 3, 1.3f); end_block_run();
  check("stop releases the sounding note", !A->core.sounding);
  ons = 0; run_secs(1.5);
  check("stopped after running: silent", ons == 0);

  /* start again at another bar: from step 1 again, not where it stopped */
  begin_block(); position(1, 7, 0); end_block_run();
  check("start again: step 1", A->core.step_count == 1);
  ons = 0; run_secs(0.6);
  check("start again: plays", ons >= 3);

  /* zynseq: the widget says stopped (JACK still rolling) -> silent; a beat again -> step 1 */
  zbeat = 3; run_secs(0.02);
  zbeat = 0; hb(1.2);
  ons = 0; hb(1.0);
  check("zynseq stopped, JACK rolling: silent", ons == 0);
  zbeat = 1; run_secs(0.02);
  check("zynseq starts a scene (no JACK start): step 1", A->core.step_count == 1);
  ons = 0; run_secs(0.6);
  check("and plays", ons >= 3);

  /* no news from the widget (closed): JACK decides again */
  zbeat = 0; hb(1.2);
  run_secs(2.0);
  ons = 0; run_secs(0.6);
  check("stale 'stopped' from the widget: plays with JACK", ons >= 3);

  /* JACK start shortly after a stopped heartbeat: the start isn't cut while the widget catches up */
  hb(0.6);
  begin_block(); position(0, 9, 0); end_block_run();
  hb(0.4);
  begin_block(); position(1, 9, 0); end_block_run();
  ons = 0; run_secs(0.4);
  check("JACK start right after a zynseq stop: plays at once", ons >= 2);

  cleanup(A);
  printf("%d failed\n", fails);
  return fails ? 1 : 0;
}
