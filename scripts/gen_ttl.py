#!/usr/bin/python3
"""Writes lv2/manifest.ttl and lv2/eka_grainflow.ttl, and with gen_presets.py lv2/presets.ttl and build/presets.inc.
Edit the scripts, not the generated files.
Port and parameter order must match src/eka_grainflow.c."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_presets  # noqa: E402

URI = "http://dmtry3.github.io/zynthian-eka/grainflow"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STREAMS = 3

# Knob pages, in this order (higher displayPriority first).
GROUPS = [("g_main", "Main"), ("g_grain", "Grain"), ("g_flow", "Flow"), ("g_out", "Out")]

# (symbol, name, default, min, max, extra properties, group). Indices 0.. in this order.
CONTROLS = [
    ("stream", "Stream", 1, 1, 3, "lv2:portProperty lv2:integer , lv2:enumeration ;\n        "
     "lv2:scalePoint [ rdfs:label \"1\" ; rdf:value 1 ] , [ rdfs:label \"2\" ; rdf:value 2 ] , "
     "[ rdfs:label \"3\" ; rdf:value 3 ] ;", "g_main"),
    ("mix", "Mix %", 50, 0, 100, "", "g_main"),
    ("feedback", "Feedback %", 0, 0, 95, "", "g_main"),
    ("freeze", "Freeze", 0, 0, 1, "lv2:portProperty lv2:toggled ;", "g_main"),
    # the selected stream (values of stream 1 by default; the plugin keeps all three)
    ("offset", "Offset ms", 100, 1, 8000, "lv2:portProperty pprops:logarithmic ;", "g_grain"),
    ("rate", "Rate ms", 100, 5, 2000, "lv2:portProperty pprops:logarithmic ;", "g_grain"),
    ("length", "Length ms", 500, 10, 8000, "lv2:portProperty pprops:logarithmic ;", "g_grain"),
    ("pitch", "Pitch", 12, -24, 24, "lv2:portProperty lv2:integer ;", "g_grain"),
    ("reverse", "Reverse %", 0, 0, 100, "", "g_out"),
    ("filter", "Filter", 0, -100, 100, "", "g_out"),
    ("spray", "Spray %", 0, 0, 100, "", "g_out"),
    ("level", "Level dB", 0, -60, 6, "", "g_out"),
    # Flow ports follow the existing input controls; the group priorities place the page before Out.
    ("scan", "Scan %", 0, -200, 200, "", "g_flow"),
    ("fine", "Fine cents", 0, -50, 50, "", "g_flow"),
    ("interval", "Intervals", 0, 0, len(gen_presets.INTERVALS) - 1,
     "lv2:portProperty lv2:integer , lv2:enumeration ;\n        lv2:scalePoint " +
     " ,\n        ".join('[ rdfs:label "%s" ; rdf:value %d ]' % (name, i)
                           for i, name in enumerate(gen_presets.INTERVALS)) + " ;", "g_flow"),
    ("spread", "Spread %", 0, 0, 100, "", "g_flow"),
]

# The 8 values of a stream (same order as the Grain/Out knobs), then "on".
VALUES = [("offset", "Offset ms", 1, 8000), ("rate", "Rate ms", 5, 2000), ("length", "Length ms", 10, 8000),
          ("pitch", "Pitch", -24, 24), ("reverse", "Reverse %", 0, 100), ("filter", "Filter", -100, 100),
          ("spray", "Spray %", 0, 100), ("level", "Level dB", -60, 6)]
DEFAULTS = [[100, 100, 500, 12, 0, 0, 0, 0], [250, 120, 400, 0, 0, 0, -3], [400, 150, 600, -12, 0, 0, 0, -3]]
SPREAD_DEFAULTS = [0, 50, 50]

MOD_DSTS = ["pitch", "offset", "length", "level"]

OUT_MONITORS = [("peak", "Input peak", 0, 0, 4), ("active", "Active grains", 0, 0, 64),
                ("loads", "Loads", 0, 0, 1e9), ("min_offset", "Min Offset ms", 0, 0, 10000),
                ("sel", "Selected stream", 1, 1, 3), ("on_mask", "Streams on", 1, 0, 7)] + \
               [("cur_" + v[0], "Selected " + v[1], d, v[2], v[3]) for v, d in zip(VALUES, DEFAULTS[0])] + \
               [("lfo%d" % (l + 1), "LFO %d" % (l + 1), 0, -1, 1) for l in range(2)] + \
               [("env", "Env", 0, 0, 1), ("bpm", "Tempo", 120, 20, 400),
                ("rate_div", "Rate note value", -1, -1, 19), ("offset_div", "Offset note value", -1, -1, 19)]


def params():
    """(symbol, label, range, default, min, max) of the LV2 parameters."""
    out = []
    for s in range(STREAMS):
        for (sym, name, lo, hi), d in zip(VALUES, DEFAULTS[s]):
            out.append(("s%d_%s" % (s + 1, sym), "Stream %d %s" % (s + 1, name), "atom:Float", d, lo, hi))
        out.append(("s%d_on" % (s + 1), "Stream %d on" % (s + 1), "atom:Int", 1 if s == 0 else 0, 0, 1))
        out.append(("s%d_fine" % (s + 1), "Stream %d fine cents" % (s + 1), "atom:Float", 0, -50, 50))
        out.append(("s%d_spread" % (s + 1), "Stream %d spread %%" % (s + 1), "atom:Float", SPREAD_DEFAULTS[s], 0, 100))
        out.append(("s%d_interval" % (s + 1), "Stream %d intervals" % (s + 1), "atom:Int", 0, 0, len(gen_presets.INTERVALS) - 1))
    out.append(("buffer", "Buffer s", "atom:Int", 4, 1, 8))
    out.append(("shape", "Shape", "atom:Float", 0, 0, 1))
    out.append(("sync", "Sync", "atom:Int", 0, 0, 1))
    for l, (rate, shape) in enumerate(((0.3, 0), (1.0, 2))):
        out.append(("lfo%d_rate" % (l + 1), "LFO %d rate Hz" % (l + 1), "atom:Float", rate, 0.02, 20))
        out.append(("lfo%d_shape" % (l + 1), "LFO %d shape" % (l + 1), "atom:Int", shape, 0, 3))
    for l in range(3):
        for s in range(STREAMS):
            for d in MOD_DSTS:
                out.append(("m%d_s%d_%s" % (l + 1, s + 1, d), "%s to stream %d %s %%" % (("LFO 1", "LFO 2", "Env")[l], s + 1, d),
                            "atom:Int", 0, -100, 100))
    for l in range(2):
        out.append(("lfo%d_rate_mod" % (l + 1), "LFO %d to LFO %d rate %%" % (2 - l, l + 1),
                    "atom:Int", 0, -100, 100))
    out.append(("freeze_amount", "Freeze amount %", "atom:Float", 0, 0, 100))
    for s in range(STREAMS):
        out.append(("s%d_scan" % (s + 1), "Stream %d scan speed %%" % (s + 1), "atom:Float", 0, -200, 200))
    for l in range(2):
        out.append(("lfo%d_mode" % (l + 1), "LFO %d polarity" % (l + 1), "atom:Int", 0, 0, 1))
    out.extend([("env_attack", "Env Attack ms", "atom:Float", 0, 0, 1000),
                ("env_release", "Env Release ms", "atom:Float", 300, 10, 5000),
                ("env_sensitivity", "Env Sensitivity dB", "atom:Float", 0, -24, 24)])
    for l in range(2):
        out.append(("lfo%d_smooth" % (l + 1), "LFO %d Smooth %%" % (l + 1), "atom:Float", 0, 0, 100))
    for l in range(2):
        out.append(("lfo%d_sync" % (l + 1), "LFO %d Sync" % (l + 1), "atom:Int", 0, 0, 1))
    for l in range(2):
        out.append(("lfo%d_div" % (l + 1), "LFO %d note length" % (l + 1), "atom:Int", 12, 0, 19))
    for l in range(3):
        for s in range(STREAMS):
            for d in ("filter", "scan"):
                out.append(("m%d_s%d_%s" % (l + 1, s + 1, d), "Source %d to stream %d %s %%" % (l + 1, s + 1, d), "atom:Int", 0, -100, 100))
    return out


def main():
    lines = ["""# Generated by scripts/gen_ttl.py - edit the script, not this file.
