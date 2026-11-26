#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate deterministic LV2 TTL files and factory presets for Eka Space."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LV2_DIR = ROOT / "lv2"
URI = "http://dmtry3.github.io/zynthian-eka/space"

GROUPS = [
    ("g_stage",  "Stage",      0, 60),
    ("g_widen",  "Widener",    1, 50),
    ("g_motion", "Motion",     2, 40),
    ("g_focus",  "Bass & Air", 3, 30),
    ("g_fx",     "Live FX",    4, 20),
    ("g_master", "Master",     5, 10),
]

# Input control ports matching enum in src/eka_space.c (indices 5..29)
CONTROL_PORTS = [
    # Page 1: Stage (Position & Depth)
    dict(idx=5,  sym="pan",          name="Pan",        grp="g_stage",  default=0.0,   min=-100.0, max=100.0,  unit="units:pc"),
    dict(idx=6,  sym="width",        name="Width",      grp="g_stage",  default=100.0, min=0.0,    max=300.0,  unit="units:pc"),
    dict(idx=7,  sym="depth",        name="Depth",      grp="g_stage",  default=0.0,   min=0.0,    max=100.0,  unit="units:pc"),
    dict(idx=8,  sym="tilt",         name="Tilt",       grp="g_stage",  default=0.0,   min=-100.0, max=100.0,  unit="units:pc"),

    # Page 2: Widener (5 Stereo Expansion Engines)
    dict(idx=9,  sym="mode",         name="Mode",       grp="g_widen",  default=1,     min=0,      max=4,
         props=["lv2:integer", "lv2:enumeration"],
         points=[("M/S", 0), ("Comb", 1), ("Haas", 2), ("Dimension", 3), ("Vortex", 4)]),
    dict(idx=10, sym="widen",        name="Widen",      grp="g_widen",  default=30.0,  min=0.0,    max=100.0,  unit="units:pc"),
    dict(idx=11, sym="spread_ms",    name="Spread",     grp="g_widen",  default=14.0,  min=0.5,    max=40.0,   unit="units:ms", props=["pprops:logarithmic"]),
    dict(idx=12, sym="room_pct",     name="Room",       grp="g_widen",  default=15.0,  min=0.0,    max=100.0,  unit="units:pc"),

    # Page 3: Motion (Tempo-Synced Spatial LFO & Dynamic Envelope)
    dict(idx=13, sym="mod_shape",    name="Mod Shape",  grp="g_motion", default=0,     min=0,      max=4,
         props=["lv2:integer", "lv2:enumeration"],
         points=[("Pan", 0), ("Breathe", 1), ("Orbit", 2), ("Figure-8", 3), ("Drift", 4)]),
    dict(idx=14, sym="mod_rate",     name="Mod Rate",   grp="g_motion", default=0.5,   min=0.05,   max=12.0,   unit="units:hz", props=["pprops:logarithmic"]),
    dict(idx=15, sym="mod_depth",    name="Mod Depth",  grp="g_motion", default=0.0,   min=0.0,    max=100.0,  unit="units:pc"),
    dict(idx=16, sym="sync",         name="Sync",       grp="g_motion", default=1,     min=0,      max=1,
         props=["lv2:integer", "lv2:toggled", "lv2:enumeration"],
         points=[("Free", 0), ("Sync", 1)]),

    # Page 4: Bass & Air (Frequency Anchor, Side Shine & Dynamic Width)
    dict(idx=17, sym="mono_low",     name="Bass Mono",  grp="g_focus",  default=120.0, min=20.0,   max=500.0,  unit="units:hz", props=["pprops:logarithmic"]),
    dict(idx=18, sym="side_hpf",     name="Side HPF",   grp="g_focus",  default=90.0,  min=20.0,   max=1000.0, unit="units:hz", props=["pprops:logarithmic"]),
    dict(idx=19, sym="side_air",     name="Side Air",   grp="g_focus",  default=1.5,   min=-12.0,  max=12.0,   unit="units:db"),
    dict(idx=20, sym="env_width",    name="Env Width",  grp="g_focus",  default=0.0,   min=-100.0, max=100.0,  unit="units:pc"),

    # Page 5: Live FX (X-Y Performance Macros)
    dict(idx=21, sym="pan_fx",       name="Pan FX",     grp="g_fx",     default=0.0,   min=-100.0, max=100.0,  unit="units:pc"),
    dict(idx=22, sym="width_fx",     name="Width FX",   grp="g_fx",     default=0.0,   min=-100.0, max=100.0,  unit="units:pc"),
    dict(idx=23, sym="spin_fx",      name="Spin FX",    grp="g_fx",     default=0.0,   min=0.0,    max=100.0,  unit="units:pc"),
    dict(idx=24, sym="fx_zero",      name="FX Zero",    grp="g_fx",     default=0,     min=0,      max=1,
         props=["lv2:integer", "lv2:toggled", "lv2:enumeration"],
         points=[("Off", 0), ("Zero", 1)]),

    # Page 6: Master (Audition, Phase Guard & Output)
    dict(idx=25, sym="audition",     name="Audition",   grp="g_master", default=0,     min=0,      max=3,
         props=["lv2:integer", "lv2:enumeration"],
         points=[("Stereo", 0), ("Mono", 1), ("Mid", 2), ("Side", 3)]),
    dict(idx=26, sym="safe_phase",   name="SafePhase",  grp="g_master", default=0,     min=0,      max=1,
         props=["lv2:integer", "lv2:toggled", "lv2:enumeration"],
         points=[("Off", 0), ("On", 1)]),
    dict(idx=27, sym="mix",          name="Mix",        grp="g_master", default=100.0, min=0.0,    max=100.0,  unit="units:pc"),
    dict(idx=28, sym="output_db",    name="Output",     grp="g_master", default=0.0,   min=-18.0,  max=12.0,   unit="units:db"),
    dict(idx=29, sym="bypass",       name="Bypass",     grp="g_master", default=0,     min=0,      max=1,
         props=["lv2:integer", "lv2:toggled", "lv2:enumeration"],
         points=[("Off", 0), ("On", 1)]),
]

