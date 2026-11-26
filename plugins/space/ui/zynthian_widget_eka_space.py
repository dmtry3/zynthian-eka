#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# SPDX-License-Identifier: GPL-3.0-or-later
# Zynthian widget for Eka Space (class and file name: zynthian_widget_eka_space).
import math
import time
import tkinter

from zyngui import zynthian_gui_config
from zyngui import zynthian_widget_base

MULTI_INSTANCE = True

REFRESH_SEC = 0.08
TRAIL_STEPS = 28
F_MIN = 20.0
F_MAX = 20000.0
MONO_MIN = 20.0
MONO_MAX = 500.0
SIDE_HPF_MAX = 1000.0
FLOOR_DB = -60.0
LONG_PRESS_SEC = 0.45
DOUBLE_TAP_SEC = 0.35

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
    ("M/S",       "#4dabf7"),
    ("COMB",      "#63e6be"),
    ("HAAS",      "#ffd43b"),
    ("DIMENSION", "#ffa94d"),
    ("VORTEX",    "#da77f2"),
)

SHAPES = (
    ("PAN",      "#4dabf7"),
    ("BREATHE",  "#63e6be"),
    ("ORBIT",    "#ffd43b"),
    ("FIGURE-8", "#ffa94d"),
    ("DRIFT",    "#b197fc"),
)

AUDITIONS = (
    ("STEREO", "#63e6be"),
    ("MONO",   "#ffd43b"),
    ("MID",    "#4dabf7"),
    ("SIDE",   "#ff8787"),
)

PANEL_BG   = "#16191e"
BUTTON_BG  = "#1f242c"
LINE_DIM   = "#2f3640"
GRID_COL   = "#252b34"
ZERO_COL   = "#3a4352"
LABEL_DIM  = "#8b949e"
LABEL_HI   = "#e6edf3"
LEFT_COL   = "#4dabf7"
RIGHT_COL  = "#63e6be"
MID_COL    = "#74c0fc"
SIDE_COL   = "#ffa94d"
SAFE_COL   = "#63e6be"
WARN_COL   = "#ffd43b"
HOT_RED    = "#ff6b6b"
FX_COL     = "#da77f2"
BAND_FILL  = "#1e3448"
MONO_FILL  = "#2d243b"
AIR_FILL   = "#1f3a36"

BYP_DIM    = "#49505a"
BYP_MID    = "#68707c"
BYP_HI     = "#9aa2ae"
BYP_FILL   = "#252930"


def _clamp(v, lo, hi):
    return lo if v < lo else (hi if v > hi else v)


def _fmt_freq(f):
    if f >= 1000.0:
        return f"{f / 1000.0:.1f}k"
    return f"{f:.0f}Hz"


def nearest_rate_division(hz, bpm):
    bpm = _clamp(bpm, 20.0, 400.0)
    q_sec = 60.0 / bpm
    log_t = math.log(_clamp(hz, 0.05, 12.0))
    best_lbl, best_hz, best_err = "1/2", hz, 1e9
    for lbl, mult in RATE_DIVISIONS:
        cand = 1.0 / (q_sec * mult)
        if cand < 0.04 or cand > 14.0:
            continue
        err = abs(math.log(cand) - log_t)
        if err < best_err:
            best_err = err
            best_lbl = lbl
            best_hz = cand
    return best_lbl, best_hz


