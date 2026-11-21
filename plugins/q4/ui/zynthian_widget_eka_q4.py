#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# SPDX-License-Identifier: GPL-3.0-or-later
# Zynthian widget for Eka Q-4 (class/file: zynthian_widget_eka_q4 & alias zynthian_widget_eka_q-4).
import math
import time
import tkinter

from zyngui import zynthian_gui_config
from zyngui import zynthian_widget_base

MULTI_INSTANCE = True

REFRESH_SEC = 0.08
F_MIN = 20.0
F_MAX = 20000.0
DB_RANGE = 18.0
SPEC_BINS = 24
SPEC_F_MIN = 28.0
SPEC_F_MAX = 16000.0
SPEC_FLOOR = -60.0
LONG_PRESS_SEC = 0.45
DOUBLE_TAP_SEC = 0.35
SAMPLE_RATE = 48000.0

BAND_SPECS = (
    # (idx, color, freq_min, freq_max, q_min, q_max, types, off_idx)
    (0, "#4dabf7", 20.0,  1000.0,  0.2, 12.0, ("SHELF", "BELL", "PULTEC", "OFF"), 3),
    (1, "#63e6be", 60.0,  4000.0,  0.2, 16.0, ("BELL", "NOTCH", "OFF"),           2),
    (2, "#ffd43b", 250.0, 12000.0, 0.2, 16.0, ("BELL", "NOTCH", "OFF"),           2),
    (3, "#ff8787", 800.0, 20000.0, 0.2, 12.0, ("SHELF", "BELL", "AIR", "OFF"),    3),
)

PANEL_BG   = "#16191e"
BUTTON_BG  = "#1f242c"
LINE_DIM   = "#2f3640"
GRID_COL   = "#252b34"
ZERO_COL   = "#3a4352"
LABEL_DIM  = "#8b949e"
LABEL_HI   = "#e6edf3"
SPEC_FILL  = "#1c2d3d"
SPEC_LINE  = "#34597a"
CURVE_COL  = "#f0f6fc"
CUT_COL    = "#b197fc"
SOLO_COL   = "#ffa94d"
AUTO_COL   = "#63e6be"
IN_COL     = "#4dabf7"
OUT_COL    = "#63e6be"
HOT_RED    = "#ff6b6b"


def _clamp(v, lo, hi):
    return lo if v < lo else (hi if v > hi else v)


def _fmt_freq(f):
    if f >= 1000.0:
        return f"{f / 1000.0:.2f}k" if f < 10000.0 else f"{f / 1000.0:.1f}k"
    return f"{f:.0f}Hz"


def _biquad_mag_sq(b0, b1, b2, a1, a2, w):
    cw = math.cos(w)
    c2w = math.cos(2.0 * w)
    sw = math.sin(w)
    s2w = math.sin(2.0 * w)
    nr = b0 + b1 * cw + b2 * c2w
    ni = -(b1 * sw + b2 * s2w)
    dr = 1.0 + a1 * cw + a2 * c2w
    di = -(a1 * sw + a2 * s2w)
    den = dr * dr + di * di
    if den < 1e-24:
        den = 1e-24
    return (nr * nr + ni * ni) / den


def _mk_hpf(f, q, sr=SAMPLE_RATE):
    f = _clamp(f, 15.0, sr * 0.46)
    w0 = 2.0 * math.pi * f / sr
    cw, sw = math.cos(w0), math.sin(w0)
    alpha = sw / (2.0 * q)
    inv = 1.0 / (1.0 + alpha)
    return ((1.0 + cw) * 0.5 * inv, -(1.0 + cw) * inv, (1.0 + cw) * 0.5 * inv,
            -2.0 * cw * inv, (1.0 - alpha) * inv)


def _mk_lpf(f, q, sr=SAMPLE_RATE):
    f = _clamp(f, 15.0, sr * 0.46)
    w0 = 2.0 * math.pi * f / sr
    cw, sw = math.cos(w0), math.sin(w0)
    alpha = sw / (2.0 * q)
    inv = 1.0 / (1.0 + alpha)
    return ((1.0 - cw) * 0.5 * inv, (1.0 - cw) * inv, (1.0 - cw) * 0.5 * inv,
            -2.0 * cw * inv, (1.0 - alpha) * inv)


