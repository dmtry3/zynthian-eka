#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# SPDX-License-Identifier: GPL-3.0-or-later
# Zynthian widget for Eka Echo (class and file name: zynthian_widget_eka_echo).
import math
import time
import tkinter

from zyngui import zynthian_gui_config
from zyngui import zynthian_widget_base

MULTI_INSTANCE = True

REFRESH_SEC = 0.08
TRACE_STEPS = 48
MIN_MS = 5.0
MAX_MS = 2500.0
F_MIN = 20.0
F_MAX = 20000.0
FLOOR_DB = -60.0
LONG_PRESS_SEC = 0.45

DIVISIONS = (
    ("1/64",   1.0 / 16.0),
    ("1/32T",  1.0 / 12.0),
    ("1/64.",  3.0 / 32.0),
    ("1/32",   1.0 / 8.0),
    ("1/16T",  1.0 / 6.0),
    ("1/32.",  3.0 / 16.0),
    ("1/16",   1.0 / 4.0),
    ("1/8T",   1.0 / 3.0),
    ("1/16.",  3.0 / 8.0),
    ("1/8",    1.0 / 2.0),
    ("1/4T",   2.0 / 3.0),
    ("1/8.",   3.0 / 4.0),
    ("1/4",    1.0),
    ("1/2T",   4.0 / 3.0),
    ("1/4.",   1.5),
    ("1/2",    2.0),
    ("1/2.",   3.0),
    ("1 bar",  4.0),
)

MODES = (
    ("CLEAN", "#4dabf7"),
    ("TAPE",  "#ffa94d"),
    ("BBD",   "#ff8787"),
    ("DIFF",  "#b197fc"),
)

ROUTINGS = (
    ("STEREO",   "#63e6be"),
    ("PINGPONG", "#ffd43b"),
    ("CROSS",    "#74c0fc"),
)

PANEL_BG    = "#16191e"
BUTTON_BG   = "#1f242c"
LINE_DIM    = "#2f3640"
GRID_COL    = "#252b34"
ZERO_COL    = "#3a4352"
LABEL_DIM   = "#8b949e"
LABEL_HI    = "#e6edf3"
LEFT_COL    = "#4dabf7"
RIGHT_COL   = "#63e6be"
DUCK_COL    = "#ff922b"
FREEZE_COL  = "#74c0fc"
TRAILS_COL  = "#ffd43b"
SYNC_COL    = "#63e6be"
LINK_COL    = "#da77f2"
HOT_RED     = "#ff6b6b"
BAND_FILL   = "#20364a"
BAND_LINE   = "#4dabf7"

BYP_DIM     = "#49505a"
BYP_MID     = "#68707c"
BYP_HI      = "#9aa2ae"
BYP_FILL    = "#252930"


def _clamp(v, lo, hi):
    return lo if v < lo else (hi if v > hi else v)


def _fmt_freq(f):
    if f >= 1000.0:
        return f"{f / 1000.0:.1f}k"
    return f"{f:.0f}Hz"


def nearest_division(ms, bpm):
    bpm = _clamp(bpm, 20.0, 400.0)
    q_ms = 60000.0 / bpm
    log_t = math.log(_clamp(ms, MIN_MS, MAX_MS))
    best_lbl, best_ms, best_err = "1/4", ms, 1e9
    for lbl, mult in DIVISIONS:
        cand = q_ms * mult
        if cand < MIN_MS or cand > MAX_MS:
            continue
        err = abs(math.log(cand) - log_t)
        if err < best_err:
            best_err = err
            best_lbl = lbl
            best_ms = cand
    return best_lbl, best_ms


def _fmt_sync_ms(ms, bpm):
    lbl, q_ms = nearest_division(ms, bpm)
    if abs(ms - q_ms) / max(1.0, q_ms) <= 0.025:
        return lbl
    return f"{ms:.0f}ms"


