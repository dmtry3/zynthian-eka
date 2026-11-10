/* Arpeggiator logic without LV2 dependencies. Time is measured in ticks (ARP_PPQN per beat). */
#ifndef ARP_CORE_H
#define ARP_CORE_H

#include <stdint.h>

#define ARP_PPQN 960
#define ARP_MAX_NOTES 32
#define ARP_MAX_OCTAVES 4
#define ARP_MAX_STEPS 16
#define ARP_GATE_GLOBAL 0     /* step gate: use the global Gate */
#define ARP_GATE_TIE 101      /* step gate: legato into the next step */
/* Step mutation: one notch of each row and how far a step may drift from the drawn value (in notches). */
#define ARP_MUT_PROB_NOTCH 20 /* % */
#define ARP_MUT_VEL_NOTCH 20  /* % */
#define ARP_MUT_GATE_NOTCH 5  /* % of the step */
#define ARP_MUT_MAX 3         /* prob, shift (scale notes), vel */
#define ARP_MUT_MAX_GATE 4

/* Rows (lanes) of the pattern; each has its own length (polyrhythm). */
enum { ARP_LANE_PROB, ARP_LANE_SHIFT, ARP_LANE_VEL, ARP_LANE_GATE, ARP_NLANES };

/* Scales: Shift is in semitones; with a scale the shifted note is pulled to the nearest scale note. */
enum { ARP_SCALE_CHROMATIC, ARP_SCALE_MAJOR, ARP_SCALE_MINOR, ARP_SCALE_DORIAN, ARP_SCALE_PHRYGIAN,
       ARP_SCALE_LYDIAN, ARP_SCALE_MIXOLYDIAN, ARP_SCALE_HARM_MINOR, ARP_SCALE_MEL_MINOR,
       ARP_SCALE_MAJ_PENT, ARP_SCALE_MIN_PENT, ARP_SCALE_BLUES, ARP_NSCALES };

enum { ARP_RATE_4, ARP_RATE_8, ARP_RATE_8T, ARP_RATE_16, ARP_RATE_16T, ARP_RATE_32, ARP_NRATES };
/* ARP_LOOP: a loop of random positions in the chord (Turing Machine style), see arp_set_loop(). */
enum { ARP_UP, ARP_DOWN, ARP_UPDOWN, ARP_DOWNUP, ARP_PLAYED, ARP_RANDOM, ARP_LOOP, ARP_NORDERS };
/* Repeats: 1..5 = each chord note plays that many grid steps; RND3/RND5 = 1..3 / 1..5 at random per note. */
#define ARP_RPT_RND3 6
#define ARP_RPT_RND5 7
/* What the repeats (not the first note) do: same note, k scale notes up/down, every other one an octave
 * up, or quieter by ARP_ECHO_PCT % each. */
enum { ARP_RPT_SAME, ARP_RPT_RUN_UP, ARP_RPT_RUN_DOWN, ARP_RPT_OCT, ARP_RPT_ECHO, ARP_NRPT_MODES };
#define ARP_ECHO_PCT 70
/* Groove: step delay pattern (share of the swing delay, in sixths) by grid position; delays are late only. */
/* Passing notes: a step before a new chord note plays a neighbour and resolves into the note. */
enum { ARP_PASS_NEIGHBOR, ARP_PASS_LEAP, ARP_PASS_APPROACH, ARP_PASS_MIXED, ARP_NPASS };
#define ARP_KEY_AUTO 12       /* key = lowest held note */
enum { ARP_FEEL_PAIRS, ARP_FEEL_FOURS, ARP_FEEL_DRAG, ARP_FEEL_THREES, ARP_NFEELS };

/* Called for every outgoing MIDI event; tick is inside the range passed to arp_process(). */
typedef void (*arp_emit_fn)(void* user, double tick, uint8_t st, uint8_t d1, uint8_t d2);

typedef struct { uint8_t ch, note, vel; uint32_t seq; } ArpNote;   /* seq: press order */

/* Per-step settings of the pattern. */
typedef struct {
  int prob;    /* 0..100 %: chance that the step plays */
  int shift;   /* -24..+24 semitones added to the chosen note */
  int vel;     /* 0..200 % of the key velocity (result clamped to 1..127) */
  int gate;    /* ARP_GATE_GLOBAL, 5..100 % of the step, or ARP_GATE_TIE */
} ArpStep;

