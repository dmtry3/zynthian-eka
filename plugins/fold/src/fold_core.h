/* Eka Fold engine, without LV2: tested in WSL as a plain program (test/test_core.c).
 * Each voice has a Principal oscillator (Source: Sine, or one of the engines VA, FM2, Wave, Harm, PD of
 * src/plaits/, with Harmonics, Color, Morph) and a Mod OSC
 * (triangle, sine, saw, square; Free / Keytrack / Ratio) that modulates it through FM, AM, Sync and Timbre with
 * one Mod Index; then the Timbre section: three shapers (folder, saw, M) blended by Order, Timbre = depth,
 * Symmetry = offset before them, Drive = soft saturation after them; a DC blocker; then the lopass gate:
 * a vactrol (fast to open, slow to close, slower the darker it gets) driven by function generator 1 and the
 * velocity, which sets a 2-pole low-pass (LP), the level (VCA) or both (Combo). The mod matrix (stage 4b) moves
 * the knobs of a voice from its function generators, the Mod OSC, Random, velocity, key, mod wheel and aftertouch.
 * Stage 5a: a waveguide / delay in each voice after the gate. A soft clip on the output.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef FOLD_CORE_H
#define FOLD_CORE_H

#include <stdint.h>

#include "plaits/engines.h"
#include "plaits/resonators.h"

#define FC_VOICES 8               /* at most; Voices (FcParams.voices) sets how many play */
#define FC_GAIN_VOICES 4          /* FC_GAIN keeps this many voices at full velocity at 0 dBFS (more: the soft clip) */
#define FC_SINE_N 2048            /* table size (power of two); one more entry for interpolation */
#define FC_STEAL_MS 2.0f          /* a stolen voice fades out this fast before the new note starts */
#define FC_GAIN 0.25f             /* per voice: 1 / FC_GAIN_VOICES */
#define FC_SMOOTH_MS 5.0f         /* Mod Index and routing switches glide over this, not to click */

/* FM is linear and through zero: Mod Index 100 % swings the Principal frequency by FC_FM_DEPTH times itself.
 * Chosen by the aliasing test (test/alias_report.c, 2026-11-11): phase modulation aliased strongly with a saw or
 * square Mod OSC (a phase step is a step in the sound), linear FM stays clean to C6 at the plugin rate. */
#define FC_FM_DEPTH 4.0f

/* Timbre section. Folder: gain 1..FC_FOLD_GAIN into a triangle folder (Timbre 0 with Symmetry 0 = no change).
 * Saw and M: the sum of Chebyshev polynomials T_n / n (all n, or odd n), which turns a sine into exactly the
 * harmonics of a saw (square) up to n = N; N per voice from its note, below 0.45 of the rate, at most
 * FC_CHEB_MAX. Timbre crossfades the input into them. The Mod OSC adds up to FC_TIMBRE_MOD x Index to Timbre.
 * Drive: tanh saturation, k = 1 + FC_DRIVE_K x Drive, crossfaded in. */
#define FC_FOLD_GAIN 8.0f
#define FC_CHEB_MAX 32
#define FC_TIMBRE_MOD 1.0f
#define FC_DRIVE_K 4.0f
#define FC_DC_HZ 5.0f             /* DC blocker per voice (Symmetry, AM and the shapers make DC) */

/* Anti-aliasing: the voices run at 2x the plugin rate, with first-order ADAA on the folder, and a halfband
 * decimator on the mix. Chosen on 2026-11-11 from four modes (1x, ADAA, 2x, 2x + ADAA; test/alias_report.c,
 * fold_bench on the Pi 5): folder at Timbre 70 % on C5: -37 / -49 / -50 / -70 dB of aliasing; 4 voices with
 * everything on: 1.4 % of one Pi 5 core (4.3 % with the Saw shaper).
 * 2026-11-12: the engine voices (VA, FM2, Wave) were tried at 4x (cleaner by 8..22 dB through the folder, twice
 * their CPU on the Pi 5) and compared by ear: no clear difference, so everything stays at 2x (commit fbc05b7 has
 * the 4x code: the rate must be chosen at the note-on, a voice moving between two decimators clicks). */
#define FC_OS 2
#define FC_CHUNK 64               /* output samples rendered at a time (the 2x buffer) */
#define FC_HB_TAPS 63             /* halfband decimator */

