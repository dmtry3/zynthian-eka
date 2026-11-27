/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Eka Prism: LV2 plugin wrapper with time:Position BPM sync & MIDI Note tracking for Zynthian. */
#include "prism_core.h"

#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/core/lv2.h>
#include <lv2/midi/midi.h>
#include <lv2/time/time.h>
#include <lv2/urid/urid.h>

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define EKA_PRISM_URI "http://dmtry3.github.io/zynthian-eka/prism"

enum {
    PORT_IN_L = 0,
    PORT_IN_R,
    PORT_OUT_L,
    PORT_OUT_R,
    PORT_CONTROL,

    /* Page 1: Filter A */
    PORT_MODEL_A,
    PORT_CUTOFF_A,
    PORT_RES_A,
    PORT_MORPH_A,

    /* Page 2: Filter B */
    PORT_MODEL_B,
    PORT_CUTOFF_B,
    PORT_RES_B,
    PORT_MORPH_B,

    /* Page 3: Routing */
    PORT_ROUTING,
    PORT_LINK_MODE,
    PORT_AB_BALANCE,
    PORT_INVERT_B,

    /* Page 4: LFO */
    PORT_LFO_SHAPE,
    PORT_LFO_RATE,
    PORT_LFO_SYNC,
    PORT_LFO_PHASE,

    /* Page 5: Mod Depth */
    PORT_LFO_MOD,
    PORT_MORPH_MOD,
    PORT_ENV_MOD,
    PORT_ENV_SPEED,

    /* Page 6: Master */
    PORT_DRIVE,
    PORT_MIX,
    PORT_OUTPUT_DB,
    PORT_BYPASS,

    /* Telemetry outputs (indices 29..38) */
    PORT_IN_PEAK,
    PORT_OUT_PEAK,
    PORT_EFF_CUT_A,
    PORT_EFF_CUT_B,
    PORT_EFF_MORPH_A,
    PORT_EFF_MORPH_B,
    PORT_LFO_A,
    PORT_LFO_B,
    PORT_ENV_LVL,
    PORT_HOST_BPM,

    PORT_COUNT
};

typedef struct {
    LV2_URID atom_object;
    LV2_URID atom_blank;
    LV2_URID atom_float;
    LV2_URID atom_double;
    LV2_URID atom_int;
    LV2_URID time_position;
    LV2_URID time_bpm;
    LV2_URID midi_event;
} PrismURIDs;

typedef struct {
    const float             *in_l;
    const float             *in_r;
    float                   *out_l;
    float                   *out_r;
    const LV2_Atom_Sequence *control;

    const float             *ctrl[PORT_IN_PEAK];
    float                   *out_ctrl[10];

    PrismURIDs               urids;
    double                   host_bpm;
    int                      active_note;
    PrismCore                core;
    PrismParams              params;
} EkaPrism;

static inline float read_port(const float *ptr, float fallback) {
    if (!ptr) return fallback;
    float v = *ptr;
    return isfinite(v) ? v : fallback;
}

static double atom_number(const PrismURIDs *u, const LV2_Atom *a) {
    const void *body = (const uint8_t *)a + sizeof(*a);
    if (a->type == u->atom_float && a->size == sizeof(float)) {
        float v; memcpy(&v, body, sizeof(v)); return (double)v;
    }
    if (a->type == u->atom_double && a->size == sizeof(double)) {
        double v; memcpy(&v, body, sizeof(v)); return v;
    }
    if (a->type == u->atom_int && a->size == sizeof(int32_t)) {
        int32_t v; memcpy(&v, body, sizeof(v)); return (double)v;
    }
    return NAN;
}

