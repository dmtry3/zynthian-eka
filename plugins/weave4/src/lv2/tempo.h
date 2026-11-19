/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef EKA_TEMPO_H
#define EKA_TEMPO_H
#include <lv2/atom/atom.h>
#include <lv2/urid/urid.h>
typedef struct {
    double bpm, beats_per_bar, beat_unit;
    int from_host;
} EkaTempo;
typedef struct {
    LV2_URID object, blank, sequence, floating, doubling, integer, frame_unit;
    LV2_URID midi, position, bpm, beats_per_bar, beat_unit;
} EkaTimeURIDs;
void eka_time_map(EkaTimeURIDs *ids, LV2_URID_Map *map);
int eka_tempo_read(const EkaTimeURIDs *ids, const LV2_Atom *atom, EkaTempo *tempo);
#endif
