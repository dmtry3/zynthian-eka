#include "arp_core.h"
#include <math.h>
#include <string.h>

#define ARP_MAX_PAT (ARP_MAX_NOTES * ARP_MAX_OCTAVES)

static const int rate_ticks[ARP_NRATES] = { 960, 480, 320, 240, 160, 120 };

/* bit i = semitone i above the key */
#define M(a, b, c, d, e, f, g) ((1u << (a)) | (1u << (b)) | (1u << (c)) | (1u << (d)) | (1u << (e)) | (1u << (f)) | (1u << (g)))
static const uint16_t scale_mask[ARP_NSCALES] = {
  0xFFF,                          /* Chromatic */
  M(0, 2, 4, 5, 7, 9, 11),        /* Major */
  M(0, 2, 3, 5, 7, 8, 10),        /* Minor (natural) */
  M(0, 2, 3, 5, 7, 9, 10),        /* Dorian */
  M(0, 1, 3, 5, 7, 8, 10),        /* Phrygian */
  M(0, 2, 4, 6, 7, 9, 11),        /* Lydian */
  M(0, 2, 4, 5, 7, 9, 10),        /* Mixolydian */
  M(0, 2, 3, 5, 7, 8, 11),        /* Harmonic minor */
  M(0, 2, 3, 5, 7, 9, 11),        /* Melodic minor */
  M(0, 2, 4, 7, 9, 0, 0),         /* Major pentatonic */
  M(0, 3, 5, 7, 10, 0, 0),        /* Minor pentatonic */
  M(0, 3, 5, 6, 7, 10, 0),        /* Blues */
};
#undef M

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void arp_init(ArpCore* a) {
  memset(a, 0, sizeof *a);
  a->rng = 0x9E3779B9u;
  arp_set_params(a, ARP_RATE_16, 50, ARP_UP, 1);
  arp_set_length(a, ARP_MAX_STEPS);
  arp_set_scale(a, 0, ARP_SCALE_CHROMATIC);
  for (int i = 0; i < ARP_MAX_STEPS; i++) { arp_set_step(a, i, 100, 0, 100, ARP_GATE_GLOBAL); arp_set_ratch(a, i, 1); }
  arp_set_loop(a, 0, 8);
  arp_set_repeats(a, 1, ARP_RPT_SAME);
  arp_set_ratch_every(a, 1);
  a->density = 50; a->dynamics = 50;
  a->roll_next = -INFINITY;
  arp_set_groove(a, 50, ARP_FEEL_PAIRS, 0, 0);
  arp_set_passing(a, 0, ARP_PASS_NEIGHBOR);
  a->done_s = -INFINITY;
  a->off_s = -INFINITY;
  arp_seed_mutation(a, 0);
  arp_reset_mutation(a);
}

void arp_set_scale(ArpCore* a, int key, int scale) {
  a->key_auto = key >= ARP_KEY_AUTO;
  if (!a->key_auto) a->key = clampi(key, 0, 11);   /* Auto: set from the chord on every step */
  a->scale = clampi(scale, 0, ARP_NSCALES - 1);
}

static int in_scale(const ArpCore* a, int p) {
  return (scale_mask[a->scale] >> (((p - a->key) % 12 + 12) % 12)) & 1;
}

int arp_shift_note(const ArpCore* a, int note, int n) {
  if (n == 0) return note;                       /* played note stays as played, even off the scale */
  int p = note + n;                              /* Shift is always in semitones */
  if (a->scale == ARP_SCALE_CHROMATIC || in_scale(a, p)) return p;
  const int dir = n > 0 ? 1 : -1;                /* off the scale: nearest scale note, ties toward the shift */
  for (int d = 1; d < 12; d++) {
    if (in_scale(a, p + dir * d)) return p + dir * d;
    if (in_scale(a, p - dir * d)) return p - dir * d;
  }
  return p;
}

void arp_set_transpose(ArpCore* a, int octaves) { a->transpose = clampi(octaves, -2, 2); }

void arp_set_repeats(ArpCore* a, int rpt, int mode) {
  a->rpt = clampi(rpt, 1, ARP_RPT_RND5);
  a->rpt_mode = clampi(mode, 0, ARP_NRPT_MODES - 1);
  const int max_left = a->rpt - 1 - a->rpt_k;   /* fewer repeats: cut the current run */
  if (a->rpt <= 5 && a->rpt_left > max_left) a->rpt_left = max_left < 0 ? 0 : max_left;
}

