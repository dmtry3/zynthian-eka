/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tracker.h"
#include "../../third_party/aubio/types.h"
#include "../../third_party/aubio/fvec.h"
#include "../../third_party/aubio/pitch/pitchyinfast.h"
#include <stdlib.h>
#include <math.h>
#include <string.h>
#define HISTORY 256
struct Record { int64_t time, good_time; float note, confidence, hz; int valid; };
struct EkaTracker {
    aubio_pitchyinfast_t *pitch;
    fvec_t input, output, window_input;
    float result;
    unsigned rate, window, hop, position, records, write, profile;
    int64_t clock, last_good;
    float last_note, last_hz;
    struct Record history[HISTORY];
};
EkaTracker *eka_tracker_create(unsigned rate) {
    EkaTracker *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->rate = rate; p->window = 512;
    while (p->window < rate * .04) p->window *= 2;
    p->hop = p->window / 8;
    p->input.length = p->hop;
    p->input.data = calloc(p->hop, sizeof(float));
    p->window_input.length = p->window;
    p->window_input.data = calloc(p->window, sizeof(float));
    p->output.length = 1; p->output.data = &p->result;
    p->pitch = new_aubio_pitchyinfast(p->window);
    if (!p->pitch || !p->input.data || !p->window_input.data) { eka_tracker_destroy(p); return NULL; }
    aubio_pitchyinfast_set_tolerance(p->pitch, .15f);
    eka_tracker_reset(p);
    return p;
}
void eka_tracker_destroy(EkaTracker *p) {
    if (!p) return;
    if (p->pitch) del_aubio_pitchyinfast(p->pitch);
    free(p->input.data); free(p->window_input.data); free(p);
}
void eka_tracker_reset(EkaTracker *p) {
    p->position = p->records = p->write = 0; p->clock = 0;
    p->last_good = -1000000000; p->last_note = p->last_hz = 0;
    memset(p->input.data, 0, p->hop * sizeof(float));
    memset(p->window_input.data, 0, p->window * sizeof(float));
}
void eka_tracker_process(EkaTracker *p, const float *input, unsigned n, unsigned profile) {
    if (profile != p->profile) {
        p->profile = profile;
        aubio_pitchyinfast_set_tolerance(p->pitch, profile == 3 ? .20f : .15f);
    }
    for (unsigned i = 0; i < n; ++i) {
        float x = input ? input[i] : 0;
        p->input.data[p->position++] = isfinite(x) ? x : 0;
        ++p->clock;
        if (p->position < p->hop) continue;
        p->position = 0;
        memmove(p->window_input.data, p->window_input.data + p->hop, (p->window - p->hop) * sizeof(float));
        memcpy(p->window_input.data + p->window - p->hop, p->input.data, p->hop * sizeof(float));
        aubio_pitchyinfast_do(p->pitch, &p->window_input, &p->output);
        float hz = p->result > 0 ? p->rate / p->result : 0;
        float confidence = aubio_pitchyinfast_get_confidence(p->pitch);
        float power = 0;
        for (unsigned j = 0; j < p->hop; ++j) power += p->input.data[j] * p->input.data[j];
        if (power / p->hop < 3.162278e-7f) hz = 0; /* -65 dB RMS */
        float low = profile == 3 ? 75 : profile == 2 ? 220 : 60;
        float high = profile == 3 ? 1100 : profile == 1 ? 1600 : 2800;
        float min_conf = profile == 3 ? .72f : .85f;
        int64_t time = p->clock - p->window / 2;
        int good = p->clock >= p->window && isfinite(hz) && isfinite(confidence)
                   && confidence >= min_conf && hz >= low && hz <= high;
        if (good) {
            p->last_note = 69 + 12 * log2f(hz / 440);
            p->last_hz = hz; p->last_good = time;
        }
        struct Record r = {time, p->last_good, p->last_note,
                           isfinite(confidence) ? confidence : 0, p->last_hz, good};
        p->history[p->write] = r;
        p->write = (p->write + 1) % HISTORY;
        if (p->records < HISTORY) ++p->records;
    }
}
int eka_tracker_read(const EkaTracker *p, unsigned age, float *note, float *confidence, float *hz) {
    *note = -1; *confidence = *hz = 0;
    int64_t target = p->clock - age;
    const struct Record *best = NULL;
    int64_t distance = INT64_MAX;
    for (unsigned i = 0; i < p->records; ++i) {
        const struct Record *r = &p->history[i];
        int64_t d = llabs(r->time - target);
        if (d < distance) { best = r; distance = d; }
    }
    if (!best) return 0;
    *confidence = best->confidence;
    double hold = p->profile == 3 ? .08 : .04;
    if (best->good_time < 0 || target - best->good_time > p->rate * hold) return 0;
    *note = best->note; *hz = best->hz;
    return 1;
}