def _mk_peak(f, q, gdb, sr=SAMPLE_RATE):
    if abs(gdb) < 1e-4:
        return (1.0, 0.0, 0.0, 0.0, 0.0)
    f = _clamp(f, 15.0, sr * 0.46)
    A = 10.0 ** (gdb / 40.0)
    w0 = 2.0 * math.pi * f / sr
    cw, sw = math.cos(w0), math.sin(w0)
    alpha = sw / (2.0 * q)
    inv = 1.0 / (1.0 + alpha / A)
    return ((1.0 + alpha * A) * inv, -2.0 * cw * inv, (1.0 - alpha * A) * inv,
            -2.0 * cw * inv, (1.0 - alpha / A) * inv)


def _mk_low_shelf(f, q, gdb, sr=SAMPLE_RATE):
    if abs(gdb) < 1e-4:
        return (1.0, 0.0, 0.0, 0.0, 0.0)
    f = _clamp(f, 15.0, sr * 0.46)
    A = 10.0 ** (gdb / 40.0)
    w0 = 2.0 * math.pi * f / sr
    cw, sw = math.cos(w0), math.sin(w0)
    alpha = sw / (2.0 * q)
    sq = 2.0 * math.sqrt(A) * alpha
    inv = 1.0 / ((A + 1.0) + (A - 1.0) * cw + sq)
    return (A * ((A + 1.0) - (A - 1.0) * cw + sq) * inv,
            2.0 * A * ((A - 1.0) - (A + 1.0) * cw) * inv,
            A * ((A + 1.0) - (A - 1.0) * cw - sq) * inv,
            -2.0 * ((A - 1.0) + (A + 1.0) * cw) * inv,
            ((A + 1.0) + (A - 1.0) * cw - sq) * inv)


def _mk_high_shelf(f, q, gdb, sr=SAMPLE_RATE):
    if abs(gdb) < 1e-4:
        return (1.0, 0.0, 0.0, 0.0, 0.0)
    f = _clamp(f, 15.0, sr * 0.46)
    A = 10.0 ** (gdb / 40.0)
    w0 = 2.0 * math.pi * f / sr
    cw, sw = math.cos(w0), math.sin(w0)
    alpha = sw / (2.0 * q)
    sq = 2.0 * math.sqrt(A) * alpha
    inv = 1.0 / ((A + 1.0) - (A - 1.0) * cw + sq)
    return (A * ((A + 1.0) + (A - 1.0) * cw + sq) * inv,
            -2.0 * A * ((A - 1.0) + (A + 1.0) * cw) * inv,
            A * ((A + 1.0) + (A - 1.0) * cw - sq) * inv,
            2.0 * ((A - 1.0) - (A + 1.0) * cw) * inv,
            ((A + 1.0) - (A - 1.0) * cw - sq) * inv)


def _mk_notch(f, q, sr=SAMPLE_RATE):
    f = _clamp(f, 15.0, sr * 0.46)
    w0 = 2.0 * math.pi * f / sr
    cw, sw = math.cos(w0), math.sin(w0)
    alpha = sw / (2.0 * q)
    inv = 1.0 / (1.0 + alpha)
    return (inv, -2.0 * cw * inv, inv, -2.0 * cw * inv, (1.0 - alpha) * inv)


