#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate deterministic LV2 TTL files and factory presets for Eka Modls."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LV2_DIR = ROOT / "lv2"
URI = "http://dmtry3.github.io/zynthian-eka/modls"

GROUPS = [
    ("g_engine", "Engine",  0, 60),
    ("g_lfo",    "LFO",     1, 50),
    ("g_uber",   "Uber",    2, 40),
    ("g_tone",   "Tone",    3, 30),
    ("g_fx",     "Live FX", 4, 20),
    ("g_master", "Master",  5, 10),
]

# Input control ports matching enum in src/eka_modls.c (indices 5..28)
CONTROL_PORTS = [
    # Page 1: Engine
    dict(idx=5,  sym="mode",       name="Mode",       grp="g_engine", default=0,       min=0,      max=5,
         props=["lv2:integer", "lv2:enumeration"],
         points=[("Chorus", 0), ("Tri-Mod", 1), ("Flanger", 2), ("Phaser", 3), ("Barber", 4), ("Uber", 5)]),
    dict(idx=6,  sym="depth",      name="Depth",      grp="g_engine", default=45.0,    min=0.0,    max=100.0,   unit="units:pc"),
    dict(idx=7,  sym="feedback",   name="Feedback",   grp="g_engine", default=20.0,    min=-100.0, max=100.0,   unit="units:pc"),
    dict(idx=8,  sym="mix",        name="Mix",        grp="g_engine", default=50.0,    min=0.0,    max=100.0,   unit="units:pc"),

    # Page 2: LFO
    dict(idx=9,  sym="shape",      name="Shape",      grp="g_lfo",    default=0,       min=0,      max=5,
         props=["lv2:integer", "lv2:enumeration"],
         points=[("Sine", 0), ("Tri", 1), ("Exp", 2), ("S&H", 3), ("Chaos", 4), ("Env", 5)]),
    dict(idx=10, sym="rate",       name="Rate",       grp="g_lfo",    default=0.65,    min=0.02,   max=20.0,    unit="units:hz", props=["pprops:logarithmic"]),
    dict(idx=11, sym="sync",       name="Sync",       grp="g_lfo",    default=0,       min=0,      max=1,
         props=["lv2:integer", "lv2:toggled", "lv2:enumeration"],
         points=[("Free", 0), ("Sync", 1)]),
    dict(idx=12, sym="stereo",     name="Stereo",     grp="g_lfo",    default=90.0,    min=0.0,    max=180.0,   unit="units:degree"),

    # Page 3: Uber
    dict(idx=13, sym="delay_ms",   name="Delay",      grp="g_uber",   default=8.0,     min=0.3,    max=120.0,   unit="units:ms", props=["pprops:logarithmic"]),
    dict(idx=14, sym="spread",     name="Spread",     grp="g_uber",   default=40.0,    min=0.0,    max=100.0,   unit="units:pc"),
    dict(idx=15, sym="diffuse",    name="Diffuse",    grp="g_uber",   default=15.0,    min=0.0,    max=100.0,   unit="units:pc"),
    dict(idx=16, sym="overmod",    name="OverMod",    grp="g_uber",   default=0.0,     min=0.0,    max=100.0,   unit="units:pc"),

    # Page 4: Tone
    dict(idx=17, sym="low_cut",    name="Low Cut",    grp="g_tone",   default=90.0,    min=20.0,   max=600.0,   unit="units:hz", props=["pprops:logarithmic"]),
    dict(idx=18, sym="high_cut",   name="High Cut",   grp="g_tone",   default=12000.0, min=1000.0, max=20000.0, unit="units:hz", props=["pprops:logarithmic"]),
    dict(idx=19, sym="drive",      name="Drive",      grp="g_tone",   default=10.0,    min=0.0,    max=100.0,   unit="units:pc"),
    dict(idx=20, sym="env_mod",    name="Env Mod",    grp="g_tone",   default=0.0,     min=-100.0, max=100.0,   unit="units:pc"),

    # Page 5: Live FX
    dict(idx=21, sym="rate_fx",    name="Rate FX",    grp="g_fx",     default=0.0,     min=-100.0, max=100.0,   unit="units:pc"),
    dict(idx=22, sym="warp_fx",    name="Warp FX",    grp="g_fx",     default=0.0,     min=0.0,    max=100.0,   unit="units:pc"),
    dict(idx=23, sym="comb_fx",    name="Comb FX",    grp="g_fx",     default=0.0,     min=-100.0, max=100.0,   unit="units:pc"),
    dict(idx=24, sym="fx_zero",    name="FX Zero",    grp="g_fx",     default=0,       min=0,      max=1,
         props=["lv2:integer", "lv2:toggled", "lv2:enumeration"],
         points=[("Off", 0), ("Zero", 1)]),

    # Page 6: Master
    dict(idx=25, sym="width",      name="Width",      grp="g_master", default=100.0,   min=0.0,    max=200.0,   unit="units:pc"),
    dict(idx=26, sym="lfo_freeze", name="LFO Freeze", grp="g_master", default=0,       min=0,      max=1,
         props=["lv2:integer", "lv2:toggled", "lv2:enumeration"],
         points=[("Run", 0), ("Freeze", 1)]),
    dict(idx=27, sym="output_db",  name="Output",     grp="g_master", default=0.0,     min=-18.0,  max=12.0,    unit="units:db"),
    dict(idx=28, sym="bypass",     name="Bypass",     grp="g_master", default=0,       min=0,      max=1,
         props=["lv2:integer", "lv2:toggled", "lv2:enumeration"],
         points=[("Off", 0), ("On", 1)]),
]

