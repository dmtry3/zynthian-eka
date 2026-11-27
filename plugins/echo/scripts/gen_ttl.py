#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate deterministic LV2 TTL files and factory presets for Eka Echo."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LV2_DIR = ROOT / "lv2"
URI = "http://dmtry3.github.io/zynthian-eka/echo"

GROUPS = [
    ("g_time",   "Time",        0, 50),
    ("g_fx",     "Effects",     1, 45),
    ("g_mode",   "Mode & Sync", 2, 40),
    ("g_tone",   "Tone",        3, 30),
    ("g_motion", "Motion",      4, 20),
    ("g_perf",   "Perform",     5, 10),
]

# Input control ports matching enum in src/eka_echo.c (indices 5..28)
CONTROL_PORTS = [
    # Page 1: Time
    dict(idx=5,  sym="time_l",      name="Time L",    grp="g_time",   default=375.0,  min=5.0,   max=2500.0,  unit="units:ms", props=["pprops:logarithmic"]),
    dict(idx=6,  sym="time_r",      name="Time R",    grp="g_time",   default=500.0,  min=5.0,   max=2500.0,  unit="units:ms", props=["pprops:logarithmic"]),
    dict(idx=7,  sym="feedback",    name="Feedback",  grp="g_time",   default=42.0,   min=0.0,   max=110.0,   unit="units:pc"),
    dict(idx=8,  sym="mix",         name="Mix",       grp="g_time",   default=35.0,   min=0.0,   max=100.0,   unit="units:pc"),

    # Page 2: Mode & Sync
    dict(idx=9,  sym="mode",        name="Mode",      grp="g_mode",   default=1,      min=0,     max=3,
         props=["lv2:integer", "lv2:enumeration"],
         points=[("Clean", 0), ("Tape", 1), ("BBD", 2), ("Diff", 3)]),
    dict(idx=10, sym="routing",     name="Routing",   grp="g_mode",   default=0,      min=0,     max=2,
         props=["lv2:integer", "lv2:enumeration"],
         points=[("Stereo", 0), ("PingPong", 1), ("Cross", 2)]),
    dict(idx=11, sym="sync",        name="Sync",      grp="g_mode",   default=1,      min=0,     max=1,
         props=["lv2:integer", "lv2:toggled", "lv2:enumeration"],
         points=[("Free", 0), ("Sync", 1)]),
    dict(idx=12, sym="width",       name="Width",     grp="g_mode",   default=100.0,  min=0.0,   max=100.0,   unit="units:pc"),

    # Page 3: Tone (Loop)
    dict(idx=13, sym="hpf_hz",      name="HPF",       grp="g_tone",   default=80.0,   min=20.0,  max=1000.0,  unit="units:hz", props=["pprops:logarithmic"]),
    dict(idx=14, sym="lpf_hz",      name="LPF",       grp="g_tone",   default=8500.0, min=500.0, max=20000.0, unit="units:hz", props=["pprops:logarithmic"]),
    dict(idx=15, sym="drive_pct",   name="Drive",     grp="g_tone",   default=15.0,   min=0.0,   max=100.0,   unit="units:pc"),
    dict(idx=16, sym="diffuse_pct", name="Diffuse",   grp="g_tone",   default=0.0,    min=0.0,   max=100.0,   unit="units:pc"),

    # Page 4: Motion
    dict(idx=17, sym="mod_rate",    name="Mod Rate",  grp="g_motion", default=0.6,    min=0.05,  max=8.0,     unit="units:hz", props=["pprops:logarithmic"]),
    dict(idx=18, sym="mod_depth",   name="Mod Depth", grp="g_motion", default=18.0,   min=0.0,   max=100.0,   unit="units:pc"),
    dict(idx=19, sym="duck_pct",    name="Duck",      grp="g_motion", default=0.0,    min=0.0,   max=100.0,   unit="units:pc"),
    dict(idx=20, sym="duck_rel",    name="Duck Rel",  grp="g_motion", default=220.0,  min=50.0,  max=1000.0,  unit="units:ms", props=["pprops:logarithmic"]),

    # Page 5: Perform
    dict(idx=21, sym="freeze",      name="Freeze",    grp="g_perf",   default=0,      min=0,     max=1,
         props=["lv2:integer", "lv2:toggled", "lv2:enumeration"],
         points=[("Off", 0), ("On", 1)]),
    dict(idx=22, sym="trails",      name="Trails",    grp="g_perf",   default=0,      min=0,     max=1,
         props=["lv2:integer", "lv2:toggled", "lv2:enumeration"],
         points=[("Send On", 0), ("Mute In", 1)]),
    dict(idx=23, sym="output_db",   name="Output",    grp="g_perf",   default=0.0,    min=-18.0, max=12.0,    unit="units:db"),
    dict(idx=24, sym="bypass",      name="Bypass",    grp="g_perf",   default=0,      min=0,     max=1,
         props=["lv2:integer", "lv2:toggled", "lv2:enumeration"],
         points=[("Off", 0), ("On", 1)]),

    # Page 2 (Live): Effects
    dict(idx=25, sym="time_fx",     name="Time FX",   grp="g_fx",     default=0.0,    min=-100.0, max=100.0,  unit="units:pc"),
    dict(idx=26, sym="time_link",   name="Time Link", grp="g_fx",     default=1,      min=0,      max=1,
         props=["lv2:integer", "lv2:toggled", "lv2:enumeration"],
         points=[("Off", 0), ("On", 1)]),
    dict(idx=27, sym="filter_fx",   name="Filter FX", grp="g_fx",     default=0.0,    min=-100.0, max=100.0,  unit="units:pc"),
    dict(idx=28, sym="wash_fx",     name="Wash FX",   grp="g_fx",     default=0.0,    min=0.0,    max=100.0,  unit="units:pc"),
    dict(idx=29, sym="fx_zero",     name="FX Zero",   grp="g_fx",     default=0,      min=0,      max=1,
         props=["lv2:integer", "lv2:toggled", "lv2:enumeration"],
         points=[("Off", 0), ("Zero", 1)]),
    dict(idx=30, sym="time_smooth", name="Time Smooth", grp="g_fx",   default=50.0,   min=0.0,    max=100.0,  unit="units:pc"),
]

