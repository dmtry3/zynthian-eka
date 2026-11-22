/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Eka Echo: LV2 plugin wrapper with time:Position BPM sync for Zynthian. */
#include "echo_core.h"

#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/core/lv2.h>
#include <lv2/time/time.h>
#include <lv2/urid/urid.h>

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define EKA_ECHO_URI "http://dmtry3.github.io/zynthian-eka/echo"

enum {
    PORT_IN_L = 0,
    PORT_IN_R,
    PORT_OUT_L,
    PORT_OUT_R,
    PORT_CONTROL,

    /* Page 1: Time */
    PORT_TIME_L,
    PORT_TIME_R,
    PORT_FEEDBACK,
    PORT_MIX,

    /* Page 2: Mode & Sync */
    PORT_MODE,
    PORT_ROUTING,
    PORT_SYNC,
    PORT_WIDTH,

    /* Page 3: Tone */
    PORT_HPF_HZ,
    PORT_LPF_HZ,
    PORT_DRIVE_PCT,
    PORT_DIFFUSE_PCT,

    /* Page 4: Motion */
    PORT_MOD_RATE,
    PORT_MOD_DEPTH,
    PORT_DUCK_PCT,
    PORT_DUCK_REL,

    /* Page 5: Perform */
    PORT_FREEZE,
    PORT_TRAILS,
    PORT_OUTPUT_DB,
    PORT_BYPASS,

    /* Page 6: Effects (Live) */
    PORT_TIME_FX,
    PORT_TIME_LINK,
    PORT_FILTER_FX,
    PORT_WASH_FX,

    /* Telemetry outputs */
    PORT_IN_PEAK,
    PORT_OUT_PEAK,
    PORT_WET_L_DB,
    PORT_WET_R_DB,
    PORT_DUCK_DB,
    PORT_ACTUAL_MS_L,
    PORT_ACTUAL_MS_R,
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
} EchoURIDs;

typedef struct {
    const float             *in_l;
    const float             *in_r;
    float                   *out_l;
    float                   *out_r;
    const LV2_Atom_Sequence *control;

    const float             *ctrl[PORT_IN_PEAK];
    float                   *out_ctrl[8];

    EchoURIDs                urids;
    double                   host_bpm;
    EchoCore                 core;
    EchoParams               params;
} EkaEcho;

static inline float read_port(const float *ptr, float fallback) {
    if (!ptr) return fallback;
    float v = *ptr;
    return isfinite(v) ? v : fallback;
}

static double atom_number(const EchoURIDs *u, const LV2_Atom *a) {
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

static void read_host_bpm(EkaEcho *self) {
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

    EkaEcho *self = (EkaEcho *)calloc(1, sizeof(EkaEcho));
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

    echo_params_default(&self->params);
    if (echo_core_init(&self->core, rate) != 0) {
        free(self);
        return NULL;
    }
    return (LV2_Handle)self;
}

static void connect_port(LV2_Handle instance, uint32_t port, void *data) {
    EkaEcho *self = (EkaEcho *)instance;
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
        if (oi < 8) self->out_ctrl[oi] = (float *)data;
    }
}

static void activate(LV2_Handle instance) {
    EkaEcho *self = (EkaEcho *)instance;
    if (self) echo_core_reset(&self->core);
}

static void run(LV2_Handle instance, uint32_t nframes) {
    EkaEcho *self = (EkaEcho *)instance;
    if (!self || !self->in_l || !self->in_r || !self->out_l || !self->out_r) return;

    read_host_bpm(self);

    EchoParams *p = &self->params;
    p->time_l_ms     = read_port(self->ctrl[PORT_TIME_L],      375.0f);
    p->time_r_ms     = read_port(self->ctrl[PORT_TIME_R],      500.0f);
    p->feedback_pct  = read_port(self->ctrl[PORT_FEEDBACK],     42.0f);
    p->mix_pct       = read_port(self->ctrl[PORT_MIX],          35.0f);

    p->mode          = (int)lroundf(read_port(self->ctrl[PORT_MODE],    1.0f));
    p->routing       = (int)lroundf(read_port(self->ctrl[PORT_ROUTING], 0.0f));
    p->sync          = read_port(self->ctrl[PORT_SYNC],          1.0f) >= 0.5f ? 1 : 0;
    p->width_pct     = read_port(self->ctrl[PORT_WIDTH],       100.0f);

    p->hpf_hz        = read_port(self->ctrl[PORT_HPF_HZ],       80.0f);
    p->lpf_hz        = read_port(self->ctrl[PORT_LPF_HZ],     8500.0f);
    p->drive_pct     = read_port(self->ctrl[PORT_DRIVE_PCT],    15.0f);
    p->diffuse_pct   = read_port(self->ctrl[PORT_DIFFUSE_PCT],   0.0f);

    p->mod_rate_hz   = read_port(self->ctrl[PORT_MOD_RATE],      0.6f);
    p->mod_depth_pct = read_port(self->ctrl[PORT_MOD_DEPTH],    18.0f);
    p->duck_pct      = read_port(self->ctrl[PORT_DUCK_PCT],      0.0f);
    p->duck_rel_ms   = read_port(self->ctrl[PORT_DUCK_REL],    220.0f);

    p->freeze        = read_port(self->ctrl[PORT_FREEZE],        0.0f) >= 0.5f ? 1 : 0;
    p->trails        = read_port(self->ctrl[PORT_TRAILS],        0.0f) >= 0.5f ? 1 : 0;
    p->output_db     = read_port(self->ctrl[PORT_OUTPUT_DB],     0.0f);
    p->bypass        = read_port(self->ctrl[PORT_BYPASS],        0.0f) >= 0.5f ? 1 : 0;

    p->time_fx_pct   = read_port(self->ctrl[PORT_TIME_FX],       0.0f);
    p->time_link     = read_port(self->ctrl[PORT_TIME_LINK],     1.0f) >= 0.5f ? 1 : 0;
    p->filter_fx_pct = read_port(self->ctrl[PORT_FILTER_FX],     0.0f);
    p->wash_fx_pct   = read_port(self->ctrl[PORT_WASH_FX],       0.0f);
    p->host_bpm      = (float)self->host_bpm;

    echo_core_process(&self->core, p, self->in_l, self->in_r, self->out_l, self->out_r, nframes);

    if (self->out_ctrl[0]) *self->out_ctrl[0] = self->core.meters.in_peak_db;
    if (self->out_ctrl[1]) *self->out_ctrl[1] = self->core.meters.out_peak_db;
    if (self->out_ctrl[2]) *self->out_ctrl[2] = self->core.meters.wet_l_db;
    if (self->out_ctrl[3]) *self->out_ctrl[3] = self->core.meters.wet_r_db;
    if (self->out_ctrl[4]) *self->out_ctrl[4] = self->core.meters.duck_db;
    if (self->out_ctrl[5]) *self->out_ctrl[5] = self->core.meters.actual_ms_l;
    if (self->out_ctrl[6]) *self->out_ctrl[6] = self->core.meters.actual_ms_r;
    if (self->out_ctrl[7]) *self->out_ctrl[7] = self->core.meters.host_bpm;
}

static void deactivate(LV2_Handle instance) {
    (void)instance;
}

static void cleanup(LV2_Handle instance) {
    EkaEcho *self = (EkaEcho *)instance;
    if (!self) return;
    echo_core_free(&self->core);
    free(self);
}

static const void *extension_data(const char *uri) {
    (void)uri;
    return NULL;
}

static const LV2_Descriptor descriptor = {
    EKA_ECHO_URI,
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
