#!/usr/bin/python3
"""Factory presets. gen_ttl.py imports this and writes them into lv2/manifest.ttl and lv2/presets.ttl;
build.sh also writes build/presets.inc, from which test_core.c renders every preset to a wav file.
Each preset sets every control port and every LV2 parameter (a missing value would keep the old one).
Edit this script, not those files."""
import os

URI = "http://dmtry3.github.io/zynthian-eka/grainflow"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# stream values in knob order, then the ones set in the widget
KEYS = ["offset", "rate", "length", "pitch", "reverse", "filter", "spray", "level", "fine", "spread", "interval", "on", "scan"]
DEFAULT = {"offset": 100, "rate": 100, "length": 500, "pitch": 0, "reverse": 0, "filter": 0, "spray": 0,
           "level": 0, "fine": 0, "spread": 0, "interval": 0, "on": 0, "scan": 0}
INTERVALS = ["Off", "Oct", "Oct +-", "Fifth", "Major", "Minor", "Sus", "Penta", "Wide",
             "Q Oct", "Q Fifth", "Q Major", "Q Minor", "Q Penta"]   # gx_interval_names
IV = {n: i for i, n in enumerate(INTERVALS)}
# presets with tempo sync on (Rate and Offset snap to note values)
SYNC = {"stutter", "reverse_echo", "pulse", "clockwork_glass"}
SOFT, SHARP = 0, 1
SINE, TRI, DRIFT, SH = range(4)                     # LFO shapes (GX_LFO_*)
MOD_DSTS = ["pitch", "offset", "length", "level", "filter", "scan"]
LFO_DEFAULT = [(0.3, SINE), (1.0, DRIFT)]
# Modulation of a preset: MODS[id] = (LFO 1 (rate, shape), LFO 2 (rate, shape), {(source, stream 1..3, dst): %})
# Sources 1, 2: the LFOs; 3: Env (the input's level).
# Pitch 100 % = +-2 semitones; Offset and Length 100 % = x 4 / 4; Level 100 % = silent at the LFO's bottom;
# Env on Level: +100 opens the stream as you play louder, -100 ducks it while you play.
MODS = {
    "tidal_memory": ((0.08, SINE), (0.11, DRIFT), {(1, 1, "scan"): 60, (1, 2, "scan"): -45,
                     (2, 1, "filter"): 25, (2, 2, "filter"): -20}),
    "breathing_light": ((0.13, DRIFT), (0.09, SINE), {(3, 1, "filter"): 80, (3, 2, "filter"): 65,
                        (3, 1, "level"): 30, (1, 2, "scan"): 20}),
    "velvet_drift": ((0.12, SH), (0.07, DRIFT), {(1, 1, "offset"): 35, (2, 1, "pitch"): 10,
                   (2, 2, "length"): -25, (3, 1, "level"): -45, (3, 2, "level"): -30}),
    "clockwork_glass": ((0.3, SH), (0.15, SINE), {(1, 1, "pitch"): 75, (2, 2, "pitch"): 65,
                      (2, 1, "level"): 35}),
    "stepped_sky": ((0.18, SINE), (0.31, DRIFT), {(1, 1, "pitch"): 100, (2, 2, "pitch"): 70,
                                                (2, 1, "offset"): 20}),
    "living_cloud": ((0.22, DRIFT), (0.07, DRIFT), {(1, 1, "offset"): 45, (1, 2, "length"): -35,
                                                 (2, 1, "pitch"): 7, (2, 2, "level"): 45,
                                                 (1, 3, "level"): -50}),
    # slow random drift plus a faster wobble, fully wet: tape wow and flutter
    "wow": ((0.8, DRIFT), (5.5, SINE), {(1, 1, "pitch"): 15, (2, 1, "pitch"): 4}),
    "cloud": ((0.1, SINE), (0.4, DRIFT), {(1, 1, "offset"): -40, (2, 2, "length"): 30, (2, 1, "pitch"): 5}),
    "shimmer": ((0.2, SINE), (1.0, DRIFT), {(1, 2, "level"): 50, (2, 3, "pitch"): 3}),
    # the two upper layers breathe against each other
    "freeze_pad": ((0.15, SINE), (0.3, DRIFT), {(1, 2, "level"): 60, (1, 3, "level"): -60, (2, 1, "pitch"): 5}),
    # the cloud comes up when you stop playing, and the louder you play the further back it reaches
    "afterglow": ((0.12, SINE), (0.5, DRIFT), {(3, 1, "level"): -80, (3, 2, "level"): -80, (3, 1, "offset"): 40,
                                               (2, 2, "pitch"): 6}),
    # the harder you play, the denser and higher the spark
    "sparks": ((0.3, SINE), (2.0, SH), {(3, 1, "pitch"): 50, (3, 1, "length"): -40, (2, 2, "offset"): 30}),
}
RATE_MODS = {"living_cloud": (70, 0)}  # depth %: other LFO -> this LFO's rate