@prefix lv2:    <http://lv2plug.in/ns/lv2core#> .
@prefix atom:   <http://lv2plug.in/ns/ext/atom#> .
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
        lines.append('<%s#%s> a pg:Group ; lv2:symbol "%s" ; lv2:name "%s" ; lv2:index %d ; lv2:displayPriority %d .'
                     % (URI, sym, sym, name, i, (len(GROUPS) - i) * 10))
    lines.append("")
    lines.append("# Stream values and settings without a knob: set in the widget, saved in the snapshot and in LV2 State.")
    for sym, label, rng, d, lo, hi in params():
        lines.append("""<%s#%s>
    a lv2:Parameter ;
    lv2:symbol "%s" ;
    rdfs:label "%s" ;
    rdfs:range %s ;
    lv2:default %s ; lv2:minimum %s ; lv2:maximum %s ;
    pprops:notOnGUI true ;
    lv2:portProperty pprops:notOnGUI .
""" % (URI, sym, sym, label, rng, d, lo, hi))
    writable = " ,\n        ".join("<%s#%s>" % (URI, p[0]) for p in params())
    lines.append("""<%s>
    a lv2:Plugin , lv2:DelayPlugin ;
    doap:name "Eka Grain Flow" ;
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
    for io, sym, name in (("Input", "in_l", "In L"), ("Input", "in_r", "In R"),
                          ("Output", "out_l", "Out L"), ("Output", "out_r", "Out R")):
        ports.append("""    [
        a lv2:%sPort , lv2:AudioPort ;
        lv2:index %d ; lv2:symbol "%s" ; lv2:name "%s" ;
    ]""" % (io, i, sym, name))
        i += 1
    ports.append("""    [
        a lv2:InputPort , atom:AtomPort ;
        atom:bufferType atom:Sequence ;
        atom:supports patch:Message , time:Position ;
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
    mons = OUT_MONITORS + [("grain%d" % (k + 1), "Grain %d" % (k + 1), -1, -1, 3) for k in range(16)]
    mons += [("freeze_phase", "Partial Freeze tape phase", -1, -1, 1),
             ("record_peak", "Recorded signal peak", 0, 0, 4)]
    mons += [("cur_scan", "Selected scan speed %", 0, -200, 200),
             ("cur_fine", "Selected fine cents", 0, -50, 50),
             ("cur_interval", "Selected intervals", 0, 0, len(gen_presets.INTERVALS) - 1),
             ("cur_spread", "Selected spread %", 0, 0, 100)]
    for sym, name, d, lo, hi in mons:
        ports.append("""    [
        a lv2:OutputPort , lv2:ControlPort ;
        lv2:index %d ; lv2:symbol "%s" ; lv2:name "%s" ;
        lv2:default %s ; lv2:minimum %s ; lv2:maximum %s ;
    ]""" % (i, sym, name, d, lo, hi if hi < 1e8 else "1000000000"))
        i += 1
    lines.append(" ,\n".join(ports) + " .")
    with open(os.path.join(ROOT, "lv2", "eka_grainflow.ttl"), "w") as f:
        f.write("\n".join(lines) + "\n")
    with open(os.path.join(ROOT, "lv2", "manifest.ttl"), "w") as f:
        f.write("""# Generated by scripts/gen_ttl.py - edit the script, not this file.
@prefix lv2:  <http://lv2plug.in/ns/lv2core#> .
@prefix pset: <http://lv2plug.in/ns/ext/presets#> .
@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .

<%s>
    a lv2:Plugin ;
    lv2:binary <eka_grainflow.so> ;
    rdfs:seeAlso <eka_grainflow.ttl> .

%s""" % (URI, gen_presets.manifest_entries()))
    gen_presets.write_presets_ttl()
    os.makedirs(os.path.join(ROOT, "build"), exist_ok=True)
    gen_presets.write_c(os.path.join(ROOT, "build", "presets.inc"))
    print("wrote lv2/eka_grainflow.ttl, lv2/manifest.ttl (%d ports)" % i)


if __name__ == "__main__":
    main()