def build_stage_coeffs(state):
    """Build per-band and global biquad coefficient lists matching src/q4_core.c."""
    scale = _clamp(state["scale_pct"], 0.0, 200.0) * 0.01
    global_stages = []
    band_stages = [[], [], [], []]

    hpf = state["hpf_freq"]
    lpf = state["lpf_freq"]
    s24 = state["slope"] >= 0.5
    if hpf > 20.5:
        if s24:
            global_stages.append(_mk_hpf(hpf, 0.54119610))
            global_stages.append(_mk_hpf(hpf, 1.30656296))
        else:
            global_stages.append(_mk_hpf(hpf, 0.70710678))
    if lpf < 19900.0:
        if s24:
            global_stages.append(_mk_lpf(lpf, 0.54119610))
            global_stages.append(_mk_lpf(lpf, 1.30656296))
        else:
            global_stages.append(_mk_lpf(lpf, 0.70710678))

    tilt = _clamp(state["tilt_db"], -6.0, 6.0) * scale
    if abs(tilt) > 1e-4:
        global_stages.append(_mk_low_shelf(1000.0, 0.5, -tilt))
        global_stages.append(_mk_high_shelf(1000.0, 0.5, tilt))

    # Band 1
    f1, g1, q1, t1 = state["b1_freq"], state["b1_gain"] * scale, state["b1_q"], int(round(state["b1_type"]))
    if t1 == 0:
        band_stages[0].append(_mk_low_shelf(f1, q1, g1))
    elif t1 == 1:
        band_stages[0].append(_mk_peak(f1, q1, g1))
    elif t1 == 2:
        band_stages[0].append(_mk_low_shelf(f1, q1, g1))
        if abs(g1) > 1e-4:
            band_stages[0].append(_mk_peak(f1 * 1.75, 1.2, -0.35 * g1))

    # Band 2 & 3
    for bi in (1, 2):
        pfx = f"b{bi + 1}"
        fb, gb, qb, tb = state[f"{pfx}_freq"], state[f"{pfx}_gain"] * scale, state[f"{pfx}_q"], int(round(state[f"{pfx}_type"]))
        if tb == 0:
            band_stages[bi].append(_mk_peak(fb, qb, gb))
        elif tb == 1 and scale > 0.01:
            band_stages[bi].append(_mk_notch(fb, qb))

    # Band 4
    f4, g4, q4, t4 = state["b4_freq"], state["b4_gain"] * scale, state["b4_q"], int(round(state["b4_type"]))
    if t4 == 0:
        band_stages[3].append(_mk_high_shelf(f4, q4, g4))
    elif t4 == 1:
        band_stages[3].append(_mk_peak(f4, q4, g4))
    elif t4 == 2:
        band_stages[3].append(_mk_high_shelf(min(19500.0, f4 * 1.15), 0.55, g4))

    return global_stages, band_stages


def eval_stages_db(stages, f_hz, sr=SAMPLE_RATE):
    w = 2.0 * math.pi * f_hz / sr
    m2 = 1.0
    for c in stages:
        m2 *= _biquad_mag_sq(c[0], c[1], c[2], c[3], c[4], w)
    return 10.0 * math.log10(max(m2, 1e-12))


