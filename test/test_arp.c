/* Tests for arp_core without LV2: feed notes and ticks, print and check outgoing events. */
#include "../src/arp_core.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct { double tick; uint8_t st, d1, d2; } Ev;
typedef struct { Ev ev[512]; int n; } Log;

static int failures = 0;

static void collect(void* user, double tick, uint8_t st, uint8_t d1, uint8_t d2) {
  Log* l = (Log*)user;
  if (l->n < 512) { l->ev[l->n].tick = tick; l->ev[l->n].st = st; l->ev[l->n].d1 = d1; l->ev[l->n].d2 = d2; l->n++; }
}

static void print_log(const char* title, const Log* l) {
  printf("  %s (%d events):\n", title, l->n);
  for (int i = 0; i < l->n; i++)
    printf("    %7.1f  %s ch%d note %3d vel %3d\n", l->ev[i].tick,
           (l->ev[i].st & 0xF0) == 0x90 ? "ON " : "OFF", l->ev[i].st & 0x0F, l->ev[i].d1, l->ev[i].d2);
}

/* expected: triples {tick, on(1)/off(0), note} */
static void check(const char* name, const Log* l, const double (*exp)[3], int nexp) {
  int ok = (l->n == nexp);
  for (int i = 0; ok && i < nexp; i++) {
    int on = (l->ev[i].st & 0xF0) == 0x90;
    if (fabs(l->ev[i].tick - exp[i][0]) > 1e-6 || on != (int)exp[i][1] || l->ev[i].d1 != (uint8_t)exp[i][2]) ok = 0;
  }
  printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) { print_log("got", l); failures++; }
}

static int same_logs(const Log* a, const Log* b) {
  if (a->n != b->n) return 0;
  for (int i = 0; i < a->n; i++)
    if (fabs(a->ev[i].tick - b->ev[i].tick) > 1e-6 || a->ev[i].st != b->ev[i].st || a->ev[i].d1 != b->ev[i].d1 ||
        a->ev[i].d2 != b->ev[i].d2) return 0;
  return 1;
}

static void chord(ArpCore* a) {
  arp_note_on(a, 0, 64, 90);
  arp_note_on(a, 0, 60, 100);   /* pressed out of order: arp must still go up */
  arp_note_on(a, 0, 67, 80);
}

static const double EXP_UP[][3] = {
  {0, 1, 60}, {120, 0, 60}, {240, 1, 64}, {360, 0, 64}, {480, 1, 67}, {600, 0, 67},
  {720, 1, 60}, {840, 0, 60}, {960, 1, 64}, {1080, 0, 64}, {1200, 1, 67}, {1320, 0, 67},
  {1440, 1, 60}, {1560, 0, 60}, {1680, 1, 64}, {1800, 0, 64},
};

static void test_up_one_block(void) {
  ArpCore a; Log l = {0};
  arp_init(&a); chord(&a);
  arp_process(&a, 0, 1920, collect, &l);
  check("chord C-E-G, up, 1/16, gate 50%, one block", &l, EXP_UP, 16);
  if (l.n && l.ev[0].d2 == 100 && l.ev[2].d2 == 90 && l.ev[4].d2 == 80 && l.ev[0].st == 0x90)
    printf("PASS: velocity and channel taken from held keys\n");
  else { printf("FAIL: velocity and channel taken from held keys\n"); failures++; }
}

static void run_chunks(Log* l, const double* sizes, int nsizes) {
  ArpCore a; arp_init(&a); chord(&a);
  double t = 0; int i = 0;
  while (t < 1920) {
    double e = t + sizes[i++ % nsizes];
    if (e > 1920) e = 1920;
    arp_process(&a, t, e, collect, l);
    t = e;
  }
}

static void test_chunks(void) {
  Log ref = {0};
  ArpCore a; arp_init(&a); chord(&a);
  arp_process(&a, 0, 1920, collect, &ref);
  const double s1[] = { 1 }, s2[] = { 7.3 }, s3[] = { 240 }, s4[] = { 120, 0.5, 333.3, 239.9, 0.1, 64 };
  const double* sets[] = { s1, s2, s3, s4 };
  const int lens[] = { 1, 1, 1, 6 };
  const char* names[] = { "1 tick", "7.3 ticks", "240 ticks (step on boundary)", "mixed sizes" };
  for (int k = 0; k < 4; k++) {
    Log l = {0};
    run_chunks(&l, sets[k], lens[k]);
    char name[96]; snprintf(name, sizeof name, "chunks of %s give the same events", names[k]);
    printf("%s: %s\n", same_logs(&ref, &l) ? "PASS" : "FAIL", name);
    if (!same_logs(&ref, &l)) { print_log("got", &l); failures++; }
  }
}

static void test_add_release(void) {
  ArpCore a; Log l = {0};
  arp_init(&a);
  arp_note_on(&a, 0, 60, 100);
  arp_note_on(&a, 0, 64, 100);
  arp_process(&a, 0, 300, collect, &l);          /* 60@0, 64@240 */
  arp_note_on(&a, 0, 62, 100);                   /* now 60 62 64; last was 64 (top) -> wraps to 60 */
  arp_process(&a, 300, 700, collect, &l);        /* off 64@360, on 60@480, off@600 */
  arp_note_off(&a, 0, 60); arp_note_off(&a, 0, 62); arp_note_off(&a, 0, 64);
  arp_process(&a, 700, 1500, collect, &l);       /* nothing */
  int first = arp_note_on(&a, 0, 67, 100);
  arp_note_on(&a, 0, 65, 100);
  arp_process(&a, 1500, 2000, collect, &l);      /* restarts from lowest: 65@1680, 67@1920 */
  static const double exp[][3] = {
    {0, 1, 60}, {120, 0, 60}, {240, 1, 64}, {360, 0, 64}, {480, 1, 60}, {600, 0, 60},
    {1680, 1, 65}, {1800, 0, 65}, {1920, 1, 67},
  };
  check("add note mid-chord, release all, new chord starts from lowest", &l, exp, 9);
  printf("%s: arp_note_on reports first key\n", first ? "PASS" : "FAIL");
  if (!first) failures++;
}

static void test_insert_below(void) {
  ArpCore a; Log l = {0};
  arp_init(&a); chord(&a);                       /* 60 64 67 */
  arp_process(&a, 0, 300, collect, &l);          /* 60@0, 64@240 */
  arp_note_on(&a, 0, 62, 100);                   /* inserted below the last played: must go on to 67 */
  arp_note_off(&a, 0, 60);                       /* removing a key does not break the order */
  arp_process(&a, 300, 1000, collect, &l);       /* 67@480, wrap to lowest 62@720, 64@960 */
  static const double exp[][3] = {
    {0, 1, 60}, {120, 0, 60}, {240, 1, 64}, {360, 0, 64}, {480, 1, 67}, {600, 0, 67},
    {720, 1, 62}, {840, 0, 62}, {960, 1, 64},
  };
  check("keys added/removed mid-arp: order keeps going up from last note", &l, exp, 9);
}

static void test_silence(void) {
  ArpCore a; Log l = {0};
  arp_init(&a);
  arp_note_on(&a, 2, 60, 100);
  arp_process(&a, 0, 50, collect, &l);           /* on@0 */
  arp_silence(&a, 50, collect, &l);              /* off@50 */
  arp_silence(&a, 51, collect, &l);              /* nothing */
  arp_process(&a, 50, 200, collect, &l);         /* no second off at 120 */
  static const double exp[][3] = { {0, 1, 60}, {50, 0, 60} };
  check("silence turns note off once, no double note-off", &l, exp, 2);
  if (l.n == 2 && l.ev[1].st == 0x82) printf("PASS: note-off on the note's channel\n");
  else { printf("FAIL: note-off on the note's channel\n"); failures++; }
}

static void test_late_off(void) {
  ArpCore a; Log l = {0};
  arp_init(&a);
  arp_note_on(&a, 0, 60, 100);
  arp_process(&a, 0, 10, collect, &l);           /* on@0, off due at 120 */
  arp_note_off(&a, 0, 60);
  arp_process(&a, 200, 230, collect, &l);        /* clock jumped past the off: emit at 200 */
  static const double exp[][3] = { {0, 1, 60}, {200, 0, 60} };
  check("overdue note-off is sent at block start", &l, exp, 2);
}

static void test_release_all(void) {
  ArpCore a; Log l = {0};
  arp_init(&a); chord(&a);
  arp_process(&a, 0, 100, collect, &l);
  arp_release_all(&a);
  arp_process(&a, 100, 1000, collect, &l);
  static const double exp[][3] = { {0, 1, 60}, {120, 0, 60} };
  check("release all: current note ends by gate, nothing new", &l, exp, 2);
}

/* ---------- stage 3: parameters ---------- */

/* note-on pitches of a log, in order */
static int ons(const Log* l, int* out) {
  int k = 0;
  for (int i = 0; i < l->n; i++) if ((l->ev[i].st & 0xF0) == 0x90) out[k++] = l->ev[i].d1;
  return k;
}

static void check_seq(const char* name, const Log* l, const int* exp, int nexp) {
  int got[512]; int n = ons(l, got);
  int ok = (n == nexp);
  for (int i = 0; ok && i < n; i++) if (got[i] != exp[i]) ok = 0;
  printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) {
    printf("  got:"); for (int i = 0; i < n; i++) printf(" %d", got[i]);
    printf("\n  exp:"); for (int i = 0; i < nexp; i++) printf(" %d", exp[i]);
    printf("\n"); failures++;
  }
}

/* chord C-E-G, given order/octaves, 1/16, n steps */
static void run_order(Log* l, int order, int octaves, int nsteps) {
  ArpCore a; arp_init(&a); chord(&a);
  arp_set_params(&a, ARP_RATE_16, 50, order, octaves);
  arp_process(&a, 0, 240.0 * nsteps, collect, l);
}

static void test_orders(void) {
  struct { const char* name; int order, oct, n; int exp[16]; } t[] = {
    { "Up, 1 oct",        ARP_UP,     1, 7, { 60, 64, 67, 60, 64, 67, 60 } },
    { "Down, 1 oct",      ARP_DOWN,   1, 7, { 67, 64, 60, 67, 64, 60, 67 } },
    { "Up-Down, no repeated ends", ARP_UPDOWN, 1, 9, { 60, 64, 67, 64, 60, 64, 67, 64, 60 } },
    { "Down-Up, no repeated ends", ARP_DOWNUP, 1, 9, { 67, 64, 60, 64, 67, 64, 60, 64, 67 } },
    { "Up, 2 oct",        ARP_UP,     2, 8, { 60, 64, 67, 72, 76, 79, 60, 64 } },
    { "Down, 2 oct",      ARP_DOWN,   2, 7, { 79, 76, 72, 67, 64, 60, 79 } },
    { "Up-Down, 2 oct",   ARP_UPDOWN, 2, 12, { 60, 64, 67, 72, 76, 79, 76, 72, 67, 64, 60, 64 } },
    { "Up, 4 oct",        ARP_UP,     4, 13, { 60, 64, 67, 72, 76, 79, 84, 88, 91, 96, 100, 103, 60 } },
  };
  for (unsigned k = 0; k < sizeof t / sizeof t[0]; k++) {
    Log l = {0}; run_order(&l, t[k].order, t[k].oct, t[k].n);
    char name[96]; snprintf(name, sizeof name, "order %s", t[k].name);
    check_seq(name, &l, t[k].exp, t[k].n);
  }
}

