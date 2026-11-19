/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <lv2/core/lv2.h>
#include <math.h>
#include <stdlib.h>
#include "../dsp/pitch.h"
#include "../dsp/delay.h"
#include "../dsp/tone.h"
#include "../dsp/tracker.h"
#include "../harmony/midi.h"
#include "../harmony/scale.h"
#include "../harmony/timing.h"
#include "tempo.h"
#include "scenes.h"
#include <lv2/atom/util.h>
#include <string.h>
#include "../harmony/chords.h"
#ifndef WEAVE_URI
#define WEAVE_URI "http://dmtry3.github.io/zynthian-eka/weave4"
#endif
#define VOICES 4
#define CHUNK 128
/* Ports 0..6 and their meanings are preserved from the one-voice prototype. */
enum { PITCH, DRY, WET, INPUT, LEFT, RIGHT, LATENCY, MODE, CHORD, VOICING,
       COUNT, PAN1, PITCH2, LEVEL2, PAN2, PITCH3, LEVEL3, PAN3, PITCH4, LEVEL4, PAN4,
       DRY_TIMING, ENABLE1, ENABLE2, ENABLE3, ENABLE4, SOLO,
       OCTAVE1, OCTAVE2, OCTAVE3, OCTAVE4, MASTER, SELECTED,
       INTERVAL1, INTERVAL2, INTERVAL3, INTERVAL4,
       PEAK1, PEAK2, PEAK3, PEAK4, ENABLED_MASK, INPUT_PEAK, OUTPUT_PEAK,
       AVAILABLE_MASK,
       TIME1, CONTROL = TIME1 + VOICES * 4,
       DELAY_ACTUAL1, DELAY_ACTUAL2, DELAY_ACTUAL3, DELAY_ACTUAL4,
       BPM, TEMPO_HOST, BEATS_PER_BAR, BEAT_UNIT, PORT_COUNT = EKA_PORT_COUNT };