# Output telemetry ports (indices 30..39)
OUTPUT_PORTS = [
    (30, "in_peak",    "Input Peak",   -60.0, 12.0,  "units:db"),
    (31, "out_peak",   "Output Peak",  -60.0, 12.0,  "units:db"),
    (32, "mid_db",     "Mid Level",    -60.0, 12.0,  "units:db"),
    (33, "side_db",    "Side Level",   -60.0, 12.0,  "units:db"),
    (34, "corr",       "Correlation",   -1.0,  1.0,  "units:coef"),
    (35, "balance",    "Balance",       -1.0,  1.0,  "units:coef"),
    (36, "eff_pan",    "Effective Pan",-100.0, 100.0,"units:pc"),
    (37, "eff_width",  "Effective Wid",  0.0, 300.0, "units:pc"),
    (38, "eff_depth",  "Effective Dep",  0.0, 100.0, "units:pc"),
    (39, "host_bpm",   "Host BPM",      20.0, 400.0, "units:bpm"),
]

DEFAULTS = {p["sym"]: p["default"] for p in CONTROL_PORTS}

PRESETS = [
    ("init_stage", "01 Init Natural Stage", dict(
        pan=0.0, width=105.0, depth=10.0, tilt=0.0,
        mode=1, widen=22.0, spread_ms=11.0, room_pct=12.0,
        mono_low=120.0, side_hpf=90.0, side_air=1.5, safe_phase=1)),
    ("mono_to_stereo_comb", "02 Mono-to-Stereo Comb Velvet", dict(
        pan=0.0, width=130.0, depth=12.0, tilt=8.0,
        mode=1, widen=68.0, spread_ms=16.0, room_pct=20.0,
        mono_low=140.0, side_hpf=110.0, side_air=2.5, safe_phase=1)),
    ("analog_dimension_d", "03 Analog Dimension Chorus", dict(
        pan=0.0, width=135.0, depth=15.0, tilt=0.0,
        mode=3, widen=65.0, spread_ms=9.5, room_pct=22.0,
        mod_shape=1, mod_rate=0.45, mod_depth=18.0, sync=0,
        mono_low=130.0, side_hpf=100.0, side_air=2.0)),
    ("psychoacoustic_haas", "04 Psychoacoustic Haas Wall", dict(
        pan=-8.0, width=145.0, depth=8.0, tilt=15.0,
        mode=2, widen=75.0, spread_ms=18.5, room_pct=15.0,
        mono_low=160.0, side_hpf=140.0, side_air=3.0)),
    ("binaural_3d_orbit", "05 Binaural 3D Orbit", dict(
        pan=0.0, width=140.0, depth=28.0, tilt=0.0,
        mode=1, widen=45.0, spread_ms=14.0, room_pct=35.0,
        mod_shape=2, mod_rate=0.5, mod_depth=62.0, sync=1,
        mono_low=110.0, side_hpf=95.0, side_air=2.0)),
    ("figure8_auto_pan", "06 Figure-8 Synced Swirl", dict(
        pan=0.0, width=150.0, depth=25.0, tilt=12.0,
        mode=3, widen=55.0, spread_ms=12.0, room_pct=28.0,
        mod_shape=3, mod_rate=1.0, mod_depth=72.0, sync=1,
        mono_low=125.0, side_hpf=105.0, side_air=2.5)),
    ("dynamic_transient_bloom", "07 Dynamic Transient Bloom", dict(
        pan=0.0, width=110.0, depth=14.0, tilt=0.0,
        mode=1, widen=50.0, spread_ms=15.0, room_pct=24.0,
        mono_low=150.0, side_hpf=120.0, side_air=3.5, env_width=75.0, safe_phase=1)),
    ("front_center_back_wings", "08 Tight Center & Distant Wings", dict(
        pan=0.0, width=165.0, depth=35.0, tilt=-10.0,
        mode=3, widen=60.0, spread_ms=22.0, room_pct=48.0,
        mod_shape=4, mod_rate=0.25, mod_depth=30.0, sync=0,
        mono_low=180.0, side_hpf=160.0, side_air=3.0, env_width=-55.0)),
    ("extreme_vortex_cloud", "09 Extreme Vortex Hyper-Wide", dict(
        pan=0.0, width=240.0, depth=42.0, tilt=20.0,
        mode=4, widen=92.0, spread_ms=28.0, room_pct=65.0,
        mod_shape=2, mod_rate=0.35, mod_depth=55.0, sync=0,
        mono_low=135.0, side_hpf=110.0, side_air=4.5, safe_phase=0)),
    ("master_bus_velvet_air", "10 Master Bus Velvet & Bass Lock", dict(
        pan=0.0, width=118.0, depth=4.0, tilt=0.0,
        mode=0, widen=20.0, spread_ms=10.0, room_pct=8.0,
        mono_low=140.0, side_hpf=110.0, side_air=2.2, safe_phase=1)),
]