static void test_single_note_orders(void) {
  for (int o = 0; o < ARP_NORDERS; o++) {
    ArpCore a; Log l = {0}; arp_init(&a);
    arp_set_params(&a, ARP_RATE_16, 50, o, 1);
    arp_note_on(&a, 0, 62, 100);
    arp_process(&a, 0, 240 * 4, collect, &l);
    static const int exp[] = { 62, 62, 62, 62 };
    char name[96]; snprintf(name, sizeof name, "single key repeats with order %d", o);
    check_seq(name, &l, exp, 4);
  }
}

static void test_played(void) {
  ArpCore a; Log l = {0}; arp_init(&a);
  arp_set_params(&a, ARP_RATE_16, 50, ARP_PLAYED, 2);
  arp_note_on(&a, 0, 64, 100); arp_note_on(&a, 0, 60, 100); arp_note_on(&a, 0, 67, 100);
  arp_process(&a, 0, 240 * 7, collect, &l);
  static const int exp[] = { 64, 60, 67, 76, 72, 79, 64 };
  check_seq("As played (E C G), 2 oct", &l, exp, 7);
}

static void random_run(Log* l, uint32_t seed) {
  ArpCore a; arp_init(&a); chord(&a);
  arp_seed(&a, seed);
  arp_set_params(&a, ARP_RATE_16, 50, ARP_RANDOM, 2);
  arp_process(&a, 0, 240 * 200, collect, l);
}

static void test_random(void) {
  Log l1 = {0}, l2 = {0}, l3 = {0};
  random_run(&l1, 42); random_run(&l2, 42); random_run(&l3, 7);
  int s1[512], s3[512]; int n = ons(&l1, s1); ons(&l3, s3);
  int ok_set = (n == 200), ok_rep = 1, hits[128] = {0};
  for (int i = 0; i < n; i++) {
    int p = s1[i];
    if (p != 60 && p != 64 && p != 67 && p != 72 && p != 76 && p != 79) ok_set = 0;
    else hits[p]++;
    if (i > 0 && s1[i] == s1[i - 1]) ok_rep = 0;
  }
  int all = hits[60] && hits[64] && hits[67] && hits[72] && hits[76] && hits[79];
  printf("%s: Random uses only held notes (2 oct), all of them\n", ok_set && all ? "PASS" : "FAIL");
  printf("%s: Random never repeats a note twice in a row\n", ok_rep ? "PASS" : "FAIL");
  printf("%s: Random with the same seed is reproducible\n", same_logs(&l1, &l2) ? "PASS" : "FAIL");
  printf("%s: Random with another seed differs\n", memcmp(s1, s3, sizeof(int) * 200) ? "PASS" : "FAIL");
  failures += !(ok_set && all) + !ok_rep + !same_logs(&l1, &l2) + !memcmp(s1, s3, sizeof(int) * 200);
}

static void test_rates(void) {
  static const int step[ARP_NRATES] = { 960, 480, 320, 240, 160, 120 };
  static const char* names[ARP_NRATES] = { "1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32" };
  for (int r = 0; r < ARP_NRATES; r++) {
    ArpCore a; Log l = {0}; arp_init(&a); chord(&a);
    arp_set_params(&a, r, 50, ARP_UP, 1);
    arp_process(&a, 100, 100 + 3840, collect, &l);     /* start off-grid */
    int ok = 1, k = 0;
    for (int i = 0; i < l.n; i++) {
      double t = l.ev[i].tick;
      if ((l.ev[i].st & 0xF0) == 0x90) {
        if (fmod(t, step[r]) != 0) ok = 0;
        k++;
      } else if (fmod(t - step[r] / 2, step[r]) != 0) ok = 0;
    }
    int expn = (int)(floor((3940.0 - 1) / step[r]) - floor(99.0 / step[r]));   /* grid points in [100, 3940) */
    char name[96]; snprintf(name, sizeof name, "rate %s: notes on its grid, gate half a step", names[r]);
    printf("%s: %s\n", ok && k == expn ? "PASS" : "FAIL", name);
    if (!(ok && k == expn)) { print_log("got", &l); failures++; }
  }
}

static void test_gates(void) {
  ArpCore a; Log l = {0}; arp_init(&a); chord(&a);
  arp_set_params(&a, ARP_RATE_16, 5, ARP_UP, 1);
  arp_process(&a, 0, 480, collect, &l);
  static const double e5[][3] = { {0, 1, 60}, {12, 0, 60}, {240, 1, 64}, {252, 0, 64} };
  check("gate 5% of 1/16 = 12 ticks", &l, e5, 4);

  Log l2 = {0}; arp_init(&a); chord(&a);
  arp_set_params(&a, ARP_RATE_16, 100, ARP_UP, 1);
  arp_process(&a, 0, 480, collect, &l2);
  static const double e100[][3] = { {0, 1, 60}, {240, 0, 60}, {240, 1, 64} };
  check("gate 100%: note-off before next note-on on the same tick", &l2, e100, 3);

  Log l3 = {0}; arp_init(&a); chord(&a);
  arp_set_params(&a, ARP_RATE_16, 0, 99, 9);        /* out of range values are clamped */
  printf("%s: parameters are clamped\n", a.gate_pct == 5 && a.order == ARP_LOOP && a.octaves == 4 ? "PASS" : "FAIL");
  if (!(a.gate_pct == 5 && a.order == ARP_LOOP && a.octaves == 4)) failures++;
  (void)l3;
}

static void test_live_changes(void) {
  ArpCore a; Log l = {0}; arp_init(&a); chord(&a);
  arp_set_params(&a, ARP_RATE_4, 100, ARP_UP, 1);
  arp_process(&a, 0, 1000, collect, &l);             /* 60@0 off@960, 64@960 (sounding till 1920) */
  arp_set_params(&a, ARP_RATE_32, 50, ARP_DOWN, 1);   /* faster, and change order */
  arp_process(&a, 1000, 1400, collect, &l);          /* 64 cut at next 1/32 step 1080; down from 64: 60, 67, 64 */
  static const double exp[][3] = {
    {0, 1, 60}, {960, 0, 60}, {960, 1, 64},
    {1080, 0, 64}, {1080, 1, 60}, {1140, 0, 60}, {1200, 1, 67}, {1260, 0, 67}, {1320, 1, 64}, {1380, 0, 64},
  };
  check("rate 1/4 -> 1/32 and Up -> Down on the fly: new grid, order continues from last note", &l, exp, 10);
}

static void test_high_notes(void) {
  ArpCore a; Log l = {0}; arp_init(&a);
  arp_set_params(&a, ARP_RATE_16, 50, ARP_UP, 4);
  arp_note_on(&a, 0, 100, 100);
  arp_note_on(&a, 0, 110, 100);
  arp_process(&a, 0, 240 * 5, collect, &l);
  static const int exp[] = { 100, 110, 112, 122, 124 };   /* 134, 136, 146 dropped */
  check_seq("notes above 127 are skipped", &l, exp, 5);
}

static void test_wide_chord_octaves(void) {
  ArpCore a; Log l = {0}; arp_init(&a);
  arp_set_params(&a, ARP_RATE_16, 50, ARP_UP, 2);
  arp_note_on(&a, 0, 48, 100); arp_note_on(&a, 0, 60, 100); arp_note_on(&a, 0, 62, 100);
  arp_process(&a, 0, 240 * 6, collect, &l);
  static const int exp[] = { 48, 60, 62, 72, 74, 48 };      /* 60 from both octaves played once */
  check_seq("span over an octave: sorted, duplicates removed", &l, exp, 6);
}

/* ---------- latch ---------- */

/* note-on pitches produced over [from, to) */
static int play(ArpCore* a, double from, double to, int* out) {
  Log l = {0};
  arp_process(a, from, to, collect, &l);
  return ons(&l, out);
}

static void expect(const char* name, const int* got, int n, const int* exp, int nexp) {
  int ok = (n == nexp);
  for (int i = 0; ok && i < n; i++) if (got[i] != exp[i]) ok = 0;
  printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) {
    printf("  got:"); for (int i = 0; i < n; i++) printf(" %d", got[i]);
    printf("\n  exp:"); for (int i = 0; i < nexp; i++) printf(" %d", exp[i]);
    printf("\n"); failures++;
  }
}

