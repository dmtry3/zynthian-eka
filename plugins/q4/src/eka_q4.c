/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Eka Q-4: LV2 plugin wrapper for Zynthian. */
#include "q4_core.h"

#include <lv2/core/lv2.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#define EKA_Q4_URI "http://dmtry3.github.io/zynthian-eka/q4"

enum {
    PORT_IN_L = 0,
    PORT_IN_R,
    PORT_OUT_L,
    PORT_OUT_R,

    /* Page 1: Band 1 */
    PORT_B1_FREQ,
    PORT_B1_GAIN,
    PORT_B1_Q,
    PORT_B1_TYPE,

    /* Page 2: Band 2 */
    PORT_B2_FREQ,
    PORT_B2_GAIN,
    PORT_B2_Q,
    PORT_B2_TYPE,

    /* Page 3: Band 3 */
    PORT_B3_FREQ,
    PORT_B3_GAIN,
    PORT_B3_Q,
    PORT_B3_TYPE,

    /* Page 4: Band 4 */
    PORT_B4_FREQ,
    PORT_B4_GAIN,
    PORT_B4_Q,
    PORT_B4_TYPE,

    /* Page 5: Cuts & Tilt */
    PORT_HPF_FREQ,
    PORT_LPF_FREQ,
    PORT_TILT_DB,
    PORT_SLOPE,

    /* Page 6: Master */
    PORT_SCALE_PCT,
    PORT_SOLO_BAND,
    PORT_AUTO_GAIN,
    PORT_OUTPUT_DB,

    /* Output telemetry ports */
    PORT_IN_PEAK,
    PORT_OUT_PEAK,
    PORT_SPEC_BASE, /* 24 bins: PORT_SPEC_BASE .. PORT_SPEC_BASE + 23 */

    PORT_BYPASS = PORT_SPEC_BASE + Q4_SPEC_BINS,
    PORT_COUNT
};

typedef struct {
    const float *in_l;
    const float *in_r;
    float       *out_l;
    float       *out_r;

    const float *ctrl[PORT_IN_PEAK];
    const float *in_bypass;
    float       *out_in_peak;
    float       *out_out_peak;
    float       *out_spec[Q4_SPEC_BINS];

    Q4Core       core;
    Q4Params     params;
} EkaQ4;

static inline float read_port(const float *ptr, float fallback) {
    if (!ptr) return fallback;
    float v = *ptr;
    return isfinite(v) ? v : fallback;
}

static LV2_Handle instantiate(const LV2_Descriptor *descriptor,
                              double rate,
                              const char *bundle_path,
                              const LV2_Feature *const *features) {
    (void)descriptor;
    (void)bundle_path;
    (void)features;

    EkaQ4 *self = (EkaQ4 *)calloc(1, sizeof(EkaQ4));
    if (!self) return NULL;
    q4_params_default(&self->params);
    q4_core_init(&self->core, rate);
    return (LV2_Handle)self;
}

static void connect_port(LV2_Handle instance, uint32_t port, void *data) {
    EkaQ4 *self = (EkaQ4 *)instance;
    if (!self || port >= PORT_COUNT) return;

    if (port == PORT_IN_L) {
        self->in_l = (const float *)data;
    } else if (port == PORT_IN_R) {
        self->in_r = (const float *)data;
    } else if (port == PORT_OUT_L) {
        self->out_l = (float *)data;
    } else if (port == PORT_OUT_R) {
        self->out_r = (float *)data;
    } else if (port < PORT_IN_PEAK) {
        self->ctrl[port] = (const float *)data;
    } else if (port == PORT_IN_PEAK) {
        self->out_in_peak = (float *)data;
    } else if (port == PORT_OUT_PEAK) {
        self->out_out_peak = (float *)data;
    } else if (port == PORT_BYPASS) {
        self->in_bypass = (const float *)data;
    } else {
        uint32_t bin = port - PORT_SPEC_BASE;
        if (bin < Q4_SPEC_BINS) {
            self->out_spec[bin] = (float *)data;
        }
    }
}

