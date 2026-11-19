/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tempo.h"
#include <lv2/time/time.h>
#include <lv2/midi/midi.h>
#include <lv2/units/units.h>
#include <lv2/atom/util.h>
#include <math.h>
#include <string.h>
void eka_time_map(EkaTimeURIDs *ids, LV2_URID_Map *map) {
    ids->midi = map->map(map->handle, LV2_MIDI__MidiEvent);
    ids->object = map->map(map->handle, LV2_ATOM__Object);
    ids->blank = map->map(map->handle, LV2_ATOM__Blank);
    ids->sequence = map->map(map->handle, LV2_ATOM__Sequence);
    ids->floating = map->map(map->handle, LV2_ATOM__Float);
    ids->doubling = map->map(map->handle, LV2_ATOM__Double);
    ids->integer = map->map(map->handle, LV2_ATOM__Int);
    ids->frame_unit = map->map(map->handle, LV2_UNITS__frame);
    ids->position = map->map(map->handle, LV2_TIME__Position);
    ids->bpm = map->map(map->handle, LV2_TIME__beatsPerMinute);
    ids->beats_per_bar = map->map(map->handle, LV2_TIME__beatsPerBar);
    ids->beat_unit = map->map(map->handle, LV2_TIME__beatUnit);
}
static double number(const EkaTimeURIDs *ids, const LV2_Atom *atom) {
    const void *body = (const uint8_t *)atom + sizeof(*atom);
    if (atom->type == ids->floating && atom->size == sizeof(float)) {
        float value; memcpy(&value, body, sizeof(value)); return value;
    }
    if (atom->type == ids->doubling && atom->size == sizeof(double)) {
        double value; memcpy(&value, body, sizeof(value)); return value;
    }
    if (atom->type == ids->integer && atom->size == sizeof(int32_t)) {
        int32_t value; memcpy(&value, body, sizeof(value)); return value;
    }
    return NAN;
}
int eka_tempo_read(const EkaTimeURIDs *ids, const LV2_Atom *atom, EkaTempo *tempo) {
    if (!ids->position || (atom->type != ids->object && atom->type != ids->blank)
        || atom->size < sizeof(LV2_Atom_Object_Body)) return 0;
    const LV2_Atom_Object *object = (const LV2_Atom_Object *)atom;
    if (object->body.otype != ids->position) return 0;
    const uint8_t *cursor = (const uint8_t *)&object->body + sizeof(object->body);
    uint32_t remaining = atom->size - sizeof(object->body);
    int changed = 0;
    while (remaining >= sizeof(LV2_Atom_Property_Body)) {
        const LV2_Atom_Property_Body *property = (const LV2_Atom_Property_Body *)cursor;
        if (property->value.size > remaining - sizeof(*property)) break;
        uint32_t size = sizeof(*property) + lv2_atom_pad_size(property->value.size);
        if (size > remaining) break;
        double value = number(ids, &property->value);
        if (isfinite(value)) {
            if (property->key == ids->bpm && value >= 20 && value <= 400) {
                tempo->bpm = value; tempo->from_host = 1; changed = 1;
            } else if (property->key == ids->beats_per_bar && value >= 1 && value <= 32) {
                tempo->beats_per_bar = value; changed = 1;
            } else if (property->key == ids->beat_unit &&
                       (value == 1 || value == 2 || value == 4 || value == 8 ||
                        value == 16 || value == 32 || value == 64)) {
                tempo->beat_unit = value; changed = 1;
            }
        }
        cursor += size; remaining -= size;
    }
    return changed;
}
