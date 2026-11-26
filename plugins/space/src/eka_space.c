/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Eka Space: LV2 plugin wrapper with time:Position BPM sync for Zynthian. */
#include "space_core.h"

#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/core/lv2.h>
#include <lv2/time/time.h>
#include <lv2/urid/urid.h>

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define EKA_SPACE_URI "http://dmtry3.github.io/zynthian-eka/space"

enum {
    PORT_IN_L = 0,
    PORT_IN_R,
    PORT_OUT_L,
    PORT_OUT_R,
    PORT_CONTROL,

    /* Page 1: Stage */
    PORT_PAN,
    PORT_WIDTH,
    PORT_DEPTH,
    PORT_TILT,

    /* Page 2: Widener */
    PORT_MODE,
    PORT_WIDEN,
    PORT_SPREAD_MS,
    PORT_ROOM_PCT,

    /* Page 3: Motion */
    PORT_MOD_SHAPE,
    PORT_MOD_RATE,
    PORT_MOD_DEPTH,
    PORT_SYNC,

    /* Page 4: Bass & Air */
    PORT_MONO_LOW,
    PORT_SIDE_HPF,
    PORT_SIDE_AIR,
    PORT_ENV_WIDTH,

    /* Page 5: Live FX */
    PORT_PAN_FX,
    PORT_WIDTH_FX,
    PORT_SPIN_FX,
    PORT_FX_ZERO,

    /* Page 6: Master */
    PORT_AUDITION,
    PORT_SAFE_PHASE,
    PORT_MIX,
    PORT_OUTPUT_DB,
    PORT_BYPASS,

    /* Telemetry outputs (indices 30..39) */
    PORT_IN_PEAK,
    PORT_OUT_PEAK,
    PORT_MID_DB,
    PORT_SIDE_DB,
    PORT_CORR,
    PORT_BALANCE,
    PORT_EFF_PAN,
    PORT_EFF_WIDTH,
    PORT_EFF_DEPTH,
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
} SpaceURIDs;

typedef struct {
    const float             *in_l;
    const float             *in_r;
    float                   *out_l;
    float                   *out_r;
    const LV2_Atom_Sequence *control;

    const float             *ctrl[PORT_IN_PEAK];
    float                   *out_ctrl[10];

    SpaceURIDs               urids;
    double                   host_bpm;
    SpaceCore                core;
    SpaceParams              params;
} EkaSpace;

static inline float read_port(const float *ptr, float fallback) {
    if (!ptr) return fallback;
    float v = *ptr;
    return isfinite(v) ? v : fallback;
}

static double atom_number(const SpaceURIDs *u, const LV2_Atom *a) {
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

static void read_host_bpm(EkaSpace *self) {
    if (!self->control || !self->urids.time_position) return;
    LV2_ATOM_SEQUENCE_FOREACH(self->control, ev) {
        const LV2_Atom *atom = &ev->body;
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

    EkaSpace *self = (EkaSpace *)calloc(1, sizeof(EkaSpace));
    if (!self) return NULL;
    self->host_bpm = 120.0;

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
                }
            }
        }
    }

    space_params_default(&self->params);
    if (space_core_init(&self->core, rate) != 0) {
        free(self);
        return NULL;
    }
    return (LV2_Handle)self;
}

static void connect_port(LV2_Handle instance, uint32_t port, void *data) {
    EkaSpace *self = (EkaSpace *)instance;
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
    EkaSpace *self = (EkaSpace *)instance;
    if (self) space_core_reset(&self->core);
}