def st(offset, rate, length, pitch=0, fine=0, reverse=0, filt=0, spread=0, level=0, spray=0, iv="Off", scan=0):
    return {"offset": offset, "rate": rate, "length": length, "pitch": pitch, "fine": fine, "reverse": reverse,
            "filter": filt, "spread": spread, "level": level, "spray": spray, "interval": IV[iv], "on": 1, "scan": scan}


# Extra State settings, shared by TTL generation and audio rendering fixtures.
EXTRA_DEFAULTS = {"env_attack": 0, "env_release": 300, "env_sensitivity": 0,
                  "lfo1_smooth": 0, "lfo2_smooth": 0, "lfo1_sync": 0, "lfo2_sync": 0,
                  "lfo1_div": 12, "lfo2_div": 12, "lfo1_mode": 0, "lfo2_mode": 0}
EXTRAS = {
    "tidal_memory": {"lfo1_smooth": 25, "lfo2_smooth": 60},
    "breathing_light": {"env_attack": 15, "env_release": 600, "env_sensitivity": 10,
                        "lfo1_smooth": 70},
    "velvet_drift": {"env_attack": 15, "env_release": 900, "env_sensitivity": 6,
                     "lfo1_smooth": 85, "lfo2_smooth": 65},
    "clockwork_glass": {"lfo1_sync": 1, "lfo2_sync": 1, "lfo1_div": 9, "lfo2_div": 17,
                        "lfo1_smooth": 12, "lfo2_smooth": 30, "lfo2_mode": 1},
}

OFF = dict(DEFAULT)

