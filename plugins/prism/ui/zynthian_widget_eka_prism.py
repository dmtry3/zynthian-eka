#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# SPDX-License-Identifier: GPL-3.0-or-later
# Zynthian widget for Eka Prism (class and file name: zynthian_widget_eka_prism).
# Retained-mode Tkinter Canvas: static chrome is created once per layout/view,
# and dynamic elements are moved/reconfigured in-place only when values change.
from collections import deque
import math
import time
import tkinter

from zyngui import zynthian_gui_config
from zyngui import zynthian_widget_base

MULTI_INSTANCE = True

REFRESH_SEC = 0.09
KNOB_ACTIVE_HOLD_SEC = 0.30
KNOB_MONITOR_SEC = 0.25
DRAG_INTERVAL = 0.033
CURVE_BINS = 56
TRAIL_STEPS = 28

PARAM_KEYS = (
    "model_a", "cutoff_a", "res_a", "morph_a",
    "model_b", "cutoff_b", "res_b", "morph_b",
    "routing", "link_mode", "ab_balance", "invert_b",
    "lfo_shape", "lfo_rate", "lfo_sync", "lfo_phase",
    "lfo_mod", "morph_mod", "env_mod", "env_speed",
    "drive", "mix", "output_db", "bypass",
)

F_MIN = 20.0
F_MAX = 18000.0
RATE_MIN = 0.05
RATE_MAX = 40.0
DB_SPAN = 26.0
FLOOR_DB = -60.0
LONG_PRESS_SEC = 0.45

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

MODELS = (
    ("SVF",     "#ffd43b"),
    ("LADDER",  "#ffa94d"),
    ("MS-20",   "#ff6b6b"),
    ("COMB",    "#63e6be"),
    ("FORMANT", "#da77f2"),
    ("PHASER",  "#4dabf7"),
)

ROUTINGS = ("SERIAL A>B", "PARALLEL A+B", "STEREO L/R")
LINKS = ("FREE", "OFFSET", "MIRROR")
LFO_SHAPES = ("SINE", "TRI", "SAW", "SQR", "S&H", "CHAOS", "STEP8")

VOWEL_FREQS = (
    (730.0, 1090.0, 2440.0),
    (530.0, 1840.0, 2480.0),
    (270.0, 2290.0, 3010.0),
    (570.0,  840.0, 2410.0),
    (300.0,  870.0, 2240.0),
)

PANEL_BG   = "#16191e"
BUTTON_BG  = "#1f242c"
LINE_DIM   = "#2f3640"
GRID_COL   = "#252b34"
ZERO_COL   = "#3a4352"
LABEL_DIM  = "#8b949e"
LABEL_HI   = "#e6edf3"
COL_A      = "#ffd43b"
COL_B      = "#4dabf7"
COL_SUM    = "#f0f6fc"
COL_ENV    = "#63e6be"
COL_MOD    = "#da77f2"
COL_DRV    = "#ffa94d"
HOT_RED    = "#ff6b6b"
FILL_SUM   = "#1e2938"

BYP_DIM    = "#49505a"
BYP_MID    = "#68707c"
BYP_HI     = "#9aa2ae"
BYP_FILL   = "#23272e"

_LOG_F_SPAN = math.log(F_MAX / F_MIN)
_LOG_R_SPAN = math.log(RATE_MAX / RATE_MIN)
_CURVE_US = tuple(i / float(CURVE_BINS) for i in range(CURVE_BINS + 1))
_CURVE_FREQS = tuple(F_MIN * math.exp(u * _LOG_F_SPAN) for u in _CURVE_US)


def _clamp(v, lo, hi):
    return lo if v < lo else (hi if v > hi else v)


def _fmt_freq(f):
    if f >= 1000.0:
        return f"{f / 1000.0:.2f}k" if f < 10000.0 else f"{f / 1000.0:.1f}k"
    return f"{f:.0f}Hz"


def _fmt_rate(hz):
    if hz >= 10.0:
        return f"{hz:.1f}Hz"
    return f"{hz:.2f}Hz"


def nearest_rate_division(hz, bpm):
    bpm = _clamp(bpm, 20.0, 400.0)
    q_sec = 60.0 / bpm
    log_t = math.log(_clamp(hz, RATE_MIN, RATE_MAX))
    best_lbl, best_hz, best_err = "1/4", hz, 1e9
    for lbl, mult in RATE_DIVISIONS:
        cand = 1.0 / (q_sec * mult)
        if cand < 0.03 or cand > 45.0:
            continue
        err = abs(math.log(cand) - log_t)
        if err < best_err:
            best_err = err
            best_lbl = lbl
            best_hz = cand
    return best_lbl, best_hz