class zynthian_widget_eka_q4(zynthian_widget_base.zynthian_widget_base):
    def __init__(self, parent):
        super().__init__(parent)
        self.next_tick = 0.0
        self.selected_band = 0
        self.last_group = None
        self.saved_types = [0, 0, 0, 0]
        self.spec_smooth = [SPEC_FLOOR] * SPEC_BINS
        self.hold_in = SPEC_FLOOR
        self.hold_out = SPEC_FLOOR
        self.hit_rects = {}
        self.node_pos = {}
        self.graph_box = (0.0, 0.0, 1.0, 1.0)
        self.q_bar_box = (0.0, 0.0, 1.0, 1.0)
        self.drag_mode = None      # ("band", idx) | ("hpf",) | ("lpf",) | ("q",) | ("tab", idx, t0)
        self.last_tap = (None, 0.0)

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

    def update(self):
        now = time.monotonic()
        if now < self.next_tick:
            return
        self.next_tick = now + REFRESH_SEC
        super().update()

    def on_size(self, event):
        if not super().on_size(event):
            return
        self.refresh_gui()

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
            return
        zctrl = self.processor.controllers_dict.get(sym)
        if zctrl is not None:
            zctrl.set_value(value)

    def _page_rows(self):
        """Zynthian's control pages: (index, group_symbol) for this processor."""
        try:
            rows = self.zyngui_control.list_data
        except Exception:
            return []
        out = []
        for i, row in enumerate(rows):
            if len(row) >= 5 and row[0] and row[3] is self.processor:
                out.append((i, row[0]))
        return out

    def _select_group(self, group_sym):
        self.last_group = group_sym
        for i, group in self._page_rows():
            if group == group_sym:
                try:
                    self.zyngui_control.select(i)
                except Exception:
                    pass
                return

    def _select_page(self, band_idx):
        self._select_group(f"g_b{band_idx + 1}")

    def _current_group(self):
        try:
            info = self.zyngui_control.screen_info
            if info and len(info) >= 5 and info[3] is self.processor:
                return info[0]
        except Exception:
            pass
        return None

    def _follow_page(self):
        """When the user changes the knob page on Zynthian, highlight the matching band."""
        group = self._current_group()
        if group == self.last_group:
            return
        self.last_group = group
        if group in ("g_b1", "g_b2", "g_b3", "g_b4"):
            self.selected_band = int(group[-1]) - 1

    def _read_state(self):
        return {
            "b1_freq":   self._get("b1_freq", 100.0),
            "b1_gain":   self._get("b1_gain", 0.0),
            "b1_q":      self._get("b1_q", 0.71),
            "b1_type":   self._get("b1_type", 0.0),
            "b2_freq":   self._get("b2_freq", 400.0),
            "b2_gain":   self._get("b2_gain", 0.0),
            "b2_q":      self._get("b2_q", 1.0),
            "b2_type":   self._get("b2_type", 0.0),
            "b3_freq":   self._get("b3_freq", 2200.0),
            "b3_gain":   self._get("b3_gain", 0.0),
            "b3_q":      self._get("b3_q", 1.0),
            "b3_type":   self._get("b3_type", 0.0),
            "b4_freq":   self._get("b4_freq", 8000.0),
            "b4_gain":   self._get("b4_gain", 0.0),
            "b4_q":      self._get("b4_q", 0.71),
            "b4_type":   self._get("b4_type", 0.0),
            "hpf_freq":  self._get("hpf_freq", 20.0),
            "lpf_freq":  self._get("lpf_freq", 20000.0),
            "tilt_db":   self._get("tilt_db", 0.0),
            "slope":     self._get("slope", 0.0),
            "scale_pct": self._get("scale_pct", 100.0),
            "solo_band": self._get("solo_band", 0.0),
            "auto_gain": self._get("auto_gain", 0.0),
            "output_db": self._get("output_db", 0.0),
        }

    def _freq_to_x(self, f, x0, x1):
        t = math.log(_clamp(f, F_MIN, F_MAX) / F_MIN) / math.log(F_MAX / F_MIN)
        return x0 + t * (x1 - x0)

    def _x_to_freq(self, x, x0, x1):
        t = _clamp((x - x0) / max(1.0, x1 - x0), 0.0, 1.0)
        return F_MIN * ((F_MAX / F_MIN) ** t)

    def _db_to_y(self, db, y0, y1):
        t = (_clamp(db, -DB_RANGE, DB_RANGE) + DB_RANGE) / (2.0 * DB_RANGE)
        return y1 - t * (y1 - y0)

    def _y_to_db(self, y, y0, y1):
        t = _clamp((y1 - y) / max(1.0, y1 - y0), 0.0, 1.0)
        return -DB_RANGE + t * (2.0 * DB_RANGE)

    def _pull_telemetry(self):
        in_db = _clamp(float(self.monitors.get("in_peak", SPEC_FLOOR)), SPEC_FLOOR, 12.0)
        out_db = _clamp(float(self.monitors.get("out_peak", SPEC_FLOOR)), SPEC_FLOOR, 12.0)
        self.hold_in = in_db if in_db >= self.hold_in else max(in_db, self.hold_in - 1.2)
        self.hold_out = out_db if out_db >= self.hold_out else max(out_db, self.hold_out - 1.2)

        for i in range(SPEC_BINS):
            raw = _clamp(float(self.monitors.get(f"spec_{i}", SPEC_FLOOR)), SPEC_FLOOR, 0.0)
            prev = self.spec_smooth[i]
            self.spec_smooth[i] = raw if raw >= prev else prev - 2.2
        return in_db, out_db

    def refresh_gui(self):
        self._follow_page()
        cv = self.widget_canvas
        w = max(240, int(self.width or cv.winfo_width() or 640))
        h = max(180, int(self.height or cv.winfo_height() or 380))
        st = self._read_state()
        in_db, out_db = self._pull_telemetry()

        cv.delete("all")
        self.hit_rects.clear()
        self.node_pos.clear()

        pad = 6
        btn_h = max(34, min(42, int(h * 0.11)))
        info_h = max(24, min(30, int(h * 0.075)))
        meter_w = 42

        gx0 = pad + 26
        gy0 = pad + 2
        gx1 = w - pad - meter_w - 6
        gy1 = h - pad - btn_h - info_h - 10
        self.graph_box = (gx0, gy0, gx1, gy1)

        # Graph background & grid
        cv.create_rectangle(gx0, gy0, gx1, gy1, fill=PANEL_BG, outline=LINE_DIM, width=1)
        for db in (-12, -6, 0, 6, 12):
            yy = self._db_to_y(db, gy0, gy1)
            cv.create_line(gx0, yy, gx1, yy, fill=ZERO_COL if db == 0 else GRID_COL,
                           width=1.5 if db == 0 else 1)
            cv.create_text(gx0 - 4, yy, text=f"{db:+d}" if db else "0", anchor="e",
                           fill=LABEL_DIM, font=("TkDefaultFont", 8))

        for fr, lbl in ((50, "50"), (100, "100"), (200, "200"), (500, "500"),
                        (1000, "1k"), (2000, "2k"), (5000, "5k"), (10000, "10k")):
            xx = self._freq_to_x(fr, gx0, gx1)
            cv.create_line(xx, gy0, xx, gy1, fill=GRID_COL, width=1)
            cv.create_text(xx, gy1 - 3, text=lbl, anchor="s", fill=LABEL_DIM, font=("TkDefaultFont", 8))

        # 24-bin live spectrum silhouette behind EQ curve
        spec_pts = [gx0, gy1]
        spec_line = []
        ratio = (SPEC_F_MAX / SPEC_F_MIN) ** (1.0 / (SPEC_BINS - 1))
        for i in range(SPEC_BINS):
            fc = SPEC_F_MIN * (ratio ** i)
            xx = self._freq_to_x(fc, gx0, gx1)
            norm = _clamp((self.spec_smooth[i] - SPEC_FLOOR) / (0.0 - SPEC_FLOOR), 0.0, 1.0)
            yy = gy1 - norm * (gy1 - gy0) * 0.92
            spec_pts.extend((xx, yy))
            spec_line.extend((xx, yy))
        spec_pts.extend((gx1, gy1))
        cv.create_polygon(*spec_pts, fill=SPEC_FILL, outline="")
        cv.create_line(*spec_line, fill=SPEC_LINE, width=1.2)

        # EQ response curves (selected band highlight + total response)
        g_stages, b_stages = build_stage_coeffs(st)
        all_stages = list(g_stages)
        for bs in b_stages:
            all_stages.extend(bs)

        n_pts = 84
        sel_pts = []
        tot_pts = []
        sel_stages = b_stages[self.selected_band]
        for i in range(n_pts + 1):
            t = i / float(n_pts)
            f = F_MIN * ((F_MAX / F_MIN) ** t)
            xx = gx0 + t * (gx1 - gx0)
            if sel_stages:
                sdb = eval_stages_db(sel_stages, f)
                sel_pts.extend((xx, self._db_to_y(sdb, gy0, gy1)))
            tdb = eval_stages_db(all_stages, f)
            tot_pts.extend((xx, self._db_to_y(tdb, gy0, gy1)))

        if sel_pts:
            sel_col = BAND_SPECS[self.selected_band][1]
            cv.create_line(*sel_pts, fill=sel_col, width=1.5, dash=(4, 2))
        cv.create_line(*tot_pts, fill=CURVE_COL, width=2.4)

        # HPF & LPF edge handles on 0 dB line
        y0db = self._db_to_y(0.0, gy0, gy1)
        hpf_active = st["hpf_freq"] > 20.5
        hx = self._freq_to_x(st["hpf_freq"], gx0, gx1)
        cv.create_oval(hx - 8, y0db - 8, hx + 8, y0db + 8,
                       fill=CUT_COL if hpf_active else PANEL_BG, outline=CUT_COL, width=1.5)
        cv.create_text(hx, y0db, text="H", fill="#111" if hpf_active else CUT_COL,
                       font=("TkDefaultFont", 8, "bold"))
        self.node_pos["hpf"] = (hx, y0db)

        lpf_active = st["lpf_freq"] < 19900.0
        lx = self._freq_to_x(st["lpf_freq"], gx0, gx1)
        cv.create_oval(lx - 8, y0db - 8, lx + 8, y0db + 8,
                       fill=CUT_COL if lpf_active else PANEL_BG, outline=CUT_COL, width=1.5)
        cv.create_text(lx, y0db, text="L", fill="#111" if lpf_active else CUT_COL,
                       font=("TkDefaultFont", 8, "bold"))
        self.node_pos["lpf"] = (lx, y0db)

        # 4 parametric band nodes
        solo_b = int(round(st["solo_band"]))
        for idx, col, _fmin, _fmax, _qmin, _qmax, types, off_idx in BAND_SPECS:
            pfx = f"b{idx + 1}"
            b_type = int(round(st[f"{pfx}_type"]))
            is_off = (b_type == off_idx)
            is_notch = (idx in (1, 2) and b_type == 1)
            nx = self._freq_to_x(st[f"{pfx}_freq"], gx0, gx1)
            ny = y0db if (is_off or is_notch) else self._db_to_y(st[f"{pfx}_gain"], gy0, gy1)
            r = 13 if idx == self.selected_band else 10
            if solo_b == idx + 1:
                cv.create_oval(nx - r - 4, ny - r - 4, nx + r + 4, ny + r + 4,
                               fill="", outline=SOLO_COL, width=2)
            cv.create_oval(
                nx - r, ny - r, nx + r, ny + r,
                fill=PANEL_BG if is_off else col,
                outline=LABEL_HI if idx == self.selected_band else col,
                width=2.2 if idx == self.selected_band else 1.4,
            )
            cv.create_text(
                nx, ny, text=str(idx + 1),
                fill=col if is_off else "#101318",
                font=("TkDefaultFont", 9, "bold"),
            )
            self.node_pos[f"b{idx}"] = (nx, ny)

        # Top-right badge (Tilt / Scale / Solo indicator)
        badges = []
        if abs(st["tilt_db"]) >= 0.05:
            badges.append(f"TILT {st['tilt_db']:+.1f}dB")
        if abs(st["scale_pct"] - 100.0) >= 0.5:
            badges.append(f"SCALE {st['scale_pct']:.0f}%")
        if solo_b > 0:
            badges.append(f"SOLO B{solo_b}")
        if badges:
            cv.create_text(gx1 - 6, gy0 + 6, text=" · ".join(badges), anchor="ne",
                           fill=SOLO_COL if solo_b > 0 else LABEL_HI,
                           font=("TkDefaultFont", 8, "bold"))

        # Vertical IN / OUT peak meters on the right
        mx0 = gx1 + 6
        mw = (w - pad - mx0 - 4) * 0.5
        for mi, (lbl, val, hold, col) in enumerate((
            ("IN", in_db, self.hold_in, IN_COL),
            ("OUT", out_db, self.hold_out, OUT_COL),
        )):
            bx0 = mx0 + mi * (mw + 4)
            bx1 = bx0 + mw
            cv.create_rectangle(bx0, gy0, bx1, gy1 - 12, fill=PANEL_BG, outline=LINE_DIM, width=1)
            norm = _clamp((val - SPEC_FLOOR) / (6.0 - SPEC_FLOOR), 0.0, 1.0)
            my = (gy1 - 12) - norm * (gy1 - 12 - gy0)
            cv.create_rectangle(bx0 + 1, my, bx1 - 1, gy1 - 13,
                                fill=HOT_RED if val > -0.5 else col, outline="")
            hnorm = _clamp((hold - SPEC_FLOOR) / (6.0 - SPEC_FLOOR), 0.0, 1.0)
            hy = (gy1 - 12) - hnorm * (gy1 - 12 - gy0)
            cv.create_line(bx0 + 1, hy, bx1 - 1, hy, fill=LABEL_HI, width=2)
            cv.create_text((bx0 + bx1) * 0.5, gy1 - 5, text=lbl, fill=LABEL_DIM, font=("TkDefaultFont", 7, "bold"))

        # Selected-band info strip + touch Q slider
        iy0 = gy1 + 4
        iy1 = iy0 + info_h
        sb_idx, sb_col, _fmin, _fmax, qmin, qmax, sb_types, _off = BAND_SPECS[self.selected_band]
        spfx = f"b{sb_idx + 1}"
        st_idx = int(_clamp(round(st[f"{spfx}_type"]), 0, len(sb_types) - 1))
        info_txt = (
            f"B{sb_idx + 1} {sb_types[st_idx]} · {_fmt_freq(st[f'{spfx}_freq'])} · "
            f"{st[f'{spfx}_gain']:+.1f} dB"
        )
        qx0 = gx0 + (gx1 - gx0) * 0.62
        qx1 = w - pad
        self.q_bar_box = (qx0, iy0, qx1, iy1)
        cv.create_rectangle(pad, iy0, qx0 - 6, iy1, fill=PANEL_BG, outline=LINE_DIM, width=1)
        cv.create_text(pad + 8, (iy0 + iy1) * 0.5, text=info_txt, anchor="w",
                       fill=sb_col, font=("TkDefaultFont", 9, "bold"))

        # Interactive Q bar
        cv.create_rectangle(qx0, iy0, qx1, iy1, fill=PANEL_BG, outline=LINE_DIM, width=1)
        qt = math.log(_clamp(st[f"{spfx}_q"], qmin, qmax) / qmin) / math.log(qmax / qmin)
        cv.create_rectangle(qx0 + 1, iy0 + 1, qx0 + 1 + qt * (qx1 - qx0 - 2), iy1 - 1,
                            fill="#22384c", outline="")
        cv.create_text((qx0 + qx1) * 0.5, (iy0 + iy1) * 0.5, text=f"Q {st[f'{spfx}_q']:.2f}",
                       fill=LABEL_HI, font=("TkDefaultFont", 9, "bold"))

        # Bottom touch buttons: 1 | 2 | 3 | 4 | TYPE | SOLO | AUTO
        by0 = iy1 + 4
        by1 = h - pad
        buttons = []
        for idx, col, *_rest in BAND_SPECS:
            off_idx = BAND_SPECS[idx][7]
            is_off = int(round(st[f"b{idx + 1}_type"])) == off_idx
            buttons.append((f"band_{idx}", f"B{idx + 1}", col, idx == self.selected_band, is_off))
        buttons.append(("type_cycle", sb_types[st_idx], sb_col, False, False))
        buttons.append(("solo_toggle", "SOLO", SOLO_COL, solo_b == self.selected_band + 1, False))
        buttons.append(("auto_toggle", "AUTO", AUTO_COL, st["auto_gain"] >= 0.5, False))

        gap = 5
        bw = (w - 2 * pad - gap * (len(buttons) - 1)) / float(len(buttons))
        for i, (key, lbl, col, active, dimmed) in enumerate(buttons):
            bx0 = pad + i * (bw + gap)
            bx1 = bx0 + bw
            self.hit_rects[key] = (bx0, by0, bx1, by1)
            fill = col if active else BUTTON_BG
            outline = col if (active or not dimmed) else LINE_DIM
            txt_col = "#101318" if active else (LABEL_DIM if dimmed else col)
            cv.create_rectangle(bx0, by0, bx1, by1, fill=fill, outline=outline, width=1.6)
            cv.create_text((bx0 + bx1) * 0.5, (by0 + by1) * 0.5, text=lbl,
                           fill=txt_col, font=("TkDefaultFont", 9, "bold"))

    def _apply_node_drag(self, target, x, y):
        gx0, gy0, gx1, gy1 = self.graph_box
        freq = self._x_to_freq(x, gx0, gx1)
        if target == "hpf":
            self._set("hpf_freq", round(_clamp(freq, 20.0, 600.0), 1))
        elif target == "lpf":
            self._set("lpf_freq", round(_clamp(freq, 2000.0, 20000.0), 1))
        elif target.startswith("b"):
            idx = int(target[1:])
            _, _col, fmin, fmax, *_ = BAND_SPECS[idx]
            pfx = f"b{idx + 1}"
            self._set(f"{pfx}_freq", round(_clamp(freq, fmin, fmax), 1))
            gain = self._y_to_db(y, gy0, gy1)
            self._set(f"{pfx}_gain", round(_clamp(gain, -DB_RANGE, DB_RANGE), 2))

    def _apply_q_drag(self, x):
        qx0, _qy0, qx1, _qy1 = self.q_bar_box
        t = _clamp((x - qx0) / max(1.0, qx1 - qx0), 0.0, 1.0)
        idx = self.selected_band
        qmin, qmax = BAND_SPECS[idx][4], BAND_SPECS[idx][5]
        qval = qmin * ((qmax / qmin) ** t)
        self._set(f"b{idx + 1}_q", round(qval, 2))

    def _on_press(self, event):
        x, y = event.x, event.y
        now = time.monotonic()
        self.drag_mode = None

        # 1. Bottom buttons
        for key, (x0, y0, x1, y1) in self.hit_rects.items():
            if x0 <= x <= x1 and y0 <= y <= y1:
                if key.startswith("band_"):
                    idx = int(key.split("_")[1])
                    self.selected_band = idx
                    self._select_page(idx)
                    self.drag_mode = ("tab", idx, now)
                    self.refresh_gui()
                    return
                if key == "type_cycle":
                    idx = self.selected_band
                    types = BAND_SPECS[idx][6]
                    sym = f"b{idx + 1}_type"
                    cur = int(round(self._get(sym, 0.0)))
                    nxt = (cur + 1) % len(types)
                    if nxt != BAND_SPECS[idx][7]:
                        self.saved_types[idx] = nxt
                    self._set(sym, nxt)
                    self.refresh_gui()
                    return
                if key == "solo_toggle":
                    cur = int(round(self._get("solo_band", 0.0)))
                    want = self.selected_band + 1
                    self._set("solo_band", 0 if cur == want else want)
                    self.refresh_gui()
                    return
                if key == "auto_toggle":
                    cur = self._get("auto_gain", 0.0)
                    self._set("auto_gain", 0 if cur >= 0.5 else 1)
                    self.refresh_gui()
                    return

        # 2. Q bar
        qx0, qy0, qx1, qy1 = self.q_bar_box
        if qx0 <= x <= qx1 and qy0 <= y <= qy1:
            self.drag_mode = ("q",)
            self._apply_q_drag(x)
            self.refresh_gui()
            return

        # 3. Graph nodes (closest within 28 px)
        gx0, gy0, gx1, gy1 = self.graph_box
        if gx0 - 14 <= x <= gx1 + 14 and gy0 - 14 <= y <= gy1 + 14:
            best_key = None
            best_d2 = 28.0 * 28.0
            for k, (nx, ny) in self.node_pos.items():
                d2 = (x - nx) ** 2 + (y - ny) ** 2
                if d2 <= best_d2:
                    best_d2 = d2
                    best_key = k
            if best_key is None:
                best_key = f"b{self.selected_band}"

            # Double tap resets band gain to 0 dB or HPF/LPF to bypassed
            prev_k, prev_t = self.last_tap
            self.last_tap = (best_key, now)
            if prev_k == best_key and (now - prev_t) <= DOUBLE_TAP_SEC:
                if best_key == "hpf":
                    self._set("hpf_freq", 20.0)
                elif best_key == "lpf":
                    self._set("lpf_freq", 20000.0)
                elif best_key.startswith("b"):
                    idx = int(best_key[1:])
                    self._set(f"b{idx + 1}_gain", 0.0)
                self.last_tap = (None, 0.0)
                self.refresh_gui()
                return

            if best_key.startswith("b"):
                idx = int(best_key[1:])
                self.selected_band = idx
                self._select_page(idx)
            elif best_key in ("hpf", "lpf"):
                self._select_group("g_cuts")
            self.drag_mode = ("node", best_key)
            self._apply_node_drag(best_key, x, y)
            self.refresh_gui()

    def _on_motion(self, event):
        if not self.drag_mode:
            return
        if self.drag_mode[0] == "node":
            self._apply_node_drag(self.drag_mode[1], event.x, event.y)
            self.refresh_gui()
        elif self.drag_mode[0] == "q":
            self._apply_q_drag(event.x)
            self.refresh_gui()

    def _on_release(self, event):
        if self.drag_mode and self.drag_mode[0] == "tab":
            _, idx, t0 = self.drag_mode
            if (time.monotonic() - t0) >= LONG_PRESS_SEC:
                sym = f"b{idx + 1}_type"
                off_idx = BAND_SPECS[idx][7]
                cur = int(round(self._get(sym, 0.0)))
                if cur == off_idx:
                    self._set(sym, self.saved_types[idx] if self.saved_types[idx] != off_idx else 0)
                else:
                    self.saved_types[idx] = cur
                    self._set(sym, off_idx)
                self.refresh_gui()
        self.drag_mode = None


# Zynthian derives filename & class name from doap:name ("Eka Q-4" -> zynthian_widget_eka_q-4).
globals()["zynthian_widget_eka_q-4"] = zynthian_widget_eka_q4
