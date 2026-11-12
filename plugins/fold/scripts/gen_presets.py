#!/usr/bin/env python3
"""Generate the factory presets: lv2/presets.ttl and lv2/manifest.ttl (edit this script, not those files).
Each preset sets every control port and every LV2 parameter (a missing one would keep the value of the
preset before). Ports and parameters, their defaults and ranges are read from lv2/eka_fold.ttl; a value out of
range stops the script. test/preset_check.c plays every preset through the plugin (scripts/test.sh).
SPDX-License-Identifier: GPL-3.0-or-later"""
import math
import os
import re
import sys

URI = "http://dmtry3.github.io/zynthian-eka/fold"
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")


def read_ttl():
    """(control input ports [(symbol, default, min, max)], parameters [(symbol, default, min, max)])"""
    t = open(os.path.join(ROOT, "lv2", "eka_fold.ttl")).read()
    ports = []
    for m in re.finditer(r"a lv2:InputPort , lv2:ControlPort ;(.*?)\]", t, re.S):
        b = m.group(1)
        sym = re.search(r'lv2:symbol "(\w+)"', b).group(1)
        idx = int(re.search(r"lv2:index (\d+)", b).group(1))
        d, lo, hi = (float(re.search(r"lv2:%s (-?[\d.]+)" % k, b).group(1)) for k in ("default", "minimum", "maximum"))
        ports.append((idx, sym, d, lo, hi))
    ports = [p[1:] for p in sorted(ports)]
    params = []
    for m in re.finditer(r"<%s#(\w+)>\s+a lv2:Parameter ;(.*?) \.\n" % re.escape(URI), t, re.S):
        b = m.group(2)
        d, lo, hi = (int(re.search(r"lv2:%s (-?\d+)" % k, b).group(1)) for k in ("default", "minimum", "maximum"))
        params.append((m.group(1), d, lo, hi))
    return ports, params


PORTS, PARAMS = read_ttl()

# the knobs' units (fold_core.c)
SINE_SRC, VA, FM2, WAVE, HARM, PD = range(6)
TRI, SINE, SAW, SQUARE = range(4)
FREE, KEY, RATIO = range(3)
VCA, LP, COMBO = range(3)
ONCE, GATE, CYCLE = range(3)
OFF, TUNED, FREE_DL, MODAL, STRING = range(5)
POLY, MONO, LEGATO = range(3)
RATIOS = [1 / 8, 1 / 6, 1 / 4, 1 / 3, 1 / 2, 2 / 3, 3 / 4, 1, 5 / 4, 4 / 3, 3 / 2, 5 / 3, 2, 5 / 2, 3, 7 / 2, 4, 5, 6, 7,
          8, 9, 10, 12, 16]
VA_SAW, VA_SQUARE = 100 / 3, 200 / 3      # Color / Morph of VA (0 triangle)
FM2_RATIO1 = 24                           # Harmonics of FM2: 1.0; 40 = 1.62, 56 = 2.33, 88 = 5.26, 0 = 0.5


def ratio(r):
    """Mod Freq in Ratio mode for the ratio r"""
    return RATIOS.index(r) * 100.0 / (len(RATIOS) - 1)


def free_hz(hz):
    """Mod Freq in Free mode: 0.01 Hz .. 10 kHz, logarithmic"""
    return 100.0 * math.log(hz / 0.01) / math.log(1e6)


def decay(ms):
    """Gate Decay for ms: 10 ms .. 2 s, logarithmic"""
    return 100.0 * math.log(ms / 10.0) / math.log(200.0)


def tuned(semis):
    """Delay Time in Tuned for semitones from the note"""
    return 50.0 + semis * 50.0 / 24.0


def free_ms(ms):
    """Delay Time in Free for ms: 2 s x (Time / 100)^3"""
    return 100.0 * (ms / 2000.0) ** (1.0 / 3.0)


def glide(ms):
    """Glide for ms: 2 s x (Glide / 100)^2"""
    return 100.0 * math.sqrt(ms / 2000.0)