static void read_control_events(EkaPrism *self) {
    if (!self->control) return;
    LV2_ATOM_SEQUENCE_FOREACH(self->control, ev) {
        const LV2_Atom *atom = &ev->body;
        if (self->urids.midi_event && atom->type == self->urids.midi_event && atom->size >= 3) {
            const uint8_t *msg = (const uint8_t *)(atom + 1);
            uint8_t st = msg[0] & 0xF0u;
            if (st == 0x90u && msg[2] > 0) {
                self->active_note = (int)(msg[1] & 0x7Fu);
            } else if ((st == 0x80u || (st == 0x90u && msg[2] == 0)) && self->active_note == (int)(msg[1] & 0x7Fu)) {
                self->active_note = -1;
            }
            continue;
        }
        if (!self->urids.time_position) continue;
        if ((atom->type != self->urids.atom_object && atom->type != self->urids.atom_blank)
            || atom->size < sizeof(LV2_Atom_Object_Body)) {
            continue;
        }
        const LV2_Atom_Object *obj = (const LV2_Atom_Object *)atom;
        if (obj->body.otype != self->urids.time_position) continue;
        const uint8_t *cur = (const uint8_t *)&obj->body + sizeof(obj->body);
        uint32_t rem = atom->size - (uint32_t)sizeof(obj->body);
        while (rem >= sizeof(LV2_Atom_Property_Body)) {
            const LV2_Atom_Property_Body *prop = (const LV2_Atom_Property_Body *)cur;
            if (prop->value.size > rem - sizeof(*prop)) break;
            if (prop->key == self->urids.time_bpm) {
                double v = atom_number(&self->urids, &prop->value);
                if (isfinite(v) && v >= 20.0 && v <= 400.0) {
                    self->host_bpm = v;
                }
            }
            uint32_t sz = (uint32_t)sizeof(*prop) + lv2_atom_pad_size(prop->value.size);
            if (sz >= rem) break;
            cur += sz;
            rem -= sz;
        }
    }
}

static LV2_Handle instantiate(const LV2_Descriptor *descriptor,
                              double rate,
                              const char *bundle_path,
                              const LV2_Feature *const *features) {
    (void)descriptor;
    (void)bundle_path;

    EkaPrism *self = (EkaPrism *)calloc(1, sizeof(EkaPrism));
    if (!self) return NULL;
    self->host_bpm = 120.0;
    self->active_note = -1;

    if (features) {
        for (int i = 0; features[i]; ++i) {
            if (strcmp(features[i]->URI, LV2_URID__map) == 0) {
                LV2_URID_Map *map = (LV2_URID_Map *)features[i]->data;
                if (map && map->map) {
                    self->urids.atom_object   = map->map(map->handle, LV2_ATOM__Object);
                    self->urids.atom_blank    = map->map(map->handle, LV2_ATOM__Blank);
                    self->urids.atom_float    = map->map(map->handle, LV2_ATOM__Float);
                    self->urids.atom_double   = map->map(map->handle, LV2_ATOM__Double);
                    self->urids.atom_int      = map->map(map->handle, LV2_ATOM__Int);
                    self->urids.time_position = map->map(map->handle, LV2_TIME__Position);
                    self->urids.time_bpm      = map->map(map->handle, LV2_TIME__beatsPerMinute);
                    self->urids.midi_event    = map->map(map->handle, LV2_MIDI__MidiEvent);
                }
            }
        }
    }

    prism_params_default(&self->params);
    if (prism_core_init(&self->core, rate) != 0) {
        free(self);
        return NULL;
    }
    return (LV2_Handle)self;
}

static void connect_port(LV2_Handle instance, uint32_t port, void *data) {
    EkaPrism *self = (EkaPrism *)instance;
    if (!self || port >= PORT_COUNT) return;

    if (port == PORT_IN_L) {
        self->in_l = (const float *)data;
    } else if (port == PORT_IN_R) {
        self->in_r = (const float *)data;
    } else if (port == PORT_OUT_L) {
        self->out_l = (float *)data;
    } else if (port == PORT_OUT_R) {
        self->out_r = (float *)data;
    } else if (port == PORT_CONTROL) {
        self->control = (const LV2_Atom_Sequence *)data;
    } else if (port < PORT_IN_PEAK) {
        self->ctrl[port] = (const float *)data;
    } else {
        uint32_t oi = port - PORT_IN_PEAK;
        if (oi < 10) self->out_ctrl[oi] = (float *)data;
    }
}

static void activate(LV2_Handle instance) {
    EkaPrism *self = (EkaPrism *)instance;
    if (self) {
        self->active_note = -1;
        prism_core_reset(&self->core);
    }
}

