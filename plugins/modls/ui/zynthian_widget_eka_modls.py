#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# SPDX-License-Identifier: GPL-3.0-or-later
# Zynthian widget for Eka Modls (class and file name: zynthian_widget_eka_modls).
# Retained-mode Tkinter Canvas: static chrome is created once per layout/view,
# and dynamic elements are moved/reconfigured in-place only when values change.
from collections import deque
import math
import time
import tkinter

from zyngui import zynthian_gui_config
from zyngui import zynthian_widget_base

MULTI_INSTANCE = True

REFRESH_SEC = 0.10
KNOB_ACTIVE_HOLD_SEC = 0.30
KNOB_MONITOR_SEC = 0.25
DRAG_INTERVAL = 0.033
TRAIL_STEPS = 40
PHASER_STEPS = 36

PARAM_KEYS = (
    "mode", "depth", "feedback", "mix",
    "shape", "rate", "sync", "stereo",
    "delay_ms", "spread", "diffuse", "overmod",
    "low_cut", "high_cut", "drive", "env_mod",
    "rate_fx", "warp_fx", "comb_fx", "fx_zero",
    "width", "lfo_freeze", "output_db", "bypass",
)
RATE_MIN = 0.02
RATE_MAX = 20.0
F_MIN = 20.0
F_MAX = 20000.0
LOW_CUT_MAX = 600.0
HIGH_CUT_MIN = 1000.0
FLOOR_DB = -60.0
LONG_PRESS_SEC = 0.45
DOUBLE_TAP_SEC = 0.35

FONT_6B  = ("TkDefaultFont", 6, "bold")
FONT_7B  = ("TkDefaultFont", 7, "bold")
FONT_8B  = ("TkDefaultFont", 8, "bold")
FONT_9B  = ("TkDefaultFont", 9, "bold")
FONT_10B = ("TkDefaultFont", 10, "bold")

RATE_DIVISIONS = (
    ("4 bars", 16.0),
    ("2 bars", 8.0),
    ("1 bar",  4.0),
    ("1/2.",   3.0),
    ("1/2",    2.0),
    ("1/4.",   1.5),
    ("1/2T",   4.0 / 3.0),
    ("1/4",    1.0),
    ("1/8.",   0.75),
    ("1/4T",   2.0 / 3.0),
    ("1/8",    0.5),
    ("1/8T",   1.0 / 3.0),
    ("1/16",   0.25),
    ("1/16T",  1.0 / 6.0),
    ("1/32",   0.125),
    ("1/64",   1.0 / 16.0),
)

MODES = (
    ("CHORUS",  "#4dabf7"),
    ("TRI-MOD", "#63e6be"),
    ("FLANGER", "#ffd43b"),
    ("PHASER",  "#ffa94d"),
    ("BARBER",  "#ff8787"),
    ("UBER",    "#da77f2"),
)

SHAPES = (
    ("SINE",  "#4dabf7"),
    ("TRI",   "#63e6be"),
    ("EXP",   "#ffd43b"),
    ("S&H",   "#ffa94d"),
    ("CHAOS", "#da77f2"),
    ("ENV",   "#ff8787"),
)

_UBER_TAPS = tuple(((t / 7.0) * 2.0 - 1.0, t * 0.785398) for t in range(8))
_PHASER_US = tuple(i / float(PHASER_STEPS) for i in range(PHASER_STEPS + 1))

PANEL_BG   = "#16191e"
BUTTON_BG  = "#1f242c"
LINE_DIM   = "#2f3640"
GRID_COL   = "#252b34"
ZERO_COL   = "#3a4352"
LABEL_DIM  = "#8b949e"
LABEL_HI   = "#e6edf3"
LEFT_COL   = "#4dabf7"
RIGHT_COL  = "#63e6be"
AMBER_COL  = "#ffd43b"
ORANGE_COL = "#ffa94d"
UBER_COL   = "#da77f2"
HOT_RED    = "#ff6b6b"
BAND_FILL  = "#1e3448"
UBER_FILL  = "#341e42"

BYP_DIM    = "#49505a"
BYP_MID    = "#68707c"
BYP_HI     = "#9aa2ae"
BYP_FILL   = "#252930"

_LOG_RATE_SPAN = math.log(RATE_MAX / RATE_MIN)
_LOG_FREQ_SPAN = math.log(F_MAX / F_MIN)


def _clamp(v, lo, hi):
    return lo if v < lo else (hi if v > hi else v)


def _fmt_freq(f):
    if f >= 1000.0:
        return f"{f / 1000.0:.1f}k"
    return f"{f:.0f}Hz"


def _fmt_rate(hz):
    if hz >= 10.0:
        return f"{hz:.1f}Hz"
    return f"{hz:.2f}Hz"


def nearest_rate_division(hz, bpm):
    bpm = _clamp(bpm, 20.0, 400.0)
    q_sec = 60.0 / bpm
    log_t = math.log(_clamp(hz, RATE_MIN, RATE_MAX))
    best_lbl, best_hz, best_err = "1/2", hz, 1e9
    for lbl, mult in RATE_DIVISIONS:
        cand = 1.0 / (q_sec * mult)
        if cand < 0.015 or cand > 24.0:
            continue
        err = abs(math.log(cand) - log_t)
        if err < best_err:
            best_err = err
            best_lbl = lbl
            best_hz = cand
    return best_lbl, best_hz


