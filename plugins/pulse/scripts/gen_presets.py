#!/usr/bin/python3
"""Factory presets for Eka Pulse.
Imported by scripts/gen_ttl.py to generate lv2/presets.ttl and manifest entries.
Edit this script, not lv2/presets.ttl."""
import os

URI = "http://dmtry3.github.io/zynthian-eka/pulse"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

PORTS = [
    "slot", "mix", "low_split", "scene",
    "mode", "param1", "param2", "depth",
    "pattern", "step_rate", "steps", "warp",
    "trig", "threshold", "env_depth", "smooth",
    "live_pad", "prev_scene", "next_scene", "output_db",
]

FACE_KEYS = [
    "mode", "param1", "param2", "depth",
    "pattern", "step_rate", "steps", "warp",
    "trig", "threshold", "env_depth", "smooth",
]
INT_KEYS = {"on", "mode", "pattern", "step_rate", "steps", "trig"}

SLOT_DEFAULTS = [
    {"on": 0, "mode": 0, "param1": 50, "param2": 50, "depth": 100, "pattern": 20, "step_rate": 2, "steps": 16, "warp": 0, "trig": 0, "threshold": -24, "env_depth": 0, "smooth": 10},
    {"on": 0, "mode": 0, "param1": 50, "param2": 50, "depth": 100, "pattern": 21, "step_rate": 2, "steps": 5,  "warp": 0, "trig": 0, "threshold": -24, "env_depth": 0, "smooth": 10},
    {"on": 0, "mode": 0, "param1": 50, "param2": 30, "depth": 100, "pattern": 22, "step_rate": 2, "steps": 7,  "warp": 0, "trig": 0, "threshold": -24, "env_depth": 0, "smooth": 10},
    {"on": 0, "mode": 0, "param1": 0,  "param2": 50, "depth": 100, "pattern": 2,  "step_rate": 2, "steps": 8,  "warp": 0, "trig": 0, "threshold": -24, "env_depth": 0, "smooth": 10},
]


def sl(slot_idx, on=1, **kw):
    d = dict(SLOT_DEFAULTS[slot_idx])
    d["on"] = on
    d.update(kw)
    return d


def off(slot_idx, **kw):
    return sl(slot_idx, on=0, **kw)


