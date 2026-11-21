#!/usr/bin/python3
# -*- coding: utf-8 -*-
# SPDX-License-Identifier: GPL-3.0-or-later
# Zynthian widget for Eka Press (class and file name: zynthian_widget_eka_press).
import math
import time
import tkinter

from zyngui import zynthian_gui_config
from zyngui import zynthian_widget_base

MULTI_INSTANCE = True

REFRESH_SEC = 0.08
TRACE_STEPS = 64
FLOOR_DB = -60.0
CEIL_DB = 0.0
MAX_GR_DB = 24.0

COMPRESSOR_MODES = (
    ("CLEAN", "#4dabf7"),
    ("OPTO",  "#63e6be"),
    ("PUNCH", "#ff8787"),
    ("GLUE",  "#ffd43b"),
)

SWITCH_BUTTONS = (
    ("auto_release", "AUTO REL",  "#4dabf7"),
    ("auto_makeup",  "AUTO GAIN", "#63e6be"),
    ("sc_listen",    "SC LSTN",   "#ffa94d"),
    ("soft_clip",    "SOFT CLIP", "#b197fc"),
)

PANEL_BG   = "#16191e"
BUTTON_BG  = "#1f242c"
LINE_DIM   = "#2f3640"
GRID_COL   = "#262c35"
LABEL_DIM  = "#8b949e"
LABEL_HI   = "#e6edf3"
IN_FILL    = "#36506c"
OUT_STROKE = "#63e6be"
GR_STROKE  = "#ff922b"
GR_FILL    = "#5c3314"
HOT_RED    = "#ff6b6b"


def _limit(val, lo, hi):
    if val < lo:
        return lo
    if val > hi:
        return hi
    return val


def static_reduction_db(level_db, threshold_db, ratio, knee_db):
    r = _limit(ratio, 1.0, 20.0)
    inv_r = 0.0 if r >= 19.9 else 1.0 / r
    slope = 1.0 - inv_r
    if slope <= 1e-6:
        return 0.0
    excess = level_db - threshold_db
    w = _limit(knee_db, 0.0, 30.0)
    if w <= 0.05:
        return slope * excess if excess > 0.0 else 0.0
    half = 0.5 * w
    if excess <= -half:
        return 0.0
    if excess >= half:
        return slope * excess
    t = excess + half
    return slope * (t * t) / (2.0 * w)


def mode_knee_db(knee_db, mode_idx):
    if mode_idx == 1:
        return _limit(knee_db + 6.0, 0.0, 24.0)
    if mode_idx == 2:
        return knee_db * 0.65
    if mode_idx == 3:
        return _limit(knee_db + 3.0, 0.0, 21.0)
    return knee_db