/* Function generators (after the Buchla 281): Rise and Fall are linear segments, FC_FG_MIN_MS..FC_FG_MAX_MS.
 * Once: a note rises and falls, the key does not matter. Gate: rises, holds while the key (or the sustain
 * pedal) is down, falls. Cycle: rises and falls again while the key is down (an LFO), then ends its fall. */
enum { FC_FG_ONCE, FC_FG_GATE, FC_FG_CYCLE, FC_NFG_MODES };
enum { FC_SEG_IDLE, FC_SEG_RISE, FC_SEG_HOLD, FC_SEG_FALL };
#define FC_FG_MIN_MS 1.0f
#define FC_FG_MAX_MS 10000.0f

/* Lopass gate. The vactrol follows FG1 x velocity: it opens with FC_VAC_RISE_MS and closes with Decay
 * (FC_DECAY_MIN_MS..FC_DECAY_MAX_MS, logarithmic), up to 1 + FC_VAC_SLOW times slower as it goes dark (the
 * "acoustic" tail of a Buchla lopass gate). Its state g (0..1) sets the cutoff FC_LPG_FMIN..FC_LPG_FMAX
 * (exponential) in LP and Combo, and the level (x g) in VCA and Combo. Velocity opens it to
 * FC_VEL_FLOOR + (1 - FC_VEL_FLOOR) x velocity / 127. The voice ends when FG1 has ended and g is below
 * FC_GATE_OFF; it fades out over the last FC_GATE_OFF of g. Resonance 0 is a Butterworth low-pass; VCA bypasses
 * the filter (no top loss). */
enum { FC_GATE_VCA, FC_GATE_LP, FC_GATE_COMBO, FC_NGATE_MODES };
#define FC_VAC_RISE_MS 2.0f
#define FC_VAC_SLOW 2.0f
#define FC_DECAY_MIN_MS 10.0f
#define FC_DECAY_MAX_MS 2000.0f
#define FC_LPG_FMIN 20.0f
#define FC_LPG_FMAX 20000.0f
#define FC_VEL_FLOOR 0.4f
#define FC_GATE_OFF 1e-3f         /* -60 dB */
/* The band state of the low-pass saturates softly at FC_RES_SAT (as in an analog filter): at full Resonance a
 * voice peaks at 0.4 instead of 3.3 (the output clipped, 2026-11-12, found on the device); the small-signal
 * lift stays about +20 dB, Resonance 0 is unchanged within 0.5 dB. */
#define FC_RES_SAT 2.0f
#define FC_COEF_EVERY 32          /* the mod matrix and the filter coefficients: every 32 samples (of the voice) */
float fc_decay_ms(float decay);   /* Decay 0..1 -> ms */

/* Principal sources: the order of the scale points of Source in lv2/eka_fold.ttl and of the widget's tiles.
 * The engines are FE_* = source - 1. A change of Source crossfades a sounding voice over FC_SRC_FADE_MS. */
enum { FC_SRC_SINE, FC_SRC_VA, FC_SRC_FM2, FC_SRC_WAVE, FC_SRC_HARM, FC_SRC_PD, FC_NSOURCES };
#define FC_SRC_FADE_MS 10.0f

/* Waveguide / delay (TZ 4.6), in each voice after the lopass gate: a delay line whose output goes through a
 * peaking EQ and a soft shaper, times Feedback (100 % = FC_DL_FB_MAX: a string still ends), back into the line; the
 * voice hears (1 - Mix) of the gate and Mix of the line. Modes: Off (no CPU), Tuned (the line is a period of the
 * note moved by Time, -24..+24 semitones: a string, Karplus-Strong), Free (Time FC_DL_MIN_MS..FC_DL_MAX_MS,
 * cubic: echoes). Tuned subtracts the delay of the EQ at the note's frequency, so it stays in tune. The EQ
 * acts on every pass: in Tuned a cut of 1 dB already damps the string (high notes pass hundreds of times a second;
 * a fixed damping low-pass was tried and dropped: at 10 kHz it killed a C8 string in 0.1 s).
 * Shaper: tanh(g x) / g with g = 1 + FC_DL_DRIVE x Shaper^2, blended in over the first quarter of the knob: slope 1
 * at 0, so a quiet loop keeps its gain (tanh(3x) was tried and lifted it x1.6 at Shaper 30 %: the loop ran away
 * above Feedback 85 %, found on the device 2026-11-12), and it squeezes from 1 / g (tanh(x) alone squeezed only
 * above 1, where the line seldom goes: it was not heard). Then a limit: straight up to 1, softly to FC_DL_LIM above
 * (the loop stays bounded even at Feedback 100 % with the EQ up; a tanh limit coloured Shaper 0 at -37 dB). The voice hears the line after the EQ and the shaper (from the first pass). The voice lives on while its line is above FC_DL_OFF, after the gate has closed; a new note
 * clears the line. Time knob 0..FC_DL_TIME_MAX, read by the mode, like Mod Freq. */