def fmt_num(v):
    if isinstance(v, int):
        return str(v)
    return format(float(v), ".8g")


def gen_manifest():
    lines = [
        "# Generated by scripts/gen_ttl.py - edit the script, not this file.",
        "@prefix lv2:  <http://lv2plug.in/ns/lv2core#> .",
        "@prefix pset: <http://lv2plug.in/ns/ext/presets#> .",
        "@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .",
        "",
        f"<{URI}>",
        "    a lv2:Plugin ;",
        "    lv2:binary <eka_space.so> ;",
        "    rdfs:seeAlso <eka_space.ttl> .",
        "",
        f"<{URI}#bank_factory>",
        "    a pset:Bank ;",
        f"    lv2:appliesTo <{URI}> ;",
        '    rdfs:label "Factory" .',
        "",
    ]
    for slug, title, _ in PRESETS:
        lines += [
            f"<{URI}#preset_{slug}>",
            "    a pset:Preset ;",
            f"    lv2:appliesTo <{URI}> ;",
            f'    rdfs:label "{title}" ;',
            "    rdfs:seeAlso <presets.ttl> .",
            "",
        ]
    return "\n".join(lines)


def gen_plugin_ttl():
    lines = [
        "# Generated by scripts/gen_ttl.py - edit the script, not this file.",
        "@prefix lv2:    <http://lv2plug.in/ns/lv2core#> .",
        "@prefix doap:   <http://usefulinc.com/ns/doap#> .",
        "@prefix atom:   <http://lv2plug.in/ns/ext/atom#> .",
        "@prefix time:   <http://lv2plug.in/ns/ext/time#> .",
        "@prefix urid:   <http://lv2plug.in/ns/ext/urid#> .",
        "@prefix resize: <http://lv2plug.in/ns/ext/resize-port#> .",
        "@prefix pprops: <http://lv2plug.in/ns/ext/port-props#> .",
        "@prefix pg:     <http://lv2plug.in/ns/ext/port-groups#> .",
        "@prefix units:  <http://lv2plug.in/ns/extensions/units#> .",
        "@prefix rdf:    <http://www.w3.org/1999/02/22-rdf-syntax-ns#> .",
        "@prefix rdfs:   <http://www.w3.org/2000/01/rdf-schema#> .",
        "",
    ]
    for sym, name, idx, prio in GROUPS:
        lines.append(
            f'<{URI}#{sym}> a pg:Group ; lv2:symbol "{sym}" ; lv2:name "{name}" ; '
            f"lv2:index {idx} ; lv2:displayPriority {prio} ."
        )
    lines += [
        "",
        f"<{URI}>",
        "    a lv2:Plugin, lv2:SpatialPlugin ;",
        '    doap:name "Eka Space" ;',
        "    doap:license <https://spdx.org/licenses/GPL-3.0-or-later.html> ;",
        "    lv2:minorVersion 1 ;",
        "    lv2:microVersion 0 ;",
        "    lv2:optionalFeature lv2:hardRTCapable, urid:map ;",
        "    lv2:port",
        '    [ a lv2:InputPort,  lv2:AudioPort ; lv2:index 0 ; lv2:symbol "in_l"  ; lv2:name "Input Left" ] ,',
        '    [ a lv2:InputPort,  lv2:AudioPort ; lv2:index 1 ; lv2:symbol "in_r"  ; lv2:name "Input Right" ] ,',
        '    [ a lv2:OutputPort, lv2:AudioPort ; lv2:index 2 ; lv2:symbol "out_l" ; lv2:name "Output Left" ] ,',
        '    [ a lv2:OutputPort, lv2:AudioPort ; lv2:index 3 ; lv2:symbol "out_r" ; lv2:name "Output Right" ] ,',
        '    [ a lv2:InputPort, atom:AtomPort ; lv2:index 4 ; lv2:symbol "control" ; lv2:name "Time" ; '
        'lv2:designation lv2:control ; atom:bufferType atom:Sequence ; atom:supports time:Position ; '
        'lv2:portProperty lv2:connectionOptional ; resize:minimumSize 4096 ] ,',
    ]

    for p in CONTROL_PORTS:
        parts = [
            f'a lv2:InputPort, lv2:ControlPort ; lv2:index {p["idx"]} ; '
            f'lv2:symbol "{p["sym"]}" ; lv2:name "{p["name"]}"',
            f'pg:group <{URI}#{p["grp"]}>',
            f'lv2:default {fmt_num(p["default"])} ; lv2:minimum {fmt_num(p["min"])} ; lv2:maximum {fmt_num(p["max"])}',
        ]
        if "unit" in p:
            parts.append(f'units:unit {p["unit"]}')
        if "props" in p:
            parts.append("lv2:portProperty " + ", ".join(p["props"]))
        if "points" in p:
            pts = ", ".join(f'[ rdfs:label "{lbl}" ; rdf:value {val} ]' for lbl, val in p["points"])
            parts.append("lv2:scalePoint " + pts)
        lines.append("    [ " + " ; ".join(parts) + " ] ,")

    for i, (idx, sym, name, mn, mx, unit) in enumerate(OUTPUT_PORTS):
        end = " ." if i == len(OUTPUT_PORTS) - 1 else " ,"
        lines.append(
            f'    [ a lv2:OutputPort, lv2:ControlPort ; lv2:index {idx} ; '
            f'lv2:symbol "{sym}" ; lv2:name "{name}" ; '
            f"lv2:minimum {fmt_num(mn)} ; lv2:maximum {fmt_num(mx)} ; units:unit {unit} ]{end}"
        )
    lines.append("")
    return "\n".join(lines)