void arp_set_passing(ArpCore* a, int pct, int style) {
  a->passing = clampi(pct, 0, 100);
  a->pass_style = clampi(style, 0, ARP_NPASS - 1);
}

void arp_set_groove(ArpCore* a, int swing, int feel, int drunk, int accent) {
  a->swing = clampi(swing, 50, 75);
  a->feel = clampi(feel, 0, ARP_NFEELS - 1);
  a->drunk = clampi(drunk, 0, 100);
  a->accent = clampi(accent, 0, 100);
}

void arp_set_lane_length(ArpCore* a, int lane, int length) {
  if (lane >= 0 && lane < ARP_NROWS) a->lane_len[lane] = clampi(length, 1, ARP_MAX_STEPS);
}

void arp_set_length(ArpCore* a, int length) {
  for (int r = 0; r < ARP_NROWS; r++) arp_set_lane_length(a, r, length);
}

void arp_set_step(ArpCore* a, int i, int prob, int shift, int vel, int gate) {
  if (i < 0 || i >= ARP_MAX_STEPS) return;
  ArpStep* st = &a->steps[i];
  st->prob = clampi(prob, 0, 100);
  st->shift = clampi(shift, -24, 24);
  st->vel = clampi(vel, 0, 200);
  st->gate = (gate <= ARP_GATE_GLOBAL) ? ARP_GATE_GLOBAL : clampi(gate, 5, ARP_GATE_TIE);
}

void arp_set_ratch(ArpCore* a, int i, int hits) {
  if (i >= 0 && i < ARP_MAX_STEPS) a->steps[i].ratch = clampi(hits, 1, ARP_MAX_RATCH);
}

void arp_set_ratch_every(ArpCore* a, int n) { a->ratch_every = clampi(n, 1, ARP_MAX_RATCH_EVERY); }

void arp_set_params(ArpCore* a, int rate, int gate_pct, int order, int octaves) {
  a->rate = clampi(rate, 0, ARP_NRATES - 1);
  a->gate_pct = clampi(gate_pct, 5, 100);
  a->order = clampi(order, 0, ARP_NORDERS - 1);
  a->octaves = clampi(octaves, 1, ARP_MAX_OCTAVES);
  a->step_ticks = rate_ticks[a->rate] * (a->fx == ARP_FX_HALF ? 2 : 1);
  a->gate_ticks = a->step_ticks * a->gate_pct / 100;
  if (a->gate_ticks < 1) a->gate_ticks = 1;
}

static const int roll_ticks_of[ARP_NFX] = { 0, 480, 240, 120, 0, 0, 0, 0, 0 };

void arp_set_play(ArpCore* a, int fx, int density, int dynamics) {
  fx = clampi(fx, 0, ARP_NFX - 1);
  a->density = clampi(density, 0, 100);
  a->dynamics = clampi(dynamics, 0, 100);
  if (fx == a->fx) return;
  const int was_half = (a->fx == ARP_FX_HALF);
  a->fx = fx;
  const int rt = roll_ticks_of[fx];
  if (rt && rt != a->roll_ticks) {                       /* a roll starts: repeat the last played note */
    if (a->has_on) { a->roll_ch = a->on_ch; a->roll_note = a->on_note; a->roll_vel = a->on_vel; }
    else if (a->nheld) { a->roll_ch = a->held[0].ch; a->roll_note = a->held[0].note; a->roll_vel = a->held[0].vel; }
    else a->roll_vel = 0;                                /* nothing to repeat yet */
    a->roll_next = -INFINITY;
    a->rat_left = 0;
  }
  a->roll_ticks = rt;
  if (was_half || fx == ARP_FX_HALF) arp_set_params(a, a->rate, a->gate_pct, a->order, a->octaves);
}

void arp_seed(ArpCore* a, uint32_t seed) { a->rng = seed ? seed : 0x9E3779B9u; }

static uint32_t xorshift(uint32_t* s) {
  uint32_t x = *s;
  x ^= x << 13; x ^= x >> 17; x ^= x << 5;
  return *s = x;
}
static uint32_t rnd(ArpCore* a) { return xorshift(&a->rng); }
static uint32_t mrnd(ArpCore* a) { return xorshift(&a->mrng); }

void arp_set_loop(ArpCore* a, int notes_pct, int loop_len) {
  a->mut_notes = clampi(notes_pct, 0, 100);
  a->loop_len = clampi(loop_len, 1, ARP_MAX_STEPS);
}