# Output telemetry ports (indices 31..38)
OUTPUT_PORTS = [
    (31, "in_peak",     "Input Peak",   -60.0, 12.0,   "units:db"),
    (32, "out_peak",    "Output Peak",  -60.0, 12.0,   "units:db"),
    (33, "wet_l_db",    "Wet L",        -60.0, 12.0,   "units:db"),
    (34, "wet_r_db",    "Wet R",        -60.0, 12.0,   "units:db"),
    (35, "duck_db",     "Duck GR",        0.0, 24.0,   "units:db"),
    (36, "actual_ms_l", "Actual L",       5.0, 2500.0, "units:ms"),
    (37, "actual_ms_r", "Actual R",       5.0, 2500.0, "units:ms"),
    (38, "host_bpm",    "Host BPM",      20.0, 400.0,  "units:bpm"),
]

DEFAULTS = {p["sym"]: p["default"] for p in CONTROL_PORTS}

PRESETS = [
    ("init_quarter", "01 Init Quarter", dict(
        time_l=500.0, time_r=500.0, feedback=38.0, mix=30.0, mode=0, routing=0, sync=1,
        hpf_hz=60.0, lpf_hz=12000.0, drive_pct=0.0, mod_depth=0.0)),
    ("dotted_eighth_edge", "02 Dotted Eighth Edge", dict(
        time_l=375.0, time_r=500.0, feedback=48.0, mix=38.0, mode=0, routing=0, sync=1,
        hpf_hz=120.0, lpf_hz=7200.0, drive_pct=12.0, mod_rate=0.5, mod_depth=15.0)),
    ("warm_space_tape", "03 Warm Space Tape", dict(
        time_l=375.0, time_r=495.0, feedback=56.0, mix=40.0, mode=1, routing=2, sync=1,
        hpf_hz=110.0, lpf_hz=4800.0, drive_pct=35.0, mod_rate=0.45, mod_depth=32.0)),
    ("analog_bbd_tail", "04 Analog BBD Slap & Tail", dict(
        time_l=240.0, time_r=265.0, feedback=52.0, mix=36.0, mode=2, routing=0, sync=0,
        hpf_hz=140.0, lpf_hz=3200.0, drive_pct=42.0, mod_rate=0.8, mod_depth=28.0)),
    ("golden_ping_pong", "05 Golden Ratio Ping-Pong", dict(
        time_l=310.0, time_r=500.0, feedback=54.0, mix=38.0, mode=1, routing=1, sync=0,
        width=100.0, hpf_hz=150.0, lpf_hz=6800.0, drive_pct=18.0, mod_depth=20.0)),
    ("ducking_lead_echo", "06 Ducking Lead Echo", dict(
        time_l=375.0, time_r=500.0, feedback=62.0, mix=46.0, mode=1, routing=1, sync=1,
        hpf_hz=180.0, lpf_hz=5500.0, drive_pct=20.0, duck_pct=78.0, duck_rel=260.0)),
    ("cloudy_diffused_wash", "07 Cloudy Diffused Wash", dict(
        time_l=420.0, time_r=640.0, feedback=68.0, mix=48.0, mode=3, routing=2, sync=0,
        hpf_hz=160.0, lpf_hz=6200.0, drive_pct=15.0, diffuse_pct=82.0, mod_rate=0.35, mod_depth=35.0)),
    ("dub_siren_throw", "08 Dub Siren Throw", dict(
        time_l=375.0, time_r=375.0, feedback=82.0, mix=45.0, mode=1, routing=1, sync=1,
        hpf_hz=320.0, lpf_hz=2600.0, drive_pct=65.0, diffuse_pct=20.0, mod_rate=0.7, mod_depth=26.0)),
    ("polyrhythm_3_vs_4", "09 Polyrhythm 3 vs 4", dict(
        time_l=333.3, time_r=375.0, feedback=55.0, mix=42.0, mode=0, routing=0, sync=1,
        hpf_hz=100.0, lpf_hz=9000.0, drive_pct=10.0, mod_depth=12.0)),
    ("infinite_freeze_pad", "10 Infinite Freeze Pad", dict(
        time_l=600.0, time_r=820.0, feedback=78.0, mix=55.0, mode=3, routing=2, sync=0,
        hpf_hz=130.0, lpf_hz=5200.0, drive_pct=20.0, diffuse_pct=90.0, mod_rate=0.25, mod_depth=40.0)),
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
        "    lv2:binary <eka_echo.so> ;",
        "    rdfs:seeAlso <eka_echo.ttl> .",
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
        "    a lv2:Plugin, lv2:DelayPlugin ;",
        '    doap:name "Eka Echo" ;',
        "    doap:license <https://spdx.org/licenses/GPL-3.0-or-later.html> ;",
        "    lv2:minorVersion 0 ;",
        "    lv2:microVersion 4 ;",
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
    (LV2_DIR / "eka_echo.ttl").write_text(gen_plugin_ttl(), encoding="utf-8")
    (LV2_DIR / "presets.ttl").write_text(gen_presets_ttl(), encoding="utf-8")
    print(f"Generated TTL + {len(PRESETS)} presets in {LV2_DIR}")


if __name__ == "__main__":
    main()