# (id, label, selected_slot 1..4, mix %, low_split Hz, low_duck, free_hz, [slot1, slot2, slot3, slot4])
PRESETS = [
    ("init", "Init", 4, 100, 0, 0, 4.0, [
        off(0), off(1), off(2),
        sl(3, mode=0, param1=0, param2=50, depth=100, pattern=2, step_rate=2, steps=8, smooth=10),
    ]),
    ("sidechain_pump", "Sidechain Pump", 4, 100, 0, 0, 4.0, [
        off(0), off(1), off(2),
        sl(3, mode=0, param1=8, param2=50, depth=100, pattern=1, step_rate=5, steps=4, warp=-25, smooth=15),
    ]),
    ("trance_gate_16", "Trance Gate 16", 4, 100, 0, 0, 4.0, [
        off(0), off(1), off(2),
        sl(3, mode=3, param1=0, param2=70, depth=100, pattern=2, step_rate=2, steps=16, smooth=-35),
    ]),
    ("euclid_groove", "Euclid Groove", 3, 100, 0, 0, 4.0, [
        off(0), off(1),
        sl(2, mode=0, param1=35, param2=55, depth=85, pattern=5, step_rate=2, steps=8, smooth=12),
        sl(3, mode=0, param1=20, param2=50, depth=80, pattern=4, step_rate=2, steps=8, smooth=8),
    ]),
    ("poly_shaper", "Poly Shaper", 3, 100, 140, 0, 4.0, [
        off(0),
        sl(1, mode=0, param1=65, param2=60, depth=90, pattern=21, step_rate=2, steps=5, smooth=10),
        sl(2, mode=0, param1=42, param2=48, depth=80, pattern=22, step_rate=2, steps=7, smooth=15),
        sl(3, mode=0, param1=15, param2=50, depth=85, pattern=2, step_rate=2, steps=8, smooth=10),
    ]),
    ("acid_sweep", "Acid Sweep", 3, 100, 100, 0, 4.0, [
        off(0),
        sl(1, mode=0, param1=70, param2=55, depth=70, pattern=7, step_rate=2, steps=8, smooth=8),
        sl(2, mode=0, param1=30, param2=78, depth=95, pattern=7, step_rate=2, steps=16, warp=20, smooth=12),
        off(3),
    ]),
    ("morph_vowel", "Morph Vowel", 3, 100, 0, 0, 4.0, [
        off(0), off(1),
        sl(2, mode=4, param1=45, param2=70, depth=100, pattern=15, step_rate=4, steps=8, smooth=25),
        off(3),
    ]),
    ("liquid_phaser", "Liquid Phaser", 3, 85, 120, 0, 4.0, [
        off(0), off(1),
        sl(2, mode=6, param1=40, param2=75, depth=90, pattern=19, step_rate=4, steps=16, smooth=45),
        off(3),
    ]),
    ("west_coast_fold", "West Coast Fold", 2, 100, 120, 0, 4.0, [
        off(0),
        sl(1, mode=1, param1=75, param2=70, depth=100, pattern=22, step_rate=2, steps=7, smooth=10),
        sl(2, mode=5, param1=55, param2=50, depth=75, pattern=21, step_rate=2, steps=5, smooth=20),
        off(3),
    ]),
    ("lofi_crusher", "Lo-Fi Crusher", 2, 100, 180, 0, 4.0, [
        off(0),
        sl(1, mode=3, param1=65, param2=72, depth=100, pattern=14, step_rate=2, steps=8, smooth=5),
        sl(2, mode=1, param1=60, param2=25, depth=-55, pattern=14, step_rate=2, steps=8, smooth=15),
        off(3),
    ]),
    ("glitch_chopper", "Glitch Chopper", 1, 100, 0, 0, 4.0, [
        sl(0, mode=1, param1=50, param2=35, depth=100, pattern=0, step_rate=4, steps=8, smooth=0,
           lvls=[0, 40, 0, 15, 0, 65, 82, 96, 0, 40, 0, 15, 0, 65, 82, 96]),
        off(1), off(2), off(3),
    ]),
    ("vinyl_brake", "Vinyl Brake", 1, 100, 0, 0, 4.0, [
        sl(0, mode=3, param1=45, param2=40, depth=100, pattern=20, step_rate=2, steps=16, smooth=10),
        sl(1, mode=0, param1=35, param2=45, depth=40, pattern=1, step_rate=5, steps=4, smooth=20),
        off(2), off(3),
    ]),
    ("stutter_roll", "Stutter Roll", 1, 100, 0, 0, 4.0, [
        sl(0, mode=2, param1=50, param2=30, depth=100, pattern=11, step_rate=2, steps=8, smooth=0),
        off(1), off(2),
        sl(3, mode=0, param1=10, param2=50, depth=70, pattern=3, step_rate=2, steps=8, smooth=-40),
    ]),
    ("scratch_curve", "Scratch Curve", 1, 100, 0, 0, 4.0, [
        sl(0, mode=0, param1=35, param2=40, depth=90, pattern=23, step_rate=2, steps=8, smooth=15),
        off(1),
        sl(2, mode=3, param1=22, param2=45, depth=65, pattern=6, step_rate=2, steps=16, smooth=15),
        off(3),
    ]),
    ("clean_bass_chop", "Clean Bass Chop", 4, 100, 200, 0, 4.0, [
        off(0), off(1),
        sl(2, mode=1, param1=55, param2=35, depth=60, pattern=5, step_rate=2, steps=8, smooth=15),
        sl(3, mode=0, param1=0, param2=50, depth=100, pattern=2, step_rate=2, steps=16, smooth=-20),
    ]),
    ("sub_duck_pump", "Sub Duck Pump", 4, 100, 160, 1, 4.0, [
        off(0),
        sl(1, mode=0, param1=50, param2=65, depth=60, pattern=8, step_rate=4, steps=4, smooth=15),
        off(2),
        sl(3, mode=0, param1=5, param2=50, depth=100, pattern=1, step_rate=5, steps=4, warp=-30, smooth=18),
    ]),
    ("ping_pong_gate", "Ping-Pong Gate", 4, 100, 0, 0, 4.0, [
        off(0), off(1), off(2),
        sl(3, mode=3, param1=0, param2=95, depth=100, pattern=6, step_rate=2, steps=16, smooth=8),
    ]),
    ("auto_pan_pulse", "Auto-Pan Pulse", 4, 100, 0, 0, 4.0, [
        off(0), off(1),
        sl(2, mode=6, param1=48, param2=60, depth=70, pattern=19, step_rate=5, steps=8, smooth=40),
        sl(3, mode=1, param1=0, param2=100, depth=90, pattern=15, step_rate=4, steps=8, smooth=35),
    ]),
    ("wide_accents", "Wide Accents", 4, 100, 130, 0, 4.0, [
        off(0),
        sl(1, mode=0, param1=45, param2=70, depth=65, pattern=13, step_rate=2, steps=16, smooth=15),
        off(2),
        sl(3, mode=2, param1=0, param2=85, depth=100, pattern=13, step_rate=2, steps=16, smooth=25),
    ]),
    ("audio_trig_filter", "Audio Trig Filter", 3, 100, 0, 0, 4.0, [
        off(0), off(1),
        sl(2, mode=0, param1=32, param2=65, depth=95, pattern=10, step_rate=2, steps=8, trig=2, threshold=-22, smooth=15),
        off(3),
    ]),
    ("env_drive_bloom", "Env Drive Bloom", 2, 100, 110, 0, 4.0, [
        off(0),
        sl(1, mode=4, param1=60, param2=45, depth=15, pattern=15, step_rate=2, steps=8, trig=1, threshold=-28, env_depth=85, smooth=20),
        sl(2, mode=1, param1=40, param2=50, depth=10, pattern=9, step_rate=4, steps=8, trig=1, threshold=-28, env_depth=80, smooth=25),
        off(3),
    ]),
    ("midi_gated_fx", "MIDI Gated FX", 4, 100, 0, 0, 4.0, [
        off(0), off(1),
        sl(2, mode=0, param1=38, param2=60, depth=85, pattern=10, step_rate=2, steps=8, trig=3, smooth=12),
        sl(3, mode=0, param1=0, param2=50, depth=100, pattern=2, step_rate=2, steps=8, trig=3, smooth=8),
    ]),
    ("free_polyrhythm", "Free Polyrhythm", 3, 100, 120, 0, 5.0, [
        off(0),
        sl(1, mode=1, param1=55, param2=65, depth=80, pattern=21, step_rate=8, steps=5, trig=1, smooth=15),
        sl(2, mode=2, param1=48, param2=65, depth=90, pattern=22, step_rate=8, steps=7, trig=1, smooth=20),
        sl(3, mode=0, param1=20, param2=50, depth=75, pattern=4, step_rate=8, steps=8, trig=1, smooth=12),
    ]),
    ("full_4slot_chaos", "Full 4-Slot", 1, 95, 120, 0, 4.0, [
        sl(0, mode=1, param1=50, param2=35, depth=100, pattern=0, step_rate=4, steps=16, smooth=0,
           lvls=[0, 0, 0, 40, 0, 0, 15, 0, 0, 0, 0, 65, 0, 40, 82, 96]),
        sl(1, mode=2, param1=55, param2=60, depth=75, pattern=21, step_rate=2, steps=5, smooth=10),
        sl(2, mode=0, param1=45, param2=55, depth=85, pattern=22, step_rate=2, steps=7, smooth=15),
        sl(3, mode=3, param1=15, param2=70, depth=80, pattern=5, step_rate=2, steps=8, smooth=8),
    ]),
]