/* Stage 7b: two resonators in the same place (src/plaits/resonators.h), excited by the voice: Modal (24 modes) and
 * String (a waveguide). Time = their pitch, -24..+24 semitones from the note (as Tuned); Feedback = their Decay
 * (0..100 %, the sign does not matter); Structure, Brightness, Position: their own parameters. Changing between
 * the line (Tuned / Free), Modal and String on a sounding voice fades out (FC_SMOOTH_MS) and starts the new one
 * empty. */
enum { FC_DL_OFF_MODE, FC_DL_TUNED, FC_DL_FREE, FC_DL_MODAL, FC_DL_STRING, FC_NDL_MODES };
#define FC_RES_IN 0.5f            /* the voice into Modal (its modes ring up to Q times their input) */
#define FC_RES_IN_STRING 1.0f     /* the voice into String (as loud as Tuned)  */
#define FC_DL_TIME_MAX 100.0f
#define FC_DL_SEMIS 24.0f
#define FC_DL_MIN_MS 1.0f
#define FC_DL_MAX_MS 2000.0f      /* Free: FC_DL_MAX_MS x (Time / 100)^3, at least FC_DL_MIN_MS (a log scale was too
                                     fine below 30 and too coarse above, 2026-11-12) */
#define FC_DL_LEN 262144          /* samples per voice (power of two): 2.73 s at 2 x 48 kHz, 1.36 s at 2 x 96 kHz */
#define FC_DL_FB_MAX 0.999f
#define FC_DL_EQ_Q 0.7f
#define FC_DL_EQ_MIN_HZ 100.0f    /* EQ Freq 0..100 % : 100 Hz .. 10 kHz, logarithmic */
#define FC_DL_EQ_MAX_HZ 10000.0f
#define FC_DL_EQ_DB 18.0f         /* EQ Gain -18..+18 dB (the LV2 parameter in tenths) */
#define FC_DL_LIM 2.0f
#define FC_DL_DRIVE 9.0f
#define FC_DL_OFF 1e-4f           /* -80 dB: the tail is over */
#define FC_DL_FREE_MS 50.0f       /* Free: Time glides over this (tape-like); Tuned follows in FC_SMOOTH_MS */
float fc_dl_semis(float units);   /* Tuned: Time knob -> semitones */
float fc_dl_ms(float units);      /* Free: Time knob -> ms */
float fc_dl_eq_hz(float units);   /* EQ Freq 0..100 -> Hz */

/* Mod matrix (TZ 4.5): cell[source][destination], -1..1 (-100..100 %), adds source x cell to the destination knob,
 * of each voice, every FC_COEF_EVERY samples; empty cells cost nothing. The order is that of the widget's grid
 * and of the LV2 parameters m_<source>_<destination> (fc_msrc_names, fc_mdst_names).
 * Sources: FG1, FG2 0..1; Mod OSC -1..1 (its wave, at the matrix rate); Random -1..1, a new value at every period
 * of the Mod OSC (and at the note-on); Velocity 0..1; Key -1..1, (note - 60) / FC_KEY_SPAN; Mod wheel (CC 1) 0..1;
 * Aftertouch 0..1 (channel or polyphonic, the larger).
 * Destinations: Pitch (100 % = FC_MOD_SEMIS semitones), Harmonics, Color, Morph, Mod Index, Order, Timbre (100 % =
 * the whole knob), Symmetry (100 % = from 0 to the end), LPG (the opening of the gate: x (1 + sum), so -100 %
 * keeps it shut and +100 % opens it twice as wide, up to fully; the voice still ends with FG1), Delay Time (100 %
 * = the whole Time knob), Delay Feedback (100 % = from 0 to the end). */