def eq_freq(hz):
    """EQ Freq (0..100) for hz: 100 Hz .. 10 kHz, logarithmic"""
    return int(round(100.0 * math.log(hz / 100.0) / math.log(100.0)))


def db(x):
    """EQ Gain in tenths of a dB"""
    return int(round(x * 10))


# (id, label "Category - Name", ports, parameters): the cells of the matrix as m_<source>_<destination>.
# The id is the preset's URI (snapshots keep it): renaming changes only the label.
PRESETS = [
    ("init", "Init", {}, {}),

    # strikes: short function generators, the lopass gate's "bonk"
    # a membrane: the body rings ~150 ms, the pitch drops 3 semitones in the first 40 ms, an off-harmonic overtone
    # (FM at 3/2); harder = brighter and more open (the first one, 30 ms, was only a click: 2026-11-12)
    ("bongo", "Perc - Bongo",
     {"mod_freq": ratio(3 / 2), "mod_index": 12, "timbre": 15, "gate_decay": decay(150), "fg1_rise": 1,
      "fg1_fall": 150, "fg2_rise": 1, "fg2_fall": 40, "level": 100},
     {"fg1_mode": ONCE, "fg2_mode": ONCE, "m_fg2_pitch": 25, "m_fg2_index": 20, "m_vel_timbre": 25,
      "m_vel_lpg": 30}),
    ("marimba", "Perc - Marimba",
     {"mod_freq": ratio(4), "mod_index": 20, "timbre": 20, "gate_decay": decay(250), "fg1_rise": 1, "fg1_fall": 80,
      "level": 85},
     {"fg1_mode": ONCE, "m_vel_timbre": 25, "m_key_timbre": -20}),
    ("woodblock", "Perc - Woodblock",
     {"source": FM2, "harmonics": 88, "color": 40, "gate_mode": LP, "gate_decay": decay(25), "fg1_rise": 1,
      "fg1_fall": 15, "level": 90},
     {"fg1_mode": ONCE, "m_vel_color": 30}),
    ("low_tom", "Perc - Low Tom",
     {"timbre": 50, "symmetry": 10, "gate_decay": decay(200), "fg1_rise": 1, "fg1_fall": 120, "fg2_rise": 1,
      "fg2_fall": 150, "level": 85},
     {"fg1_mode": ONCE, "fg2_mode": ONCE, "m_fg2_pitch": 40, "m_vel_timbre": 20}),

    # strings: the delay Tuned to the note (Karplus-Strong), a short strike into it
    ("pluck", "String - Pluck",
     {"source": VA, "color": VA_SAW, "morph": VA_SAW, "gate_decay": decay(30), "fg1_rise": 1, "fg1_fall": 8,
      "delay_mode": TUNED, "delay_time": tuned(0), "delay_fb": 97, "delay_mix": 85, "level": 80},
     {"fg1_mode": ONCE, "delay_shaper": 10, "delay_eq_freq": eq_freq(2500), "delay_eq_gain": db(-0.3)}),
    ("harp", "String - Harp",
     {"timbre": 40, "gate_decay": decay(60), "fg1_rise": 1, "fg1_fall": 20, "delay_mode": TUNED,
      "delay_time": tuned(0), "delay_fb": 95, "delay_mix": 70, "level": 85},
     {"fg1_mode": ONCE, "delay_shaper": 0, "delay_eq_freq": eq_freq(1600), "delay_eq_gain": db(-0.5)}),
    # (a string tuned above the note rings only on the harmonics of the strike that fall on its own: a square has
    # no even ones, an octave up it stayed at -26 dB; a saw has them all)
    ("snap_string", "String - Snap",
     {"source": VA, "color": VA_SAW, "morph": VA_SAW, "timbre": 30, "gate_decay": decay(20), "fg1_rise": 1,
      "fg1_fall": 5, "delay_mode": TUNED, "delay_time": tuned(12), "delay_fb": 96, "delay_mix": 80, "level": 100},
     {"fg1_mode": ONCE, "delay_shaper": 25, "delay_eq_gain": 0, "m_vel_dfb": 3}),
    # a burst of noise (FM2 with strong feedback) plucks it; Legato glides the string itself
    ("koto_glide", "String - Koto Glide",
     {"source": FM2, "harmonics": FM2_RATIO1, "color": 70, "morph": 10, "gate_decay": decay(40), "fg1_rise": 1,
      "fg1_fall": 25, "delay_mode": TUNED, "delay_time": tuned(0), "delay_fb": 97, "delay_mix": 85,
      "play_mode": LEGATO, "glide": glide(60), "level": 100},
     {"fg1_mode": ONCE, "delay_shaper": 20, "delay_eq_freq": eq_freq(3000), "delay_eq_gain": db(-0.3)}),

    # resonators (Delay Modal / String): a short strike of the voice excites them; Feedback = their Decay
    ("modal_bell", "Bell - Modal",
     {"source": FM2, "harmonics": FM2_RATIO1, "color": 70, "morph": 10, "gate_decay": decay(30), "fg1_rise": 1,
      "fg1_fall": 10, "delay_mode": MODAL, "delay_time": tuned(0), "delay_fb": 85, "delay_mix": 100, "level": 60},
     {"fg1_mode": ONCE, "res_structure": 70, "res_brightness": 70, "res_position": 10, "m_vel_lpg": 30}),
    ("modal_bar", "Perc - Bar",
     {"timbre": 40, "gate_decay": decay(30), "fg1_rise": 1, "fg1_fall": 6, "delay_mode": MODAL,
      "delay_time": tuned(0), "delay_fb": 55, "delay_mix": 100, "level": 100},
     {"fg1_mode": ONCE, "res_structure": 40, "res_brightness": 45, "res_position": 30, "m_vel_dfb": 15}),
    ("string_sitar", "String - Sitar",
     {"source": VA, "color": VA_SAW, "morph": VA_SAW, "gate_decay": decay(30), "fg1_rise": 1, "fg1_fall": 8,
      "delay_mode": STRING, "delay_time": tuned(0), "delay_fb": 88, "delay_mix": 100, "level": 45},
     {"fg1_mode": ONCE, "res_structure": 5, "res_brightness": 60}),
    ("string_piano", "String - Piano",
     {"timbre": 40, "gate_decay": decay(40), "fg1_rise": 1, "fg1_fall": 10, "delay_mode": STRING,
      "delay_time": tuned(0), "delay_fb": 80, "delay_mix": 90, "level": 35},
     {"fg1_mode": ONCE, "res_structure": 55, "res_brightness": 50, "m_vel_timbre": 30, "m_key_dfb": -15}),

    # bells: FM2 at ratios off the harmonics, long strikes
    ("glass_bell", "Bell - Glass",
     {"source": FM2, "harmonics": 40, "color": 45, "gate_decay": decay(1500), "fg1_rise": 2, "fg1_fall": 2500,
      "level": 80},
     {"fg1_mode": ONCE, "m_vel_color": 30, "m_fg1_color": 20}),
    ("gong", "Bell - Gong",
     {"source": FM2, "harmonics": 56, "color": 60, "timbre": 30, "mod_mode": FREE, "mod_freq": free_hz(0.3),
      "gate_decay": decay(1800), "fg1_rise": 5, "fg1_fall": 4000, "delay_mode": FREE_DL,
      "delay_time": free_ms(300), "delay_fb": 45, "delay_mix": 35, "level": 75},
     {"route_fm": 0, "fg1_mode": ONCE, "delay_shaper": 40, "m_mod_color": 20, "m_vel_timbre": 20}),
    ("tubular", "Bell - Tubular",
     {"source": WAVE, "harmonics": 10, "color": 90, "morph": 50, "gate_decay": decay(1500), "fg1_rise": 1,
      "fg1_fall": 3000, "level": 100},
     {"fg1_mode": ONCE, "m_fg1_color": 15}),

    # pads: slow function generators, the Mod OSC as an LFO through the matrix
    ("fold_pad", "Pad - Fold",
     {"source": VA, "harmonics": 52, "color": VA_SAW, "morph": VA_SAW, "timbre": 30, "gate_mode": LP,
      "gate_decay": decay(800), "fg1_rise": 1200, "fg1_fall": 2500, "fg2_rise": 3000, "fg2_fall": 3000,
      "delay_mode": FREE_DL, "delay_time": free_ms(500), "delay_fb": 30, "delay_mix": 25, "level": 60},
     {"fg2_mode": CYCLE, "m_fg2_timbre": 40, "delay_shaper": 0}),
    ("wave_drift", "Pad - Wave Drift",
     {"source": WAVE, "harmonics": 40, "mod_mode": FREE, "mod_freq": free_hz(0.2), "fg1_rise": 800,
      "fg1_fall": 2000, "gate_decay": decay(600), "delay_mode": FREE_DL, "delay_time": free_ms(700),
      "delay_fb": 40, "delay_mix": 30, "level": 65},
     {"route_fm": 0, "m_mod_morph": 40, "m_mod_color": 25, "delay_shaper": 0}),
    ("soft_choir", "Pad - Soft Choir",
     {"source": VA, "harmonics": 52, "color": 0, "morph": 10, "timbre": 15, "gate_mode": LP,
      "gate_decay": decay(500), "fg1_rise": 600, "fg1_fall": 1500, "fg2_rise": 150, "fg2_fall": 150, "level": 70},
     {"fg2_mode": CYCLE, "m_fg2_pitch": 1, "m_wheel_timbre": 40}),

    ("harm_organ", "Pad - Organ",
     {"source": HARM, "harmonics": 0, "color": 25, "morph": 70, "gate_mode": LP, "gate_decay": decay(700),
      "fg1_rise": 400, "fg1_fall": 1200, "fg2_rise": 2500, "fg2_fall": 2500, "mod_mode": FREE,
      "mod_freq": free_hz(5.5), "delay_mode": FREE_DL, "delay_time": free_ms(450), "delay_fb": 25, "delay_mix": 20,
      "level": 70},
     {"route_fm": 0, "fg2_mode": CYCLE, "m_fg2_color": 25, "m_mod_pitch": 1, "m_wheel_harmonics": 50,
      "delay_shaper": 0}),

    # drones: Cycle and long feedback
    ("west_coast_drone", "Drone - West Coast",
     {"mod_shape": TRI, "mod_freq": ratio(3 / 2), "mod_index": 30, "fg1_rise": 2000, "fg1_fall": 4000,
      "fg2_rise": 4000, "fg2_fall": 4000, "gate_decay": decay(1000), "delay_mode": TUNED, "delay_time": tuned(0),
      "delay_fb": 99, "delay_mix": 40, "level": 60},
     {"route_timbre": 1, "fg2_mode": CYCLE, "m_fg2_index": 40, "delay_eq_gain": db(-0.2)}),
    ("dark_hum", "Drone - Dark Hum",
     {"source": FM2, "harmonics": 0, "color": 40, "morph": 60, "timbre": 20, "drive": 30, "gate_mode": LP,
      "gate_decay": decay(1000), "fg1_rise": 300, "fg1_fall": 2000, "fg2_rise": 5000, "fg2_fall": 5000,
      "play_mode": MONO, "level": 100},
     {"fg2_mode": CYCLE, "m_fg2_color": 30}),

    # basses: Mono / Legato
    ("fm_bass", "Bass - FM",
     {"source": FM2, "harmonics": FM2_RATIO1, "color": 55, "drive": 30, "gate_decay": decay(150), "fg1_rise": 1,
      "fg1_fall": 300, "fg2_rise": 1, "fg2_fall": 200, "play_mode": MONO, "level": 100},
     {"fg2_mode": ONCE, "m_fg2_color": 40, "m_vel_color": 20}),
    ("fold_bass", "Bass - Fold",
     {"timbre": 55, "symmetry": 15, "drive": 40, "gate_decay": decay(200), "fg1_rise": 2, "fg1_fall": 200,
      "fg2_rise": 1, "fg2_fall": 250, "play_mode": LEGATO, "glide": glide(80), "level": 100},
     {"fg2_mode": ONCE, "m_fg2_timbre": 30}),
    ("sync_bass", "Bass - Sync",
     {"source": VA, "color": VA_SAW, "morph": VA_SAW, "mod_freq": ratio(1 / 2), "timbre": 20,
      "gate_decay": decay(200), "fg1_rise": 1, "fg1_fall": 250, "fg2_rise": 1, "fg2_fall": 400,
      "play_mode": MONO, "level": 100},
     {"route_fm": 0, "route_sync": 1, "pitch": 12, "fg2_mode": ONCE, "m_fg2_timbre": 40}),

    ("pd_bass", "Bass - PD",
     {"source": PD, "harmonics": FM2_RATIO1, "color": 45, "morph": 60, "gate_decay": decay(200), "fg1_rise": 1,
      "fg1_fall": 300, "fg2_rise": 1, "fg2_fall": 250, "play_mode": MONO, "level": 100},
     {"fg2_mode": ONCE, "m_fg2_color": 40, "m_vel_color": 20}),

    # leads
    ("glide_lead", "Lead - Glide",
     {"source": VA, "color": VA_SAW, "morph": VA_SAW, "mod_mode": FREE, "mod_freq": free_hz(5.5), "gate_mode": LP,
      "gate_res": 30, "gate_decay": decay(300), "fg1_rise": 5, "fg1_fall": 400, "play_mode": LEGATO,
      "glide": glide(120), "level": 100},
     {"route_fm": 0, "m_mod_pitch": 1, "m_wheel_timbre": 40}),
    ("sync_lead", "Lead - Sync",
     {"source": WAVE, "harmonics": 30, "mod_freq": ratio(1 / 2), "gate_decay": decay(300), "fg1_rise": 3,
      "fg1_fall": 400, "fg2_rise": 1, "fg2_fall": 600, "play_mode": LEGATO, "glide": glide(60), "level": 100},
     {"route_fm": 0, "route_sync": 1, "pitch": 12, "fg2_mode": ONCE, "m_fg2_morph": 50}),

    ("harm_lead", "Lead - Harm Fold",
     {"source": HARM, "harmonics": 30, "color": 15, "morph": 40, "timbre": 45, "symmetry": 10, "gate_mode": LP,
      "gate_res": 25, "gate_decay": decay(300), "fg1_rise": 5, "fg1_fall": 400, "fg2_rise": 600, "fg2_fall": 600,
      "play_mode": LEGATO, "glide": glide(90), "level": 100},
     {"fg2_mode": CYCLE, "m_fg2_timbre": 20, "m_wheel_color": 40, "m_at_timbre": 30}),

    # echoes and textures
    ("echo_plucks", "FX - Echo Plucks",
     {"timbre": 40, "gate_decay": decay(50), "fg1_rise": 1, "fg1_fall": 40, "delay_mode": FREE_DL,
      "delay_time": free_ms(375), "delay_fb": 55, "delay_mix": 40, "level": 80},
     {"fg1_mode": ONCE, "delay_shaper": 20, "delay_eq_freq": eq_freq(4000), "delay_eq_gain": db(-1.0)}),
    ("random_bloops", "FX - Random Bloops",
     {"timbre": 20, "mod_mode": FREE, "mod_freq": free_hz(4), "mod_shape": SQUARE, "gate_decay": decay(40),
      "fg1_rise": 2, "fg1_fall": 120, "delay_mode": FREE_DL, "delay_time": free_ms(250), "delay_fb": 50,
      "delay_mix": 35, "level": 75},
     {"route_fm": 0, "fg1_mode": CYCLE, "m_rnd_pitch": 25, "m_rnd_timbre": 40}),
    ("metal_texture", "FX - Metal Texture",
     {"source": WAVE, "harmonics": 60, "color": 70, "morph": 30, "mod_freq": ratio(3 / 4), "fg1_rise": 300,
      "fg1_fall": 1500, "fg2_rise": 2000, "fg2_fall": 2000, "gate_decay": decay(600), "delay_mode": TUNED,
      "delay_time": tuned(0), "delay_fb": 90, "delay_mix": 40, "level": 60},
     {"route_fm": 0, "route_sync": 1, "fg2_mode": CYCLE, "m_fg2_morph": 40, "delay_shaper": 70}),
]


