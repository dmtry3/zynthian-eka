#!/usr/bin/python3
"""Generates lv2/manifest.ttl and lv2/eka_pulse.ttl.
Edit this script, not the generated TTL files.
Port and parameter order must match src/eka_pulse.c."""
import os

URI = "http://dmtry3.github.io/zynthian-eka/pulse"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SLOTS = 4
MAX_STEPS = 16

# 5 encoder pages, 4 knobs each (higher displayPriority first).
GROUPS = [
    ("g_main", "Main"),
    ("g_fx", "FX"),
    ("g_curve", "Curve"),
    ("g_trigger", "Trigger"),
    ("g_perform", "Perform"),
]

SLOT_NAMES = ["1:Time", "2:Drive", "3:Filter", "4:Gate"]
SCENE_NAMES = ["A", "B", "C", "D"]
PATTERN_NAMES = [
    "Custom",
    "Sidechain",
    "Trance Gate",
    "Stutter 16",
    "Euclid 3/8",
    "Euclid 5/8",
    "Euclid 7/16",
    "Syncopated",
    "Offbeat Pump",
    "Swell",
    "Fade Out",
    "Bounce",
    "Triplet Roll",
    "Heartbeat",
    "Broken Beat",
    "Tremolo",
    "Chop & Hold",
    "Stairs Up",
    "Stairs Down",
    "Peak Center",
    "Tape Stop",
    "Poly 5",
    "Poly 7",
    "Glitch",
    "Random",
]
STEP_RATES = ["1/32", "1/16T", "1/16", "1/8T", "1/8", "1/4", "1/2", "1 bar", "Free"]
TRIG_MODES = ["Sync", "Free", "Audio", "MIDI"]
LIVE_PADS = [
    "Off",
    "TapeStop",
    "Rpt 1/16",
    "Rpt 1/8",
    "Reverse",
    "Half 1/2",
    "GateChop",
    "FiltDrop",
    "BitCrush",
]


def enum_props(names, start=0):
    pts = " ,\n        ".join(
        '[ rdfs:label "%s" ; rdf:value %d ]' % (n, i + start)
        for i, n in enumerate(names)
    )
    return "lv2:portProperty lv2:integer , lv2:enumeration ;\n        lv2:scalePoint " + pts + " ;"


# Input control ports: indices 0..19 (must come first for Zynthian LV2 parameter numbering).
CONTROLS = [
    # 1. Main
    ("slot", "Slot", 4, 1, 4, enum_props(SLOT_NAMES, start=1), "g_main"),
    ("mix", "Mix %", 100, 0, 100, "", "g_main"),
    ("low_split", "Low Split", 0, 0, 500, 'lv2:scalePoint [ rdfs:label "Off" ; rdf:value 0 ] ;', "g_main"),
    ("scene", "Scene", 0, 0, 3, enum_props(SCENE_NAMES), "g_main"),
    # 2. FX (selected slot)
    ("mode", "Mode", 0, 0, 6, "lv2:portProperty lv2:integer ;", "g_fx"),
    ("param1", "Param 1", 0, 0, 100, "", "g_fx"),
    ("param2", "Param 2", 50, 0, 100, "", "g_fx"),
    ("depth", "Depth %", 100, -100, 100, "", "g_fx"),
    # 3. Curve (selected slot)
    ("pattern", "Pattern", 2, 0, len(PATTERN_NAMES) - 1, enum_props(PATTERN_NAMES), "g_curve"),
    ("step_rate", "Step Rate", 2, 0, len(STEP_RATES) - 1, enum_props(STEP_RATES), "g_curve"),
    ("steps", "Steps", 8, 1, MAX_STEPS, "lv2:portProperty lv2:integer ;", "g_curve"),
    ("warp", "Warp %", 0, -100, 100, "", "g_curve"),
    # 4. Trigger (selected slot)
    ("trig", "Trig", 0, 0, len(TRIG_MODES) - 1, enum_props(TRIG_MODES), "g_trigger"),
    ("threshold", "Threshold dB", -24, -60, 0, "", "g_trigger"),
    ("env_depth", "Env->Depth %", 0, -100, 100, "", "g_trigger"),
    ("smooth", "Smooth/Punch", 10, -100, 100, "", "g_trigger"),
    # 5. Perform
    ("live_pad", "Live Pad", 0, 0, len(LIVE_PADS) - 1, enum_props(LIVE_PADS), "g_perform"),
    ("prev_scene", "Prev Scene", 0, 0, 1, "lv2:portProperty lv2:toggled ;", "g_perform"),
    ("next_scene", "Next Scene", 0, 0, 1, "lv2:portProperty lv2:toggled ;", "g_perform"),
    ("output_db", "Output dB", 0, -24, 6, "", "g_perform"),
]

