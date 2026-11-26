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

REFRESH_SEC = 0.10
KNOB_ACTIVE_HOLD_SEC = 0.30
KNOB_MONITOR_SEC = 0.25
DRAG_THROTTLE_SEC = 0.04
LONG_PRESS_MS = 500
LONG_PRESS_SLOP = 16
TRACE_STEPS = 40
CURVE_STEPS = 32
FLOOR_DB = -60.0
CEIL_DB = 0.0
MAX_GR_DB = 24.0

PARAM_KEYS = (
    "threshold",
    "ratio",
    "makeup",
    "mix",
    "attack",
    "release",
    "knee",
    "mode",
    "sc_hpf",
    "drive",
    "auto_release",
    "auto_makeup",
    "sc_listen",
    "soft_clip",
    "bypass",
)

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
IN_BAR_COL = "#4dabf7"
SC_BADGE_C = "#ffa94d"

# Monochrome greyscale palette used when bypass is active
BYP_ACCENT   = "#888888"
BYP_STROKE   = "#6e6e6e"
BYP_IN_FILL  = "#2c2c2c"
BYP_GR_FILL  = "#343434"
BYP_TEXT     = "#909090"
BYP_TEXT_DIM = "#666666"
BYP_BTN_ON   = "#4e4e4e"
BYP_BTN_TEXT = "#d0d0d0"


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
        self._last_monitor_ts = 0.0
        self._last_param_ts = 0.0
        self._last_update_params = None
        self._pending_trace_step = False
        self.in_ring = [FLOOR_DB] * TRACE_STEPS
        self.out_ring = [FLOOR_DB] * TRACE_STEPS
        self.gr_ring = [0.0] * TRACE_STEPS
        self.hold_in = FLOOR_DB
        self.hold_out = FLOOR_DB
        self.hold_gr = 0.0
        self.hit_rects = {}
        self.active_drag = None
        self._drag_last_ts = 0.0
        self._drag_pending = None
        self.graph_box = (0.0, 0.0, 1.0, 1.0)
        self._mode_pressed = False
        self._mode_press_ts = 0.0
        self._mode_long_fired = False
        self._mode_after_id = None

        # Retained canvas item handles and dirty-tracking caches
        self._layout_size = None
        self._geom = None
        self._trace_xs = ()
        self._items = {}
        self._reset_caches()

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

    def _reset_caches(self):
        self._c_palette = None
        self._c_thresh_px = None
        self._c_curve = None
        self._c_op = None
        self._c_p1_mode = None
        self._c_p1_tr = None
        self._c_gr_txt = None
        self._c_sc_badge = None
        self._c_status = None
        self._c_sw = [None] * len(SWITCH_BUTTONS)
        self._c_mode_btn = None
        self._c_meters = [None, None, None]
        self._c_trace_audio_flat = None
        self._c_trace_gr_flat = None

    def _read_param_snapshot(self):
        proc = self.processor
        if proc is None:
            return None
        ctrls = getattr(proc, "controllers_dict", None)
        if not ctrls:
            return None
        vals = []
        for sym in PARAM_KEYS:
            ctrl = ctrls.get(sym)
            vals.append(ctrl.value if ctrl is not None else None)
        return tuple(vals)

    def update(self):
        if self.processor is None:
            return

        now = time.monotonic()
        snap = self._read_param_snapshot()
        params_changed = (snap != self._last_update_params)
        if params_changed:
            self._last_update_params = snap
            self._last_param_ts = now

        knob_active = (now - self._last_param_ts) < KNOB_ACTIVE_HOLD_SEC
        interval = KNOB_MONITOR_SEC if knob_active else REFRESH_SEC

        if (now - self._last_monitor_ts) >= interval:
            self._last_monitor_ts = now
            self._pending_trace_step = True
            try:
                super().update()
            finally:
                self._pending_trace_step = False
        elif params_changed and self._layout_size is not None:
            # Fast incremental redraw without blocking on Jalv monitor IPC
            self._pending_trace_step = False
            self.refresh_gui()

    def set_processor(self, processor):
        super().set_processor(processor)
        self._last_update_params = None
        self._last_monitor_ts = 0.0

    def on_size(self, event):
        if not super().on_size(event):
            return
        self.widget_canvas.grid(row=0, column=0, sticky="news")
        self._layout_size = None
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

    def _cancel_mode_timer(self):
        if self._mode_after_id is not None:
            try:
                self.after_cancel(self._mode_after_id)
            except Exception:
                pass
            self._mode_after_id = None

    def _toggle_bypass(self):
        cur = 1 if self._param("bypass", 0.0) >= 0.5 else 0
        self._write_param("bypass", 0 if cur else 1)
        self._last_param_ts = time.monotonic()
        self.refresh_gui()

    def _on_mode_long_press(self):
        self._mode_after_id = None
        if not self._mode_pressed:
            return
        self._mode_long_fired = True
        self._toggle_bypass()

    def _handle_press(self, event):
        px, py = event.x, event.y
        self.active_drag = None
        self._drag_pending = None
        self._cancel_mode_timer()
        self._mode_pressed = False
        self._mode_long_fired = False

        box = self.hit_rects.get("mode_cycle")
        if box and box[0] <= px <= box[2] and box[1] <= py <= box[3]:
            self._mode_pressed = True
            self._mode_press_ts = time.monotonic()
            if hasattr(self, "after"):
                try:
                    self._mode_after_id = self.after(LONG_PRESS_MS, self._on_mode_long_press)
                except Exception:
                    self._mode_after_id = None
            return

        for sym, _, _ in SWITCH_BUTTONS:
            box = self.hit_rects.get(f"sw_{sym}")
            if box and box[0] <= px <= box[2] and box[1] <= py <= box[3]:
                cur = 1 if self._param(sym, 0.0) >= 0.5 else 0
                self._write_param(sym, 0 if cur else 1)
                self._last_param_ts = time.monotonic()
                self.refresh_gui()
                return

        gx0, gy0, gx1, gy1 = self.graph_box
        if gx0 <= px <= gx1 and gy0 <= py <= gy1:
            self.active_drag = "transfer"
            self._drag_last_ts = 0.0
            self._drag_transfer(px, py, force=True)

    def _handle_motion(self, event):
        if self._mode_pressed:
            box = self.hit_rects.get("mode_cycle")
            if not box or not (
                box[0] - LONG_PRESS_SLOP <= event.x <= box[2] + LONG_PRESS_SLOP
                and box[1] - LONG_PRESS_SLOP <= event.y <= box[3] + LONG_PRESS_SLOP
            ):
                self._cancel_mode_timer()
                self._mode_pressed = False
            return
        if self.active_drag == "transfer":
            self._drag_transfer(event.x, event.y, force=False)

    def _handle_release(self, _event):
        if self.active_drag == "transfer" and self._drag_pending is not None:
            thresh, ratio = self._drag_pending
            self._drag_pending = None
            self._apply_transfer(thresh, ratio)
        self.active_drag = None
        if self._mode_pressed:
            self._cancel_mode_timer()
            self._mode_pressed = False
            if not self._mode_long_fired:
                if (time.monotonic() - self._mode_press_ts) >= (LONG_PRESS_MS / 1000.0):
                    self._toggle_bypass()
                else:
                    cur_mode = int(_limit(round(self._param("mode", 0.0)), 0, len(COMPRESSOR_MODES) - 1))
                    self._write_param("mode", (cur_mode + 1) % len(COMPRESSOR_MODES))
                    self._last_param_ts = time.monotonic()
                    self.refresh_gui()
            self._mode_long_fired = False

    def _apply_transfer(self, thresh, ratio):
        changed = False
        if abs(thresh - self._param("threshold", -18.0)) > 1e-4:
            self._write_param("threshold", thresh)
            changed = True
        if abs(ratio - self._param("ratio", 4.0)) > 1e-4:
            self._write_param("ratio", ratio)
            changed = True
        if changed:
            self._last_param_ts = time.monotonic()
            self.refresh_gui()

    def _drag_transfer(self, px, py, force=False):
        gx0, gy0, gx1, gy1 = self.graph_box
        nx = _limit((px - gx0) / max(1.0, gx1 - gx0), 0.0, 1.0)
        ny = _limit((py - gy0) / max(1.0, gy1 - gy0), 0.0, 1.0)
        thresh = round(FLOOR_DB + nx * (CEIL_DB - FLOOR_DB), 1)
        ratio = round(_limit(20.0 ** ny, 1.0, 20.0), 2)
        now = time.monotonic()
        if not force and (now - self._drag_last_ts) < DRAG_THROTTLE_SEC:
            self._drag_pending = (thresh, ratio)
            return
        self._drag_last_ts = now
        self._drag_pending = None
        self._apply_transfer(thresh, ratio)

    def _build_layout(self, w, h):
        cv = self.widget_canvas
        cv.delete("all")
        self._items = {}
        self.hit_rects = {}
        self._reset_caches()

        margin = 8
        bot_pad = 4
        footer_h = max(28, int(h * 0.10))
        strip_h = 20
        top_y = margin
        body_h = max(110, h - footer_h - strip_h - margin * 2 - bot_pad - 4)
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

        mp = 6
        bar_y0 = top_y + 18
        bar_h = body_h - 24
        bar_y1 = bar_y0 + bar_h
        col_w = (meter_w - mp * 4) / 3.0
        meter_cols = []
        for col_idx in range(3):
            bx0 = mx0 + mp + col_idx * (col_w + mp)
            bx1 = bx0 + col_w
            meter_cols.append((int(round(bx0)), int(round(bx1))))

        self._geom = {
            "w": w, "h": h,
            "top_y": top_y, "bot_y": bot_y, "body_h": body_h,
            "gx0": gx0, "gx1": gx1, "curve_w": curve_w,
            "tx0": tx0, "tx1": tx1, "trace_w": trace_w,
            "mx0": mx0, "mx1": mx1, "meter_w": meter_w,
            "bar_y0": bar_y0, "bar_y1": bar_y1, "bar_h": bar_h,
            "meter_cols": meter_cols,
        }

        step_x = trace_w / max(1, TRACE_STEPS - 1)
        self._trace_xs = tuple(int(round(tx0 + i * step_x)) for i in range(TRACE_STEPS))

        font_sm = ("DejaVu Sans", max(8, int(h * 0.028)))
        font_md = ("DejaVu Sans", max(9, int(h * 0.032)), "bold")
        font_gr = ("DejaVu Sans", max(11, int(h * 0.044)), "bold")

        items = self._items

        # Panel 1: Transfer Curve (static chrome + dynamic items)
        cv.create_rectangle(gx0, top_y, gx1, bot_y, fill=PANEL_BG, outline=LINE_DIM, width=1)
        for tick in (-48, -36, -24, -12):
            f = (tick - FLOOR_DB) / (CEIL_DB - FLOOR_DB)
            vx = int(round(gx0 + f * curve_w))
            hy = int(round(bot_y - f * body_h))
            cv.create_line(vx, top_y, vx, bot_y, fill=GRID_COL)
            cv.create_line(gx0, hy, gx1, hy, fill=GRID_COL)
        cv.create_line(gx0, bot_y, gx1, top_y, fill="#39414d", dash=(3, 3))

        items["p1_thresh"] = cv.create_line(gx0, top_y, gx0, bot_y, fill=COMPRESSOR_MODES[0][1], dash=(2, 3))
        items["p1_curve"] = cv.create_line(gx0, bot_y, gx1, top_y, fill=COMPRESSOR_MODES[0][1], width=2)
        items["p1_drop"] = cv.create_line(gx0, bot_y, gx0, bot_y, fill=GR_STROKE, width=2, state="hidden")
        items["p1_dot"] = cv.create_oval(
            gx0 - 4, bot_y - 4, gx0 + 4, bot_y + 4,
            fill=LABEL_HI, outline=COMPRESSOR_MODES[0][1], width=2,
        )
        items["p1_mode_txt"] = cv.create_text(
            gx0 + 6, top_y + 5, anchor="nw", text="", fill=COMPRESSOR_MODES[0][1], font=font_md,
        )
        items["p1_tr_txt"] = cv.create_text(
            gx1 - 6, top_y + 6, anchor="ne", text="", fill=LABEL_HI, font=font_sm,
        )

        # Panel 2: Scrolling Timeline
        cv.create_rectangle(tx0, top_y, tx1, bot_y, fill=PANEL_BG, outline=LINE_DIM, width=1)
        for tick in (-48, -36, -24, -12):
            gy = int(round(bot_y - ((tick - FLOOR_DB) / (CEIL_DB - FLOOR_DB)) * body_h))
            cv.create_line(tx0, gy, tx1, gy, fill=GRID_COL)
        items["p2_thresh"] = cv.create_line(tx0, bot_y, tx1, bot_y, fill=COMPRESSOR_MODES[0][1], dash=(2, 4))

        items["p2_in_poly"] = cv.create_polygon(
            tx0, bot_y, tx1, bot_y, tx1, bot_y, fill=IN_FILL, outline="", state="hidden",
        )
        items["p2_out_line"] = cv.create_line(tx0, bot_y, tx1, bot_y, fill=OUT_STROKE, width=1.5)
        items["p2_gr_poly"] = cv.create_polygon(
            tx0, top_y, tx1, top_y, tx1, top_y, fill=GR_FILL, outline="", state="hidden",
        )
        items["p2_gr_line"] = cv.create_line(tx0, top_y, tx1, top_y, fill=GR_STROKE, width=2)
        items["p2_gr_txt"] = cv.create_text(
            tx1 - 8, top_y + 6, anchor="ne", text="GR 0.0 dB", fill=LABEL_DIM, font=font_gr,
        )
        items["p2_sc_txt"] = cv.create_text(
            tx0 + 8, top_y + 6, anchor="nw", text="SC LISTEN",
            fill=SC_BADGE_C, font=font_md, state="hidden",
        )

        # Panel 3: Vertical Peak Meters
        cv.create_rectangle(mx0, top_y, mx1, bot_y, fill=PANEL_BG, outline=LINE_DIM, width=1)
        titles = ("IN", "GR", "OUT")
        m_titles = []
        m_bars = []
        m_peaks = []
        for idx, title in enumerate(titles):
            bx0, bx1 = meter_cols[idx]
            m_titles.append(cv.create_text(
                (bx0 + bx1) * 0.5, top_y + 9, text=title,
                fill=GR_STROKE if idx == 1 else LABEL_DIM, font=font_sm,
            ))
            cv.create_rectangle(bx0, bar_y0, bx1, bar_y1, fill="#0e1116", outline="")
            init_y = bar_y0 if idx == 1 else bar_y1
            m_bars.append(cv.create_rectangle(
                bx0 + 1, init_y, bx1 - 1, init_y,
                fill=GR_STROKE if idx == 1 else (IN_BAR_COL if idx == 0 else OUT_STROKE),
                outline="", state="hidden",
            ))
            m_peaks.append(cv.create_line(bx0, init_y, bx1, init_y, fill=LABEL_HI, width=2))
        items["m_titles"] = m_titles
        items["m_bars"] = m_bars
        items["m_peaks"] = m_peaks

        # Compact status readout line
        sy = bot_y + margin
        items["status_txt"] = cv.create_text(
            w * 0.5, sy + strip_h * 0.5, text="", fill=LABEL_DIM, font=font_sm,
        )

        # Footer: single row of 5 buttons (4 toggles + 5th mode/bypass button)
        fy0 = sy + strip_h + 4
        fy1 = h - bot_pad
        gap = 5
        bw = (w - margin * 2 - gap * 4) / 5.0

        sw_rects = []
        sw_txts = []
        for i, (sym, lbl, _col) in enumerate(SWITCH_BUTTONS):
            x0 = margin + i * (bw + gap)
            x1 = x0 + bw
            sw_rects.append(cv.create_rectangle(x0, fy0, x1, fy1, fill=BUTTON_BG, outline=LINE_DIM, width=1))
            sw_txts.append(cv.create_text((x0 + x1) * 0.5, (fy0 + fy1) * 0.5, text=lbl, fill=LABEL_DIM, font=font_sm))
            self.hit_rects[f"sw_{sym}"] = (x0, fy0, x1, fy1)
        items["sw_rects"] = sw_rects
        items["sw_txts"] = sw_txts

        mx_b0 = margin + 4 * (bw + gap)
        mx_b1 = mx_b0 + bw
        items["mode_rect"] = cv.create_rectangle(
            mx_b0, fy0, mx_b1, fy1, fill=COMPRESSOR_MODES[0][1], outline=COMPRESSOR_MODES[0][1], width=2,
        )
        items["mode_txt"] = cv.create_text(
            (mx_b0 + mx_b1) * 0.5, (fy0 + fy1) * 0.5, text=COMPRESSOR_MODES[0][0], fill="#0b0d10", font=font_md,
        )
        self.hit_rects["mode_cycle"] = (mx_b0, fy0, mx_b1, fy1)

        self._layout_size = (w, h)

    def refresh_gui(self):
        cv = self.widget_canvas
        w = max(320, self.width or cv.winfo_width() or 600)
        h = max(200, self.height or cv.winfo_height() or 360)

        if self._layout_size != (w, h):
            self._build_layout(w, h)

        g = self._geom
        items = self._items

        thresh = self._param("threshold", -18.0)
        ratio = self._param("ratio", 4.0)
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
        bypassed = self._param("bypass", 0.0) >= 0.5

        in_db = _limit(self._meter("in_peak", FLOOR_DB), FLOOR_DB, 6.0)
        out_db = _limit(self._meter("out_peak", FLOOR_DB), FLOOR_DB, 6.0)
        sc_db = _limit(self._meter("sc_peak", FLOOR_DB), FLOOR_DB, 6.0)
        gr_db = 0.0 if bypassed else _limit(self._meter("gr_db", 0.0), 0.0, 36.0)

        advance_trace = self._pending_trace_step
        if advance_trace:
            self.in_ring.pop(0)
            self.in_ring.append(in_db)
            self.out_ring.pop(0)
            self.out_ring.append(out_db)
            self.gr_ring.pop(0)
            self.gr_ring.append(gr_db)

            self.hold_in = max(in_db, self.hold_in - 1.2)
            self.hold_out = max(out_db, self.hold_out - 1.2)
            self.hold_gr = 0.0 if bypassed else max(gr_db, self.hold_gr - 0.8)
        else:
            if bypassed:
                self.hold_gr = 0.0
                if any(v > 0.0 for v in self.gr_ring):
                    self.gr_ring = [0.0] * TRACE_STEPS
                    self._c_trace_gr_flat = None
            else:
                if in_db > self.hold_in:
                    self.hold_in = in_db
                if out_db > self.hold_out:
                    self.hold_out = out_db
                if gr_db > self.hold_gr:
                    self.hold_gr = gr_db

        mode_label, raw_accent = COMPRESSOR_MODES[mode_idx]
        accent = BYP_ACCENT if bypassed else raw_accent
        in_fill_c = BYP_IN_FILL if bypassed else IN_FILL
        out_stroke_c = BYP_STROKE if bypassed else OUT_STROKE
        gr_fill_c = BYP_GR_FILL if bypassed else GR_FILL
        gr_stroke_c = BYP_STROKE if bypassed else GR_STROKE
        hot_c = BYP_TEXT if bypassed else HOT_RED
        hi_text_c = BYP_TEXT if bypassed else LABEL_HI
        dim_text_c = BYP_TEXT_DIM if bypassed else LABEL_DIM
        in_bar_c = BYP_STROKE if bypassed else IN_BAR_COL
        sc_badge_c = BYP_STROKE if bypassed else SC_BADGE_C
        eff_k = mode_knee_db(knee, mode_idx)

        top_y = g["top_y"]
        bot_y = g["bot_y"]
        body_h = g["body_h"]
        gx0 = g["gx0"]
        curve_w = g["curve_w"]
        tx0 = g["tx0"]
        tx1 = g["tx1"]
        db_span = CEIL_DB - FLOOR_DB
        inv_db_span = 1.0 / db_span

        # 1. Palette updates (only when mode accent or bypass changes)
        pal_key = (mode_idx, bypassed)
        if pal_key != self._c_palette:
            self._c_palette = pal_key
            cv.itemconfigure(items["p1_thresh"], fill=accent)
            cv.itemconfigure(items["p1_curve"], fill=accent)
            cv.itemconfigure(items["p1_drop"], fill=gr_stroke_c)
            cv.itemconfigure(items["p1_dot"], fill=hi_text_c, outline=accent)
            cv.itemconfigure(items["p2_thresh"], fill=accent)
            cv.itemconfigure(items["p2_in_poly"], fill=in_fill_c)
            cv.itemconfigure(items["p2_out_line"], fill=out_stroke_c)
            cv.itemconfigure(items["p2_gr_poly"], fill=gr_fill_c)
            cv.itemconfigure(items["p2_gr_line"], fill=gr_stroke_c)
            cv.itemconfigure(items["m_titles"][0], fill=dim_text_c)
            cv.itemconfigure(items["m_titles"][1], fill=gr_stroke_c)
            cv.itemconfigure(items["m_titles"][2], fill=dim_text_c)
            for idx in range(3):
                cv.itemconfigure(items["m_peaks"][idx], fill=hi_text_c)
            cv.itemconfigure(items["status_txt"], fill=dim_text_c)

        # 2. Threshold lines (Panel 1 vertical + Panel 2 horizontal)
        tf = _limit((thresh - FLOOR_DB) * inv_db_span, 0.0, 1.0)
        tx_px = int(round(gx0 + tf * curve_w))
        ty_px = int(round(bot_y - tf * body_h))
        thresh_key = (tx_px, ty_px)
        if thresh_key != self._c_thresh_px:
            self._c_thresh_px = thresh_key
            cv.coords(items["p1_thresh"], tx_px, top_y, tx_px, bot_y)
            cv.coords(items["p2_thresh"], tx0, ty_px, tx1, ty_px)

        # 3. Transfer Curve (only when threshold/ratio/eff_knee actually changed)
        curve_key = (round(thresh, 2), round(ratio, 3), round(eff_k, 2))
        if curve_key != self._c_curve:
            self._c_curve = curve_key
            pts = []
            inv_n = 1.0 / CURVE_STEPS
            for i in range(CURVE_STEPS + 1):
                frac = i * inv_n
                lv = FLOOR_DB + db_span * frac
                y_db = _limit(lv - static_reduction_db(lv, thresh, ratio, eff_k), FLOOR_DB, CEIL_DB)
                pts.append(int(round(gx0 + frac * curve_w)))
                pts.append(int(round(bot_y - ((y_db - FLOOR_DB) * inv_db_span) * body_h)))
            cv.coords(items["p1_curve"], *pts)

        # 4. Panel 1 Operating Point & GR drop line
        sc_cl = _limit(sc_db, FLOOR_DB, CEIL_DB)
        op_y_db = _limit(sc_cl - gr_db, FLOOR_DB, CEIL_DB)
        dx_pt = int(round(gx0 + ((sc_cl - FLOOR_DB) * inv_db_span) * curve_w))
        dy_pt = int(round(bot_y - ((op_y_db - FLOOR_DB) * inv_db_span) * body_h))
        dy_raw = int(round(bot_y - ((sc_cl - FLOOR_DB) * inv_db_span) * body_h))
        drop_vis = gr_db > 0.2
        op_key = (dx_pt, dy_pt, dy_raw, drop_vis)
        if op_key != self._c_op:
            prev_drop_vis = self._c_op[3] if self._c_op is not None else None
            self._c_op = op_key
            if drop_vis:
                cv.coords(items["p1_drop"], dx_pt, dy_raw, dx_pt, dy_pt)
                if prev_drop_vis is not True:
                    cv.itemconfigure(items["p1_drop"], state="normal")
            elif prev_drop_vis is not False:
                cv.itemconfigure(items["p1_drop"], state="hidden")
            cv.coords(items["p1_dot"], dx_pt - 4, dy_pt - 4, dx_pt + 4, dy_pt + 4)

        # 5. Panel 1 Header texts
        p1_mode_txt = f"{mode_label} (BYP)" if bypassed else mode_label
        p1_mode_key = (p1_mode_txt, accent)
        if p1_mode_key != self._c_p1_mode:
            self._c_p1_mode = p1_mode_key
            cv.itemconfigure(items["p1_mode_txt"], text=p1_mode_txt, fill=accent)

        r_txt = "LIM" if ratio >= 19.9 else f"{ratio:.1f}:1"
        p1_tr_txt = f"{thresh:.0f}dB {r_txt}"
        p1_tr_key = (p1_tr_txt, hi_text_c)
        if p1_tr_key != self._c_p1_tr:
            self._c_p1_tr = p1_tr_key
            cv.itemconfigure(items["p1_tr_txt"], text=p1_tr_txt, fill=hi_text_c)

        # 6. Panel 2 Scrolling Timeline (skip polygon/line writes when flat)
        if advance_trace or self._c_trace_audio_flat is None:
            audio_flat = (
                max(self.in_ring) <= FLOOR_DB + 0.05
                and max(self.out_ring) <= FLOOR_DB + 0.05
            )
            if audio_flat:
                if self._c_trace_audio_flat is not True:
                    self._c_trace_audio_flat = True
                    cv.itemconfigure(items["p2_in_poly"], state="hidden")
                    cv.coords(items["p2_out_line"], tx0, bot_y, tx1, bot_y)
            else:
                was_flat = self._c_trace_audio_flat is not False
                self._c_trace_audio_flat = False
                xs = self._trace_xs
                in_poly = [tx0, bot_y]
                out_pts = []
                in_r = self.in_ring
                out_r = self.out_ring
                for i in range(TRACE_STEPS):
                    px = xs[i]
                    yi = int(round(bot_y - _limit((in_r[i] - FLOOR_DB) * inv_db_span, 0.0, 1.0) * body_h))
                    yo = int(round(bot_y - _limit((out_r[i] - FLOOR_DB) * inv_db_span, 0.0, 1.0) * body_h))
                    in_poly.append(px)
                    in_poly.append(yi)
                    out_pts.append(px)
                    out_pts.append(yo)
                in_poly.append(tx1)
                in_poly.append(bot_y)
                cv.coords(items["p2_in_poly"], *in_poly)
                if was_flat:
                    cv.itemconfigure(items["p2_in_poly"], state="normal")
                cv.coords(items["p2_out_line"], *out_pts)

        if advance_trace or self._c_trace_gr_flat is None:
            gr_flat = max(self.gr_ring) <= 0.05
            if gr_flat:
                if self._c_trace_gr_flat is not True:
                    self._c_trace_gr_flat = True
                    cv.itemconfigure(items["p2_gr_poly"], state="hidden")
                    cv.coords(items["p2_gr_line"], tx0, top_y, tx1, top_y)
            else:
                was_flat = self._c_trace_gr_flat is not False
                self._c_trace_gr_flat = False
                xs = self._trace_xs
                gr_poly = [tx0, top_y]
                gr_pts = []
                gr_r = self.gr_ring
                gr_scale = (body_h * 0.72) / MAX_GR_DB
                for i in range(TRACE_STEPS):
                    px = xs[i]
                    yg = int(round(top_y + _limit(gr_r[i], 0.0, MAX_GR_DB) * gr_scale))
                    gr_poly.append(px)
                    gr_poly.append(yg)
                    gr_pts.append(px)
                    gr_pts.append(yg)
                gr_poly.append(tx1)
                gr_poly.append(top_y)
                cv.coords(items["p2_gr_poly"], *gr_poly)
                if was_flat:
                    cv.itemconfigure(items["p2_gr_poly"], state="normal")
                cv.coords(items["p2_gr_line"], *gr_pts)

        # Panel 2 GR readout + SC LISTEN badge
        if bypassed:
            gr_txt = "BYPASS"
            gr_txt_c = BYP_TEXT
        else:
            gr_label = f"-{gr_db:.1f} dB" if gr_db >= 0.05 else "0.0 dB"
            gr_txt = f"GR {gr_label}"
            gr_txt_c = gr_stroke_c if gr_db >= 0.5 else LABEL_DIM
        gr_txt_key = (gr_txt, gr_txt_c)
        if gr_txt_key != self._c_gr_txt:
            self._c_gr_txt = gr_txt_key
            cv.itemconfigure(items["p2_gr_txt"], text=gr_txt, fill=gr_txt_c)

        sc_badge_key = (sc_lstn, sc_badge_c)
        if sc_badge_key != self._c_sc_badge:
            self._c_sc_badge = sc_badge_key
            cv.itemconfigure(
                items["p2_sc_txt"],
                fill=sc_badge_c,
                state="normal" if sc_lstn else "hidden",
            )

        # 7. Panel 3: Vertical Peak Meters (integer pixel dirty check)
        bar_y0 = g["bar_y0"]
        bar_y1 = g["bar_y1"]
        bar_h = g["bar_h"]
        meter_cols = g["meter_cols"]
        meter_specs = (
            (0, in_db,  self.hold_in,  in_bar_c,     False),
            (1, gr_db,  self.hold_gr,  gr_stroke_c,  True),
            (2, out_db, self.hold_out, out_stroke_c, False),
        )
        m_bars = items["m_bars"]
        m_peaks = items["m_peaks"]
        for col_idx, val, pk, col, downward in meter_specs:
            bx0, bx1 = meter_cols[col_idx]
            if downward:
                frac = _limit(val / MAX_GR_DB, 0.0, 1.0)
                pfrac = _limit(pk / MAX_GR_DB, 0.0, 1.0)
                fill_c = col
                vis = frac > 0.005
                by0 = bar_y0
                by1 = int(round(bar_y0 + frac * bar_h))
                py = int(round(bar_y0 + pfrac * bar_h))
            else:
                frac = _limit((val - FLOOR_DB) * inv_db_span, 0.0, 1.0)
                pfrac = _limit((pk - FLOOR_DB) * inv_db_span, 0.0, 1.0)
                fill_c = hot_c if val > -0.5 else col
                vis = frac > 0.005
                by0 = int(round(bar_y1 - frac * bar_h))
                by1 = bar_y1
                py = int(round(bar_y1 - pfrac * bar_h))

            m_key = (vis, by0, by1, py, fill_c)
            prev = self._c_meters[col_idx]
            if m_key != prev:
                self._c_meters[col_idx] = m_key
                prev_vis = prev[0] if prev is not None else None
                prev_fill = prev[4] if prev is not None else None
                prev_py = prev[3] if prev is not None else None
                if vis:
                    if prev is None or by0 != prev[1] or by1 != prev[2]:
                        cv.coords(m_bars[col_idx], bx0 + 1, by0, bx1 - 1, by1)
                    if fill_c != prev_fill or prev_vis is not True:
                        cv.itemconfigure(m_bars[col_idx], fill=fill_c, state="normal")
                else:
                    if fill_c != prev_fill or prev_vis is not False:
                        cv.itemconfigure(m_bars[col_idx], fill=fill_c, state="hidden")
                if py != prev_py:
                    cv.coords(m_peaks[col_idx], bx0, py, bx1, py)

        # 8. Compact status readout line
        hpf_str = "OFF" if hpf <= 20.5 else f"{hpf:.0f}Hz"
        info = (
            f"A {atk:.1f}ms  R {rel:.0f}ms  K {knee:.0f}dB  "
            f"HPF {hpf_str}  DRV {drive:.0f}%  MIX {mix:.0f}%"
        )
        if info != self._c_status:
            self._c_status = info
            cv.itemconfigure(items["status_txt"], text=info)

        # 9. Footer toggle buttons (only update changed buttons)
        sw_states = (auto_rel, auto_mk, sc_lstn, sclip)
        sw_rects = items["sw_rects"]
        sw_txts = items["sw_txts"]
        for i, (_sym, _lbl, col) in enumerate(SWITCH_BUTTONS):
            on = sw_states[i]
            sw_key = (on, bypassed)
            if sw_key != self._c_sw[i]:
                self._c_sw[i] = sw_key
                if bypassed:
                    btn_fill = BYP_BTN_ON if on else BUTTON_BG
                    btn_out = BYP_STROKE if on else LINE_DIM
                    btn_txt = "#16191e" if on else BYP_TEXT_DIM
                else:
                    btn_fill = col if on else BUTTON_BG
                    btn_out = col if on else LINE_DIM
                    btn_txt = "#0b0d10" if on else LABEL_DIM
                cv.itemconfigure(sw_rects[i], fill=btn_fill, outline=btn_out, width=2 if on else 1)
                cv.itemconfigure(sw_txts[i], fill=btn_txt)

        # 10. 5th Mode / Bypass button
        mode_btn_key = (mode_idx, bypassed)
        if mode_btn_key != self._c_mode_btn:
            self._c_mode_btn = mode_btn_key
            if bypassed:
                cv.itemconfigure(items["mode_rect"], fill=BYP_BTN_ON, outline=BYP_ACCENT)
                cv.itemconfigure(items["mode_txt"], text="BYPASS", fill=BYP_BTN_TEXT)
            else:
                cv.itemconfigure(items["mode_rect"], fill=accent, outline=accent)
                cv.itemconfigure(items["mode_txt"], text=mode_label, fill="#0b0d10")
