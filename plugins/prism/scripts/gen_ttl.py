#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate deterministic LV2 TTL files and factory presets for Eka Prism."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LV2_DIR = ROOT / "lv2"
URI = "http://dmtry3.github.io/zynthian-eka/prism"

GROUPS = [
    ("g_flt_a",  "Filter A", 0, 60),
    ("g_flt_b",  "Filter B", 1, 50),
    ("g_route",  "Routing",  2, 40),
    ("g_lfo",    "LFO",      3, 30),
    ("g_mod",    "Mod Depth",4, 20),
    ("g_master", "Master",   5, 10),
]

FILTER_MODELS = [
    ("SVF Morph", 0),
    ("Ladder 24", 1),
    ("MS-20 Scream", 2),
    ("Comb Res", 3),
    ("Formant", 4),
    ("Phaser 6", 5),
]

# Input control ports matching enum in src/eka_prism.c (indices 5..28)
CONTROL_PORTS = [
    # Page 1: Filter A
    dict(idx=5,  sym="model_a",     name="Model A",    grp="g_flt_a",  default=0,       min=0,      max=5,
         props=["lv2:integer", "lv2:enumeration"], points=FILTER_MODELS),
    dict(idx=6,  sym="cutoff_a",    name="Cutoff A",   grp="g_flt_a",  default=650.0,   min=20.0,   max=18000.0, unit="units:hz", props=["pprops:logarithmic"]),
    dict(idx=7,  sym="res_a",       name="Res A",      grp="g_flt_a",  default=35.0,    min=0.0,    max=100.0,   unit="units:pc"),
    dict(idx=8,  sym="morph_a",     name="Morph A",    grp="g_flt_a",  default=0.0,     min=0.0,    max=100.0,   unit="units:pc"),

    # Page 2: Filter B
    dict(idx=9,  sym="model_b",     name="Model B",    grp="g_flt_b",  default=1,       min=0,      max=5,
         props=["lv2:integer", "lv2:enumeration"], points=FILTER_MODELS),
    dict(idx=10, sym="cutoff_b",    name="Cutoff B",   grp="g_flt_b",  default=1800.0,  min=20.0,   max=18000.0, unit="units:hz", props=["pprops:logarithmic"]),
    dict(idx=11, sym="res_b",       name="Res B",      grp="g_flt_b",  default=40.0,    min=0.0,    max=100.0,   unit="units:pc"),
    dict(idx=12, sym="morph_b",     name="Morph B",    grp="g_flt_b",  default=25.0,    min=0.0,    max=100.0,   unit="units:pc"),

    # Page 3: Routing
    dict(idx=13, sym="routing",     name="Routing",    grp="g_route",  default=0,       min=0,      max=2,
         props=["lv2:integer", "lv2:enumeration"],
         points=[("Serial A>B", 0), ("Parallel A+B", 1), ("Stereo L/R", 2)]),
    dict(idx=14, sym="link_mode",   name="Link B",     grp="g_route",  default=0,       min=0,      max=2,
         props=["lv2:integer", "lv2:enumeration"],
         points=[("Free", 0), ("Offset", 1), ("Mirror", 2)]),
    dict(idx=15, sym="ab_balance",  name="Balance",    grp="g_route",  default=0.0,     min=-100.0, max=100.0,   unit="units:pc"),
    dict(idx=16, sym="invert_b",    name="Phase B",    grp="g_route",  default=0,       min=0,      max=1,
         props=["lv2:integer", "lv2:toggled", "lv2:enumeration"],
         points=[("Norm", 0), ("Invert", 1)]),

    # Page 4: LFO
    dict(idx=17, sym="lfo_shape",   name="Shape",      grp="g_lfo",    default=0,       min=0,      max=6,
         props=["lv2:integer", "lv2:enumeration"],
         points=[("Sine", 0), ("Tri", 1), ("Saw", 2), ("Square", 3), ("S&H", 4), ("Chaos", 5), ("Step 8", 6)]),
    dict(idx=18, sym="lfo_rate",    name="Rate",       grp="g_lfo",    default=0.80,    min=0.05,   max=40.0,    unit="units:hz", props=["pprops:logarithmic"]),
    dict(idx=19, sym="lfo_sync",    name="Sync",       grp="g_lfo",    default=0,       min=0,      max=1,
         props=["lv2:integer", "lv2:toggled", "lv2:enumeration"],
         points=[("Free", 0), ("Sync", 1)]),
    dict(idx=20, sym="lfo_phase",   name="B Phase",    grp="g_lfo",    default=90.0,    min=0.0,    max=180.0,   unit="units:degree"),

    # Page 5: Mod Depth
    dict(idx=21, sym="lfo_mod",     name="LFO > Cut",  grp="g_mod",    default=25.0,    min=-100.0, max=100.0,   unit="units:pc"),
    dict(idx=22, sym="morph_mod",   name="Mod > Morph",grp="g_mod",    default=0.0,     min=-100.0, max=100.0,   unit="units:pc"),
    dict(idx=23, sym="env_mod",     name="Env > Cut",  grp="g_mod",    default=0.0,     min=-100.0, max=100.0,   unit="units:pc"),
    dict(idx=24, sym="env_speed",   name="Env Speed",  grp="g_mod",    default=50.0,    min=0.0,    max=100.0,   unit="units:pc"),

    # Page 6: Master
    dict(idx=25, sym="drive",       name="Drive",      grp="g_master", default=15.0,    min=0.0,    max=100.0,   unit="units:pc"),
    dict(idx=26, sym="mix",         name="Mix",        grp="g_master", default=100.0,   min=0.0,    max=100.0,   unit="units:pc"),
    dict(idx=27, sym="output_db",   name="Output",     grp="g_master", default=0.0,     min=-18.0,  max=12.0,    unit="units:db"),
    dict(idx=28, sym="bypass",      name="Bypass",     grp="g_master", default=0,       min=0,      max=1,
         props=["lv2:integer", "lv2:toggled", "lv2:enumeration"],
         points=[("Off", 0), ("On", 1)]),
]