def _eval_model_db(model, f_arr, fc, res_pct, morph_pct):
    """Fast analytic frequency response magnitude in dB for each of the 6 filter models."""
    fc = _clamp(fc, F_MIN, F_MAX)
    res = _clamp(res_pct, 0.0, 100.0) * 0.01
    morph = _clamp(morph_pct, 0.0, 100.0) * 0.01
    q = 0.65 + 14.0 * (res ** 1.6)
    out = []

    if model == 0:  # SVF Morph (LP -> BP -> HP -> Notch)
        m3 = morph * 3.0
        for f in f_arr:
            r = f / fc
            r2 = r * r
            den = math.sqrt((1.0 - r2) ** 2 + (r / q) ** 2) + 1e-6
            lp = 1.0 / den
            bp = (r / max(q * 0.45, 0.7)) / den
            hp = r2 / den
            notch = abs(1.0 - r2) / den
            if m3 <= 1.0:
                mag = lp * (1.0 - m3) + bp * m3
            elif m3 <= 2.0:
                t = m3 - 1.0
                mag = bp * (1.0 - t) + hp * t
            else:
                t = m3 - 2.0
                mag = hp * (1.0 - t) + notch * t
            out.append(_clamp(20.0 * math.log10(max(mag, 0.025)), -DB_SPAN, DB_SPAN))
        return out

    if model == 1:  # Ladder 24 dB (LP24 -> LP12 -> BP4 -> HP2)
        m3 = morph * 3.0
        peak_q = 0.6 + 16.0 * (res ** 1.5)
        for f in f_arr:
            r = f / fc
            r2 = r * r
            r4 = r2 * r2
            res_bump = (1.0 + 3.2 * res) / (math.sqrt((1.0 - r2) ** 2 + (r / peak_q) ** 2) + 1e-5)
            lp24 = (1.0 / math.sqrt(1.0 + r4)) * (0.55 + 0.45 * res_bump)
            lp12 = (1.0 / math.sqrt(1.0 + r2)) * (0.60 + 0.40 * res_bump)
            bp = (r2 / (1.0 + r4)) * (1.2 + 0.6 * res_bump)
            hp = (r2 / math.sqrt(1.0 + r4)) * (0.65 + 0.35 * res_bump)
            if m3 <= 1.0:
                mag = lp24 * (1.0 - m3) + lp12 * m3
            elif m3 <= 2.0:
                t = m3 - 1.0
                mag = lp12 * (1.0 - t) + bp * t
            else:
                t = m3 - 2.0
                mag = bp * (1.0 - t) + hp * t
            out.append(_clamp(20.0 * math.log10(max(mag, 0.025)), -DB_SPAN, DB_SPAN))
        return out

    if model == 2:  # MS-20 Screamer (aggressive peak + asymmetric pre-resonance)
        m2 = morph * 2.0
        in_lp = _clamp(1.0 - m2, 0.0, 1.0)
        in_bp = 1.0 - abs(m2 - 1.0)
        in_hp = _clamp(m2 - 1.0, 0.0, 1.0)
        sq = 0.7 + 18.0 * (res ** 1.35)
        for f in f_arr:
            r = f / fc
            r2 = r * r
            den = math.sqrt((1.0 - r2) ** 2 + (r / sq) ** 2) + 1e-6
            # Sub-octave growl bump typical of MS-20
            growl = 1.0 + 0.35 * res * math.exp(-((r - 0.72) ** 2) * 14.0)
            mag = ((in_lp + in_bp * r * 1.15 + in_hp * r2) / den) * growl
            out.append(_clamp(20.0 * math.log10(max(mag, 0.025)), -DB_SPAN, DB_SPAN))
        return out

    if model == 3:  # Comb Resonator
        pol = (morph - 0.5) * 2.0
        sign = 1.0 if pol >= 0.0 else -1.0
        fb = sign * (0.25 + 0.68 * (res ** 0.65))
        fc_comb = _clamp(fc, 30.0, 4000.0)
        for f in f_arr:
            # Cap visual comb density at high frequencies so curve stays readable
            phase = 2.0 * math.pi * min(f / fc_comb, 14.0)
            c_ph = math.cos(phase)
            s_ph = math.sin(phase)
            den = math.sqrt((1.0 - fb * c_ph) ** 2 + (fb * s_ph) ** 2) + 1e-5
            mag = 0.45 + 0.55 / den
            out.append(_clamp(20.0 * math.log10(max(mag, 0.025)), -DB_SPAN, DB_SPAN))
        return out

    if model == 4:  # Formant Vowel A-E-I-O-U
        vpos = morph * 4.0
        v0 = min(int(vpos), 3)
        v1 = v0 + 1
        vf = vpos - v0
        shift = _clamp((fc / 1000.0) ** 0.55, 0.35, 2.8)
        fq = 3.2 + 14.0 * (res ** 1.3)
        peaks = [
            (VOWEL_FREQS[v0][b] * (1.0 - vf) + VOWEL_FREQS[v1][b] * vf) * shift
            for b in range(3)
        ]
        gains = (1.15, 0.90, 0.65)
        for f in f_arr:
            mag = 0.14
            for b in range(3):
                r = f / max(peaks[b], 40.0)
                r2 = r * r
                den = math.sqrt((1.0 - r2) ** 2 + (r / fq) ** 2) + 1e-5
                mag += gains[b] * (r / max(fq * 0.35, 0.8)) / den
            out.append(_clamp(20.0 * math.log10(max(mag, 0.025)), -DB_SPAN, DB_SPAN))
        return out

    # Model 5: 6-Stage Phaser
    spread = 0.65 + 0.75 * abs(morph - 0.5) * 2.0
    depth = 0.45 + 0.50 * res
    notches = (fc * (spread ** -0.9), fc, fc * (spread ** 0.9))
    for f in f_arr:
        mag = 1.0 + 0.35 * res
        for nf in notches:
            dist = math.log(max(f, 20.0) / max(nf, 20.0))
            if morph < 0.5:
                mag *= 1.0 - depth * math.exp(-(dist * dist) * 18.0)
            else:
                mag += depth * 1.15 * math.exp(-(dist * dist) * 18.0)
        out.append(_clamp(20.0 * math.log10(max(mag, 0.025)), -DB_SPAN, DB_SPAN))
    return out