static void run(LV2_Handle instance, uint32_t nframes) {
    EkaPrism *self = (EkaPrism *)instance;
    if (!self || !self->in_l || !self->in_r || !self->out_l || !self->out_r) return;

    read_control_events(self);

    PrismParams *p = &self->params;
    p->model_a        = (int)lroundf(read_port(self->ctrl[PORT_MODEL_A],     0.0f));
    p->cutoff_a_hz    = read_port(self->ctrl[PORT_CUTOFF_A],               650.0f);
    p->res_a_pct      = read_port(self->ctrl[PORT_RES_A],                   35.0f);
    p->morph_a_pct    = read_port(self->ctrl[PORT_MORPH_A],                  0.0f);

    p->model_b        = (int)lroundf(read_port(self->ctrl[PORT_MODEL_B],     1.0f));
    p->cutoff_b_hz    = read_port(self->ctrl[PORT_CUTOFF_B],              1800.0f);
    p->res_b_pct      = read_port(self->ctrl[PORT_RES_B],                   40.0f);
    p->morph_b_pct    = read_port(self->ctrl[PORT_MORPH_B],                 25.0f);

    p->routing        = (int)lroundf(read_port(self->ctrl[PORT_ROUTING],     0.0f));
    p->link_mode      = (int)lroundf(read_port(self->ctrl[PORT_LINK_MODE],   0.0f));
    p->ab_balance_pct = read_port(self->ctrl[PORT_AB_BALANCE],               0.0f);
    p->invert_b       = read_port(self->ctrl[PORT_INVERT_B],                 0.0f) >= 0.5f ? 1 : 0;

    p->lfo_shape      = (int)lroundf(read_port(self->ctrl[PORT_LFO_SHAPE],   0.0f));
    p->lfo_rate_hz    = read_port(self->ctrl[PORT_LFO_RATE],                 0.80f);
    p->lfo_sync       = read_port(self->ctrl[PORT_LFO_SYNC],                 0.0f) >= 0.5f ? 1 : 0;
    p->lfo_phase_deg  = read_port(self->ctrl[PORT_LFO_PHASE],               90.0f);

    p->lfo_mod_pct    = read_port(self->ctrl[PORT_LFO_MOD],                 25.0f);
    p->morph_mod_pct  = read_port(self->ctrl[PORT_MORPH_MOD],                0.0f);
    p->env_mod_pct    = read_port(self->ctrl[PORT_ENV_MOD],                  0.0f);
    p->env_speed_pct  = read_port(self->ctrl[PORT_ENV_SPEED],               50.0f);

    p->drive_pct      = read_port(self->ctrl[PORT_DRIVE],                   15.0f);
    p->mix_pct        = read_port(self->ctrl[PORT_MIX],                    100.0f);
    p->output_db      = read_port(self->ctrl[PORT_OUTPUT_DB],                0.0f);
    p->bypass         = read_port(self->ctrl[PORT_BYPASS],                   0.0f) >= 0.5f ? 1 : 0;

    p->host_bpm       = (float)self->host_bpm;
    p->midi_note      = self->active_note;

    prism_core_process(&self->core, p, self->in_l, self->in_r, self->out_l, self->out_r, nframes);

    if (self->out_ctrl[0]) *self->out_ctrl[0] = self->core.meters.in_peak_db;
    if (self->out_ctrl[1]) *self->out_ctrl[1] = self->core.meters.out_peak_db;
    if (self->out_ctrl[2]) *self->out_ctrl[2] = self->core.meters.eff_cut_a;
    if (self->out_ctrl[3]) *self->out_ctrl[3] = self->core.meters.eff_cut_b;
    if (self->out_ctrl[4]) *self->out_ctrl[4] = self->core.meters.eff_morph_a;
    if (self->out_ctrl[5]) *self->out_ctrl[5] = self->core.meters.eff_morph_b;
    if (self->out_ctrl[6]) *self->out_ctrl[6] = self->core.meters.lfo_a;
    if (self->out_ctrl[7]) *self->out_ctrl[7] = self->core.meters.lfo_b;
    if (self->out_ctrl[8]) *self->out_ctrl[8] = self->core.meters.env_lvl;
    if (self->out_ctrl[9]) *self->out_ctrl[9] = self->core.meters.host_bpm;
}

static void deactivate(LV2_Handle instance) {
    (void)instance;
}

static void cleanup(LV2_Handle instance) {
    EkaPrism *self = (EkaPrism *)instance;
    if (!self) return;
    prism_core_free(&self->core);
    free(self);
}

static const void *extension_data(const char *uri) {
    (void)uri;
    return NULL;
}

static const LV2_Descriptor descriptor = {
    EKA_PRISM_URI,
    instantiate,
    connect_port,
    activate,
    run,
    deactivate,
    cleanup,
    extension_data
};

LV2_SYMBOL_EXPORT const LV2_Descriptor *lv2_descriptor(uint32_t index) {
    return (index == 0) ? &descriptor : NULL;
}