# (id, label, mix %, feedback %, shape, buffer s, [stream 1, 2, 3]); streams that are off keep sensible values,
# so turning them on gives something usable.
PRESETS = [
    ("tidal_memory", "Tidal Memory", 65, 15, 0.15, 8,
     [st(700, 70, 700, fine=5, filt=-35, spread=100, level=3, spray=15),
      st(1600, 110, 1000, pitch=-12, fine=-5, filt=-30, spread=100, level=-3, spray=20),
      dict(OFF, offset=2400, rate=160, length=1200, pitch=12, spread=90, level=-8)]),
    ("breathing_light", "Breathing Light", 65, 12, 0.25, 4,
     [st(180, 45, 450, filt=-80, spread=90, level=3, spray=10),
      st(700, 90, 700, pitch=12, filt=-65, spread=100, level=-4, spray=15),
      dict(OFF, offset=1200, rate=120, length=800, pitch=-12, spread=90, level=-8)]),
    ("velvet_drift", "Velvet Drift", 65, 18, SOFT, 8,
     [st(800, 65, 900, fine=7, spread=100, filt=-15, level=3, spray=25, scan=12),
      st(1600, 100, 1200, pitch=12, fine=-7, reverse=20, filt=-25, spread=100, level=-2, spray=20),
      dict(OFF, offset=2200, rate=150, length=1500, pitch=-12, spread=100, level=-8)]),
    ("clockwork_glass", "Clockwork Glass", 60, 12, 0.65, 4,
     [st(500, 125, 140, iv="Q Penta", spread=90),
      st(1000, 250, 300, pitch=-12, iv="Q Oct", spread=100, level=-6),
      dict(OFF, offset=1500, rate=500, length=500, pitch=12, spread=90, level=-9)]),
    ("stepped_sky", "Stepped Sky", 60, 15, SOFT, 4,
     [st(500, 90, 200, iv="Q Oct", spread=80, level=2, spray=15),
      st(900, 150, 400, pitch=-12, iv="Q Fifth", spread=100, level=-5, spray=20),
      dict(OFF, offset=1400, rate=220, length=600, pitch=12, spread=100, level=-9)]),
    # independent forward and backward source cursors; play a phrase, then Freeze to explore it
    ("slow_motion", "Slow Motion", 65, 15, SOFT, 4,
     [st(100, 45, 400, scan=25, filt=-15, spread=70, level=3, spray=10),
      st(900, 80, 600, pitch=12, scan=-15, filt=-25, spread=100, level=-4, spray=15),
      dict(OFF, offset=1800, rate=100, length=800, pitch=-12, scan=10, spread=90, level=-9)]),
    ("living_cloud", "Living Cloud", 65, 25, SOFT, 8,
     [st(900, 65, 900, fine=5, reverse=25, filt=-20, spread=100, spray=55),
      st(1600, 100, 1200, pitch=12, fine=-5, reverse=40, filt=-15, spread=100, level=-7, spray=60),
      st(2400, 140, 1600, pitch=-12, filt=-35, spread=90, level=-9, spray=50)]),
    ("init", "Init", 50, 0, SOFT, 4,
     [dict(OFF, offset=100, rate=100, length=500, pitch=12),
      st(250, 120, 400, pitch=0, spread=50, level=-3),
      dict(OFF, offset=400, rate=150, length=600, pitch=-12, spread=50, level=-3)]),
    # short grains just behind the input, slightly detuned both ways: drums and plucks get wider and denser
    ("thicken", "Thicken", 40, 0, SOFT, 4,
     [st(12, 12, 35, fine=10, spread=60, spray=30), st(18, 12, 35, fine=-10, spread=60, spray=30), dict(OFF, offset=30, rate=20, length=40, pitch=-12)]),
    # two detuned copies, panned: a chorus-like doubler
    ("doubler", "Doubler", 50, 0, SOFT, 4,
     [st(25, 40, 90, fine=14, spread=90, spray=25), st(40, 45, 100, fine=-14, spread=90, spray=25), dict(OFF, offset=60, rate=50, length=120, pitch=12, level=-9)]),
    ("octave_up", "Octave Up", 45, 0, SOFT, 4,
     [st(90, 40, 80, pitch=12, filt=-10, spread=30), dict(OFF, offset=120, rate=50, length=80, pitch=24, level=-9),
      dict(OFF, offset=30, rate=60, length=120, pitch=-12)]),
    ("sub_octave", "Sub Octave", 40, 0, SOFT, 4,
     [st(30, 50, 120, pitch=-12, filt=-45), dict(OFF, offset=40, rate=60, length=150, pitch=-24, filt=-50, level=-6),
      dict(OFF, offset=90, rate=40, length=80, pitch=12)]),
    # octaves and a fifth far behind the input, wide: a shimmer tail
    # an octave up written back into the buffer climbs again and again: a shimmer tail
    ("shimmer", "Shimmer", 35, 45, SOFT, 4,
     [st(500, 80, 400, pitch=12, spread=70, level=-4, spray=40), st(800, 100, 300, pitch=24, filt=20, spread=100, level=-10, spray=40),
      st(650, 120, 500, pitch=7, spread=80, level=-12, spray=40)]),
    # tiny gappy grains through a high-pass: a small broken speaker
    ("radio", "Radio", 90, 0, SHARP, 4,
     [st(8, 20, 22, filt=60), dict(OFF, offset=15, rate=35, length=18, filt=75, level=-6),
      dict(OFF, offset=40, rate=80, length=30, pitch=12, filt=70)]),
    # long, dark, reversed and panned grains far back: a reverb-like wash
    ("wash", "Wash", 40, 50, SOFT, 4,
     [st(300, 60, 800, fine=6, reverse=30, filt=-30, spread=100, level=0, spray=50),
      st(700, 80, 1000, fine=-6, reverse=30, filt=-30, spread=100, level=0, spray=50),
      st(1200, 120, 1500, pitch=12, reverse=30, filt=-15, spread=100, level=-6, spray=50)]),
    ("cloud", "Cloud", 60, 0, SOFT, 4,
     [st(1500, 20, 300, reverse=50, spread=100, level=2, spray=70), st(2500, 40, 600, pitch=-12, reverse=50, filt=-20, spread=100, level=-1, spray=70),
      dict(OFF, offset=2000, rate=60, length=400, pitch=12, reverse=50, spread=100, level=-8)]),
    # each grain plays the last 400 ms backwards, one after another
    ("reverse_echo", "Reverse Echo", 45, 25, SOFT, 4,
     [st(400, 400, 400, reverse=100, spread=30), dict(OFF, offset=800, rate=800, length=800, reverse=100, spread=60, level=-6),
      dict(OFF, offset=600, rate=400, length=400, pitch=12, reverse=100, level=-9)]),
    ("wow", "Wow", 100, 0, SOFT, 4,
     [st(30, 25, 70), dict(OFF, offset=50, rate=30, length=80, fine=-8, spread=60), dict(OFF, offset=40, rate=40, length=60, pitch=12, level=-9)]),
    ("stutter", "Stutter", 60, 20, SHARP, 4,
     [st(100, 125, 60), st(250, 250, 60, pitch=12, spread=40, level=-6), dict(OFF, offset=375, rate=375, length=60, pitch=-12)]),
    # grains take notes of a pentatonic scale over what you play: scattered arpeggio rain
    ("arp_rain", "Arp Rain", 45, 20, SOFT, 4,
     [st(300, 110, 160, spread=80, spray=50, iv="Penta"), st(450, 220, 220, pitch=12, spread=100, level=-6, spray=40, iv="Fifth"),
      dict(OFF, offset=600, rate=330, length=400, pitch=-12, spread=60, level=-8, interval=IV["Oct"])]),
    # chords out of one note: each grain takes a note of a major chord
    ("chord_cloud", "Chord Cloud", 50, 30, SOFT, 4,
     [st(200, 40, 500, spread=90, spray=60, iv="Major"), st(500, 60, 800, pitch=-12, filt=-30, spread=100, level=-4, spray=60, iv="Oct"),
      dict(OFF, offset=400, rate=50, length=600, spread=100, level=-6, spray=60, interval=IV["Minor"])]),
    # on the beat: 1/16 grains an eighth behind, the upper octave on 1/8 (tempo sync)
    ("pulse", "Pulse", 55, 30, SHARP, 4,
     [st(250, 125, 90, spread=30), st(375, 250, 120, pitch=12, spread=70, level=-5),
      dict(OFF, offset=500, rate=500, length=250, pitch=-12, level=-4)]),
    ("afterglow", "Afterglow", 60, 40, SOFT, 4,
     [st(800, 40, 600, spread=100, spray=60, filt=-20, level=4), st(1600, 70, 900, pitch=12, spread=100, level=-2, spray=60),
      dict(OFF, offset=1200, rate=80, length=700, pitch=-12, spread=100, level=-6)]),
    ("sparks", "Sparks", 45, 15, SHARP, 4,
     [st(80, 25, 60, pitch=12, spread=100, spray=70, iv="Oct"), st(200, 60, 40, pitch=24, spread=100, level=-8, spray=80),
      dict(OFF, offset=150, rate=40, length=80, pitch=7, spread=100, level=-6)]),
    # play a chord, press Freeze: it holds as a pad
    ("freeze_pad", "Freeze Pad", 55, 0, SOFT, 4,
     [st(800, 200, 2000, fine=5, filt=-20, spread=100, level=3), st(600, 250, 1500, pitch=12, spread=100, level=-3),
      st(1000, 300, 2500, pitch=-12, filt=-30, spread=100, level=-3)]),
]
PORTS = ["stream", "mix", "feedback", "freeze", "offset", "rate", "length", "pitch", "reverse", "filter", "spray", "level",
         "scan", "fine", "interval", "spread"]