typedef struct {
  ArpNote held[ARP_MAX_NOTES];  /* notes being arpeggiated, sorted by note, then channel */
  int nheld;
  uint32_t press_seq;
  ArpNote phys[ARP_MAX_NOTES];  /* keys physically down (unsorted) */
  int nphys;
  int latch;                    /* latched: released keys stay in held until a new chord */

  /* parameters */
  int rate, gate_pct, order, octaves;
  int step_ticks, gate_ticks;
  ArpStep steps[ARP_MAX_STEPS];
  int lane_len[ARP_NLANES];     /* length of each row 1..16 */
  int key, scale;               /* key 0..11 (C..B), scale ARP_SCALE_* */
  int key_auto;                 /* Key = Auto: key follows the lowest held note */
  int passing, pass_style;      /* 0..100 % chance of a passing note before a new chord note, ARP_PASS_* */
  int transpose;                /* -2..+2 octaves, added to every generated note */
  int rpt, rpt_mode;            /* repeats 1..5 or ARP_RPT_RND*, ARP_RPT_* mode */
  int swing, feel, drunk, accent; /* swing 50..75 %, ARP_FEEL_*, drunk 0..100 %, accent 0..100 % (velocity and gate) */

  /* note order state; reset when all keys are released */
  int has_last;                 /* last played pitch valid */
  uint8_t last_ch, last_note;
  int dir;                      /* +1 / -1 for Up-Down and Down-Up */
  uint32_t played_idx;          /* As played position */
  uint32_t loop_pos;            /* Loop order position (advances once per chord note, not per repeat) */
  int rpt_left, rpt_k;          /* steps still to play of the current note, number of the current repeat
                                 * (-1: the passing note before it) */
  int pass_iv, pass_approach;   /* passing note: scale steps from the chord note, or a semitone below */
  ArpNote rpt_note;             /* the chord note being repeated */

  /* groove timing: grid step s plays at s + offset */
  double done_s;                /* last grid step played (-INFINITY: none) */
  double off_s, off_step, off_val; /* cached offset of grid step off_s (with step length off_step) */
  int off_sixths;               /* its share of the swing delay, for Accent */
  float drunk_w;                /* Drunk random walk 0..1 */
  uint32_t count;               /* grid steps since the pattern restarted; row r uses step count % lane_len[r] */
  uint32_t step_count;          /* number of the last step (1-based) for display; 0 when idle */
  uint32_t rng;

  /* mutation (own random generator, so the other orders don't depend on it) */
  uint16_t loop[ARP_MAX_STEPS]; /* Loop order: relative position in the chord, 0 = lowest note */
  int loop_len;                 /* 1..16 */
  int mut_notes;                /* 0..100 %: chance that the loop step gets a new random position */
  int mut_steps;                /* 0..100 %: chance that a played step drifts by one notch */
  int8_t dev[ARP_MAX_STEPS][ARP_NLANES];   /* drift from the drawn steps, in notches */
  uint32_t mrng;

  int sounding;                 /* a generated note is on */
  int tied;                     /* the sounding note is tied into the next step */
  uint8_t on_ch, on_note;
  double off_tick;
} ArpCore;

void arp_init(ArpCore* a);
/* Values are clamped. Rate/order take effect on the next step, gate on the next note. */
void arp_set_params(ArpCore* a, int rate, int gate_pct, int order, int octaves);
void arp_seed(ArpCore* a, uint32_t seed);
/* Pattern: length 1..16 of one row, of all rows, and settings of step i (0-based). Values are clamped. */
void arp_set_lane_length(ArpCore* a, int lane, int length);
void arp_set_length(ArpCore* a, int length);
void arp_set_step(ArpCore* a, int i, int prob, int shift, int vel, int gate);
/* Loop order: length 1..16 and mutation 0..100 % (0 = the loop is frozen). */
void arp_set_loop(ArpCore* a, int notes_pct, int loop_len);
/* Step mutation 0..100 %: when a step plays, with this chance one of its rows (the step that row
 * reads) drifts by one notch; 0 = the drift is frozen. */
void arp_set_mut_steps(ArpCore* a, int pct);
/* Seed of the mutation generator; arp_reset_mutation() makes a new random loop and clears the drift. */
void arp_seed_mutation(ArpCore* a, uint32_t seed);
void arp_reset_mutation(ArpCore* a);
/* Clear the drift of one row of step i (the step was edited). */
void arp_clear_mutation(ArpCore* a, int i, int lane);
/* Value of row lane (prob, vel, gate) drawn as v after a drift of d notches; shift is returned as is. */
int  arp_drifted(const ArpCore* a, int lane, int v, int d);
/* One notch of drift for row lane of step i (what Steps % does when the step plays). */
void arp_mutate_step(ArpCore* a, int i, int lane);
/* Transpose all generated notes by -2..+2 octaves (takes effect on the next step). */
void arp_set_transpose(ArpCore* a, int octaves);
/* Repeats 1..5 / ARP_RPT_RND3 / ARP_RPT_RND5 and their mode ARP_RPT_* (takes effect on the next step). */
void arp_set_repeats(ArpCore* a, int rpt, int mode);
/* Groove: swing 50..75 %, feel ARP_FEEL_*, drunk 0..100 %, accent 0..100 %. */
void arp_set_groove(ArpCore* a, int swing, int feel, int drunk, int accent);
/* Delay of grid step s in ticks (computed once per step: Drunk advances), see arp_set_groove(). */
double arp_step_offset(ArpCore* a, double s);
/* Passing notes: chance 0..100 % and style ARP_PASS_*. */
void arp_set_passing(ArpCore* a, int pct, int style);
/* Key 0..11 (C..B) or ARP_KEY_AUTO, and scale for Shift. */
void arp_set_scale(ArpCore* a, int key, int scale);
/* note + n semitones, pulled into the scale (n != 0); may return a value outside 0..127. */
int  arp_shift_note(const ArpCore* a, int note, int n);
/* Latch on: the chord stays after the keys are released; the next chord (pressed after all keys
 * were released) replaces it. Latch off: only the keys still down are kept. */
void arp_set_latch(ArpCore* a, int on);
/* Returns 1 if nothing was being arpeggiated before this key. */
int  arp_note_on(ArpCore* a, uint8_t ch, uint8_t note, uint8_t vel);
void arp_note_off(ArpCore* a, uint8_t ch, uint8_t note);
/* All notes off: clears pressed keys and the latched chord. */
void arp_release_all(ArpCore* a);
/* Generate events with tick in [from, to). Steps fall on multiples of step_ticks. */
void arp_process(ArpCore* a, double from, double to, arp_emit_fn emit, void* user);
/* Turn off the generated note now (transport jump etc.). */
void arp_silence(ArpCore* a, double tick, arp_emit_fn emit, void* user);

#endif