static void test_latch(void) {
  int g[64], n;
  ArpCore a;

  /* latch on: chord keeps playing after release */
  arp_init(&a); arp_set_latch(&a, 1); chord(&a);
  arp_note_off(&a, 0, 60); arp_note_off(&a, 0, 64); arp_note_off(&a, 0, 67);
  n = play(&a, 0, 240 * 4, g);
  { static const int e[] = { 60, 64, 67, 60 }; expect("latch: chord keeps playing after keys released", g, n, e, 4); }

  /* new chord after release replaces it, order restarts */
  arp_note_on(&a, 0, 50, 100); arp_note_on(&a, 0, 53, 100);
  n = play(&a, 960, 960 + 240 * 3, g);
  { static const int e[] = { 50, 53, 50 }; expect("latch: next chord replaces latched one, order from start", g, n, e, 3); }

  /* adding a key while others are down adds to the chord; releasing one keeps it */
  arp_note_on(&a, 0, 57, 100);                 /* 50 53 still down */
  arp_note_off(&a, 0, 50);                     /* 53 57 down, 50 stays latched */
  n = play(&a, 1680, 1680 + 240 * 3, g);       /* last was 50 -> 53 57 50 */
  { static const int e[] = { 53, 57, 50 }; expect("latch: key added while holding joins, released key stays", g, n, e, 3); }

  /* latch off while some keys are down: only those stay */
  arp_set_latch(&a, 0);                        /* 53 57 down */
  n = play(&a, 2400, 2400 + 240 * 3, g);       /* last 50 -> 53 57 53 */
  { static const int e[] = { 53, 57, 53 }; expect("latch off with keys down: only pressed keys keep playing", g, n, e, 3); }

  /* latch off with nothing down: stops */
  arp_init(&a); arp_set_latch(&a, 1); chord(&a);
  arp_note_off(&a, 0, 60); arp_note_off(&a, 0, 64); arp_note_off(&a, 0, 67);
  n = play(&a, 0, 240, g);
  arp_set_latch(&a, 0);
  Log l = {0};
  arp_process(&a, 240, 1200, collect, &l);
  { static const double e[][3] = { {120, 0, 60} };
    (void)n; check("latch off with nothing down: arp stops (sounding note already ended)", &l, e, 0); }
  printf("%s: latch off clears the latched chord\n", a.nheld == 0 ? "PASS" : "FAIL");
  if (a.nheld) failures++;

  /* latch turned on while holding: chord latches on release */
  arp_init(&a); chord(&a);
  arp_set_latch(&a, 1);
  arp_note_off(&a, 0, 60); arp_note_off(&a, 0, 64); arp_note_off(&a, 0, 67);
  n = play(&a, 0, 240 * 3, g);
  { static const int e[] = { 60, 64, 67 }; expect("latch on while holding: chord latches on release", g, n, e, 3); }

  /* latch on with nothing held: nothing happens until a chord */
  arp_init(&a); arp_set_latch(&a, 1);
  n = play(&a, 0, 960, g);
  { static const int e[] = { 0 }; expect("latch on with nothing pressed: silence", g, n, e, 0); }

  /* all notes off clears the latched chord */
  arp_init(&a); arp_set_latch(&a, 1); chord(&a);
  arp_note_off(&a, 0, 60); arp_note_off(&a, 0, 64); arp_note_off(&a, 0, 67);
  arp_release_all(&a);
  n = play(&a, 0, 960, g);
  { static const int e[] = { 0 }; expect("all notes off clears the latched chord", g, n, e, 0); }
  /* and the next press starts fresh (first key) */
  int first = arp_note_on(&a, 0, 62, 100);
  printf("%s: after all notes off the next key is a first key\n", first ? "PASS" : "FAIL");
  if (!first) failures++;

  /* replacing a latched chord is not a 'first key' (arp keeps its rhythm) */
  arp_init(&a); arp_set_latch(&a, 1); chord(&a);
  arp_note_off(&a, 0, 60); arp_note_off(&a, 0, 64); arp_note_off(&a, 0, 67);
  first = arp_note_on(&a, 0, 50, 100);
  printf("%s: replacing a latched chord keeps the rhythm (not a first key)\n", !first ? "PASS" : "FAIL");
  if (first) failures++;

  /* latch off -> behaves exactly as before */
  arp_init(&a); arp_set_latch(&a, 1); arp_set_latch(&a, 0); chord(&a);
  arp_note_off(&a, 0, 60); arp_note_off(&a, 0, 64); arp_note_off(&a, 0, 67);
  n = play(&a, 0, 960, g);
  { static const int e[] = { 0 }; expect("latch off: releasing keys stops the arp", g, n, e, 0); }
}

/* ---------- stage 4a: pattern steps ---------- */

static void test_pattern_shift_example(void) {
  ArpCore a; Log l = {0}; arp_init(&a); chord(&a);
  arp_set_length(&a, 4);
  arp_set_step(&a, 2, 100, 12, 100, 0);
  arp_set_step(&a, 3, 100, -5, 100, 0);
  arp_process(&a, 0, 240 * 8, collect, &l);
  static const int exp[] = { 60, 64, 79, 55, 64, 67, 72, 59 };   /* C E G' G- E G C' B- */
  check_seq("pattern Length 4, shifts 0 0 +12 -5 over C-E-G (example table)", &l, exp, 8);
}

static void test_prob(void) {
  int g[512], n;
  ArpCore a;
  arp_init(&a); chord(&a);
  arp_set_length(&a, 2);
  arp_set_step(&a, 1, 0, 0, 100, 0);           /* every 2nd step silent */
  n = play(&a, 0, 240 * 6, g);
  { static const int e[] = { 60, 67, 64 }; expect("prob 0%: step silent, note order still advances", g, n, e, 3); }

  Log l1 = {0}, l2 = {0};
  for (int k = 0; k < 2; k++) {
    arp_init(&a); chord(&a); arp_seed(&a, 1234);
    arp_set_length(&a, 1);
    arp_set_step(&a, 0, 50, 0, 100, 0);
    arp_process(&a, 0, 240 * 400, collect, k ? &l2 : &l1);
  }
  n = ons(&l1, g);
  printf("%s: prob 50%%: about half of 400 steps play (%d)\n", n > 160 && n < 240 ? "PASS" : "FAIL", n);
  printf("%s: prob 50%% is reproducible with the same seed\n", same_logs(&l1, &l2) ? "PASS" : "FAIL");
  failures += !(n > 160 && n < 240) + !same_logs(&l1, &l2);
}

static void test_shift_range(void) {
  int g[64], n;
  ArpCore a; arp_init(&a);
  arp_note_on(&a, 0, 120, 100); arp_note_on(&a, 0, 5, 100);
  arp_set_length(&a, 2);
  arp_set_step(&a, 0, 100, -24, 100, 0);       /* 5-24 < 0: silent */
  arp_set_step(&a, 1, 100, 24, 100, 0);        /* 120+24 > 127: silent */
  n = play(&a, 0, 240 * 4, g);
  { static const int e[] = { 0 }; expect("shift out of 0..127 makes the step silent", g, n, e, 0); }
  arp_set_step(&a, 0, 100, -60, 300, 50);       /* clamped to -24, 200 */
  printf("%s: step values are clamped\n", a.steps[0].shift == -24 && a.steps[0].vel == 200 ? "PASS" : "FAIL");
  if (!(a.steps[0].shift == -24 && a.steps[0].vel == 200)) failures++;
}

static void test_step_velocity(void) {
  ArpCore a; Log l = {0}; arp_init(&a);
  arp_note_on(&a, 0, 60, 100);
  arp_set_length(&a, 4);
  arp_set_step(&a, 0, 100, 0, 50, 0);
  arp_set_step(&a, 1, 100, 0, 200, 0);         /* 200 -> clamped 127 */
  arp_set_step(&a, 2, 100, 0, 0, 0);           /* 0 -> 1 (not a note-off) */
  arp_set_step(&a, 3, 100, 0, 110, 0);
  arp_process(&a, 0, 240 * 4, collect, &l);
  int v[4], k = 0;
  for (int i = 0; i < l.n; i++) if ((l.ev[i].st & 0xF0) == 0x90 && k < 4) v[k++] = l.ev[i].d2;
  int ok = k == 4 && v[0] == 50 && v[1] == 127 && v[2] == 1 && v[3] == 110;
  printf("%s: step velocity %% of key velocity, clamped to 1..127\n", ok ? "PASS" : "FAIL");
  if (!ok) failures++;
}

static void test_step_gate(void) {
  ArpCore a; Log l = {0}; arp_init(&a); chord(&a);
  arp_set_length(&a, 3);
  arp_set_step(&a, 0, 100, 0, 100, 0);         /* global 50% -> 120 */
  arp_set_step(&a, 1, 100, 0, 100, 10);        /* 24 */
  arp_set_step(&a, 2, 100, 0, 100, 100);       /* 240, off before next on */
  arp_process(&a, 0, 240 * 4, collect, &l);
  static const double exp[][3] = {
    {0, 1, 60}, {120, 0, 60}, {240, 1, 64}, {264, 0, 64}, {480, 1, 67}, {720, 0, 67}, {720, 1, 60}, {840, 0, 60},
  };
  check("step gate: Global, 10%, 100%", &l, exp, 8);
}

static void test_tie(void) {
  ArpCore a; Log l = {0}; arp_init(&a); chord(&a);
  arp_set_length(&a, 4);
  arp_set_step(&a, 0, 100, 0, 100, ARP_GATE_TIE);
  arp_set_step(&a, 1, 100, -4, 100, ARP_GATE_TIE);   /* 64-4 = 60: same pitch as the tied note */
  arp_set_step(&a, 2, 100, 0, 100, ARP_GATE_TIE);
  arp_set_step(&a, 3, 0, 0, 100, 0);                 /* silent: the tie ends here */
  arp_process(&a, 0, 240 * 5, collect, &l);
  /* step 1: 60 tied; step 2: 60 again -> continues, no events; step 3: legato 67 (on before off);
   * step 4 silent: tie ends; step 5 = pattern step 1 again: 64 tied (no off inside the range) */
  static const double exp2[][3] = { {0, 1, 60}, {480, 1, 67}, {480, 0, 60}, {720, 0, 67}, {960, 1, 64} };
  check("tie: same pitch continues, legato on-before-off, silent step ends it", &l, exp2, 5);

  Log l2 = {0};
  arp_note_off(&a, 0, 60); arp_note_off(&a, 0, 64); arp_note_off(&a, 0, 67);
  arp_process(&a, 1200, 1500, collect, &l2);
  static const double e3[][3] = { {1200, 0, 64} };
  check("tie: keys released -> tied note ends at the next step", &l2, e3, 1);

  ArpCore b; Log l3 = {0}; arp_init(&b); chord(&b);
  arp_set_step(&b, 0, 100, 0, 100, ARP_GATE_TIE);
  arp_process(&b, 0, 100, collect, &l3);
  arp_silence(&b, 100, collect, &l3);
  arp_process(&b, 100, 200, collect, &l3);
  static const double e4[][3] = { {0, 1, 60}, {100, 0, 60} };
  check("tie: transport jump silences the tied note", &l3, e4, 2);
}

static void test_pattern_reset_and_length(void) {
  int g[64], n;
  ArpCore a; arp_init(&a); chord(&a);
  arp_set_length(&a, 4);
  arp_set_step(&a, 0, 100, 12, 100, 0);
  n = play(&a, 0, 240 * 2, g);                 /* steps 1,2: 72 64 */
  arp_note_off(&a, 0, 60); arp_note_off(&a, 0, 64); arp_note_off(&a, 0, 67);
  chord(&a);
  n = play(&a, 480, 480 + 240 * 2, g);         /* restarts at step 1: 72 64 */
  { static const int e[] = { 72, 64 }; expect("pattern restarts at step 1 after all keys released", g, n, e, 2); }

  arp_set_length(&a, 1);                       /* only step 1 (+12) now */
  n = play(&a, 960, 960 + 240 * 3, g);         /* order continues 67 60 64, all +12 */
  { static const int e[] = { 79, 72, 76 }; expect("Length changed on the fly", g, n, e, 3); }
}

static void test_step_count(void) {
  ArpCore a; Log l = {0}; arp_init(&a);
  int ok = (a.step_count == 0);
  chord(&a); arp_set_length(&a, 3);
  arp_set_step(&a, 1, 0, 0, 100, 0);           /* a silent step still counts */
  uint32_t seen[5];
  for (int k = 0; k < 5; k++) { arp_process(&a, 240.0 * k, 240.0 * k + 1, collect, &l); seen[k] = a.step_count; }
  ok = ok && seen[0] == 1 && seen[1] == 2 && seen[2] == 3 && seen[3] == 4 && seen[4] == 5;
  arp_note_off(&a, 0, 60); arp_note_off(&a, 0, 64); arp_note_off(&a, 0, 67);
  arp_process(&a, 1200, 1201, collect, &l);
  ok = ok && a.step_count == 0;
  printf("%s: step_count counts steps since restart (1..5), 0 when idle\n", ok ? "PASS" : "FAIL");
  if (!ok) failures++;
}