# Per-slot face values (order matches FX, Curve, Trigger pages).
SLOT_FACES = [
    ("mode", "Mode", "atom:Int", 0, 0, 6),
    ("param1", "Param 1", "atom:Float", 0, 0, 100),
    ("param2", "Param 2", "atom:Float", 50, 0, 100),
    ("depth", "Depth %", "atom:Float", 100, -100, 100),
    ("pattern", "Pattern", "atom:Int", 2, 0, len(PATTERN_NAMES) - 1),
    ("step_rate", "Step Rate", "atom:Int", 2, 0, len(STEP_RATES) - 1),
    ("steps", "Steps", "atom:Int", 8, 1, MAX_STEPS),
    ("warp", "Warp %", "atom:Float", 0, -100, 100),
    ("trig", "Trig", "atom:Int", 0, 0, len(TRIG_MODES) - 1),
    ("threshold", "Threshold dB", "atom:Float", -24, -60, 0),
    ("env_depth", "Env->Depth %", "atom:Float", 0, -100, 100),
    ("smooth", "Smooth/Punch", "atom:Float", 10, -100, 100),
]

# Default per-slot face values: Slot 1 (Time), 2 (Drive), 3 (Filter), 4 (Gate)
SLOT_DEFAULTS = [
    [0, 50, 50, 100, 20, 2, 16, 0, 0, -24, 0, 10],  # 1: Time
    [0, 50, 50, 100, 21, 2, 5, 0, 0, -24, 0, 10],   # 2: Drive
    [0, 50, 30, 100, 22, 2, 7, 0, 0, -24, 0, 10],   # 3: Filter
    [0, 0, 50, 100, 2, 2, 8, 0, 0, -24, 0, 10],     # 4: Gate (on by default)
]

OUT_MONITORS = [
    ("peak", "Input peak", 0, 0, 4),
    ("out_peak", "Output peak", 0, 0, 4),
    ("lim_gr", "Limiter GR", 0, 0, 1),
    ("loads", "Loads", 0, 0, 1e9),
    ("bpm", "Tempo", 120, 20, 400),
    ("sel", "Selected slot", 4, 1, 4),
    ("on_mask", "Slots on", 8, 0, 15),
]
for s in range(SLOTS):
    OUT_MONITORS.append(("phase%d" % (s + 1), "Slot %d phase" % (s + 1), 0, 0, 1))
for s in range(SLOTS):
    OUT_MONITORS.append(("val%d" % (s + 1), "Slot %d value" % (s + 1), 0, 0, 1))
for s in range(SLOTS):
    OUT_MONITORS.append(("steps%d" % (s + 1), "Slot %d steps" % (s + 1), SLOT_DEFAULTS[s][6], 1, MAX_STEPS))
for s in range(SLOTS):
    OUT_MONITORS.append(("mode%d" % (s + 1), "Slot %d mode" % (s + 1), SLOT_DEFAULTS[s][0], 0, 6))
for idx, (sym, name, _rng, _d, lo, hi) in enumerate(SLOT_FACES):
    OUT_MONITORS.append(("cur_" + sym, "Selected " + name, SLOT_DEFAULTS[3][idx], lo, hi))


def params():
    """(symbol, label, range, default, min, max) of LV2 patch:writable parameters."""
    out = []
    for s in range(SLOTS):
        out.append(("s%d_on" % (s + 1), "Slot %d on" % (s + 1), "atom:Int", 1 if s == 3 else 0, 0, 1))
        for idx, (sym, name, rng, _d, lo, hi) in enumerate(SLOT_FACES):
            out.append(("s%d_%s" % (s + 1, sym), "Slot %d %s" % (s + 1, name), rng, SLOT_DEFAULTS[s][idx], lo, hi))
        for k in range(MAX_STEPS):
            out.append(("s%d_lvl%d" % (s + 1, k + 1), "Slot %d step %d level" % (s + 1, k + 1), "atom:Float", 100, 0, 100))
        for k in range(MAX_STEPS):
            out.append(("s%d_shp%d" % (s + 1, k + 1), "Slot %d step %d shape" % (s + 1, k + 1), "atom:Int", 1, 0, 5))
    out.append(("low_duck", "Low Duck", "atom:Int", 0, 0, 1))
    out.append(("free_hz", "Free Rate Hz", "atom:Float", 4.0, 0.25, 32.0))
    return out