void arp_seed_mutation(ArpCore* a, uint32_t seed) { a->mrng = seed ? seed : 0x2545F491u; }

void arp_set_mut_steps(ArpCore* a, int pct) { a->mut_steps = clampi(pct, 0, 100); }

void arp_reset_mutation(ArpCore* a) {
  for (int i = 0; i < ARP_MAX_STEPS; i++) a->loop[i] = (uint16_t)(mrnd(a) >> 16);
  memset(a->dev, 0, sizeof a->dev);
}

void arp_clear_mutation(ArpCore* a, int i, int lane) {
  if (i >= 0 && i < ARP_MAX_STEPS && lane >= 0 && lane < ARP_NLANES) a->dev[i][lane] = 0;
}

static int chance(ArpCore* a, int pct) { return pct >= 100 || (pct > 0 && (int)(mrnd(a) % 100u) < pct); }

static int key_less(uint8_t n1, uint8_t c1, uint8_t n2, uint8_t c2) {
  return n1 != n2 ? n1 < n2 : c1 < c2;
}

static void reset_order(ArpCore* a) {
  a->has_last = 0; a->played_idx = 0; a->count = 0; a->loop_pos = 0;
  a->rpt_left = 0; a->rpt_k = 0;
  a->rat_left = 0;
}

void arp_set_sync(ArpCore* a, int on) { a->sync = on != 0; }
void arp_restart(ArpCore* a) { reset_order(a); }

static int find(const ArpNote* v, int n, uint8_t ch, uint8_t note) {
  for (int i = 0; i < n; i++) if (v[i].ch == ch && v[i].note == note) return i;
  return -1;
}

static void held_add(ArpCore* a, uint8_t ch, uint8_t note, uint8_t vel) {
  int i = find(a->held, a->nheld, ch, note);
  if (i >= 0) { a->held[i].vel = vel; return; }
  if (a->nheld >= ARP_MAX_NOTES) return;
  int pos = a->nheld;
  while (pos > 0 && key_less(note, ch, a->held[pos - 1].note, a->held[pos - 1].ch)) {
    a->held[pos] = a->held[pos - 1];
    pos--;
  }
  a->held[pos].ch = ch; a->held[pos].note = note; a->held[pos].vel = vel;
  a->held[pos].seq = a->press_seq++;
  a->nheld++;
}

static void held_remove(ArpCore* a, uint8_t ch, uint8_t note) {
  int i = find(a->held, a->nheld, ch, note);
  if (i < 0) return;
  memmove(&a->held[i], &a->held[i + 1], (size_t)(a->nheld - i - 1) * sizeof(ArpNote));
  a->nheld--;
  if (a->nheld == 0) reset_order(a);
}

int arp_note_on(ArpCore* a, uint8_t ch, uint8_t note, uint8_t vel) {
  int first = (a->nheld == 0);
  if (a->latch && a->nphys == 0 && a->nheld > 0) {   /* new chord replaces the latched one */
    a->nheld = 0;
    reset_order(a);
  }
  if (find(a->phys, a->nphys, ch, note) < 0 && a->nphys < ARP_MAX_NOTES) {
    a->phys[a->nphys].ch = ch; a->phys[a->nphys].note = note; a->phys[a->nphys].vel = vel;
    a->nphys++;
  }
  held_add(a, ch, note, vel);
  return first;
}

void arp_note_off(ArpCore* a, uint8_t ch, uint8_t note) {
  int i = find(a->phys, a->nphys, ch, note);
  if (i >= 0) a->phys[i] = a->phys[--a->nphys];
  if (!a->latch) held_remove(a, ch, note);
}

void arp_set_latch(ArpCore* a, int on) {
  on = on ? 1 : 0;
  if (on == a->latch) return;
  a->latch = on;
  if (!on)   /* keep only the keys that are still down */
    for (int i = a->nheld - 1; i >= 0; i--)
      if (find(a->phys, a->nphys, a->held[i].ch, a->held[i].note) < 0) held_remove(a, a->held[i].ch, a->held[i].note);
}

void arp_release_all(ArpCore* a) { a->nheld = 0; a->nphys = 0; reset_order(a); }

/* Held keys repeated over the octaves, notes above 127 dropped.
 * by_pitch: sorted by pitch (duplicates removed); otherwise octave by octave in press order. */
