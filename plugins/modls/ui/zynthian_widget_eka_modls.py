#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# SPDX-License-Identifier: GPL-3.0-or-later
# Zynthian widget for Eka Modls (class and file name: zynthian_widget_eka_modls).
import math
import time
import tkinter

from zyngui import zynthian_gui_config
from zyngui import zynthian_widget_base

MULTI_INSTANCE = True

REFRESH_SEC = 0.08
TRAIL_STEPS = 44
RATE_MIN = 0.02
RATE_MAX = 20.0
F_MIN = 20.0
F_MAX = 20000.0
LOW_CUT_MAX = 600.0
HIGH_CUT_MIN = 1000.0
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


def _clamp(v, lo, hi):
    return lo if v < lo else (hi if v > hi else v)


def _fmt_freq(f):
    if f >= 1000.0:
        return f"{f / 1000.0:.1f}k"
    return f"{f:.0f}Hz"


def _fmt_rate(hz):
    if hz >= 10.0:
        return f"{hz:.1f}Hz"
    if hz >= 1.0:
        return f"{hz:.2f}Hz"
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
        self.next_tick = 0.0
        self.trail = []  # list of (lfo_l, lfo_r, eff_mod)
        self.hold_in = FLOOR_DB
        self.hold_out = FLOOR_DB
        self.hold_mod = 0.0
        self.hold_fb = 0.0

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
        t = math.log(_clamp(hz, RATE_MIN, RATE_MAX) / RATE_MIN) / math.log(RATE_MAX / RATE_MIN)
        return x0 + t * (x1 - x0)

    def _x_to_rate(self, x, x0, x1):
        t = _clamp((x - x0) / max(1.0, x1 - x0), 0.0, 1.0)
        return RATE_MIN * ((RATE_MAX / RATE_MIN) ** t)

    def _freq_to_x(self, f, x0, x1):
        t = math.log(_clamp(f, F_MIN, F_MAX) / F_MIN) / math.log(F_MAX / F_MIN)
        return x0 + t * (x1 - x0)

    def _x_to_freq(self, x, x0, x1):
        t = _clamp((x - x0) / max(1.0, x1 - x0), 0.0, 1.0)
        return F_MIN * ((F_MAX / F_MIN) ** t)

    def _pull_telemetry(self, st):
        in_db   = _clamp(float(self.monitors.get("in_peak", FLOOR_DB)), FLOOR_DB, 12.0)
        out_db  = _clamp(float(self.monitors.get("out_peak", FLOOR_DB)), FLOOR_DB, 12.0)
        lfo_l   = _clamp(float(self.monitors.get("lfo_l", 0.0)), -1.0, 1.0)
        lfo_r   = _clamp(float(self.monitors.get("lfo_r", 0.0)), -1.0, 1.0)
        env_lvl = _clamp(float(self.monitors.get("env_lvl", 0.0)), 0.0, 1.0)
        eff_mod = _clamp(float(self.monitors.get("eff_mod", st["depth"] * 0.005)), 0.0, 1.0)
        fb_en   = _clamp(float(self.monitors.get("fb_energy", 0.0)), 0.0, 1.0)
        bpm     = _clamp(float(self.monitors.get("host_bpm", 120.0)), 20.0, 400.0)

        self.hold_in  = in_db   if in_db   >= self.hold_in  else max(in_db,   self.hold_in  - 1.2)
        self.hold_out = out_db  if out_db  >= self.hold_out else max(out_db,  self.hold_out - 1.2)
        self.hold_mod = eff_mod if eff_mod >= self.hold_mod else max(eff_mod, self.hold_mod - 0.035)
        self.hold_fb  = fb_en   if fb_en   >= self.hold_fb  else max(fb_en,   self.hold_fb  - 0.035)

        self.trail.append((lfo_l, lfo_r, eff_mod))
        if len(self.trail) > TRAIL_STEPS:
            self.trail.pop(0)
        return in_db, out_db, lfo_l, lfo_r, env_lvl, eff_mod, fb_en, bpm

    def refresh_gui(self):
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
        in_db, out_db, lfo_l, lfo_r, env_lvl, eff_mod, fb_en, bpm = self._pull_telemetry(st)

        self._sync_encoder_labels(st, bpm)

        cv.delete("all")
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

        mode_name, mode_col = MODES[st["mode"]]
        shape_name, shape_col = SHAPES[st["shape"]]

        if self.fx_view:
            self._draw_fx_screen(cv, st, byp, bpm, eff_mod, fb_en, gx0, gy0, gx1, fy1, mode_name, mode_col)
            self._draw_meters(cv, byp, in_db, out_db, eff_mod, fb_en, gx1, gy0, fy1, w, pad)
            self._draw_fx_buttons(cv, st, byp, pad, fy1 + 6, w - pad, h - pad, mode_name, mode_col)
        else:
            self._draw_scope(cv, st, byp, bpm, lfo_l, lfo_r, env_lvl, eff_mod, fb_en,
                             gx0, gy0, gx1, gy1, mode_name, mode_col, shape_name, shape_col)
            self._draw_tone_uber_strip(cv, st, byp, gx0, gy1 + 5, gx1, fy1)
            self._draw_meters(cv, byp, in_db, out_db, eff_mod, fb_en, gx1, gy0, fy1, w, pad)
            self._draw_main_buttons(cv, st, byp, pad, fy1 + 6, w - pad, h - pad,
                                    mode_name, mode_col, shape_name, shape_col)

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

    def _draw_scope(self, cv, st, byp, bpm, lfo_l, lfo_r, env_lvl, eff_mod, fb_en,
                    x0, y0, x1, y1, mode_name, mode_col, shape_name, shape_col):
        c_mode = BYP_MID if byp else mode_col
        c_l = BYP_MID if byp else LEFT_COL
        c_r = BYP_DIM if byp else RIGHT_COL
        c_hi = BYP_HI if byp else LABEL_HI

        cv.create_rectangle(x0, y0, x1, y1, fill=PANEL_BG, outline=LINE_DIM, width=1.4)

        inner_x0 = x0 + 12
        inner_x1 = x1 - 12
        inner_y0 = y0 + 26
        inner_y1 = y1 - 24
        iw = max(20.0, inner_x1 - inner_x0)
        ih = max(20.0, inner_y1 - inner_y0)
        cy = (inner_y0 + inner_y1) * 0.5
        cx = (inner_x0 + inner_x1) * 0.5

        # Subtle grid
        for frac in (0.25, 0.5, 0.75):
            gy = inner_y0 + ih * frac
            cv.create_line(inner_x0, gy, inner_x1, gy, fill=ZERO_COL if frac == 0.5 else GRID_COL, width=1.0)
            gx = inner_x0 + iw * frac
            cv.create_line(gx, inner_y0, gx, inner_y1, fill=GRID_COL, width=1.0)

        mode = st["mode"]
        spread = st["spread"] / 100.0
        diffuse = st["diffuse"] / 100.0
        over = _clamp((st["overmod"] + st["warp_fx"] * 0.85) / 100.0, 0.0, 1.5)
        fb = st["feedback"] / 100.0
        comb = st["comb_fx"] / 100.0

        # Mode-specific live DSP topology visualization
        if mode == 0:
            # CHORUS: dual breathing stereo tap ribbons
            for idx, (val, col) in enumerate(((lfo_l, c_l), (lfo_r, c_r))):
                base_x = cx + (-0.16 if idx == 0 else 0.16) * iw
                span_x = (0.08 + 0.18 * (st["depth"] / 100.0)) * iw
                tx = _clamp(base_x + val * span_x, inner_x0 + 10, inner_x1 - 10)
                ty = cy + (-0.18 if idx == 0 else 0.18) * ih + val * 0.12 * ih
                r = 8.0 + 10.0 * spread
                cv.create_oval(tx - r, ty - r, tx + r, ty + r, fill="", outline=col, width=1.6)
                r2 = r * (0.55 + 0.35 * diffuse)
                cv.create_oval(tx - r2, ty - r2, tx + r2, ty + r2, fill=col, outline="")
        elif mode == 1:
            # TRI-MOD: 3-phase 0/120/240 orbiting constellation
            rad_x = iw * (0.14 + 0.18 * spread)
            rad_y = ih * (0.18 + 0.22 * (st["depth"] / 100.0))
            ang0 = math.atan2(lfo_r, lfo_l if abs(lfo_l) > 1e-4 else 0.01)
            pts = []
            for k in range(3):
                a = ang0 + k * (2.0 * math.pi / 3.0)
                px = cx + math.cos(a) * rad_x
                py = cy + math.sin(a) * rad_y
                pts.append((px, py))
            cv.create_polygon(pts[0][0], pts[0][1], pts[1][0], pts[1][1], pts[2][0], pts[2][1],
                              fill="", outline=c_mode, width=1.4)
            for k, (px, py) in enumerate(pts):
                cv.create_oval(px - 6, py - 6, px + 6, py + 6,
                               fill=c_l if k == 0 else (c_r if k == 1 else c_mode), outline="")
        elif mode == 2:
            # FLANGER: Through-Zero reference line + sweeping comb markers
            cv.create_line(cx, inner_y0, cx, inner_y1, fill=BYP_HI if byp else AMBER_COL, width=1.8, dash=(4, 3))
            cv.create_text(cx, inner_y0 + 8, text="0 ms TZF", fill=BYP_HI if byp else AMBER_COL,
                           font=("TkDefaultFont", 7, "bold"))
            swing = iw * 0.38 * (0.15 + 0.85 * st["depth"] / 100.0)
            lx = _clamp(cx + (lfo_l + comb * 0.6) * swing, inner_x0 + 8, inner_x1 - 8)
            rx = _clamp(cx + (lfo_r + comb * 0.6) * swing, inner_x0 + 8, inner_x1 - 8)
            cv.create_line(cx, cy - 16, lx, cy - 16, fill=c_l, width=3.0)
            cv.create_line(cx, cy + 16, rx, cy + 16, fill=c_r, width=3.0)
            cv.create_oval(lx - 6, cy - 22, lx + 6, cy - 10, fill=c_l, outline=c_hi, width=1.2)
            cv.create_oval(rx - 6, cy + 10, rx + 6, cy + 22, fill=c_r, outline=c_hi, width=1.2)
            if abs(fb) > 0.15:
                rr = 14.0 + 26.0 * abs(fb) * (0.5 + 0.5 * fb_en)
                cv.create_oval(cx - rr, cy - rr, cx + rr, cy + rr,
                               fill="", outline=BYP_MID if byp else (HOT_RED if fb < 0 else AMBER_COL), width=1.5)
        elif mode in (3, 4):
            # PHASER / BARBER: sweeping spectral notch curves
            n_notches = 4 if mode == 4 else int(round(2 + spread * 4))
            pts_l = []
            pts_r = []
            steps = 48
            for i in range(steps + 1):
                u = i / float(steps)
                px = inner_x0 + u * iw
                val_l = 0.0
                val_r = 0.0
                for k in range(n_notches):
                    if mode == 4:
                        # Barberpole scrolling peaks
                        base = ((k / float(n_notches)) + (lfo_l * 0.25) + comb * 0.4) % 1.0
                        base_r = ((k / float(n_notches)) + (lfo_r * 0.25) + comb * 0.4) % 1.0
                    else:
                        base = _clamp(0.18 + (k + 0.5) / float(n_notches) * 0.64 + lfo_l * 0.16 * (st["depth"] / 100.0) + comb * 0.18, 0.05, 0.95)
                        base_r = _clamp(0.18 + (k + 0.5) / float(n_notches) * 0.64 + lfo_r * 0.16 * (st["depth"] / 100.0) + comb * 0.18, 0.05, 0.95)
                    dl = (u - base) / 0.055
                    dr = (u - base_r) / 0.055
                    val_l += math.exp(-dl * dl)
                    val_r += math.exp(-dr * dr)
                amp = ih * (0.18 + 0.22 * abs(fb))
                pts_l.extend((px, cy + 10 - val_l * amp))
                pts_r.extend((px, cy + 16 - val_r * amp * 0.8))
            cv.create_line(*pts_r, fill=c_r, width=1.6, smooth=True)
            cv.create_line(*pts_l, fill=c_mode, width=2.2, smooth=True)
            if mode == 4:
                arrow = ">>> SHEPARD UP >>>" if fb >= 0.0 else "<<< SHEPARD DOWN <<<"
                cv.create_text(cx, inner_y1 - 8, text=arrow, fill=c_mode, font=("TkDefaultFont", 7, "bold"))
        else:
            # UBER: 8-tap diffused cloud constellation + OverMod aura
            halo_r = 22.0 + 48.0 * diffuse + 22.0 * over
            cv.create_oval(cx - halo_r * 1.4, cy - halo_r * 0.65, cx + halo_r * 1.4, cy + halo_r * 0.65,
                           fill=BYP_FILL if byp else UBER_FILL, outline=c_mode, width=1.2)
            for t in range(8):
                u = (t / 7.0) * 2.0 - 1.0
                ph = t * 0.785398
                ox = u * iw * (0.14 + 0.30 * spread) + math.sin(ph + lfo_l * 2.4) * 14.0 * (0.3 + 0.7 * over)
                oy = math.cos(ph + lfo_r * 2.4) * ih * (0.14 + 0.22 * (st["depth"] / 100.0))
                tx = _clamp(cx + ox, inner_x0 + 8, inner_x1 - 8)
                ty = _clamp(cy + oy, inner_y0 + 8, inner_y1 - 8)
                tr = 3.5 + 3.0 * diffuse
                cv.create_oval(tx - tr, ty - tr, tx + tr, ty + tr,
                               fill=c_l if (t % 2 == 0) else c_r, outline="")

        # Scrolling dual L/R LFO waveform trail
        if len(self.trail) >= 2:
            n = len(self.trail)
            pts_l = []
            pts_r = []
            for idx, (tl, tr, _em) in enumerate(self.trail):
                tx = inner_x0 + (idx / float(TRAIL_STEPS - 1)) * iw
                pts_l.extend((tx, cy - tl * (ih * 0.36)))
                pts_r.extend((tx, cy - tr * (ih * 0.36)))
            cv.create_line(*pts_r, fill=c_r, width=1.6, smooth=True)
            cv.create_line(*pts_l, fill=c_l, width=2.0, smooth=True)

        # Interactive Rate x Depth puck
        px = self._rate_to_x(st["rate"], inner_x0, inner_x1)
        py = inner_y1 - _clamp(st["depth"] / 100.0, 0.0, 1.0) * ih
        self.handle_pos["puck"] = (px, py)
        cv.create_line(px, inner_y0, px, inner_y1, fill=GRID_COL, width=1.0, dash=(2, 3))
        cv.create_line(inner_x0, py, inner_x1, py, fill=GRID_COL, width=1.0, dash=(2, 3))
        p_col = self._uber_color(st["overmod"] + st["warp_fx"] * 0.8, byp) if over > 0.25 else c_mode
        cv.create_oval(px - 9, py - 9, px + 9, py + 9, fill=p_col, outline=c_hi, width=2.0)
        cv.create_oval(px - 2.5, py - 2.5, px + 2.5, py + 2.5, fill="#101318", outline="")

        # Top header bar text
        if st["sync"]:
            div_lbl, _ = nearest_rate_division(st["rate"], bpm)
            rate_txt = f"{div_lbl} ({bpm:.0f})"
        else:
            rate_txt = _fmt_rate(st["rate"])

        title = f"{mode_name} · {shape_name} {rate_txt} · D {st['depth']:.0f}% · FB {st['feedback']:+.0f}%"
        cv.create_text(x0 + 10, y0 + 6, text=title, anchor="nw",
                       fill=c_hi, font=("TkDefaultFont", 8, "bold"))

        badges = []
        if byp:
            badges.append(("BYPASS", BYP_HI))
        else:
            if st["lfo_freeze"]:
                badges.append(("FREEZE", AMBER_COL))
            if st["overmod"] >= 20.0:
                badges.append((f"UBER {st['overmod']:.0f}%", self._uber_color(st["overmod"], byp)))
        bx = x1 - 8
        for txt, col in badges:
            cv.create_text(bx, y0 + 6, text=txt, anchor="ne", fill=col, font=("TkDefaultFont", 8, "bold"))
            bx -= len(txt) * 7 + 10

        # Bottom subtle info line inside scope
        foot = (f"DEL {st['delay_ms']:.1f}ms · SPR {st['spread']:.0f}% · "
                f"DIFF {st['diffuse']:.0f}% · MIX {st['mix']:.0f}% · ST {st['stereo']:.0f}°")
        cv.create_text(x0 + 10, y1 - 6, text=foot, anchor="sw",
                       fill=BYP_MID if byp else LABEL_DIM, font=("TkDefaultFont", 7, "bold"))
        if env_lvl > 0.05 and abs(st["env_mod"]) > 1.0:
            cv.create_text(x1 - 8, y1 - 6, text=f"ENV {st['env_mod']:+.0f}%", anchor="se",
                           fill=BYP_MID if byp else RIGHT_COL, font=("TkDefaultFont", 7, "bold"))

    def _draw_tone_uber_strip(self, cv, st, byp, x0, y0, x1, y1):
        # Left 62%: Tone band (Low Cut .. High Cut); Right 38%: OverMod heat bar
        gap = 6
        split_x = x0 + (x1 - x0) * 0.62
        tx0, ty0, tx1, ty1 = x0, y0, split_x - gap * 0.5, y1
        ox0, oy0, ox1, oy1 = split_x + gap * 0.5, y0, x1, y1
        self.tone_box = (tx0, ty0, tx1, ty1)
        self.over_box = (ox0, oy0, ox1, oy1)

        # Tone passband panel
        cv.create_rectangle(tx0, ty0, tx1, ty1, fill=PANEL_BG, outline=LINE_DIM, width=1.2)
        lx = self._freq_to_x(st["low_cut"], tx0 + 8, tx1 - 8)
        hx = self._freq_to_x(st["high_cut"], tx0 + 8, tx1 - 8)
        self.handle_pos["lc"] = (lx, (ty0 + ty1) * 0.5)
        self.handle_pos["hc"] = (hx, (ty0 + ty1) * 0.5)

        cv.create_rectangle(lx, ty0 + 3, hx, ty1 - 3,
                            fill=BYP_FILL if byp else BAND_FILL, outline="")
        c_lc = BYP_MID if byp else LEFT_COL
        c_hc = BYP_MID if byp else ORANGE_COL
        cv.create_line(lx, ty0 + 2, lx, ty1 - 2, fill=c_lc, width=2.4)
        cv.create_line(hx, ty0 + 2, hx, ty1 - 2, fill=c_hc, width=2.4)
        cv.create_text(tx0 + 8, (ty0 + ty1) * 0.5,
                       text=f"LC {_fmt_freq(st['low_cut'])}", anchor="w",
                       fill=BYP_HI if byp else LABEL_HI, font=("TkDefaultFont", 7, "bold"))
        cv.create_text((tx0 + tx1) * 0.5, (ty0 + ty1) * 0.5,
                       text=f"DRV {st['drive']:.0f}%", anchor="center",
                       fill=BYP_MID if byp else LABEL_DIM, font=("TkDefaultFont", 7, "bold"))
        cv.create_text(tx1 - 8, (ty0 + ty1) * 0.5,
                       text=f"HC {_fmt_freq(st['high_cut'])}", anchor="e",
                       fill=BYP_HI if byp else LABEL_HI, font=("TkDefaultFont", 7, "bold"))

        # OverMod heat bar
        cv.create_rectangle(ox0, oy0, ox1, oy1, fill=PANEL_BG, outline=LINE_DIM, width=1.2)
        ov = _clamp(st["overmod"] / 100.0, 0.0, 1.0)
        ov_col = self._uber_color(st["overmod"], byp)
        vx = ox0 + 2 + ov * (ox1 - ox0 - 4)
        if ov > 0.01:
            cv.create_rectangle(ox0 + 2, oy0 + 3, vx, oy1 - 3, fill=BYP_FILL if byp else ov_col, outline="")
        cv.create_line(vx, oy0 + 2, vx, oy1 - 2, fill=BYP_HI if byp else LABEL_HI, width=2.0)
        cv.create_text((ox0 + ox1) * 0.5, (oy0 + oy1) * 0.5,
                       text=f"OVERMOD {st['overmod']:.0f}%",
                       fill=BYP_HI if byp else ("#101318" if ov > 0.35 else LABEL_HI),
                       font=("TkDefaultFont", 8, "bold"))

    def _draw_meters(self, cv, byp, in_db, out_db, eff_mod, fb_en, gx1, y0, y1, w, pad):
        mx0 = gx1 + 6
        mx1 = w - pad
        cv.create_rectangle(mx0, y0, mx1, y1, fill=PANEL_BG, outline=LINE_DIM, width=1.2)

        top = y0 + 18
        bot = y1 - 16
        mh = max(20.0, bot - top)
        cols = (
            ("IN", _clamp((in_db - FLOOR_DB) / 72.0, 0.0, 1.0),
             _clamp((self.hold_in - FLOOR_DB) / 72.0, 0.0, 1.0), LEFT_COL),
            ("MD", _clamp(eff_mod, 0.0, 1.0), _clamp(self.hold_mod, 0.0, 1.0), UBER_COL),
            ("FB", _clamp(fb_en, 0.0, 1.0),   _clamp(self.hold_fb, 0.0, 1.0),  AMBER_COL),
            ("OT", _clamp((out_db - FLOOR_DB) / 72.0, 0.0, 1.0),
             _clamp((self.hold_out - FLOOR_DB) / 72.0, 0.0, 1.0), RIGHT_COL),
        )
        bw = (mx1 - mx0 - 12) / 4.0
        for i, (lbl, val, hold, col) in enumerate(cols):
            bx0 = mx0 + 4 + i * (bw + 1.3)
            bx1 = bx0 + bw - 1.5
            cv.create_rectangle(bx0, top, bx1, bot, fill="#101318", outline=GRID_COL, width=0.8)
            fy = bot - val * mh
            if val > 0.01:
                cv.create_rectangle(bx0 + 1, fy, bx1 - 1, bot - 1,
                                    fill=BYP_MID if byp else col, outline="")
            hy = bot - hold * mh
            cv.create_line(bx0, hy, bx1, hy, fill=BYP_HI if byp else LABEL_HI, width=1.4)
            cv.create_text((bx0 + bx1) * 0.5, y1 - 8, text=lbl,
                           fill=BYP_MID if byp else LABEL_DIM, font=("TkDefaultFont", 6, "bold"))

        cv.create_text((mx0 + mx1) * 0.5, y0 + 9, text="LEVELS",
                       fill=BYP_MID if byp else LABEL_DIM, font=("TkDefaultFont", 6, "bold"))

    def _draw_btn(self, cv, key, x0, y0, x1, y1, title, sub, active, col, byp):
        self.hit_rects[key] = (x0, y0, x1, y1)
        fill = (BYP_FILL if byp else col) if active else BUTTON_BG
        out = BYP_MID if byp else (col if active else LINE_DIM)
        fg_t = (BYP_HI if byp else "#101318") if active else (BYP_HI if byp else LABEL_HI)
        fg_s = (BYP_MID if byp else "#16191e") if active else (BYP_MID if byp else col)
        cv.create_rectangle(x0, y0, x1, y1, fill=fill, outline=out, width=1.4)
        cx = (x0 + x1) * 0.5
        cy = (y0 + y1) * 0.5
        if sub:
            cv.create_text(cx, cy - 7, text=title, fill=fg_t, font=("TkDefaultFont", 7, "bold"))
            cv.create_text(cx, cy + 7, text=sub, fill=fg_s, font=("TkDefaultFont", 8, "bold"))
        else:
            cv.create_text(cx, cy, text=title, fill=fg_t, font=("TkDefaultFont", 8, "bold"))

    def _draw_main_buttons(self, cv, st, byp, x0, y0, x1, y1, mode_name, mode_col, shape_name, shape_col):
        gap = 5
        bw = (x1 - x0 - gap * 5) / 6.0
        ov_pct = st["overmod"]
        uber_sub = "BYP" if byp else f"{ov_pct:.0f}%"
        uber_col = self._uber_color(ov_pct, byp) if ov_pct > 2.0 else UBER_COL

        specs = [
            ("mode",   "MODE",   mode_name,                     False, mode_col),
            ("shape",  "SHAPE",  shape_name,                    False, shape_col),
            ("sync",   "SYNC",   "ON" if st["sync"] else "FREE", st["sync"], RIGHT_COL),
            ("fx",     "LIVE",   "FX PAD",                      False, UBER_COL),
            ("freeze", "LFO",    "FREEZE" if st["lfo_freeze"] else "RUN", st["lfo_freeze"], AMBER_COL),
            ("uber",   "UBER*",  uber_sub,                      ov_pct >= 25.0 or byp, uber_col),
        ]
        for i, (key, t, s, act, col) in enumerate(specs):
            bx0 = x0 + i * (bw + gap)
            self._draw_btn(cv, key, bx0, y0, bx0 + bw, y1, t, s, act, col, byp)

    def _draw_fx_buttons(self, cv, st, byp, x0, y0, x1, y1, mode_name, mode_col):
        gap = 5
        bw = (x1 - x0 - gap * 5) / 6.0
        has_fx = (abs(st["rate_fx"]) > 0.5 or st["warp_fx"] > 0.5 or abs(st["comb_fx"]) > 0.5)
        specs = [
            ("fx",     "VIEW",   "MAIN",                              True,  LEFT_COL),
            ("snap",   "SPRING", "SNAP 0" if self.fx_snap else "HOLD", self.fx_snap, RIGHT_COL),
            ("zero",   "RESET",  "ZERO 0",                            has_fx, HOT_RED),
            ("freeze", "LFO",    "FREEZE" if st["lfo_freeze"] else "RUN", st["lfo_freeze"], AMBER_COL),
            ("mode",   "MODE",   mode_name,                           False, mode_col),
            ("uber",   "UBER*",  "BYP" if byp else f"{st['overmod']:.0f}%", byp or st["overmod"] >= 25.0, UBER_COL),
        ]
        for i, (key, t, s, act, col) in enumerate(specs):
            bx0 = x0 + i * (bw + gap)
            self._draw_btn(cv, key, bx0, y0, bx0 + bw, y1, t, s, act, col, byp)

    def _draw_fx_screen(self, cv, st, byp, bpm, eff_mod, fb_en, x0, y0, x1, y1, mode_name, mode_col):
        cv.create_rectangle(x0, y0, x1, y1, fill=PANEL_BG, outline=LINE_DIM, width=1.4)
        c_hi = BYP_HI if byp else LABEL_HI
        c_fx = BYP_MID if byp else UBER_COL

        cv.create_text(x0 + 10, y0 + 6,
                       text=f"LIVE FX · {mode_name} · BPM {bpm:.0f}",
                       anchor="nw", fill=c_hi, font=("TkDefaultFont", 8, "bold"))

        inner_x0 = x0 + 10
        inner_x1 = x1 - 10
        pad_y0 = y0 + 24
        slider_h = 34
        pad_y1 = y1 - slider_h - 14
        self.xy_box = (inner_x0, pad_y0, inner_x1, pad_y1)

        # X-Y Pad: X = COMB FX (-100..+100%), Y = WARP FX (0..100%)
        cv.create_rectangle(inner_x0, pad_y0, inner_x1, pad_y1, fill="#111419", outline=LINE_DIM, width=1.3)
        cx = (inner_x0 + inner_x1) * 0.5
        cy = pad_y1 - 8.0
        for frac in (0.25, 0.5, 0.75):
            gx = inner_x0 + (inner_x1 - inner_x0) * frac
            gy = pad_y0 + (pad_y1 - pad_y0) * frac
            cv.create_line(gx, pad_y0, gx, pad_y1, fill=ZERO_COL if frac == 0.5 else GRID_COL, width=1.0)
            cv.create_line(inner_x0, gy, inner_x1, gy, fill=GRID_COL, width=1.0)

        cfx = st["comb_fx"]
        wfx = st["warp_fx"]
        px = inner_x0 + _clamp((cfx + 100.0) / 200.0, 0.0, 1.0) * (inner_x1 - inner_x0)
        py = pad_y1 - _clamp(wfx / 100.0, 0.0, 1.0) * (pad_y1 - pad_y0)

        if abs(px - cx) > 2.0 or abs(py - pad_y1) > 2.0:
            cv.create_line(cx, pad_y1, px, py, fill=c_fx, width=2.4)
        glow_r = 14.0 + 16.0 * (wfx / 100.0) * (0.5 + 0.5 * fb_en)
        cv.create_oval(px - glow_r, py - glow_r, px + glow_r, py + glow_r,
                       fill="", outline=c_fx, width=1.2)
        cv.create_oval(px - 11, py - 11, px + 11, py + 11, fill=c_fx, outline=c_hi, width=2.0)
        cv.create_oval(px - 3, py - 3, px + 3, py + 3, fill="#101318", outline="")

        cv.create_text(inner_x0 + 6, pad_y0 + 4,
                       text=f"COMB FX {cfx:+.0f}% · WARP FX {wfx:.0f}%",
                       anchor="nw", fill=c_hi, font=("TkDefaultFont", 8, "bold"))
        if self.fx_snap:
            cv.create_text(inner_x1 - 6, pad_y0 + 4, text="SPRING",
                           anchor="ne", fill=BYP_MID if byp else RIGHT_COL, font=("TkDefaultFont", 7, "bold"))
        cv.create_text(inner_x0 + 6, pad_y1 - 4, text="-COMB", anchor="sw",
                       fill=BYP_MID if byp else LABEL_DIM, font=("TkDefaultFont", 7, "bold"))
        cv.create_text(inner_x1 - 6, pad_y1 - 4, text="+COMB", anchor="se",
                       fill=BYP_MID if byp else LABEL_DIM, font=("TkDefaultFont", 7, "bold"))
        cv.create_text(cx, pad_y0 + 14, text="UBER WARP ^", anchor="n",
                       fill=BYP_MID if byp else LABEL_DIM, font=("TkDefaultFont", 7, "bold"))

        # Bottom bipolar slider: RATE FX (-100..+100%) + "0" reset button
        sy0 = pad_y1 + 6
        sy1 = y1 - 8
        z_btn_w = 42
        bar_x1 = inner_x1 - z_btn_w - 6
        rfx = st["rate_fx"]
        self.fx_sliders["rate_fx"] = (inner_x0, sy0, bar_x1, sy1, True)
        c_act = BYP_MID if byp else LEFT_COL

        cv.create_rectangle(inner_x0, sy0, bar_x1, sy1, fill=BUTTON_BG, outline=LINE_DIM, width=1.2)
        mid_x = (inner_x0 + bar_x1) * 0.5
        cv.create_line(mid_x, sy0 + 2, mid_x, sy1 - 2, fill=ZERO_COL, width=1.2)
        vx = inner_x0 + _clamp((rfx + 100.0) / 200.0, 0.0, 1.0) * (bar_x1 - inner_x0)
        if abs(rfx) > 0.5:
            cv.create_rectangle(min(mid_x, vx), sy0 + 2, max(mid_x, vx), sy1 - 2,
                                fill=BYP_FILL if byp else c_act, outline="")
        cv.create_line(vx, sy0 + 2, vx, sy1 - 2, fill=LABEL_HI, width=2.4)
        cv.create_text(mid_x, (sy0 + sy1) * 0.5,
                       text=f"RATE FX (SPEED BEND)  {rfx:+.0f}%",
                       fill=LABEL_HI, font=("TkDefaultFont", 9, "bold"))

        zx0 = bar_x1 + 6
        zx1 = inner_x1
        self.hit_rects["z_rate_fx"] = (zx0, sy0, zx1, sy1)
        active_z = (abs(rfx) >= 0.75) and not byp
        cv.create_rectangle(zx0, sy0, zx1, sy1,
                            fill=c_act if active_z else BUTTON_BG,
                            outline=c_act, width=1.4)
        cv.create_text((zx0 + zx1) * 0.5, (sy0 + sy1) * 0.5, text="0",
                       fill="#101318" if active_z else LABEL_HI, font=("TkDefaultFont", 10, "bold"))

    def _apply_scope_drag(self, x, y):
        gx0, gy0, gx1, gy1 = self.scope_box
        ix0, ix1 = gx0 + 12, gx1 - 12
        iy0, iy1 = gy0 + 26, gy1 - 24
        rate = self._x_to_rate(x, ix0, ix1)
        depth = _clamp(((iy1 - y) / max(1.0, iy1 - iy0)) * 100.0, 0.0, 100.0)
        self._set("rate", round(rate, 3))
        self._set("depth", round(depth, 1))

    def _apply_tone_drag(self, which, x):
        tx0, _ty0, tx1, _ty1 = self.tone_box
        f = self._x_to_freq(x, tx0 + 8, tx1 - 8)
        if which == "lc":
            self._set("low_cut", round(_clamp(f, F_MIN, LOW_CUT_MAX), 1))
        else:
            self._set("high_cut", round(_clamp(f, HIGH_CUT_MIN, F_MAX), 1))

    def _apply_over_drag(self, x):
        ox0, _oy0, ox1, _oy1 = self.over_box
        t = _clamp((x - ox0) / max(1.0, ox1 - ox0), 0.0, 1.0)
        v = t * 100.0
        if v < 2.5:
            v = 0.0
        self._set("overmod", round(v, 1))

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
        self._set("comb_fx", round(_clamp(cfx, -100.0, 100.0), 1))
        self._set("warp_fx", round(_clamp(wfx, 0.0, 100.0), 1))

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
            self._set(sym, round(_clamp(t * 100.0, 0.0, 100.0), 1))

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
                        self._select_group("g_engine", "Engine")
                    self.refresh_gui()
                    return
                if key == "zero":
                    self._set("rate_fx", 0.0)
                    self._set("warp_fx", 0.0)
                    self._set("comb_fx", 0.0)
                    self._select_group("g_fx", "Live FX")
                    self.refresh_gui()
                    return
                if key.startswith("z_"):
                    self._set(key[2:], 0.0)
                    self._select_group("g_fx", "Live FX")
                    self.refresh_gui()
                    return
                if key == "mode":
                    cur = int(round(self._get("mode", 0.0)))
                    self._set("mode", (cur + 1) % len(MODES))
                    self._select_group("g_engine", "Engine")
                    self.refresh_gui()
                    return
                if key == "shape":
                    cur = int(round(self._get("shape", 0.0)))
                    self._set("shape", (cur + 1) % len(SHAPES))
                    self._select_group("g_lfo", "LFO")
                    self.refresh_gui()
                    return
                if key == "sync":
                    cur = self._get("sync", 0.0)
                    self._set("sync", 0 if cur >= 0.5 else 1)
                    self._select_group("g_lfo", "LFO")
                    self.refresh_gui()
                    return
                if key == "snap":
                    self.fx_snap = not self.fx_snap
                    self._select_group("g_fx", "Live FX")
                    self.refresh_gui()
                    return
                if key == "freeze":
                    cur = self._get("lfo_freeze", 0.0)
                    self._set("lfo_freeze", 0 if cur >= 0.5 else 1)
                    self._select_group("g_master", "Master")
                    self.refresh_gui()
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

        # 3. Tone passband strip
        tx0, ty0, tx1, ty1 = self.tone_box
        if tx0 <= x <= tx1 and ty0 <= y <= ty1:
            lx = self.handle_pos.get("lc", (tx0, 0))[0]
            hx = self.handle_pos.get("hc", (tx1, 0))[0]
            which = "lc" if abs(x - lx) <= abs(x - hx) else "hc"
            self.drag_mode = ("tone", which)
            self._select_group("g_tone", "Tone")
            self._apply_tone_drag(which, x)
            self.refresh_gui()
            return

        # 4. OverMod bar
        ox0, oy0, ox1, oy1 = self.over_box
        if ox0 <= x <= ox1 and oy0 <= y <= oy1:
            self.drag_mode = ("over",)
            self._select_group("g_uber", "Uber")
            self._apply_over_drag(x)
            self.refresh_gui()
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
                self.refresh_gui()
                return
            self._last_scope_tap = now
            self.drag_mode = ("scope",)
            self._select_group("g_lfo", "LFO")
            self._apply_scope_drag(x, y)
            self.refresh_gui()

    def _on_motion(self, event):
        if not self.drag_mode:
            return
        kind = self.drag_mode[0]
        if kind == "scope":
            self._apply_scope_drag(event.x, event.y)
            self.refresh_gui()
        elif kind == "tone":
            self._apply_tone_drag(self.drag_mode[1], event.x)
            self.refresh_gui()
        elif kind == "over":
            self._apply_over_drag(event.x)
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
                self.refresh_gui()
            elif kind == "xy" and self.fx_snap:
                self._set("comb_fx", 0.0)
                self._set("warp_fx", 0.0)
                self.refresh_gui()
            elif kind == "fx" and self.fx_snap:
                self._set(self.drag_mode[1], 0.0)
                self.refresh_gui()
        self.drag_mode = None