class zynthian_widget_eka_space(zynthian_widget_base.zynthian_widget_base):
    def __init__(self, parent):
        super().__init__(parent)
        self.next_tick = 0.0
        self.trail = []  # list of (pan_pct, width_pct, depth_pct)
        self.hold_in = FLOOR_DB
        self.hold_out = FLOOR_DB
        self.hold_mid = FLOOR_DB
        self.hold_side = FLOOR_DB

        self.stage_box = (0.0, 0.0, 1.0, 1.0)
        self.focus_box = (0.0, 0.0, 1.0, 1.0)
        self.xy_box = (0.0, 0.0, 1.0, 1.0)
        self.fx_sliders = {}
        self.fx_view = False
        self.fx_snap = False
        self.handle_pos = {}
        self.hit_rects = {}
        self._last_ctrl_titles = {}
        self._last_stage_tap = 0.0
        self.drag_mode = None  # ("stage", "pos"|"width") | ("focus", "mono"|"hpf"|"air") | ("xy",) | ("fx", sym) | ("safe", t0)

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

    def _sync_encoder_labels(self, st, bpm):
        if not self.processor:
            return
        if st["sync"]:
            lbl, _ = nearest_rate_division(st["mod_rate"], bpm)
            want = {"mod_rate": f"Mod Rate {lbl}"}
        else:
            want = {"mod_rate": "Mod Rate"}
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
        try:
            rows = self.zyngui_control.list_data
        except Exception:
            return
        for i, row in enumerate(rows):
            if len(row) >= 4 and row[3] is self.processor:
                if row[0] == group_sym or (group_name and (row[0] == group_name or row[2] == group_name)):
                    try:
                        self.zyngui_control.select(i)
                    except Exception:
                        pass
                    return

    def _read_state(self):
        return {
            "pan":        self._get("pan", 0.0),
            "width":      self._get("width", 100.0),
            "depth":      self._get("depth", 0.0),
            "tilt":       self._get("tilt", 0.0),
            "mode":       int(_clamp(round(self._get("mode", 1.0)), 0, 4)),
            "widen":      self._get("widen", 30.0),
            "spread_ms":  self._get("spread_ms", 14.0),
            "room_pct":   self._get("room_pct", 15.0),
            "mod_shape":  int(_clamp(round(self._get("mod_shape", 0.0)), 0, 4)),
            "mod_rate":   self._get("mod_rate", 0.5),
            "mod_depth":  self._get("mod_depth", 0.0),
            "sync":       self._get("sync", 1.0) >= 0.5,
            "mono_low":   self._get("mono_low", 120.0),
            "side_hpf":   self._get("side_hpf", 90.0),
            "side_air":   self._get("side_air", 1.5),
            "env_width":  self._get("env_width", 0.0),
            "pan_fx":     self._get("pan_fx", 0.0),
            "width_fx":   self._get("width_fx", 0.0),
            "spin_fx":    self._get("spin_fx", 0.0),
            "fx_zero":    self._get("fx_zero", 0.0) >= 0.5,
            "audition":   int(_clamp(round(self._get("audition", 0.0)), 0, 3)),
            "safe_phase": self._get("safe_phase", 0.0) >= 0.5,
            "mix":        self._get("mix", 100.0),
            "output_db":  self._get("output_db", 0.0),
            "bypass":     self._get("bypass", 0.0) >= 0.5,
        }

    def _freq_to_x(self, f, x0, x1):
        t = math.log(_clamp(f, F_MIN, F_MAX) / F_MIN) / math.log(F_MAX / F_MIN)
        return x0 + t * (x1 - x0)

    def _x_to_freq(self, x, x0, x1):
        t = _clamp((x - x0) / max(1.0, x1 - x0), 0.0, 1.0)
        return F_MIN * ((F_MAX / F_MIN) ** t)

    def _stage_to_xy(self, pan_pct, depth_pct, gx0, gy0, gx1, gy1):
        pad_x = 18.0
        pad_top = 26.0
        pad_bot = 28.0
        sx0 = gx0 + pad_x
        sx1 = gx1 - pad_x
        sy0 = gy0 + pad_top
        sy1 = gy1 - pad_bot
        px = (sx0 + sx1) * 0.5 + _clamp(pan_pct / 100.0, -1.0, 1.0) * ((sx1 - sx0) * 0.5)
        py = sy1 - _clamp(depth_pct / 100.0, 0.0, 1.0) * (sy1 - sy0)
        return px, py

    def _xy_to_stage(self, x, y, gx0, gy0, gx1, gy1):
        pad_x = 18.0
        pad_top = 26.0
        pad_bot = 28.0
        sx0 = gx0 + pad_x
        sx1 = gx1 - pad_x
        sy0 = gy0 + pad_top
        sy1 = gy1 - pad_bot
        pan_pct = ((x - (sx0 + sx1) * 0.5) / max(1.0, (sx1 - sx0) * 0.5)) * 100.0
        depth_pct = ((sy1 - y) / max(1.0, sy1 - sy0)) * 100.0
        return _clamp(pan_pct, -100.0, 100.0), _clamp(depth_pct, 0.0, 100.0)

    def _pull_telemetry(self, st):
        in_db   = _clamp(float(self.monitors.get("in_peak", FLOOR_DB)), FLOOR_DB, 12.0)
        out_db  = _clamp(float(self.monitors.get("out_peak", FLOOR_DB)), FLOOR_DB, 12.0)
        mid_db  = _clamp(float(self.monitors.get("mid_db", FLOOR_DB)), FLOOR_DB, 12.0)
        side_db = _clamp(float(self.monitors.get("side_db", FLOOR_DB)), FLOOR_DB, 12.0)
        corr    = _clamp(float(self.monitors.get("corr", 1.0)), -1.0, 1.0)
        bal     = _clamp(float(self.monitors.get("balance", 0.0)), -1.0, 1.0)
        eff_pan = _clamp(float(self.monitors.get("eff_pan", st["pan"] + st["pan_fx"])), -100.0, 100.0)
        eff_wid = _clamp(float(self.monitors.get("eff_width", st["width"] + st["width_fx"] * 1.5)), 0.0, 300.0)
        eff_dep = _clamp(float(self.monitors.get("eff_depth", st["depth"] + st["spin_fx"] * 0.35)), 0.0, 100.0)
        bpm     = _clamp(float(self.monitors.get("host_bpm", 120.0)), 20.0, 400.0)

        self.hold_in   = in_db   if in_db   >= self.hold_in   else max(in_db,   self.hold_in   - 1.2)
        self.hold_out  = out_db  if out_db  >= self.hold_out  else max(out_db,  self.hold_out  - 1.2)
        self.hold_mid  = mid_db  if mid_db  >= self.hold_mid  else max(mid_db,  self.hold_mid  - 1.2)
        self.hold_side = side_db if side_db >= self.hold_side else max(side_db, self.hold_side - 1.2)

        self.trail.append((eff_pan, eff_wid, eff_dep))
        if len(self.trail) > TRAIL_STEPS:
            self.trail.pop(0)
        return in_db, out_db, mid_db, side_db, corr, bal, eff_pan, eff_wid, eff_dep, bpm

    def refresh_gui(self):
        cv = self.widget_canvas
        w = max(240, int(self.width or cv.winfo_width() or 640))
        h = max(180, int(self.height or cv.winfo_height() or 380))
        st = self._read_state()
        if st["fx_zero"]:
            self._set("pan_fx", 0.0)
            self._set("width_fx", 0.0)
            self._set("spin_fx", 0.0)
            self._set("fx_zero", 0)
            st["pan_fx"] = 0.0
            st["width_fx"] = 0.0
            st["spin_fx"] = 0.0
            st["fx_zero"] = False
        byp = st["bypass"]
        in_db, out_db, mid_db, side_db, corr, bal, eff_pan, eff_wid, eff_dep, bpm = self._pull_telemetry(st)

        self._sync_encoder_labels(st, bpm)

        cv.delete("all")
        self.hit_rects.clear()
        self.handle_pos.clear()
        self.fx_sliders.clear()

        pad = 6
        btn_h = max(36, min(44, int(h * 0.12)))
        focus_h = max(28, min(36, int(h * 0.095)))
        meter_w = 72

        gx0 = pad
        gy0 = pad
        gx1 = w - pad - meter_w - 6
        gy1 = h - pad - btn_h - focus_h - 12
        fy1 = gy1 + 5 + focus_h
        self.stage_box = (gx0, gy0, gx1, gy1)

        mode_name, mode_col = MODES[st["mode"]]
        shape_name, shape_col = SHAPES[st["mod_shape"]]
        aud_name, aud_col = AUDITIONS[st["audition"]]

        if self.fx_view:
            self._draw_fx_screen(cv, st, byp, bpm, eff_pan, eff_wid, eff_dep, corr, gx0, gy0, gx1, fy1, mode_name, mode_col)
            self._draw_meters(cv, byp, in_db, out_db, mid_db, side_db, gx1, gy0, fy1, w, pad)
            by0 = fy1 + 6
            by1 = h - pad
            fx_nonzero = (abs(st["pan_fx"]) >= 0.75 or abs(st["width_fx"]) >= 0.75 or st["spin_fx"] >= 0.75)
            buttons = (
                ("fx",    "STAGE",                           BYP_MID if byp else LEFT_COL,  False),
                ("mode",  mode_name,                         BYP_MID if byp else mode_col,  False),
                ("snap",  "SNAP 0" if self.fx_snap else "HOLD",
                          BYP_MID if byp else RIGHT_COL,     self.fx_snap and not byp),
                ("zero",  "ZERO 0",                          BYP_MID if byp else WARN_COL,  fx_nonzero and not byp),
                ("aud",   aud_name,                          BYP_MID if byp else aud_col,   st["audition"] != 0 and not byp),
                ("safe",  "BYPASS" if byp else ("SAFE ON" if st["safe_phase"] else "SAFE"),
                          BYP_HI if byp else SAFE_COL,       byp or st["safe_phase"]),
            )
            self._draw_buttons(cv, buttons, w, pad, by0, by1)
            return

        # 1. Soundstage Radar panel
        cv.create_rectangle(gx0, gy0, gx1, gy1, fill=PANEL_BG, outline=LINE_DIM, width=1)
        cx = (gx0 + gx1) * 0.5
        listener_y = gy1 - 24
        max_rx = max(20.0, (gx1 - gx0) * 0.45)
        max_ry = max(20.0, (listener_y - (gy0 + 24)))

        # Semicircular depth arcs (25%, 50%, 75%, 100%) & azimuth spokes
        for frac in (0.33, 0.66, 1.0):
            rx = max_rx * frac
            ry = max_ry * frac
            cv.create_arc(cx - rx, listener_y - ry, cx + rx, listener_y + ry,
                          start=0, extent=180, style="arc", outline=GRID_COL, width=1)
        cv.create_line(cx, gy0 + 20, cx, listener_y + 4, fill=ZERO_COL, width=1.3)
        cv.create_line(gx0 + 14, listener_y, gx1 - 14, listener_y, fill=ZERO_COL, width=1.3)
        for ang_deg, lbl in ((-60, "L"), (-30, "30°"), (30, "30°"), (60, "R")):
            rad = math.radians(ang_deg)
            x_end = cx + math.sin(rad) * max_rx
            y_end = listener_y - math.cos(rad) * max_ry
            cv.create_line(cx, listener_y, x_end, y_end, fill=GRID_COL, width=1)
            cv.create_text(x_end, y_end - 2, text=lbl, anchor="s", fill=LABEL_DIM, font=("TkDefaultFont", 7))

        # Listener head marker at bottom center
        cv.create_oval(cx - 5, listener_y - 5, cx + 5, listener_y + 5,
                       fill=BYP_DIM if byp else LABEL_DIM, outline="")

        # Animated M/S stereo cloud aura + motion trail
        eff_x, eff_y = self._stage_to_xy(eff_pan, eff_dep, gx0, gy0, gx1, gy1)
        wing_span = _clamp(eff_wid / 300.0, 0.0, 1.0) * (max_rx * 0.78)
        cloud_ry = max(10.0, 14.0 + _clamp(st["room_pct"] / 100.0, 0.0, 1.0) * 26.0)
        if wing_span > 3.0:
            cv.create_oval(eff_x - wing_span, eff_y - cloud_ry, eff_x + wing_span, eff_y + cloud_ry,
                           fill=BYP_FILL if byp else "#192d3e",
                           outline=BYP_MID if byp else mode_col, width=1.2)

        # Motion orbit trail
        if len(self.trail) >= 2 and (st["mod_depth"] > 0.5 or st["spin_fx"] > 0.5):
            pts = []
            for tp, _tw, td in self.trail:
                tx, ty = self._stage_to_xy(tp, td, gx0, gy0, gx1, gy1)
                pts.extend((tx, ty))
            if len(pts) >= 4:
                cv.create_line(*pts, fill=BYP_DIM if byp else shape_col, width=1.6)

        # Base Stage Anchor + Left/Right Width Wing Handles
        bx, by = self._stage_to_xy(st["pan"], st["depth"], gx0, gy0, gx1, gy1)
        base_span = _clamp(st["width"] / 300.0, 0.0, 1.0) * (max_rx * 0.78)
        wl_x = _clamp(bx - base_span, gx0 + 12, gx1 - 12)
        wr_x = _clamp(bx + base_span, gx0 + 12, gx1 - 12)

        p_col = BYP_MID if byp else mode_col
        w_col = BYP_MID if byp else RIGHT_COL
        cv.create_line(wl_x, by, wr_x, by, fill=w_col, width=2.2)

        # Live modulated position satellite dot if modulated away from base
        if abs(eff_x - bx) > 2.5 or abs(eff_y - by) > 2.5:
            cv.create_line(bx, by, eff_x, eff_y, fill=BYP_DIM if byp else shape_col, width=1.4)
            cv.create_oval(eff_x - 5, eff_y - 5, eff_x + 5, eff_y + 5,
                           fill=BYP_MID if byp else shape_col, outline="")

        # Width wing handles (L & R)
        for key, wx, tag in (("wing_l", wl_x, "W"), ("wing_r", wr_x, "W")):
            cv.create_oval(wx - 8, by - 8, wx + 8, by + 8,
                           fill=BUTTON_BG, outline=w_col, width=1.8)
            cv.create_text(wx, by, text=tag, fill=w_col, font=("TkDefaultFont", 7, "bold"))
            self.handle_pos[key] = (wx, by)

        # Center Stage node (Pan & Depth)
        cv.create_oval(bx - 12, by - 12, bx + 12, by + 12,
                       fill=p_col, outline=BYP_HI if byp else LABEL_HI, width=2.0)
        cv.create_text(bx, by, text="S", fill="#101318", font=("TkDefaultFont", 9, "bold"))
        self.handle_pos["pos"] = (bx, by)

        # Embedded Phase Correlation bar (-1 .. 0 .. +1) at bottom of Stage panel
        cb_x0 = gx0 + 56
        cb_x1 = gx1 - 68
        cb_y0 = gy1 - 13
        cb_y1 = gy1 - 5
        cb_mid = (cb_x0 + cb_x1) * 0.5
        cv.create_rectangle(cb_x0, cb_y0, cb_x1, cb_y1, fill=BUTTON_BG, outline=LINE_DIM, width=1)
        cv.create_line(cb_mid, cb_y0, cb_mid, cb_y1, fill=ZERO_COL, width=1.4)
        cx_corr = cb_mid + corr * ((cb_x1 - cb_x0) * 0.5)
        c_col = BYP_MID if byp else (HOT_RED if corr < -0.15 else (WARN_COL if corr < 0.15 else SAFE_COL))
        if cx_corr >= cb_mid:
            cv.create_rectangle(cb_mid, cb_y0 + 1, cx_corr, cb_y1 - 1, fill=c_col, outline="")
        else:
            cv.create_rectangle(cx_corr, cb_y0 + 1, cb_mid, cb_y1 - 1, fill=c_col, outline="")
        cv.create_text(cb_x0 - 4, (cb_y0 + cb_y1) * 0.5, text="CORR -1", anchor="e",
                       fill=LABEL_DIM, font=("TkDefaultFont", 7, "bold"))
        cv.create_text(cb_x1 + 4, (cb_y0 + cb_y1) * 0.5, text=f"+1 ({corr:+.2f})", anchor="w",
                       fill=c_col, font=("TkDefaultFont", 7, "bold"))

        # Stage header info
        pan_txt = "C" if abs(eff_pan) < 1.0 else (f"L{-eff_pan:.0f}" if eff_pan < 0 else f"R{eff_pan:.0f}")
        fx_tag = " [FX]" if (abs(st["pan_fx"]) >= 0.75 or abs(st["width_fx"]) >= 0.75 or st["spin_fx"] >= 0.75) else ""
        hdr_l = (f"PAN {pan_txt} · WID {eff_wid:.0f}% · DEP {eff_dep:.0f}%{fx_tag} · "
                 f"SPR {st['spread_ms']:.1f}ms · RM {st['room_pct']:.0f}%")
        cv.create_text(gx0 + 6, gy0 + 4, text=hdr_l, anchor="nw",
                       fill=BYP_HI if byp else LABEL_HI, font=("TkDefaultFont", 8, "bold"))

        if byp:
            badge_txt, badge_col = "BYPASS", BYP_HI
        elif st["audition"] != 0:
            badge_txt, badge_col = f"AUD: {aud_name}", aud_col
        elif st["mod_depth"] > 0.5:
            if st["sync"]:
                rlbl, _ = nearest_rate_division(st["mod_rate"], bpm)
                badge_txt = f"{mode_name} · {shape_name} {rlbl}"
            else:
                badge_txt = f"{mode_name} · {shape_name} {st['mod_rate']:.1f}Hz"
            badge_col = mode_col
        else:
            badge_txt, badge_col = f"{mode_name} · WIDEN {st['widen']:.0f}%", mode_col
        cv.create_text(gx1 - 6, gy0 + 4, text=badge_txt, anchor="ne",
                       fill=badge_col, font=("TkDefaultFont", 8, "bold"))

        # 2. Interactive Bass Mono & Side Air bar
        fy0 = gy1 + 5
        fx0 = gx0
        fx1 = gx1
        self.focus_box = (fx0, fy0, fx1, fy1)
        cv.create_rectangle(fx0, fy0, fx1, fy1, fill=PANEL_BG, outline=LINE_DIM, width=1)

        mx = self._freq_to_x(_clamp(st["mono_low"], MONO_MIN, MONO_MAX), fx0 + 8, fx1 - 8)
        hx = self._freq_to_x(_clamp(st["side_hpf"], F_MIN, SIDE_HPF_MAX), fx0 + 8, fx1 - 8)
        ax = self._freq_to_x(6500.0, fx0 + 8, fx1 - 8)

        # Bass Mono zone shaded on left, active stereo side band in middle, Side Air shelf on right
        cv.create_rectangle(fx0 + 2, fy0 + 2, mx, fy1 - 2,
                            fill=BYP_FILL if byp else MONO_FILL, outline="")
        cv.create_rectangle(max(mx, hx), fy0 + 2, fx1 - 2, fy1 - 2,
                            fill=BYP_FILL if byp else BAND_FILL, outline="")
        if abs(st["side_air"]) > 0.2:
            cv.create_rectangle(ax, fy0 + 2, fx1 - 2, fy1 - 2,
                                fill=BYP_FILL if byp else AIR_FILL, outline="")

        cv.create_line(mx, fy0 + 2, mx, fy1 - 2, fill=BYP_MID if byp else "#b197fc", width=2.2)
        cv.create_line(hx, fy0 + 2, hx, fy1 - 2, fill=BYP_MID if byp else LEFT_COL, width=2.2)
        air_norm = _clamp((st["side_air"] + 12.0) / 24.0, 0.0, 1.0)
        ay = (fy1 - 4) - air_norm * (fy1 - fy0 - 8)
        cv.create_oval(ax - 5, ay - 5, ax + 5, ay + 5,
                       fill=BYP_MID if byp else RIGHT_COL, outline="")

        self.handle_pos["mono"] = (mx, (fy0 + fy1) * 0.5)
        self.handle_pos["hpf"]  = (hx, (fy0 + fy1) * 0.5)
        self.handle_pos["air"]  = (ax, ay)

        focus_lbl = (f"MONO {_fmt_freq(st['mono_low'])} · "
                     f"SIDE HPF {_fmt_freq(st['side_hpf'])} · "
                     f"AIR {st['side_air']:+.1f}dB · "
                     f"TILT {st['tilt']:+.0f}%")
        cv.create_text((fx0 + fx1) * 0.5, (fy0 + fy1) * 0.5, text=focus_lbl,
                       fill=BYP_HI if byp else LABEL_HI, font=("TkDefaultFont", 8, "bold"))

        # 3. Right Vertical Meters: IN | M | S | OUT
        self._draw_meters(cv, byp, in_db, out_db, mid_db, side_db, gx1, gy0, fy1, w, pad)

        # 4. Bottom 6 touch buttons: MODE | SHAPE | SYNC | FX | AUD | SAFE (long press = BYPASS)
        by0 = fy1 + 6
        by1 = h - pad
        fx_nonzero = (abs(st["pan_fx"]) >= 0.75 or abs(st["width_fx"]) >= 0.75 or st["spin_fx"] >= 0.75)
        buttons = (
            ("mode",  mode_name,                          BYP_MID if byp else mode_col,  False),
            ("shape", shape_name,                         BYP_MID if byp else shape_col, st["mod_depth"] > 0.5 and not byp),
            ("sync",  "SYNC" if st["sync"] else "FREE",   BYP_MID if byp else RIGHT_COL, st["sync"] and not byp),
            ("fx",    "FX *" if fx_nonzero else "FX",     BYP_MID if byp else FX_COL,    fx_nonzero and not byp),
            ("aud",   aud_name,                           BYP_MID if byp else aud_col,   st["audition"] != 0 and not byp),
            ("safe",  "BYPASS" if byp else ("SAFE ON" if st["safe_phase"] else "SAFE"),
                      BYP_HI if byp else SAFE_COL,        byp or st["safe_phase"]),
        )
        self._draw_buttons(cv, buttons, w, pad, by0, by1)

    def _draw_meters(self, cv, byp, in_db, out_db, mid_db, side_db, gx1, gy0, fy1, w, pad):
        mx0 = gx1 + 6
        my1 = fy1
        mw = (w - pad - mx0 - 9) / 4.0
        for mi, (lbl, val, hold, col) in enumerate((
            ("IN",  in_db,   self.hold_in,   BYP_MID if byp else LEFT_COL),
            ("M",   mid_db,  self.hold_mid,  BYP_MID if byp else MID_COL),
            ("S",   side_db, self.hold_side, BYP_MID if byp else SIDE_COL),
            ("OUT", out_db,  self.hold_out,  BYP_MID if byp else RIGHT_COL),
        )):
            bx0 = mx0 + mi * (mw + 3)
            bx1 = bx0 + mw
            bar_bot = my1 - 12
            cv.create_rectangle(bx0, gy0, bx1, bar_bot, fill=PANEL_BG, outline=LINE_DIM, width=1)
            norm = _clamp((val - FLOOR_DB) / (6.0 - FLOOR_DB), 0.0, 1.0)
            my = bar_bot - norm * (bar_bot - gy0)
            bcol = BYP_MID if byp else (HOT_RED if val > -0.5 else col)
            cv.create_rectangle(bx0 + 1, my, bx1 - 1, bar_bot - 1, fill=bcol, outline="")
            hnorm = _clamp((hold - FLOOR_DB) / (6.0 - FLOOR_DB), 0.0, 1.0)
            hy = bar_bot - hnorm * (bar_bot - gy0)
            cv.create_line(bx0 + 1, hy, bx1 - 1, hy, fill=BYP_HI if byp else LABEL_HI, width=2)
            cv.create_text((bx0 + bx1) * 0.5, my1 - 5, text=lbl, fill=LABEL_DIM, font=("TkDefaultFont", 7, "bold"))

    def _draw_buttons(self, cv, buttons, w, pad, by0, by1):
        gap = 5
        bw = (w - 2 * pad - gap * (len(buttons) - 1)) / float(len(buttons))
        for i, (key, lbl, col, active) in enumerate(buttons):
            bx0 = pad + i * (bw + gap)
            bx1 = bx0 + bw
            self.hit_rects[key] = (bx0, by0, bx1, by1)
            fill = col if active else BUTTON_BG
            txt_col = "#101318" if active else col
            cv.create_rectangle(bx0, by0, bx1, by1, fill=fill, outline=col, width=1.6)
            cv.create_text((bx0 + bx1) * 0.5, (by0 + by1) * 0.5, text=lbl,
                           fill=txt_col, font=("TkDefaultFont", 9, "bold"))

    def _draw_fx_screen(self, cv, st, byp, bpm, eff_pan, eff_wid, _eff_dep, corr, x0, y0, x1, y1, mode_name, mode_col):
        cv.create_rectangle(x0, y0, x1, y1, fill=PANEL_BG, outline=LINE_DIM, width=1)
        hdr = f"LIVE FX · PAN {eff_pan:+.0f}% · WIDTH {eff_wid:.0f}% · CORR {corr:+.2f}"
        cv.create_text(x0 + 8, y0 + 5, text=hdr, anchor="nw",
                       fill=BYP_HI if byp else LABEL_HI, font=("TkDefaultFont", 8, "bold"))
        badge = "BYPASS" if byp else f"{mode_name} · {bpm:.0f} BPM"
        cv.create_text(x1 - 8, y0 + 5, text=badge, anchor="ne",
                       fill=BYP_HI if byp else mode_col, font=("TkDefaultFont", 8, "bold"))

        inner_x0 = x0 + 8
        inner_x1 = x1 - 8
        spin_h = max(30.0, min(38.0, (y1 - y0) * 0.17))
        pad_y0 = y0 + 22
        pad_y1 = y1 - spin_h - 14
        self.xy_box = (inner_x0, pad_y0, inner_x1, pad_y1)

        # 1. X-Y Pad: X = PAN FX (-100..+100%), Y = WIDTH FX (-100..+100%)
        cv.create_rectangle(inner_x0, pad_y0, inner_x1, pad_y1, fill=BUTTON_BG, outline=LINE_DIM, width=1.2)
        cx = (inner_x0 + inner_x1) * 0.5
        cy = (pad_y0 + pad_y1) * 0.5
        pw = max(1.0, inner_x1 - inner_x0)
        ph = max(1.0, pad_y1 - pad_y0)

        for frac, lbl in ((-0.5, "L50"), (0.0, "CENTER"), (0.5, "R50")):
            gx = cx + frac * pw * 0.5
            cv.create_line(gx, pad_y0 + 1, gx, pad_y1 - 1,
                           fill=ZERO_COL if frac == 0.0 else GRID_COL,
                           width=1.4 if frac == 0.0 else 1.0)
            cv.create_text(gx, pad_y1 - 3, text=lbl, anchor="s",
                           fill=LABEL_DIM, font=("TkDefaultFont", 7, "bold"))
        cv.create_line(inner_x0 + 1, cy, inner_x1 - 1, cy, fill=ZERO_COL, width=1.4)

        cv.create_text(cx, pad_y0 + 3, text="▲ HYPER WIDE (+150%)", anchor="n",
                       fill=BYP_DIM if byp else RIGHT_COL, font=("TkDefaultFont", 7, "bold"))
        cv.create_text(cx, pad_y1 - 14, text="▼ COLLAPSE MONO (-150%)", anchor="s",
                       fill=BYP_DIM if byp else LEFT_COL, font=("TkDefaultFont", 7, "bold"))

        pfx = st["pan_fx"]
        wfx = st["width_fx"]
        px = cx + _clamp(pfx / 100.0, -1.0, 1.0) * (pw * 0.5)
        py = cy - _clamp(wfx / 100.0, -1.0, 1.0) * (ph * 0.5)

        xy_active = (abs(pfx) >= 0.75 or abs(wfx) >= 0.75)
        p_col = BYP_MID if byp else (FX_COL if xy_active else LABEL_HI)
        if xy_active:
            cv.create_line(px, pad_y0 + 2, px, pad_y1 - 2, fill=GRID_COL, width=1.2)
            cv.create_line(inner_x0 + 2, py, inner_x1 - 2, py, fill=GRID_COL, width=1.2)
            cv.create_line(cx, cy, px, py, fill=p_col, width=2.2)

        r = 12.0
        cv.create_oval(px - r, py - r, px + r, py + r,
                       fill=p_col, outline=BYP_HI if byp else LABEL_HI, width=2.0)
        cv.create_oval(px - 3, py - 3, px + 3, py + 3, fill="#101318", outline="")

        xy_readout = f"PAN FX {pfx:+.0f}% · WIDTH FX {wfx:+.0f}%"
        cv.create_text(inner_x0 + 6, pad_y0 + 4, text=xy_readout, anchor="nw",
                       fill=BYP_HI if byp else LABEL_HI, font=("TkDefaultFont", 8, "bold"))
        if self.fx_snap:
            cv.create_text(inner_x1 - 6, pad_y0 + 4, text="SPRING", anchor="ne",
                           fill=BYP_MID if byp else RIGHT_COL, font=("TkDefaultFont", 7, "bold"))

        # 2. Bottom slider: SPIN FX (0..100%) + individual "0" button
        sy0 = pad_y1 + 6
        sy1 = y1 - 8
        z_btn_w = 42
        bar_x1 = inner_x1 - z_btn_w - 6
        sfx = st["spin_fx"]
        self.fx_sliders["spin_fx"] = (inner_x0, sy0, bar_x1, sy1, False)
        c_act = BYP_MID if byp else FX_COL

        cv.create_rectangle(inner_x0, sy0, bar_x1, sy1, fill=BUTTON_BG, outline=LINE_DIM, width=1.2)
        norm = _clamp(sfx / 100.0, 0.0, 1.0)
        vx = inner_x0 + norm * (bar_x1 - inner_x0)
        if norm > 0.005:
            cv.create_rectangle(inner_x0 + 1, sy0 + 2, vx, sy1 - 2,
                                fill=BYP_FILL if byp else c_act, outline="")
        cv.create_line(vx, sy0 + 2, vx, sy1 - 2, fill=LABEL_HI, width=2.4)
        cv.create_text((inner_x0 + bar_x1) * 0.5, (sy0 + sy1) * 0.5,
                       text=f"SPIN FX (3D ORBIT & ROOM)  {sfx:.0f}%",
                       fill=LABEL_HI, font=("TkDefaultFont", 9, "bold"))

        zx0 = bar_x1 + 6
        zx1 = inner_x1
        self.hit_rects["z_spin_fx"] = (zx0, sy0, zx1, sy1)
        active_z = (sfx >= 0.75) and not byp
        cv.create_rectangle(zx0, sy0, zx1, sy1,
                            fill=c_act if active_z else BUTTON_BG,
                            outline=c_act, width=1.4)
        cv.create_text((zx0 + zx1) * 0.5, (sy0 + sy1) * 0.5, text="0",
                       fill="#101318" if active_z else LABEL_HI, font=("TkDefaultFont", 10, "bold"))

    def _apply_stage_drag(self, which, x, y):
        gx0, gy0, gx1, gy1 = self.stage_box
        if which == "pos":
            pan_pct, depth_pct = self._xy_to_stage(x, y, gx0, gy0, gx1, gy1)
            if abs(pan_pct) < 3.0:
                pan_pct = 0.0
            self._set("pan", round(pan_pct, 1))
            self._set("depth", round(depth_pct, 1))
        else:
            bx, _by = self._stage_to_xy(self._get("pan", 0.0), self._get("depth", 0.0), gx0, gy0, gx1, gy1)
            max_rx = max(20.0, (gx1 - gx0) * 0.45)
            span = abs(x - bx)
            wid_pct = _clamp((span / (max_rx * 0.78)) * 300.0, 0.0, 300.0)
            self._set("width", round(wid_pct, 1))

    def _apply_focus_drag(self, which, x, y):
        fx0, fy0, fx1, fy1 = self.focus_box
        if which == "mono":
            f = self._x_to_freq(x, fx0 + 8, fx1 - 8)
            self._set("mono_low", round(_clamp(f, MONO_MIN, MONO_MAX), 1))
        elif which == "hpf":
            f = self._x_to_freq(x, fx0 + 8, fx1 - 8)
            self._set("side_hpf", round(_clamp(f, F_MIN, SIDE_HPF_MAX), 1))
        else:
            norm = _clamp(((fy1 - 4) - y) / max(1.0, fy1 - fy0 - 8), 0.0, 1.0)
            db = norm * 24.0 - 12.0
            if abs(db) < 0.5:
                db = 0.0
            self._set("side_air", round(_clamp(db, -12.0, 12.0), 1))

    def _apply_xy_drag(self, x, y):
        px0, py0, px1, py1 = self.xy_box
        tx = _clamp((x - px0) / max(1.0, px1 - px0), 0.0, 1.0)
        ty = _clamp((y - py0) / max(1.0, py1 - py0), 0.0, 1.0)
        pfx = (tx * 2.0 - 1.0) * 100.0
        wfx = (1.0 - ty * 2.0) * 100.0
        if abs(pfx) < 4.0:
            pfx = 0.0
        if abs(wfx) < 4.0:
            wfx = 0.0
        self._set("pan_fx", round(_clamp(pfx, -100.0, 100.0), 1))
        self._set("width_fx", round(_clamp(wfx, -100.0, 100.0), 1))

    def _apply_fx_drag(self, sym, x):
        info = self.fx_sliders.get(sym)
        if not info:
            return
        sx0, _sy0, sx1, _sy1, bipolar = info
        t = _clamp((x - sx0) / max(1.0, sx1 - sx0), 0.0, 1.0)
        if bipolar:
            v = (t * 2.0 - 1.0) * 100.0
            if abs(v) < 3.0:
                v = 0.0
            self._set(sym, round(_clamp(v, -100.0, 100.0), 1))
        else:
            v = t * 100.0
            self._set(sym, round(_clamp(v, 0.0, 100.0), 1))

    def _on_press(self, event):
        x, y = event.x, event.y
        now = time.monotonic()
        self.drag_mode = None

        # 1. Bottom buttons + FX reset buttons
        for key, (x0, y0, x1, y1) in self.hit_rects.items():
            if x0 <= x <= x1 and y0 <= y <= y1:
                if key == "fx":
                    self.fx_view = not self.fx_view
                    if self.fx_view:
                        self._select_group("g_fx", "Live FX")
                    else:
                        self._select_group("g_stage", "Stage")
                    self.refresh_gui()
                    return
                if key == "zero":
                    self._set("pan_fx", 0.0)
                    self._set("width_fx", 0.0)
                    self._set("spin_fx", 0.0)
                    self._select_group("g_fx", "Live FX")
                    self.refresh_gui()
                    return
                if key.startswith("z_"):
                    self._set(key[2:], 0.0)
                    self._select_group("g_fx", "Live FX")
                    self.refresh_gui()
                    return
                if key == "mode":
                    cur = int(round(self._get("mode", 1.0)))
                    self._set("mode", (cur + 1) % len(MODES))
                    self._select_group("g_widen", "Widener")
                    self.refresh_gui()
                    return
                if key == "shape":
                    cur = int(round(self._get("mod_shape", 0.0)))
                    self._set("mod_shape", (cur + 1) % len(SHAPES))
                    self._select_group("g_motion", "Motion")
                    self.refresh_gui()
                    return
                if key == "sync":
                    cur = self._get("sync", 1.0)
                    self._set("sync", 0 if cur >= 0.5 else 1)
                    self._select_group("g_motion", "Motion")
                    self.refresh_gui()
                    return
                if key == "snap":
                    self.fx_snap = not self.fx_snap
                    self._select_group("g_fx", "Live FX")
                    self.refresh_gui()
                    return
                if key == "aud":
                    cur = int(round(self._get("audition", 0.0)))
                    self._set("audition", (cur + 1) % len(AUDITIONS))
                    self._select_group("g_master", "Master")
                    self.refresh_gui()
                    return
                if key == "safe":
                    self.drag_mode = ("safe", now)
                    self._select_group("g_master", "Master")
                    return

        # 2. Dedicated Live FX screen
        if self.fx_view:
            px0, py0, px1, py1 = self.xy_box
            if px0 <= x <= px1 and py0 <= y <= py1:
                self.drag_mode = ("xy",)
                self._select_group("g_fx", "Live FX")
                self._apply_xy_drag(x, y)
                self.refresh_gui()
                return
            for sym, (sx0, sy0, sx1, sy1, _bip) in self.fx_sliders.items():
                if sx0 <= x <= sx1 and sy0 <= y <= sy1:
                    self.drag_mode = ("fx", sym)
                    self._select_group("g_fx", "Live FX")
                    self._apply_fx_drag(sym, x)
                    self.refresh_gui()
                    return
            return

        # 3. Bass Mono & Side Air bar
        fx0, fy0, fx1, fy1 = self.focus_box
        if fx0 <= x <= fx1 and fy0 <= y <= fy1:
            mx = self.handle_pos.get("mono", (fx0, 0))[0]
            hx = self.handle_pos.get("hpf", (fx0, 0))[0]
            ax = self.handle_pos.get("air", (fx1, 0))[0]
            dm = abs(x - mx)
            dh = abs(x - hx)
            da = abs(x - ax)
            if da < dm and da < dh and x > (fx0 + (fx1 - fx0) * 0.55):
                which = "air"
            elif dm <= dh:
                which = "mono"
            else:
                which = "hpf"
            self.drag_mode = ("focus", which)
            self._select_group("g_focus", "Bass & Air")
            self._apply_focus_drag(which, x, y)
            self.refresh_gui()
            return

        # 4. Soundstage Radar
        gx0, gy0, gx1, gy1 = self.stage_box
        if gx0 <= x <= gx1 and gy0 <= y <= gy1:
            if (now - self._last_stage_tap) <= DOUBLE_TAP_SEC:
                self._last_stage_tap = 0.0
                self._set("pan", 0.0)
                self._set("width", 100.0)
                self._set("depth", 0.0)
                self._select_group("g_stage", "Stage")
                self.refresh_gui()
                return
            self._last_stage_tap = now
            px, py = self.handle_pos.get("pos", ((gx0 + gx1) * 0.5, (gy0 + gy1) * 0.5))
            wl_x, wl_y = self.handle_pos.get("wing_l", (px - 30, py))
            wr_x, wr_y = self.handle_pos.get("wing_r", (px + 30, py))
            d_pos = math.hypot(x - px, y - py)
            d_wl  = math.hypot(x - wl_x, y - wl_y)
            d_wr  = math.hypot(x - wr_x, y - wr_y)
            if min(d_wl, d_wr) < d_pos and min(d_wl, d_wr) <= 28.0:
                which = "width"
            else:
                which = "pos"
            self.drag_mode = ("stage", which)
            self._select_group("g_stage", "Stage")
            self._apply_stage_drag(which, x, y)
            self.refresh_gui()

    def _on_motion(self, event):
        if not self.drag_mode:
            return
        kind = self.drag_mode[0]
        if kind == "stage":
            self._apply_stage_drag(self.drag_mode[1], event.x, event.y)
            self.refresh_gui()
        elif kind == "focus":
            self._apply_focus_drag(self.drag_mode[1], event.x, event.y)
            self.refresh_gui()
        elif kind == "xy":
            self._apply_xy_drag(event.x, event.y)
            self.refresh_gui()
        elif kind == "fx":
            self._apply_fx_drag(self.drag_mode[1], event.x)
            self.refresh_gui()

    def _on_release(self, event):
        if self.drag_mode:
            kind = self.drag_mode[0]
            if kind == "safe":
                _, t0 = self.drag_mode
                if (time.monotonic() - t0) >= LONG_PRESS_SEC:
                    cur_byp = self._get("bypass", 0.0)
                    self._set("bypass", 0 if cur_byp >= 0.5 else 1)
                else:
                    cur_sf = self._get("safe_phase", 0.0)
                    self._set("safe_phase", 0 if cur_sf >= 0.5 else 1)
                self.refresh_gui()
            elif kind == "xy" and self.fx_snap:
                self._set("pan_fx", 0.0)
                self._set("width_fx", 0.0)
                self.refresh_gui()
            elif kind == "fx" and self.fx_snap:
                self._set(self.drag_mode[1], 0.0)
                self.refresh_gui()
        self.drag_mode = None
