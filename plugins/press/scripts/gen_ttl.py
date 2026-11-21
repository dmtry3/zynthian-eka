#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate deterministic LV2 TTL files and factory presets for Eka Press."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LV2_DIR = ROOT / "lv2"
URI = "http://dmtry3.github.io/zynthian-eka/press"

GROUPS = [
    ("g_main",  "Main",      0, 50),
    ("g_env",   "Envelope",  1, 40),
    ("g_sc",    "Sidechain", 2, 30),
    ("g_color", "Color",     3, 20),
]

# Input control ports matching enum in src/eka_press.c (indices 4..19)
CONTROL_PORTS = [
    # Page 1: Main
    dict(idx=4,  sym="threshold",    name="Thresh",   grp="g_main",  default=-18.0, min=-60.0, max=0.0,    unit="units:db"),
    dict(idx=5,  sym="ratio",        name="Ratio",    grp="g_main",  default=4.0,   min=1.0,   max=20.0,   props=["pprops:logarithmic"]),
    dict(idx=6,  sym="makeup",       name="Makeup",   grp="g_main",  default=0.0,   min=-12.0, max=24.0,   unit="units:db"),
    dict(idx=7,  sym="mix",          name="Mix",      grp="g_main",  default=100.0, min=0.0,   max=100.0,  unit="units:pc"),

    # Page 2: Envelope
    dict(idx=8,  sym="attack",       name="Attack",   grp="g_env",   default=10.0,  min=0.1,   max=100.0,  unit="units:ms", props=["pprops:logarithmic"]),
    dict(idx=9,  sym="release",      name="Release",  grp="g_env",   default=150.0, min=10.0,  max=1500.0, unit="units:ms", props=["pprops:logarithmic"]),
    dict(idx=10, sym="knee",         name="Knee",     grp="g_env",   default=6.0,   min=0.0,   max=18.0,   unit="units:db"),
    dict(idx=11, sym="mode",         name="Mode",     grp="g_env",   default=0,     min=0,     max=3,
         props=["lv2:integer", "lv2:enumeration"],
         points=[("Clean", 0), ("Opto", 1), ("Punch", 2), ("Glue", 3)]),

    # Page 3: Sidechain
    dict(idx=12, sym="sc_hpf",       name="SC HPF",   grp="g_sc",    default=20.0,  min=20.0,  max=500.0,  unit="units:hz", props=["pprops:logarithmic"]),
    dict(idx=13, sym="stereo_link",  name="Link",     grp="g_sc",    default=100.0, min=0.0,   max=100.0,  unit="units:pc"),
    dict(idx=14, sym="auto_release", name="Auto Rel", grp="g_sc",    default=0,     min=0,     max=1,
         props=["lv2:integer", "lv2:toggled", "lv2:enumeration"],
         points=[("Off", 0), ("On", 1)]),
    dict(idx=15, sym="sc_listen",    name="SC Lstn",  grp="g_sc",    default=0,     min=0,     max=1,
         props=["lv2:integer", "lv2:toggled", "lv2:enumeration"],
         points=[("Off", 0), ("On", 1)]),

    # Page 4: Color
    dict(idx=16, sym="drive",        name="Drive",    grp="g_color", default=0.0,   min=0.0,   max=100.0,  unit="units:pc"),
    dict(idx=17, sym="auto_makeup",  name="AutoGain", grp="g_color", default=0,     min=0,     max=1,
         props=["lv2:integer", "lv2:toggled", "lv2:enumeration"],
         points=[("Off", 0), ("On", 1)]),
    dict(idx=18, sym="soft_clip",    name="SoftClip", grp="g_color", default=1,     min=0,     max=1,
         props=["lv2:integer", "lv2:toggled", "lv2:enumeration"],
         points=[("Off", 0), ("On", 1)]),
    dict(idx=19, sym="output",       name="Output",   grp="g_color", default=0.0,   min=-24.0, max=12.0,   unit="units:db"),
]

OUTPUT_PORTS = [
    (20, "in_peak",  "Input Peak",  -60.0, 12.0, "units:db"),
    (21, "out_peak", "Output Peak", -60.0, 12.0, "units:db"),
    (22, "sc_peak",  "SC Peak",     -60.0, 12.0, "units:db"),
    (23, "gr_db",    "Gain Red",      0.0, 36.0, "units:db"),
]