enum { FC_MS_FG1, FC_MS_FG2, FC_MS_MOD, FC_MS_RND, FC_MS_VEL, FC_MS_KEY, FC_MS_WHEEL, FC_MS_AT, FC_NMSRC };
enum { FC_MD_PITCH, FC_MD_HARM, FC_MD_COLOR, FC_MD_MORPH, FC_MD_INDEX, FC_MD_ORDER, FC_MD_SYM, FC_MD_TIMBRE,
       FC_MD_LPG, FC_MD_DTIME, FC_MD_DFB, FC_NMDST };
#define FC_NCELLS (FC_NMSRC * FC_NMDST)
#define FC_MOD_SEMIS 12.0f
#define FC_KEY_SPAN 48.0f
extern const char* const fc_msrc_names[FC_NMSRC];   /* fg1 fg2 mod rnd vel key wheel at */
extern const char* const fc_mdst_names[FC_NMDST];   /* pitch harmonics color morph index order symmetry timbre lpg
                                                       dtime dfb */

/* Play (stage 5b): Poly (up to Voices voices), Mono (one voice, the last key wins, letting it go returns to the
 * key still held before it, every note restarts the function generators), Legato (the same, but a key pressed
 * while another is held only moves the pitch, and only then it glides). Glide: the pitch moves linearly in
 * semitones over fc_glide_ms(Glide), from the voice's last note (Poly, Mono) or the held one (Legato). */
enum { FC_PLAY_POLY, FC_PLAY_MONO, FC_PLAY_LEGATO, FC_NPLAY };
#define FC_GLIDE_MAX_MS 2000.0f   /* Glide 0..100: FC_GLIDE_MAX_MS x (Glide / 100)^2 */
#define FC_KEYS 16                /* Mono / Legato: the keys held, newest last */
float fc_glide_ms(float units);

/* Mod OSC shapes and modes: the order of the scale points in lv2/eka_fold.ttl and of the widget's lists. */
enum { FC_MOD_TRI, FC_MOD_SINE, FC_MOD_SAW, FC_MOD_SQUARE, FC_NMOD_SHAPES };
enum { FC_MODE_FREE, FC_MODE_KEY, FC_MODE_RATIO, FC_NMODES };

/* Mod Freq knob, 0..FC_FREQ_MAX units, read by the mode:
 * Free: Hz, FC_FREE_MIN..FC_FREE_MAX, logarithmic; Keytrack: semitones from the note, -48..+48;
 * Ratio: one of FC_NRATIOS ratios to the Principal frequency. The widget has the same formulas. */
#define FC_FREQ_MAX 100.0f
#define FC_FREE_MIN 0.01f
#define FC_FREE_MAX 10000.0f
#define FC_KEY_RANGE 48.0f
#define FC_NRATIOS 25
extern const float fc_ratios[FC_NRATIOS];
float fc_free_hz(float units);
float fc_key_semis(float units);
int fc_ratio_index(float units);

typedef struct {
  float pitch;                    /* semitones */
  float fine;                     /* cents */
  int source;                     /* FC_SRC_* */
  float harmonics, color, morph;  /* 0..1: the engine's knobs (Sine has none) */
  float fg_rise_ms[2], fg_fall_ms[2];
  int fg_mode[2];
  int gate_mode;
  float gate_decay, gate_res, level;   /* 0..1 */
  float matrix[FC_NMSRC][FC_NMDST];   /* -1..1, see above */
  int delay_mode;                 /* FC_DL_* */
  float delay_time;               /* 0..FC_DL_TIME_MAX, read by the mode */
  float delay_fb;                 /* -1..1 */
  float delay_mix;                /* 0..1 */
  float delay_shaper;             /* 0..1 */
  float delay_eq_freq;            /* 0..1 -> fc_dl_eq_hz */
  float delay_eq_db;              /* -FC_DL_EQ_DB..FC_DL_EQ_DB */
  float res_structure, res_brightness, res_position;   /* 0..1: Modal, String */
  int play_mode;                  /* FC_PLAY_* */
  int voices;                     /* 1..FC_VOICES (Poly) */
  float glide;                    /* 0..100, fc_glide_ms */
  float mod_freq;                 /* units, see above */
  float mod_index;                /* 0..1 */
  int mod_shape, mod_mode;
  int route_fm, route_am, route_sync, route_timbre;
  float timbre, order, drive;     /* 0..1 */
  float symmetry;                 /* -1..1 */
} FcParams;