/* ---------- stage 5: row lengths (polyrhythm) ---------- */

static void test_lane_lengths(void) {
  ArpCore a; Log l = {0}; arp_init(&a);
  arp_note_on(&a, 0, 60, 100);
  arp_set_lane_length(&a, ARP_LANE_SHIFT, 3);
  arp_set_lane_length(&a, ARP_LANE_VEL, 5);
  arp_set_step(&a, 2, 100, 12, 100, 0);        /* shift row: step 3 = +12 */
  arp_set_step(&a, 4, 100, 0, 50, 0);          /* vel row: step 5 = 50% */
  arp_process(&a, 0, 240 * 15, collect, &l);
  int pitch[15], vel[15], k = 0;
  for (int i = 0; i < l.n; i++)
    if ((l.ev[i].st & 0xF0) == 0x90 && k < 15) { pitch[k] = l.ev[i].d1; vel[k] = l.ev[i].d2; k++; }
  int ok = (k == 15);
  for (int i = 0; ok && i < 15; i++) {
    int ep = (i % 3 == 2) ? 72 : 60;           /* every 3rd step */
    int ev = (i % 5 == 4) ? 50 : 100;          /* every 5th step */
    if (pitch[i] != ep || vel[i] != ev) ok = 0;
  }
  printf("%s: Shift length 3 and Vel length 5 run independently (step 15: both)\n", ok ? "PASS" : "FAIL");
  if (!ok) {
    printf("  got:"); for (int i = 0; i < k; i++) printf(" %d/%d", pitch[i], vel[i]); printf("\n");
    failures++;
  }
  ok = ok && pitch[14] == 72 && vel[14] == 50;
  printf("%s: rows meet again after 15 steps (3 x 5)\n", ok ? "PASS" : "FAIL");
  if (!ok) failures++;
}

static void test_lane_prob_gate(void) {
  ArpCore a; Log l = {0}; arp_init(&a);
  arp_note_on(&a, 0, 60, 100);
  arp_set_lane_length(&a, ARP_LANE_PROB, 2);
  arp_set_lane_length(&a, ARP_LANE_GATE, 3);
  arp_set_step(&a, 1, 0, 0, 100, 0);           /* prob row (len 2): step 2 silent */
  arp_set_step(&a, 0, 100, 0, 100, 25);        /* gate row (len 3): step 1 = 25% (60 ticks); prob row step 1 = 100 */
  arp_process(&a, 0, 240 * 6, collect, &l);
  /* steps 1..6: prob idx 0 1 0 1 0 1 -> play on 1,3,5; gate idx 0 1 2 0 1 2 -> step 1: 60, 3: global 120, 5: global 120 */
  static const double exp[][3] = { {0, 1, 60}, {60, 0, 60}, {480, 1, 60}, {600, 0, 60}, {960, 1, 60}, {1080, 0, 60} };
  check("Prob length 2 and Gate length 3 run independently", &l, exp, 6);
}

static void test_lane_reset_and_change(void) {
  int g[64], n;
  ArpCore a; arp_init(&a);
  arp_note_on(&a, 0, 60, 100);
  arp_set_lane_length(&a, ARP_LANE_SHIFT, 3);
  arp_set_step(&a, 0, 100, 1, 100, 0);
  arp_set_step(&a, 1, 100, 2, 100, 0);
  arp_set_step(&a, 2, 100, 3, 100, 0);
  n = play(&a, 0, 240 * 2, g);                 /* 61 62 */
  arp_note_off(&a, 0, 60); arp_note_on(&a, 0, 60, 100);
  n = play(&a, 480, 480 + 240 * 4, g);         /* restart: 61 62 63 61 */
  { static const int e[] = { 61, 62, 63, 61 }; expect("all rows restart at step 1 after keys released", g, n, e, 4); }
  arp_set_lane_length(&a, ARP_LANE_SHIFT, 2);  /* 4 steps since restart: count 4 -> 4 % 2 = 0 -> step 1 */
  n = play(&a, 1440, 1440 + 240 * 3, g);
  { static const int e[] = { 61, 62, 61 }; expect("row length changed on the fly follows count % length", g, n, e, 3); }
}

/* ---------- stage 6a: scale ---------- */

static void test_scale_shift(void) {
  ArpCore a; arp_init(&a);
  struct { int key, scale, note, n, exp; const char* what; } t[] = {
    { 0, ARP_SCALE_CHROMATIC, 60,  5, 65, "Chromatic: any semitone" },
    { 0, ARP_SCALE_CHROMATIC, 60,  1, 61, "Chromatic: C +1 = C#" },
    { 0, ARP_SCALE_MAJOR,     64,  1, 65, "C major: E +1 = F (in scale)" },
    { 0, ARP_SCALE_MAJOR,     60,  2, 62, "C major: C +2 = D" },
    { 0, ARP_SCALE_MAJOR,     60, 12, 72, "C major: C +12 = C" },
    { 0, ARP_SCALE_MAJOR,     60, -1, 59, "C major: C -1 = B" },
    { 0, ARP_SCALE_MAJOR,     60,  1, 62, "C major: C +1 = C# -> tie C/D, toward the shift: D" },
    { 0, ARP_SCALE_MAJOR,     60, -2, 57, "C major: C -2 = A# -> tie A/B, toward the shift: A" },
    { 9, ARP_SCALE_MINOR,     57,  3, 60, "A minor: A +3 = C" },
    { 9, ARP_SCALE_MINOR,     60,  3, 64, "A minor: C +3 = D# -> tie D/E, up: E" },
    { 9, ARP_SCALE_MINOR,     60,  4, 64, "A minor: C +4 = E" },
    { 9, ARP_SCALE_HARM_MINOR,57, 11, 68, "A harmonic minor: A +11 = G#" },
    { 4, ARP_SCALE_PHRYGIAN,  64,  1, 65, "E phrygian: E +1 = F" },
    { 5, ARP_SCALE_LYDIAN,    65,  6, 71, "F lydian: F +6 = B" },
    { 0, ARP_SCALE_MAJ_PENT,  64,  2, 67, "C major pent.: E +2 = F# -> nearest G" },
    { 9, ARP_SCALE_BLUES,     57,  6, 63, "A blues: A +6 = D# (blue note)" },
    { 9, ARP_SCALE_BLUES,     57,  1, 57, "A blues: A +1 = A# -> nearest A" },
    { 0, ARP_SCALE_MAJOR,     61,  0, 61, "off-scale note with Shift 0 stays as played" },
    { 0, ARP_SCALE_MAJOR,     61,  1, 62, "off-scale C# +1 = D" },
    { 0, ARP_SCALE_MAJOR,     61, -1, 60, "off-scale C# -1 = C" },
    { 7, ARP_SCALE_MAJOR,     66,  1, 67, "G major: F# +1 = G (key moves the scale)" },
    { 7, ARP_SCALE_MAJOR,     60,  6, 66, "G major: C +6 = F# (in G major)" },
  };
  int bad = 0;
  for (unsigned k = 0; k < sizeof t / sizeof t[0]; k++) {
    arp_set_scale(&a, t[k].key, t[k].scale);
    int got = arp_shift_note(&a, t[k].note, t[k].n);
    if (got != t[k].exp) { printf("FAIL: %s (got %d, exp %d)\n", t[k].what, got, t[k].exp); bad++; }
  }
  printf("%s: semitone shifts pulled into the scale (%u cases)\n", bad ? "FAIL" : "PASS", (unsigned)(sizeof t / sizeof t[0]));
  failures += bad ? 1 : 0;
}

static void test_scale_in_pattern(void) {
  ArpCore a; Log l = {0}; arp_init(&a);
  arp_set_scale(&a, 9, ARP_SCALE_MINOR);                  /* A minor */
  arp_note_on(&a, 0, 57, 100);                            /* A */
  arp_set_length(&a, 4);
  arp_set_step(&a, 1, 100, 3, 100, 0);                    /* +3 -> C */
  arp_set_step(&a, 2, 100, -1, 100, 0);                   /* -1 = G# -> tie G/A, down: G */
  arp_set_step(&a, 3, 100, 24, 100, 0);                   /* +24 -> A two octaves up */
  arp_process(&a, 0, 240 * 4, collect, &l);
  static const int exp[] = { 57, 60, 55, 81 };
  check_seq("A minor pattern: 0, +3, -1, +24 semitones", &l, exp, 4);

  Log l2 = {0}; arp_init(&a);
  arp_set_scale(&a, 0, ARP_SCALE_MAJOR);
  arp_note_on(&a, 0, 120, 100);
  arp_set_length(&a, 1);
  arp_set_step(&a, 0, 100, 12, 100, 0);                   /* 120 + 12 = 132 > 127 */
  arp_process(&a, 0, 240 * 2, collect, &l2);
  static const int none[] = { 0 };
  check_seq("shift above 127 makes the step silent", &l2, none, 0);

  Log l3 = {0}; arp_init(&a); chord(&a);                  /* Chromatic default = old behaviour */
  arp_set_length(&a, 4);
  arp_set_step(&a, 2, 100, 12, 100, 0);
  arp_set_step(&a, 3, 100, -5, 100, 0);
  arp_process(&a, 0, 240 * 8, collect, &l3);
  static const int e3[] = { 60, 64, 79, 55, 64, 67, 72, 59 };
  check_seq("Chromatic (default) keeps semitone shifts", &l3, e3, 8);

  arp_set_scale(&a, -3, 99);
  printf("%s: key and scale are clamped\n", a.key == 0 && a.scale == ARP_SCALE_BLUES && !a.key_auto ? "PASS" : "FAIL");
  if (!(a.key == 0 && a.scale == ARP_SCALE_BLUES && !a.key_auto)) failures++;
  arp_set_scale(&a, 15, 0);
  printf("%s: key 12 and above is Auto\n", a.key_auto ? "PASS" : "FAIL");
  failures += !a.key_auto;
}

/* ---------- Loop order and note mutation ---------- */

static void loop_core(ArpCore* a, int len, int notes) {
  arp_init(a); chord(a);
  arp_set_params(a, ARP_RATE_16, 50, ARP_LOOP, 1);
  arp_set_loop(a, notes, len);
  static const uint16_t fixed[4] = { 0, 0x8000, 0xFFFF, 0x5555 };   /* chord of 3: 0, 1, 2, 0 */
  for (int i = 0; i < ARP_MAX_STEPS; i++) a->loop[i] = fixed[i % 4];
}