static int build_pattern(const ArpCore* a, ArpNote* p, int by_pitch) {
  int m = 0;
  if (by_pitch) {
    for (int o = 0; o < a->octaves; o++)
      for (int i = 0; i < a->nheld; i++) {
        int pitch = a->held[i].note + 12 * o;
        if (pitch > 127) continue;
        ArpNote n = a->held[i];
        n.note = (uint8_t)pitch;
        int pos = m;
        while (pos > 0 && key_less(n.note, n.ch, p[pos - 1].note, p[pos - 1].ch)) { p[pos] = p[pos - 1]; pos--; }
        p[pos] = n;
        m++;
      }
    int w = 0;   /* remove duplicates (same pitch and channel from different octaves) */
    for (int i = 0; i < m; i++)
      if (w == 0 || p[i].note != p[w - 1].note || p[i].ch != p[w - 1].ch) p[w++] = p[i];
    return w;
  }
  ArpNote ord[ARP_MAX_NOTES];
  int n = a->nheld;
  for (int i = 0; i < n; i++) {   /* held keys in press order */
    int pos = i;
    while (pos > 0 && ord[pos - 1].seq > a->held[i].seq) { ord[pos] = ord[pos - 1]; pos--; }
    ord[pos] = a->held[i];
  }
  for (int o = 0; o < a->octaves; o++)
    for (int i = 0; i < n; i++) {
      int pitch = ord[i].note + 12 * o;
      if (pitch > 127) continue;
      p[m] = ord[i];
      p[m].note = (uint8_t)pitch;
      m++;
    }
  return m;
}

/* index of the first entry above / last entry below the last played pitch, -1 if none */
static int above(const ArpCore* a, const ArpNote* p, int m) {
  for (int i = 0; i < m; i++) if (key_less(a->last_note, a->last_ch, p[i].note, p[i].ch)) return i;
  return -1;
}
static int below(const ArpCore* a, const ArpNote* p, int m) {
  for (int i = m - 1; i >= 0; i--) if (key_less(p[i].note, p[i].ch, a->last_note, a->last_ch)) return i;
  return -1;
}

static int pick(ArpCore* a, const ArpNote* p, int m) {
  int i;
  int order = a->order;
  const int rev = (a->fx == ARP_FX_REV);
  if (rev && order == ARP_UP) order = ARP_DOWN;          /* Rev: the order runs backwards */
  else if (rev && order == ARP_DOWN) order = ARP_UP;
  switch (order) {
  case ARP_DOWN:
    i = a->has_last ? below(a, p, m) : -1;
    return i >= 0 ? i : m - 1;
  case ARP_UPDOWN:
  case ARP_DOWNUP:
    if (!a->has_last) { a->dir = (a->order == ARP_UPDOWN) ? 1 : -1; return a->dir > 0 ? 0 : m - 1; }
    if (a->dir == 0) a->dir = 1;
    if (rev) { a->dir = -a->dir; }                         /* Rev: walk the other way (dir restored below) */
    i = a->dir > 0 ? above(a, p, m) : below(a, p, m);
    if (i < 0) { a->dir = -a->dir; i = a->dir > 0 ? above(a, p, m) : below(a, p, m); }
    if (rev) a->dir = -a->dir;
    return i >= 0 ? i : 0;
  case ARP_PLAYED:
    if (rev) { a->played_idx += (uint32_t)m - 1; return (int)(a->played_idx % (uint32_t)m); }
    return (int)(a->played_idx++ % (uint32_t)m);
  case ARP_RANDOM: {
    int k = -1;
    if (a->has_last)
      for (int j = 0; j < m; j++) if (p[j].note == a->last_note && p[j].ch == a->last_ch) { k = j; break; }
    if (k < 0 || m < 2) return (int)(rnd(a) % (uint32_t)m);
    i = (int)(rnd(a) % (uint32_t)(m - 1));
    return i >= k ? i + 1 : i;
  }
  case ARP_LOOP: {   /* the loop step may mutate when it is read, then maps onto the chord */
    uint32_t pos;
    if (rev) {                                             /* Rev: the loop backwards */
      a->loop_pos = (a->loop_pos % (uint32_t)a->loop_len + (uint32_t)a->loop_len - 1) % (uint32_t)a->loop_len;
      pos = a->loop_pos;
    } else pos = a->loop_pos++ % (uint32_t)a->loop_len;
    if (chance(a, a->mut_notes)) a->loop[pos] = (uint16_t)(mrnd(a) >> 16);
    return (int)(((uint32_t)a->loop[pos] * (uint32_t)m) >> 16);
  }
  default:   /* ARP_UP */
    i = a->has_last ? above(a, p, m) : -1;
    return i >= 0 ? i : 0;
  }
}