static const unsigned pitch_ports[VOICES] = { PITCH, PITCH2, PITCH3, PITCH4 };
static const unsigned level_ports[VOICES] = { WET, LEVEL2, LEVEL3, LEVEL4 };
static const unsigned pan_ports[VOICES] = { PAN1, PAN2, PAN3, PAN4 };
static const float pitch_defaults[VOICES] = { 7, 4, 10, 12 };
typedef struct {
    EkaPitch *dsp[VOICES];
    EkaDelay *delay[VOICES];
    EkaTone tone[VOICES];
    EkaTempo tempo;
    EkaLeading leading;
    EkaTracker *tracker;
    EkaMidi midi;
    EkaScale scale;
    unsigned tracked_mode;
    float scale_controls[EKA_SCALE_SIZE];
    uint64_t clock, tracking_epoch;
    int tracking_mode, tracking_dirty;
    unsigned midi_channel, profile;
    float tracked_pitch[VOICES], midi_octave[VOICES], tracking_gain[VOICES];
    float input_note, confidence, input_hz;
    unsigned tracked_valid;
    EkaTimeURIDs time_ids;
    float *ports[PORT_COUNT], pan[VOICES], pan_smoothing, master;
    float peaks[VOICES], input_peak, output_peak;
    float level[VOICES], monitor_gain[VOICES];
    double rate;
    int started;
    unsigned scene;
} Plugin;
static float value(const Plugin *p, unsigned port, float fallback, float low, float high) {
    if (p->scene && port >= EKA_SCALE_BASE && port < EKA_SCALE_BASE + EKA_SCALE_SIZE) {
        port = EKA_SCALE_BANK + (p->scene - 1) * EKA_SCALE_SIZE + port - EKA_SCALE_BASE;
    } else if (p->scene && port >= EKA_CHARACTER_BASE && port < EKA_CHARACTER_BASE + EKA_CHARACTER_SIZE) {
        port = EKA_CHARACTER_BANK + (p->scene - 1) * EKA_CHARACTER_SIZE + port - EKA_CHARACTER_BASE;
    } else if (p->scene && port >= EKA_TONE_BASE && port < EKA_TONE_BASE + EKA_TONE_SIZE) {
        port = EKA_TONE_BANK + (p->scene - 1) * EKA_TONE_SIZE + port - EKA_TONE_BASE;
    } else if (p->scene) {
        for (unsigned i = 0; i < EKA_SCENE_SIZE; ++i)
            if (eka_scene_ports[i] == port) {
                port = EKA_SCENE_BANK + (p->scene - 1) * EKA_SCENE_SIZE + i;
                break;
            }
    }
    float x = p->ports[port] ? *p->ports[port] : fallback;
    return isfinite(x) ? fminf(high, fmaxf(low, x)) : fallback;
}
static LV2_Handle instantiate(const LV2_Descriptor *d, double rate, const char *path,
                             const LV2_Feature *const *features) {
    (void)d; (void)path;
    if (!isfinite(rate) || rate < 8000 || rate > 192000) return NULL;
    Plugin *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    for (unsigned v = 0; v < VOICES; ++v) {
        p->dsp[v] = eka_pitch_create(rate);
        p->delay[v] = eka_delay_create(rate);
        eka_tone_reset(&p->tone[v], rate);
        if (!p->dsp[v] || !p->delay[v]) {
            for (unsigned i = 0; i <= v; ++i) {
                eka_pitch_destroy(p->dsp[i]);
                eka_delay_destroy(p->delay[i]);
            }
            free(p); return NULL;
        }
        eka_pitch_schedule(p->dsp[v], v, VOICES);
    }
    p->tracker = eka_tracker_create((unsigned)rate);
    if (!p->tracker) {
        for (unsigned v = 0; v < VOICES; ++v) {
            eka_pitch_destroy(p->dsp[v]); eka_delay_destroy(p->delay[v]);
        }
        free(p); return NULL;
    }
    eka_midi_reset(&p->midi);
    p->tempo = (EkaTempo){120, 4, 4, 0};
    if (features) {
        for (unsigned i = 0; features[i]; ++i) {
            if (!strcmp(features[i]->URI, LV2_URID__map) && features[i]->data) {
                LV2_URID_Map *map = (LV2_URID_Map *)features[i]->data;
                if (map->map) eka_time_map(&p->time_ids, map);
            }
        }
    }
    p->rate = rate;
    p->master = 1;
    p->pan_smoothing = (float)(1 - exp(-1 / (rate * .010)));
    return p;
}
static void connect_port(LV2_Handle instance, uint32_t port, void *data) {
    if (port < PORT_COUNT) ((Plugin *)instance)->ports[port] = data;
}
static void activate(LV2_Handle instance) {
    Plugin *p = instance;
    for (unsigned v = 0; v < VOICES; ++v) {
        eka_pitch_reset(p->dsp[v]);
        eka_delay_reset(p->delay[v]);
        eka_tone_reset(&p->tone[v], p->rate);
        p->pan[v] = 0;
        p->level[v] = p->monitor_gain[v] = 0;
    }
    p->master = 1;
    p->tempo = (EkaTempo){120, 4, 4, 0};
    p->input_peak = p->output_peak = 0;
    for (unsigned v = 0; v < VOICES; ++v) p->peaks[v] = 0;
    p->started = 0;
    eka_leading_reset(&p->leading);
    eka_tracker_reset(p->tracker); eka_midi_reset(&p->midi); eka_scale_reset(&p->scale);
    p->clock = 0; p->tracking_epoch = UINT64_MAX; p->tracking_mode = 0; p->tracking_dirty = 1;
    memset(p->tracking_gain, 0, sizeof(p->tracking_gain));
}
static void tracking_update(Plugin *p) {
    int scale_mode = p->tracked_mode == 3;
    static const float defaults[EKA_SCALE_SIZE] = {0,0,2,4,6,7};
    if (scale_mode) for (unsigned i = 0; i < EKA_SCALE_SIZE; ++i) {
        float x = lroundf(value(p, EKA_SCALE_BASE + i, defaults[i], i < 2 ? 0 : -7,
                               i == 0 ? 11 : i == 1 ? EKA_SCALE_COUNT - 1 : 7));
        if (x != p->scale_controls[i]) p->tracking_dirty = 1;
        p->scale_controls[i] = x;
    }
    uint64_t epoch = p->clock / CHUNK;
    for (unsigned v = 0; v < VOICES; ++v) {
        float octave = lroundf(value(p, OCTAVE1 + v, 0, -1, 1));
        if (octave != p->midi_octave[v]) p->tracking_dirty = 1;
        p->midi_octave[v] = octave;
    }
    if (!p->tracking_dirty && p->tracking_epoch == epoch) return;
    p->tracking_dirty = 0; p->tracking_epoch = epoch;
    if (!scale_mode) eka_midi_select(&p->midi, p->midi_channel);
    int tracked = eka_tracker_read(p->tracker, eka_pitch_tracking_age(p->dsp[0]),
                                  &p->input_note, &p->confidence, &p->input_hz);
    float targets[VOICES] = {-1,-1,-1,-1};
    if (scale_mode && !tracked) eka_scale_reset(&p->scale);
    if (scale_mode && tracked) {
        int steps[VOICES];
        for (unsigned v = 0; v < VOICES; ++v) steps[v] = (int)p->scale_controls[2 + v];
        eka_scale_targets(&p->scale, (unsigned)p->scale_controls[0],
                          (unsigned)p->scale_controls[1], p->input_note, steps, targets);
    }
    p->tracked_valid = 0;
    for (unsigned v = 0; v < VOICES; ++v) {
        float target = scale_mode ? targets[v] : p->midi.target[v];
        float sounding = target + (scale_mode ? 0 : 12 * p->midi_octave[v]);
        float shift = sounding - p->input_note;
        /* Never fold unreachable targets. Allow 1 cent of detector roundoff at range edges. */
        if (tracked && target >= 0 && shift >= -12.01f && shift <= 21.01f) {
            p->tracked_pitch[v] = fminf(21, fmaxf(-12, shift)); p->tracked_valid |= 1u << v;
        }
        if (p->ports[EKA_MIDI_NOTE + v]) *p->ports[EKA_MIDI_NOTE + v] = scale_mode ? -1 : target;
        if (p->ports[EKA_TARGET_NOTE + v]) *p->ports[EKA_TARGET_NOTE + v] = target < 0 ? -1 : sounding;
    }
    if (p->ports[EKA_INPUT_NOTE]) *p->ports[EKA_INPUT_NOTE] = p->input_note;
    if (p->ports[EKA_CONFIDENCE]) *p->ports[EKA_CONFIDENCE] = p->confidence;
    if (p->ports[EKA_INPUT_HZ]) *p->ports[EKA_INPUT_HZ] = p->input_hz;
    if (p->ports[EKA_HELD_NOTES]) *p->ports[EKA_HELD_NOTES] = scale_mode ? 0 : p->midi.count;
}
static void run_span(Plugin *p, uint32_t base, uint32_t n) {
    unsigned count = (unsigned)lroundf(value(p, COUNT, 1, 0, 4));
    float pitch[VOICES], target_pan[VOICES], level_target[VOICES], monitor_target[VOICES];
    unsigned note_count = VOICES;
    unsigned mode = (unsigned)lroundf(value(p, MODE, 0, 0, 3));
    if (mode < 2) for (unsigned v = 0; v < VOICES; ++v) p->tracking_gain[v] = 1;
    unsigned available_mask = 15;
    if (mode >= 2) {
        eka_leading_reset(&p->leading);
        tracking_update(p);
        memcpy(pitch, p->tracked_pitch, sizeof(pitch));
        available_mask = p->tracked_valid;
    } else if (mode == 1) {
        unsigned chord = (unsigned)lroundf(value(p, CHORD, 0, 0, EKA_CHORD_COUNT - 1));
        unsigned voicing = (unsigned)lroundf(value(p, VOICING, 0, 0, EKA_VOICING_COUNT - 1));
        float notes[VOICES];
        note_count = eka_chord_intervals(chord, voicing, notes);
        if (voicing == EKA_VOICING_LEAD)
            eka_leading_apply(&p->leading, notes, note_count, pitch);
        else {
            memcpy(pitch, notes, sizeof(pitch));
            eka_leading_seed(&p->leading, notes, note_count);
        }
        for (unsigned v = 0; v < VOICES; ++v) {
            float shifted = pitch[v] + 12 * lroundf(value(p, OCTAVE1 + v, 0, -1, 1));
            /* Keep chord roles intact when an octave would exceed the tested DSP range. */
            if (shifted >= -12 && shifted <= 21) pitch[v] = shifted;
        }
    } else {
        eka_leading_reset(&p->leading);
        for (unsigned v = 0; v < VOICES; ++v)
            pitch[v] = value(p, pitch_ports[v], pitch_defaults[v], -12, 12);
    }
    if (mode < 2) available_mask = (1u << note_count) - 1;
    unsigned latency = 0, enabled_mask = 0;
    unsigned solo = (unsigned)lroundf(value(p, SOLO, 0, 0, VOICES));
    float master_db = value(p, MASTER, 0, -60, 6);
    float master_target = master_db <= -60 ? 0 : powf(10, master_db / 20);
    if (!p->started) p->master = master_target;
    float block_peaks[VOICES] = {0}, input_peak = 0, output_peak = 0;
    float dry_db = value(p, DRY, 0, -60, 6);
    for (unsigned v = 0; v < VOICES; ++v) {
        float enable = value(p, ENABLE1 + v, -1, -1, 1);
        int has_note = mode == 2 ? p->midi.target[v] >= 0 : v < note_count;
        int on = enable < -.5f ? (mode == 2 ? has_note : v < count) : enable >= .5f;
        if (!has_note) on = 0;
        if (on) enabled_mask |= 1u << v;
        int audible = solo ? v + 1 == solo && has_note : on;
        float level_db = value(p, level_ports[v], v ? -12 : -6, -60, 6);
        level_target[v] = level_db <= -60 ? 0 : powf(10, level_db / 20);
        monitor_target[v] = !solo || v + 1 == solo ? 1 : 0;
        if (!p->started) {
            p->level[v] = level_target[v];
            p->monitor_gain[v] = monitor_target[v];
        }
        /* On gates the delay input; its already-recorded repeats can finish.
         * Level and Solo act after the delay, without erasing its history. */
        int source = audible && (level_target[v] > 0 || p->level[v] > 1e-6f);
        unsigned character_port = EKA_CHARACTER_BASE + v * 3;
        eka_pitch_character(p->dsp[v], value(p, character_port, 0, -50, 50),
                            value(p, character_port + 1, 0, 0, 100),
                            value(p, character_port + 2, .2f, .05f, 1));
        eka_pitch_set(p->dsp[v], pitch[v], v == 0 && !solo ? dry_db : -60, source ? 0 : -60);
        unsigned tone_port = EKA_TONE_BASE + v * 3;
        eka_tone_set(&p->tone[v], value(p, tone_port, 20, 20, 4000),
                     value(p, tone_port + 1, 20000, 200, 20000),
                     value(p, tone_port + 2, 0, 0, 1) >= .5f);
        unsigned time_port = TIME1 + v * 4;
        double milliseconds = value(p, time_port, 0, 0, EKA_DELAY_MAX_MS);
        if (value(p, time_port + 2, 0, 0, 1) >= .5f) {
            unsigned division = (unsigned)lroundf(value(p, time_port + 3, 12, 0, EKA_DIVISIONS - 1));
            milliseconds = eka_division_ms(division, p->tempo.bpm,
                                           p->tempo.beats_per_bar, p->tempo.beat_unit);
        }
        eka_delay_set(p->delay[v], milliseconds, value(p, time_port + 1, 0, 0, 90) / 100);
        if (p->ports[DELAY_ACTUAL1 + v])
            *p->ports[DELAY_ACTUAL1 + v] = (float)eka_delay_milliseconds(p->delay[v]);
        unsigned delay = eka_pitch_latency(p->dsp[v]);
        /* On/Solo controls do not erase canonical voice settings. */
        if ((on || audible) && delay > latency) latency = delay;
        target_pan[v] = value(p, pan_ports[v], 0, -1, 1);
        if (!p->started) p->pan[v] = target_pan[v];
    }
    /* Preserve legacy aligned-dry timing when all voices are off. */
    if (!latency) latency = eka_pitch_latency(p->dsp[0]);
    /* Disconnected port preserves legacy host behaviour; new instances default to Live in TTL. */
    int aligned_dry = value(p, DRY_TIMING, 1, 0, 1) >= .5f;
    int aligned_voices = value(p, EKA_VOICE_TIMING, 1, 0, 1) >= .5f;
    for (unsigned v = 0; v < VOICES; ++v) {
        eka_pitch_dry_alignment(p->dsp[v], aligned_dry);
        eka_pitch_voice_alignment(p->dsp[v], aligned_voices);
        eka_pitch_align(p->dsp[v], latency);
    }
    /* Bounded scratch buffers also make input/output aliasing safe. */
    for (uint32_t offset = 0; offset < n; ) {
        unsigned frames = n - offset > CHUNK ? CHUNK : n - offset;
        float input[CHUNK], left[CHUNK] = {0}, right[CHUNK] = {0};
        for (unsigned i = 0; i < frames; ++i)
            {
                float sample = p->ports[INPUT] ? p->ports[INPUT][base + offset + i] : 0;
                input[i] = isfinite(sample) ? sample : 0;
                float magnitude = fabsf(input[i]);
                if (magnitude > input_peak) input_peak = magnitude;
            }
        for (unsigned v = 0; v < VOICES; ++v) {
            float voice[CHUNK], dry[CHUNK];
            eka_pitch_process(p->dsp[v], frames, input, voice, dry);
            if (mode >= 2) for (unsigned i = 0; i < frames; ++i) {
                float target = (p->tracked_valid & (1u << v)) ? 1 : 0;
                p->tracking_gain[v] += p->pan_smoothing * (target - p->tracking_gain[v]);
                if (fabsf(target - p->tracking_gain[v]) < 1e-6f) p->tracking_gain[v] = target;
                /* Keep pitch input history while waiting for the first valid estimate.
                 * Gate before musical delay so existing repeats can finish. */
                voice[i] *= p->tracking_gain[v];
            }
            eka_delay_process(p->delay[v], frames, voice);
            eka_tone_process(&p->tone[v], frames, voice);
            for (unsigned i = 0; i < frames; ++i) {
                p->level[v] += p->pan_smoothing * (level_target[v] - p->level[v]);
                if (fabsf(level_target[v] - p->level[v]) < 1e-6f) p->level[v] = level_target[v];
                p->monitor_gain[v] += p->pan_smoothing * (monitor_target[v] - p->monitor_gain[v]);
                if (fabsf(monitor_target[v] - p->monitor_gain[v]) < 1e-6f)
                    p->monitor_gain[v] = monitor_target[v];
                voice[i] *= p->level[v] * p->monitor_gain[v];
                float magnitude = fabsf(voice[i]);
                if (magnitude > block_peaks[v]) block_peaks[v] = magnitude;
                p->pan[v] += p->pan_smoothing * (target_pan[v] - p->pan[v]);
                if (fabsf(target_pan[v] - p->pan[v]) < 1e-4f) p->pan[v] = target_pan[v];
                /* Linear balance: centre retains the legacy unity gain on both outputs. */
                float l = p->pan[v] > 0 ? 1 - p->pan[v] : 1;
                float r = p->pan[v] < 0 ? 1 + p->pan[v] : 1;
                left[i] += voice[i] * l + dry[i];
                right[i] += voice[i] * r + dry[i];
            }
        }
        if (mode >= 2) eka_tracker_process(p->tracker, input, frames, p->profile);
        for (unsigned i = 0; i < frames; ++i) {
            p->master += p->pan_smoothing * (master_target - p->master);
            if (fabsf(master_target - p->master) < 1e-6f) p->master = master_target;
            float l = p->master * left[i], r = p->master * right[i];
            float l_peak = fabsf(l), r_peak = fabsf(r);
            if (l_peak > output_peak) output_peak = l_peak;
            if (r_peak > output_peak) output_peak = r_peak;
            if (p->ports[LEFT]) p->ports[LEFT][base + offset + i] = l;
            if (p->ports[RIGHT]) p->ports[RIGHT][base + offset + i] = r;
        }
        offset += frames;
    }
    float release = (float)exp(-(double)n / (p->rate * .15));
    for (unsigned v = 0; v < VOICES; ++v) {
        p->peaks[v] = fmaxf(block_peaks[v], p->peaks[v] * release);
        if (p->ports[EKA_FINE_ACTUAL + v])
            *p->ports[EKA_FINE_ACTUAL + v] = eka_pitch_fine_actual(p->dsp[v]);
        if (p->ports[EKA_TIME_ACTUAL + v])
            *p->ports[EKA_TIME_ACTUAL + v] = eka_pitch_time_actual(p->dsp[v]);
        if (p->ports[INTERVAL1 + v]) *p->ports[INTERVAL1 + v] = pitch[v];
        if (p->ports[PEAK1 + v]) *p->ports[PEAK1 + v] = p->peaks[v];
    }
    p->input_peak = fmaxf(input_peak, p->input_peak * release);
    p->output_peak = fmaxf(output_peak, p->output_peak * release);
    if (p->ports[ENABLED_MASK]) *p->ports[ENABLED_MASK] = enabled_mask;
    if (p->ports[AVAILABLE_MASK]) *p->ports[AVAILABLE_MASK] = available_mask;
    if (p->ports[INPUT_PEAK]) *p->ports[INPUT_PEAK] = p->input_peak;
    if (p->ports[OUTPUT_PEAK]) *p->ports[OUTPUT_PEAK] = p->output_peak;
    if (p->ports[EKA_SAMPLE_RATE]) *p->ports[EKA_SAMPLE_RATE] = (float)p->rate;
    if (p->ports[BPM]) *p->ports[BPM] = (float)p->tempo.bpm;
    if (p->ports[TEMPO_HOST]) *p->ports[TEMPO_HOST] = p->tempo.from_host;
    if (p->ports[BEATS_PER_BAR]) *p->ports[BEATS_PER_BAR] = (float)p->tempo.beats_per_bar;
    if (p->ports[BEAT_UNIT]) *p->ports[BEAT_UNIT] = (float)p->tempo.beat_unit;
    if (n) p->started = 1;
    if (p->ports[LATENCY]) *p->ports[LATENCY] = aligned_dry ? latency : 0;
}
static void run_audio(Plugin *p, uint32_t base, uint32_t n) {
    if (!p->tracking_mode) { run_span(p, base, n); p->clock += n; return; }
    if (!n) { run_span(p, base, 0); return; }
    while (n) {
        unsigned frames = CHUNK - p->clock % CHUNK;
        if (frames > n) frames = n;
        run_span(p, base, frames);
        p->clock += frames; base += frames; n -= frames;
    }
}
static void run(LV2_Handle instance, uint32_t n) {
    Plugin *p = instance;
    uint32_t processed = 0;
    /* Scene A uses the original canonical ports. B-D are ordinary persistent
     * LV2 controls; a selector changes the complete bank in one block. */
    unsigned mask = 1;
    for (unsigned b = 1; b < 4; ++b)
        if (value(p, EKA_SCENE_READY + b - 1, 0, 0, 1) >= .5f) mask |= 1u << b;
    unsigned wanted = (unsigned)lroundf(value(p, EKA_SCENE_SELECT, 0, 0, 3));
    p->scene = mask & (1u << wanted) ? wanted : 0;
    if (p->ports[EKA_SCENE_ACTIVE]) *p->ports[EKA_SCENE_ACTIVE] = p->scene;
    if (p->ports[EKA_SCENE_MASK]) *p->ports[EKA_SCENE_MASK] = mask;
    unsigned mode = (unsigned)lroundf(value(p, MODE, 0, 0, 3));
    unsigned profile = (unsigned)lroundf(value(p, EKA_TRACKING_PROFILE, 0, 0, 2));
    unsigned channel = (unsigned)lroundf(value(p, EKA_MIDI_CHANNEL, 0, 0, 16));
    if ((mode >= 2) != p->tracking_mode || profile != p->profile) {
        eka_tracker_reset(p->tracker); eka_scale_reset(&p->scale); p->tracking_dirty = 1;
        memset(p->tracking_gain, 0, sizeof(p->tracking_gain));
    }
    if (channel != p->midi_channel) p->tracking_dirty = 1;
    if (mode != p->tracked_mode) { p->tracking_dirty = 1; eka_scale_reset(&p->scale); }
    p->tracked_mode = mode;
    p->tracking_mode = mode >= 2; p->profile = profile; p->midi_channel = channel;
    if (!p->tracking_mode) {
        if (p->ports[EKA_INPUT_NOTE]) *p->ports[EKA_INPUT_NOTE] = -1;
        if (p->ports[EKA_CONFIDENCE]) *p->ports[EKA_CONFIDENCE] = 0;
        if (p->ports[EKA_INPUT_HZ]) *p->ports[EKA_INPUT_HZ] = 0;
        if (p->ports[EKA_HELD_NOTES]) *p->ports[EKA_HELD_NOTES] = 0;
        for (unsigned v = 0; v < VOICES; ++v) {
            if (p->ports[EKA_MIDI_NOTE + v]) *p->ports[EKA_MIDI_NOTE + v] = -1;
            if (p->ports[EKA_TARGET_NOTE + v]) *p->ports[EKA_TARGET_NOTE + v] = -1;
        }
    }
    const LV2_Atom_Sequence *sequence = (const LV2_Atom_Sequence *)p->ports[CONTROL];
    if (p->time_ids.sequence && sequence && sequence->atom.type == p->time_ids.sequence
        && sequence->atom.size >= sizeof(sequence->body) && sequence->atom.size <= 65536
        && (!sequence->body.unit || sequence->body.unit == p->time_ids.frame_unit)) {
        const uint8_t *cursor = (const uint8_t *)&sequence->body + sizeof(sequence->body);
        uint32_t remaining = sequence->atom.size - sizeof(sequence->body);
        unsigned events = 0;
        for (; events < 256 && remaining >= sizeof(LV2_Atom_Event); ++events) {
            const LV2_Atom_Event *event = (const LV2_Atom_Event *)cursor;
            if (event->body.size > remaining - sizeof(*event)) { eka_midi_reset(&p->midi); p->tracking_dirty = 1; break; }
            uint32_t size = sizeof(*event) + lv2_atom_pad_size(event->body.size);
            if (size > remaining) { eka_midi_reset(&p->midi); p->tracking_dirty = 1; break; }
            if (event->time.frames >= processed && event->time.frames <= n) {
                EkaTempo next = p->tempo;
                int midi = event->body.type == p->time_ids.midi;
                int tempo = !midi && eka_tempo_read(&p->time_ids, &event->body, &next);
                if (midi || tempo) {
                    uint32_t position = (uint32_t)event->time.frames;
                    if (position > processed) run_audio(p, processed, position - processed);
                    if (tempo) p->tempo = next;
                    if (midi) {
                        eka_midi_event(&p->midi, (const uint8_t *)(&event->body + 1), event->body.size);
                        if (mode == 2) p->tracking_dirty = 1;
                    }
                    processed = position;
                }
            }
            cursor += size;
            remaining -= size;
        }
        if (remaining) { eka_midi_reset(&p->midi); p->tracking_dirty = 1; }
    } else if (sequence && p->time_ids.sequence) {
        eka_midi_reset(&p->midi); p->tracking_dirty = 1;
    }
    run_audio(p, processed, n - processed);
}
static void cleanup(LV2_Handle instance) {
    Plugin *p = instance;
    for (unsigned v = 0; v < VOICES; ++v) {
        eka_pitch_destroy(p->dsp[v]);
        eka_delay_destroy(p->delay[v]);
    }
    eka_tracker_destroy(p->tracker);
    free(p);
}
static const LV2_Descriptor descriptor = { WEAVE_URI, instantiate, connect_port, activate, run, NULL, cleanup, NULL };
LV2_SYMBOL_EXPORT const LV2_Descriptor *lv2_descriptor(uint32_t index) {
    return index == 0 ? &descriptor : NULL;
}