static void test_loop_frozen(void) {
  ArpCore a; int g[64], n;
  loop_core(&a, 4, 0);
  n = play(&a, 0, 240 * 8, g);
  static const int e1[] = { 60, 64, 67, 60, 60, 64, 67, 60 };
  expect("Loop len 4, Notes 0%: positions low/mid/high/low repeat", g, n, e1, 8);

  loop_core(&a, 2, 0);
  n = play(&a, 0, 240 * 6, g);
  static const int e2[] = { 60, 64, 60, 64, 60, 64 };
  expect("Loop len 2", g, n, e2, 6);

  loop_core(&a, 4, 0);   /* new chord D-F-A-C: same shape on the new notes */
  arp_note_off(&a, 0, 60); arp_note_off(&a, 0, 64); arp_note_off(&a, 0, 67);
  arp_note_on(&a, 0, 62, 100); arp_note_on(&a, 0, 65, 100); arp_note_on(&a, 0, 69, 100); arp_note_on(&a, 0, 72, 100);
  n = play(&a, 0, 240 * 4, g);
  static const int e3[] = { 62, 69, 72, 65 };   /* 4 notes: 0, 2, 3, 1 */
  expect("Loop keeps its shape on a new chord (4 notes)", g, n, e3, 4);

  loop_core(&a, 4, 0);
  arp_set_params(&a, ARP_RATE_16, 50, ARP_LOOP, 2);   /* 6 notes: 0, 3, 5, 1 */
  n = play(&a, 0, 240 * 4, g);
  static const int e4[] = { 60, 72, 79, 64 };
  expect("Loop over 2 octaves", g, n, e4, 4);

  loop_core(&a, 4, 0);
  arp_reset_mutation(&a);
  int changed = a.loop[0] != 0 || a.loop[1] != 0x8000 || a.loop[2] != 0xFFFF || a.loop[3] != 0x5555;
  n = play(&a, 0, 240 * 12, g);
  int rep = 1;
  for (int i = 4; i < n; i++) if (g[i] != g[i - 4]) rep = 0;
  printf("%s: reset makes a new loop, still repeating every 4 steps\n", changed && rep && n == 12 ? "PASS" : "FAIL");
  failures += !(changed && rep && n == 12);
}

static void test_loop_mutation(void) {
  ArpCore a; int g[512], n;
  /* 100 %: a new position on every step, only chord notes */
  loop_core(&a, 4, 100);
  n = play(&a, 0, 240 * 200, g);
  int ok = (n == 200), periodic = 1, hits[128] = {0};
  for (int i = 0; i < n; i++) { if (g[i] != 60 && g[i] != 64 && g[i] != 67) ok = 0; else hits[g[i]]++; }
  for (int i = 4; i < n; i++) if (g[i] != g[i - 4]) periodic = 0;
  printf("%s: Notes 100%%: random chord notes, all used, not a 4-step loop\n",
         ok && hits[60] && hits[64] && hits[67] && !periodic ? "PASS" : "FAIL");
  failures += !(ok && hits[60] && hits[64] && hits[67] && !periodic);

  /* 25 %: the loop drifts slowly: compare each pass of the loop with the previous one */
  loop_core(&a, 8, 25);
  arp_set_params(&a, ARP_RATE_16, 50, ARP_LOOP, 4);   /* 12 notes: changes are rarely invisible */
  n = play(&a, 0, 240 * 400, g);
  int diff = 0;
  for (int i = 8; i < n; i++) diff += g[i] != g[i - 8];
  double f = diff / (double)(n - 8);
  printf("%s: Notes 25%%: about a quarter of the steps change per pass (%.2f)\n", f > 0.15 && f < 0.32 ? "PASS" : "FAIL", f);
  failures += !(f > 0.15 && f < 0.32);

  /* then back to 0 %: frozen in its current state */
  arp_set_loop(&a, 0, 8);
  n = play(&a, 240 * 400, 240 * 432, g);
  int rep = n == 32;
  for (int i = 8; i < n; i++) if (g[i] != g[i - 8]) rep = 0;
  printf("%s: back to Notes 0%%: the grown loop is frozen\n", rep ? "PASS" : "FAIL");
  failures += !rep;

  /* the mutation generator doesn't change the other orders */
  Log l1 = {0}, l2 = {0};
  random_run(&l1, 42);
  ArpCore b; arp_init(&b); chord(&b); arp_seed(&b, 42);
  arp_set_params(&b, ARP_RATE_16, 50, ARP_RANDOM, 2);
  arp_set_loop(&b, 100, 3); arp_seed_mutation(&b, 99); arp_reset_mutation(&b);
  arp_process(&b, 0, 240 * 200, collect, &l2);
  printf("%s: Random order is not affected by Loop settings\n", same_logs(&l1, &l2) ? "PASS" : "FAIL");
  failures += !same_logs(&l1, &l2);
}

/* ---------- step mutation ---------- */

static void test_step_mutation(void) {
  ArpCore a; Log l = {0};
  /* single key C4, vel 100, C major, everything drifts */
  arp_init(&a); arp_note_on(&a, 0, 60, 100);
  arp_set_scale(&a, 0, ARP_SCALE_MAJOR);
  arp_set_mut_steps(&a, 100);
  arp_process(&a, 0, 240 * 300, collect, &l);
  int ok_p = 1, ok_v = 1, ok_g = 1, moved_p = 0, moved_v = 0, moved_g = 0, nons = 0;
  for (int i = 0; i < l.n; i++) {
    if ((l.ev[i].st & 0xF0) != 0x90) continue;
    nons++;
    int p = l.ev[i].d1, v = l.ev[i].d2;
    /* C major, at most 3 scale notes from C4: F3 .. F4 */
    if (!(p == 53 || p == 55 || p == 57 || p == 59 || p == 60 || p == 62 || p == 64 || p == 65)) ok_p = 0;
    if (p != 60) moved_p = 1;
    if (v < 40 || v > 127) ok_v = 0;          /* 100 % +- 3 x 20 % */
    if (v != 100) moved_v = 1;
    for (int j = i + 1; j < l.n; j++)
      if ((l.ev[j].st & 0xF0) == 0x80 && l.ev[j].d1 == p) {
        double len = l.ev[j].tick - l.ev[i].tick;   /* global 50 % = 120 ticks, +- 4 x 5 % */
        if (len < 72 - 1e-6 || len > 168 + 1e-6) ok_g = 0;
        if (fabs(len - 120) > 1e-6) moved_g = 1;
        break;
      }
  }
  int lim_ok = 1;
  for (int i = 0; i < ARP_MAX_STEPS; i++)
    for (int r = 0; r < ARP_NLANES; r++) {
      int lim = r == ARP_LANE_GATE ? ARP_MUT_MAX_GATE : ARP_MUT_MAX;
      if (a.dev[i][r] < -lim || a.dev[i][r] > lim) lim_ok = 0;
    }
  printf("%s: Steps 100%%: pitch stays in the scale within 3 notes, and moves\n", ok_p && moved_p ? "PASS" : "FAIL");
  printf("%s: Steps 100%%: velocity within +-60%%, and moves\n", ok_v && moved_v ? "PASS" : "FAIL");
  printf("%s: Steps 100%%: gate within +-4 notches of 5%%, and moves\n", ok_g && moved_g ? "PASS" : "FAIL");
  printf("%s: drift never exceeds its limits\n", lim_ok ? "PASS" : "FAIL");
  printf("%s: drifting prob makes some steps silent (%d notes of 300)\n", nons < 300 && nons > 150 ? "PASS" : "FAIL", nons);
  failures += !(ok_p && moved_p) + !(ok_v && moved_v) + !(ok_g && moved_g) + !lim_ok + !(nons < 300 && nons > 150);

  /* back to 0 %: frozen */
  int8_t saved[ARP_MAX_STEPS][ARP_NLANES];
  memcpy(saved, a.dev, sizeof saved);
  arp_set_mut_steps(&a, 0);
  Log l2 = {0};
  arp_process(&a, 240 * 300, 240 * 364, collect, &l2);
  int frozen = !memcmp(saved, a.dev, sizeof saved), any = 0;
  for (int i = 0; i < ARP_MAX_STEPS; i++) for (int r = 0; r < ARP_NLANES; r++) any |= a.dev[i][r];
  printf("%s: Steps 0%%: the grown drift is kept\n", frozen && any ? "PASS" : "FAIL");
  failures += !(frozen && any);

  /* editing a step clears that row of that step; reset clears all */
  int i0 = -1, r0 = -1;
  for (int i = 0; i < ARP_MAX_STEPS && i0 < 0; i++) for (int r = 0; r < ARP_NLANES; r++) if (a.dev[i][r]) { i0 = i; r0 = r; break; }
  int other = 0;
  for (int i = 0; i < ARP_MAX_STEPS; i++) for (int r = 0; r < ARP_NLANES; r++) if ((i != i0 || r != r0) && a.dev[i][r]) other = 1;
  arp_clear_mutation(&a, i0, r0);
  int cleared = a.dev[i0][r0] == 0, kept = 0;
  for (int i = 0; i < ARP_MAX_STEPS; i++) for (int r = 0; r < ARP_NLANES; r++) kept |= a.dev[i][r];
  printf("%s: clearing one step row keeps the others\n", cleared && (kept || !other) ? "PASS" : "FAIL");
  failures += !(cleared && (kept || !other));
  arp_reset_mutation(&a);
  any = 0;
  for (int i = 0; i < ARP_MAX_STEPS; i++) for (int r = 0; r < ARP_NLANES; r++) any |= a.dev[i][r];
  printf("%s: reset clears all drift\n", !any ? "PASS" : "FAIL");
  failures += any;
}