def main():
    os.makedirs(os.path.join(ROOT, "lv2"), exist_ok=True)
    lines = ["""# Generated by scripts/gen_ttl.py - edit the script, not this file.
@prefix lv2:    <http://lv2plug.in/ns/lv2core#> .
@prefix atom:   <http://lv2plug.in/ns/ext/atom#> .
@prefix midi:   <http://lv2plug.in/ns/ext/midi#> .
@prefix urid:   <http://lv2plug.in/ns/ext/urid#> .
@prefix state:  <http://lv2plug.in/ns/ext/state#> .
@prefix doap:   <http://usefulinc.com/ns/doap#> .
@prefix patch:  <http://lv2plug.in/ns/ext/patch#> .
@prefix pprops: <http://lv2plug.in/ns/ext/port-props#> .
@prefix pg:     <http://lv2plug.in/ns/ext/port-groups#> .
@prefix time:   <http://lv2plug.in/ns/ext/time#> .
@prefix rdf:    <http://www.w3.org/1999/02/22-rdf-syntax-ns#> .
@prefix rdfs:   <http://www.w3.org/2000/01/rdf-schema#> .
"""]
    for i, (sym, name) in enumerate(GROUPS):
        lines.append(
            '<%s#%s> a pg:Group ; lv2:symbol "%s" ; lv2:name "%s" ; lv2:index %d ; lv2:displayPriority %d .'
            % (URI, sym, sym, name, i, (len(GROUPS) - i) * 10)
        )
    lines.append("")
    lines.append("# Slot parameters, steps and settings without a face knob: patch:writable + notOnGUI.")
    plist = params()
    for sym, label, rng, d, lo, hi in plist:
        lines.append("""<%s#%s>
    a lv2:Parameter ;
    lv2:symbol "%s" ;
    rdfs:label "%s" ;
    rdfs:range %s ;
    lv2:default %s ; lv2:minimum %s ; lv2:maximum %s ;
    pprops:notOnGUI true ;
    lv2:portProperty pprops:notOnGUI .
""" % (URI, sym, sym, label, rng, d, lo, hi))
    writable = " ,\n        ".join("<%s#%s>" % (URI, p[0]) for p in plist)
    lines.append("""<%s>
    a lv2:Plugin , lv2:ModulatorPlugin ;
    doap:name "Eka Pulse" ;
    doap:license <http://opensource.org/licenses/GPL-3.0> ;
    lv2:minorVersion 0 ;
    lv2:microVersion 1 ;
    lv2:requiredFeature urid:map ;
    lv2:optionalFeature lv2:hardRTCapable ;
    lv2:extensionData state:interface ;
    patch:writable
        %s ;
    # Control input ports MUST be indices 0..N-1: Zynthian numbers LV2 parameters from len(control ports)+1.
    lv2:port""" % (URI, writable))

    ports = []
    prio = {}
    for i, (sym, name, d, lo, hi, extra, grp) in enumerate(CONTROLS):
        prio[grp] = prio.get(grp, 50) - 10
        ports.append("""    [
        a lv2:InputPort , lv2:ControlPort ;
        lv2:index %d ; lv2:symbol "%s" ; lv2:name "%s" ;
        lv2:default %s ; lv2:minimum %s ; lv2:maximum %s ;%s
        lv2:displayPriority %d ; pg:group <%s#%s> ;
    ]""" % (i, sym, name, d, lo, hi, ("\n        " + extra) if extra else "", prio[grp], URI, grp))

    i = len(CONTROLS)
    for io, sym, name in (
        ("Input", "in_l", "In L"),
        ("Input", "in_r", "In R"),
        ("Output", "out_l", "Out L"),
        ("Output", "out_r", "Out R"),
    ):
        ports.append("""    [
        a lv2:%sPort , lv2:AudioPort ;
        lv2:index %d ; lv2:symbol "%s" ; lv2:name "%s" ;
    ]""" % (io, i, sym, name))
        i += 1

    ports.append("""    [
        a lv2:InputPort , atom:AtomPort ;
        atom:bufferType atom:Sequence ;
        atom:supports patch:Message , time:Position , midi:MidiEvent ;
        lv2:designation lv2:control ;
        lv2:index %d ; lv2:symbol "control" ; lv2:name "Control" ;
    ]""" % i)
    i += 1

    ports.append("""    [
        a lv2:OutputPort , atom:AtomPort ;
        atom:bufferType atom:Sequence ;
        atom:supports patch:Message ;
        lv2:designation lv2:control ;
        lv2:index %d ; lv2:symbol "notify" ; lv2:name "Notify" ;
    ]""" % i)
    i += 1

    for sym, name, d, lo, hi in OUT_MONITORS:
        ports.append("""    [
        a lv2:OutputPort , lv2:ControlPort ;
        lv2:index %d ; lv2:symbol "%s" ; lv2:name "%s" ;
        lv2:default %s ; lv2:minimum %s ; lv2:maximum %s ;
    ]""" % (i, sym, name, d, lo, hi if hi < 1e8 else "1000000000"))
        i += 1

    lines.append(" ,\n".join(ports) + " .")
    with open(os.path.join(ROOT, "lv2", "eka_pulse.ttl"), "w") as f:
        f.write("\n".join(lines) + "\n")

    with open(os.path.join(ROOT, "lv2", "manifest.ttl"), "w") as f:
        f.write("""# Generated by scripts/gen_ttl.py - edit the script, not this file.
@prefix lv2:  <http://lv2plug.in/ns/lv2core#> .
@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .

<%s>
    a lv2:Plugin ;
    lv2:binary <eka_pulse.so> ;
    rdfs:seeAlso <eka_pulse.ttl> .
""" % URI)

    print("wrote lv2/eka_pulse.ttl, lv2/manifest.ttl (%d ports, %d params)" % (i, len(plist)))


if __name__ == "__main__":
    main()