static void run(LV2_Handle instance, uint32_t nframes) {
    EkaSpace *self = (EkaSpace *)instance;
    if (!self || !self->in_l || !self->in_r || !self->out_l || !self->out_r) return;

    read_host_bpm(self);

    SpaceParams *p = &self->params;
    p->pan_pct       = read_port(self->ctrl[PORT_PAN],           0.0f);
    p->width_pct     = read_port(self->ctrl[PORT_WIDTH],       100.0f);
    p->depth_pct     = read_port(self->ctrl[PORT_DEPTH],         0.0f);
    p->tilt_pct      = read_port(self->ctrl[PORT_TILT],          0.0f);

    p->mode          = (int)lroundf(read_port(self->ctrl[PORT_MODE], 1.0f));
    p->widen_pct     = read_port(self->ctrl[PORT_WIDEN],        30.0f);
    p->spread_ms     = read_port(self->ctrl[PORT_SPREAD_MS],    14.0f);
    p->room_pct      = read_port(self->ctrl[PORT_ROOM_PCT],     15.0f);

    p->mod_shape     = (int)lroundf(read_port(self->ctrl[PORT_MOD_SHAPE], 0.0f));
    p->mod_rate_hz   = read_port(self->ctrl[PORT_MOD_RATE],      0.5f);
    p->mod_depth_pct = read_port(self->ctrl[PORT_MOD_DEPTH],     0.0f);
    p->sync          = read_port(self->ctrl[PORT_SYNC],          1.0f) >= 0.5f ? 1 : 0;

    p->mono_low_hz   = read_port(self->ctrl[PORT_MONO_LOW],    120.0f);
    p->side_hpf_hz   = read_port(self->ctrl[PORT_SIDE_HPF],     90.0f);
    p->side_air_db   = read_port(self->ctrl[PORT_SIDE_AIR],      1.5f);
    p->env_width_pct = read_port(self->ctrl[PORT_ENV_WIDTH],     0.0f);

    p->pan_fx_pct    = read_port(self->ctrl[PORT_PAN_FX],        0.0f);
    p->width_fx_pct  = read_port(self->ctrl[PORT_WIDTH_FX],      0.0f);
    p->spin_fx_pct   = read_port(self->ctrl[PORT_SPIN_FX],       0.0f);
    p->fx_zero       = read_port(self->ctrl[PORT_FX_ZERO],       0.0f) >= 0.5f ? 1 : 0;

    p->audition      = (int)lroundf(read_port(self->ctrl[PORT_AUDITION], 0.0f));
    p->safe_phase    = read_port(self->ctrl[PORT_SAFE_PHASE],    0.0f) >= 0.5f ? 1 : 0;
    p->mix_pct       = read_port(self->ctrl[PORT_MIX],         100.0f);
    p->output_db     = read_port(self->ctrl[PORT_OUTPUT_DB],     0.0f);
    p->bypass        = read_port(self->ctrl[PORT_BYPASS],        0.0f) >= 0.5f ? 1 : 0;
    p->host_bpm      = (float)self->host_bpm;

    space_core_process(&self->core, p, self->in_l, self->in_r, self->out_l, self->out_r, nframes);

    if (self->out_ctrl[0]) *self->out_ctrl[0] = self->core.meters.in_peak_db;
    if (self->out_ctrl[1]) *self->out_ctrl[1] = self->core.meters.out_peak_db;
    if (self->out_ctrl[2]) *self->out_ctrl[2] = self->core.meters.mid_db;
    if (self->out_ctrl[3]) *self->out_ctrl[3] = self->core.meters.side_db;
    if (self->out_ctrl[4]) *self->out_ctrl[4] = self->core.meters.corr;
    if (self->out_ctrl[5]) *self->out_ctrl[5] = self->core.meters.balance;
    if (self->out_ctrl[6]) *self->out_ctrl[6] = self->core.meters.eff_pan_pct;
    if (self->out_ctrl[7]) *self->out_ctrl[7] = self->core.meters.eff_width_pct;
    if (self->out_ctrl[8]) *self->out_ctrl[8] = self->core.meters.eff_depth_pct;
    if (self->out_ctrl[9]) *self->out_ctrl[9] = self->core.meters.host_bpm;
}

static void deactivate(LV2_Handle instance) {
    (void)instance;
}

static void cleanup(LV2_Handle instance) {
    EkaSpace *self = (EkaSpace *)instance;
    if (!self) return;
    space_core_free(&self->core);
    free(self);
}

static const void *extension_data(const char *uri) {
    (void)uri;
    return NULL;
}

static const LV2_Descriptor descriptor = {
    EKA_SPACE_URI,
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