static void activate(LV2_Handle instance) {
    EkaQ4 *self = (EkaQ4 *)instance;
    if (self) q4_core_reset(&self->core);
}

static void run(LV2_Handle instance, uint32_t nframes) {
    EkaQ4 *self = (EkaQ4 *)instance;
    if (!self || !self->in_l || !self->in_r || !self->out_l || !self->out_r) return;

    Q4Params *p = &self->params;
    p->bands[0].freq_hz = read_port(self->ctrl[PORT_B1_FREQ], 100.0f);
    p->bands[0].gain_db = read_port(self->ctrl[PORT_B1_GAIN], 0.0f);
    p->bands[0].q       = read_port(self->ctrl[PORT_B1_Q],    0.71f);
    p->bands[0].type    = (int)lroundf(read_port(self->ctrl[PORT_B1_TYPE], 0.0f));

    p->bands[1].freq_hz = read_port(self->ctrl[PORT_B2_FREQ], 400.0f);
    p->bands[1].gain_db = read_port(self->ctrl[PORT_B2_GAIN], 0.0f);
    p->bands[1].q       = read_port(self->ctrl[PORT_B2_Q],    1.0f);
    p->bands[1].type    = (int)lroundf(read_port(self->ctrl[PORT_B2_TYPE], 0.0f));

    p->bands[2].freq_hz = read_port(self->ctrl[PORT_B3_FREQ], 2200.0f);
    p->bands[2].gain_db = read_port(self->ctrl[PORT_B3_GAIN], 0.0f);
    p->bands[2].q       = read_port(self->ctrl[PORT_B3_Q],    1.0f);
    p->bands[2].type    = (int)lroundf(read_port(self->ctrl[PORT_B3_TYPE], 0.0f));

    p->bands[3].freq_hz = read_port(self->ctrl[PORT_B4_FREQ], 8000.0f);
    p->bands[3].gain_db = read_port(self->ctrl[PORT_B4_GAIN], 0.0f);
    p->bands[3].q       = read_port(self->ctrl[PORT_B4_Q],    0.71f);
    p->bands[3].type    = (int)lroundf(read_port(self->ctrl[PORT_B4_TYPE], 0.0f));

    p->hpf_hz    = read_port(self->ctrl[PORT_HPF_FREQ], 20.0f);
    p->lpf_hz    = read_port(self->ctrl[PORT_LPF_FREQ], 20000.0f);
    p->tilt_db   = read_port(self->ctrl[PORT_TILT_DB],  0.0f);
    p->slope_24  = read_port(self->ctrl[PORT_SLOPE],    0.0f) >= 0.5f ? 1 : 0;

    p->scale_pct = read_port(self->ctrl[PORT_SCALE_PCT], 100.0f);
    p->solo_band = (int)lroundf(read_port(self->ctrl[PORT_SOLO_BAND], 0.0f));
    p->auto_gain = read_port(self->ctrl[PORT_AUTO_GAIN], 0.0f) >= 0.5f ? 1 : 0;
    p->output_db = read_port(self->ctrl[PORT_OUTPUT_DB], 0.0f);
    p->bypass    = read_port(self->in_bypass,            0.0f) >= 0.5f ? 1 : 0;

    q4_core_process(&self->core, p, self->in_l, self->in_r, self->out_l, self->out_r, nframes);

    if (self->out_in_peak)  *self->out_in_peak  = self->core.meters.in_peak_db;
    if (self->out_out_peak) *self->out_out_peak = self->core.meters.out_peak_db;
    for (int b = 0; b < Q4_SPEC_BINS; ++b) {
        if (self->out_spec[b]) *self->out_spec[b] = self->core.meters.spec_db[b];
    }
}

static void deactivate(LV2_Handle instance) {
    (void)instance;
}

static void cleanup(LV2_Handle instance) {
    free(instance);
}

static const void *extension_data(const char *uri) {
    (void)uri;
    return NULL;
}

static const LV2_Descriptor descriptor = {
    EKA_Q4_URI,
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