def manifest_entries():
    out = [
        '<%s#bank_factory>\n    a pset:Bank ;\n    lv2:appliesTo <%s> ;\n    rdfs:label "Factory" .\n' % (URI, URI)
    ]
    for pid, label, *_ in PRESETS:
        out.append(
            '<%s#preset_%s>\n    a pset:Preset ;\n    lv2:appliesTo <%s> ;\n    rdfs:label "%s" ;\n'
            '    rdfs:seeAlso <presets.ttl> .\n' % (URI, pid, URI, label)
        )
    return "\n".join(out)


def write_presets_ttl():
    out = ["""# Generated by scripts/gen_presets.py - edit the script, not this file.
@prefix lv2:   <http://lv2plug.in/ns/lv2core#> .
@prefix pset:  <http://lv2plug.in/ns/ext/presets#> .
@prefix rdfs:  <http://www.w3.org/2000/01/rdf-schema#> .
@prefix state: <http://lv2plug.in/ns/ext/state#> .
"""]
    for pid, label, sel_slot, mix, low_split, low_duck, free_hz, slots in PRESETS:
        cur = slots[sel_slot - 1]
        ports = {
            "slot": sel_slot, "mix": mix, "low_split": low_split, "scene": 0,
            "live_pad": 0, "prev_scene": 0, "next_scene": 0, "output_db": 0,
        }
        for k in FACE_KEYS:
            ports[k] = cur[k]
        lines = [
            "<%s#preset_%s>" % (URI, pid),
            "    a pset:Preset ;",
            "    lv2:appliesTo <%s> ;" % URI,
            '    rdfs:label "%s" ;' % label,
            "    pset:bank <%s#bank_factory> ;" % URI,
            "    lv2:port",
            " ,\n".join('        [ lv2:symbol "%s" ; pset:value %.1f ]' % (k, float(ports[k])) for k in PORTS) + " ;",
            "    state:state [",
        ]
        st_lines = []
        for s_idx, s in enumerate(slots):
            st_lines.append("        <%s#s%d_on> %d" % (URI, s_idx + 1, int(s["on"])))
            for k in FACE_KEYS:
                v = s[k]
                st_lines.append(
                    "        <%s#s%d_%s> %s" % (URI, s_idx + 1, k, str(int(v)) if k in INT_KEYS else "%.1f" % float(v))
                )
            if "lvls" in s:
                for k_idx, lv in enumerate(s["lvls"]):
                    st_lines.append("        <%s#s%d_lvl%d> %.1f" % (URI, s_idx + 1, k_idx + 1, float(lv)))
            if "shps" in s:
                for k_idx, sh in enumerate(s["shps"]):
                    st_lines.append("        <%s#s%d_shp%d> %d" % (URI, s_idx + 1, k_idx + 1, int(sh)))
        st_lines.append("        <%s#low_duck> %d" % (URI, int(low_duck)))
        st_lines.append("        <%s#free_hz> %.2f" % (URI, float(free_hz)))
        st_lines.append("        <%s#xy_x> 50.0" % URI)
        st_lines.append("        <%s#xy_y> 50.0" % URI)
        st_lines.append("        <%s#xy_on> 0" % URI)
        st_lines.append("        <%s#store_scene> 0" % URI)
        lines.append(" ;\n".join(st_lines))
        lines.append("    ] .\n")
        out.append("\n".join(lines))
    with open(os.path.join(ROOT, "lv2", "presets.ttl"), "w") as f:
        f.write("\n".join(out))


if __name__ == "__main__":
    write_presets_ttl()
    print("wrote lv2/presets.ttl (%d presets)" % len(PRESETS))