static void test_mutation_details(void) {
  ArpCore a; int g[64], n;
  /* shift drift in Chromatic is in semitones, applied after the drawn shift */
  arp_init(&a); arp_note_on(&a, 0, 60, 100);
  arp_set_length(&a, 1);
  arp_set_step(&a, 0, 100, 7, 100, ARP_GATE_GLOBAL);
  a.dev[0][ARP_LANE_SHIFT] = -2;
  n = play(&a, 0, 240 * 2, g);
  static const int e1[] = { 65, 65 };
  expect("Chromatic: shift +7 with drift -2 -> F", g, n, e1, 2);
  /* in A minor the drift walks scale notes */
  arp_init(&a); arp_note_on(&a, 0, 57, 100);
  arp_set_length(&a, 1); arp_set_scale(&a, 9, ARP_SCALE_MINOR);
  a.dev[0][ARP_LANE_SHIFT] = 3;
  n = play(&a, 0, 240, g);
  static const int e2[] = { 62 };   /* A +3 scale notes: B C D */
  expect("A minor: drift +3 from A -> D", g, n, e2, 1);
  a.dev[0][ARP_LANE_SHIFT] = -2;
  n = play(&a, 240, 480, g);
  static const int e3[] = { 53 };   /* A -2: G F */
  expect("A minor: drift -2 from A -> F", g, n, e3, 1);

  /* gate drift: Tie can't go higher, Global counts from the global gate */
  arp_init(&a);
  if (arp_drifted(&a, ARP_LANE_GATE, ARP_GATE_TIE, 1) != ARP_GATE_TIE || arp_drifted(&a, ARP_LANE_GATE, ARP_GATE_TIE, -1) != 100 ||
      arp_drifted(&a, ARP_LANE_GATE, ARP_GATE_GLOBAL, 2) != 60 || arp_drifted(&a, ARP_LANE_GATE, 5, -1) != 5 ||
      arp_drifted(&a, ARP_LANE_PROB, 100, 1) != 100 || arp_drifted(&a, ARP_LANE_PROB, 100, -2) != 60 ||
      arp_drifted(&a, ARP_LANE_VEL, 190, 1) != 200) {
    printf("FAIL: drifted values (gate Tie/Global/5%%, prob, vel clamps)\n"); failures++;
  } else printf("PASS: drifted values (gate Tie/Global/5%%, prob, vel clamps)\n");

  /* a clamped direction is not chosen: prob 100 can only go down */
  arp_init(&a); arp_seed_mutation(&a, 5);
  int ok = 1;
  for (int k = 0; k < 50; k++) { a.dev[3][ARP_LANE_PROB] = 0; arp_mutate_step(&a, 3, ARP_LANE_PROB); if (a.dev[3][ARP_LANE_PROB] != -1) ok = 0; }
  a.dev[3][ARP_LANE_PROB] = -3;
  arp_mutate_step(&a, 3, ARP_LANE_PROB);
  printf("%s: prob 100%% drifts only down; at the limit only back\n", ok && a.dev[3][ARP_LANE_PROB] == -2 ? "PASS" : "FAIL");
  failures += !(ok && a.dev[3][ARP_LANE_PROB] == -2);

  /* Steps 0 does not touch the mutation generator: Loop stays as it was */
  ArpCore b, c;
  loop_core(&b, 4, 30); loop_core(&c, 4, 30);
  arp_set_mut_steps(&c, 0);
  int gb[64], gc[64];
  int nb = play(&b, 0, 240 * 40, gb), nc = play(&c, 0, 240 * 40, gc);
  printf("%s: Steps 0%% leaves Loop mutation unchanged\n", nb == nc && !memcmp(gb, gc, sizeof(int) * nb) ? "PASS" : "FAIL");
  failures += !(nb == nc && !memcmp(gb, gc, sizeof(int) * nb));
}

/* ---------- transpose ---------- */

static void test_transpose(void) {
  ArpCore a; int g[64], n;
  arp_init(&a); chord(&a);
  arp_set_transpose(&a, 1);
  n = play(&a, 0, 240 * 3, g);
  static const int e1[] = { 72, 76, 79 };
  expect("transpose +1 octave", g, n, e1, 3);
  arp_set_transpose(&a, -2);
  n = play(&a, 240 * 3, 240 * 6, g);
  static const int e2[] = { 36, 40, 43 };
  expect("transpose -2 octaves, from the next step, order continues", g, n, e2, 3);
  arp_set_transpose(&a, 9);
  printf("%s: transpose is clamped to +-2\n", a.transpose == 2 ? "PASS" : "FAIL");
  failures += a.transpose != 2;

  /* after shift and scale: A minor, shift +3 from A -> C, +1 octave -> C5 */
  arp_init(&a); arp_note_on(&a, 0, 57, 100);
  arp_set_length(&a, 1); arp_set_scale(&a, 9, ARP_SCALE_MINOR);
  arp_set_step(&a, 0, 100, 3, 100, ARP_GATE_GLOBAL);
  arp_set_transpose(&a, 1);
  n = play(&a, 0, 240, g);
  static const int e3[] = { 72 };
  expect("transpose applies after shift and scale", g, n, e3, 1);

  /* out of range: silent */
  arp_init(&a); arp_note_on(&a, 0, 110, 100); arp_note_on(&a, 0, 10, 100);
  arp_set_transpose(&a, 2);
  n = play(&a, 0, 240 * 2, g);
  static const int e4[] = { 34 };   /* 10+24; 110+24 > 127 is silent */
  expect("notes transposed above 127 are silent", g, n, e4, 1);

  /* tie: the same transposed pitch continues */
  arp_init(&a); arp_note_on(&a, 0, 60, 100);
  arp_set_length(&a, 2);
  arp_set_step(&a, 0, 100, 0, 100, ARP_GATE_TIE);
  arp_set_transpose(&a, -1);
  Log l = {0};
  arp_process(&a, 0, 240 * 2, collect, &l);
  static const double e5[][3] = { {0, 1, 48}, {360, 0, 48} };
  check("tie on a transposed note continues it", &l, e5, 2);
}

/* ---------- repeats ---------- */

static void test_repeats(void) {
  ArpCore a; int g[128], n;
  arp_init(&a); chord(&a);
  arp_set_repeats(&a, 2, ARP_RPT_SAME);
  n = play(&a, 0, 240 * 8, g);
  static const int e1[] = { 60, 60, 64, 64, 67, 67, 60, 60 };
  expect("Repeats 2, Up: C C E E G G C C", g, n, e1, 8);

  arp_init(&a); chord(&a);
  arp_set_params(&a, ARP_RATE_16, 50, ARP_UPDOWN, 1);
  arp_set_repeats(&a, 3, ARP_RPT_SAME);
  n = play(&a, 0, 240 * 12, g);
  static const int e2[] = { 60, 60, 60, 64, 64, 64, 67, 67, 67, 64, 64, 64 };
  expect("Repeats 3, Up-Down: the order waits during repeats", g, n, e2, 12);

  /* Run up/down in C major (scale notes), chromatic = semitones */
  arp_init(&a); chord(&a); arp_set_scale(&a, 0, ARP_SCALE_MAJOR);
  arp_set_repeats(&a, 3, ARP_RPT_RUN_UP);
  n = play(&a, 0, 240 * 9, g);
  static const int e3[] = { 60, 62, 64, 64, 65, 67, 67, 69, 71 };
  expect("Run up, C major: C D E | E F G | G A B", g, n, e3, 9);
  arp_init(&a); chord(&a); arp_set_scale(&a, 0, ARP_SCALE_MAJOR);
  arp_set_repeats(&a, 3, ARP_RPT_RUN_DOWN);
  n = play(&a, 0, 240 * 9, g);
  static const int e4[] = { 60, 59, 57, 64, 62, 60, 67, 65, 64 };
  expect("Run down, C major: C B A | E D C | G F E", g, n, e4, 9);
  arp_init(&a); chord(&a);
  arp_set_repeats(&a, 2, ARP_RPT_RUN_UP);
  n = play(&a, 0, 240 * 4, g);
  static const int e5[] = { 60, 61, 64, 65 };
  expect("Run up, chromatic: semitones", g, n, e5, 4);

  arp_init(&a); chord(&a);
  arp_set_repeats(&a, 4, ARP_RPT_OCT);
  n = play(&a, 0, 240 * 8, g);
  static const int e6[] = { 60, 72, 60, 72, 64, 76, 64, 76 };
  expect("Octave: every other repeat an octave up", g, n, e6, 8);

  /* Echo: 100, 70, 49, 34 % of the key velocity */
  arp_init(&a); arp_note_on(&a, 0, 60, 100);
  arp_set_repeats(&a, 4, ARP_RPT_ECHO);
  Log l = {0};
  arp_process(&a, 0, 240 * 5, collect, &l);
  int v[8], k = 0;
  for (int i = 0; i < l.n; i++) if ((l.ev[i].st & 0xF0) == 0x90 && k < 8) v[k++] = l.ev[i].d2;
  int ok = k == 5 && v[0] == 100 && v[1] == 70 && v[2] == 49 && v[3] == 34 && v[4] == 100;
  printf("%s: Echo: each repeat quieter, a new note at full velocity\n", ok ? "PASS" : "FAIL");
  if (!ok) { printf("  got:"); for (int i = 0; i < k; i++) printf(" %d", v[i]); printf("\n"); failures++; }

  /* pattern rows advance on every repeat: vel row 50 % on step 2 hits the repeat */
  arp_init(&a); arp_note_on(&a, 0, 60, 100); arp_note_on(&a, 0, 64, 100);
  arp_set_length(&a, 2);
  arp_set_step(&a, 1, 100, 0, 50, ARP_GATE_GLOBAL);
  arp_set_repeats(&a, 2, ARP_RPT_SAME);
  l.n = 0;
  arp_process(&a, 0, 240 * 4, collect, &l);
  k = 0;
  int p[8];
  for (int i = 0; i < l.n; i++) if ((l.ev[i].st & 0xF0) == 0x90 && k < 8) { p[k] = l.ev[i].d1; v[k] = l.ev[i].d2; k++; }
  ok = k == 4 && p[0] == 60 && p[1] == 60 && p[2] == 64 && p[3] == 64 && v[0] == 100 && v[1] == 50 && v[2] == 100 && v[3] == 50;
  printf("%s: pattern rows advance on repeats\n", ok ? "PASS" : "FAIL");
  failures += !ok;

  /* a silent step (prob 0) still counts as a repeat */
  arp_init(&a); chord(&a);
  arp_set_length(&a, 2);
  arp_set_step(&a, 1, 0, 0, 100, ARP_GATE_GLOBAL);
  arp_set_repeats(&a, 2, ARP_RPT_SAME);
  n = play(&a, 0, 240 * 6, g);
  static const int e7[] = { 60, 64, 67 };
  expect("a silent repeat is still a repeat", g, n, e7, 3);

  /* Loop: repeats do not skip loop positions and do not mutate it */
  loop_core(&a, 4, 0);
  arp_set_repeats(&a, 2, ARP_RPT_SAME);
  n = play(&a, 0, 240 * 8, g);
  static const int e8[] = { 60, 60, 64, 64, 67, 67, 60, 60 };
  expect("Loop with repeats 2: every loop position, each twice", g, n, e8, 8);
  loop_core(&a, 4, 100);
  arp_set_repeats(&a, 3, ARP_RPT_SAME);
  play(&a, 0, 240 * 3, g);
  uint32_t r1 = a.mrng;
  loop_core(&a, 4, 100);
  arp_set_repeats(&a, 1, ARP_RPT_SAME);
  play(&a, 0, 240, g);
  printf("%s: Loop mutation happens once per note, not per repeat\n", a.mrng == r1 ? "PASS" : "FAIL");
  failures += a.mrng != r1;

  /* Rnd 3: counts 1..3, reproducible from the seed, all three occur */
  int runs[3][64], nr[3];
  for (int t = 0; t < 2; t++) {
    arp_init(&a); arp_note_on(&a, 0, 60, 100); arp_note_on(&a, 0, 72, 100);
    arp_seed_mutation(&a, 12345);
    arp_set_repeats(&a, ARP_RPT_RND3, ARP_RPT_SAME);
    nr[t] = play(&a, 0, 240 * 64, runs[t]);
  }
  int seen[6] = {0}; ok = nr[0] == 64 && nr[0] == nr[1] && !memcmp(runs[0], runs[1], sizeof(int) * 64);
  for (int i = 0; ok && i < 64;) {
    int j = i; while (j < 64 && runs[0][j] == runs[0][i]) j++;
    int len = j - i;
    if (j < 64 && (len < 1 || len > 3)) ok = 0;   /* the last run may be cut by the end of the test */
    if (j < 64) seen[len] = 1;
    i = j;
  }
  ok = ok && seen[1] && seen[2] && seen[3];
  printf("%s: Rnd 3: 1..3 repeats per note, reproducible, all counts occur\n", ok ? "PASS" : "FAIL");
  failures += !ok;

  /* the repeated note is released (latch off): the next note starts at once */
  arp_init(&a); chord(&a);
  arp_set_repeats(&a, 4, ARP_RPT_SAME);
  n = play(&a, 0, 240 * 2, g);
  arp_note_off(&a, 0, 60);
  n = play(&a, 240 * 2, 240 * 3, g);
  static const int e9[] = { 64 };
  expect("released repeated note: the order moves on", g, n, e9, 1);

  /* fewer repeats while running: the current run is cut */
  arp_init(&a); chord(&a);
  arp_set_repeats(&a, 5, ARP_RPT_SAME);
  play(&a, 0, 240, g);
  arp_set_repeats(&a, 2, ARP_RPT_SAME);
  n = play(&a, 240, 240 * 4, g);
  static const int e10[] = { 60, 64, 64 };
  expect("Repeats lowered from 5 to 2 mid-run", g, n, e10, 3);

  /* run with transpose and out of range */
  arp_init(&a); arp_note_on(&a, 0, 126, 100);
  arp_set_repeats(&a, 3, ARP_RPT_RUN_UP);
  n = play(&a, 0, 240 * 3, g);
  static const int e11[] = { 126, 127 };
  expect("Run up above 127 is silent", g, n, e11, 2);
}