/* Share of the swing delay (sixths) of each grid position, per feel. */
static const int8_t feel_len[ARP_NFEELS] = { 2, 4, 4, 3 };
static const int8_t feel_shape[ARP_NFEELS][4] = {
  { 0, 6 },          /* Pairs:  0 1 */
  { 0, 3, 6, 3 },    /* Fours:  0 1/2 1 1/2 */
  { 0, 2, 4, 6 },    /* Drag:   0 1/3 2/3 1 */
  { 0, 6, 0 },       /* Threes: 0 1 0 */
};

double arp_step_offset(ArpCore* a, double s) {
  const double step = a->step_ticks;
  if (s == a->off_s && step == a->off_step) return a->off_val;
  const long long pos = llround(s / step);
  const int len = feel_len[a->feel];
  const int sixths = feel_shape[a->feel][(int)(((pos % len) + len) % len)];
  double off = step * (2 * a->swing - 100) / 100.0 * sixths / 6.0;
  if (a->drunk > 0) {   /* random walk: at most 0.2 of the range per step, reflected at 0 and 1 */
    float w = a->drunk_w + ((float)(mrnd(a) >> 8) / 16777216.0f * 2.0f - 1.0f) * 0.2f;
    if (w < 0) w = -w;
    if (w > 1) w = 2 - w;
    a->drunk_w = w;
    off += w * a->drunk / 100.0 * step / 4.0;
  }
  a->off_s = s; a->off_step = step; a->off_val = off; a->off_sixths = sixths;
  return off;
}

static void note_off_at(ArpCore* a, double tick, arp_emit_fn emit, void* user) {
  emit(user, tick, (uint8_t)(0x80 | a->on_ch), a->on_note, 0);
  a->sounding = 0;
  a->tied = 0;
}

void arp_silence(ArpCore* a, double tick, arp_emit_fn emit, void* user) {
  a->rat_left = 0;
  if (a->sounding) note_off_at(a, tick, emit, user);
}

/* Gate of a step as a notch 0..20 (5 %..100 %, 20 = Tie); Global counts from the global Gate. */
static int gate_notch(const ArpCore* a, int gate) {
  if (gate == ARP_GATE_TIE) return 20;
  int pct = (gate == ARP_GATE_GLOBAL) ? a->gate_pct : gate;
  return clampi((pct + 2) / 5 - 1, 0, 19);
}

/* Value of a row after a drift of d notches (shift: d is applied to the pitch, see drift_pitch). */
int arp_drifted(const ArpCore* a, int lane, int v, int d) {
  if (d == 0) return v;
  switch (lane) {
  case ARP_LANE_PROB: return clampi(v + d * ARP_MUT_PROB_NOTCH, 0, 100);
  case ARP_LANE_VEL:  return clampi(v + d * ARP_MUT_VEL_NOTCH, 0, 200);
  case ARP_LANE_GATE: { int n = clampi(gate_notch(a, v) + d, 0, 20); return n == 20 ? ARP_GATE_TIE : (n + 1) * 5; }
  default: return v;
  }
}

/* Pitch moved by d scale notes (semitones with Chromatic). */
static int drift_pitch(const ArpCore* a, int p, int d) {
  const int dir = d > 0 ? 1 : -1;
  for (int k = 0; k != d; k += dir) {
    p += dir;
    if (a->scale != ARP_SCALE_CHROMATIC)
      for (int j = 0; j < 11 && !in_scale(a, p); j++) p += dir;
  }
  return p;
}

/* One notch of drift for row r of step i: a random direction that changes the value and stays within
 * the limit (at the limit it can only go back). */
void arp_mutate_step(ArpCore* a, int i, int r) {
  const int lim = (r == ARP_LANE_GATE) ? ARP_MUT_MAX_GATE : ARP_MUT_MAX;
  const int d = a->dev[i][r];
  int v = 0;
  switch (r) {
  case ARP_LANE_PROB: v = a->steps[i].prob; break;
  case ARP_LANE_VEL:  v = a->steps[i].vel; break;
  case ARP_LANE_GATE: v = a->steps[i].gate; break;
  default: break;
  }
  int ok[2], n = 0;
  for (int dir = -1; dir <= 1; dir += 2) {
    int nd = d + dir;
    if (nd < -lim || nd > lim) continue;
    if (r != ARP_LANE_SHIFT && arp_drifted(a, r, v, nd) == arp_drifted(a, r, v, d)) continue;   /* clamped: no change */
    ok[n++] = dir;
  }
  if (n == 0) return;
  a->dev[i][r] = (int8_t)(d + ok[n == 2 ? (int)(mrnd(a) & 1u) : 0]);
}