enum { FC_IDLE, FC_PLAY, FC_STEAL };

typedef struct {
  float lvl;                      /* 0..1 */
  int seg;                        /* FC_SEG_* */
} FcFg;

typedef struct {
  int stage;
  int note;
  float pitch;                    /* the note it sounds now (gliding to `note`), MIDI semitones */
  float pitch_step;               /* its glide per sample at the voices' rate */
  int played;                     /* it has had a note (Poly glides from it) */
  float vel;                      /* vactrol drive from the velocity, FC_VEL_FLOOR..1 */
  float velo;                     /* the velocity 0..1 (a matrix source) */
  float at;                       /* polyphonic aftertouch 0..1 */
  float rnd;                      /* Random: -1..1, new at every Mod OSC period */
  float mval;                     /* the Mod OSC wave at the last sample (a matrix source) */
  float md[FC_NMDST];             /* the matrix's sums for this voice (for the widget and the tests) */
  float idx_g, tim_g, ord_g, sym_g;   /* the targets of idx, tim, ord, sym: the knobs plus the matrix */
  float lpg_g;                    /* the gate's opening factor from the matrix, 0..2 */
  int snap;                       /* a new note: the first matrix values without the glide */
  float cheb_a[FC_CHEB_MAX + 1];  /* the Saw / M weights, recomputed when Order or the pitch moves */
  float cheb_o, cheb_inc;         /* what they were computed for (cheb_o < 0: not yet) */
  int cheb_n;
  int tail;                       /* the gate has closed, only the delay line still sounds */
  float dl_len, dl_len_g;         /* the delay in samples (smoothed), its target */
  float dl_fb, dl_fb_g;           /* Feedback, its target with the matrix */
  float dl_mix;                   /* Mix, smoothed (to 0 in Off: the line fades, then costs nothing) */
  float dl_pk;                    /* peak of what goes into the line, decaying over two of its lengths */
  float dl_pk_r;                  /* its decay per sample */
  int dl_wi;                      /* write index */
  int dl_used;                    /* samples of the line written since it was cleared (to clear only those) */
  float eq_x1, eq_x2, eq_y1, eq_y2;   /* EQ state */
  int dl_cur;                     /* what the line runs now: FC_DL_TUNED (also Free), FC_DL_MODAL, FC_DL_STRING, or 0 */
  FrModal modal;
  FrString string;                /* its line is dl_buf */
  float dl_buf[FC_DL_LEN];
  FcFg fg[2];
  float vac;                      /* vactrol state g, 0..1 */
  float wv, wl, lvl, res;         /* gate mode (VCA, LP shares), Level, Resonance: smoothed */
  float ic1, ic2;                 /* low-pass state (TPT state-variable filter) */
  float a1, a2, a3;               /* its coefficients, every FC_COEF_EVERY samples */
  int tick;                       /* samples to the next matrix / coefficient update */
  float sg;                       /* steal fade, 1 = none */
  float phase;                    /* Principal, cycles 0..1 (Sine) */
  int src, src_prev;              /* Source, and the one it fades from while xf < 1 */
  float xf;                       /* crossfade 0..1 */
  FeState fe[FE_NENGINES];        /* the engines' states (each one keeps its own phase) */
  float mphase;                   /* Mod OSC, cycles 0..1 */
  float idx, gfm, gam, gtim;      /* Mod Index and the FM / AM / Timbre switches, smoothed */
  float tim, ord, sym, drv;       /* Timbre knobs, smoothed */
  float au, aF;                   /* ADAA: last folder input and its antiderivative */
  float dcx, dcy;                 /* DC blocker */
  float y_prev;                   /* Principal, one sample late: the sync step is corrected on both sides */
  uint32_t age;                   /* note-on order: lower = older */
  int held;                       /* the key is down */
  int pedal;                      /* released while the sustain pedal was down */
  float steal_dec;                /* FC_STEAL: sg step per output sample */
  int next_note, next_vel;        /* FC_STEAL: the note that starts when the fade ends */
} FcVoice;