/* ---------- groove ---------- */

/* note-on ticks of a log */
static int on_ticks(const Log* l, double* out, int* vel) {
  int k = 0;
  for (int i = 0; i < l->n; i++) if ((l->ev[i].st & 0xF0) == 0x90) { out[k] = l->ev[i].tick; if (vel) vel[k] = l->ev[i].d2; k++; }
  return k;
}

static void expect_ticks(const char* name, const double* got, int n, const double* exp, int nexp) {
  int ok = n == nexp;
  for (int i = 0; ok && i < n; i++) if (fabs(got[i] - exp[i]) > 1e-6) ok = 0;
  printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) {
    printf("  got:"); for (int i = 0; i < n; i++) printf(" %.1f", got[i]);
    printf("\n  exp:"); for (int i = 0; i < nexp; i++) printf(" %.1f", exp[i]);
    printf("\n"); failures++;
  }
}

static void groove_run(Log* l, int swing, int feel, int drunk, int accent, int nsteps, const double* chunks, int nch) {
  ArpCore a; arp_init(&a); arp_note_on(&a, 0, 60, 100);
  arp_set_groove(&a, swing, feel, drunk, accent);
  arp_seed_mutation(&a, 777);
  double t = 0, end = 240.0 * nsteps; int i = 0;
  while (t < end) {
    double e = nch ? t + chunks[i++ % nch] : end;
    if (e > end) e = end;
    arp_process(&a, t, e, collect, l);
    t = e;
  }
}

static void test_groove(void) {
  Log l; double tk[256]; int vel[256], n;
  /* swing 75 % at 1/16: full delay = 120 ticks */
  l.n = 0; groove_run(&l, 75, ARP_FEEL_PAIRS, 0, 0, 4, NULL, 0);
  n = on_ticks(&l, tk, NULL);
  static const double e1[] = { 0, 360, 480, 840 };
  expect_ticks("Pairs, swing 75 %: every 2nd step +120", tk, n, e1, 4);
  l.n = 0; groove_run(&l, 75, ARP_FEEL_FOURS, 0, 0, 4, NULL, 0);
  n = on_ticks(&l, tk, NULL);
  static const double e2[] = { 0, 300, 600, 780 };
  expect_ticks("Fours: 0 1/2 1 1/2", tk, n, e2, 4);
  l.n = 0; groove_run(&l, 75, ARP_FEEL_DRAG, 0, 0, 5, NULL, 0);
  n = on_ticks(&l, tk, NULL);
  static const double e3[] = { 0, 280, 560, 840, 960 };
  expect_ticks("Drag: 0 1/3 2/3 1, back on the beat", tk, n, e3, 5);
  l.n = 0; groove_run(&l, 75, ARP_FEEL_THREES, 0, 0, 6, NULL, 0);
  n = on_ticks(&l, tk, NULL);
  static const double e4[] = { 0, 360, 480, 720, 1080, 1200 };
  expect_ticks("Threes: 0 1 0", tk, n, e4, 6);
  l.n = 0; groove_run(&l, 50, ARP_FEEL_PAIRS, 0, 0, 4, NULL, 0);
  n = on_ticks(&l, tk, NULL);
  static const double e5[] = { 0, 240, 480, 720 };
  expect_ticks("swing 50 %: straight", tk, n, e5, 4);
  l.n = 0; groove_run(&l, 66, ARP_FEEL_PAIRS, 0, 0, 2, NULL, 0);
  n = on_ticks(&l, tk, NULL);
  static const double e5b[] = { 0, 240 + 240 * 0.32 };
  expect_ticks("swing 66 %: +0.32 step", tk, n, e5b, 2);

  /* the gate of a delayed note: 50 % of the step from its own start */
  l.n = 0; groove_run(&l, 75, ARP_FEEL_PAIRS, 0, 0, 3, NULL, 0);
  static const double e6[][3] = { {0, 1, 60}, {120, 0, 60}, {360, 1, 60}, {480, 0, 60}, {480, 1, 60}, {600, 0, 60} };
  check("delayed note keeps its gate", &l, e6, 6);

  /* chunked processing: a step delayed past the block end plays once, same as one block */
  Log ref = {0}, c = {0};
  groove_run(&ref, 72, ARP_FEEL_DRAG, 60, 50, 64, NULL, 0);
  const double ch1[] = { 1 }, ch2[] = { 100, 37.5, 333, 7 }, ch3[] = { 239.9, 0.2 };
  int ok = 1;
  c.n = 0; groove_run(&c, 72, ARP_FEEL_DRAG, 60, 50, 64, ch1, 1); ok = ok && same_logs(&ref, &c);
  c.n = 0; groove_run(&c, 72, ARP_FEEL_DRAG, 60, 50, 64, ch2, 4); ok = ok && same_logs(&ref, &c);
  c.n = 0; groove_run(&c, 72, ARP_FEEL_DRAG, 60, 50, 64, ch3, 2); ok = ok && same_logs(&ref, &c);
  n = on_ticks(&ref, tk, NULL);
  ok = ok && n == 64;
  printf("%s: groove: any block sizes give the same events, every step once\n", ok ? "PASS" : "FAIL");
  failures += !ok;

  /* Drunk: late only, within 1/4 step, smooth, reproducible */
  l.n = 0; groove_run(&l, 50, ARP_FEEL_PAIRS, 100, 0, 200, NULL, 0);
  n = on_ticks(&l, tk, NULL);
  double mx = 0, maxjump = 0, prev = 0; ok = n == 200;
  for (int i = 0; ok && i < n; i++) {
    double d = tk[i] - 240.0 * i;
    if (d < -1e-9 || d > 60 + 1e-9) ok = 0;
    if (d > mx) mx = d;
    if (i && fabs(d - prev) > maxjump) maxjump = fabs(d - prev);
    prev = d;
  }
  ok = ok && mx > 30 && maxjump <= 0.2 * 60 + 1e-6;
  printf("%s: Drunk 100 %%: 0..60 ticks late (max %.1f), changes at most 12 per step (max %.1f)\n",
         ok ? "PASS" : "FAIL", mx, maxjump);
  failures += !ok;
  c.n = 0; groove_run(&c, 50, ARP_FEEL_PAIRS, 100, 0, 200, NULL, 0);
  printf("%s: Drunk is reproducible from the mutation seed\n", same_logs(&l, &c) ? "PASS" : "FAIL");
  failures += !same_logs(&l, &c);
  l.n = 0; groove_run(&l, 75, ARP_FEEL_PAIRS, 100, 0, 200, NULL, 0);
  n = on_ticks(&l, tk, NULL);
  ok = n == 200;
  for (int i = 1; ok && i < n; i++) if (tk[i] <= tk[i - 1]) ok = 0;
  printf("%s: swing 75 %% + Drunk 100 %%: notes stay in order\n", ok ? "PASS" : "FAIL");
  failures += !ok;

  /* Accent: by the feel shape, works without swing too; velocity and gate */
  l.n = 0; groove_run(&l, 50, ARP_FEEL_FOURS, 0, 100, 4, NULL, 0);
  n = on_ticks(&l, tk, vel);
  ok = n == 4 && vel[0] == 127 && vel[1] == 79 && vel[2] == 30 && vel[3] == 79;
  printf("%s: Accent 100 %%, Fours: velocity 127 79 30 79\n", ok ? "PASS" : "FAIL");
  if (!ok) { printf("  got:"); for (int i = 0; i < n; i++) printf(" %d", vel[i]); printf("\n"); failures++; }
  static const double eg[][3] = { {0, 1, 60}, {120, 0, 60}, {240, 1, 60}, {330, 0, 60}, {480, 1, 60}, {540, 0, 60},
                                  {720, 1, 60}, {810, 0, 60} };
  check("Accent 100 %, Fours: gate 50 % / 37.5 % / 25 % / 37.5 % of the step", &l, eg, 8);
  l.n = 0; groove_run(&l, 50, ARP_FEEL_PAIRS, 0, 50, 2, NULL, 0);
  n = on_ticks(&l, tk, vel);
  ok = n == 2 && vel[0] == 113 && vel[1] == 65;
  printf("%s: Accent 50 %%, Pairs: velocity 113 65\n", ok ? "PASS" : "FAIL");
  if (!ok) { printf("  got:"); for (int i = 0; i < n; i++) printf(" %d", vel[i]); printf("\n"); failures++; }
  {   /* a tie on a weak step stays a tie */
    ArpCore a; arp_init(&a); arp_note_on(&a, 0, 60, 100); arp_note_on(&a, 0, 64, 100);
    arp_set_length(&a, 1); arp_set_step(&a, 0, 100, 0, 100, ARP_GATE_TIE);
    arp_set_groove(&a, 50, ARP_FEEL_PAIRS, 0, 100);
    Log x = {0};
    arp_process(&a, 0, 480, collect, &x);
    static const double et[][3] = { {0, 1, 60}, {240, 1, 64}, {240, 0, 60} };
    check("Accent keeps ties", &x, et, 3);
  }

  /* relocation back: steps play again */
  {
    ArpCore a; arp_init(&a); arp_note_on(&a, 0, 60, 100);
    arp_set_groove(&a, 75, ARP_FEEL_PAIRS, 0, 0);
    Log x = {0};
    arp_process(&a, 0, 960, collect, &x);
    arp_silence(&a, 960, collect, &x);
    x.n = 0;
    arp_process(&a, 0, 480, collect, &x);
    n = on_ticks(&x, tk, NULL);
    static const double e7[] = { 0, 360 };
    expect_ticks("relocation back to 0: the steps play again", tk, n, e7, 2);
    /* jump forward into the middle of a delayed step: it still plays */
    x.n = 0;
    arp_silence(&a, 480, collect, &x);
    arp_process(&a, 1700, 1920, collect, &x);
    n = on_ticks(&x, tk, NULL);
    static const double e8[] = { 1800 };   /* step 1680 delayed to 1800 */
    expect_ticks("jump into a swing delay: the delayed step plays", tk, n, e8, 1);
  }

  /* tie with swing: legato into the delayed note */
  {
    ArpCore a; arp_init(&a); arp_note_on(&a, 0, 60, 100); arp_note_on(&a, 0, 64, 100);
    arp_set_length(&a, 1); arp_set_step(&a, 0, 100, 0, 100, ARP_GATE_TIE);
    arp_set_groove(&a, 75, ARP_FEEL_PAIRS, 0, 0);
    Log x = {0};
    arp_process(&a, 0, 480, collect, &x);
    static const double e9[][3] = { {0, 1, 60}, {360, 1, 64}, {360, 0, 60} };
    check("tie with swing: the next note starts at its delayed time", &x, e9, 3);
  }
}