/* One step of the grid: grid step number g (from tick 0), played at tick s (grid step + groove offset). */
static void do_step(ArpCore* a, long long g, double s, arp_emit_fn emit, void* user) {
  a->rat_left = 0;                                        /* hits of the last step are over by now */
  if (a->nheld == 0) {
    if (a->sounding && a->tied) note_off_at(a, s, emit, user);   /* a tie has nothing to go to */
    a->step_count = 0;
    return;
  }
  if (a->key_auto) a->key = a->held[0].note % 12;      /* held is sorted: [0] is the lowest key */
  ArpNote pat[ARP_MAX_PAT];
  int m = build_pattern(a, pat, a->order != ARP_PLAYED);
  if (m == 0) return;
  /* Repeats: the same chord note for several steps; the note order (and Loop) waits meanwhile. */
  int base = -1;
  if (a->rpt_left > 0)
    for (int j = 0; j < m; j++) if (pat[j].note == a->rpt_note.note && pat[j].ch == a->rpt_note.ch) { base = j; break; }
  if (base >= 0) {
    a->rpt_left--; a->rpt_k++;
  } else {                                                /* a new note (or the repeated one was released) */
    base = pick(a, pat, m);                               /* the note order advances even if the step is silent */
    a->rpt_k = 0;
    a->rpt_left = (a->rpt <= 5 ? a->rpt : 1 + (int)(mrnd(a) % (a->rpt == ARP_RPT_RND3 ? 3u : 5u))) - 1;
    if (chance(a, a->passing)) {                          /* a passing note first, the chord note next step */
      int st = a->pass_style == ARP_PASS_MIXED ? (int)(mrnd(a) % 3u) : a->pass_style;
      const int dir = (mrnd(a) >> 31) ? 1 : -1;
      a->pass_approach = (st == ARP_PASS_APPROACH);       /* a semitone below */
      a->pass_iv = (st == ARP_PASS_LEAP) ? dir * (2 + (int)(mrnd(a) >> 31)) : dir;
      a->rpt_k = -1;
      a->rpt_left++;
    }
  }
  a->rpt_note = pat[base];
  const ArpNote* n = &a->rpt_note;
  a->has_last = 1; a->last_ch = n->ch; a->last_note = n->note;

  /* each row reads its own step */
  const uint64_t pos = a->sync ? (uint64_t)(g > 0 ? g : 0) : a->count;
  int idx[ARP_NROWS];
  for (int r = 0; r < ARP_NROWS; r++) idx[r] = (int)(pos % (uint64_t)a->lane_len[r]);
  if (chance(a, a->mut_steps)) {                          /* mutation: one row drifts, heard right away */
    const int r = (int)(mrnd(a) % ARP_NLANES);
    arp_mutate_step(a, idx[r], r);
  }
  const ArpStep* sp = &a->steps[idx[ARP_LANE_PROB]];
  const ArpStep* ss = &a->steps[idx[ARP_LANE_SHIFT]];
  const ArpStep* sv = &a->steps[idx[ARP_LANE_VEL]];
  const ArpStep* sg = &a->steps[idx[ARP_LANE_GATE]];
  const int prob  = arp_drifted(a, ARP_LANE_PROB, sp->prob, a->dev[idx[ARP_LANE_PROB]][ARP_LANE_PROB]);
  const int velp  = arp_drifted(a, ARP_LANE_VEL, sv->vel, a->dev[idx[ARP_LANE_VEL]][ARP_LANE_VEL]);
  const int gate  = arp_drifted(a, ARP_LANE_GATE, sg->gate, a->dev[idx[ARP_LANE_GATE]][ARP_LANE_GATE]);
  const int sdev  = a->dev[idx[ARP_LANE_SHIFT]][ARP_LANE_SHIFT];
  a->count = (uint32_t)pos + 1;
  a->step_count = a->count;

  int pr = prob;                                          /* Play: Density 50 = as drawn */
  if (a->density < 50) pr = pr * a->density / 50;
  else if (a->density > 50) pr = pr + (100 - pr) * (a->density - 50) / 50;
  int play = pr >= 100 || (pr > 0 && (int)(rnd(a) % 100u) < pr);
  if (a->fx == ARP_FX_DROP) play = 0;                     /* Drop: silent, the pattern keeps going */
  int note = n->note;
  if (a->rpt_k < 0)                                       /* passing note: from the chord note, then as usual */
    note = a->pass_approach ? note - 1 : drift_pitch(a, note, a->pass_iv);
  int pitch = arp_shift_note(a, note, ss->shift);
  if (sdev) pitch = drift_pitch(a, pitch, sdev);
  const int k = a->rpt_k;
  if (k > 0) {
    if (a->rpt_mode == ARP_RPT_RUN_UP) pitch = drift_pitch(a, pitch, k);
    else if (a->rpt_mode == ARP_RPT_RUN_DOWN) pitch = drift_pitch(a, pitch, -k);
    else if (a->rpt_mode == ARP_RPT_OCT && (k & 1)) pitch += 12;
  }
  pitch += 12 * a->transpose;
  if (a->fx == ARP_FX_OCT) pitch += 12;
  if (pitch < 0 || pitch > 127) play = 0;
  if (!play) {
    if (a->sounding) note_off_at(a, s, emit, user);
    return;
  }
  int vel = n->vel * velp / 100;
  if (a->rpt_mode == ARP_RPT_ECHO)
    for (int j = 0; j < k; j++) vel = vel * ARP_ECHO_PCT / 100;
  vel = clampi(vel, 1, 127);
  if (a->dynamics < 50) vel = clampi(vel * (30 + 70 * a->dynamics / 50) / 100, 1, 127);   /* Play: Dynamics */
  else if (a->dynamics > 50) vel = clampi(vel + (127 - vel) * (a->dynamics - 50) / 50, 1, 127);
  if (a->accent) {   /* by the feel shape h: h = 0 toward 127, h = 1 down to 30 % (at Accent 100 %) */
    const int hi = vel + a->accent * (127 - vel) / 100;
    const int lo = vel * (100 - 70 * a->accent / 100) / 100;
    vel = clampi((hi * (6 - a->off_sixths) + lo * a->off_sixths + 3) / 6, 1, 127);
  }

  if (a->sounding && a->tied && a->on_note == pitch && a->on_ch == n->ch) {
    /* tied into the same pitch: the note just continues */
  } else if (a->sounding && a->tied) {
    uint8_t och = a->on_ch, onote = a->on_note;          /* legato: new note on before the old one off */
    emit(user, s, (uint8_t)(0x90 | n->ch), (uint8_t)pitch, (uint8_t)vel);
    emit(user, s, (uint8_t)(0x80 | och), onote, 0);
  } else {
    if (a->sounding) note_off_at(a, s, emit, user);
    emit(user, s, (uint8_t)(0x90 | n->ch), (uint8_t)pitch, (uint8_t)vel);
  }
  a->sounding = 1; a->on_ch = n->ch; a->on_note = (uint8_t)pitch; a->on_vel = (uint8_t)vel; a->has_on = 1;
  int gt = 0;                                           /* gate of the step in ticks (not Tie) */
  if (gate != ARP_GATE_TIE) {
    gt = (gate == ARP_GATE_GLOBAL) ? a->gate_ticks : a->step_ticks * gate / 100;
    if (a->accent) gt = gt * (1200 - a->accent * a->off_sixths) / 1200;   /* delayed steps shorter, down to 1/2 */
    if (a->dynamics < 50) gt = gt * (40 + 60 * a->dynamics / 50) / 100;  /* Play: Dynamics, down to 40 % */
    else if (a->dynamics > 50) gt = gt + (a->step_ticks - gt) * (a->dynamics - 50) / 50;
  }
  int hits = a->steps[idx[ARP_LANE_RATCH]].ratch;
  if (hits > 1 && a->ratch_every > 1 &&                 /* Ratch every N: only the last pass of each N */
      (pos / (uint64_t)a->lane_len[ARP_LANE_RATCH]) % (uint64_t)a->ratch_every != (uint64_t)(a->ratch_every - 1))
    hits = 1;
  if (a->fx == ARP_FX_FILL) hits = ARP_FILL_HITS;
  if (hits > 1) {   /* ratchet: even hits from s to the next grid step, each with the step's gate share */
    const double span = (double)(g + 1) * a->step_ticks - s;
    a->rat_len = span / hits;
    a->rat_left = hits - 1;
    a->rat_next = s + a->rat_len;
    a->rat_vel = (uint8_t)vel;
    a->rat_tie = (gate == ARP_GATE_TIE);
    a->rat_gate = (int)(a->rat_len * gt / a->step_ticks);
    if (a->rat_gate < 1) a->rat_gate = 1;
    a->tied = 0;
    a->off_tick = a->rat_tie ? a->rat_next : s + a->rat_gate;
  } else if (gate == ARP_GATE_TIE) {
    a->tied = 1;
    a->off_tick = INFINITY;                             /* ends at the next step */
  } else {
    a->tied = 0;
    a->off_tick = s + (gt < 1 ? 1 : gt);
  }
}