# Output telemetry ports (indices 29..38)
OUTPUT_PORTS = [
    (29, "in_peak",     "Input Peak",   -60.0, 12.0,    "units:db"),
    (30, "out_peak",    "Output Peak",  -60.0, 12.0,    "units:db"),
    (31, "eff_cut_a",   "Eff Cutoff A",  20.0, 18000.0, "units:hz"),
    (32, "eff_cut_b",   "Eff Cutoff B",  20.0, 18000.0, "units:hz"),
    (33, "eff_morph_a", "Eff Morph A",    0.0, 100.0,   "units:pc"),
    (34, "eff_morph_b", "Eff Morph B",    0.0, 100.0,   "units:pc"),
    (35, "lfo_a",       "LFO A",         -1.0, 1.0,     "units:coef"),
    (36, "lfo_b",       "LFO B",         -1.0, 1.0,     "units:coef"),
    (37, "env_lvl",     "Env Level",      0.0, 1.0,     "units:coef"),
    (38, "host_bpm",    "Host BPM",      20.0, 400.0,   "units:bpm"),
]

DEFAULTS = {p["sym"]: p["default"] for p in CONTROL_PORTS}

PRESETS = [
    ("init_dual_morph", "01 Init Dual Morph", dict(
        model_a=0, cutoff_a=650.0, res_a=35.0, morph_a=10.0,
        model_b=1, cutoff_b=1800.0, res_b=40.0, morph_b=25.0,
        routing=0, link_mode=0, ab_balance=0.0, invert_b=0,
        lfo_shape=0, lfo_rate=0.65, lfo_sync=0, lfo_phase=90.0,
        lfo_mod=25.0, morph_mod=15.0, env_mod=0.0, drive=15.0)),
    ("blades_parallel_notch", "02 Blades Parallel Phase Notch", dict(
        model_a=0, cutoff_a=520.0, res_a=55.0, morph_a=32.0,
        model_b=0, cutoff_b=1450.0, res_b=58.0, morph_b=64.0,
        routing=1, link_mode=1, ab_balance=0.0, invert_b=1,
        lfo_shape=1, lfo_rate=0.40, lfo_sync=0, lfo_phase=180.0,
        lfo_mod=42.0, morph_mod=50.0, env_mod=25.0, drive=28.0)),
    ("moog_ladder_bass_punch", "03 Twin Ladder Bass Punch", dict(
        model_a=0, cutoff_a=85.0, res_a=48.0, morph_a=67.0,
        model_b=1, cutoff_b=420.0, res_b=52.0, morph_b=0.0,
        routing=0, link_mode=1, ab_balance=0.0, invert_b=0,
        lfo_shape=0, lfo_rate=0.25, lfo_sync=0, lfo_phase=45.0,
        lfo_mod=8.0, morph_mod=0.0, env_mod=58.0, env_speed=78.0, drive=38.0)),
    ("ms20_acid_screamer", "04 MS-20 Acid Screamer", dict(
        model_a=2, cutoff_a=380.0, res_a=78.0, morph_a=12.0,
        model_b=2, cutoff_b=1200.0, res_b=72.0, morph_b=48.0,
        routing=0, link_mode=2, ab_balance=0.0, invert_b=0,
        lfo_shape=2, lfo_rate=2.0, lfo_sync=1, lfo_phase=90.0,
        lfo_mod=35.0, morph_mod=30.0, env_mod=72.0, env_speed=82.0, drive=65.0)),
    ("talking_formant_choir", "05 Talking Formant Choir", dict(
        model_a=4, cutoff_a=950.0, res_a=48.0, morph_a=20.0,
        model_b=4, cutoff_b=1150.0, res_b=52.0, morph_b=75.0,
        routing=1, link_mode=2, ab_balance=0.0, invert_b=0,
        lfo_shape=0, lfo_rate=0.45, lfo_sync=0, lfo_phase=120.0,
        lfo_mod=18.0, morph_mod=78.0, env_mod=35.0, env_speed=45.0, drive=22.0)),
    ("stereo_mirror_horizon", "06 Stereo Mirror Horizon", dict(
        model_a=0, cutoff_a=780.0, res_a=50.0, morph_a=22.0,
        model_b=1, cutoff_b=1100.0, res_b=50.0, morph_b=22.0,
        routing=2, link_mode=2, ab_balance=0.0, invert_b=0,
        lfo_shape=0, lfo_rate=0.50, lfo_sync=1, lfo_phase=180.0,
        lfo_mod=55.0, morph_mod=40.0, env_mod=0.0, drive=20.0)),
    ("funk_autowah_phaser", "07 Funk Auto-Wah + Phaser", dict(
        model_a=0, cutoff_a=310.0, res_a=64.0, morph_a=33.0,
        model_b=5, cutoff_b=900.0, res_b=62.0, morph_b=70.0,
        routing=0, link_mode=1, ab_balance=0.0, invert_b=0,
        lfo_shape=0, lfo_rate=0.75, lfo_sync=0, lfo_phase=90.0,
        lfo_mod=15.0, morph_mod=25.0, env_mod=82.0, env_speed=85.0, drive=32.0)),
    ("rhythmic_step8_gate", "08 Synced Step-8 Filter Gate", dict(
        model_a=1, cutoff_a=540.0, res_a=62.0, morph_a=0.0,
        model_b=0, cutoff_b=1600.0, res_b=55.0, morph_b=33.0,
        routing=1, link_mode=1, ab_balance=0.0, invert_b=0,
        lfo_shape=6, lfo_rate=2.0, lfo_sync=1, lfo_phase=90.0,
        lfo_mod=68.0, morph_mod=35.0, env_mod=20.0, drive=35.0)),
    ("tuned_comb_resonator", "09 Tuned Comb Pluck Resonator", dict(
        model_a=3, cutoff_a=220.0, res_a=76.0, morph_a=82.0,
        model_b=3, cutoff_b=330.0, res_b=74.0, morph_b=18.0,
        routing=1, link_mode=1, ab_balance=0.0, invert_b=0,
        lfo_shape=5, lfo_rate=0.35, lfo_sync=0, lfo_phase=135.0,
        lfo_mod=8.0, morph_mod=30.0, env_mod=25.0, drive=25.0, mix=75.0)),
    ("buchla_chaos_drift", "10 Buchla Chaos Spectral Drift", dict(
        model_a=0, cutoff_a=620.0, res_a=66.0, morph_a=45.0,
        model_b=5, cutoff_b=1400.0, res_b=68.0, morph_b=30.0,
        routing=2, link_mode=0, ab_balance=0.0, invert_b=1,
        lfo_shape=5, lfo_rate=1.40, lfo_sync=0, lfo_phase=150.0,
        lfo_mod=62.0, morph_mod=65.0, env_mod=-35.0, drive=30.0)),
    ("sample_hold_robot_step", "11 S&H Robot Stepper", dict(
        model_a=2, cutoff_a=750.0, res_a=72.0, morph_a=33.0,
        model_b=4, cutoff_b=1200.0, res_b=60.0, morph_b=50.0,
        routing=1, link_mode=2, ab_balance=0.0, invert_b=0,
        lfo_shape=4, lfo_rate=4.0, lfo_sync=1, lfo_phase=90.0,
        lfo_mod=70.0, morph_mod=60.0, env_mod=0.0, drive=42.0)),
    ("audio_rate_fm_growl", "12 Audio-Rate FM Metallic Growl", dict(
        model_a=2, cutoff_a=580.0, res_a=75.0, morph_a=28.0,
        model_b=3, cutoff_b=440.0, res_b=70.0, morph_b=75.0,
        routing=0, link_mode=1, ab_balance=0.0, invert_b=0,
        lfo_shape=0, lfo_rate=32.0, lfo_sync=0, lfo_phase=90.0,
        lfo_mod=48.0, morph_mod=35.0, env_mod=55.0, env_speed=75.0, drive=58.0)),
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
        "    lv2:binary <eka_prism.so> ;",
        "    rdfs:seeAlso <eka_prism.ttl> .",
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
        "@prefix midi:   <http://lv2plug.in/ns/ext/midi#> .",
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
        "    a lv2:Plugin, lv2:FilterPlugin ;",
        '    doap:name "Eka Prism" ;',
        "    doap:license <https://spdx.org/licenses/GPL-3.0-or-later.html> ;",
        "    lv2:minorVersion 1 ;",
        "    lv2:microVersion 0 ;",
        "    lv2:optionalFeature lv2:hardRTCapable, urid:map ;",
        "    lv2:port",
        '    [ a lv2:InputPort,  lv2:AudioPort ; lv2:index 0 ; lv2:symbol "in_l"  ; lv2:name "Input Left" ] ,',
        '    [ a lv2:InputPort,  lv2:AudioPort ; lv2:index 1 ; lv2:symbol "in_r"  ; lv2:name "Input Right" ] ,',
        '    [ a lv2:OutputPort, lv2:AudioPort ; lv2:index 2 ; lv2:symbol "out_l" ; lv2:name "Output Left" ] ,',
        '    [ a lv2:OutputPort, lv2:AudioPort ; lv2:index 3 ; lv2:symbol "out_r" ; lv2:name "Output Right" ] ,',
        '    [ a lv2:InputPort, atom:AtomPort ; lv2:index 4 ; lv2:symbol "control" ; lv2:name "Control" ; '
        'lv2:designation lv2:control ; atom:bufferType atom:Sequence ; atom:supports time:Position, midi:MidiEvent ; '
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
    (LV2_DIR / "eka_prism.ttl").write_text(gen_plugin_ttl(), encoding="utf-8")
    (LV2_DIR / "presets.ttl").write_text(gen_presets_ttl(), encoding="utf-8")
    print(f"Generated TTL + {len(PRESETS)} presets in {LV2_DIR}")


if __name__ == "__main__":
    main()