/* ---------- passing notes, key auto ---------- */

static int in_set(int v, const int* set, int n) { for (int i = 0; i < n; i++) if (set[i] == v) return 1; return 0; }

/* every new chord note gets a passing note: pairs (passing, target); checks passing is in the allowed set
 * for its target, returns which allowed entries were seen (bit mask) */
static int check_pairs(const char* name, ArpCore* a, int npairs, const int targets[3], const int allowed[3][4], int nallowed) {
  int g[512], n = play(a, 0, 240.0 * 2 * npairs, g), ok = n == 2 * npairs, seen = 0;
  for (int i = 0; ok && i < npairs; i++) {
    const int t = targets[i % 3], p = g[2 * i];
    if (g[2 * i + 1] != t) { ok = 0; break; }
    int k = -1;
    for (int j = 0; j < nallowed; j++) if (allowed[i % 3][j] == p) k = j;
    if (k < 0) ok = 0; else seen |= 1 << k;
  }
  ok = ok && seen == (1 << nallowed) - 1;
  printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) { printf("  got:"); for (int i = 0; i < n && i < 24; i++) printf(" %d", g[i]); printf("\n"); failures++; }
  return seen;
}

static void test_passing(void) {
  ArpCore a; int g[128], n;
  static const int tg[3] = { 60, 64, 67 };

  arp_init(&a); chord(&a); arp_set_scale(&a, 0, ARP_SCALE_MAJOR);
  arp_set_passing(&a, 100, ARP_PASS_APPROACH);
  n = play(&a, 0, 240 * 8, g);
  static const int e1[] = { 59, 60, 63, 64, 66, 67, 59, 60 };
  expect("Approach 100 %: a semitone below, then the chord note", g, n, e1, 8);

  arp_init(&a); chord(&a); arp_set_scale(&a, 0, ARP_SCALE_MAJOR); arp_seed_mutation(&a, 5);
  arp_set_passing(&a, 100, ARP_PASS_NEIGHBOR);
  static const int nb[3][4] = { { 62, 59 }, { 65, 62 }, { 69, 65 } };
  check_pairs("Neighbor, C major: next scale note up or down, then resolves", &a, 60, tg, nb, 2);

  arp_init(&a); chord(&a); arp_set_scale(&a, 0, ARP_SCALE_MAJOR); arp_seed_mutation(&a, 6);
  arp_set_passing(&a, 100, ARP_PASS_LEAP);
  static const int lp[3][4] = { { 64, 65, 57, 55 }, { 67, 69, 60, 59 }, { 71, 72, 64, 62 } };
  check_pairs("Leap, C major: 2 or 3 scale notes up or down", &a, 90, tg, lp, 4);

  arp_init(&a); chord(&a); arp_seed_mutation(&a, 7);
  arp_set_passing(&a, 100, ARP_PASS_NEIGHBOR);
  static const int nc[3][4] = { { 61, 59 }, { 65, 63 }, { 68, 66 } };
  check_pairs("Neighbor, Chromatic: a semitone", &a, 60, tg, nc, 2);

  arp_init(&a); chord(&a); arp_set_scale(&a, 0, ARP_SCALE_MAJOR); arp_seed_mutation(&a, 8);
  arp_set_passing(&a, 100, ARP_PASS_MIXED);
  static const int mx[3][4] = { { 62, 59, 64, 65 }, { 65, 62, 67, 69 }, { 69, 65, 71, 72 } };
  int seen = 0;
  {
    int gg[512]; n = play(&a, 0, 240.0 * 2 * 120, gg);
    int ok = n == 240;
    for (int i = 0; ok && i < 120; i++) {
      const int t = tg[i % 3], p = gg[2 * i];
      if (gg[2 * i + 1] != t) ok = 0;
      if (p == t - 1) seen |= 1;                                   /* approach */
      else if (in_set(p, mx[i % 3], 2)) seen |= 2;                 /* neighbour */
      else if (in_set(p, mx[i % 3] + 2, 2) || p == t - 7 || p == t - 5 || p == t - 3 || p == t - 4 ||
               p == t - 8 || p == t - 9) seen |= 4;                /* leap (up or down) */
      else ok = 0;
    }
    ok = ok && seen == 7;
    printf("%s: Mixed: approach, neighbour and leap all occur\n", ok ? "PASS" : "FAIL");
    failures += !ok;
  }

  /* 50 %: some new notes get one, never on the resolution step */
  arp_init(&a); chord(&a); arp_set_scale(&a, 0, ARP_SCALE_MAJOR); arp_seed_mutation(&a, 9);
  arp_set_passing(&a, 50, ARP_PASS_APPROACH);
  {
    int gg[512]; n = play(&a, 0, 240.0 * 200, gg);
    int ok = n == 200, np = 0, idx = 0;   /* walk the chord C E G in order, a passing note may precede each */
    for (int i = 0; ok && i < n; i++) {
      const int t = tg[idx % 3];
      if (gg[i] == t) idx++;
      else if (gg[i] == t - 1 && i + 1 < n && gg[i + 1] == t) np++;
      else if (!(gg[i] == t - 1 && i + 1 == n)) ok = 0;
    }
    ok = ok && np > 20 && np < 100;
    printf("%s: Passing 50 %%: %d passing notes, order intact\n", ok ? "PASS" : "FAIL", np);
    failures += !ok;
  }

  /* with repeats: the passing note comes before the whole run */
  arp_init(&a); chord(&a); arp_set_scale(&a, 0, ARP_SCALE_MAJOR);
  arp_set_passing(&a, 100, ARP_PASS_APPROACH);
  arp_set_repeats(&a, 3, ARP_RPT_RUN_UP);
  n = play(&a, 0, 240 * 8, g);
  static const int e2[] = { 59, 60, 62, 64, 63, 64, 65, 67 };
  expect("Approach + Repeats 3 Run up: B | C D E | D# | E F G", g, n, e2, 8);
  arp_set_repeats(&a, 1, ARP_RPT_SAME);   /* called every run(): must not cut the resolution */
  n = play(&a, 240 * 8, 240 * 12, g);
  static const int e2b[] = { 66, 67, 59, 60 };   /* F# before G, its resolution G (run cut to 1), then B C */
  expect("Repeats lowered: the run is cut, passing + resolution stay", g, n, e2b, 4);

  /* Loop order: one loop position per chord note */
  loop_core(&a, 4, 0);
  arp_set_passing(&a, 100, ARP_PASS_APPROACH);
  n = play(&a, 0, 240 * 8, g);
  static const int e3[] = { 59, 60, 63, 64, 66, 67, 59, 60 };
  expect("Loop with passing notes: positions not skipped", g, n, e3, 8);

  /* the target is released before it resolves: the order moves on */
  arp_init(&a); chord(&a);
  arp_set_passing(&a, 100, ARP_PASS_APPROACH);
  n = play(&a, 0, 240, g);
  arp_note_off(&a, 0, 60);
  n = play(&a, 240, 240 * 2, g);
  static const int e4[] = { 63 };
  expect("released target: next note's passing note", g, n, e4, 1);

  /* passing with shift: from the chord note, then shifted */
  arp_init(&a); arp_note_on(&a, 0, 60, 100);
  arp_set_length(&a, 1); arp_set_step(&a, 0, 100, 12, 100, ARP_GATE_GLOBAL);
  arp_set_passing(&a, 100, ARP_PASS_APPROACH);
  n = play(&a, 0, 240 * 2, g);
  static const int e5[] = { 71, 72 };
  expect("passing note goes through Shift like the chord note", g, n, e5, 2);

  /* Key Auto: follows the lowest held key */
  arp_init(&a); arp_note_on(&a, 0, 65, 100);
  arp_set_length(&a, 1); arp_set_step(&a, 0, 100, 6, 100, ARP_GATE_GLOBAL);
  arp_set_scale(&a, 0, ARP_SCALE_MAJOR);
  n = play(&a, 0, 240, g);
  arp_set_scale(&a, ARP_KEY_AUTO, ARP_SCALE_MAJOR);
  n += play(&a, 240, 480, g + 1);
  static const int e6[] = { 71, 72 };   /* F +6: B in C major, but B -> C in F major */
  expect("Key Auto: F alone -> F major", g, n, e6, 2);
  arp_note_off(&a, 0, 65); arp_note_on(&a, 0, 62, 100); arp_note_on(&a, 0, 69, 100);
  play(&a, 480, 720, g);
  printf("%s: Key Auto follows a new chord (D-A -> D)\n", a.key == 2 ? "PASS" : "FAIL");
  failures += a.key != 2;
}

int main(void) {
  test_up_one_block();
  test_chunks();
  test_add_release();
  test_insert_below();
  test_silence();
  test_late_off();
  test_release_all();
  test_orders();
  test_single_note_orders();
  test_played();
  test_random();
  test_rates();
  test_gates();
  test_live_changes();
  test_high_notes();
  test_wide_chord_octaves();
  test_latch();
  test_pattern_shift_example();
  test_prob();
  test_shift_range();
  test_step_velocity();
  test_step_gate();
  test_tie();
  test_pattern_reset_and_length();
  test_step_count();
  test_lane_lengths();
  test_lane_prob_gate();
  test_lane_reset_and_change();
  test_scale_shift();
  test_scale_in_pattern();
  test_loop_frozen();
  test_loop_mutation();
  test_step_mutation();
  test_mutation_details();
  test_transpose();
  test_repeats();
  test_groove();
  test_passing();
  printf(failures ? "\n%d FAILED\n" : "\nALL PASSED\n", failures);
  return failures ? 1 : 0;
}