def manifest_entries():
    out = ["<%s#bank_factory>\n    a pset:Bank ;\n    lv2:appliesTo <%s> ;\n    rdfs:label \"Factory\" .\n" % (URI, URI)]
    for pid, label, *_ in PRESETS:
        out.append("<%s#preset_%s>\n    a pset:Preset ;\n    lv2:appliesTo <%s> ;\n    rdfs:label \"%s\" ;\n"
                   "    rdfs:seeAlso <presets.ttl> .\n" % (URI, pid, URI, label))
    return "\n".join(out)


def write_presets_ttl():
    out = ["""# Generated by scripts/gen_presets.py - edit the script, not this file.
@prefix lv2:   <http://lv2plug.in/ns/lv2core#> .
@prefix pset:  <http://lv2plug.in/ns/ext/presets#> .
@prefix rdfs:  <http://www.w3.org/2000/01/rdf-schema#> .
@prefix state: <http://lv2plug.in/ns/ext/state#> .
"""]
    for pid, label, mix, fb, shape, buf, streams in PRESETS:
        s1 = streams[0]
        ports = {"stream": 1, "mix": mix, "feedback": fb, "freeze": 0}
        ports.update({k: s1[k] for k in PORTS[4:]})
        lines = ["<%s#preset_%s>" % (URI, pid), "    a pset:Preset ;", "    lv2:appliesTo <%s> ;" % URI,
                 "    rdfs:label \"%s\" ;" % label, "    pset:bank <%s#bank_factory> ;" % URI, "    lv2:port"]
        lines.append(" ,\n".join('        [ lv2:symbol "%s" ; pset:value %.1f ]' % (k, ports[k]) for k in PORTS) + " ;")
        lines.append("    state:state [")
        st_lines = []
        for i, s in enumerate(streams):
            for k in KEYS:
                v = s[k]
                st_lines.append("        <%s#s%d_%s> %s" % (URI, i + 1, k, str(int(v)) if k in ("on", "interval") else "%.1f" % v))
        st_lines.append("        <%s#buffer> %d" % (URI, buf))
        st_lines.append("        <%s#shape> %g" % (URI, shape))
        st_lines.append("        <%s#sync> %d" % (URI, 1 if pid in SYNC else 0))
        st_lines.append("        <%s#freeze_amount> 0.0" % URI)
        extra = dict(EXTRA_DEFAULTS, **EXTRAS.get(pid, {}))
        for key, value in extra.items():
            st_lines.append("        <%s#%s> %.1f" % (URI, key, value))
        l1, l2, cells = MODS.get(pid, (LFO_DEFAULT[0], LFO_DEFAULT[1], {}))
        for l, (rate, shp) in enumerate((l1, l2)):
            st_lines.append("        <%s#lfo%d_rate> %.2f" % (URI, l + 1, rate))
            st_lines.append("        <%s#lfo%d_shape> %d" % (URI, l + 1, shp))
            st_lines.append("        <%s#lfo%d_rate_mod> %d" % (URI, l + 1, RATE_MODS.get(pid, (0, 0))[l]))
        for l in (1, 2, 3):
            for s in (1, 2, 3):
                for d in MOD_DSTS:
                    st_lines.append("        <%s#m%d_s%d_%s> %d" % (URI, l, s, d, cells.get((l, s, d), 0)))
        lines.append(" ;\n".join(st_lines))
        lines.append("    ] .\n")
        out.append("\n".join(lines))
    with open(os.path.join(ROOT, "lv2", "presets.ttl"), "w") as f:
        f.write("\n".join(out))