class zynthian_widget_eka_press(zynthian_widget_base.zynthian_widget_base):
    def __init__(self, parent):
        super().__init__(parent)
        self.next_tick = 0.0
        self.in_ring = [FLOOR_DB] * TRACE_STEPS
        self.out_ring = [FLOOR_DB] * TRACE_STEPS
        self.gr_ring = [0.0] * TRACE_STEPS
        self.hold_in = FLOOR_DB
        self.hold_out = FLOOR_DB
        self.hold_gr = 0.0
        self.hit_rects = {}
        self.active_drag = None
        self.graph_box = (0.0, 0.0, 1.0, 1.0)

        self.widget_canvas = tkinter.Canvas(
            self,
            highlightthickness=0,
            relief="flat",
            bg=zynthian_gui_config.color_bg,
        )
        self.widget_canvas.grid(row=0, column=0, sticky="news")
        self.widget_canvas.bind("<ButtonPress-1>", self._handle_press)
        self.widget_canvas.bind("<B1-Motion>", self._handle_motion)
        self.widget_canvas.bind("<ButtonRelease-1>", self._handle_release)

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

    def _param(self, sym, fallback=0.0):
        try:
            v = float(self.processor.controllers_dict[sym].value)
            return v if math.isfinite(v) else fallback
        except (AttributeError, KeyError, TypeError, ValueError):
            return fallback

    def _write_param(self, sym, val):
        try:
            ctrl = self.processor.controllers_dict.get(sym)
            if ctrl is not None:
                ctrl.set_value(val)
                return True
        except Exception:
            pass
        return False

    def _meter(self, sym, fallback=0.0):
        try:
            v = float((self.monitors or {}).get(sym, fallback))
            return v if math.isfinite(v) else fallback
        except (TypeError, ValueError):
            return fallback

    def _handle_press(self, event):
        px, py = event.x, event.y
        self.active_drag = None

        for idx in range(len(COMPRESSOR_MODES)):
            box = self.hit_rects.get(f"mode_{idx}")
            if box and box[0] <= px <= box[2] and box[1] <= py <= box[3]:
                self._write_param("mode", idx)
                self.refresh_gui()
                return

        for sym, _, _ in SWITCH_BUTTONS:
            box = self.hit_rects.get(f"sw_{sym}")
            if box and box[0] <= px <= box[2] and box[1] <= py <= box[3]:
                cur = 1 if self._param(sym, 0.0) >= 0.5 else 0
                self._write_param(sym, 0 if cur else 1)
                self.refresh_gui()
                return

        gx0, gy0, gx1, gy1 = self.graph_box
        if gx0 <= px <= gx1 and gy0 <= py <= gy1:
            self.active_drag = "transfer"
            self._drag_transfer(px, py)

    def _handle_motion(self, event):
        if self.active_drag == "transfer":
            self._drag_transfer(event.x, event.y)

    def _handle_release(self, _event):
        self.active_drag = None

    def _drag_transfer(self, px, py):
        gx0, gy0, gx1, gy1 = self.graph_box
        nx = _limit((px - gx0) / max(1.0, gx1 - gx0), 0.0, 1.0)
        ny = _limit((py - gy0) / max(1.0, gy1 - gy0), 0.0, 1.0)
        thresh = FLOOR_DB + nx * (CEIL_DB - FLOOR_DB)
        ratio = 20.0 ** ny
        self._write_param("threshold", round(thresh, 1))
        self._write_param("ratio", round(_limit(ratio, 1.0, 20.0), 2))
        self.refresh_gui()

    def refresh_gui(self):
        cv = self.widget_canvas
        w = max(320, self.width or cv.winfo_width() or 600)
        h = max(200, self.height or cv.winfo_height() or 360)

        thresh = self._param("threshold", -18.0)
        ratio = self._param("ratio", 4.0)
        makeup = self._param("makeup", 0.0)
        mix = self._param("mix", 100.0)
        atk = self._param("attack", 10.0)
        rel = self._param("release", 150.0)
        knee = self._param("knee", 6.0)
        mode_idx = int(_limit(round(self._param("mode", 0.0)), 0, len(COMPRESSOR_MODES) - 1))
        hpf = self._param("sc_hpf", 20.0)
        drive = self._param("drive", 0.0)
        auto_rel = self._param("auto_release", 0.0) >= 0.5
        auto_mk = self._param("auto_makeup", 0.0) >= 0.5
        sc_lstn = self._param("sc_listen", 0.0) >= 0.5
        sclip = self._param("soft_clip", 1.0) >= 0.5

        in_db = _limit(self._meter("in_peak", FLOOR_DB), FLOOR_DB, 6.0)
        out_db = _limit(self._meter("out_peak", FLOOR_DB), FLOOR_DB, 6.0)
        sc_db = _limit(self._meter("sc_peak", FLOOR_DB), FLOOR_DB, 6.0)
        gr_db = _limit(self._meter("gr_db", 0.0), 0.0, 36.0)

        self.in_ring.pop(0)
        self.in_ring.append(in_db)
        self.out_ring.pop(0)
        self.out_ring.append(out_db)
        self.gr_ring.pop(0)
        self.gr_ring.append(gr_db)

        self.hold_in = max(in_db, self.hold_in - 1.2)
        self.hold_out = max(out_db, self.hold_out - 1.2)
        self.hold_gr = max(gr_db, self.hold_gr - 0.8)

        mode_label, accent = COMPRESSOR_MODES[mode_idx]
        eff_k = mode_knee_db(knee, mode_idx)

        cv.delete("all")
        self.hit_rects = {}

        margin = 8
        footer_h = max(62, int(h * 0.22))
        strip_h = 22
        top_y = margin
        body_h = max(100, h - footer_h - strip_h - margin * 3)
        bot_y = top_y + body_h

        curve_w = int((w - margin * 4) * 0.36)
        meter_w = max(84, int((w - margin * 4) * 0.19))
        trace_w = max(100, w - margin * 4 - curve_w - meter_w)

        gx0 = margin
        gx1 = gx0 + curve_w
        tx0 = gx1 + margin
        tx1 = tx0 + trace_w
        mx0 = tx1 + margin
        mx1 = w - margin
        self.graph_box = (gx0, top_y, gx1, bot_y)

        font_sm = ("DejaVu Sans", max(8, int(h * 0.028)))
        font_md = ("DejaVu Sans", max(9, int(h * 0.032)), "bold")
        font_gr = ("DejaVu Sans", max(11, int(h * 0.044)), "bold")

        # Panel 1: Transfer Curve
        cv.create_rectangle(gx0, top_y, gx1, bot_y, fill=PANEL_BG, outline=LINE_DIM, width=1)
        for tick in (-48, -36, -24, -12):
            f = (tick - FLOOR_DB) / (CEIL_DB - FLOOR_DB)
            cv.create_line(gx0 + f * curve_w, top_y, gx0 + f * curve_w, bot_y, fill=GRID_COL)
            cv.create_line(gx0, bot_y - f * body_h, gx1, bot_y - f * body_h, fill=GRID_COL)
        cv.create_line(gx0, bot_y, gx1, top_y, fill="#39414d", dash=(3, 3))

        tf = _limit((thresh - FLOOR_DB) / (CEIL_DB - FLOOR_DB), 0.0, 1.0)
        cv.create_line(gx0 + tf * curve_w, top_y, gx0 + tf * curve_w, bot_y, fill=accent, dash=(2, 3))

        pts = []
        n_seg = 48
        for i in range(n_seg + 1):
            lv = FLOOR_DB + (CEIL_DB - FLOOR_DB) * (i / n_seg)
            y_db = _limit(lv - static_reduction_db(lv, thresh, ratio, eff_k), FLOOR_DB, CEIL_DB)
            pts.extend((gx0 + (i / n_seg) * curve_w, bot_y - ((y_db - FLOOR_DB) / (CEIL_DB - FLOOR_DB)) * body_h))
        cv.create_line(*pts, fill=accent, width=2)

        sc_cl = _limit(sc_db, FLOOR_DB, CEIL_DB)
        op_y_db = _limit(sc_cl - gr_db, FLOOR_DB, CEIL_DB)
        dx_pt = gx0 + ((sc_cl - FLOOR_DB) / (CEIL_DB - FLOOR_DB)) * curve_w
        dy_pt = bot_y - ((op_y_db - FLOOR_DB) / (CEIL_DB - FLOOR_DB)) * body_h
        dy_raw = bot_y - ((sc_cl - FLOOR_DB) / (CEIL_DB - FLOOR_DB)) * body_h
        if gr_db > 0.2:
            cv.create_line(dx_pt, dy_raw, dx_pt, dy_pt, fill=GR_STROKE, width=2)
        cv.create_oval(dx_pt - 4, dy_pt - 4, dx_pt + 4, dy_pt + 4, fill=LABEL_HI, outline=accent, width=2)

        r_txt = "LIM" if ratio >= 19.9 else f"{ratio:.1f}:1"
        cv.create_text(gx0 + 6, top_y + 5, anchor="nw", text=f"THR {thresh:.0f}dB  {r_txt}",
                       fill=LABEL_HI, font=font_sm)

        # Panel 2: Scrolling Timeline
        cv.create_rectangle(tx0, top_y, tx1, bot_y, fill=PANEL_BG, outline=LINE_DIM, width=1)
        for tick in (-48, -36, -24, -12):
            gy = bot_y - ((tick - FLOOR_DB) / (CEIL_DB - FLOOR_DB)) * body_h
            cv.create_line(tx0, gy, tx1, gy, fill=GRID_COL)
        cv.create_line(tx0, bot_y - tf * body_h, tx1, bot_y - tf * body_h, fill=accent, dash=(2, 4))

        step_x = trace_w / max(1, TRACE_STEPS - 1)
        in_poly = [tx0, bot_y]
        out_pts = []
        gr_poly = [tx0, top_y]
        gr_pts = []
        for i in range(TRACE_STEPS):
            px = tx0 + i * step_x
            yi = bot_y - _limit((self.in_ring[i] - FLOOR_DB) / (CEIL_DB - FLOOR_DB), 0.0, 1.0) * body_h
            yo = bot_y - _limit((self.out_ring[i] - FLOOR_DB) / (CEIL_DB - FLOOR_DB), 0.0, 1.0) * body_h
            yg = top_y + _limit(self.gr_ring[i] / MAX_GR_DB, 0.0, 1.0) * (body_h * 0.72)
            in_poly.extend((px, yi))
            out_pts.extend((px, yo))
            gr_poly.extend((px, yg))
            gr_pts.extend((px, yg))
        in_poly.extend((tx1, bot_y))
        gr_poly.extend((tx1, top_y))

        cv.create_polygon(*in_poly, fill=IN_FILL, outline="")
        cv.create_line(*out_pts, fill=OUT_STROKE, width=1.5)
        cv.create_polygon(*gr_poly, fill=GR_FILL, outline="")
        cv.create_line(*gr_pts, fill=GR_STROKE, width=2)

        gr_label = f"-{gr_db:.1f} dB" if gr_db >= 0.05 else "0.0 dB"
        cv.create_text(tx1 - 8, top_y + 6, anchor="ne", text=f"GR {gr_label}",
                       fill=GR_STROKE if gr_db >= 0.5 else LABEL_DIM, font=font_gr)
        if sc_lstn:
            cv.create_text(tx0 + 8, top_y + 6, anchor="nw", text="SC LISTEN",
                           fill="#ffa94d", font=font_md)

        # Panel 3: Vertical Peak Meters
        cv.create_rectangle(mx0, top_y, mx1, bot_y, fill=PANEL_BG, outline=LINE_DIM, width=1)
        mp = 6
        bar_y0 = top_y + 18
        bar_h = body_h - 24
        bar_y1 = bar_y0 + bar_h
        col_w = (meter_w - mp * 4) / 3.0

        def draw_meter(col_idx, title, val, pk, col, downward=False):
            bx0 = mx0 + mp + col_idx * (col_w + mp)
            bx1 = bx0 + col_w
            cv.create_text((bx0 + bx1) * 0.5, top_y + 9, text=title,
                           fill=GR_STROKE if downward else LABEL_DIM, font=font_sm)
            cv.create_rectangle(bx0, bar_y0, bx1, bar_y1, fill="#0e1116", outline="")
            if downward:
                frac = _limit(val / MAX_GR_DB, 0.0, 1.0)
                pfrac = _limit(pk / MAX_GR_DB, 0.0, 1.0)
                if frac > 0.005:
                    cv.create_rectangle(bx0 + 1, bar_y0, bx1 - 1, bar_y0 + frac * bar_h, fill=col, outline="")
                py = bar_y0 + pfrac * bar_h
            else:
                frac = _limit((val - FLOOR_DB) / (CEIL_DB - FLOOR_DB), 0.0, 1.0)
                pfrac = _limit((pk - FLOOR_DB) / (CEIL_DB - FLOOR_DB), 0.0, 1.0)
                fill_c = HOT_RED if val > -0.5 else col
                if frac > 0.005:
                    cv.create_rectangle(bx0 + 1, bar_y1 - frac * bar_h, bx1 - 1, bar_y1, fill=fill_c, outline="")
                py = bar_y1 - pfrac * bar_h
            cv.create_line(bx0, py, bx1, py, fill=LABEL_HI, width=2)

        draw_meter(0, "IN", in_db, self.hold_in, "#4dabf7", downward=False)
        draw_meter(1, "GR", gr_db, self.hold_gr, GR_STROKE, downward=True)
        draw_meter(2, "OUT", out_db, self.hold_out, OUT_STROKE, downward=False)

        # Status readout line
        sy = bot_y + margin
        hpf_str = "OFF" if hpf <= 20.5 else f"{hpf:.0f}Hz"
        info = (
            f"MODE {mode_label}   ATK {atk:.1f}ms   REL {rel:.0f}ms   "
            f"KNEE {knee:.0f}dB   HPF {hpf_str}   DRV {drive:.0f}%   "
            f"MKP {makeup:+.1f}dB   MIX {mix:.0f}%"
        )
        cv.create_text(w * 0.5, sy + strip_h * 0.5, text=info, fill=LABEL_DIM, font=font_sm)

        # Footer buttons: Row 1 (Modes), Row 2 (Switches)
        fy0 = sy + strip_h + 4
        rh = max(24, (h - fy0 - margin - 4) // 2)
        gap = 6
        bw = (w - margin * 2 - gap * 3) / 4.0

        for i, (m_name, m_col) in enumerate(COMPRESSOR_MODES):
            x0 = margin + i * (bw + gap)
            x1 = x0 + bw
            y0, y1 = fy0, fy0 + rh
            sel = (i == mode_idx)
            cv.create_rectangle(x0, y0, x1, y1, fill=m_col if sel else BUTTON_BG,
                                outline=m_col if sel else LINE_DIM, width=2 if sel else 1)
            cv.create_text((x0 + x1) * 0.5, (y0 + y1) * 0.5, text=m_name,
                           fill="#0b0d10" if sel else LABEL_HI, font=font_md)
            self.hit_rects[f"mode_{i}"] = (x0, y0, x1, y1)

        ry0 = fy0 + rh + 4
        ry1 = min(h - margin, ry0 + rh)
        sw_map = {
            "auto_release": auto_rel,
            "auto_makeup": auto_mk,
            "sc_listen": sc_lstn,
            "soft_clip": sclip,
        }
        for i, (sym, lbl, col) in enumerate(SWITCH_BUTTONS):
            x0 = margin + i * (bw + gap)
            x1 = x0 + bw
            on = sw_map[sym]
            cv.create_rectangle(x0, ry0, x1, ry1, fill=col if on else BUTTON_BG,
                                outline=col if on else LINE_DIM, width=2 if on else 1)
            cv.create_text((x0 + x1) * 0.5, (ry0 + ry1) * 0.5, text=lbl,
                           fill="#0b0d10" if on else LABEL_DIM, font=font_sm)
            self.hit_rects[f"sw_{sym}"] = (x0, ry0, x1, ry1)