# Output telemetry ports (indices 29..36)
OUTPUT_PORTS = [
    (29, "in_peak",   "Input Peak",  -60.0, 12.0,  "units:db"),
    (30, "out_peak",  "Output Peak", -60.0, 12.0,  "units:db"),
    (31, "lfo_l",     "LFO Left",     -1.0,  1.0,  "units:coef"),
    (32, "lfo_r",     "LFO Right",    -1.0,  1.0,  "units:coef"),
    (33, "env_lvl",   "Env Level",     0.0,  1.0,  "units:coef"),
    (34, "eff_mod",   "Eff Mod",       0.0,  1.0,  "units:coef"),
    (35, "fb_energy", "FB Energy",     0.0,  1.0,  "units:coef"),
    (36, "host_bpm",  "Host BPM",     20.0, 400.0, "units:bpm"),
]

DEFAULTS = {p["sym"]: p["default"] for p in CONTROL_PORTS}

PRESETS = [
    # Musical Classics (01..07)
    ("init_warm_chorus", "01 Init Warm Chorus", dict(
        mode=0, depth=42.0, feedback=14.0, mix=48.0,
        shape=0, rate=0.55, sync=0, stereo=90.0,
        delay_ms=9.5, spread=35.0, diffuse=12.0, overmod=0.0,
        low_cut=95.0, high_cut=9500.0, drive=12.0, width=115.0)),
    ("juno_velvet_ensemble", "02 Juno Velvet Ensemble", dict(
        mode=1, depth=54.0, feedback=18.0, mix=55.0,
        shape=0, rate=0.42, sync=0, stereo=120.0,
        delay_ms=11.0, spread=62.0, diffuse=25.0, overmod=12.0,
        low_cut=110.0, high_cut=8500.0, drive=18.0, width=140.0)),
    ("tzf_tape_jet", "03 Through-Zero Tape Jet", dict(
        mode=2, depth=68.0, feedback=58.0, mix=50.0,
        shape=1, rate=0.28, sync=0, stereo=45.0,
        delay_ms=5.5, spread=20.0, diffuse=5.0, overmod=6.0,
        low_cut=75.0, high_cut=11500.0, drive=22.0, width=115.0)),
    ("vintage_optical_phaser", "04 Vintage Optical Phaser", dict(
        mode=3, depth=66.0, feedback=55.0, mix=50.0,
        shape=2, rate=0.45, sync=0, stereo=95.0,
        delay_ms=7.0, spread=35.0, diffuse=8.0, overmod=0.0,
        low_cut=65.0, high_cut=13000.0, drive=15.0, width=120.0)),
    ("synced_dub_barberpole", "05 Synced Dub Barberpole", dict(
        mode=4, depth=72.0, feedback=48.0, mix=52.0,
        shape=0, rate=0.25, sync=1, stereo=110.0,
        delay_ms=6.5, spread=45.0, diffuse=22.0, overmod=8.0,
        low_cut=100.0, high_cut=10000.0, drive=18.0, width=130.0)),
    ("dimension_bass_safe", "06 Dimension Bass-Safe Wide", dict(
        mode=1, depth=34.0, feedback=8.0, mix=45.0,
        shape=0, rate=0.35, sync=0, stereo=150.0,
        delay_ms=7.5, spread=48.0, diffuse=18.0, overmod=0.0,
        low_cut=180.0, high_cut=12500.0, drive=6.0, width=155.0)),
    ("dynamic_touch_flanger", "07 Dynamic Touch Flanger", dict(
        mode=2, depth=62.0, feedback=-62.0, mix=50.0,
        shape=5, rate=0.80, sync=0, stereo=75.0,
        delay_ms=4.5, spread=25.0, diffuse=10.0, overmod=10.0,
        low_cut=85.0, high_cut=12000.0, drive=20.0, env_mod=65.0, width=125.0)),

    # Uber-Mod & Extreme Sound Design (08..14)
    ("uber_diffused_halo", "08 Uber Diffused Halo", dict(
        mode=5, depth=56.0, feedback=52.0, mix=58.0,
        shape=0, rate=0.38, sync=0, stereo=135.0,
        delay_ms=26.0, spread=72.0, diffuse=76.0, overmod=24.0,
        low_cut=120.0, high_cut=7800.0, drive=22.0, width=155.0)),
    ("warped_vinyl_chaos", "09 Warped Vinyl Chaos", dict(
        mode=0, depth=78.0, feedback=25.0, mix=88.0,
        shape=4, rate=1.35, sync=0, stereo=65.0,
        delay_ms=16.0, spread=55.0, diffuse=20.0, overmod=64.0,
        low_cut=130.0, high_cut=4800.0, drive=52.0, width=110.0)),
    ("metallic_comb_ring", "10 Metallic Comb Resonator", dict(
        mode=2, depth=35.0, feedback=-90.0, mix=62.0,
        shape=3, rate=1.50, sync=1, stereo=140.0,
        delay_ms=3.2, spread=58.0, diffuse=18.0, overmod=35.0,
        low_cut=120.0, high_cut=14500.0, drive=35.0, width=140.0)),
    ("laser_12stage_vortex", "11 Laser 12-Stage Vortex", dict(
        mode=3, depth=88.0, feedback=86.0, mix=62.0,
        shape=2, rate=1.80, sync=0, stereo=165.0,
        delay_ms=4.0, spread=100.0, diffuse=34.0, overmod=58.0,
        low_cut=90.0, high_cut=16000.0, drive=30.0, width=165.0)),
    ("infinite_shepard_dive", "12 Infinite Shepard Dive", dict(
        mode=4, depth=92.0, feedback=-82.0, mix=68.0,
        shape=0, rate=0.55, sync=0, stereo=180.0,
        delay_ms=5.0, spread=85.0, diffuse=62.0, overmod=45.0,
        low_cut=105.0, high_cut=11000.0, drive=28.0, width=175.0)),
    ("audio_rate_fm_alien", "13 Audio-Rate FM Alien", dict(
        mode=1, depth=82.0, feedback=-68.0, mix=72.0,
        shape=4, rate=9.50, sync=0, stereo=155.0,
        delay_ms=6.0, spread=70.0, diffuse=42.0, overmod=92.0,
        low_cut=140.0, high_cut=13500.0, drive=48.0, width=160.0)),
    ("cosmic_self_osc_wash", "14 Cosmic Self-Osc Wash", dict(
        mode=5, depth=85.0, feedback=91.0, mix=74.0,
        shape=4, rate=0.48, sync=0, stereo=160.0,
        delay_ms=64.0, spread=95.0, diffuse=92.0, overmod=78.0,
        low_cut=150.0, high_cut=6800.0, drive=44.0, env_mod=-45.0, width=185.0)),
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
        "    lv2:binary <eka_modls.so> ;",
        "    rdfs:seeAlso <eka_modls.ttl> .",
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
        "    a lv2:Plugin, lv2:ModulatorPlugin ;",
        '    doap:name "Eka Modls" ;',
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
    (LV2_DIR / "eka_modls.ttl").write_text(gen_plugin_ttl(), encoding="utf-8")
    (LV2_DIR / "presets.ttl").write_text(gen_presets_ttl(), encoding="utf-8")
    print(f"Generated TTL + {len(PRESETS)} presets in {LV2_DIR}")


if __name__ == "__main__":
    main()