/* What depends on the voices' rate. */
typedef struct {
  float sr;                       /* the plugin rate x FC_OS */
  float step;                     /* plugin samples per voice sample: 1 / os */
  float smooth;                   /* one-pole coefficient for FC_SMOOTH_MS */
  float dc_r;                     /* DC blocker pole */
  float src_fade;                 /* Source crossfade step per sample */
  float fg_rise_inc[2], fg_fall_inc[2];   /* per sample */
  float vac_rise, vac_fall;       /* vactrol: one-pole steps per sample (fall before the slow-down) */
} FcRate;

typedef struct {
  float sr_out;                   /* the plugin rate */
  FcRate rt;
  FcVoice v[FC_VOICES];
  uint32_t clock;
  int sustain_pedal;
  int play_mode, nvoices;
  float glide_ms;
  int keys[FC_KEYS], key_vel[FC_KEYS];   /* Mono / Legato: the keys held, newest last */
  int nkeys;
  float bend;                     /* semitones */
  float tune;                     /* semitones: pitch + fine */
  int src_t;                      /* Source */
  float harm_t, color_t, morph_t; /* engine knobs */
  float fg_rise_ms[2], fg_fall_ms[2], decay_ms;
  int fg_mode[2];
  float wv_t, wl_t, lvl_t, res_t;
  float fmax;                     /* the highest cutoff */
  int mod_shape, mod_mode;
  float mod_hz, mod_semis, mod_ratio;   /* Mod Freq read for each mode */
  float idx_t, fm_t, am_t, tim_route_t;   /* targets of the smoothed values */
  float tim_t, ord_t, sym_t, drv_t;
  float hb[FC_HB_TAPS];           /* halfband taps 2x -> 1x */
  int dl_mode;                    /* FC_DL_* */
  float dl_time_u, dl_fb_t, dl_mix_t, dl_shape;
  float dl_g, dl_wsh;             /* the shaper's drive and its share */
  float res_s, res_b, res_p;      /* Structure, Brightness, Position */
  float eq_b0, eq_b1, eq_b2, eq_a1, eq_a2;   /* peaking EQ at the voices' rate */
  float dl_glide;                 /* Free: one-pole step of the delay time per sample */
  float hbuf[FC_HB_TAPS - 1 + FC_OS * FC_CHUNK];        /* 2x mix: history, then the chunk */
  int sync;
  int ncells;                     /* the non-empty matrix cells */
  uint8_t cell_src[FC_NCELLS], cell_dst[FC_NCELLS];
  float cell_amt[FC_NCELLS];
  float wheel, chan_at;           /* mod wheel, channel aftertouch: 0..1 */
  uint32_t rng;                   /* Random */
  float table[FC_SINE_N + 1];
} FoldCore;

void fc_init(FoldCore* c, float sr);
void fc_default_params(FcParams* p);
void fc_set_params(FoldCore* c, const FcParams* p);
void fc_note_on(FoldCore* c, int note, int vel);
void fc_note_off(FoldCore* c, int note);
void fc_set_sustain(FoldCore* c, int on);
void fc_set_bend(FoldCore* c, float semitones);
void fc_set_wheel(FoldCore* c, float v);              /* 0..1 */
void fc_set_aftertouch(FoldCore* c, float v);         /* channel pressure, 0..1 */
void fc_set_poly_aftertouch(FoldCore* c, int note, float v);   /* 0..1 */
void fc_all_notes_off(FoldCore* c);    /* every key and the pedal up (CC 123) */
void fc_all_sound_off(FoldCore* c);    /* every voice fades out in FC_STEAL_MS (CC 120) */
/* Renders n samples into l and r (overwrites them). */
void fc_process(FoldCore* c, float* l, float* r, uint32_t n);
/* MIDI note of voice i for the widget (the coming note while a voice is stolen), -1 = idle. */
int fc_voice_note(const FoldCore* c, int i);
int fc_active_voices(const FoldCore* c);
/* For the widget, from the newest sounding voice (-1 = none): the Mod OSC phase 0..1; the vactrol state g 0..1;
 * where function generator i is: 0..1 rising (= its level), 1.5 holding, 2..3 falling (2 + 1 - level),
 * 3 ended. */
float fc_mod_phase(const FoldCore* c);
float fc_gate_level(const FoldCore* c);
float fc_fg_pos(const FoldCore* c, int i);
/* Peak level in the delay line of the newest sounding voice (-1 = none, 0 with the delay Off). */
float fc_delay_level(const FoldCore* c);
/* Flush denormals to zero in this thread (call at the start of run()). */
void fc_enable_ftz(void);

#endif
