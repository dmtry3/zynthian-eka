/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pitch.h"
#include "character.h"
#include "backend.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
struct EkaPitch {
    EkaBackend *shifter;
    float *input, *output, *dry_ring, *wet_ring;
    unsigned block, position, schedule, capacity, write, native_latency, alignment, flush;
    double ratio, smoothing, dry_delay, wet_delay;
    float dry, wet, target_dry, target_wet, input_gate, dry_alignment, target_dry_alignment;
    float voice_alignment, target_voice_alignment;
    int started;
    unsigned voice;
    float base_pitch;
    double rate, human_delay, latency_pitch;
    EkaCharacter character;
};
static float control(float x, float fallback, float low, float high) {
    return isfinite(x) ? fminf(high, fmaxf(low, x)) : fallback;
}
static float gain(float db) { return db <= -60 ? 0 : powf(10, db / 20); }
EkaPitch *eka_pitch_create(double rate) {
    if (!isfinite(rate) || rate < 8000 || rate > 192000) return NULL;
    EkaPitch *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->shifter = eka_backend_create((unsigned)rate);
    if (!p->shifter) { free(p); return NULL; }
    p->block = eka_backend_block(p->shifter);
    p->capacity = (unsigned)rate / 2 + p->block + 16;
    p->input = calloc(p->block, sizeof(float));
    p->output = calloc(p->block, sizeof(float));
    p->dry_ring = calloc(p->capacity, sizeof(float));
    p->wet_ring = calloc(p->capacity, sizeof(float));
    if (!p->input || !p->output || !p->dry_ring || !p->wet_ring) {
        eka_pitch_destroy(p); return NULL;
    }
    p->rate = rate;
    p->smoothing = 1 - exp(-1 / (rate * .010));
    eka_pitch_reset(p);
    return p;
}
void eka_pitch_destroy(EkaPitch *p) {
    if (!p) return;
    if (p->shifter) eka_backend_destroy(p->shifter);
    free(p->input); free(p->output); free(p->dry_ring); free(p->wet_ring); free(p);
}
void eka_pitch_schedule(EkaPitch *p, unsigned voice, unsigned voices) {
    p->voice = voice;
    eka_character_reset(&p->character, p->rate, voice);
    p->schedule = voices ? (voice % voices) * p->block / voices : 0;
    p->position = p->schedule;
}
void eka_pitch_reset(EkaPitch *p) {
    eka_backend_reset(p->shifter);
    eka_backend_pitch(p->shifter, 1);
    memset(p->input, 0, p->block * sizeof(float));
    memset(p->output, 0, p->block * sizeof(float));
    memset(p->dry_ring, 0, p->capacity * sizeof(float));
    memset(p->wet_ring, 0, p->capacity * sizeof(float));
    p->position = p->schedule;
    p->write = p->flush = 0;
    p->ratio = 1;
    p->base_pitch = 0; p->human_delay = 0; p->latency_pitch = 0;
    eka_character_reset(&p->character, p->rate, p->voice);
    p->native_latency = p->block + eka_backend_delay(p->shifter);
    p->alignment = p->native_latency;
    p->dry_delay = p->alignment;
    p->wet_delay = 0;
    p->dry = p->target_dry = 1;
    p->wet = p->target_wet = 0;
    p->input_gate = 0;
    p->dry_alignment = p->target_dry_alignment = 1;
    p->voice_alignment = p->target_voice_alignment = 0;
    p->started = 0;
}
static void update_pitch(EkaPitch *p) {
    double shift = p->base_pitch + p->character.cents / 100.0;
    if (shift < -12) shift = -12;
    if (shift > 21) shift = 21;
    double ratio = pow(2, shift / 12.0);
    if (ratio != p->ratio) {
        eka_backend_pitch(p->shifter, ratio);
        p->ratio = ratio;
    }
}
void eka_pitch_character(EkaPitch *p, float fine, float amount, float speed) {
    eka_character_set(&p->character, fine, amount, speed);
}
float eka_pitch_fine_actual(const EkaPitch *p) {
    /* While asleep the library ratio may deliberately remain unchanged. Report
     * the bounded current setting that will be applied before the next shift. */
    double shift = p->base_pitch + p->character.cents / 100.0;
    if (shift < -12) shift = -12;
    if (shift > 21) shift = 21;
    return (float)((shift - p->base_pitch) * 100);
}
float eka_pitch_time_actual(const EkaPitch *p) {
    return (float)(p->human_delay * 1000 / p->rate);
}
void eka_pitch_set(EkaPitch *p, float pitch, float dry, float wet) {
    p->base_pitch = control(pitch, 0, -12, 21);
    /* Alignment follows the steady fine-tuned note, not every random pitch
     * excursion. Otherwise host block boundaries modulate the alignment itself. */
    double reference = p->base_pitch + p->character.target_fine / 100.0;
    if (reference < -12) reference = -12;
    if (reference > 21) reference = 21;
    if (reference != p->latency_pitch) {
        double scale = pow(2, reference / 12.0);
        eka_backend_pitch(p->shifter, scale);
        p->native_latency = p->block + eka_backend_delay(p->shifter);
        if (scale != p->ratio) eka_backend_pitch(p->shifter, p->ratio);
        p->latency_pitch = reference;
    }
    float level = gain(control(wet, -60, -60, 6));
    /* LiveShifter can do substantial lazy setup on pitch edits before its first
     * shift. Do not continually retune a sleeping voice for silent Humanize. */
    if (level > 0 || p->target_wet > 0 || p->flush > 0) update_pitch(p);
    /* Drain with silence before sleeping, keeping stale input out of reactivation.
     * Buffer positions continue advancing while asleep, preserving the schedule. */
    if (level == 0 && p->target_wet > 0) p->flush = p->capacity + p->block;
    p->target_dry = gain(control(dry, 0, -60, 6));
    p->target_wet = level;
    if (!p->started) {
        p->dry = p->target_dry;
        p->input_gate = p->target_wet > 0 ? 1.f : 0.f;
    }
}
void eka_pitch_align(EkaPitch *p, unsigned total_latency) {
    p->alignment = total_latency;
    if (!p->started) {
        p->dry_delay = p->alignment;
        p->wet_delay = p->target_voice_alignment ?
            (p->alignment > p->native_latency ? p->alignment - p->native_latency : 0) : 0;
    }
}
void eka_pitch_dry_alignment(EkaPitch *p, int aligned) {
    p->target_dry_alignment = aligned ? 1 : 0;
    if (!p->started) p->dry_alignment = p->target_dry_alignment;
}
void eka_pitch_voice_alignment(EkaPitch *p, int aligned) {
    p->target_voice_alignment = aligned ? 1 : 0;
    if (!p->started) p->voice_alignment = p->target_voice_alignment;
}
static float read_delay(const float *ring, unsigned capacity, unsigned write, double delay) {
    double pos = write - fmin(capacity - 2, fmax(0, delay));
    if (pos < 0) pos += capacity;
    unsigned a = (unsigned)pos;
    unsigned b = a + 1 == capacity ? 0 : a + 1;
    return ring[a] + (float)(pos - a) * (ring[b] - ring[a]);
}
void eka_pitch_process(EkaPitch *p, uint32_t n, const float *input, float *wet, float *dry) {
    double max_padding = p->alignment > p->native_latency ? p->alignment - p->native_latency : 0;
    for (uint32_t i = 0; i < n; ++i) {
        float x = input && isfinite(input[i]) ? input[i] : 0;
        int running = p->target_wet > 0 || p->flush > 0 || p->input_gate > 0;
        p->dry_ring[p->write] = x;
        p->wet_ring[p->write] = running ? p->output[p->position] : 0;
        p->dry_delay += p->smoothing * (p->alignment - p->dry_delay);
        p->voice_alignment += (float)p->smoothing * (p->target_voice_alignment - p->voice_alignment);
        if (fabsf(p->target_voice_alignment - p->voice_alignment) < 1e-6f)
            p->voice_alignment = p->target_voice_alignment;
        double padding = p->voice_alignment * max_padding;
        p->wet_delay += p->smoothing * (padding - p->wet_delay);
        p->human_delay += p->smoothing *
            (p->character.milliseconds * p->rate / 1000 - p->human_delay);
        if (fabs(p->human_delay) < 1e-12) p->human_delay = 0;
        p->dry += (float)p->smoothing * (p->target_dry - p->dry);
        p->wet += (float)p->smoothing * (p->target_wet - p->wet);
        if (fabsf(p->target_dry - p->dry) < 1e-6f) p->dry = p->target_dry;
        if (fabsf(p->target_wet - p->wet) < 1e-6f) p->wet = p->target_wet;
        float target_gate = p->target_wet > 0 ? 1.f : 0.f;
        p->input_gate += (float)p->smoothing * (target_gate - p->input_gate);
        if (fabsf(target_gate - p->input_gate) < 1e-6f) p->input_gate = target_gate;
        p->dry_alignment += (float)p->smoothing * (p->target_dry_alignment - p->dry_alignment);
        if (fabsf(p->target_dry_alignment - p->dry_alignment) < 1e-6f)
            p->dry_alignment = p->target_dry_alignment;
        float delayed_dry = read_delay(p->dry_ring, p->capacity, p->write, p->dry_delay);
        /* Crossfade the two dry paths without sweeping the delay tap to zero. */
        float dry_sample = p->dry * ((1 - p->dry_alignment) * x + p->dry_alignment * delayed_dry);
        float wet_sample = p->wet * read_delay(p->wet_ring, p->capacity, p->write, p->wet_delay + p->human_delay);
        p->input[p->position] = p->input_gate * x;
        if (wet) wet[i] = wet_sample;
        if (dry) dry[i] = dry_sample;
        if (++p->write == p->capacity) p->write = 0;
        if (++p->position == p->block) {
            eka_character_advance(&p->character, p->block);
            if (running) {
                update_pitch(p);
                eka_backend_process(p->shifter, p->input, p->output);
            }
            p->position = 0;
        }
        if (p->flush) --p->flush;
        p->started = 1;
    }
}
unsigned eka_pitch_latency(const EkaPitch *p) { return p->native_latency; }

unsigned eka_pitch_tracking_age(const EkaPitch *p) { return eka_backend_tracking_age(p->shifter); }