class zynthian_widget_eka_prism(zynthian_widget_base.zynthian_widget_base):
    def __init__(self, parent):
        super().__init__(parent)
        self._last_monitor_ts = 0.0
        self._last_param_ts = 0.0
        self._last_update_params = None
        self.trail = deque(maxlen=TRAIL_STEPS)
        self._last_trail_pt = None

        self.hold_in = FLOOR_DB
        self.hold_out = FLOOR_DB
        self._telemetry = (FLOOR_DB, FLOOR_DB, 650.0, 1800.0, 0.0, 25.0, 0.0, 0.0, 0.0, 120.0)

        self.graph_box = (0.0, 0.0, 1.0, 1.0)
        self.morph_a_box = (0.0, 0.0, 1.0, 1.0)
        self.morph_b_box = (0.0, 0.0, 1.0, 1.0)
        self.mod_bars = {}
        self.xy_box = (0.0, 0.0, 1.0, 1.0)
        self.fx_sliders = {}
        self.fx_view = False
        self.fx_snap = False
        self._snap_origin = {}
        self.node_pos = {}
        self.hit_rects = {}
        self._last_ctrl_titles = {}
        self.drag_mode = None
        self._last_drag_ts = 0.0
        self._pending_drag = None

        self._scene_key = None
        self._geom = {}
        self._ui = {}
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

    def _get_mon(self, sym, fallback):
        if not isinstance(self.monitors, dict):
            return fallback
        val = self.monitors.get(sym)
        if val is None:
            return fallback
        try:
            v = float(val)
            return v if math.isfinite(v) else fallback
        except (TypeError, ValueError):
            return fallback

    def _set(self, sym, val):
        if not self.processor:
            return
        zctrl = self.processor.controllers_dict.get(sym)
        if zctrl is None:
            return
        if hasattr(zctrl, "set_value"):
            zctrl.set_value(val)
        else:
            zctrl.value = val

    def _select_group(self, group_sym):
        ctrl_screen = getattr(self, "zyngui_control", None)
        if ctrl_screen is None:
            return
        list_data = getattr(ctrl_screen, "list_data", None)
        if not list_data:
            return
        for idx, row in enumerate(list_data):
            if row and row[0] == group_sym:
                if getattr(ctrl_screen, "selected_index", -1) != idx and hasattr(ctrl_screen, "select"):
                    ctrl_screen.select(idx)
                return

    def _sync_encoder_titles(self, sync_on, div_lbl):
        ctrl_screen = getattr(self, "zyngui_control", None)
        if ctrl_screen is None:
            return
        zctrls = getattr(ctrl_screen, "zgui_controllers", None)
        if not zctrls:
            return
        target = f"Rate {div_lbl}" if sync_on else "Rate"
        for zgui in zctrls:
            zc = getattr(zgui, "zctrl", None)
            if zc is not None and getattr(zc, "short_name", "") in ("Rate", "lfo_rate") or (
                self.processor and zc is self.processor.controllers_dict.get("lfo_rate")
            ):
                if self._last_ctrl_titles.get("lfo_rate") != target and hasattr(zgui, "set_title"):
                    zgui.set_title(target)
                    self._last_ctrl_titles["lfo_rate"] = target

    def _cfg(self, key, **kwargs):
        iid = self._ui.get(key)
        if iid is None:
            return
        prev = self._cache.get(key)
        if prev == kwargs:
            return
        if prev is None:
            self._cache[key] = dict(kwargs)
            self.widget_canvas.itemconfig(iid, **kwargs)
        else:
            diff = {k: v for k, v in kwargs.items() if prev.get(k) != v}
            if diff:
                prev.update(diff)
                self.widget_canvas.itemconfig(iid, **diff)

    def _coords(self, key, *pts):
        iid = self._ui.get(key)
        if iid is None:
            return
        ck = ("_c", key)
        if self._cache.get(ck) != pts:
            self._cache[ck] = pts
            self.widget_canvas.coords(iid, *pts)

    def _freq_to_x(self, f, gx0, gx1):
        u = math.log(_clamp(f, F_MIN, F_MAX) / F_MIN) / _LOG_F_SPAN
        return gx0 + u * (gx1 - gx0)

    def _x_to_freq(self, x, gx0, gx1):
        u = _clamp((x - gx0) / max(1.0, gx1 - gx0), 0.0, 1.0)
        return F_MIN * math.exp(u * _LOG_F_SPAN)

    def _db_to_y(self, db, gy0, gy1):
        u = 0.5 - (_clamp(db, -DB_SPAN, DB_SPAN) / (2.0 * DB_SPAN))
        return gy0 + u * (gy1 - gy0)

    def _res_to_y(self, res_pct, gy0, gy1):
        u = 1.0 - _clamp(res_pct, 0.0, 100.0) * 0.01
        return gy0 + (0.10 + 0.80 * u) * (gy1 - gy0)

    def _y_to_res(self, y, gy0, gy1):
        u = (y - gy0) / max(1.0, gy1 - gy0)
        norm = 1.0 - _clamp((u - 0.10) / 0.80, 0.0, 1.0)
        return norm * 100.0

    def _build_main_scene(self, w, h):
        c = self.widget_canvas
        c.delete("all")
        self._ui.clear()
        self._cache.clear()
        self.hit_rects.clear()
        self.mod_bars.clear()

        pad = max(6, int(w * 0.012))
        top_h = max(28, int(h * 0.088))
        bot_h = max(46, int(h * 0.145))
        meter_w = max(42, int(w * 0.078))

        # Top header bar buttons: MODEL A, MODEL B, ROUTING, LINK, INV B, LFO, FX, BYP
        btns = [
            ("model_a", 0.145),
            ("model_b", 0.145),
            ("routing", 0.150),
            ("link",    0.115),
            ("inv_b",   0.090),
            ("shape",   0.130),
            ("sync",    0.115),
            ("fx",      0.110),
        ]
        avail_w = w - 2 * pad - (len(btns) - 1) * 4
        bx = pad
        by0, by1 = pad, pad + top_h
        for key, frac in btns:
            bw = int(avail_w * frac)
            bx1 = bx + bw if key != "fx" else (w - pad)
            self.hit_rects[key] = (bx, by0, bx1, by1)
            self._ui[f"btn_{key}"] = c.create_rectangle(bx, by0, bx1, by1, fill=BUTTON_BG, outline=LINE_DIM, width=1)
            self._ui[f"txt_{key}"] = c.create_text((bx + bx1) * 0.5, (by0 + by1) * 0.5, text="", fill=LABEL_HI, font=FONT_7B)
            bx = bx1 + 4

        # Main Frequency Response Graph area
        gx0 = pad
        gy0 = by1 + 6
        gx1 = w - pad - meter_w - 6
        gy1 = h - pad - bot_h - 6
        self.graph_box = (gx0, gy0, gx1, gy1)
        gw = gx1 - gx0
        gh = gy1 - gy0

        c.create_rectangle(gx0, gy0, gx1, gy1, fill=PANEL_BG, outline=LINE_DIM, width=1)

        # Log frequency grid lines
        for gf, glbl in ((50.0, "50"), (100.0, "100"), (250.0, "250"), (500.0, "500"),
                         (1000.0, "1k"), (2500.0, "2.5k"), (5000.0, "5k"), (10000.0, "10k")):
            xg = self._freq_to_x(gf, gx0, gx1)
            c.create_line(xg, gy0 + 1, xg, gy1 - 1, fill=GRID_COL, width=1)
            c.create_text(xg, gy1 - 8, text=glbl, fill=LABEL_DIM, font=FONT_6B)

        # Horizontal dB grid lines
        for gdb in (-12.0, 0.0, 12.0):
            yg = self._db_to_y(gdb, gy0, gy1)
            col = ZERO_COL if gdb == 0.0 else GRID_COL
            c.create_line(gx0 + 1, yg, gx1 - 1, yg, fill=col, width=1)
            c.create_text(gx0 + 16, yg - 6, text=f"{gdb:+.0f}dB", fill=LABEL_DIM, font=FONT_6B)

        # Frequency Response polygons & lines (Combined fill, Curve A, Curve B, Combined line)
        flat_poly = [gx0, gy1, gx0, (gy0 + gy1) * 0.5, gx1, (gy0 + gy1) * 0.5, gx1, gy1]
        self._ui["poly_sum"] = c.create_polygon(*flat_poly, fill=FILL_SUM, outline="")
        flat_line = [gx0, (gy0 + gy1) * 0.5, gx1, (gy0 + gy1) * 0.5]
        self._ui["curve_a"] = c.create_line(*flat_line, fill=COL_A, width=2)
        self._ui["curve_b"] = c.create_line(*flat_line, fill=COL_B, width=2)
        self._ui["curve_sum"] = c.create_line(*flat_line, fill=COL_SUM, width=2)

        # Modulation spans & live modulated cutoff markers for A and B
        self._ui["span_a"] = c.create_line(gx0, gy0, gx0, gy0, fill=COL_A, width=3)
        self._ui["span_b"] = c.create_line(gx0, gy0, gx0, gy0, fill=COL_B, width=3)
        self._ui["eff_line_a"] = c.create_line(gx0, gy0 + 2, gx0, gy1 - 2, fill=COL_A, width=1)
        self._ui["eff_line_b"] = c.create_line(gx0, gy0 + 2, gx0, gy1 - 2, fill=COL_B, width=1)
        self._ui["eff_dot_a"] = c.create_oval(gx0 - 4, gy0 - 4, gx0 + 4, gy0 + 4, fill=COL_A, outline=PANEL_BG, width=1)
        self._ui["eff_dot_b"] = c.create_oval(gx0 - 4, gy0 - 4, gx0 + 4, gy0 + 4, fill=COL_B, outline=PANEL_BG, width=1)

        # Interactive base handle nodes for Filter A and Filter B
        self._ui["node_a_halo"] = c.create_oval(0, 0, 0, 0, fill="", outline=COL_A, width=1)
        self._ui["node_a"] = c.create_oval(0, 0, 0, 0, fill=COL_A, outline=PANEL_BG, width=2)
        self._ui["node_a_lbl"] = c.create_text(0, 0, text="A", fill=PANEL_BG, font=FONT_7B)

        self._ui["node_b_halo"] = c.create_oval(0, 0, 0, 0, fill="", outline=COL_B, width=1)
        self._ui["node_b"] = c.create_oval(0, 0, 0, 0, fill=COL_B, outline=PANEL_BG, width=2)
        self._ui["node_b_lbl"] = c.create_text(0, 0, text="B", fill=PANEL_BG, font=FONT_7B)

        # Readout badges inside top of graph
        self._ui["info_a"] = c.create_text(gx0 + 8, gy0 + 12, anchor="w", text="", fill=COL_A, font=FONT_8B)
        self._ui["info_b"] = c.create_text(gx1 - 8, gy0 + 12, anchor="e", text="", fill=COL_B, font=FONT_8B)
        self._ui["info_mid"] = c.create_text((gx0 + gx1) * 0.5, gy0 + 12, text="", fill=LABEL_HI, font=FONT_7B)

        # Right column: Input/Output Peak Meters + Env Level + Drive/Bypass badge
        mx0 = gx1 + 6
        mx1 = w - pad
        my0 = gy0
        byp_h = 26
        my1 = gy1 - byp_h - 4
        c.create_rectangle(mx0, my0, mx1, my1, fill=PANEL_BG, outline=LINE_DIM, width=1)

        mw = (mx1 - mx0 - 10) // 3
        m_top = my0 + 16
        m_bot = my1 - 6
        self._geom["meter_y"] = (m_top, m_bot)
        cols = (("in", "IN", COL_B), ("env", "ENV", COL_ENV), ("out", "OUT", COL_A))
        for idx, (mkey, mlbl, mcol) in enumerate(cols):
            bx0 = mx0 + 3 + idx * (mw + 2)
            bx1 = bx0 + mw
            self._geom[f"m_{mkey}_x"] = (bx0, bx1)
            c.create_text((bx0 + bx1) * 0.5, my0 + 8, text=mlbl, fill=LABEL_DIM, font=FONT_6B)
            c.create_rectangle(bx0, m_top, bx1, m_bot, fill="#111418", outline=GRID_COL, width=1)
            self._ui[f"bar_{mkey}"] = c.create_rectangle(bx0 + 1, m_bot, bx1 - 1, m_bot, fill=mcol, outline="")

        # Drive / Bypass button under meters (short tap: cycle Drive preset/page; long press: toggle Bypass)
        by_y0 = my1 + 4
        by_y1 = gy1
        self.hit_rects["drive_byp"] = (mx0, by_y0, mx1, by_y1)
        self._ui["btn_drive_byp"] = c.create_rectangle(mx0, by_y0, mx1, by_y1, fill=BUTTON_BG, outline=LINE_DIM, width=1)
        self._ui["txt_drive_byp"] = c.create_text((mx0 + mx1) * 0.5, (by_y0 + by_y1) * 0.5, text="", fill=COL_DRV, font=FONT_7B)

        # Bottom dock: Row 1 = Morph A bar & Morph B bar; Row 2 = LFO>Cut, Mod>Morph, Env>Cut, Balance
        dy0 = gy1 + 6
        dy1 = h - pad
        row1_h = int((dy1 - dy0 - 4) * 0.48)
        r1_y0 = dy0
        r1_y1 = dy0 + row1_h
        r2_y0 = r1_y1 + 4
        r2_y1 = dy1

        half_w = (w - 2 * pad - 6) // 2
        ma_x0, ma_x1 = pad, pad + half_w
        mb_x0, mb_x1 = ma_x1 + 6, w - pad
        self.morph_a_box = (ma_x0, r1_y0, ma_x1, r1_y1)
        self.morph_b_box = (mb_x0, r1_y0, mb_x1, r1_y1)

        for key, box, col in (("morph_a", self.morph_a_box, COL_A), ("morph_b", self.morph_b_box, COL_B)):
            x0, y0, x1, y1 = box
            c.create_rectangle(x0, y0, x1, y1, fill=PANEL_BG, outline=LINE_DIM, width=1)
            self._ui[f"fill_{key}"] = c.create_rectangle(x0 + 1, y0 + 1, x0 + 1, y1 - 1, fill="#2c2614" if key == "morph_a" else "#16283a", outline="")
            self._ui[f"eff_{key}"] = c.create_line(x0, y0 + 2, x0, y1 - 2, fill=col, width=2)
            self._ui[f"txt_{key}"] = c.create_text((x0 + x1) * 0.5, (y0 + y1) * 0.5, text="", fill=LABEL_HI, font=FONT_7B)

        # Row 2: 4 bipolar/unipolar touch bars (LFO>Cut, Mod>Morph, Env>Cut, A/B Balance)
        bars = (
            ("lfo_mod",    "LFO>CUT",   COL_MOD, True,  "g_mod"),
            ("morph_mod",  "MOD>MORPH", COL_A,   True,  "g_mod"),
            ("env_mod",    "ENV>CUT",   COL_ENV, True,  "g_mod"),
            ("ab_balance", "A/B BAL",   COL_B,   True,  "g_route"),
        )
        bw = (w - 2 * pad - 3 * 4) // 4
        bx = pad
        for idx, (sym, lbl, col, bipolar, grp) in enumerate(bars):
            bx1 = bx + bw if idx < 3 else (w - pad)
            self.mod_bars[sym] = (bx, r2_y0, bx1, r2_y1, bipolar, grp)
            c.create_rectangle(bx, r2_y0, bx1, r2_y1, fill=PANEL_BG, outline=LINE_DIM, width=1)
            if bipolar:
                xm = (bx + bx1) * 0.5
                c.create_line(xm, r2_y0 + 1, xm, r2_y1 - 1, fill=ZERO_COL, width=1)
            self._ui[f"fill_{sym}"] = c.create_rectangle(bx + 1, r2_y0 + 1, bx + 1, r2_y1 - 1, fill="#252c38", outline="")
            self._ui[f"txt_{sym}"] = c.create_text((bx + bx1) * 0.5, (r2_y0 + r2_y1) * 0.5, text=lbl, fill=LABEL_HI, font=FONT_7B)
            bx = bx1 + 4

        self._geom["w"] = w
        self._geom["h"] = h

    def _build_fx_scene(self, w, h):
        c = self.widget_canvas
        c.delete("all")
        self._ui.clear()
        self._cache.clear()
        self.hit_rects.clear()
        self.fx_sliders.clear()

        pad = max(6, int(w * 0.012))
        top_h = max(28, int(h * 0.088))

        # Top header bar in Live FX view: BACK, ROUTING, LINK, SNAP 0, ZERO MOD, BYPASS
        btns = [
            ("fx_back", 0.16),
            ("routing", 0.18),
            ("link",    0.15),
            ("snap",    0.17),
            ("zero_mod",0.17),
            ("byp_fx",  0.17),
        ]
        avail_w = w - 2 * pad - (len(btns) - 1) * 4
        bx = pad
        by0, by1 = pad, pad + top_h
        for idx, (key, frac) in enumerate(btns):
            bw = int(avail_w * frac)
            bx1 = bx + bw if idx < len(btns) - 1 else (w - pad)
            self.hit_rects[key] = (bx, by0, bx1, by1)
            self._ui[f"btn_{key}"] = c.create_rectangle(bx, by0, bx1, by1, fill=BUTTON_BG, outline=LINE_DIM, width=1)
            self._ui[f"txt_{key}"] = c.create_text((bx + bx1) * 0.5, (by0 + by1) * 0.5, text="", fill=LABEL_HI, font=FONT_7B)
            bx = bx1 + 4

        # Left 58%: Dual X-Y Pad (X = Cutoff A, Y = Cutoff B; plus LFO phase orbit trail)
        px0 = pad
        py0 = by1 + 6
        px1 = pad + int((w - 2 * pad) * 0.57)
        py1 = h - pad
        self.xy_box = (px0, py0, px1, py1)
        c.create_rectangle(px0, py0, px1, py1, fill=PANEL_BG, outline=LINE_DIM, width=1)

        pxm = (px0 + px1) * 0.5
        pym = (py0 + py1) * 0.5
        c.create_line(pxm, py0 + 1, pxm, py1 - 1, fill=ZERO_COL, width=1)
        c.create_line(px0 + 1, pym, px1 - 1, pym, fill=ZERO_COL, width=1)
        c.create_text(pxm, py0 + 11, text="PRISM X-Y PAD (X: CUTOFF A  |  Y: CUTOFF B)", fill=LABEL_DIM, font=FONT_7B)

        self._ui["xy_trail"] = c.create_line(pxm, pym, pxm, pym, fill=COL_MOD, width=2)
        self._ui["xy_eff"] = c.create_oval(pxm - 5, pym - 5, pxm + 5, pym + 5, fill=COL_ENV, outline=PANEL_BG, width=1)
        self._ui["xy_puck_halo"] = c.create_oval(pxm - 13, pym - 13, pxm + 13, pym + 13, fill="", outline=COL_A, width=1)
        self._ui["xy_puck"] = c.create_oval(pxm - 8, pym - 8, pxm + 8, pym + 8, fill=COL_A, outline=PANEL_BG, width=2)
        self._ui["xy_readout"] = c.create_text(pxm, py1 - 12, text="", fill=LABEL_HI, font=FONT_8B)

        # Right 42%: 4 quick performance sliders with "0" reset buttons
        sx0 = px1 + 6
        sx1 = w - pad
        sliders = (
            ("lfo_mod",   "LFO > CUT",   -100.0, 100.0, 0.0, COL_MOD, "g_mod"),
            ("morph_mod", "MOD > MORPH", -100.0, 100.0, 0.0, COL_A,   "g_mod"),
            ("env_mod",   "ENV > CUT",   -100.0, 100.0, 0.0, COL_ENV, "g_mod"),
            ("drive",     "PRE DRIVE",      0.0, 100.0, 15.0,COL_DRV, "g_master"),
        )
        sh = (py1 - py0 - 3 * 6) // 4
        sy = py0
        z_w = 28
        for idx, (sym, lbl, mn, mx, dflt, col, grp) in enumerate(sliders):
            sy1 = sy + sh if idx < 3 else py1
            bar_x1 = sx1 - z_w - 4
            self.fx_sliders[sym] = (sx0, sy, bar_x1, sy1, mn, mx, dflt, grp)
            c.create_rectangle(sx0, sy, bar_x1, sy1, fill=PANEL_BG, outline=LINE_DIM, width=1)
            if mn < 0.0:
                xm = (sx0 + bar_x1) * 0.5
                c.create_line(xm, sy + 1, xm, sy1 - 1, fill=ZERO_COL, width=1)
            self._ui[f"fxs_fill_{sym}"] = c.create_rectangle(sx0 + 1, sy + 1, sx0 + 1, sy1 - 1, fill="#262d38", outline="")
            self._ui[f"fxs_txt_{sym}"] = c.create_text((sx0 + bar_x1) * 0.5, (sy + sy1) * 0.5, text=lbl, fill=col, font=FONT_8B)

            zx0 = bar_x1 + 4
            self.hit_rects[f"z_{sym}"] = (zx0, sy, sx1, sy1)
            c.create_rectangle(zx0, sy, sx1, sy1, fill=BUTTON_BG, outline=LINE_DIM, width=1)
            c.create_text((zx0 + sx1) * 0.5, (sy + sy1) * 0.5, text="0", fill=LABEL_HI, font=FONT_8B)
            sy = sy1 + 6

    def refresh_gui(self, sample_telemetry=True):
        w = self.width or self.widget_canvas.winfo_width() or 640
        h = self.height or self.widget_canvas.winfo_height() or 380
        key = (int(w), int(h), bool(self.fx_view))
        if self._scene_key != key:
            self._scene_key = key
            if self.fx_view:
                self._build_fx_scene(int(w), int(h))
            else:
                self._build_main_scene(int(w), int(h))

        model_a = int(round(_clamp(self._get("model_a", 0), 0, 5)))
        cut_a   = _clamp(self._get("cutoff_a", 650.0), F_MIN, F_MAX)
        res_a   = _clamp(self._get("res_a", 35.0), 0.0, 100.0)
        morph_a = _clamp(self._get("morph_a", 0.0), 0.0, 100.0)

        model_b = int(round(_clamp(self._get("model_b", 1), 0, 5)))
        cut_b   = _clamp(self._get("cutoff_b", 1800.0), F_MIN, F_MAX)
        res_b   = _clamp(self._get("res_b", 40.0), 0.0, 100.0)
        morph_b = _clamp(self._get("morph_b", 25.0), 0.0, 100.0)

        routing = int(round(_clamp(self._get("routing", 0), 0, 2)))
        link_m  = int(round(_clamp(self._get("link_mode", 0), 0, 2)))
        ab_bal  = _clamp(self._get("ab_balance", 0.0), -100.0, 100.0)
        inv_b   = int(round(_clamp(self._get("invert_b", 0), 0, 1)))

        shape   = int(round(_clamp(self._get("lfo_shape", 0), 0, 6)))
        rate    = _clamp(self._get("lfo_rate", 0.80), RATE_MIN, RATE_MAX)
        sync_on = int(round(_clamp(self._get("lfo_sync", 0), 0, 1))) == 1
        phase_b = _clamp(self._get("lfo_phase", 90.0), 0.0, 180.0)

        lfo_mod   = _clamp(self._get("lfo_mod", 25.0), -100.0, 100.0)
        morph_mod = _clamp(self._get("morph_mod", 0.0), -100.0, 100.0)
        env_mod   = _clamp(self._get("env_mod", 0.0), -100.0, 100.0)
        drive     = _clamp(self._get("drive", 15.0), 0.0, 100.0)
        mix       = _clamp(self._get("mix", 100.0), 0.0, 100.0)
        byp       = int(round(_clamp(self._get("bypass", 0), 0, 1))) == 1

        if sample_telemetry:
            in_pk  = self._get_mon("in_peak", FLOOR_DB)
            out_pk = self._get_mon("out_peak", FLOOR_DB)
            eff_ca = _clamp(self._get_mon("eff_cut_a", cut_a), F_MIN, F_MAX)
            eff_cb = _clamp(self._get_mon("eff_cut_b", cut_b), F_MIN, F_MAX)
            eff_ma = _clamp(self._get_mon("eff_morph_a", morph_a), 0.0, 100.0)
            eff_mb = _clamp(self._get_mon("eff_morph_b", morph_b), 0.0, 100.0)
            lfo_a  = _clamp(self._get_mon("lfo_a", 0.0), -1.0, 1.0)
            lfo_b  = _clamp(self._get_mon("lfo_b", 0.0), -1.0, 1.0)
            env_lv = _clamp(self._get_mon("env_lvl", 0.0), 0.0, 1.0)
            bpm    = _clamp(self._get_mon("host_bpm", 120.0), 20.0, 400.0)
            self._telemetry = (in_pk, out_pk, eff_ca, eff_cb, eff_ma, eff_mb, lfo_a, lfo_b, env_lv, bpm)
            if self.drag_mode is None:
                pt = (round(eff_ca, 1), round(eff_cb, 1))
                if pt != self._last_trail_pt:
                    self._last_trail_pt = pt
                    self.trail.append((eff_ca, eff_cb))
        else:
            in_pk, out_pk, _, _, _, _, lfo_a, lfo_b, env_lv, bpm = self._telemetry
            # Recompute responsive effective cutoffs & morphs immediately during knob/touch drag
            oct_a = math.log2(cut_a)
            oct_b = math.log2(cut_b)
            if link_m == 1:
                oct_b = oct_a + (oct_b - 9.965784)
            elif link_m == 2:
                oct_b = 2.0 * 9.965784 - oct_a + (oct_b - 9.965784) * 0.5
            env_oct_a = (env_mod * 0.01) * env_lv * 4.5
            env_oct_b = -env_oct_a if link_m == 2 else env_oct_a
            eff_ca = _clamp(2.0 ** (oct_a + (lfo_mod * 0.01) * lfo_a * 3.8 + env_oct_a), F_MIN, F_MAX)
            eff_cb = _clamp(2.0 ** (oct_b + (lfo_mod * 0.01) * lfo_b * 3.8 + env_oct_b), F_MIN, F_MAX)
            eff_ma = _clamp(morph_a + morph_mod * (0.65 * lfo_a + 0.55 * (env_lv - 0.3)), 0.0, 100.0)
            eff_mb = _clamp(morph_b + morph_mod * (0.65 * lfo_b + 0.55 * (env_lv - 0.3)), 0.0, 100.0)

        div_lbl, _ = nearest_rate_division(rate, bpm)
        self._sync_encoder_titles(sync_on, div_lbl)

        if self.fx_view:
            self._refresh_fx_view(cut_a, cut_b, eff_ca, eff_cb, routing, link_m, lfo_mod, morph_mod, env_mod, drive, byp)
            return

        # Top buttons update
        m_a_name, m_a_col = MODELS[model_a]
        m_b_name, m_b_col = MODELS[model_b]
        col_a = BYP_MID if byp else COL_A
        col_b = BYP_MID if byp else COL_B
        col_sum = BYP_HI if byp else COL_SUM

        self._cfg("txt_model_a", text=f"A: {m_a_name}", fill=BYP_HI if byp else m_a_col)
        self._cfg("txt_model_b", text=f"B: {m_b_name}", fill=BYP_HI if byp else m_b_col)
        self._cfg("txt_routing", text=ROUTINGS[routing], fill=LABEL_HI)
        self._cfg("txt_link", text=f"LNK:{LINKS[link_m]}", fill=COL_A if link_m > 0 and not byp else LABEL_DIM)
        self._cfg("txt_inv_b", text="ØB:INV" if inv_b else "ØB:NRM", fill=HOT_RED if inv_b and not byp else LABEL_DIM)
        self._cfg("txt_shape", text=f"LFO:{LFO_SHAPES[shape]}", fill=COL_MOD if not byp else LABEL_DIM)
        rate_txt = f"SYNC {div_lbl}" if sync_on else _fmt_rate(rate)
        self._cfg("txt_sync", text=rate_txt, fill=COL_ENV if sync_on and not byp else LABEL_HI)
        self._cfg("txt_fx", text="XY / FX", fill=COL_A if not byp else LABEL_DIM)

        # Frequency Response curves calculation at live modulated cutoffs & morphs
        gx0, gy0, gx1, gy1 = self.graph_box
        db_a = _eval_model_db(model_a, _CURVE_FREQS, eff_ca, res_a, eff_ma)
        db_b = _eval_model_db(model_b, _CURVE_FREQS, eff_cb, res_b, eff_mb)

        bal = ab_bal * 0.01
        ga = _clamp(1.0 - max(0.0, bal), 0.0, 1.0)
        gb = _clamp(1.0 + min(0.0, bal), 0.0, 1.0)

        pts_a = []
        pts_b = []
        pts_sum = []
        gw = gx1 - gx0
        for idx, u in enumerate(_CURVE_US):
            x = gx0 + u * gw
            da = db_a[idx]
            db = db_b[idx]
            pts_a.extend((x, self._db_to_y(da, gy0, gy1)))
            pts_b.extend((x, self._db_to_y(db, gy0, gy1)))

            if routing == 0:  # Serial A -> B
                d_sum = da * ga + db * gb
            elif routing == 1:  # Parallel A + B (with phase invert B cancellation)
                la = (10.0 ** (da / 20.0)) * ga
                lb = (10.0 ** (db / 20.0)) * gb
                comb = abs(la - lb) if inv_b else (la + lb) * 0.65
                d_sum = 20.0 * math.log10(max(comb, 0.025))
            else:  # Stereo Split L/R envelope
                d_sum = max(da * ga, db * gb)
            d_sum = _clamp(d_sum * (mix * 0.01), -DB_SPAN, DB_SPAN)
            pts_sum.extend((x, self._db_to_y(d_sum, gy0, gy1)))

        self._coords("curve_a", *pts_a)
        self._coords("curve_b", *pts_b)
        self._coords("curve_sum", *pts_sum)
        self._coords("poly_sum", gx0, gy1, *pts_sum, gx1, gy1)

        self._cfg("curve_a", fill=col_a)
        self._cfg("curve_b", fill=col_b)
        self._cfg("curve_sum", fill=col_sum)
        self._cfg("poly_sum", fill=BYP_FILL if byp else FILL_SUM)

        # Base handles & live modulated indicators
        xa_base = self._freq_to_x(cut_a, gx0, gx1)
        ya_base = self._res_to_y(res_a, gy0, gy1)
        xb_base = self._freq_to_x(cut_b, gx0, gx1)
        yb_base = self._res_to_y(res_b, gy0, gy1)
        self.node_pos["a"] = (xa_base, ya_base)
        self.node_pos["b"] = (xb_base, yb_base)

        xa_eff = self._freq_to_x(eff_ca, gx0, gx1)
        xb_eff = self._freq_to_x(eff_cb, gx0, gx1)

        self._coords("span_a", xa_base, ya_base, xa_eff, ya_base)
        self._coords("span_b", xb_base, yb_base, xb_eff, yb_base)
        self._coords("eff_line_a", xa_eff, gy0 + 2, xa_eff, gy1 - 2)
        self._coords("eff_line_b", xb_eff, gy0 + 2, xb_eff, gy1 - 2)
        self._coords("eff_dot_a", xa_eff - 4, ya_base - 4, xa_eff + 4, ya_base + 4)
        self._coords("eff_dot_b", xb_eff - 4, yb_base - 4, xb_eff + 4, yb_base + 4)

        r = 10
        rh = 14
        self._coords("node_a_halo", xa_base - rh, ya_base - rh, xa_base + rh, ya_base + rh)
        self._coords("node_a", xa_base - r, ya_base - r, xa_base + r, ya_base + r)
        self._coords("node_a_lbl", xa_base, ya_base)

        self._coords("node_b_halo", xb_base - rh, yb_base - rh, xb_base + rh, yb_base + rh)
        self._coords("node_b", xb_base - r, yb_base - r, xb_base + r, yb_base + r)
        self._coords("node_b_lbl", xb_base, yb_base)

        self._cfg("info_a", text=f"A: {_fmt_freq(eff_ca)}  Q:{res_a:.0f}%", fill=col_a)
        self._cfg("info_b", text=f"B: {_fmt_freq(eff_cb)}  Q:{res_b:.0f}%", fill=col_b)
        self._cfg("info_mid", text=f"Δφ:{phase_b:.0f}°  MIX:{mix:.0f}%", fill=BYP_MID if byp else LABEL_DIM)

        # Meters update
        m_top, m_bot = self._geom["meter_y"]
        mh = max(1.0, m_bot - m_top)
        for mkey, val_norm in (
            ("in",  _clamp((in_pk - FLOOR_DB) / (6.0 - FLOOR_DB), 0.0, 1.0)),
            ("env", _clamp(env_lv, 0.0, 1.0)),
            ("out", _clamp((out_pk - FLOOR_DB) / (6.0 - FLOOR_DB), 0.0, 1.0)),
        ):
            bx0, bx1 = self._geom[f"m_{mkey}_x"]
            yt = m_bot - val_norm * mh
            self._coords(f"bar_{mkey}", bx0 + 1, yt, bx1 - 1, m_bot)

        if byp:
            self._cfg("btn_drive_byp", fill="#352024")
            self._cfg("txt_drive_byp", text="BYPASS", fill=HOT_RED)
        else:
            self._cfg("btn_drive_byp", fill=BUTTON_BG)
            self._cfg("txt_drive_byp", text=f"DRV {drive:.0f}%", fill=COL_DRV)

        # Morph A & B bars
        for key, box, val, eff_v, lbl in (
            ("morph_a", self.morph_a_box, morph_a, eff_ma, "MORPH A"),
            ("morph_b", self.morph_b_box, morph_b, eff_mb, "MORPH B"),
        ):
            x0, y0, x1, y1 = box
            xf = x0 + 1 + (x1 - x0 - 2) * (val * 0.01)
            xe = x0 + 1 + (x1 - x0 - 2) * (eff_v * 0.01)
            self._coords(f"fill_{key}", x0 + 1, y0 + 1, xf, y1 - 1)
            self._coords(f"eff_{key}", xe, y0 + 2, xe, y1 - 2)
            self._cfg(f"txt_{key}", text=f"{lbl}: {val:.0f}%")

        # Bottom row 2 modulation & balance bars
        vals = {
            "lfo_mod":    (lfo_mod,   f"LFO>CUT {lfo_mod:+.0f}%"),
            "morph_mod":  (morph_mod, f"MOD>MRPH {morph_mod:+.0f}%"),
            "env_mod":    (env_mod,   f"ENV>CUT {env_mod:+.0f}%"),
            "ab_balance": (ab_bal,    f"BAL {ab_bal:+.0f}%"),
        }
        for sym, (bx0, by0, bx1, by1, bipolar, _) in self.mod_bars.items():
            v, txt = vals[sym]
            xm = (bx0 + bx1) * 0.5
            xv = xm + (v * 0.01) * ((bx1 - bx0) * 0.5 - 1)
            self._coords(f"fill_{sym}", min(xm, xv), by0 + 1, max(xm, xv), by1 - 1)
            self._cfg(f"txt_{sym}", text=txt)

    def _refresh_fx_view(self, cut_a, cut_b, eff_ca, eff_cb, routing, link_m, lfo_mod, morph_mod, env_mod, drive, byp):
        self._cfg("txt_fx_back", text="< GRAPH", fill=COL_A)
        self._cfg("txt_routing", text=ROUTINGS[routing], fill=LABEL_HI)
        self._cfg("txt_link", text=f"LNK:{LINKS[link_m]}", fill=COL_A if link_m > 0 else LABEL_DIM)
        self._cfg("txt_snap", text="SNAP 0: ON" if self.fx_snap else "SNAP 0: OFF", fill=COL_ENV if self.fx_snap else LABEL_DIM)
        self._cfg("txt_zero_mod", text="ZERO MOD", fill=COL_MOD)
        self._cfg("txt_byp_fx", text="BYPASS: ON" if byp else "BYPASS: OFF", fill=HOT_RED if byp else LABEL_DIM)

        px0, py0, px1, py1 = self.xy_box
        pw = px1 - px0
        ph = py1 - py0

        def _f_to_px(f):
            u = math.log(_clamp(f, F_MIN, F_MAX) / F_MIN) / _LOG_F_SPAN
            return px0 + 10 + u * (pw - 20)

        def _f_to_py(f):
            u = math.log(_clamp(f, F_MIN, F_MAX) / F_MIN) / _LOG_F_SPAN
            return py1 - 10 - u * (ph - 24)

        xb = _f_to_px(cut_a)
        yb = _f_to_py(cut_b)
        xe = _f_to_px(eff_ca)
        ye = _f_to_py(eff_cb)

        if len(self.trail) >= 2:
            tpts = []
            for ta, tb in self.trail:
                tpts.extend((_f_to_px(ta), _f_to_py(tb)))
            self._coords("xy_trail", *tpts)
        else:
            self._coords("xy_trail", xb, yb, xe, ye)

        self._coords("xy_eff", xe - 5, ye - 5, xe + 5, ye + 5)
        self._coords("xy_puck_halo", xb - 13, yb - 13, xb + 13, yb + 13)
        self._coords("xy_puck", xb - 8, yb - 8, xb + 8, yb + 8)
        self._cfg("xy_readout", text=f"A: {_fmt_freq(eff_ca)}   |   B: {_fmt_freq(eff_cb)}")

        s_vals = {
            "lfo_mod":   (lfo_mod,   f"LFO > CUT: {lfo_mod:+.0f}%"),
            "morph_mod": (morph_mod, f"MOD > MORPH: {morph_mod:+.0f}%"),
            "env_mod":   (env_mod,   f"ENV > CUT: {env_mod:+.0f}%"),
            "drive":     (drive,     f"PRE DRIVE: {drive:.0f}%"),
        }
        for sym, (sx0, sy0, sx1, sy1, mn, mx, _, _) in self.fx_sliders.items():
            v, txt = s_vals[sym]
            if mn < 0.0:
                xm = (sx0 + sx1) * 0.5
                xv = xm + (v / mx) * ((sx1 - sx0) * 0.5 - 1)
                self._coords(f"fxs_fill_{sym}", min(xm, xv), sy0 + 1, max(xm, xv), sy1 - 1)
            else:
                xf = sx0 + 1 + (v / mx) * (sx1 - sx0 - 2)
                self._coords(f"fxs_fill_{sym}", sx0 + 1, sy0 + 1, xf, sy1 - 1)
            self._cfg(f"fxs_txt_{sym}", text=txt)

    @staticmethod
    def _in_rect(x, y, r):
        return r[0] <= x <= r[2] and r[1] <= y <= r[3]

    def _apply_drag(self, x, y):
        if not self.drag_mode:
            return
        kind = self.drag_mode[0]
        if kind == "node":
            which = self.drag_mode[1]
            gx0, gy0, gx1, gy1 = self.graph_box
            freq = self._x_to_freq(x, gx0, gx1)
            res = self._y_to_res(y, gy0, gy1)
            if which == "a":
                self._set("cutoff_a", freq)
                self._set("res_a", res)
            else:
                self._set("cutoff_b", freq)
                self._set("res_b", res)
        elif kind == "morph_a":
            x0, _, x1, _ = self.morph_a_box
            u = _clamp((x - x0) / max(1.0, x1 - x0), 0.0, 1.0)
            self._set("morph_a", u * 100.0)
        elif kind == "morph_b":
            x0, _, x1, _ = self.morph_b_box
            u = _clamp((x - x0) / max(1.0, x1 - x0), 0.0, 1.0)
            self._set("morph_b", u * 100.0)
        elif kind == "mod_bar":
            sym = self.drag_mode[1]
            bx0, _, bx1, _, bipolar, _ = self.mod_bars[sym]
            u = _clamp((x - bx0) / max(1.0, bx1 - bx0), 0.0, 1.0)
            val = (u * 200.0 - 100.0) if bipolar else (u * 100.0)
            self._set(sym, val)
        elif kind == "xy":
            px0, py0, px1, py1 = self.xy_box
            ux = _clamp((x - (px0 + 10)) / max(1.0, (px1 - px0 - 20)), 0.0, 1.0)
            uy = _clamp(((py1 - 10) - y) / max(1.0, (py1 - py0 - 24)), 0.0, 1.0)
            fa = F_MIN * math.exp(ux * _LOG_F_SPAN)
            fb = F_MIN * math.exp(uy * _LOG_F_SPAN)
            self._set("cutoff_a", fa)
            self._set("cutoff_b", fb)
        elif kind == "fx_slider":
            sym = self.drag_mode[1]
            sx0, _, sx1, _, mn, mx, _, _ = self.fx_sliders[sym]
            u = _clamp((x - sx0) / max(1.0, sx1 - sx0), 0.0, 1.0)
            self._set(sym, mn + u * (mx - mn))

    def _on_press(self, event):
        x, y = float(event.x), float(event.y)
        self.drag_mode = None
        self._pending_drag = None

        if self.fx_view:
            for key, rect in self.hit_rects.items():
                if self._in_rect(x, y, rect):
                    if key == "fx_back":
                        self.fx_view = False
                        self._scene_key = None
                        self.refresh_gui(sample_telemetry=False)
                        return
                    if key == "routing":
                        cur = int(round(self._get("routing", 0)))
                        self._set("routing", (cur + 1) % 3)
                        self._select_group("g_route")
                        self.refresh_gui(sample_telemetry=False)
                        return
                    if key == "link":
                        cur = int(round(self._get("link_mode", 0)))
                        self._set("link_mode", (cur + 1) % 3)
                        self._select_group("g_route")
                        self.refresh_gui(sample_telemetry=False)
                        return
                    if key == "snap":
                        self.fx_snap = not self.fx_snap
                        self.refresh_gui(sample_telemetry=False)
                        return
                    if key == "zero_mod":
                        self._set("lfo_mod", 0.0)
                        self._set("morph_mod", 0.0)
                        self._set("env_mod", 0.0)
                        self._select_group("g_mod")
                        self.refresh_gui(sample_telemetry=False)
                        return
                    if key == "byp_fx":
                        cur = int(round(self._get("bypass", 0)))
                        self._set("bypass", 0 if cur else 1)
                        self._select_group("g_master")
                        self.refresh_gui(sample_telemetry=False)
                        return
                    if key.startswith("z_"):
                        sym = key[2:]
                        if sym in self.fx_sliders:
                            _, _, _, _, _, _, dflt, grp = self.fx_sliders[sym]
                            self._set(sym, dflt)
                            self._select_group(grp)
                            self.refresh_gui(sample_telemetry=False)
                        return

            if self._in_rect(x, y, self.xy_box):
                self._snap_origin = {
                    "cutoff_a": self._get("cutoff_a", 650.0),
                    "cutoff_b": self._get("cutoff_b", 1800.0),
                }
                self.drag_mode = ("xy",)
                self._select_group("g_flt_a")
                self._apply_drag(x, y)
                self._last_drag_ts = time.monotonic()
                self.refresh_gui(sample_telemetry=False)
                return

            for sym, rect in self.fx_sliders.items():
                if self._in_rect(x, y, rect[:4]):
                    self.drag_mode = ("fx_slider", sym)
                    self._select_group(rect[7])
                    self._apply_drag(x, y)
                    self._last_drag_ts = time.monotonic()
                    self.refresh_gui(sample_telemetry=False)
                    return
            return

        # Main view header & meter buttons
        for key, rect in self.hit_rects.items():
            if self._in_rect(x, y, rect):
                if key == "model_a":
                    cur = int(round(self._get("model_a", 0)))
                    self._set("model_a", (cur + 1) % len(MODELS))
                    self._select_group("g_flt_a")
                    self.refresh_gui(sample_telemetry=False)
                    return
                if key == "model_b":
                    cur = int(round(self._get("model_b", 1)))
                    self._set("model_b", (cur + 1) % len(MODELS))
                    self._select_group("g_flt_b")
                    self.refresh_gui(sample_telemetry=False)
                    return
                if key == "routing":
                    cur = int(round(self._get("routing", 0)))
                    self._set("routing", (cur + 1) % 3)
                    self._select_group("g_route")
                    self.refresh_gui(sample_telemetry=False)
                    return
                if key == "link":
                    cur = int(round(self._get("link_mode", 0)))
                    self._set("link_mode", (cur + 1) % 3)
                    self._select_group("g_route")
                    self.refresh_gui(sample_telemetry=False)
                    return
                if key == "inv_b":
                    cur = int(round(self._get("invert_b", 0)))
                    self._set("invert_b", 0 if cur else 1)
                    self._select_group("g_route")
                    self.refresh_gui(sample_telemetry=False)
                    return
                if key == "shape":
                    cur = int(round(self._get("lfo_shape", 0)))
                    self._set("lfo_shape", (cur + 1) % len(LFO_SHAPES))
                    self._select_group("g_lfo")
                    self.refresh_gui(sample_telemetry=False)
                    return
                if key == "sync":
                    cur = int(round(self._get("lfo_sync", 0)))
                    self._set("lfo_sync", 0 if cur else 1)
                    self._select_group("g_lfo")
                    self.refresh_gui(sample_telemetry=False)
                    return
                if key == "fx":
                    self.fx_view = True
                    self._scene_key = None
                    self._select_group("g_mod")
                    self.refresh_gui(sample_telemetry=False)
                    return
                if key == "drive_byp":
                    self.drag_mode = ("drive_byp", time.monotonic())
                    return

        # Graph area: pick nearest node A or B
        if self._in_rect(x, y, self.graph_box):
            xa, ya = self.node_pos.get("a", (0.0, 0.0))
            xb, yb = self.node_pos.get("b", (0.0, 0.0))
            da = (x - xa) ** 2 + (y - ya) ** 2
            db = (x - xb) ** 2 + (y - yb) ** 2
            which = "a" if da <= db else "b"
            self.drag_mode = ("node", which)
            self._select_group("g_flt_a" if which == "a" else "g_flt_b")
            self._apply_drag(x, y)
            self._last_drag_ts = time.monotonic()
            self.refresh_gui(sample_telemetry=False)
            return

        if self._in_rect(x, y, self.morph_a_box):
            self.drag_mode = ("morph_a",)
            self._select_group("g_flt_a")
            self._apply_drag(x, y)
            self._last_drag_ts = time.monotonic()
            self.refresh_gui(sample_telemetry=False)
            return

        if self._in_rect(x, y, self.morph_b_box):
            self.drag_mode = ("morph_b",)
            self._select_group("g_flt_b")
            self._apply_drag(x, y)
            self._last_drag_ts = time.monotonic()
            self.refresh_gui(sample_telemetry=False)
            return

        for sym, rect in self.mod_bars.items():
            if self._in_rect(x, y, rect[:4]):
                self.drag_mode = ("mod_bar", sym)
                self._select_group(rect[5])
                self._apply_drag(x, y)
                self._last_drag_ts = time.monotonic()
                self.refresh_gui(sample_telemetry=False)
                return

    def _on_motion(self, event):
        if not self.drag_mode or self.drag_mode[0] == "drive_byp":
            return
        x, y = float(event.x), float(event.y)
        now = time.monotonic()
        if (now - self._last_drag_ts) < DRAG_INTERVAL:
            self._pending_drag = (x, y)
            return
        self._pending_drag = None
        self._last_drag_ts = now
        self._apply_drag(x, y)
        self.refresh_gui(sample_telemetry=False)

    def _on_release(self, event):
        if not self.drag_mode:
            return
        kind = self.drag_mode[0]
        if kind == "drive_byp":
            t0 = self.drag_mode[1]
            self.drag_mode = None
            if (time.monotonic() - t0) >= LONG_PRESS_SEC:
                cur = int(round(self._get("bypass", 0)))
                self._set("bypass", 0 if cur else 1)
            else:
                if int(round(self._get("bypass", 0))) == 1:
                    self._set("bypass", 0)
                else:
                    # Cycle useful drive levels on short tap & select Master page
                    drv = self._get("drive", 15.0)
                    nxt = 45.0 if drv < 30.0 else (80.0 if drv < 65.0 else 15.0)
                    self._set("drive", nxt)
            self._select_group("g_master")
            self.refresh_gui(sample_telemetry=False)
            return

        if self._pending_drag is not None:
            px, py = self._pending_drag
            self._pending_drag = None
            self._apply_drag(px, py)
        else:
            self._apply_drag(float(event.x), float(event.y))

        if kind == "xy" and self.fx_snap and self._snap_origin:
            self._set("cutoff_a", self._snap_origin.get("cutoff_a", 650.0))
            self._set("cutoff_b", self._snap_origin.get("cutoff_b", 1800.0))

        self.drag_mode = None
        self.refresh_gui(sample_telemetry=False)