class zynthian_widget_eka_modls(zynthian_widget_base.zynthian_widget_base):
    def __init__(self, parent):
        super().__init__(parent)
        self._last_monitor_ts = 0.0
        self._last_param_ts = 0.0
        self._last_update_params = None
        self.trail = deque(maxlen=TRAIL_STEPS)  # (lfo_l, lfo_r)
        self._trail_ver = 0
        self._last_trail_pt = None
        self._trail_same_count = 0

        self.hold_in = FLOOR_DB
        self.hold_out = FLOOR_DB
        self.hold_mod = 0.0
        self.hold_fb = 0.0
        self._telemetry = (FLOOR_DB, FLOOR_DB, 0.0, 0.0, 0.0, 0.0, 0.0, 120.0)

        self.scope_box = (0.0, 0.0, 1.0, 1.0)
        self.tone_box = (0.0, 0.0, 1.0, 1.0)
        self.over_box = (0.0, 0.0, 1.0, 1.0)
        self.xy_box = (0.0, 0.0, 1.0, 1.0)
        self.fx_sliders = {}
        self.fx_view = False
        self.fx_snap = False
        self.handle_pos = {}
        self.hit_rects = {}
        self._last_ctrl_titles = {}
        self._last_scope_tap = 0.0
        self.drag_mode = None  # ("scope",) | ("tone", "lc"|"hc") | ("over",) | ("xy",) | ("fx", sym) | ("uber", t0)
        self._last_drag_ts = 0.0
        self._pending_drag = None

        self._scene_key = None
        self._geom = {}
        self._ui = {}
        self._topo_mode = None
        self._topo = {}
        self._cache = {}

        self.widget_canvas = tkinter.Canvas(
            self,
            highlightthickness=0,
            relief="flat",
            bg=zynthian_gui_config.color_bg,
        )
        self.widget_canvas.grid(row=0, column=0, sticky="news")
        self.widget_canvas.bind("<ButtonPress-1>", self._on_press)
        self.widget_canvas.bind("<B1-Motion>", self._on_motion)
        self.widget_canvas.bind("<ButtonRelease-1>", self._on_release)

    def _read_param_snapshot(self):
        proc = self.processor
        if proc is None:
            return None
        ctrls = getattr(proc, "controllers_dict", None)
        if not ctrls:
            return None
        return tuple(ctrls[k].value if k in ctrls else None for k in PARAM_KEYS)

    def update(self):
        if self.processor is None:
            return
        now = time.monotonic()
        snap = self._read_param_snapshot()
        params_changed = (snap != self._last_update_params)
        if params_changed:
            self._last_update_params = snap
            self._last_param_ts = now

        knob_active = (self.drag_mode is not None) or ((now - self._last_param_ts) < KNOB_ACTIVE_HOLD_SEC)
        interval = KNOB_MONITOR_SEC if knob_active else REFRESH_SEC

        if (now - self._last_monitor_ts) >= interval:
            self._last_monitor_ts = now
            super().update()
        elif params_changed and self._scene_key is not None:
            # Fast incremental redraw without blocking on Jalv monitor IPC
            self.refresh_gui(sample_telemetry=False)

    def set_processor(self, processor):
        if hasattr(super(), "set_processor"):
            super().set_processor(processor)
        else:
            self.processor = processor
        self._last_update_params = None
        self._last_monitor_ts = 0.0

    def on_size(self, event):
        if not super().on_size(event):
            return
        self.widget_canvas.grid(row=0, column=0, sticky="news")
        self._scene_key = None
        self.refresh_gui(sample_telemetry=False)

    def _get(self, sym, fallback):
        if not self.processor:
            return fallback
        zctrl = self.processor.controllers_dict.get(sym)
        if zctrl is None or zctrl.value is None:
            return fallback
        try:
            return float(zctrl.value)
        except (TypeError, ValueError):
            return fallback

    def _set(self, sym, value):
        if not self.processor:
            return False
        zctrl = self.processor.controllers_dict.get(sym)
        if zctrl is not None and zctrl.value != value:
            zctrl.set_value(value)
            return True
        return False

    def _sync_encoder_labels(self, st, bpm):
        if not self.processor:
            return
        if st["sync"]:
            lbl, _ = nearest_rate_division(st["rate"], bpm)
            want = {"rate": f"Rate {lbl}"}
        else:
            want = {"rate": "Rate"}
        if want == self._last_ctrl_titles:
            return
        self._last_ctrl_titles = dict(want)
        zgui_ctrls = getattr(getattr(self, "zyngui_control", None), "zgui_controllers", None) or ()
        for sym, title in want.items():
            zctrl = self.processor.controllers_dict.get(sym)
            if zctrl is None:
                continue
            try:
                zctrl.short_name = title
            except Exception:
                pass
            for zgc in zgui_ctrls:
                if getattr(zgc, "zctrl", None) is zctrl:
                    try:
                        zgc.set_title(title)
                    except Exception:
                        pass

    def _select_group(self, group_sym, group_name=None):
        ctrl_scr = getattr(self, "zyngui_control", None)
        if ctrl_scr is None:
            return
        try:
            info = getattr(ctrl_scr, "screen_info", None)
            if info and len(info) >= 4 and info[3] is self.processor:
                if info[0] == group_sym or (group_name and (info[0] == group_name or info[2] == group_name)):
                    return
            rows = ctrl_scr.list_data
        except Exception:
            return
        for i, row in enumerate(rows):
            if len(row) >= 4 and row[3] is self.processor:
                if row[0] == group_sym or (group_name and (row[0] == group_name or row[2] == group_name)):
                    try:
                        ctrl_scr.select(i)
                    except Exception:
                        pass
                    return

    def _read_state(self):
        return {
            "mode":       int(_clamp(round(self._get("mode", 0.0)), 0, len(MODES) - 1)),
            "depth":      self._get("depth", 45.0),
            "feedback":   self._get("feedback", 20.0),
            "mix":        self._get("mix", 50.0),
            "shape":      int(_clamp(round(self._get("shape", 0.0)), 0, len(SHAPES) - 1)),
            "rate":       self._get("rate", 0.65),
            "sync":       self._get("sync", 0.0) >= 0.5,
            "stereo":     self._get("stereo", 90.0),
            "delay_ms":   self._get("delay_ms", 8.0),
            "spread":     self._get("spread", 40.0),
            "diffuse":    self._get("diffuse", 15.0),
            "overmod":    self._get("overmod", 0.0),
            "low_cut":    self._get("low_cut", 90.0),
            "high_cut":   self._get("high_cut", 12000.0),
            "drive":      self._get("drive", 10.0),
            "env_mod":    self._get("env_mod", 0.0),
            "rate_fx":    self._get("rate_fx", 0.0),
            "warp_fx":    self._get("warp_fx", 0.0),
            "comb_fx":    self._get("comb_fx", 0.0),
            "fx_zero":    self._get("fx_zero", 0.0) >= 0.5,
            "width":      self._get("width", 100.0),
            "lfo_freeze": self._get("lfo_freeze", 0.0) >= 0.5,
            "output_db":  self._get("output_db", 0.0),
            "bypass":     self._get("bypass", 0.0) >= 0.5,
        }

    def _rate_to_x(self, hz, x0, x1):
        t = math.log(_clamp(hz, RATE_MIN, RATE_MAX) / RATE_MIN) / _LOG_RATE_SPAN
        return x0 + t * (x1 - x0)

    def _x_to_rate(self, x, x0, x1):
        t = _clamp((x - x0) / max(1.0, x1 - x0), 0.0, 1.0)
        return RATE_MIN * ((RATE_MAX / RATE_MIN) ** t)

    def _freq_to_x(self, f, x0, x1):
        t = math.log(_clamp(f, F_MIN, F_MAX) / F_MIN) / _LOG_FREQ_SPAN
        return x0 + t * (x1 - x0)

    def _x_to_freq(self, x, x0, x1):
        t = _clamp((x - x0) / max(1.0, x1 - x0), 0.0, 1.0)
        return F_MIN * ((F_MAX / F_MIN) ** t)

    def _pull_telemetry(self, st):
        mons = self.monitors or {}
        in_db   = _clamp(float(mons.get("in_peak", FLOOR_DB)), FLOOR_DB, 12.0)
        out_db  = _clamp(float(mons.get("out_peak", FLOOR_DB)), FLOOR_DB, 12.0)
        lfo_l   = _clamp(float(mons.get("lfo_l", 0.0)), -1.0, 1.0)
        lfo_r   = _clamp(float(mons.get("lfo_r", 0.0)), -1.0, 1.0)
        env_lvl = _clamp(float(mons.get("env_lvl", 0.0)), 0.0, 1.0)
        eff_mod = _clamp(float(mons.get("eff_mod", st["depth"] * 0.005)), 0.0, 1.0)
        fb_en   = _clamp(float(mons.get("fb_energy", 0.0)), 0.0, 1.0)
        bpm     = _clamp(float(mons.get("host_bpm", 120.0)), 20.0, 400.0)

        self.hold_in  = in_db   if in_db   >= self.hold_in  else max(in_db,   self.hold_in  - 1.5)
        self.hold_out = out_db  if out_db  >= self.hold_out else max(out_db,  self.hold_out - 1.5)
        self.hold_mod = eff_mod if eff_mod >= self.hold_mod else max(eff_mod, self.hold_mod - 0.045)
        self.hold_fb  = fb_en   if fb_en   >= self.hold_fb  else max(fb_en,   self.hold_fb  - 0.045)

        pt = (round(lfo_l, 3), round(lfo_r, 3))
        self.trail.append(pt)
        if pt == self._last_trail_pt:
            self._trail_same_count += 1
        else:
            self._last_trail_pt = pt
            self._trail_same_count = 1
        if len(self.trail) <= TRAIL_STEPS and self._trail_same_count <= TRAIL_STEPS:
            self._trail_ver += 1

        self._telemetry = (in_db, out_db, lfo_l, lfo_r, env_lvl, eff_mod, fb_en, bpm)
        return self._telemetry

    def _uber_color(self, over_pct, byp):
        if byp:
            return BYP_MID
        if over_pct >= 65.0:
            return UBER_COL
        if over_pct >= 25.0:
            return ORANGE_COL
        if over_pct > 2.0:
            return AMBER_COL
        return RIGHT_COL

    # ---------- static scene construction (only on resize or Main <-> FX toggle) ----------

    def _ensure_scene(self, cv, w, h):
        key = (w, h, self.fx_view)
        if self._scene_key == key:
            return
        self._scene_key = key
        cv.delete("all")
        self._ui.clear()
        self._topo.clear()
        self._topo_mode = None
        self._cache.clear()
        self.hit_rects.clear()
        self.handle_pos.clear()
        self.fx_sliders.clear()

        pad = 6
        btn_h = max(36, min(44, int(h * 0.12)))
        strip_h = max(30, min(38, int(h * 0.10)))
        meter_w = 80

        gx0 = pad
        gy0 = pad
        gx1 = w - pad - meter_w - 6
        gy1 = h - pad - btn_h - strip_h - 12
        fy1 = gy1 + 5 + strip_h
        self.scope_box = (gx0, gy0, gx1, gy1)

        inner_x0 = gx0 + 12
        inner_x1 = gx1 - 12
        inner_y0 = gy0 + 26
        inner_y1 = gy1 - 24
        iw = max(20.0, inner_x1 - inner_x0)
        ih = max(20.0, inner_y1 - inner_y0)

        self._geom = {
            "w": w, "h": h, "pad": pad,
            "gx0": gx0, "gy0": gy0, "gx1": gx1, "gy1": gy1, "fy1": fy1,
            "ix0": inner_x0, "ix1": inner_x1, "iy0": inner_y0, "iy1": inner_y1,
            "iw": iw, "ih": ih,
            "cx": (inner_x0 + inner_x1) * 0.5,
            "cy": (inner_y0 + inner_y1) * 0.5,
            "trail_xs": tuple(inner_x0 + (i / float(TRAIL_STEPS - 1)) * iw for i in range(TRAIL_STEPS)),
            "phaser_xs": tuple(inner_x0 + u * iw for u in _PHASER_US),
        }

        if self.fx_view:
            self._build_fx_screen(cv, gx0, gy0, gx1, fy1)
            self._build_meters(cv, gx1, gy0, fy1, w, pad)
            self._build_buttons(cv, pad, fy1 + 6, w - pad, h - pad,
                                ("fx", "snap", "zero", "freeze", "mode", "uber"))
        else:
            self._build_scope(cv, gx0, gy0, gx1, gy1)
            self._build_tone_uber_strip(cv, gx0, gy1 + 5, gx1, fy1)
            self._build_meters(cv, gx1, gy0, fy1, w, pad)
            self._build_buttons(cv, pad, fy1 + 6, w - pad, h - pad,
                                ("mode", "shape", "sync", "fx", "freeze", "uber"))

    def _build_scope(self, cv, x0, y0, x1, y1):
        g = self._geom
        ix0, ix1, iy0, iy1 = g["ix0"], g["ix1"], g["iy0"], g["iy1"]
        iw, ih = g["iw"], g["ih"]

        cv.create_rectangle(x0, y0, x1, y1, fill=PANEL_BG, outline=LINE_DIM, width=1)
        for frac in (0.25, 0.5, 0.75):
            gy = iy0 + ih * frac
            cv.create_line(ix0, gy, ix1, gy, fill=ZERO_COL if frac == 0.5 else GRID_COL, width=1)
            gx = ix0 + iw * frac
            cv.create_line(gx, iy0, gx, iy1, fill=GRID_COL, width=1)

        ui = self._ui
        # Foreground items kept above mode-specific topology via tag "scope_fg"
        ui["trail_r"] = cv.create_line(0, 0, 0, 0, fill=RIGHT_COL, width=2, state="hidden", tags="scope_fg")
        ui["trail_l"] = cv.create_line(0, 0, 0, 0, fill=LEFT_COL, width=2, state="hidden", tags="scope_fg")

        ui["puck_vx"] = cv.create_line(0, iy0, 0, iy1, fill=GRID_COL, width=1, dash=(2, 3), tags="scope_fg")
        ui["puck_hy"] = cv.create_line(ix0, 0, ix1, 0, fill=GRID_COL, width=1, dash=(2, 3), tags="scope_fg")
        ui["puck_out"] = cv.create_oval(0, 0, 0, 0, fill=LEFT_COL, outline=LABEL_HI, width=2, tags="scope_fg")
        ui["puck_in"] = cv.create_oval(0, 0, 0, 0, fill="#101318", outline="", tags="scope_fg")

        ui["sc_title"] = cv.create_text(x0 + 10, y0 + 6, text="", anchor="nw", fill=LABEL_HI, font=FONT_8B, tags="scope_fg")
        ui["sc_b0"] = cv.create_text(x1 - 8, y0 + 6, text="", anchor="ne", fill=AMBER_COL, font=FONT_8B, state="hidden", tags="scope_fg")
        ui["sc_b1"] = cv.create_text(x1 - 8, y0 + 6, text="", anchor="ne", fill=UBER_COL, font=FONT_8B, state="hidden", tags="scope_fg")
        ui["sc_foot"] = cv.create_text(x0 + 10, y1 - 6, text="", anchor="sw", fill=LABEL_DIM, font=FONT_7B, tags="scope_fg")
        ui["sc_env"] = cv.create_text(x1 - 8, y1 - 6, text="", anchor="se", fill=RIGHT_COL, font=FONT_7B, state="hidden", tags="scope_fg")

    def _build_topo(self, cv, mode):
        cv.delete("topo")
        self._topo.clear()
        self._topo_mode = mode
        self._cache.pop("topo", None)
        self._cache.pop("topo_byp", None)
        self._cache.pop("barber_dir", None)

        g = self._geom
        cx, cy = g["cx"], g["cy"]
        iy0, iy1 = g["iy0"], g["iy1"]
        t = self._topo

        if mode == 0:
            t["o0"] = cv.create_oval(0, 0, 0, 0, fill="", outline=LEFT_COL, width=2, tags="topo")
            t["i0"] = cv.create_oval(0, 0, 0, 0, fill=LEFT_COL, outline="", tags="topo")
            t["o1"] = cv.create_oval(0, 0, 0, 0, fill="", outline=RIGHT_COL, width=2, tags="topo")
            t["i1"] = cv.create_oval(0, 0, 0, 0, fill=RIGHT_COL, outline="", tags="topo")
        elif mode == 1:
            t["poly"] = cv.create_polygon(0, 0, 0, 0, 0, 0, fill="", outline=MODES[1][1], width=2, tags="topo")
            t["d0"] = cv.create_oval(0, 0, 0, 0, fill=LEFT_COL, outline="", tags="topo")
            t["d1"] = cv.create_oval(0, 0, 0, 0, fill=RIGHT_COL, outline="", tags="topo")
            t["d2"] = cv.create_oval(0, 0, 0, 0, fill=MODES[1][1], outline="", tags="topo")
        elif mode == 2:
            t["tzf_l"] = cv.create_line(cx, iy0, cx, iy1, fill=AMBER_COL, width=2, dash=(4, 3), tags="topo")
            t["tzf_t"] = cv.create_text(cx, iy0 + 8, text="0 ms TZF", fill=AMBER_COL, font=FONT_7B, tags="topo")
            t["arm_l"] = cv.create_line(cx, cy - 16, cx, cy - 16, fill=LEFT_COL, width=3, tags="topo")
            t["arm_r"] = cv.create_line(cx, cy + 16, cx, cy + 16, fill=RIGHT_COL, width=3, tags="topo")
            t["dot_l"] = cv.create_oval(0, 0, 0, 0, fill=LEFT_COL, outline=LABEL_HI, width=1, tags="topo")
            t["dot_r"] = cv.create_oval(0, 0, 0, 0, fill=RIGHT_COL, outline=LABEL_HI, width=1, tags="topo")
            t["ring"] = cv.create_oval(0, 0, 0, 0, fill="", outline=AMBER_COL, width=2, state="hidden", tags="topo")
        elif mode in (3, 4):
            t["ph_r"] = cv.create_line(0, 0, 0, 0, fill=RIGHT_COL, width=2, tags="topo")
            t["ph_l"] = cv.create_line(0, 0, 0, 0, fill=MODES[mode][1], width=2, tags="topo")
            if mode == 4:
                t["arrow"] = cv.create_text(cx, iy1 - 8, text=">>> SHEPARD UP >>>", fill=MODES[4][1], font=FONT_7B, tags="topo")
        else:
            t["halo"] = cv.create_oval(0, 0, 0, 0, fill=UBER_FILL, outline=MODES[5][1], width=1, tags="topo")
            t["taps"] = tuple(
                cv.create_oval(0, 0, 0, 0, fill=LEFT_COL if (idx % 2 == 0) else RIGHT_COL, outline="", tags="topo")
                for idx in range(8)
            )

        cv.tag_raise("scope_fg")

    def _build_tone_uber_strip(self, cv, x0, y0, x1, y1):
        gap = 6
        split_x = x0 + (x1 - x0) * 0.62
        tx0, ty0, tx1, ty1 = x0, y0, split_x - gap * 0.5, y1
        ox0, oy0, ox1, oy1 = split_x + gap * 0.5, y0, x1, y1
        self.tone_box = (tx0, ty0, tx1, ty1)
        self.over_box = (ox0, oy0, ox1, oy1)

        ui = self._ui
        tmid_y = (ty0 + ty1) * 0.5
        omid_y = (oy0 + oy1) * 0.5

        cv.create_rectangle(tx0, ty0, tx1, ty1, fill=PANEL_BG, outline=LINE_DIM, width=1)
        ui["t_band"] = cv.create_rectangle(tx0 + 8, ty0 + 3, tx1 - 8, ty1 - 3, fill=BAND_FILL, outline="")
        ui["t_lc_l"] = cv.create_line(tx0 + 8, ty0 + 2, tx0 + 8, ty1 - 2, fill=LEFT_COL, width=2)
        ui["t_hc_l"] = cv.create_line(tx1 - 8, ty0 + 2, tx1 - 8, ty1 - 2, fill=ORANGE_COL, width=2)
        ui["t_lc_t"] = cv.create_text(tx0 + 8, tmid_y, text="", anchor="w", fill=LABEL_HI, font=FONT_7B)
        ui["t_drv_t"] = cv.create_text((tx0 + tx1) * 0.5, tmid_y, text="", anchor="center", fill=LABEL_DIM, font=FONT_7B)
        ui["t_hc_t"] = cv.create_text(tx1 - 8, tmid_y, text="", anchor="e", fill=LABEL_HI, font=FONT_7B)

        cv.create_rectangle(ox0, oy0, ox1, oy1, fill=PANEL_BG, outline=LINE_DIM, width=1)
        ui["o_fill"] = cv.create_rectangle(ox0 + 2, oy0 + 3, ox0 + 2, oy1 - 3, fill=RIGHT_COL, outline="", state="hidden")
        ui["o_line"] = cv.create_line(ox0 + 2, oy0 + 2, ox0 + 2, oy1 - 2, fill=LABEL_HI, width=2)
        ui["o_txt"] = cv.create_text((ox0 + ox1) * 0.5, omid_y, text="", fill=LABEL_HI, font=FONT_8B)

    def _build_meters(self, cv, gx1, y0, y1, w, pad):
        mx0 = gx1 + 6
        mx1 = w - pad
        cv.create_rectangle(mx0, y0, mx1, y1, fill=PANEL_BG, outline=LINE_DIM, width=1)

        top = y0 + 18
        bot = y1 - 16
        mh = max(20.0, bot - top)
        bw = (mx1 - mx0 - 12) / 4.0

        ui = self._ui
        ui["m_title"] = cv.create_text((mx0 + mx1) * 0.5, y0 + 9, text="LEVELS", fill=LABEL_DIM, font=FONT_6B)
        labels = ("IN", "MD", "FB", "OT")
        colors = (LEFT_COL, UBER_COL, AMBER_COL, RIGHT_COL)
        m_items = []
        for i in range(4):
            bx0 = mx0 + 4 + i * (bw + 1.3)
            bx1 = bx0 + bw - 1.5
            cv.create_rectangle(bx0, top, bx1, bot, fill="#101318", outline=GRID_COL, width=1)
            bar_id = cv.create_rectangle(bx0 + 1, bot - 1, bx1 - 1, bot - 1, fill=colors[i], outline="", state="hidden")
            hold_id = cv.create_line(bx0, bot, bx1, bot, fill=LABEL_HI, width=2)
            lbl_id = cv.create_text((bx0 + bx1) * 0.5, y1 - 8, text=labels[i], fill=LABEL_DIM, font=FONT_6B)
            m_items.append((bx0, bx1, bar_id, hold_id, lbl_id, colors[i]))

        self._geom["m_top"] = top
        self._geom["m_bot"] = bot
        self._geom["m_h"] = mh
        ui["meters"] = tuple(m_items)

    def _build_buttons(self, cv, x0, y0, x1, y1, keys):
        gap = 5
        bw = (x1 - x0 - gap * 5) / 6.0
        cy = (y0 + y1) * 0.5
        slots = []
        for i, key in enumerate(keys):
            bx0 = x0 + i * (bw + gap)
            bx1 = bx0 + bw
            self.hit_rects[key] = (bx0, y0, bx1, y1)
            cx = (bx0 + bx1) * 0.5
            r_id = cv.create_rectangle(bx0, y0, bx1, y1, fill=BUTTON_BG, outline=LINE_DIM, width=1)
            t_id = cv.create_text(cx, cy - 7, text="", fill=LABEL_HI, font=FONT_7B)
            s_id = cv.create_text(cx, cy + 7, text="", fill=LEFT_COL, font=FONT_8B)
            slots.append((r_id, t_id, s_id))
        self._ui["btns"] = tuple(slots)

    def _build_fx_screen(self, cv, x0, y0, x1, y1):
        cv.create_rectangle(x0, y0, x1, y1, fill=PANEL_BG, outline=LINE_DIM, width=1)
        ui = self._ui
        ui["fx_title"] = cv.create_text(x0 + 10, y0 + 6, text="", anchor="nw", fill=LABEL_HI, font=FONT_8B)

        inner_x0 = x0 + 10
        inner_x1 = x1 - 10
        pad_y0 = y0 + 24
        slider_h = 34
        pad_y1 = y1 - slider_h - 14
        self.xy_box = (inner_x0, pad_y0, inner_x1, pad_y1)

        cv.create_rectangle(inner_x0, pad_y0, inner_x1, pad_y1, fill="#111419", outline=LINE_DIM, width=1)
        cx = (inner_x0 + inner_x1) * 0.5
        for frac in (0.25, 0.5, 0.75):
            gx = inner_x0 + (inner_x1 - inner_x0) * frac
            gy = pad_y0 + (pad_y1 - pad_y0) * frac
            cv.create_line(gx, pad_y0, gx, pad_y1, fill=ZERO_COL if frac == 0.5 else GRID_COL, width=1)
            cv.create_line(inner_x0, gy, inner_x1, gy, fill=GRID_COL, width=1)

        ui["fx_tether"] = cv.create_line(cx, pad_y1, cx, pad_y1, fill=UBER_COL, width=2, state="hidden")
        ui["fx_glow"] = cv.create_oval(0, 0, 0, 0, fill="", outline=UBER_COL, width=1)
        ui["fx_puck"] = cv.create_oval(0, 0, 0, 0, fill=UBER_COL, outline=LABEL_HI, width=2)
        ui["fx_dot"] = cv.create_oval(0, 0, 0, 0, fill="#101318", outline="")

        ui["fx_xy_txt"] = cv.create_text(inner_x0 + 6, pad_y0 + 4, text="", anchor="nw", fill=LABEL_HI, font=FONT_8B)
        ui["fx_spring"] = cv.create_text(inner_x1 - 6, pad_y0 + 4, text="SPRING", anchor="ne", fill=RIGHT_COL, font=FONT_7B, state="hidden")
        ui["fx_lbl_l"] = cv.create_text(inner_x0 + 6, pad_y1 - 4, text="-COMB", anchor="sw", fill=LABEL_DIM, font=FONT_7B)
        ui["fx_lbl_r"] = cv.create_text(inner_x1 - 6, pad_y1 - 4, text="+COMB", anchor="se", fill=LABEL_DIM, font=FONT_7B)
        ui["fx_lbl_t"] = cv.create_text(cx, pad_y0 + 14, text="UBER WARP ^", anchor="n", fill=LABEL_DIM, font=FONT_7B)

        sy0 = pad_y1 + 6
        sy1 = y1 - 8
        z_btn_w = 42
        bar_x1 = inner_x1 - z_btn_w - 6
        self.fx_sliders["rate_fx"] = (inner_x0, sy0, bar_x1, sy1, True)
        mid_x = (inner_x0 + bar_x1) * 0.5
        mid_y = (sy0 + sy1) * 0.5

        cv.create_rectangle(inner_x0, sy0, bar_x1, sy1, fill=BUTTON_BG, outline=LINE_DIM, width=1)
        cv.create_line(mid_x, sy0 + 2, mid_x, sy1 - 2, fill=ZERO_COL, width=1)
        ui["fx_rfx_fill"] = cv.create_rectangle(mid_x, sy0 + 2, mid_x, sy1 - 2, fill=LEFT_COL, outline="", state="hidden")
        ui["fx_rfx_line"] = cv.create_line(mid_x, sy0 + 2, mid_x, sy1 - 2, fill=LABEL_HI, width=2)
        ui["fx_rfx_txt"] = cv.create_text(mid_x, mid_y, text="", fill=LABEL_HI, font=FONT_9B)

        zx0 = bar_x1 + 6
        zx1 = inner_x1
        self.hit_rects["z_rate_fx"] = (zx0, sy0, zx1, sy1)
        ui["fx_z_rect"] = cv.create_rectangle(zx0, sy0, zx1, sy1, fill=BUTTON_BG, outline=LEFT_COL, width=1)
        ui["fx_z_txt"] = cv.create_text((zx0 + zx1) * 0.5, mid_y, text="0", fill=LABEL_HI, font=FONT_10B)

        self._geom["fx_slider"] = (inner_x0, sy0, bar_x1, sy1, mid_x)

    # ---------- incremental per-layer updates ----------

    def refresh_gui(self, sample_telemetry=True):
        cv = self.widget_canvas
        w = max(240, int(self.width or cv.winfo_width() or 640))
        h = max(180, int(self.height or cv.winfo_height() or 380))
        st = self._read_state()
        if st["fx_zero"]:
            self._set("rate_fx", 0.0)
            self._set("warp_fx", 0.0)
            self._set("comb_fx", 0.0)
            self._set("fx_zero", 0)
            st["rate_fx"] = 0.0
            st["warp_fx"] = 0.0
            st["comb_fx"] = 0.0
            st["fx_zero"] = False
        byp = st["bypass"]
        if sample_telemetry:
            in_db, out_db, lfo_l, lfo_r, env_lvl, eff_mod, fb_en, bpm = self._pull_telemetry(st)
        else:
            in_db, out_db, lfo_l, lfo_r, env_lvl, eff_mod, fb_en, bpm = self._telemetry

        self._sync_encoder_labels(st, bpm)
        self._ensure_scene(cv, w, h)

        mode_name, mode_col = MODES[st["mode"]]
        shape_name, shape_col = SHAPES[st["shape"]]

        if self.fx_view:
            self._update_fx_screen(cv, st, byp, bpm, fb_en, mode_name)
            if sample_telemetry or "m_byp" not in self._cache or self._cache["m_byp"] != byp:
                self._update_meters(cv, byp, in_db, out_db, eff_mod, fb_en)
            self._update_fx_buttons(cv, st, byp, mode_name, mode_col)
        else:
            self._update_scope(cv, st, byp, bpm, lfo_l, lfo_r, env_lvl, fb_en,
                               mode_name, mode_col, shape_name, sample_telemetry)
            self._update_tone_uber_strip(cv, st, byp)
            if sample_telemetry or "m_byp" not in self._cache or self._cache["m_byp"] != byp:
                self._update_meters(cv, byp, in_db, out_db, eff_mod, fb_en)
            self._update_main_buttons(cv, st, byp, mode_name, mode_col, shape_name, shape_col)

    def _update_scope(self, cv, st, byp, bpm, lfo_l, lfo_r, env_lvl, fb_en,
                      mode_name, mode_col, shape_name, sample_telemetry):
        mode = st["mode"]
        if self._topo_mode != mode:
            self._build_topo(cv, mode)

        self._update_topo(cv, st, byp, lfo_l, lfo_r, fb_en, mode_col)
        if sample_telemetry or self._cache.get("trail_byp") != byp or "trail_ver" not in self._cache:
            self._update_trail(cv, byp)
        self._update_puck(cv, st, byp, mode_col)
        self._update_scope_texts(cv, st, byp, bpm, env_lvl, mode_name, shape_name)

    def _update_topo(self, cv, st, byp, lfo_l, lfo_r, fb_en, mode_col):
        mode = st["mode"]
        c_mode = BYP_MID if byp else mode_col
        c_l = BYP_MID if byp else LEFT_COL
        c_r = BYP_DIM if byp else RIGHT_COL
        c_hi = BYP_HI if byp else LABEL_HI

        t = self._topo
        if self._cache.get("topo_byp") != byp:
            self._cache["topo_byp"] = byp
            if mode == 0:
                cv.itemconfig(t["o0"], outline=c_l)
                cv.itemconfig(t["i0"], fill=c_l)
                cv.itemconfig(t["o1"], outline=c_r)
                cv.itemconfig(t["i1"], fill=c_r)
            elif mode == 1:
                cv.itemconfig(t["poly"], outline=c_mode)
                cv.itemconfig(t["d0"], fill=c_l)
                cv.itemconfig(t["d1"], fill=c_r)
                cv.itemconfig(t["d2"], fill=c_mode)
            elif mode == 2:
                tzf_c = BYP_HI if byp else AMBER_COL
                cv.itemconfig(t["tzf_l"], fill=tzf_c)
                cv.itemconfig(t["tzf_t"], fill=tzf_c)
                cv.itemconfig(t["arm_l"], fill=c_l)
                cv.itemconfig(t["arm_r"], fill=c_r)
                cv.itemconfig(t["dot_l"], fill=c_l, outline=c_hi)
                cv.itemconfig(t["dot_r"], fill=c_r, outline=c_hi)
            elif mode in (3, 4):
                cv.itemconfig(t["ph_r"], fill=c_r)
                cv.itemconfig(t["ph_l"], fill=c_mode)
                if mode == 4:
                    cv.itemconfig(t["arrow"], fill=c_mode)
            else:
                cv.itemconfig(t["halo"], fill=BYP_FILL if byp else UBER_FILL, outline=c_mode)
                for idx, tid in enumerate(t["taps"]):
                    cv.itemconfig(tid, fill=c_l if (idx % 2 == 0) else c_r)

        ql = round(lfo_l, 3)
        qr = round(lfo_r, 3)
        depth_q = round(st["depth"], 1)
        spread_q = round(st["spread"], 1)
        diffuse_q = round(st["diffuse"], 1)
        fb_q = round(st["feedback"], 1)
        comb_q = round(st["comb_fx"], 1)
        over_q = round(st["overmod"] + st["warp_fx"] * 0.85, 1)
        fben_q = round(fb_en, 2)

        if mode == 0:
            tkey = (0, ql, qr, depth_q, spread_q, diffuse_q)
        elif mode == 1:
            tkey = (1, ql, qr, depth_q, spread_q)
        elif mode == 2:
            tkey = (2, ql, qr, depth_q, fb_q, comb_q, fben_q, byp)
        elif mode in (3, 4):
            tkey = (mode, ql, qr, depth_q, spread_q, fb_q, comb_q)
        else:
            tkey = (5, ql, qr, depth_q, spread_q, diffuse_q, over_q)

        if self._cache.get("topo") == tkey:
            return
        self._cache["topo"] = tkey

        g = self._geom
        ix0, ix1, iy0, iy1 = g["ix0"], g["ix1"], g["iy0"], g["iy1"]
        iw, ih, cx, cy = g["iw"], g["ih"], g["cx"], g["cy"]
        spread = st["spread"] * 0.01
        diffuse = st["diffuse"] * 0.01
        depth = st["depth"] * 0.01
        fb = st["feedback"] * 0.01
        comb = st["comb_fx"] * 0.01
        over = _clamp((st["overmod"] + st["warp_fx"] * 0.85) * 0.01, 0.0, 1.5)

        if mode == 0:
            span_x = (0.08 + 0.18 * depth) * iw
            r = 8.0 + 10.0 * spread
            r2 = r * (0.55 + 0.35 * diffuse)
            tx0 = _clamp(cx - 0.16 * iw + lfo_l * span_x, ix0 + 10, ix1 - 10)
            ty0 = cy - 0.18 * ih + lfo_l * 0.12 * ih
            tx1 = _clamp(cx + 0.16 * iw + lfo_r * span_x, ix0 + 10, ix1 - 10)
            ty1 = cy + 0.18 * ih + lfo_r * 0.12 * ih
            cv.coords(t["o0"], tx0 - r, ty0 - r, tx0 + r, ty0 + r)
            cv.coords(t["i0"], tx0 - r2, ty0 - r2, tx0 + r2, ty0 + r2)
            cv.coords(t["o1"], tx1 - r, ty1 - r, tx1 + r, ty1 + r)
            cv.coords(t["i1"], tx1 - r2, ty1 - r2, tx1 + r2, ty1 + r2)
        elif mode == 1:
            rad_x = iw * (0.14 + 0.18 * spread)
            rad_y = ih * (0.18 + 0.22 * depth)
            ang0 = math.atan2(lfo_r, lfo_l if abs(lfo_l) > 1e-4 else 0.01)
            step = 2.0943951023931953  # 2*pi/3
            p0x = cx + math.cos(ang0) * rad_x
            p0y = cy + math.sin(ang0) * rad_y
            p1x = cx + math.cos(ang0 + step) * rad_x
            p1y = cy + math.sin(ang0 + step) * rad_y
            p2x = cx + math.cos(ang0 + 2.0 * step) * rad_x
            p2y = cy + math.sin(ang0 + 2.0 * step) * rad_y
            cv.coords(t["poly"], p0x, p0y, p1x, p1y, p2x, p2y)
            cv.coords(t["d0"], p0x - 6, p0y - 6, p0x + 6, p0y + 6)
            cv.coords(t["d1"], p1x - 6, p1y - 6, p1x + 6, p1y + 6)
            cv.coords(t["d2"], p2x - 6, p2y - 6, p2x + 6, p2y + 6)
        elif mode == 2:
            swing = iw * 0.38 * (0.15 + 0.85 * depth)
            lx = _clamp(cx + (lfo_l + comb * 0.6) * swing, ix0 + 8, ix1 - 8)
            rx = _clamp(cx + (lfo_r + comb * 0.6) * swing, ix0 + 8, ix1 - 8)
            cv.coords(t["arm_l"], cx, cy - 16, lx, cy - 16)
            cv.coords(t["arm_r"], cx, cy + 16, rx, cy + 16)
            cv.coords(t["dot_l"], lx - 6, cy - 22, lx + 6, cy - 10)
            cv.coords(t["dot_r"], rx - 6, cy + 10, rx + 6, cy + 22)
            if abs(fb) > 0.15:
                rr = 14.0 + 26.0 * abs(fb) * (0.5 + 0.5 * fb_en)
                rc = BYP_MID if byp else (HOT_RED if fb < 0 else AMBER_COL)
                cv.coords(t["ring"], cx - rr, cy - rr, cx + rr, cy + rr)
                cv.itemconfig(t["ring"], outline=rc, state="normal")
            else:
                cv.itemconfig(t["ring"], state="hidden")
        elif mode in (3, 4):
            n_notches = 4 if mode == 4 else int(round(2 + spread * 4))
            inv_n = 1.0 / float(n_notches)
            if mode == 4:
                off_l = lfo_l * 0.25 + comb * 0.4
                off_r = lfo_r * 0.25 + comb * 0.4
                bases_l = tuple(((k * inv_n) + off_l) % 1.0 for k in range(n_notches))
                bases_r = tuple(((k * inv_n) + off_r) % 1.0 for k in range(n_notches))
                bdir = fb >= 0.0
                if self._cache.get("barber_dir") != bdir:
                    self._cache["barber_dir"] = bdir
                    cv.itemconfig(t["arrow"], text=">>> SHEPARD UP >>>" if bdir else "<<< SHEPARD DOWN <<<")
            else:
                off_l = lfo_l * 0.16 * depth + comb * 0.18
                off_r = lfo_r * 0.16 * depth + comb * 0.18
                bases_l = tuple(_clamp(0.18 + (k + 0.5) * inv_n * 0.64 + off_l, 0.05, 0.95) for k in range(n_notches))
                bases_r = tuple(_clamp(0.18 + (k + 0.5) * inv_n * 0.64 + off_r, 0.05, 0.95) for k in range(n_notches))

            amp = ih * (0.18 + 0.22 * abs(fb))
            amp_r = amp * 0.8
            y_base_l = cy + 10.0
            y_base_r = cy + 16.0
            inv_w = 1.0 / 0.055
            exp_fn = math.exp
            pts_l = []
            pts_r = []
            for u, px in zip(_PHASER_US, g["phaser_xs"]):
                vl = 0.0
                for b in bases_l:
                    d = (u - b) * inv_w
                    dd = d * d
                    if dd < 9.0:
                        vl += exp_fn(-dd)
                vr = 0.0
                for b in bases_r:
                    d = (u - b) * inv_w
                    dd = d * d
                    if dd < 9.0:
                        vr += exp_fn(-dd)
                pts_l.append(px)
                pts_l.append(y_base_l - vl * amp)
                pts_r.append(px)
                pts_r.append(y_base_r - vr * amp_r)
            cv.coords(t["ph_r"], *pts_r)
            cv.coords(t["ph_l"], *pts_l)
        else:
            halo_r = 22.0 + 48.0 * diffuse + 22.0 * over
            cv.coords(t["halo"], cx - halo_r * 1.4, cy - halo_r * 0.65, cx + halo_r * 1.4, cy + halo_r * 0.65)
            sp_w = iw * (0.14 + 0.30 * spread)
            ov_a = 14.0 * (0.3 + 0.7 * over)
            dp_h = ih * (0.14 + 0.22 * depth)
            tr = 3.5 + 3.0 * diffuse
            ph_l = lfo_l * 2.4
            ph_r = lfo_r * 2.4
            for (u, ph), tid in zip(_UBER_TAPS, t["taps"]):
                tx = _clamp(cx + u * sp_w + math.sin(ph + ph_l) * ov_a, ix0 + 8, ix1 - 8)
                ty = _clamp(cy + math.cos(ph + ph_r) * dp_h, iy0 + 8, iy1 - 8)
                cv.coords(tid, tx - tr, ty - tr, tx + tr, ty + tr)

    def _update_trail(self, cv, byp):
        ui = self._ui
        if self._cache.get("trail_byp") != byp:
            self._cache["trail_byp"] = byp
            cv.itemconfig(ui["trail_l"], fill=BYP_MID if byp else LEFT_COL)
            cv.itemconfig(ui["trail_r"], fill=BYP_DIM if byp else RIGHT_COL)

        if self._cache.get("trail_ver") == self._trail_ver:
            return
        self._cache["trail_ver"] = self._trail_ver

        if len(self.trail) < 2:
            cv.itemconfig(ui["trail_r"], state="hidden")
            cv.itemconfig(ui["trail_l"], state="hidden")
            return

        g = self._geom
        cy = g["cy"]
        scale = g["ih"] * 0.36
        xs = g["trail_xs"]
        pts_l = []
        pts_r = []
        for idx, (tl, tr) in enumerate(self.trail):
            tx = xs[idx]
            pts_l.append(tx)
            pts_l.append(cy - tl * scale)
            pts_r.append(tx)
            pts_r.append(cy - tr * scale)
        cv.coords(ui["trail_r"], *pts_r)
        cv.coords(ui["trail_l"], *pts_l)
        if not self._cache.get("trail_vis"):
            self._cache["trail_vis"] = True
            cv.itemconfig(ui["trail_r"], state="normal")
            cv.itemconfig(ui["trail_l"], state="normal")

    def _update_puck(self, cv, st, byp, mode_col):
        over = _clamp((st["overmod"] + st["warp_fx"] * 0.85) * 0.01, 0.0, 1.5)
        p_col = self._uber_color(st["overmod"] + st["warp_fx"] * 0.8, byp) if over > 0.25 else (BYP_MID if byp else mode_col)
        c_hi = BYP_HI if byp else LABEL_HI
        pkey = (round(st["rate"], 3), round(st["depth"], 1), p_col, c_hi)
        if self._cache.get("puck") == pkey:
            return
        self._cache["puck"] = pkey

        g = self._geom
        ix0, ix1, iy0, iy1, ih = g["ix0"], g["ix1"], g["iy0"], g["iy1"], g["ih"]
        px = self._rate_to_x(st["rate"], ix0, ix1)
        py = iy1 - _clamp(st["depth"] * 0.01, 0.0, 1.0) * ih
        self.handle_pos["puck"] = (px, py)

        ui = self._ui
        cv.coords(ui["puck_vx"], px, iy0, px, iy1)
        cv.coords(ui["puck_hy"], ix0, py, ix1, py)
        cv.coords(ui["puck_out"], px - 9, py - 9, px + 9, py + 9)
        cv.coords(ui["puck_in"], px - 2.5, py - 2.5, px + 2.5, py + 2.5)
        cv.itemconfig(ui["puck_out"], fill=p_col, outline=c_hi)

    def _update_scope_texts(self, cv, st, byp, bpm, env_lvl, mode_name, shape_name):
        ui = self._ui
        c_hi = BYP_HI if byp else LABEL_HI

        bpm_i = int(round(bpm)) if st["sync"] else 0
        hkey = (mode_name, shape_name, st["sync"], round(st["rate"], 3), bpm_i,
                round(st["depth"], 0), round(st["feedback"], 0), byp)
        if self._cache.get("sc_title") != hkey:
            self._cache["sc_title"] = hkey
            if st["sync"]:
                div_lbl, _ = nearest_rate_division(st["rate"], bpm)
                rate_txt = f"{div_lbl} ({bpm:.0f})"
            else:
                rate_txt = _fmt_rate(st["rate"])
            title = f"{mode_name} · {shape_name} {rate_txt} · D {st['depth']:.0f}% · FB {st['feedback']:+.0f}%"
            cv.itemconfig(ui["sc_title"], text=title, fill=c_hi)

        bkey = (byp, st["lfo_freeze"], round(st["overmod"], 0))
        if self._cache.get("sc_badges") != bkey:
            self._cache["sc_badges"] = bkey
            badges = []
            if byp:
                badges.append(("BYPASS", BYP_HI))
            else:
                if st["lfo_freeze"]:
                    badges.append(("FREEZE", AMBER_COL))
                if st["overmod"] >= 20.0:
                    badges.append((f"UBER {st['overmod']:.0f}%", self._uber_color(st["overmod"], byp)))
            bx = self._geom["gx1"] - 8
            y_b = self._geom["gy0"] + 6
            for idx, bid in enumerate((ui["sc_b0"], ui["sc_b1"])):
                if idx < len(badges):
                    txt, col = badges[idx]
                    cv.coords(bid, bx, y_b)
                    cv.itemconfig(bid, text=txt, fill=col, state="normal")
                    bx -= len(txt) * 7 + 10
                else:
                    cv.itemconfig(bid, state="hidden")

        fkey = (round(st["delay_ms"], 1), round(st["spread"], 0), round(st["diffuse"], 0),
                round(st["mix"], 0), round(st["stereo"], 0), byp)
        if self._cache.get("sc_foot") != fkey:
            self._cache["sc_foot"] = fkey
            foot = (f"DEL {st['delay_ms']:.1f}ms · SPR {st['spread']:.0f}% · "
                    f"DIFF {st['diffuse']:.0f}% · MIX {st['mix']:.0f}% · ST {st['stereo']:.0f}°")
            cv.itemconfig(ui["sc_foot"], text=foot, fill=BYP_MID if byp else LABEL_DIM)

        show_env = (env_lvl > 0.05 and abs(st["env_mod"]) > 1.0)
        ekey = (show_env, round(st["env_mod"], 0) if show_env else 0.0, byp)
        if self._cache.get("sc_env") != ekey:
            self._cache["sc_env"] = ekey
            if show_env:
                cv.itemconfig(ui["sc_env"], text=f"ENV {st['env_mod']:+.0f}%",
                              fill=BYP_MID if byp else RIGHT_COL, state="normal")
            else:
                cv.itemconfig(ui["sc_env"], state="hidden")

    def _update_tone_uber_strip(self, cv, st, byp):
        ui = self._ui
        tkey = (round(st["low_cut"], 1), round(st["high_cut"], 1), round(st["drive"], 0), byp)
        if self._cache.get("tone") != tkey:
            self._cache["tone"] = tkey
            tx0, ty0, tx1, ty1 = self.tone_box
            lx = self._freq_to_x(st["low_cut"], tx0 + 8, tx1 - 8)
            hx = self._freq_to_x(st["high_cut"], tx0 + 8, tx1 - 8)
            self.handle_pos["lc"] = (lx, (ty0 + ty1) * 0.5)
            self.handle_pos["hc"] = (hx, (ty0 + ty1) * 0.5)

            cv.coords(ui["t_band"], lx, ty0 + 3, hx, ty1 - 3)
            cv.itemconfig(ui["t_band"], fill=BYP_FILL if byp else BAND_FILL)
            cv.coords(ui["t_lc_l"], lx, ty0 + 2, lx, ty1 - 2)
            cv.itemconfig(ui["t_lc_l"], fill=BYP_MID if byp else LEFT_COL)
            cv.coords(ui["t_hc_l"], hx, ty0 + 2, hx, ty1 - 2)
            cv.itemconfig(ui["t_hc_l"], fill=BYP_MID if byp else ORANGE_COL)
            c_hi = BYP_HI if byp else LABEL_HI
            cv.itemconfig(ui["t_lc_t"], text=f"LC {_fmt_freq(st['low_cut'])}", fill=c_hi)
            cv.itemconfig(ui["t_drv_t"], text=f"DRV {st['drive']:.0f}%", fill=BYP_MID if byp else LABEL_DIM)
            cv.itemconfig(ui["t_hc_t"], text=f"HC {_fmt_freq(st['high_cut'])}", fill=c_hi)

        okey = (round(st["overmod"], 1), byp)
        if self._cache.get("over") != okey:
            self._cache["over"] = okey
            ox0, oy0, ox1, oy1 = self.over_box
            ov = _clamp(st["overmod"] * 0.01, 0.0, 1.0)
            ov_col = self._uber_color(st["overmod"], byp)
            vx = ox0 + 2 + ov * (ox1 - ox0 - 4)
            if ov > 0.01:
                cv.coords(ui["o_fill"], ox0 + 2, oy0 + 3, vx, oy1 - 3)
                cv.itemconfig(ui["o_fill"], fill=BYP_FILL if byp else ov_col, state="normal")
            else:
                cv.itemconfig(ui["o_fill"], state="hidden")
            cv.coords(ui["o_line"], vx, oy0 + 2, vx, oy1 - 2)
            cv.itemconfig(ui["o_line"], fill=BYP_HI if byp else LABEL_HI)
            cv.itemconfig(ui["o_txt"], text=f"OVERMOD {st['overmod']:.0f}%",
                          fill=BYP_HI if byp else ("#101318" if ov > 0.35 else LABEL_HI))

    def _update_meters(self, cv, byp, in_db, out_db, eff_mod, fb_en):
        ui = self._ui
        g = self._geom
        bot = g["m_bot"]
        mh = g["m_h"]
        meters = ui["meters"]

        if self._cache.get("m_byp") != byp:
            self._cache["m_byp"] = byp
            c_dim = BYP_MID if byp else LABEL_DIM
            c_hi = BYP_HI if byp else LABEL_HI
            cv.itemconfig(ui["m_title"], fill=c_dim)
            for _bx0, _bx1, bar_id, hold_id, lbl_id, col in meters:
                cv.itemconfig(bar_id, fill=BYP_MID if byp else col)
                cv.itemconfig(hold_id, fill=c_hi)
                cv.itemconfig(lbl_id, fill=c_dim)

        vals = (
            (_clamp((in_db - FLOOR_DB) / 72.0, 0.0, 1.0),
             _clamp((self.hold_in - FLOOR_DB) / 72.0, 0.0, 1.0)),
            (_clamp(eff_mod, 0.0, 1.0), _clamp(self.hold_mod, 0.0, 1.0)),
            (_clamp(fb_en, 0.0, 1.0),   _clamp(self.hold_fb, 0.0, 1.0)),
            (_clamp((out_db - FLOOR_DB) / 72.0, 0.0, 1.0),
             _clamp((self.hold_out - FLOOR_DB) / 72.0, 0.0, 1.0)),
        )
        prev = self._cache.get("m_px")
        if prev is None:
            prev = [None, None, None, None]
            self._cache["m_px"] = prev

        for i, ((val, hold), (bx0, bx1, bar_id, hold_id, _lbl_id, _col)) in enumerate(zip(vals, meters)):
            fy_px = int(round(bot - val * mh)) if val > 0.01 else None
            hy_px = int(round(bot - hold * mh))
            p = prev[i]
            if p is None or p[0] != fy_px:
                if fy_px is not None:
                    cv.coords(bar_id, bx0 + 1, fy_px, bx1 - 1, bot - 1)
                    if p is None or p[0] is None:
                        cv.itemconfig(bar_id, state="normal")
                elif p is not None and p[0] is not None:
                    cv.itemconfig(bar_id, state="hidden")
            if p is None or p[1] != hy_px:
                cv.coords(hold_id, bx0, hy_px, bx1, hy_px)
            prev[i] = (fy_px, hy_px)

    def _update_buttons(self, cv, specs, byp):
        prev = self._cache.get("btns")
        if prev is None:
            prev = [None] * len(specs)
            self._cache["btns"] = prev
        slots = self._ui["btns"]
        for i, (t, s, act, col) in enumerate(specs):
            skey = (t, s, act, col, byp)
            if prev[i] == skey:
                continue
            prev[i] = skey
            r_id, t_id, s_id = slots[i]
            fill = (BYP_FILL if byp else col) if act else BUTTON_BG
            out = BYP_MID if byp else (col if act else LINE_DIM)
            fg_t = (BYP_HI if byp else "#101318") if act else (BYP_HI if byp else LABEL_HI)
            fg_s = (BYP_MID if byp else "#16191e") if act else (BYP_MID if byp else col)
            cv.itemconfig(r_id, fill=fill, outline=out)
            cv.itemconfig(t_id, text=t, fill=fg_t)
            cv.itemconfig(s_id, text=s, fill=fg_s)

    def _update_main_buttons(self, cv, st, byp, mode_name, mode_col, shape_name, shape_col):
        ov_pct = st["overmod"]
        uber_sub = "BYP" if byp else f"{ov_pct:.0f}%"
        uber_col = self._uber_color(ov_pct, byp) if ov_pct > 2.0 else UBER_COL
        specs = (
            ("MODE",   mode_name,                      False, mode_col),
            ("SHAPE",  shape_name,                     False, shape_col),
            ("SYNC",   "ON" if st["sync"] else "FREE", st["sync"], RIGHT_COL),
            ("LIVE",   "FX PAD",                       False, UBER_COL),
            ("LFO",    "FREEZE" if st["lfo_freeze"] else "RUN", st["lfo_freeze"], AMBER_COL),
            ("UBER*",  uber_sub,                       ov_pct >= 25.0 or byp, uber_col),
        )
        self._update_buttons(cv, specs, byp)

    def _update_fx_buttons(self, cv, st, byp, mode_name, mode_col):
        has_fx = (abs(st["rate_fx"]) > 0.5 or st["warp_fx"] > 0.5 or abs(st["comb_fx"]) > 0.5)
        specs = (
            ("VIEW",   "MAIN",                               True,  LEFT_COL),
            ("SPRING", "SNAP 0" if self.fx_snap else "HOLD", self.fx_snap, RIGHT_COL),
            ("RESET",  "ZERO 0",                             has_fx, HOT_RED),
            ("LFO",    "FREEZE" if st["lfo_freeze"] else "RUN", st["lfo_freeze"], AMBER_COL),
            ("MODE",   mode_name,                            False, mode_col),
            ("UBER*",  "BYP" if byp else f"{st['overmod']:.0f}%", byp or st["overmod"] >= 25.0, UBER_COL),
        )
        self._update_buttons(cv, specs, byp)

    def _update_fx_screen(self, cv, st, byp, bpm, fb_en, mode_name):
        ui = self._ui
        c_hi = BYP_HI if byp else LABEL_HI
        c_fx = BYP_MID if byp else UBER_COL

        if self._cache.get("fx_byp") != byp:
            self._cache["fx_byp"] = byp
            c_dim = BYP_MID if byp else LABEL_DIM
            cv.itemconfig(ui["fx_lbl_l"], fill=c_dim)
            cv.itemconfig(ui["fx_lbl_r"], fill=c_dim)
            cv.itemconfig(ui["fx_lbl_t"], fill=c_dim)
            cv.itemconfig(ui["fx_tether"], fill=c_fx)
            cv.itemconfig(ui["fx_glow"], outline=c_fx)
            cv.itemconfig(ui["fx_puck"], fill=c_fx, outline=c_hi)

        tkey = (mode_name, int(round(bpm)), byp)
        if self._cache.get("fx_title") != tkey:
            self._cache["fx_title"] = tkey
            cv.itemconfig(ui["fx_title"], text=f"LIVE FX · {mode_name} · BPM {bpm:.0f}", fill=c_hi)

        cfx = st["comb_fx"]
        wfx = st["warp_fx"]
        xy_key = (round(cfx, 1), round(wfx, 1), round(fb_en, 2), byp)
        if self._cache.get("fx_xy") != xy_key:
            self._cache["fx_xy"] = xy_key
            px0, py0, px1, py1 = self.xy_box
            cx = (px0 + px1) * 0.5
            px = px0 + _clamp((cfx + 100.0) * 0.005, 0.0, 1.0) * (px1 - px0)
            py = py1 - _clamp(wfx * 0.01, 0.0, 1.0) * (py1 - py0)
            if abs(px - cx) > 2.0 or abs(py - py1) > 2.0:
                cv.coords(ui["fx_tether"], cx, py1, px, py)
                cv.itemconfig(ui["fx_tether"], state="normal")
            else:
                cv.itemconfig(ui["fx_tether"], state="hidden")
            glow_r = 14.0 + 16.0 * (wfx * 0.01) * (0.5 + 0.5 * fb_en)
            cv.coords(ui["fx_glow"], px - glow_r, py - glow_r, px + glow_r, py + glow_r)
            cv.coords(ui["fx_puck"], px - 11, py - 11, px + 11, py + 11)
            cv.coords(ui["fx_dot"], px - 3, py - 3, px + 3, py + 3)
            cv.itemconfig(ui["fx_xy_txt"], text=f"COMB FX {cfx:+.0f}% · WARP FX {wfx:.0f}%", fill=c_hi)

        sp_key = (self.fx_snap, byp)
        if self._cache.get("fx_spring") != sp_key:
            self._cache["fx_spring"] = sp_key
            if self.fx_snap:
                cv.itemconfig(ui["fx_spring"], fill=BYP_MID if byp else RIGHT_COL, state="normal")
            else:
                cv.itemconfig(ui["fx_spring"], state="hidden")

        rfx = st["rate_fx"]
        rkey = (round(rfx, 1), byp)
        if self._cache.get("fx_rfx") != rkey:
            self._cache["fx_rfx"] = rkey
            sx0, sy0, sx1, sy1, mid_x = self._geom["fx_slider"]
            c_act = BYP_MID if byp else LEFT_COL
            vx = sx0 + _clamp((rfx + 100.0) * 0.005, 0.0, 1.0) * (sx1 - sx0)
            if abs(rfx) > 0.5:
                cv.coords(ui["fx_rfx_fill"], min(mid_x, vx), sy0 + 2, max(mid_x, vx), sy1 - 2)
                cv.itemconfig(ui["fx_rfx_fill"], fill=BYP_FILL if byp else c_act, state="normal")
            else:
                cv.itemconfig(ui["fx_rfx_fill"], state="hidden")
            cv.coords(ui["fx_rfx_line"], vx, sy0 + 2, vx, sy1 - 2)
            cv.itemconfig(ui["fx_rfx_txt"], text=f"RATE FX (SPEED BEND)  {rfx:+.0f}%")
            active_z = (abs(rfx) >= 0.75) and not byp
            cv.itemconfig(ui["fx_z_rect"], fill=c_act if active_z else BUTTON_BG, outline=c_act)
            cv.itemconfig(ui["fx_z_txt"], fill="#101318" if active_z else LABEL_HI)

    # ---------- touch / pointer interaction ----------

    def _apply_scope_drag(self, x, y):
        gx0, gy0, gx1, gy1 = self.scope_box
        ix0, ix1 = gx0 + 12, gx1 - 12
        iy0, iy1 = gy0 + 26, gy1 - 24
        rate = self._x_to_rate(x, ix0, ix1)
        depth = _clamp(((iy1 - y) / max(1.0, iy1 - iy0)) * 100.0, 0.0, 100.0)
        c1 = self._set("rate", round(rate, 3))
        c2 = self._set("depth", round(depth, 1))
        return c1 or c2

    def _apply_tone_drag(self, which, x):
        tx0, _ty0, tx1, _ty1 = self.tone_box
        f = self._x_to_freq(x, tx0 + 8, tx1 - 8)
        if which == "lc":
            return self._set("low_cut", round(_clamp(f, F_MIN, LOW_CUT_MAX), 1))
        return self._set("high_cut", round(_clamp(f, HIGH_CUT_MIN, F_MAX), 1))

    def _apply_over_drag(self, x):
        ox0, _oy0, ox1, _oy1 = self.over_box
        t = _clamp((x - ox0) / max(1.0, ox1 - ox0), 0.0, 1.0)
        v = t * 100.0
        if v < 2.5:
            v = 0.0
        return self._set("overmod", round(v, 1))

    def _apply_xy_drag(self, x, y):
        px0, py0, px1, py1 = self.xy_box
        tx = _clamp((x - px0) / max(1.0, px1 - px0), 0.0, 1.0)
        ty = _clamp((y - py0) / max(1.0, py1 - py0), 0.0, 1.0)
        cfx = (tx * 2.0 - 1.0) * 100.0
        wfx = (1.0 - ty) * 100.0
        if abs(cfx) < 4.0:
            cfx = 0.0
        if wfx < 3.0:
            wfx = 0.0
        c1 = self._set("comb_fx", round(_clamp(cfx, -100.0, 100.0), 1))
        c2 = self._set("warp_fx", round(_clamp(wfx, 0.0, 100.0), 1))
        return c1 or c2

    def _apply_fx_drag(self, sym, x):
        info = self.fx_sliders.get(sym)
        if not info:
            return False
        sx0, _sy0, sx1, _sy1, bipolar = info
        t = _clamp((x - sx0) / max(1.0, sx1 - sx0), 0.0, 1.0)
        if bipolar:
            v = (t * 2.0 - 1.0) * 100.0
            if abs(v) < 3.0:
                v = 0.0
            return self._set(sym, round(_clamp(v, -100.0, 100.0), 1))
        return self._set(sym, round(_clamp(t * 100.0, 0.0, 100.0), 1))

    def _apply_drag_kind(self, x, y):
        kind = self.drag_mode[0]
        if kind == "scope":
            return self._apply_scope_drag(x, y)
        if kind == "tone":
            return self._apply_tone_drag(self.drag_mode[1], x)
        if kind == "over":
            return self._apply_over_drag(x)
        if kind == "xy":
            return self._apply_xy_drag(x, y)
        if kind == "fx":
            return self._apply_fx_drag(self.drag_mode[1], x)
        return False

    def _on_press(self, event):
        x, y = event.x, event.y
        now = time.monotonic()
        self.drag_mode = None
        self._pending_drag = None
        self._last_drag_ts = now

        # 1. Bottom buttons + FX reset buttons
        for key, (x0, y0, x1, y1) in self.hit_rects.items():
            if x0 <= x <= x1 and y0 <= y <= y1:
                if key == "fx":
                    self.fx_view = not self.fx_view
                    if self.fx_view:
                        self._select_group("g_fx", "Live FX")
                    else:
                        self._select_group("g_engine", "Engine")
                    self.refresh_gui(sample_telemetry=False)
                    return
                if key == "zero":
                    self._set("rate_fx", 0.0)
                    self._set("warp_fx", 0.0)
                    self._set("comb_fx", 0.0)
                    self._select_group("g_fx", "Live FX")
                    self.refresh_gui(sample_telemetry=False)
                    return
                if key.startswith("z_"):
                    self._set(key[2:], 0.0)
                    self._select_group("g_fx", "Live FX")
                    self.refresh_gui(sample_telemetry=False)
                    return
                if key == "mode":
                    cur = int(round(self._get("mode", 0.0)))
                    self._set("mode", (cur + 1) % len(MODES))
                    self._select_group("g_engine", "Engine")
                    self.refresh_gui(sample_telemetry=False)
                    return
                if key == "shape":
                    cur = int(round(self._get("shape", 0.0)))
                    self._set("shape", (cur + 1) % len(SHAPES))
                    self._select_group("g_lfo", "LFO")
                    self.refresh_gui(sample_telemetry=False)
                    return
                if key == "sync":
                    cur = self._get("sync", 0.0)
                    self._set("sync", 0 if cur >= 0.5 else 1)
                    self._select_group("g_lfo", "LFO")
                    self.refresh_gui(sample_telemetry=False)
                    return
                if key == "snap":
                    self.fx_snap = not self.fx_snap
                    self._select_group("g_fx", "Live FX")
                    self.refresh_gui(sample_telemetry=False)
                    return
                if key == "freeze":
                    cur = self._get("lfo_freeze", 0.0)
                    self._set("lfo_freeze", 0 if cur >= 0.5 else 1)
                    self._select_group("g_master", "Master")
                    self.refresh_gui(sample_telemetry=False)
                    return
                if key == "uber":
                    self.drag_mode = ("uber", now)
                    self._select_group("g_uber", "Uber")
                    return

        # 2. Dedicated Live FX screen
        if self.fx_view:
            px0, py0, px1, py1 = self.xy_box
            if px0 <= x <= px1 and py0 <= y <= py1:
                self.drag_mode = ("xy",)
                self._select_group("g_fx", "Live FX")
                self._apply_xy_drag(x, y)
                self.refresh_gui(sample_telemetry=False)
                return
            for sym, (sx0, sy0, sx1, sy1, _bip) in self.fx_sliders.items():
                if sx0 <= x <= sx1 and sy0 <= y <= sy1:
                    self.drag_mode = ("fx", sym)
                    self._select_group("g_fx", "Live FX")
                    self._apply_fx_drag(sym, x)
                    self.refresh_gui(sample_telemetry=False)
                    return
            return

        # 3. Tone passband strip
        tx0, ty0, tx1, ty1 = self.tone_box
        if tx0 <= x <= tx1 and ty0 <= y <= ty1:
            lx = self.handle_pos.get("lc", (tx0, 0))[0]
            hx = self.handle_pos.get("hc", (tx1, 0))[0]
            which = "lc" if abs(x - lx) <= abs(x - hx) else "hc"
            self.drag_mode = ("tone", which)
            self._select_group("g_tone", "Tone")
            self._apply_tone_drag(which, x)
            self.refresh_gui(sample_telemetry=False)
            return

        # 4. OverMod bar
        ox0, oy0, ox1, oy1 = self.over_box
        if ox0 <= x <= ox1 and oy0 <= y <= oy1:
            self.drag_mode = ("over",)
            self._select_group("g_uber", "Uber")
            self._apply_over_drag(x)
            self.refresh_gui(sample_telemetry=False)
            return

        # 5. Main Scope (Rate x Depth; double-tap resets OverMod & FX)
        gx0, gy0, gx1, gy1 = self.scope_box
        if gx0 <= x <= gx1 and gy0 <= y <= gy1:
            if (now - self._last_scope_tap) <= DOUBLE_TAP_SEC:
                self._last_scope_tap = 0.0
                self._set("overmod", 0.0)
                self._set("warp_fx", 0.0)
                self._set("comb_fx", 0.0)
                self._select_group("g_uber", "Uber")
                self.refresh_gui(sample_telemetry=False)
                return
            self._last_scope_tap = now
            self.drag_mode = ("scope",)
            self._select_group("g_lfo", "LFO")
            self._apply_scope_drag(x, y)
            self.refresh_gui(sample_telemetry=False)

    def _on_motion(self, event):
        if not self.drag_mode or self.drag_mode[0] == "uber":
            return
        now = time.monotonic()
        if (now - self._last_drag_ts) < DRAG_INTERVAL:
            self._pending_drag = (event.x, event.y)
            return
        self._last_drag_ts = now
        self._pending_drag = None
        if self._apply_drag_kind(event.x, event.y):
            self.refresh_gui(sample_telemetry=False)

    def _on_release(self, event):
        if self.drag_mode:
            kind = self.drag_mode[0]
            if kind == "uber":
                _, t0 = self.drag_mode
                if (time.monotonic() - t0) >= LONG_PRESS_SEC:
                    cur_byp = self._get("bypass", 0.0)
                    self._set("bypass", 0 if cur_byp >= 0.5 else 1)
                    self._select_group("g_master", "Master")
                else:
                    cur_ov = self._get("overmod", 0.0)
                    if cur_ov < 20.0:
                        nxt = 45.0
                    elif cur_ov < 65.0:
                        nxt = 85.0
                    else:
                        nxt = 0.0
                    self._set("overmod", nxt)
                self.refresh_gui(sample_telemetry=False)
            elif kind == "xy" and self.fx_snap:
                self._set("comb_fx", 0.0)
                self._set("warp_fx", 0.0)
                self.refresh_gui(sample_telemetry=False)
            elif kind == "fx" and self.fx_snap:
                self._set(self.drag_mode[1], 0.0)
                self.refresh_gui(sample_telemetry=False)
            elif self._pending_drag is not None:
                px, py = self._pending_drag
                if self._apply_drag_kind(px, py):
                    self.refresh_gui(sample_telemetry=False)
        self._pending_drag = None
        self.drag_mode = None
