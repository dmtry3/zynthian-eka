#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate deterministic LV2 TTL files and factory presets for Eka Q-4."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LV2_DIR = ROOT / "lv2"
URI = "http://dmtry3.github.io/zynthian-eka/q4"

GROUPS = [
    ("g_b1",     "Band 1",      0, 60),
    ("g_b2",     "Band 2",      1, 50),
    ("g_b3",     "Band 3",      2, 40),
    ("g_b4",     "Band 4",      3, 30),
    ("g_cuts",   "Cuts & Tilt", 4, 20),
    ("g_master", "Master",      5, 10),
]

# Input control ports matching enum in src/eka_q4.c (indices 4..27)
CONTROL_PORTS = [
    # Page 1: Band 1 (Low)
    dict(idx=4,  sym="b1_freq",   name="B1 Freq",  grp="g_b1", default=100.0,   min=20.0,   max=1000.0,  unit="units:hz", props=["pprops:logarithmic"]),
    dict(idx=5,  sym="b1_gain",   name="B1 Gain",  grp="g_b1", default=0.0,     min=-18.0,  max=18.0,    unit="units:db"),
    dict(idx=6,  sym="b1_q",      name="B1 Q",     grp="g_b1", default=0.71,    min=0.2,    max=12.0,    props=["pprops:logarithmic"]),
    dict(idx=7,  sym="b1_type",   name="B1 Type",  grp="g_b1", default=0,       min=0,      max=3,
         props=["lv2:integer", "lv2:enumeration"],
         points=[("Shelf", 0), ("Bell", 1), ("Pultec", 2), ("Off", 3)]),

    # Page 2: Band 2 (Low-Mid)
    dict(idx=8,  sym="b2_freq",   name="B2 Freq",  grp="g_b2", default=400.0,   min=60.0,   max=4000.0,  unit="units:hz", props=["pprops:logarithmic"]),
    dict(idx=9,  sym="b2_gain",   name="B2 Gain",  grp="g_b2", default=0.0,     min=-18.0,  max=18.0,    unit="units:db"),
    dict(idx=10, sym="b2_q",      name="B2 Q",     grp="g_b2", default=1.0,     min=0.2,    max=16.0,    props=["pprops:logarithmic"]),
    dict(idx=11, sym="b2_type",   name="B2 Type",  grp="g_b2", default=0,       min=0,      max=2,
         props=["lv2:integer", "lv2:enumeration"],
         points=[("Bell", 0), ("Notch", 1), ("Off", 2)]),

    # Page 3: Band 3 (High-Mid)
    dict(idx=12, sym="b3_freq",   name="B3 Freq",  grp="g_b3", default=2200.0,  min=250.0,  max=12000.0, unit="units:hz", props=["pprops:logarithmic"]),
    dict(idx=13, sym="b3_gain",   name="B3 Gain",  grp="g_b3", default=0.0,     min=-18.0,  max=18.0,    unit="units:db"),
    dict(idx=14, sym="b3_q",      name="B3 Q",     grp="g_b3", default=1.0,     min=0.2,    max=16.0,    props=["pprops:logarithmic"]),
    dict(idx=15, sym="b3_type",   name="B3 Type",  grp="g_b3", default=0,       min=0,      max=2,
         props=["lv2:integer", "lv2:enumeration"],
         points=[("Bell", 0), ("Notch", 1), ("Off", 2)]),

    # Page 4: Band 4 (High)
    dict(idx=16, sym="b4_freq",   name="B4 Freq",  grp="g_b4", default=8000.0,  min=800.0,  max=20000.0, unit="units:hz", props=["pprops:logarithmic"]),
    dict(idx=17, sym="b4_gain",   name="B4 Gain",  grp="g_b4", default=0.0,     min=-18.0,  max=18.0,    unit="units:db"),
    dict(idx=18, sym="b4_q",      name="B4 Q",     grp="g_b4", default=0.71,    min=0.2,    max=12.0,    props=["pprops:logarithmic"]),
    dict(idx=19, sym="b4_type",   name="B4 Type",  grp="g_b4", default=0,       min=0,      max=3,
         props=["lv2:integer", "lv2:enumeration"],
         points=[("Shelf", 0), ("Bell", 1), ("Air", 2), ("Off", 3)]),

    # Page 5: Cuts & Tilt
    dict(idx=20, sym="hpf_freq",  name="HPF",      grp="g_cuts", default=20.0,    min=20.0,   max=600.0,   unit="units:hz", props=["pprops:logarithmic"]),
    dict(idx=21, sym="lpf_freq",  name="LPF",      grp="g_cuts", default=20000.0, min=2000.0, max=20000.0, unit="units:hz", props=["pprops:logarithmic"]),
    dict(idx=22, sym="tilt_db",   name="Tilt",     grp="g_cuts", default=0.0,     min=-6.0,   max=6.0,     unit="units:db"),
    dict(idx=23, sym="slope",     name="Slope",    grp="g_cuts", default=0,       min=0,      max=1,
         props=["lv2:integer", "lv2:enumeration"],
         points=[("12 dB", 0), ("24 dB", 1)]),

    # Page 6: Master
    dict(idx=24, sym="scale_pct", name="Scale",    grp="g_master", default=100.0, min=0.0,    max=200.0,   unit="units:pc"),
    dict(idx=25, sym="solo_band", name="Solo",     grp="g_master", default=0,     min=0,      max=4,
         props=["lv2:integer", "lv2:enumeration"],
         points=[("Off", 0), ("Band 1", 1), ("Band 2", 2), ("Band 3", 3), ("Band 4", 4)]),
    dict(idx=26, sym="auto_gain", name="AutoGain", grp="g_master", default=0,     min=0,      max=1,
         props=["lv2:integer", "lv2:toggled", "lv2:enumeration"],
         points=[("Off", 0), ("On", 1)]),
    dict(idx=27, sym="output_db", name="Output",   grp="g_master", default=0.0,   min=-18.0,  max=18.0,    unit="units:db"),
]