/* The next hit of a ratchet: the step's note again at the same velocity. */
static void do_hit(ArpCore* a, double t, arp_emit_fn emit, void* user) {
  if (a->nheld == 0) { a->rat_left = 0; return; }        /* keys released: the rest of the hits is dropped */
  if (a->sounding) note_off_at(a, t, emit, user);
  emit(user, t, (uint8_t)(0x90 | a->on_ch), a->on_note, a->rat_vel);
  a->sounding = 1;
  a->rat_left--;
  if (a->rat_tie && a->rat_left == 0) { a->tied = 1; a->off_tick = INFINITY; }
  else a->off_tick = a->rat_tie ? a->rat_next + a->rat_len : t + a->rat_gate;
  a->rat_next += a->rat_len;
}

/* Roll: the latched note on every roll tick, half a roll tick long; the pattern steps are skipped. */
static void process_roll(ArpCore* a, double from, double to, arp_emit_fn emit, void* user) {
  const double rt = a->roll_ticks;
  if (a->roll_next < from - rt) a->roll_next = -INFINITY; /* time went back */
  if (a->roll_next == -INFINITY) a->roll_next = ceil(from / rt) * rt;
  for (;;) {
    const int off_due = a->sounding && a->off_tick < to;
    const int hit_due = a->roll_next < to;
    if (off_due && (!hit_due || a->off_tick <= a->roll_next)) {
      note_off_at(a, a->off_tick > from ? a->off_tick : from, emit, user);
      continue;
    }
    if (!hit_due) break;
    const double t = a->roll_next > from ? a->roll_next : from;
    a->roll_next += rt;
    if (a->nheld == 0 || a->roll_vel == 0) continue;    /* nothing held: the roll is silent */
    if (a->sounding) note_off_at(a, t, emit, user);
    emit(user, t, (uint8_t)(0x90 | a->roll_ch), a->roll_note, a->roll_vel);
    a->sounding = 1; a->tied = 0; a->on_ch = a->roll_ch; a->on_note = a->roll_note;
    a->off_tick = t + rt / 2;
  }
  /* the steps under the roll are over: after it the pattern comes back on the next step of the grid */
  const double step = a->step_ticks;
  const double last = ceil(to / step) * step - step;   /* last grid step before `to` */
  if (last > a->done_s) a->done_s = last;
}