def check(pid, kind, sym, v, lo, hi):
    if not lo <= v <= hi:
        sys.exit("preset %s: %s %s = %s outside %s..%s" % (pid, kind, sym, v, lo, hi))


def preset_ttl(pid, label, ports, params):
    known_p = {p[0] for p in PORTS}
    known_i = {p[0] for p in PARAMS}
    for k in ports:
        if k not in known_p:
            sys.exit("preset %s: no port %s" % (pid, k))
    for k in params:
        if k not in known_i:
            sys.exit("preset %s: no parameter %s" % (pid, k))
    out = ["<%s#preset_%s>" % (URI, pid), "    a pset:Preset ;", "    lv2:appliesTo <%s> ;" % URI,
           '    rdfs:label "%s" ;' % label, "    pset:bank <%s#bank_factory> ;" % URI, "    lv2:port"]
    plines = []
    for sym, d, lo, hi in PORTS:
        v = round(float(ports.get(sym, d)), 3)
        check(pid, "port", sym, v, lo, hi)
        plines.append('        [ lv2:symbol "%s" ; pset:value %s ]' % (sym, repr(v)))
    out.append(" ,\n".join(plines) + " ;")
    out.append("    state:state [")
    lines = []
    for sym, d, lo, hi in PARAMS:
        v = int(round(params.get(sym, d)))
        check(pid, "parameter", sym, v, lo, hi)
        lines.append("        <%s#%s> %d" % (URI, sym, v))
    out.append(" ;\n".join(lines))
    out.append("    ] .\n")
    return "\n".join(out)