class zynthian_widget_eka_echo(zynthian_widget_base.zynthian_widget_base):
    def __init__(self, parent):
        super().__init__(parent)
        self.next_tick = 0.0
        self.wet_l_ring = [FLOOR_DB] * TRACE_STEPS
        self.wet_r_ring = [FLOOR_DB] * TRACE_STEPS
        self.hold_in = FLOOR_DB
        self.hold_out = FLOOR_DB
        self.hold_duck = 0.0

        self.tap_box = (0.0, 0.0, 1.0, 1.0)
        self.tone_box = (0.0, 0.0, 1.0, 1.0)
        self.xy_box = (0.0, 0.0, 1.0, 1.0)
        self.fx_sliders = {}   # sym -> (x0, y0, x1, y1, bipolar)
        self.fx_view = False   # False = Tap Map, True = dedicated Live Effects screen
        self.fx_snap = False   # False = HOLD, True = SNAP 0 (spring-return on release)
        self.handle_pos = {}
        self.hit_rects = {}
        self._last_ctrl_titles = {}
        self.drag_mode = None  # ("tap", "L"|"R", ratio) | ("tone", "hpf"|"lpf") | ("xy",) | ("fx", sym) | ("trails", t0)

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
            lbl_l, _ = nearest_division(st["time_l"], bpm)
            lbl_r, _ = nearest_division(st["time_r"], bpm)
            want = {"time_l": f"Time L {lbl_l}", "time_r": f"Time R {lbl_r}"}
        else:
            want = {"time_l": "Time L", "time_r": "Time R"}
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
            "time_l":      self._get("time_l", 375.0),
            "time_r":      self._get("time_r", 500.0),
            "feedback":    self._get("feedback", 42.0),
            "mix":         self._get("mix", 35.0),
            "mode":        int(_clamp(round(self._get("mode", 1.0)), 0, 3)),
            "routing":     int(_clamp(round(self._get("routing", 0.0)), 0, 2)),
            "sync":        self._get("sync", 1.0) >= 0.5,
            "width":       self._get("width", 100.0),
            "hpf_hz":      self._get("hpf_hz", 80.0),
            "lpf_hz":      self._get("lpf_hz", 8500.0),
            "drive_pct":   self._get("drive_pct", 15.0),
            "diffuse_pct": self._get("diffuse_pct", 0.0),
            "mod_rate":    self._get("mod_rate", 0.6),
            "mod_depth":   self._get("mod_depth", 18.0),
            "duck_pct":    self._get("duck_pct", 0.0),
            "duck_rel":    self._get("duck_rel", 220.0),
            "freeze":      self._get("freeze", 0.0) >= 0.5,
            "trails":      self._get("trails", 0.0) >= 0.5,
            "output_db":   self._get("output_db", 0.0),
            "bypass":      self._get("bypass", 0.0) >= 0.5,
            "time_fx":     self._get("time_fx", 0.0),
            "time_link":   self._get("time_link", 1.0) >= 0.5,
            "filter_fx":   self._get("filter_fx", 0.0),
            "wash_fx":     self._get("wash_fx", 0.0),
        }

    def _ms_to_x(self, ms, x0, x1):
        t = math.sqrt(_clamp((ms - MIN_MS) / (MAX_MS - MIN_MS), 0.0, 1.0))
        return x0 + t * (x1 - x0)

    def _x_to_ms(self, x, x0, x1):
        t = _clamp((x - x0) / max(1.0, x1 - x0), 0.0, 1.0)
        return MIN_MS + (t * t) * (MAX_MS - MIN_MS)

    def _freq_to_x(self, f, x0, x1):
        t = math.log(_clamp(f, F_MIN, F_MAX) / F_MIN) / math.log(F_MAX / F_MIN)
        return x0 + t * (x1 - x0)

    def _x_to_freq(self, x, x0, x1):
        t = _clamp((x - x0) / max(1.0, x1 - x0), 0.0, 1.0)
        return F_MIN * ((F_MAX / F_MIN) ** t)

    def _pull_telemetry(self, st):
        in_db = _clamp(float(self.monitors.get("in_peak", FLOOR_DB)), FLOOR_DB, 12.0)
        out_db = _clamp(float(self.monitors.get("out_peak", FLOOR_DB)), FLOOR_DB, 12.0)
        wl_db = _clamp(float(self.monitors.get("wet_l_db", FLOOR_DB)), FLOOR_DB, 12.0)
        wr_db = _clamp(float(self.monitors.get("wet_r_db", FLOOR_DB)), FLOOR_DB, 12.0)
        dk_db = _clamp(float(self.monitors.get("duck_db", 0.0)), 0.0, 24.0)
        bpm = _clamp(float(self.monitors.get("host_bpm", 120.0)), 20.0, 400.0)
        act_l = _clamp(float(self.monitors.get("actual_ms_l", st["time_l"])), MIN_MS, MAX_MS)
        act_r = _clamp(float(self.monitors.get("actual_ms_r", st["time_r"])), MIN_MS, MAX_MS)

        self.hold_in = in_db if in_db >= self.hold_in else max(in_db, self.hold_in - 1.2)
        self.hold_out = out_db if out_db >= self.hold_out else max(out_db, self.hold_out - 1.2)
        self.hold_duck = dk_db if dk_db >= self.hold_duck else max(dk_db, self.hold_duck - 0.8)

        self.wet_l_ring.pop(0)
        self.wet_l_ring.append(wl_db)
        self.wet_r_ring.pop(0)
        self.wet_r_ring.append(wr_db)
        return in_db, out_db, dk_db, bpm, act_l, act_r

    def refresh_gui(self):
        cv = self.widget_canvas
        w = max(240, int(self.width or cv.winfo_width() or 640))
        h = max(180, int(self.height or cv.winfo_height() or 380))
        st = self._read_state()
        byp = st["bypass"]
        in_db, out_db, dk_db, bpm, act_l, act_r = self._pull_telemetry(st)

        self._sync_encoder_labels(st, bpm)

        cv.delete("all")
        self.hit_rects.clear()
        self.handle_pos.clear()
        self.fx_sliders.clear()

        pad = 6
        btn_h = max(36, min(44, int(h * 0.12)))
        tone_h = max(28, min(36, int(h * 0.095)))
        meter_w = 60

        gx0 = pad
        gy0 = pad
        gx1 = w - pad - meter_w - 6
        gy1 = h - pad - btn_h - tone_h - 12
        ty1 = gy1 + 5 + tone_h
        self.tap_box = (gx0, gy0, gx1, gy1)

        mode_name, mode_col = MODES[st["mode"]]
        route_name, route_col = ROUTINGS[st["routing"]]

        if self.fx_view:
            self._draw_fx_screen(cv, st, byp, bpm, act_l, act_r, gx0, gy0, gx1, ty1, mode_name, mode_col)
            self._draw_meters(cv, byp, in_db, out_db, dk_db, gx1, gy0, ty1, w, pad)
            by0 = ty1 + 6
            by1 = h - pad
            fx_nonzero = (abs(st["time_fx"]) >= 0.75 or abs(st["filter_fx"]) >= 0.75 or st["wash_fx"] >= 0.75)
            buttons = (
                ("fx",     "MAIN",                            BYP_MID if byp else LEFT_COL,   False),
                ("link",   "LINK" if st["time_link"] else "L ONLY",
                           BYP_MID if byp else LINK_COL,      st["time_link"] and not byp),
                ("snap",   "SNAP 0" if self.fx_snap else "HOLD",
                           BYP_MID if byp else SYNC_COL,      self.fx_snap and not byp),
                ("zero",   "ZERO 0",                          BYP_MID if byp else TRAILS_COL, fx_nonzero and not byp),
                ("freeze", "FREEZE",                          BYP_MID if byp else FREEZE_COL, st["freeze"] and not byp),
                ("trails", "BYPASS" if byp else ("MUTE IN" if st["trails"] else "TRAILS"),
                           BYP_HI if byp else TRAILS_COL,     byp or st["trails"]),
            )
            self._draw_buttons(cv, buttons, w, pad, by0, by1)
            return

        mid_y = (gy0 + gy1) * 0.5
        half_h = max(10.0, (gy1 - gy0) * 0.46)

        l_col = BYP_MID if byp else (FREEZE_COL if st["freeze"] else LEFT_COL)
        r_col = BYP_MID if byp else (FREEZE_COL if st["freeze"] else RIGHT_COL)

        # 1. Tap Map panel background & time grid
        cv.create_rectangle(gx0, gy0, gx1, gy1, fill=PANEL_BG, outline=LINE_DIM, width=1)
        cv.create_line(gx0, mid_y, gx1, mid_y, fill=ZERO_COL, width=1.4)
        if st["sync"]:
            q_ms = 60000.0 / bpm
            ticks = []
            last_xx = -1e9
            for lbl, mult in (("1/16", 0.25), ("1/8", 0.5), ("1/4", 1.0), ("1/2", 2.0), ("1 bar", 4.0)):
                ms_t = q_ms * mult
                if MIN_MS <= ms_t <= MAX_MS:
                    xx = self._ms_to_x(ms_t, gx0, gx1)
                    if xx - last_xx >= 32.0:
                        ticks.append((xx, lbl))
                        last_xx = xx
            for xx, lbl in ticks:
                cv.create_line(xx, gy0 + 18, xx, gy1 - 14, fill=GRID_COL, width=1)
                cv.create_text(xx, gy1 - 3, text=lbl, anchor="s", fill=SYNC_COL if not byp else LABEL_DIM,
                               font=("TkDefaultFont", 8))
        else:
            for ms_tick, lbl in ((100, "100"), (250, "250"), (500, "500"), (1000, "1s"), (1500, "1.5s"), (2000, "2s")):
                xx = self._ms_to_x(ms_tick, gx0, gx1)
                cv.create_line(xx, gy0 + 18, xx, gy1 - 14, fill=GRID_COL, width=1)
                cv.create_text(xx, gy1 - 3, text=lbl, anchor="s", fill=LABEL_DIM, font=("TkDefaultFont", 8))

        # Live scrolling wet envelope ribbon (L up, R down)
        step_x = (gx1 - gx0) / float(max(1, TRACE_STEPS - 1))
        for i in range(TRACE_STEPS):
            xx = gx0 + i * step_x
            nl = _clamp((self.wet_l_ring[i] - FLOOR_DB) / (0.0 - FLOOR_DB), 0.0, 1.0)
            nr = _clamp((self.wet_r_ring[i] - FLOOR_DB) / (0.0 - FLOOR_DB), 0.0, 1.0)
            if nl > 0.03:
                cv.create_line(xx, mid_y, xx, mid_y - nl * half_h * 0.55,
                               fill=BYP_FILL if byp else "#1e3852", width=2)
            if nr > 0.03:
                cv.create_line(xx, mid_y, xx, mid_y + nr * half_h * 0.55,
                               fill=BYP_FILL if byp else "#1a3c35", width=2)

        # Echo tap decay impulses (L above midline, R below midline)
        wash = _clamp(st["wash_fx"], 0.0, 100.0) * 0.01
        if st["freeze"]:
            fb = 0.96
        else:
            fb = _clamp(st["feedback"] * 0.01, 0.0, 1.05)
            if wash > 1e-4:
                w_fb = wash * (0.45 + 0.55 * wash)
                fb = _clamp(fb + w_fb * (max(fb, 0.94) - fb), 0.0, 1.05)
        fx_active = abs(st["time_fx"]) >= 0.75
        ms_l = act_l if (st["sync"] or fx_active) else st["time_l"]
        ms_r = act_r if (st["sync"] or (fx_active and st["time_link"])) else st["time_r"]

        def _draw_taps(t_ms, up, col, start_t=0.0):
            amp = 0.92
            t = start_t + t_ms
            for _ in range(16):
                if t > MAX_MS or amp < 0.05:
                    break
                xx = self._ms_to_x(t, gx0, gx1)
                h_px = amp * half_h * 0.82
                y2 = mid_y - h_px if up else mid_y + h_px
                cv.create_line(xx, mid_y, xx, y2, fill=col, width=2.4)
                cv.create_oval(xx - 2.5, y2 - 2.5, xx + 2.5, y2 + 2.5, fill=col, outline="")
                amp *= min(fb, 0.94)
                t += t_ms

        if st["routing"] == 1:  # Ping-Pong alternating L -> R -> L -> R
            amp = 0.92
            t = ms_l
            is_left = True
            pp_lr = min(0.70 + 0.30 * fb, 0.94)
            pp_rl = min(fb, 0.94)
            for _ in range(18):
                if t > MAX_MS or amp < 0.05:
                    break
                xx = self._ms_to_x(t, gx0, gx1)
                h_px = amp * half_h * 0.82
                y2 = (mid_y - h_px) if is_left else (mid_y + h_px)
                col = l_col if is_left else r_col
                cv.create_line(xx, mid_y, xx, y2, fill=col, width=2.4)
                cv.create_oval(xx - 2.5, y2 - 2.5, xx + 2.5, y2 + 2.5, fill=col, outline="")
                amp *= (pp_lr if is_left else pp_rl)
                t += (ms_r if is_left else ms_l)
                is_left = not is_left
        else:
            _draw_taps(ms_l, True, l_col)
            _draw_taps(ms_r, False, r_col)

        # Draggable L & R handles
        fb_norm = _clamp(st["feedback"] / 110.0, 0.0, 1.0)
        lx = self._ms_to_x(ms_l, gx0, gx1)
        ly = mid_y - (0.18 + fb_norm * 0.72) * half_h
        rx = self._ms_to_x(ms_r, gx0, gx1)
        ry = mid_y + (0.18 + fb_norm * 0.72) * half_h

        for key, hx, hy, col, lbl, t_ms in (
            ("L", lx, ly, l_col, "L", ms_l),
            ("R", rx, ry, r_col, "R", ms_r),
        ):
            cv.create_oval(hx - 12, hy - 12, hx + 12, hy + 12,
                           fill=col, outline=BYP_HI if byp else LABEL_HI, width=1.8)
            cv.create_text(hx, hy, text=lbl, fill="#101318", font=("TkDefaultFont", 9, "bold"))
            if st["sync"]:
                tag = _fmt_sync_ms(t_ms, bpm)
                tx = hx + 16 if hx < (gx1 - 46) else hx - 16
                anc = "w" if hx < (gx1 - 46) else "e"
                cv.create_text(tx, hy, text=tag, anchor=anc, fill=col, font=("TkDefaultFont", 8, "bold"))
            self.handle_pos[key] = (hx, hy)

        # Header info inside Tap Map
        fx_tag = f" · FX {st['time_fx']:+.0f}%" if fx_active else ""
        if st["sync"]:
            lbl_l = _fmt_sync_ms(ms_l, bpm)
            lbl_r = _fmt_sync_ms(ms_r, bpm)
            hdr_l = f"L {lbl_l} · R {lbl_r}{fx_tag} · FB {st['feedback']:.0f}% · MIX {st['mix']:.0f}%"
        else:
            hdr_l = f"L {ms_l:.0f}ms · R {ms_r:.0f}ms{fx_tag} · FB {st['feedback']:.0f}% · MIX {st['mix']:.0f}%"
        cv.create_text(gx0 + 6, gy0 + 4, text=hdr_l, anchor="nw",
                       fill=BYP_HI if byp else LABEL_HI, font=("TkDefaultFont", 8, "bold"))

        if byp:
            badge_txt, badge_col = "BYPASS", BYP_HI
        elif st["freeze"]:
            badge_txt, badge_col = "FREEZE", FREEZE_COL
        elif st["trails"]:
            badge_txt, badge_col = "TRAILS (MUTE IN)", TRAILS_COL
        else:
            badge_txt, badge_col = f"{mode_name} · {bpm:.0f} BPM", mode_col
        cv.create_text(gx1 - 6, gy0 + 4, text=badge_txt, anchor="ne",
                       fill=badge_col, font=("TkDefaultFont", 8, "bold"))

        # 2. Interactive Loop Filter bar (HPF — LPF, reflecting active Filter FX)
        ty0 = gy1 + 5
        tx0 = gx0
        tx1 = gx1
        self.tone_box = (tx0, ty0, tx1, ty1)
        cv.create_rectangle(tx0, ty0, tx1, ty1, fill=PANEL_BG, outline=LINE_DIM, width=1)
        base_hx = self._freq_to_x(st["hpf_hz"], tx0 + 8, tx1 - 8)
        base_lx = self._freq_to_x(st["lpf_hz"], tx0 + 8, tx1 - 8)
        eff_hpf = _clamp(st["hpf_hz"], 20.0, 1000.0)
        eff_lpf = _clamp(st["lpf_hz"], 500.0, 20000.0)
        ffx = _clamp(st["filter_fx"], -100.0, 100.0) * 0.01
        if ffx < -0.005:
            eff_lpf = min(eff_lpf, 20000.0 * ((280.0 / 20000.0) ** (-ffx)))
        elif ffx > 0.005:
            eff_hpf = max(eff_hpf, 20.0 * ((3200.0 / 20.0) ** ffx))
        if eff_hpf > eff_lpf * 0.85:
            eff_hpf = eff_lpf * 0.85
        hx = self._freq_to_x(eff_hpf, tx0 + 8, tx1 - 8)
        lx_f = self._freq_to_x(eff_lpf, tx0 + 8, tx1 - 8)
        if lx_f < hx + 4:
            lx_f = hx + 4
        cv.create_rectangle(hx, ty0 + 2, lx_f, ty1 - 2,
                            fill=BYP_FILL if byp else BAND_FILL, outline="")
        cv.create_line(hx, ty0 + 2, hx, ty1 - 2, fill=BYP_MID if byp else BAND_LINE, width=2.2)
        cv.create_line(lx_f, ty0 + 2, lx_f, ty1 - 2, fill=BYP_MID if byp else RIGHT_COL, width=2.2)
        self.handle_pos["hpf"] = (base_hx, (ty0 + ty1) * 0.5)
        self.handle_pos["lpf"] = (base_lx, (ty0 + ty1) * 0.5)

        fx_flt_tag = " [FX]" if abs(ffx) > 0.005 else ""
        tone_lbl = f"LOOP HPF {_fmt_freq(eff_hpf)} — LPF {_fmt_freq(eff_lpf)}{fx_flt_tag}  ·  DRV {st['drive_pct']:.0f}%  ·  MOD {st['mod_depth']:.0f}%"
        cv.create_text((tx0 + tx1) * 0.5, (ty0 + ty1) * 0.5, text=tone_lbl,
                       fill=BYP_HI if byp else LABEL_HI, font=("TkDefaultFont", 8, "bold"))

        # 3. Vertical Meters on the right: IN | DK | OUT
        self._draw_meters(cv, byp, in_db, out_db, dk_db, gx1, gy0, ty1, w, pad)

        # 4. Bottom 6 touch buttons: MODE | ROUTE | SYNC | FX | FREEZE | TRAILS (long press = BYPASS)
        by0 = ty1 + 6
        by1 = h - pad
        fx_nonzero = (abs(st["time_fx"]) >= 0.75 or abs(st["filter_fx"]) >= 0.75 or st["wash_fx"] >= 0.75)
        buttons = (
            ("mode",   mode_name,                          BYP_MID if byp else mode_col,   False),
            ("route",  route_name,                         BYP_MID if byp else route_col,  False),
            ("sync",   "SYNC" if st["sync"] else "FREE",   BYP_MID if byp else SYNC_COL,   st["sync"] and not byp),
            ("fx",     "FX" if not fx_nonzero else "FX *", BYP_MID if byp else LINK_COL,   fx_nonzero and not byp),
            ("freeze", "FREEZE",                           BYP_MID if byp else FREEZE_COL, st["freeze"] and not byp),
            ("trails", "BYPASS" if byp else ("MUTE IN" if st["trails"] else "TRAILS"),
                       BYP_HI if byp else TRAILS_COL,      byp or st["trails"]),
        )
        self._draw_buttons(cv, buttons, w, pad, by0, by1)

    def _draw_meters(self, cv, byp, in_db, out_db, dk_db, gx1, gy0, ty1, w, pad):
        mx0 = gx1 + 6
        my1 = ty1
        mw = (w - pad - mx0 - 8) / 3.0
        for mi, (lbl, val, hold, col, is_duck) in enumerate((
            ("IN",  in_db,  self.hold_in,   BYP_MID if byp else LEFT_COL,  False),
            ("DK",  dk_db,  self.hold_duck, BYP_MID if byp else DUCK_COL,  True),
            ("OUT", out_db, self.hold_out,  BYP_MID if byp else RIGHT_COL, False),
        )):
            bx0 = mx0 + mi * (mw + 4)
            bx1 = bx0 + mw
            bar_bot = my1 - 12
            cv.create_rectangle(bx0, gy0, bx1, bar_bot, fill=PANEL_BG, outline=LINE_DIM, width=1)
            if is_duck:
                norm = _clamp(val / 24.0, 0.0, 1.0)
                my = gy0 + norm * (bar_bot - gy0)
                if norm > 0.01:
                    cv.create_rectangle(bx0 + 1, gy0 + 1, bx1 - 1, my, fill=col, outline="")
            else:
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

    def _draw_fx_screen(self, cv, st, byp, bpm, act_l, act_r, x0, y0, x1, y1, mode_name, mode_col):
        cv.create_rectangle(x0, y0, x1, y1, fill=PANEL_BG, outline=LINE_DIM, width=1)
        link_txt = "LINK" if st["time_link"] else "L ONLY"
        if st["sync"]:
            bl, _ = nearest_division(st["time_l"], bpm)
            br, _ = nearest_division(st["time_r"], bpm)
            nl = _fmt_sync_ms(act_l, bpm)
            nr = _fmt_sync_ms(act_r, bpm)
            hdr = f"FX · {bl}/{br} → {nl}/{nr} · {link_txt}"
        else:
            hdr = f"FX · {st['time_l']:.0f}/{st['time_r']:.0f}ms → {act_l:.0f}/{act_r:.0f}ms · {link_txt}"
        cv.create_text(x0 + 8, y0 + 5, text=hdr, anchor="nw",
                       fill=BYP_HI if byp else LABEL_HI, font=("TkDefaultFont", 8, "bold"))
        if byp:
            badge, badge_col = "BYPASS", BYP_HI
        elif st["freeze"]:
            badge, badge_col = "FREEZE", FREEZE_COL
        elif st["trails"]:
            badge, badge_col = "TRAILS (MUTE IN)", TRAILS_COL
        else:
            badge, badge_col = f"{mode_name} · {bpm:.0f} BPM", mode_col
        cv.create_text(x1 - 8, y0 + 5, text=badge, anchor="ne",
                       fill=badge_col, font=("TkDefaultFont", 8, "bold"))

        inner_x0 = x0 + 8
        inner_x1 = x1 - 8
        wash_h = max(30.0, min(38.0, (y1 - y0) * 0.17))
        pad_y0 = y0 + 22
        pad_y1 = y1 - wash_h - 14
        self.xy_box = (inner_x0, pad_y0, inner_x1, pad_y1)

        # 1. X-Y Pad: X = TIME FX (-100..+100%), Y = FILTER FX (down=LPF -100%, up=HPF +100%)
        cv.create_rectangle(inner_x0, pad_y0, inner_x1, pad_y1, fill=BUTTON_BG, outline=LINE_DIM, width=1.2)
        cx = (inner_x0 + inner_x1) * 0.5
        cy = (pad_y0 + pad_y1) * 0.5
        pw = max(1.0, inner_x1 - inner_x0)
        ph = max(1.0, pad_y1 - pad_y0)

        # Subtle live wet envelope ribbon in the background of the X-Y pad
        step_x = pw / float(max(1, TRACE_STEPS - 1))
        for i in range(TRACE_STEPS):
            xx = inner_x0 + i * step_x
            nl = _clamp((self.wet_l_ring[i] - FLOOR_DB) / (0.0 - FLOOR_DB), 0.0, 1.0)
            nr = _clamp((self.wet_r_ring[i] - FLOOR_DB) / (0.0 - FLOOR_DB), 0.0, 1.0)
            if nl > 0.04:
                cv.create_line(xx, cy, xx, cy - nl * ph * 0.34,
                               fill=BYP_FILL if byp else "#1c3044", width=2)
            if nr > 0.04:
                cv.create_line(xx, cy, xx, cy + nr * ph * 0.34,
                               fill=BYP_FILL if byp else "#19352f", width=2)

        # Reference grid lines: 0.5x (-50%), 1x (0%), 2x (+50%) and FLAT (0%)
        for frac, lbl in ((-0.5, "0.5x"), (0.0, "1x"), (0.5, "2x")):
            gx = cx + frac * pw * 0.5
            cv.create_line(gx, pad_y0 + 1, gx, pad_y1 - 1,
                           fill=ZERO_COL if frac == 0.0 else GRID_COL,
                           width=1.4 if frac == 0.0 else 1.0)
            cv.create_text(gx, pad_y1 - 3, text=lbl, anchor="s",
                           fill=LABEL_DIM, font=("TkDefaultFont", 7, "bold"))
        cv.create_line(inner_x0 + 1, cy, inner_x1 - 1, cy, fill=ZERO_COL, width=1.4)

        # Axis direction hints
        cv.create_text(cx, pad_y0 + 3, text="▲ HPF", anchor="n",
                       fill=BYP_DIM if byp else RIGHT_COL, font=("TkDefaultFont", 7, "bold"))
        cv.create_text(cx, pad_y1 - 14, text="▼ LPF", anchor="s",
                       fill=BYP_DIM if byp else BAND_LINE, font=("TkDefaultFont", 7, "bold"))
        cv.create_text(inner_x0 + 5, cy - 3, text="0.25x", anchor="sw",
                       fill=LABEL_DIM, font=("TkDefaultFont", 7))
        cv.create_text(inner_x1 - 5, cy - 3, text="4x", anchor="se",
                       fill=LABEL_DIM, font=("TkDefaultFont", 7))

        tfx = st["time_fx"]
        mult = 1.0 if abs(tfx) < 0.75 else (2.0 ** (2.0 * (tfx * 0.01)))
        ffx = st["filter_fx"]
        px = cx + _clamp(tfx / 100.0, -1.0, 1.0) * (pw * 0.5)
        py = cy - _clamp(ffx / 100.0, -1.0, 1.0) * (ph * 0.5)

        xy_active = (abs(tfx) >= 0.75 or abs(ffx) >= 0.75)
        if byp:
            p_col = BYP_MID
        elif abs(tfx) >= abs(ffx) and abs(tfx) >= 0.75:
            p_col = LEFT_COL if tfx < 0 else "#ffa94d"
        elif abs(ffx) >= 0.75:
            p_col = RIGHT_COL if ffx > 0 else BAND_LINE
        else:
            p_col = LABEL_HI

        if xy_active:
            cv.create_line(px, pad_y0 + 2, px, pad_y1 - 2, fill=GRID_COL, width=1.2)
            cv.create_line(inner_x0 + 2, py, inner_x1 - 2, py, fill=GRID_COL, width=1.2)
            cv.create_line(cx, cy, px, py, fill=p_col, width=2.2)

        r = 12.0
        cv.create_oval(px - r, py - r, px + r, py + r,
                       fill=p_col, outline=BYP_HI if byp else LABEL_HI, width=2.0)
        cv.create_oval(px - 3, py - 3, px + 3, py + 3, fill="#101318", outline="")

        flt_txt = f"HPF {ffx:+.0f}%" if ffx > 0.5 else (f"LPF {ffx:+.0f}%" if ffx < -0.5 else "FLAT")
        xy_readout = f"TIME {tfx:+.0f}% ({mult:.2f}x) · {flt_txt}"
        cv.create_text(inner_x0 + 6, pad_y0 + 4, text=xy_readout, anchor="nw",
                       fill=BYP_HI if byp else LABEL_HI, font=("TkDefaultFont", 8, "bold"))
        if self.fx_snap:
            cv.create_text(inner_x1 - 6, pad_y0 + 4, text="SPRING", anchor="ne",
                           fill=BYP_MID if byp else SYNC_COL, font=("TkDefaultFont", 7, "bold"))

        # 2. Bottom horizontal slider: WASH FX (0..100%) + individual "0" button
        wy0 = pad_y1 + 6
        wy1 = y1 - 8
        z_btn_w = 42
        bar_x1 = inner_x1 - z_btn_w - 6
        wfx = st["wash_fx"]
        self.fx_sliders["wash_fx"] = (inner_x0, wy0, bar_x1, wy1, False)
        c_act = BYP_MID if byp else LINK_COL

        cv.create_rectangle(inner_x0, wy0, bar_x1, wy1, fill=BUTTON_BG, outline=LINE_DIM, width=1.2)
        norm = _clamp(wfx / 100.0, 0.0, 1.0)
        vx = inner_x0 + norm * (bar_x1 - inner_x0)
        if norm > 0.005:
            cv.create_rectangle(inner_x0 + 1, wy0 + 2, vx, wy1 - 2,
                                fill=BYP_FILL if byp else c_act, outline="")
        cv.create_line(vx, wy0 + 2, vx, wy1 - 2, fill=LABEL_HI, width=2.4)
        cv.create_text((inner_x0 + bar_x1) * 0.5, (wy0 + wy1) * 0.5,
                       text=f"WASH FX  {wfx:.0f}%",
                       fill=LABEL_HI, font=("TkDefaultFont", 9, "bold"))

        zx0 = bar_x1 + 6
        zx1 = inner_x1
        self.hit_rects["z_wash_fx"] = (zx0, wy0, zx1, wy1)
        active_z = (wfx >= 0.75) and not byp
        cv.create_rectangle(zx0, wy0, zx1, wy1,
                            fill=c_act if active_z else BUTTON_BG,
                            outline=c_act, width=1.4)
        cv.create_text((zx0 + zx1) * 0.5, (wy0 + wy1) * 0.5, text="0",
                       fill="#101318" if active_z else LABEL_HI, font=("TkDefaultFont", 10, "bold"))

    def _apply_tap_drag(self, which, x, y, link_ratio=None):
        gx0, gy0, gx1, gy1 = self.tap_box
        ms = self._x_to_ms(x, gx0, gx1)
        synced = self._get("sync", 1.0) >= 0.5
        bpm = _clamp(float(self.monitors.get("host_bpm", 120.0)), 20.0, 400.0)
        if synced:
            _, ms = nearest_division(ms, bpm)
        sym = "time_l" if which == "L" else "time_r"
        other = "time_r" if which == "L" else "time_l"
        old_ms = max(MIN_MS, self._get(sym, 375.0))
        new_ms = round(_clamp(ms, MIN_MS, MAX_MS), 1)
        self._set(sym, new_ms)

        if self._get("time_link", 1.0) >= 0.5:
            ratio = link_ratio if link_ratio is not None else (self._get(other, 500.0) / old_ms)
            other_ms = _clamp(new_ms * ratio, MIN_MS, MAX_MS)
            if synced:
                _, other_ms = nearest_division(other_ms, bpm)
            self._set(other, round(_clamp(other_ms, MIN_MS, MAX_MS), 1))

        mid_y = (gy0 + gy1) * 0.5
        half_h = max(10.0, (gy1 - gy0) * 0.46)
        dist = (mid_y - y) if which == "L" else (y - mid_y)
        fb = _clamp((dist / half_h - 0.18) / 0.72, 0.0, 1.0) * 110.0
        self._set("feedback", round(fb, 1))

    def _apply_tone_drag(self, which, x):
        tx0, _ty0, tx1, _ty1 = self.tone_box
        f = self._x_to_freq(x, tx0 + 8, tx1 - 8)
        if which == "hpf":
            self._set("hpf_hz", round(_clamp(f, 20.0, 1000.0), 1))
        else:
            self._set("lpf_hz", round(_clamp(f, 500.0, 20000.0), 1))

    def _apply_xy_drag(self, x, y):
        px0, py0, px1, py1 = self.xy_box
        tx = _clamp((x - px0) / max(1.0, px1 - px0), 0.0, 1.0)
        ty = _clamp((y - py0) / max(1.0, py1 - py0), 0.0, 1.0)
        tfx = (tx * 2.0 - 1.0) * 100.0
        ffx = (1.0 - ty * 2.0) * 100.0
        if abs(tfx) < 4.0:
            tfx = 0.0
        if abs(ffx) < 4.0:
            ffx = 0.0
        self._set("time_fx", round(_clamp(tfx, -100.0, 100.0), 1))
        self._set("filter_fx", round(_clamp(ffx, -100.0, 100.0), 1))

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

        # 1. Buttons (bottom bar + FX per-slider "0" reset buttons)
        for key, (x0, y0, x1, y1) in self.hit_rects.items():
            if x0 <= x <= x1 and y0 <= y <= y1:
                if key == "fx":
                    self.fx_view = not self.fx_view
                    if self.fx_view:
                        self._select_group("g_fx", "Effects")
                    else:
                        self._select_group("g_time", "Time")
                    self.refresh_gui()
                    return
                if key == "zero":
                    self._set("time_fx", 0.0)
                    self._set("filter_fx", 0.0)
                    self._set("wash_fx", 0.0)
                    self._select_group("g_fx", "Effects")
                    self.refresh_gui()
                    return
                if key.startswith("z_"):
                    self._set(key[2:], 0.0)
                    self._select_group("g_fx", "Effects")
                    self.refresh_gui()
                    return
                if key == "mode":
                    cur = int(round(self._get("mode", 1.0)))
                    self._set("mode", (cur + 1) % len(MODES))
                    self._select_group("g_mode", "Mode & Sync")
                    self.refresh_gui()
                    return
                if key == "route":
                    cur = int(round(self._get("routing", 0.0)))
                    self._set("routing", (cur + 1) % len(ROUTINGS))
                    self._select_group("g_mode", "Mode & Sync")
                    self.refresh_gui()
                    return
                if key == "sync":
                    cur = self._get("sync", 1.0)
                    self._set("sync", 0 if cur >= 0.5 else 1)
                    self._select_group("g_mode", "Mode & Sync")
                    self.refresh_gui()
                    return
                if key == "link":
                    cur = self._get("time_link", 1.0)
                    self._set("time_link", 0 if cur >= 0.5 else 1)
                    self._select_group("g_fx", "Effects")
                    self.refresh_gui()
                    return
                if key == "snap":
                    self.fx_snap = not self.fx_snap
                    self._select_group("g_fx", "Effects")
                    self.refresh_gui()
                    return
                if key == "freeze":
                    cur = self._get("freeze", 0.0)
                    self._set("freeze", 0 if cur >= 0.5 else 1)
                    self._select_group("g_perf", "Perform")
                    self.refresh_gui()
                    return
                if key == "trails":
                    self.drag_mode = ("trails", now)
                    self._select_group("g_perf", "Perform")
                    return

        # 2. Dedicated FX screen (X-Y pad + bottom Wash slider)
        if self.fx_view:
            px0, py0, px1, py1 = self.xy_box
            if px0 <= x <= px1 and py0 <= y <= py1:
                self.drag_mode = ("xy",)
                self._select_group("g_fx", "Effects")
                self._apply_xy_drag(x, y)
                self.refresh_gui()
                return
            for sym, (sx0, sy0, sx1, sy1, _bip) in self.fx_sliders.items():
                if sx0 <= x <= sx1 and sy0 <= y <= sy1:
                    self.drag_mode = ("fx", sym)
                    self._select_group("g_fx", "Effects")
                    self._apply_fx_drag(sym, x)
                    self.refresh_gui()
                    return
            return

        # 3. Tone filter bar
        tx0, ty0, tx1, ty1 = self.tone_box
        if tx0 <= x <= tx1 and ty0 <= y <= ty1:
            hx = self.handle_pos.get("hpf", (tx0, 0))[0]
            lx = self.handle_pos.get("lpf", (tx1, 0))[0]
            which = "hpf" if abs(x - hx) <= abs(x - lx) else "lpf"
            self.drag_mode = ("tone", which)
            self._select_group("g_tone", "Tone")
            self._apply_tone_drag(which, x)
            self.refresh_gui()
            return

        # 4. Tap Map
        gx0, gy0, gx1, gy1 = self.tap_box
        if gx0 <= x <= gx1 and gy0 <= y <= gy1:
            mid_y = (gy0 + gy1) * 0.5
            which = "L" if y <= mid_y else "R"
            tl = max(MIN_MS, self._get("time_l", 375.0))
            tr = max(MIN_MS, self._get("time_r", 500.0))
            ratio = (tr / tl) if which == "L" else (tl / tr)
            self.drag_mode = ("tap", which, ratio)
            self._select_group("g_time", "Time")
            self._apply_tap_drag(which, x, y, ratio)
            self.refresh_gui()

    def _on_motion(self, event):
        if not self.drag_mode:
            return
        kind = self.drag_mode[0]
        if kind == "tap":
            self._apply_tap_drag(self.drag_mode[1], event.x, event.y, self.drag_mode[2])
            self.refresh_gui()
        elif kind == "tone":
            self._apply_tone_drag(self.drag_mode[1], event.x)
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
            if kind == "trails":
                _, t0 = self.drag_mode
                if (time.monotonic() - t0) >= LONG_PRESS_SEC:
                    cur_byp = self._get("bypass", 0.0)
                    self._set("bypass", 0 if cur_byp >= 0.5 else 1)
                else:
                    cur_tr = self._get("trails", 0.0)
                    self._set("trails", 0 if cur_tr >= 0.5 else 1)
                self.refresh_gui()
            elif kind == "xy" and self.fx_snap:
                self._set("time_fx", 0.0)
                self._set("filter_fx", 0.0)
                self.refresh_gui()
            elif kind == "fx" and self.fx_snap:
                self._set(self.drag_mode[1], 0.0)
                self.refresh_gui()
        self.drag_mode = None
