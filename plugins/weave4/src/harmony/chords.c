/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "chords.h"
#include <limits.h>
#include <math.h>
#include <string.h>
typedef struct { unsigned count; float notes[4]; } Chord;
static const Chord chords[EKA_CHORD_COUNT] = {
    {2, {4, 7}},          /* Major */
    {2, {3, 7}},          /* Minor */
    {3, {4, 7, 10}},      /* 7 */
    {3, {4, 7, 11}},      /* maj7 */
    {3, {3, 7, 10}},      /* m7 */
    {4, {4, 7, 11, 14}},  /* maj9 */
    {4, {3, 7, 10, 14}},  /* m9 */
    {4, {4, 7, 9, 14}},   /* 6/9 */
    {4, {4, 7, 10, 14}},  /* 9 */
    {4, {4, 10, 14, 21}}, /* 13, omit 5 and 11 */
    {4, {4, 7, 10, 15}},  /* 7#9 */
    {2, {2, 7}},          /* sus2 */
    {2, {5, 7}},          /* sus4 */
    {3, {3, 6, 9}}        /* dim7 */
};
unsigned eka_chord_intervals(unsigned chord, unsigned voicing, float pitches[4]) {
    if (chord >= EKA_CHORD_COUNT) chord = 0;
    if (voicing > 3) voicing = 0;
    const Chord *c = &chords[chord];
    for (unsigned i = 0; i < 4; ++i) {
        pitches[i] = c->notes[i];
        if (i >= c->count) continue;
        if (voicing == 1 && i % 2 == 0) pitches[i] -= 12; /* Open */
        if (voicing == 2) pitches[i] -= 12;              /* Low */
        if (voicing == 3 && i + 2 == c->count) pitches[i] -= 12; /* Drop 2 */
    }
    return c->count;
}

/* Search at most 4! * 3^4 complete assignments, only when the chord changes.
 * Compare movement, largest leap, crossings, then distance from Close register.
 * Tone order/octave iteration supplies a deterministic final tie-break. */
typedef struct {
    int target[4], previous[4], candidate[4], best[4];
    unsigned count, movement, leap, crossings, register_distance;
} LeadSearch;
static unsigned distance(int a, int b) { return (unsigned)(a > b ? a-b : b-a); }
static void leading_search(LeadSearch *s, unsigned voice, unsigned used,
                           unsigned movement, unsigned leap, unsigned reg) {
    if (movement > s->movement) return;
    if (voice == s->count) {
        unsigned crossings = 0;
        for (unsigned a = 0; a < s->count; ++a)
            for (unsigned b = a+1; b < s->count; ++b)
                if ((s->previous[a]-s->previous[b]) *
                    (s->candidate[a]-s->candidate[b]) < 0) ++crossings;
        int better = movement < s->movement ||
            (movement == s->movement && (leap < s->leap ||
             (leap == s->leap && (crossings < s->crossings ||
              (crossings == s->crossings && reg < s->register_distance)))));
        if (better) {
            s->movement=movement; s->leap=leap; s->crossings=crossings;
            s->register_distance=reg;
            memcpy(s->best,s->candidate,sizeof(s->best));
        }
        return;
    }
    for (unsigned tone=0;tone<s->count;++tone) {
        if (used & (1u<<tone)) continue;
        for (int octave=-2;octave<=1;++octave) {
            int pitch=s->target[tone]+12*octave;
            if (pitch < -12 || pitch > 21) continue;
            unsigned delta=distance(pitch,s->previous[voice]);
            s->candidate[voice]=pitch;
            leading_search(s,voice+1,used|(1u<<tone),movement+delta,
                           delta>leap?delta:leap,reg+distance(pitch,s->target[tone]));
        }
    }
}
void eka_leading_reset(EkaLeading *s) { memset(s,0,sizeof(*s)); }
void eka_leading_seed(EkaLeading *s,const float notes[4],unsigned count) {
    memcpy(s->previous,notes,sizeof(s->previous));
    s->count=count>4?4:count; s->valid=1; s->cached=0;
}
void eka_leading_apply(EkaLeading *s,const float notes[4],unsigned count,float out[4]) {
    if (count>4) count=4;
    if (s->cached && s->input_count==count && !memcmp(s->input,notes,sizeof(s->input))) {
        memcpy(out,s->previous,sizeof(s->previous)); return;
    }
    memcpy(out,notes,sizeof(s->previous));
    if (s->valid) {
        LeadSearch search={0};
        search.count=count;
        search.movement=search.leap=search.crossings=search.register_distance=UINT_MAX;
        for (unsigned v=0;v<count;++v) {
            search.target[v]=(int)lroundf(notes[v]);
            search.previous[v]=(int)lroundf(v<s->count?s->previous[v]:notes[v]);
        }
        leading_search(&search,0,0,0,0,0);
        for (unsigned v=0;v<count;++v) out[v]=(float)search.best[v];
    }
    eka_leading_seed(s,out,count);
    memcpy(s->input,notes,sizeof(s->input));
    s->input_count=count; s->cached=1;
}