void arp_process(ArpCore* a, double from, double to, arp_emit_fn emit, void* user) {
  if (to <= from) return;
  if (a->roll_ticks) { process_roll(a, from, to, emit, user); return; }
  const double step = a->step_ticks;
  if (from < a->done_s - step) a->done_s = -INFINITY;     /* time went back (relocation) */
  /* a grid step before `from` may still be due: its offset is always less than a step */
  double s = ceil(from / step) * step - step;
  for (;;) {
    if (s <= a->done_s) { s += step; continue; }
    const double e = s + arp_step_offset(a, s);
    if (e < from) { a->done_s = s; s += step; continue; }  /* missed (jump into the middle) */
    int off_due = a->sounding && a->off_tick < to;
    int step_due = e < to;
    int hit_due = a->rat_left > 0 && a->rat_next < to;   /* hits always come before the next step */
    if (off_due && (!step_due || a->off_tick <= e) && (!hit_due || a->off_tick <= a->rat_next)) {
      note_off_at(a, a->off_tick > from ? a->off_tick : from, emit, user);
      continue;
    }
    if (hit_due && (!step_due || a->rat_next < e)) {
      do_hit(a, a->rat_next > from ? a->rat_next : from, emit, user);
      continue;
    }
    if (!step_due) break;
    do_step(a, llround(s / step), e, emit, user);
    a->done_s = s;
    s += step;
  }
}
