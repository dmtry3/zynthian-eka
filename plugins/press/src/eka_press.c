/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Eka Press: LV2 plugin wrapper for Zynthian. */
#include "press_core.h"

#include <lv2/core/lv2.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#define EKA_PRESS_URI "http://dmtry3.github.io/zynthian-eka/press"

enum {
    PORT_IN_L = 0,
    PORT_IN_R,
    PORT_OUT_L,
    PORT_OUT_R,

    /* Page 1: Main */
    PORT_THRESHOLD,
    PORT_RATIO,
    PORT_MAKEUP,
    PORT_MIX,

    /* Page 2: Envelope */
    PORT_ATTACK,
    PORT_RELEASE,
    PORT_KNEE,
    PORT_MODE,

    /* Page 3: Sidechain */
    PORT_SC_HPF,
    PORT_STEREO_LINK,
    PORT_AUTO_RELEASE,
    PORT_SC_LISTEN,

    /* Page 4: Color */
    PORT_DRIVE,
    PORT_AUTO_MAKEUP,
    PORT_SOFT_CLIP,
    PORT_OUTPUT,

    /* Output telemetry ports */
    PORT_IN_PEAK,
    PORT_OUT_PEAK,
    PORT_SC_PEAK,
    PORT_GR_DB,

    /* Utility */
    PORT_BYPASS,

    PORT_COUNT
};

typedef struct {
    const float *in_l;
    const float *in_r;
    float       *out_l;
    float       *out_r;

    const float *ctrl[PORT_COUNT];
    float       *out_ctrl[4];

    PressCore    core;
    PressParams  params;
} EkaPress;

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

    EkaPress *self = (EkaPress *)calloc(1, sizeof(EkaPress));
    if (!self) return NULL;
    press_params_default(&self->params);
    press_core_init(&self->core, (float)rate);
    return (LV2_Handle)self;
}

static void connect_port(LV2_Handle instance, uint32_t port, void *data) {
    EkaPress *self = (EkaPress *)instance;
    if (!self || port >= PORT_COUNT) return;

    switch (port) {
        case PORT_IN_L:     self->in_l  = (const float *)data; break;
        case PORT_IN_R:     self->in_r  = (const float *)data; break;
        case PORT_OUT_L:    self->out_l = (float *)data; break;
        case PORT_OUT_R:    self->out_r = (float *)data; break;
        case PORT_IN_PEAK:  self->out_ctrl[0] = (float *)data; break;
        case PORT_OUT_PEAK: self->out_ctrl[1] = (float *)data; break;
        case PORT_SC_PEAK:  self->out_ctrl[2] = (float *)data; break;
        case PORT_GR_DB:    self->out_ctrl[3] = (float *)data; break;
        default:
            self->ctrl[port] = (const float *)data;
            break;
    }
}

static void activate(LV2_Handle instance) {
    EkaPress *self = (EkaPress *)instance;
    if (self) press_core_reset(&self->core);
}

static void run(LV2_Handle instance, uint32_t nframes) {
    EkaPress *self = (EkaPress *)instance;
    if (!self || !self->in_l || !self->in_r || !self->out_l || !self->out_r) return;

    PressParams *p = &self->params;
    p->threshold_db = read_port(self->ctrl[PORT_THRESHOLD],    -18.0f);
    p->ratio        = read_port(self->ctrl[PORT_RATIO],          4.0f);
    p->makeup_db    = read_port(self->ctrl[PORT_MAKEUP],         0.0f);
    p->mix_pct      = read_port(self->ctrl[PORT_MIX],          100.0f);

    p->attack_ms    = read_port(self->ctrl[PORT_ATTACK],        10.0f);
    p->release_ms   = read_port(self->ctrl[PORT_RELEASE],      150.0f);
    p->knee_db      = read_port(self->ctrl[PORT_KNEE],           6.0f);
    p->mode         = (int)lroundf(read_port(self->ctrl[PORT_MODE], 0.0f));

    p->sc_hpf_hz    = read_port(self->ctrl[PORT_SC_HPF],        20.0f);
    p->stereo_link  = read_port(self->ctrl[PORT_STEREO_LINK],  100.0f);
    p->auto_release = read_port(self->ctrl[PORT_AUTO_RELEASE],   0.0f) >= 0.5f ? 1 : 0;
    p->sc_listen    = read_port(self->ctrl[PORT_SC_LISTEN],      0.0f) >= 0.5f ? 1 : 0;

    p->drive_pct    = read_port(self->ctrl[PORT_DRIVE],          0.0f);
    p->auto_makeup  = read_port(self->ctrl[PORT_AUTO_MAKEUP],    0.0f) >= 0.5f ? 1 : 0;
    p->soft_clip    = read_port(self->ctrl[PORT_SOFT_CLIP],      1.0f) >= 0.5f ? 1 : 0;
    p->output_db    = read_port(self->ctrl[PORT_OUTPUT],         0.0f);
    p->bypass       = read_port(self->ctrl[PORT_BYPASS],         0.0f) >= 0.5f ? 1 : 0;

    press_core_process(&self->core, p, self->in_l, self->in_r, self->out_l, self->out_r, nframes);

    if (self->out_ctrl[0]) *self->out_ctrl[0] = self->core.meters.in_peak_db;
    if (self->out_ctrl[1]) *self->out_ctrl[1] = self->core.meters.out_peak_db;
    if (self->out_ctrl[2]) *self->out_ctrl[2] = self->core.meters.sc_peak_db;
    if (self->out_ctrl[3]) *self->out_ctrl[3] = self->core.meters.gr_db;
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
    EKA_PRESS_URI,
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