def write_c(path):
    """build/presets.inc for test_core.c: GxParams of every preset."""
    rows = []
    for pid, label, mix, fb, shape, buf, streams in PRESETS:
        ss = []
        for s in streams:
            ss.append("{ %d, %g, %g, %g, %g, %g, %g, %g, %g, %g, %g, %d, %g }" % (
                s["on"], s["offset"], s["rate"], s["length"], s["pitch"], s["fine"], s["reverse"] / 100.0,
                s["filter"] / 100.0, s["spread"] / 100.0, s["level"], s["spray"] / 100.0, s["interval"], s["scan"] / 100.0))
        l1, l2, cells = MODS.get(pid, (LFO_DEFAULT[0], LFO_DEFAULT[1], {}))
        mods = ", ".join("{ %d, %d, %d, %g }" % (l - 1, s - 1, MOD_DSTS.index(d), v / 100.0) for (l, s, d), v in cells.items())
        rows.append('  { "%s", %g, %g, %g, %d, %d, { %s }, { { %g, %d }, { %g, %d } }, { %g, %g }, %d, { %s }, { %s } },' % (
            pid, mix / 100.0, fb / 100.0, shape, buf, 1 if pid in SYNC else 0, ", ".join(ss), l1[0], l1[1], l2[0], l2[1],
            RATE_MODS.get(pid, (0, 0))[0] / 100.0, RATE_MODS.get(pid, (0, 0))[1] / 100.0, len(cells),
            mods if mods else "{ 0, 0, 0, 0 }",
            ", ".join(str(EXTRAS.get(pid, {}).get(key, value)) for key, value in EXTRA_DEFAULTS.items())))
    with open(path, "w") as f:
        f.write("/* Generated by scripts/gen_presets.py. Stream: on, offset, rate, length, pitch, fine, reverse,\n"
                " * filter, spread (0..1 / -1..1), level dB, spray, interval, scan speed. */\n")
        f.write("static const struct { const char* id; float mix, feedback, shape; int buffer, sync;\n"
                "  struct { int on; float offset, rate, length, pitch, fine, reverse, filter, spread, level_db, spray;\n"
                "           int interval; float scan; } s[3];\n"
                "  struct { float rate; int shape; } lfo[2]; float rate_mod[2];\n"
                "  int nmods; struct { int l, s, d; float depth; } mods[8]; float extra[11]; }\n"
                "presets[] = {\n" + "\n".join(rows) + "\n};\n")


if __name__ == "__main__":
    write_presets_ttl()
    print("wrote lv2/presets.ttl (%d presets)" % len(PRESETS))