def gen_presets_ttl():
    lines = [
        "# Generated by scripts/gen_ttl.py - edit the script, not this file.",
        "@prefix lv2:  <http://lv2plug.in/ns/lv2core#> .",
        "@prefix pset: <http://lv2plug.in/ns/ext/presets#> .",
        "@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .",
        "",
    ]
    for slug, title, overrides in PRESETS:
        values = dict(DEFAULTS)
        unknown = set(overrides) - set(DEFAULTS)
        assert not unknown, f"Unknown preset keys in {slug}: {unknown}"
        values.update(overrides)
        lines += [
            f"<{URI}#preset_{slug}>",
            "    a pset:Preset ;",
            f"    lv2:appliesTo <{URI}> ;",
            f"    pset:bank <{URI}#bank_factory> ;",
            f'    rdfs:label "{title}" ;',
            "    lv2:port",
        ]
        items = list(values.items())
        for i, (sym, val) in enumerate(items):
            end = " ." if i == len(items) - 1 else " ,"
            lines.append(f'        [ lv2:symbol "{sym}" ; pset:value {fmt_num(val)} ]{end}')
        lines.append("")
    return "\n".join(lines)


def main():
    LV2_DIR.mkdir(parents=True, exist_ok=True)
    (LV2_DIR / "manifest.ttl").write_text(gen_manifest(), encoding="utf-8")
    (LV2_DIR / "eka_space.ttl").write_text(gen_plugin_ttl(), encoding="utf-8")
    (LV2_DIR / "presets.ttl").write_text(gen_presets_ttl(), encoding="utf-8")
    print(f"Generated TTL + {len(PRESETS)} presets in {LV2_DIR}")


if __name__ == "__main__":
    main()