HEAD = """@prefix lv2:   <http://lv2plug.in/ns/lv2core#> .
@prefix pset:  <http://lv2plug.in/ns/ext/presets#> .
@prefix rdfs:  <http://www.w3.org/2000/01/rdf-schema#> .
@prefix state: <http://lv2plug.in/ns/ext/state#> .
"""

if __name__ == "__main__":
    ids = [p[0] for p in PRESETS]
    if len(set(ids)) != len(ids):
        sys.exit("duplicate preset ids")
    with open(os.path.join(ROOT, "lv2", "presets.ttl"), "w") as f:
        f.write("# Generated by scripts/gen_presets.py - edit the script, not this file.\n" + HEAD + "\n")
        for p in PRESETS:
            f.write(preset_ttl(*p) + "\n")
    with open(os.path.join(ROOT, "lv2", "manifest.ttl"), "w") as f:
        f.write("# Generated by scripts/gen_presets.py - edit the script, not this file.\n" + HEAD + "\n")
        f.write("<%s>\n    a lv2:Plugin ;\n    lv2:binary <eka_fold.so> ;\n"
                "    rdfs:seeAlso <eka_fold.ttl> .\n\n" % URI)
        f.write('<%s#bank_factory>\n    a pset:Bank ;\n    lv2:appliesTo <%s> ;\n    rdfs:label "Factory" .\n\n'
                % (URI, URI))
        for pid, label, _, _ in PRESETS:
            f.write("<%s#preset_%s>\n    a pset:Preset ;\n    lv2:appliesTo <%s> ;\n    rdfs:label \"%s\" ;\n"
                    "    rdfs:seeAlso <presets.ttl> .\n\n" % (URI, pid, URI, label))
    print("Wrote lv2/presets.ttl and lv2/manifest.ttl (%d presets, %d ports, %d parameters each)"
          % (len(PRESETS), len(PORTS), len(PARAMS)))
