/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "midi.h"
#include <string.h>
void eka_midi_reset(EkaMidi *p) {
    memset(p, 0, sizeof(*p));
    for (unsigned v = 0; v < 4; ++v) p->target[v] = -1;
}
void eka_midi_event(EkaMidi *p, const uint8_t *d, unsigned size) {
    if (!size) return;
    if (d[0] == 0xff) { eka_midi_reset(p); return; }
    unsigned type = d[0] & 0xf0, ch = d[0] & 15;
    if (size != 3 || d[1] > 127 || d[2] > 127) return;
    unsigned note = d[1];
    if (type == 0x90 && d[2]) {
        if (p->pressed[ch][note] < 255) ++p->pressed[ch][note];
        p->sustained[ch][note] = 0;
    } else if (type == 0x80 || type == 0x90) {
        if (p->pressed[ch][note]) {
            --p->pressed[ch][note];
            if (!p->pressed[ch][note]) p->sustained[ch][note] = p->pedal[ch];
        }
    } else if (type == 0xb0) {
        if (note == 64) {
            p->pedal[ch] = d[2] >= 64;
            if (!p->pedal[ch]) memset(p->sustained[ch], 0, 128);
        } else if (note == 120 || note == 123) {
            memset(p->pressed[ch], 0, 128); memset(p->sustained[ch], 0, 128);
            p->pedal[ch] = 0;
        } else if (note == 121) {
            p->pedal[ch] = 0; memset(p->sustained[ch], 0, 128);
        }
    }
}
void eka_midi_select(EkaMidi *p, unsigned channel) {
    uint8_t held[128] = {0}, chosen[128] = {0};
    p->count = 0;
    for (unsigned ch = 0; ch < 16; ++ch) if (!channel || ch + 1 == channel)
        for (unsigned n = 0; n < 128; ++n)
            held[n] |= p->pressed[ch][n] || p->sustained[ch][n];
    unsigned selected = 0;
    for (unsigned n = 0; n < 128; ++n) if (held[n]) {
        ++p->count;
        if (selected < 4) { chosen[n] = 1; ++selected; }
    }
    /* Retain physical voice settings for common notes; fill vacant slots low first. */
    for (unsigned v = 0; v < 4; ++v) {
        int n = p->target[v];
        if (n >= 0 && chosen[n]) chosen[n] = 0;
        else p->target[v] = -1;
    }
    for (unsigned v = 0; v < 4; ++v) if (p->target[v] < 0)
        for (unsigned n = 0; n < 128; ++n) if (chosen[n]) {
            p->target[v] = (int)n; chosen[n] = 0; break;
        }
}