# Output telemetry ports (indices 28..53)
OUTPUT_PORTS = [
    (28, "in_peak",  "Input Peak",  -60.0, 12.0, "units:db"),
    (29, "out_peak", "Output Peak", -60.0, 12.0, "units:db"),
] + [
    (30 + i, f"spec_{i}", f"Spec {i + 1}", -60.0, 0.0, "units:db")
    for i in range(24)
]

DEFAULTS = {p["sym"]: p["default"] for p in CONTROL_PORTS}

PRESETS = [
    ("init", "01 Init", {}),
    ("vocal_clarity_air", "02 Vocal Clarity & Air", dict(
        hpf_freq=85.0, slope=1,
        b1_freq=140.0, b1_gain=-1.5, b1_q=0.8, b1_type=0,
        b2_freq=290.0, b2_gain=-3.0, b2_q=1.8, b2_type=0,
        b3_freq=3200.0, b3_gain=2.8, b3_q=1.2, b3_type=0,
        b4_freq=10500.0, b4_gain=3.5, b4_q=0.65, b4_type=2,
        auto_gain=1)),
    ("acoustic_guitar_polish", "03 Acoustic Guitar Polish", dict(
        hpf_freq=75.0, slope=0,
        b1_freq=110.0, b1_gain=1.5, b1_q=0.9, b1_type=1,
        b2_freq=220.0, b2_gain=-3.5, b2_q=2.2, b2_type=0,
        b3_freq=2600.0, b3_gain=2.0, b3_q=1.1, b3_type=0,
        b4_freq=9000.0, b4_gain=3.0, b4_q=0.7, b4_type=2,
        auto_gain=1)),
    ("bass_pultec_weight", "04 Bass Pultec Weight", dict(
        hpf_freq=32.0, slope=1,
        b1_freq=60.0, b1_gain=4.5, b1_q=0.75, b1_type=2,
        b2_freq=250.0, b2_gain=-2.5, b2_q=1.6, b2_type=0,
        b3_freq=1100.0, b3_gain=2.2, b3_q=1.3, b3_type=0,
        b4_freq=6000.0, b4_gain=-2.0, b4_q=0.7, b4_type=0,
        auto_gain=1)),
    ("warm_keys_rhodes", "05 Warm Keys & Rhodes", dict(
        hpf_freq=45.0, lpf_freq=11500.0, tilt_db=-1.2, slope=0,
        b1_freq=120.0, b1_gain=2.5, b1_q=0.7, b1_type=0,
        b2_freq=420.0, b2_gain=-1.5, b2_q=1.4, b2_type=0,
        b3_freq=1800.0, b3_gain=2.5, b3_q=1.0, b3_type=0,
        b4_freq=7500.0, b4_gain=1.0, b4_q=0.65, b4_type=2)),
    ("tape_loop_mud_clean", "06 Tape Loop Mud Clean", dict(
        hpf_freq=95.0, slope=1, tilt_db=0.8,
        b1_freq=150.0, b1_gain=-2.0, b1_q=0.8, b1_type=0,
        b2_freq=340.0, b2_gain=-3.2, b2_q=1.7, b2_type=0,
        b3_freq=2400.0, b3_gain=1.8, b3_q=1.1, b3_type=0,
        b4_freq=8500.0, b4_gain=2.0, b4_q=0.7, b4_type=0,
        auto_gain=1)),
    ("granular_cloud_sculpt", "07 Granular Cloud Sculpt", dict(
        hpf_freq=120.0, lpf_freq=13500.0, slope=0,
        b1_freq=180.0, b1_gain=-2.0, b1_q=0.9, b1_type=0,
        b2_freq=650.0, b2_gain=-2.5, b2_q=2.0, b2_type=0,
        b3_freq=3800.0, b3_gain=-3.0, b3_q=2.4, b3_type=0,
        b4_freq=11000.0, b4_gain=2.5, b4_q=0.6, b4_type=2)),
    ("west_coast_fold_tame", "08 West Coast Fold Tame", dict(
        hpf_freq=38.0, lpf_freq=14500.0, slope=0,
        b1_freq=80.0, b1_gain=3.0, b1_q=0.7, b1_type=2,
        b2_freq=520.0, b2_gain=-1.5, b2_q=1.5, b2_type=0,
        b3_freq=3400.0, b3_gain=-3.5, b3_q=2.2, b3_type=0,
        b4_freq=10000.0, b4_gain=1.5, b4_q=0.7, b4_type=2)),
    ("stage_anti_feedback", "09 Stage Anti-Feedback", dict(
        hpf_freq=100.0, slope=1,
        b1_freq=120.0, b1_gain=0.0, b1_q=0.71, b1_type=0,
        b2_freq=500.0, b2_gain=0.0, b2_q=8.0, b2_type=1,
        b3_freq=2500.0, b3_gain=0.0, b3_q=8.0, b3_type=1,
        b4_freq=9000.0, b4_gain=0.0, b4_q=0.71, b4_type=0)),
    ("master_smile_air", "10 Master Smile & Air", dict(
        hpf_freq=28.0, slope=1,
        b1_freq=75.0, b1_gain=2.0, b1_q=0.65, b1_type=2,
        b2_freq=380.0, b2_gain=-1.2, b2_q=1.2, b2_type=0,
        b3_freq=2800.0, b3_gain=1.0, b3_q=1.0, b3_type=0,
        b4_freq=12000.0, b4_gain=2.2, b4_q=0.6, b4_type=2,
        auto_gain=1)),
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
        "    lv2:binary <eka_q4.so> ;",
        "    rdfs:seeAlso <eka_q4.ttl> .",
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
        "    a lv2:Plugin, lv2:ParaEQPlugin ;",
        '    doap:name "Eka Q-4" ;',
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
    (LV2_DIR / "eka_q4.ttl").write_text(gen_plugin_ttl(), encoding="utf-8")
    (LV2_DIR / "presets.ttl").write_text(gen_presets_ttl(), encoding="utf-8")
    print(f"Generated TTL + {len(PRESETS)} presets in {LV2_DIR}")


if __name__ == "__main__":
    main()