DEFAULTS = {p["sym"]: p["default"] for p in CONTROL_PORTS}

PRESETS = [
    ("init", "01 Init", {}),
    ("guitar_mic_leveler", "02 Guitar & Mic Leveler", dict(
        mode=1, threshold=-20.0, ratio=3.0, attack=12.0, release=220.0,
        knee=8.0, sc_hpf=75.0, auto_release=1, auto_makeup=1, drive=10.0, soft_clip=1)),
    ("acoustic_sparkle", "03 Acoustic Sparkle", dict(
        mode=0, threshold=-16.0, ratio=3.5, attack=18.0, release=120.0,
        knee=6.0, sc_hpf=120.0, makeup=2.0, mix=85.0, soft_clip=1)),
    ("opto_vocal_flute", "04 Opto Vocal & Flute", dict(
        mode=1, threshold=-22.0, ratio=3.5, attack=15.0, release=280.0,
        knee=10.0, sc_hpf=100.0, auto_release=1, auto_makeup=1, drive=15.0, soft_clip=1)),
    ("synth_punch", "05 Synth Punch", dict(
        mode=2, threshold=-18.0, ratio=6.0, attack=4.0, release=90.0,
        knee=3.0, sc_hpf=60.0, makeup=3.5, drive=25.0, soft_clip=1)),
    ("bass_lock", "06 Bass Lock", dict(
        mode=2, threshold=-20.0, ratio=5.0, attack=8.0, release=110.0,
        knee=4.0, sc_hpf=80.0, auto_makeup=1, drive=20.0, soft_clip=1)),
    ("tape_bus_glue", "07 Tape Bus Glue", dict(
        mode=3, threshold=-14.0, ratio=2.2, attack=25.0, release=180.0,
        knee=6.0, sc_hpf=90.0, auto_release=1, makeup=1.5, drive=15.0, soft_clip=1)),
    ("ny_parallel_crush", "08 NY Parallel Crush", dict(
        mode=2, threshold=-30.0, ratio=12.0, attack=1.5, release=80.0,
        knee=2.0, sc_hpf=85.0, makeup=8.0, drive=40.0, mix=45.0, soft_clip=1)),
    ("warm_analog_squeeze", "09 Warm Analog Squeeze", dict(
        mode=1, threshold=-24.0, ratio=4.5, attack=10.0, release=200.0,
        knee=12.0, sc_hpf=65.0, auto_makeup=1, drive=55.0, soft_clip=1)),
    ("brickwall_limiter", "10 Brickwall Limiter", dict(
        mode=0, threshold=-4.0, ratio=20.0, attack=0.2, release=70.0,
        knee=0.0, sc_hpf=20.0, makeup=2.0, soft_clip=1)),
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
        "    lv2:binary <eka_press.so> ;",
        "    rdfs:seeAlso <eka_press.ttl> .",
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
        "    a lv2:Plugin, lv2:CompressorPlugin ;",
        '    doap:name "Eka Press" ;',
        "    doap:license <https://spdx.org/licenses/GPL-3.0-or-later.html> ;",
        "    lv2:minorVersion 0 ;",
        "    lv2:microVersion 1 ;",
        "    lv2:optionalFeature lv2:hardRTCapable ;",
        "    lv2:port",
        '    [ a lv2:InputPort,  lv2:AudioPort ; lv2:index 0 ; lv2:symbol "in_l"  ; lv2:name "Input Left" ] ,',
        '    [ a lv2:InputPort,  lv2:AudioPort ; lv2:index 1 ; lv2:symbol "in_r"  ; lv2:name "Input Right" ] ,',
        '    [ a lv2:OutputPort, lv2:AudioPort ; lv2:index 2 ; lv2:symbol "out_l" ; lv2:name "Output Left" ] ,',
        '    [ a lv2:OutputPort, lv2:AudioPort ; lv2:index 3 ; lv2:symbol "out_r" ; lv2:name "Output Right" ] ,',
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
    (LV2_DIR / "eka_press.ttl").write_text(gen_plugin_ttl(), encoding="utf-8")
    (LV2_DIR / "presets.ttl").write_text(gen_presets_ttl(), encoding="utf-8")
    print(f"Generated TTL + {len(PRESETS)} presets in {LV2_DIR}")


if __name__ == "__main__":
    main()
