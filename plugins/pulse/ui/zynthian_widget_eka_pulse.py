#!/usr/bin/python3
# -*- coding: utf-8 -*-
# Zynthian finds this widget by doap:name: "Eka Pulse" -> zynthian_widget_eka_pulse.
# Both the file name and class name must be zynthian_widget_eka_pulse.
import logging
import math
import random
import time
import tkinter
import tkinter.font

from zyngui import zynthian_gui_config
from zyngui import zynthian_widget_base

MULTI_INSTANCE = True

UPDATE_PERIOD = 0.08     # s (~12.5 Hz)
CURVE_PERIOD = 0.15      # s (curve overlay throttle)
LONG_PRESS = 0.6         # s (toggle slot ON/OFF)
SLOTS = 4
MAX_STEPS = 16
NCOLS = 64               # input level silhouette columns
CURVE_PTS = 96           # smooth curve samples across the loop

SLOT_INFO = [
    ("1 TIME",   "#4dabf7", ["Curve", "Chop", "Stut", "Stop"]),
    ("2 DRIVE",  "#ffa94d", ["Tube", "Fold", "Hard", "Crush", "Combo"]),
    ("3 FILTER", "#63e6be", ["LP24", "LP12", "BP", "HP", "Morph", "Notch", "Phase"]),
    ("4 GATE",   "#b197fc", ["Gate", "Pan", "Width", "GtPan"]),
]

# Dynamic encoder labels & per-slot help reference
SLOT_META = [
    {
        "mode_name": "Time Mode",
        "modes": ["Curve", "Chop", "Stutter", "TapeStop"],
        "p1": "Range",
        "p2": "Window",
        "help": [
            "Mode: Curve (TimeShaper) | Chop | Stutter | TapeStop",
            "Param 1 (Range): buffer depth 1/8..1 bar (TapeStop brake time)",
            "Param 2 (Window): jump de-click crossfade (2..10 ms)",
            "Chop: 0%=THRU, 1-25=1/8, 26-50=1/16, 51-75=1/2x, 76-90=REV, 91+=STOP",
        ],
    },
    {
        "mode_name": "Drive Mode",
        "modes": ["Tube", "Fold", "Hard", "Crush", "Combo"],
        "p1": "Drive %",
        "p2": "Tone/Crush",
        "help": [
            "Mode: Tube (warm) | Fold (ADAA) | Hard (fuzz) | Crush | Combo",
            "Param 1 (Drive): saturation / degradation intensity",
            "Param 2 (Tone/Crush): tone brightness (Tube/Fold/Hard) or bit/SR (Crush)",
            "Depth: step curve modulation of drive amount",
        ],
    },
    {
        "mode_name": "Filter Mode",
        "modes": ["LP 24", "LP 12", "BP", "HP", "Morph", "Notch", "Phaser"],
        "p1": "Cutoff",
        "p2": "Res / FB",
        "help": [
            "Mode: LP24 | LP12 | BP | HP | Morph (LP-BP-HP) | Notch | Phaser",
            "Param 1 (Cutoff): base cutoff frequency (40 Hz .. 18 kHz)",
            "Param 2 (Res / FB): SVF resonance or Phaser feedback",
            "Depth: cutoff sweep from step curve (+ up / - down, +-3.5 oct)",
        ],
    },
    {
        "mode_name": "Gate Mode",
        "modes": ["Gate", "Pan", "Width", "Gate+Pan"],
        "p1": "Floor %",
        "p2": "Width %",
        "help": [
            "Mode: Gate (chopper) | Pan (auto-pan) | Width (M/S) | Gate+Pan",
            "Param 1 (Floor): minimum level when gate is closed",
            "Param 2 (Width): stereo pan / M/S width",
            "Depth: + opens on curve, - ducks (sidechain); Smooth<0 = Punch",
        ],
    },
]

SHAPE_LABELS = ["_", "\\", "/", "/\\", "2x", "3x"]
CHOP_ACTIONS = [
    ("THRU", "#7a828e", 0.0),
    ("1/8",  "#4dabf7", 15.0),
    ("1/16", "#63e6be", 40.0),
    ("1/2x", "#ffa94d", 65.0),
    ("REV",  "#b197fc", 82.0),
    ("STOP", "#ff6b6b", 96.0),
]
TABS = ["Steps", "Perform", "Flow", "Help"]


def chop_action_idx(lvl_pct):
    if lvl_pct < 0.5:
        return 0
    if lvl_pct <= 25.0:
        return 1
    if lvl_pct <= 50.0:
        return 2
    if lvl_pct <= 75.0:
        return 3
    if lvl_pct <= 90.0:
        return 4
    return 5
SCENES = ["A", "B", "C", "D"]
PERF_PADS = [
    ("TapeStop", 1, "#ff6b6b"),
    ("1/16 Rpt", 2, "#4dabf7"),
    ("1/8 Rpt",  3, "#339af0"),
    ("Reverse",  4, "#b197fc"),
    ("Half 1/2", 5, "#9775fa"),
    ("GateChop", 6, "#63e6be"),
    ("FiltDrop", 7, "#20c997"),
    ("BitCrush", 8, "#ffa94d"),
    ("Bypass",   9, "#868e96"),
]

FACES = [
    "mode", "param1", "param2", "depth",
    "pattern", "step_rate", "steps", "warp",
    "trig", "threshold", "env_depth", "smooth",
]
SLOT_GROUPS = ("g_fx", "g_curve", "g_trigger")

DIM = "#484848"
BG_CARD = "#1c1f24"
BG_AREA = "#111317"
LEVEL_COLOR = "#25303d"
GRID_COLOR = "#262b33"
LIM_COLOR = "#ff6b6b"
HEAD_COLOR = "#ffffff"


def eval_shape(shape, u, warp):
    u = min(max(u, 0.0), 0.999999)
    if abs(warp) > 1e-4:
        exp_w = 2.0 ** (-min(max(warp, -1.0), 1.0) * 2.0)
        uw = u ** exp_w
    else:
        uw = u
    if shape == 0:
        return 1.0
    if shape == 1:
        return 1.0 - uw
    if shape == 2:
        return uw
    if shape == 3:
        return 1.0 - abs(2.0 * uw - 1.0)
    if shape == 4:
        u2 = (uw * 2.0) % 1.0
        return 1.0 - u2
    if shape == 5:
        u3 = (uw * 3.0) % 1.0
        return 1.0 - u3
    return 1.0


class zynthian_widget_eka_pulse(zynthian_widget_base.zynthian_widget_base):

    def __init__(self, parent):
        super().__init__(parent)
        self.next_update = 0.0
        self.next_curve = 0.0
        self.drawn = None
        self.view = 0                     # 0=Steps, 1=Perform, 2=Flow
        self.levels_hist = [0.0] * NCOLS  # rolling input peak history
        self.last_sample = 0.0

        self.slot_boxes = []              # [(x0, y0, x1, y1)] * 4
        self.tab_boxes = []               # [(x0, y0, x1, y1)] * 3
        self.step_boxes = []              # [(x0, y0, x1, y1, idx)]
        self.shape_boxes = []             # [(x0, y0, x1, y1, idx)]
        self.action_boxes = {}            # name -> (x0, y0, x1, y1)
        self.scene_boxes = []             # [(x0, y0, x1, y1)] * 4
        self.store_box = None
        self.store_armed = False
        self.pad_boxes = []               # [(x0, y0, x1, y1, pad_id)] * 9
        self.pad_rect_items = []
        self.xy_box = None
        self.xy_dot_item = None
        self.xy_hline = None
        self.xy_vline = None
        self.xy_txt_item = None
        self.active_pad = 0               # currently held pad on touchscreen (1..9)
        self.saved_mix = None             # mix value saved while holding Bypass pad
        self.dragging_xy = False
        self.last_group = None
        self.flow_xover_box = None
        self.flow_duck_box = None
        self.flow_meter_box = None

        self.hist_items = []
        self.bar_items = []
        self.bar_bg_items = []
        self.shape_rect_items = []
        self.shape_txt_items = []
        self.curve_item = None
        self.playhead_item = None
        self.other_head_items = []
        self.slot_bar_items = []
        self.top_bpm_item = None
        self.top_lim_item = None
        self.top_lim_bg = None
        self.flow_low_item = None
        self.flow_high_item = None
        self.flow_handle_item = None
        self.flow_split_txt = None
        self.flow_duck_rect = None
        self.flow_duck_txt = None
        self.flow_route_txt = None
        self.flow_meter_bar = None
        self.flow_thresh_line = None
        self.flow_thresh_txt = None
        self.flow_trig_bg = None
        self.flow_trig_txt = None

        self.slot_view = None
        self.top_view = None
        self.steps_view = None
        self.curve_sig = None
        self.bottom_view = None
        self.flow_sig = None

        self.press = None                 # (kind, arg, t0, x0, y0)
        self.dragging_steps = False
        self.dragging_flow = None         # "xover" | "thresh" | None

        self.sync_pending = True
        self.last_slot = None
        self.last_loads = None
        self.last_mon_scene = None
        self.meta_slot = None
        self.help_slot = None

        self.widget_canvas = tkinter.Canvas(
            self, highlightthickness=0, relief="flat", bg=zynthian_gui_config.color_bg
        )
        self.widget_canvas.grid(sticky="news")
        self.widget_canvas.bind("<ButtonPress-1>", self._on_press)
        self.widget_canvas.bind("<B1-Motion>", self._on_motion)
        self.widget_canvas.bind("<ButtonRelease-1>", self._on_release)

    def update(self):
        now = time.monotonic()
        if now < self.next_update:
            return
        self.next_update = now + UPDATE_PERIOD
        super().update()

    def on_size(self, event):
        if not super().on_size(event):
            return
        self.widget_canvas.grid(row=0, column=0, sticky="news")
        self.drawn = None
        self.refresh_gui()

    # ---------- plugin access ----------
    def _zval(self, sym, default=0.0):
        try:
            return float(self.processor.controllers_dict[sym].value)
        except Exception:
            return default

    def _zset(self, sym, value, force=False):
        try:
            z = self.processor.controllers_dict[sym]
            if force and z.value == value:
                z.set_value(0 if value != 0 else 1)
            if force or z.value != value:
                z.set_value(value)
            return True
        except Exception as e:
            logging.debug("Eka Pulse widget: can't set %s => %s", sym, e)
            return False

    def _mon(self, key, default=0.0):
        try:
            return float((self.monitors or {}).get(key, default))
        except Exception:
            return default

    def _slot(self):
        return min(max(int(round(self._zval("slot", 4))), 1), SLOTS)

    def _on_mask(self):
        return int(round(self._mon("on_mask", 8)))

    def _scene_mask(self):
        m = int(round(self._mon("scene_mask", 1)))
        return m if (m & 0x0F) else 1

    def _slot_on(self, s):
        return bool((self._on_mask() >> (s - 1)) & 1)

    def _slot_steps(self, s):
        v = self._mon("steps%d" % s, self._zval("s%d_steps" % s, 8))
        return min(max(int(round(v)), 1), MAX_STEPS)

    def _slot_mode(self, s):
        v = self._mon("mode%d" % s, self._zval("s%d_mode" % s, 0))
        return max(0, int(round(v)))

    # ---------- encoder page & knob sync ----------
    def _page_rows(self):
        try:
            rows = self.zyngui_control.list_data
        except Exception:
            return []
        return [(i, row[0]) for i, row in enumerate(rows) if len(row) >= 5 and row[0] and row[3] is self.processor]

    def _current_group(self):
        try:
            info = self.zyngui_control.screen_info
            if info and len(info) >= 5 and info[3] is self.processor:
                return info[0]
        except Exception:
            pass
        return None

    def _show_slot_page(self):
        if self._current_group() in SLOT_GROUPS:
            return
        for i, grp in self._page_rows():
            if grp == "g_fx":
                try:
                    self.zyngui_control.select(i)
                except Exception as e:
                    logging.debug("Eka Pulse widget: can't select FX page => %s", e)
                return

    def _update_slot_labels(self, slot):
        """Update Zynthian encoder names and Mode labels to match the selected slot."""
        if self.meta_slot == slot or not self.processor:
            return
        self.meta_slot = slot
        meta = SLOT_META[slot - 1]
        ctrls = getattr(self.processor, "controllers_dict", None) or {}
        changed = []
        z_mode = ctrls.get("mode")
        if z_mode is not None and hasattr(z_mode, "set_options"):
            try:
                n = len(meta["modes"])
                z_mode.set_options({
                    "name": meta["mode_name"],
                    "short_name": meta["mode_name"],
                    "labels": list(meta["modes"]),
                    "ticks": list(range(n)),
                    "value_min": 0,
                    "value_max": n - 1,
                    "is_integer": True,
                })
                changed.append(z_mode)
            except Exception as e:
                logging.debug("Eka Pulse widget: set_options(mode) failed => %s", e)
        for sym, key in (("param1", "p1"), ("param2", "p2")):
            zc = ctrls.get(sym)
            if zc is not None and hasattr(zc, "set_options"):
                try:
                    zc.set_options({"name": meta[key], "short_name": meta[key]})
                    changed.append(zc)
                except Exception as e:
                    logging.debug("Eka Pulse widget: set_options(%s) failed => %s", sym, e)
        if changed:
            for zgui in getattr(self.zyngui_control, "zgui_controllers", []) or []:
                if getattr(zgui, "zctrl", None) in changed:
                    try:
                        if hasattr(zgui, "config"):
                            zgui.config(zgui.zctrl)
                        if hasattr(zgui, "calculate_plot_values"):
                            zgui.calculate_plot_values()
                        if hasattr(zgui, "plot_value"):
                            zgui.plot_value()
                    except Exception:
                        pass

    def _sync_faces(self):
        slot = self._slot()
        self._update_slot_labels(slot)
        # Sync scene controller when plugin changes cur_scene (pedal / Store / restore),
        # or snap knob back if turned onto an empty scene. Never clobber a valid user selection.
        if self.monitors and "cur_scene" in self.monitors:
            cs = min(max(int(round(self._mon("cur_scene", 0))), 0), 3)
            smask = self._scene_mask()
            knob_sc = min(max(int(round(self._zval("scene", 0))), 0), 3)
            if self.last_mon_scene is None or cs != self.last_mon_scene:
                self.last_mon_scene = cs
                if knob_sc != cs:
                    self._zset("scene", cs)
                self.bottom_view = None
            elif not (smask & (1 << knob_sc)):
                # Knob was turned to an empty scene: snap back to active scene
                self._zset("scene", cs)
                self.bottom_view = None
        # Auto-open Perform tab when user switches encoders to Perform page
        grp = self._current_group()
        if grp != self.last_group:
            self.last_group = grp
            if grp == "g_perform" and self.view != 1:
                self.view = 1
                self.drawn = None
        loads = self._mon("loads", 0)
        if slot != self.last_slot or loads != self.last_loads:
            self.last_slot, self.last_loads = slot, loads
            self.sync_pending = True
        if not self.sync_pending or not self.monitors:
            return
        if int(round(self._mon("sel", 0))) != slot:
            return
        for sym in FACES:
            key = "cur_" + sym
            if key in self.monitors:
                self._zset(sym, self._mon(key))
        # Sync selected slot's 16 step levels & shapes from monitors (for scene/preset/pattern changes)
        if "cur_lvl1" in self.monitors:
            for k in range(MAX_STEPS):
                lk = "cur_lvl%d" % (k + 1)
                sk = "cur_shp%d" % (k + 1)
                if lk in self.monitors:
                    self._zset("s%d_lvl%d" % (slot, k + 1), self._mon(lk))
                if sk in self.monitors:
                    self._zset("s%d_shp%d" % (slot, k + 1), int(round(self._mon(sk))))
            self.steps_view = None
            self.curve_sig = None
        self.sync_pending = False

    # ---------- layout ----------
    def _font_size(self):
        return max(8, min(max(self.height, 20) // 22, 11))

    def _fit_font(self, text, max_w, max_h, max_pt=12, min_pt=7, bold=False):
        """Return a Tk font tuple guaranteed to fit inside (max_w, max_h)."""
        ch_w = 0.68 if bold else 0.60
        pt_w = int(max(max_w - 6, 8) / max(len(text), 1) / ch_w)
        pt_h = int(max(max_h - 4, 8) * 0.60)
        pt = max(min_pt, min(max_pt, pt_w, pt_h))
        return (zynthian_gui_config.font_family, pt, "bold" if bold else "normal")

    def _top_h(self):
        return max(28, min(36, int(self.height * 0.095)))

    def _bot_h(self):
        return max(38, min(48, int(self.height * 0.135)))

    def _left_w(self):
        return max(90, min(int(self.width * 0.18), 126))

    def _main_area(self):
        x0 = self._left_w() + 4
        y0 = self._top_h() + 3
        x1 = max(self.width, 160) - 4
        y1 = max(self.height, 120) - self._bot_h() - 3
        return x0, y0, x1, y1

    def _steps_rect(self):
        x0, y0, x1, y1 = self._main_area()
        shape_h = max(20, min(28, int((y1 - y0) * 0.105)))
        return x0, y0, x1, y1 - shape_h - 3, y1 - shape_h, y1

    def _flow_rects(self):
        x0, y0, x1, y1 = self._main_area()
        w = max(x1 - x0, 120)
        h = max(y1 - y0, 80)
        split_x = x0 + int(w * 0.68)
        ctrl_h = max(26, int(h * 0.20))
        xover = (x0, y0, split_x - 4, y1 - ctrl_h - 4)
        duck = (x0, y1 - ctrl_h, x0 + min(130, int((split_x - x0) * 0.48)), y1)
        route = (duck[2] + 6, y1 - ctrl_h, split_x - 4, y1)
        meter = (split_x + 2, y0, x1, y1)
        return xover, duck, route, meter

    def _perf_rects(self):
        x0, y0, x1, y1 = self._main_area()
        w = max(x1 - x0, 120)
        split_x = x0 + int(w * 0.67)
        return (x0, y0, split_x - 4, y1), (split_x + 2, y0, x1, y1)

    # ---------- drawing ----------
    def _reset_canvas(self):
        c = self.widget_canvas
        c.delete("all")
        self.slot_view = self.top_view = self.steps_view = self.bottom_view = None
        self.curve_sig = None
        self.flow_sig = None
        self.pad_boxes = []
        self.pad_rect_items = []
        self.xy_box = None
        self.xy_dot_item = self.xy_hline = self.xy_vline = self.xy_txt_item = None
        self.hist_items = []
        self.bar_items = []
        self.bar_bg_items = []
        self.shape_rect_items = []
        self.shape_txt_items = []
        self.curve_item = None
        self.playhead_item = None
        self.other_head_items = []
        self.slot_bar_items = []
        self.top_bpm_item = self.top_lim_item = self.top_lim_bg = None
        self.flow_low_item = self.flow_high_item = self.flow_handle_item = None
        self.flow_split_txt = self.flow_duck_rect = self.flow_duck_txt = self.flow_route_txt = None
        self.flow_meter_bar = self.flow_thresh_line = self.flow_thresh_txt = None
        self.flow_trig_bg = self.flow_trig_txt = None

    def refresh_gui(self):
        if not self.processor:
            return
        self._sync_faces()
        self._sample_peak()

        size = (self.width, self.height, self.view)
        if self.drawn != size:
            self.drawn = size
            self._reset_canvas()
            self._build_static_view()

        self._draw_slot_column()
        self._draw_top_bar()
        self._draw_bottom_bar()
        if self.view == 0:
            self._draw_steps_view()
        elif self.view == 1:
            self._draw_perform_view()
        elif self.view == 2:
            self._draw_flow_view()
        elif self.view == 3:
            self._draw_help_view()

    def _build_static_view(self):
        c = self.widget_canvas
        if self.view == 0:
            x0, y0, x1, y1, _sy0, _sy1 = self._steps_rect()
            c.create_rectangle(x0, y0, x1, y1, fill=BG_AREA, outline=DIM, tags="steps_bg")
            for frac in (0.25, 0.5, 0.75):
                gy = y1 - (y1 - y0) * frac
                c.create_line(x0 + 1, gy, x1 - 1, gy, fill=GRID_COLOR, tags="steps_bg")
            for _ in range(NCOLS):
                self.hist_items.append(c.create_line(0, 0, 0, 0, fill=LEVEL_COLOR, width=2, tags="hist"))
            for _ in range(MAX_STEPS):
                self.bar_bg_items.append(c.create_rectangle(0, 0, 0, 0, fill="", outline="#2c323b", state="hidden", tags="bars"))
                self.bar_items.append(c.create_rectangle(0, 0, 0, 0, fill=SLOT_INFO[3][1], outline="", state="hidden", tags="bars"))
                self.shape_rect_items.append(c.create_rectangle(0, 0, 0, 0, fill=BG_CARD, outline=DIM, state="hidden", tags="shapes"))
                self.shape_txt_items.append(c.create_text(0, 0, text="_", fill="#dddddd", state="hidden", tags="shapes"))
            self.curve_item = c.create_line(0, 0, 0, 0, fill="#ffffff", width=2, tags="curve")
            for s in range(SLOTS):
                self.other_head_items.append(
                    c.create_line(0, 0, 0, 0, fill=SLOT_INFO[s][1], width=2, state="hidden", tags="heads")
                )
            self.playhead_item = c.create_line(x0, y0, x0, y1, fill=HEAD_COLOR, width=2, tags="heads")
        elif self.view == 2:
            xover, duck, route, meter = self._flow_rects()
            self.flow_xover_box = xover
            self.flow_duck_box = duck
            self.flow_meter_box = meter
            fs = self._font_size()
            f_sm = (zynthian_gui_config.font_family, max(7, fs - 2))
            f_md = (zynthian_gui_config.font_family, fs, "bold")

            # Low Split Crossover box
            xx0, xy0, xx1, xy1 = xover
            c.create_rectangle(xx0, xy0, xx1, xy1, fill=BG_AREA, outline=DIM, tags="flow_bg")
            for frac, lbl in ((0.06, "Off"), (0.20, "60"), (0.45, "120"), (0.72, "250"), (0.94, "500Hz")):
                gx = xx0 + (xx1 - xx0) * frac
                c.create_line(gx, xy0 + 18, gx, xy1 - 14, fill=GRID_COLOR, tags="flow_bg")
                c.create_text(gx, xy1 - 7, text=lbl, fill="#7a828e", font=f_sm, tags="flow_bg")
            self.flow_low_item = c.create_line(xx0, xy1 - 16, xx1, xy1 - 16, fill="#ffa94d", width=2, tags="flow")
            self.flow_high_item = c.create_line(xx0, xy0 + 22, xx1, xy0 + 22, fill="#63e6be", width=2, tags="flow")
            self.flow_handle_item = c.create_line(xx0, xy0 + 18, xx0, xy1 - 14, fill="#ffffff", width=2, tags="flow")
            self.flow_split_txt = c.create_text(xx0 + 8, xy0 + 9, anchor="w", text="Low Split: Off", fill="#ffffff", font=f_md, tags="flow")

            # Low Duck toggle + routing readout
            dx0, dy0, dx1, dy1 = duck
            f_duck = self._fit_font("Low Duck: OFF", dx1 - dx0, dy1 - dy0, max_pt=11, bold=True)
            self.flow_duck_rect = c.create_rectangle(dx0, dy0, dx1, dy1, fill=BG_CARD, outline=DIM, tags="flow")
            self.flow_duck_txt = c.create_text((dx0 + dx1) / 2, (dy0 + dy1) / 2, text="Low Duck: OFF", fill="#cfd6e0", font=f_duck, tags="flow")
            rx0, ry0, rx1, ry1 = route
            f_rt = self._fit_font("Low: Gate Duck | High", rx1 - rx0, ry1 - ry0, max_pt=10)
            c.create_rectangle(rx0, ry0, rx1, ry1, fill=BG_CARD, outline=DIM, tags="flow_bg")
            self.flow_route_txt = c.create_text((rx0 + rx1) / 2, (ry0 + ry1) / 2, text="Full Band", fill="#9aa4b2", font=f_rt, tags="flow")

            # Input Level + Audio Trig Threshold meter
            mx0, my0, mx1, my1 = meter
            c.create_rectangle(mx0, my0, mx1, my1, fill=BG_AREA, outline=DIM, tags="flow_bg")
            c.create_text(mx0 + 6, my0 + 10, anchor="w", text="IN", fill="#cfd6e0", font=f_sm, tags="flow_bg")
            self.flow_trig_bg = c.create_rectangle(mx1 - 42, my0 + 3, mx1 - 4, my0 + 19, fill=BG_CARD, outline=DIM, tags="flow")
            self.flow_trig_txt = c.create_text(mx1 - 23, my0 + 11, text="TRIG", fill="#68707c", font=f_sm, tags="flow")
            bar_x0 = mx0 + 14
            bar_x1 = mx1 - 14
            c.create_rectangle(bar_x0, my0 + 24, bar_x1, my1 - 16, fill="#181c22", outline=GRID_COLOR, tags="flow_bg")
            self.flow_meter_bar = c.create_rectangle(bar_x0 + 2, my1 - 16, bar_x1 - 2, my1 - 16, fill="#4dabf7", outline="", tags="flow")
            self.flow_thresh_line = c.create_line(mx0 + 6, my0 + 40, mx1 - 6, my0 + 40, fill="#ffd43b", width=2, tags="flow")
            self.flow_thresh_txt = c.create_text((mx0 + mx1) / 2, my1 - 8, text="Thr -24 dB", fill="#ffd43b", font=f_sm, tags="flow")
        elif self.view == 1:
            pads_r, xy_r = self._perf_rects()
            px0, py0, px1, py1 = pads_r
            pw = (px1 - px0 - 8) / 3.0
            ph = (py1 - py0 - 8) / 3.0
            f_pad = self._fit_font("TapeStop", pw, ph, max_pt=12, min_pt=7, bold=True)
            self.pad_boxes = []
            self.pad_rect_items = []
            for idx, (lbl, pid, col) in enumerate(PERF_PADS):
                r, col_i = divmod(idx, 3)
                bx0 = px0 + col_i * (pw + 4)
                by0 = py0 + r * (ph + 4)
                bx1 = bx0 + pw
                by1 = by0 + ph
                self.pad_boxes.append((bx0, by0, bx1, by1, pid))
                rid = c.create_rectangle(bx0, by0, bx1, by1, fill=BG_CARD, outline=col, width=2, tags="perf")
                c.create_text((bx0 + bx1) / 2, (by0 + by1) / 2, text=lbl, fill="#ffffff", font=f_pad, tags="perf")
                self.pad_rect_items.append((rid, pid, col))

            # XY Pad
            xx0, xy0, xx1, xy1 = xy_r
            self.xy_box = xy_r
            xy_w = xx1 - xx0
            c.create_rectangle(xx0, xy0, xx1, xy1, fill=BG_AREA, outline="#4dabf7", width=2, tags="perf")
            c.create_line((xx0 + xx1) / 2, xy0 + 2, (xx0 + xx1) / 2, xy1 - 2, fill=GRID_COLOR, tags="perf")
            c.create_line(xx0 + 2, (xy0 + xy1) / 2, xx1 - 2, (xy0 + xy1) / 2, fill=GRID_COLOR, tags="perf")
            f_xy_t = self._fit_font("XY PAD", xy_w, 18, max_pt=10, bold=True)
            f_xy_s = self._fit_font("X:Cut  Y:Depth", xy_w, 16, max_pt=9)
            c.create_text((xx0 + xx1) / 2, xy0 + 10, text="XY PAD", fill="#74c0fc", font=f_xy_t, tags="perf")
            c.create_text((xx0 + xx1) / 2, xy1 - 9, text="X:Cut  Y:Depth", fill="#7a828e", font=f_xy_s, tags="perf")
            self.xy_hline = c.create_line(xx0 + 2, (xy0 + xy1) / 2, xx1 - 2, (xy0 + xy1) / 2, fill="#4dabf7", dash=(2, 2), tags="perf")
            self.xy_vline = c.create_line((xx0 + xx1) / 2, xy0 + 2, (xx0 + xx1) / 2, xy1 - 2, fill="#4dabf7", dash=(2, 2), tags="perf")
            cx, cy = (xx0 + xx1) / 2, (xy0 + xy1) / 2
            self.xy_dot_item = c.create_oval(cx - 8, cy - 8, cx + 8, cy + 8, fill="#4dabf7", outline="#ffffff", width=2, tags="perf")
            self.xy_txt_item = c.create_text(xx0 + 6, xy0 + 22, anchor="nw", text="", fill="#cfd6e0", font=f_xy_s, tags="perf")
        elif self.view == 3:
            self.help_slot = None

    def _sample_peak(self):
        now = time.monotonic()
        if now - self.last_sample < UPDATE_PERIOD * 0.8:
            return
        self.last_sample = now
        pk = min(max(self._mon("peak", 0.0), 0.0), 1.0)
        self.levels_hist.pop(0)
        self.levels_hist.append(pk)

    def _draw_slot_column(self):
        c = self.widget_canvas
        sel = self._slot()
        mask = self._on_mask()
        steps_tuple = tuple(self._slot_steps(s + 1) for s in range(SLOTS))
        modes_tuple = tuple(self._slot_mode(s + 1) for s in range(SLOTS))
        sig = (sel, mask, steps_tuple, modes_tuple)

        lw = self._left_w()
        h = max(self.height, 120)
        pad = 3
        card_h = (h - pad * (SLOTS + 1)) / float(SLOTS)

        if self.slot_view != sig:
            self.slot_view = sig
            c.delete("slots")
            self.slot_boxes = []
            self.slot_bar_items = []
            card_w = lw - 2 * pad - 12
            f_title = self._fit_font("3 FILTER", card_w, card_h * 0.42, max_pt=11, min_pt=7, bold=True)
            f_sub = self._fit_font("Morph 16/16", card_w, card_h * 0.34, max_pt=9, min_pt=7)

            for s in range(SLOTS):
                y0 = pad + s * (card_h + pad)
                y1 = y0 + card_h
                x0, x1 = pad, lw - pad
                self.slot_boxes.append((x0, y0, x1, y1))

                title, col, mode_names = SLOT_INFO[s]
                is_sel = (s + 1 == sel)
                is_on = bool((mask >> s) & 1)
                fill = "#242a34" if is_sel else BG_CARD
                outline = col if is_sel else (col if is_on else DIM)
                width = 2 if is_sel else 1

                c.create_rectangle(x0, y0, x1, y1, fill=fill, outline=outline, width=width, tags="slots")
                # Left accent bar
                c.create_rectangle(x0 + 1, y0 + 1, x0 + 5, y1 - 1, fill=col if is_on else DIM, outline="", tags="slots")
                c.create_text(x0 + 8, y0 + 4, anchor="nw", text=title, fill=col if is_on else "#7a828e", font=f_title, tags="slots")

                m_idx = modes_tuple[s]
                m_lbl = mode_names[m_idx] if m_idx < len(mode_names) else str(m_idx)
                sub = "%s %d/16" % (m_lbl, steps_tuple[s])
                c.create_text(x0 + 8, y0 + card_h * 0.47, anchor="nw", text=sub, fill="#cfd6e0" if is_on else "#666c75", font=f_sub, tags="slots")

                # Mini live curve level bar at bottom of card
                by0 = y1 - 7
                by1 = y1 - 3
                c.create_rectangle(x0 + 9, by0, x1 - 5, by1, fill="#12151a", outline="", tags="slots")
                bar_id = c.create_rectangle(x0 + 9, by0, x0 + 9, by1, fill=col if is_on else DIM, outline="", tags="slots")
                self.slot_bar_items.append((bar_id, x0 + 9, by0, x1 - 5, by1))

        # Update live mini-bars every refresh
        for s, (bar_id, bx0, by0, bx1, by1) in enumerate(self.slot_bar_items):
            v = min(max(self._mon("val%d" % (s + 1), 0.0), 0.0), 1.0)
            c.coords(bar_id, bx0, by0, bx0 + (bx1 - bx0) * v, by1)

    def _draw_top_bar(self):
        c = self.widget_canvas
        x0 = self._left_w() + 4
        x1 = max(self.width, 160) - 4
        th = self._top_h()
        bpm = int(round(self._mon("bpm", 120)))
        lim_on = self._mon("lim_gr", 0.0) > 0.005

        if self.top_view != self.view:
            self.top_view = self.view
            c.delete("top")
            self.tab_boxes = []
            lim_w = 34
            bpm_w = 56
            tabs_avail = max(x1 - x0 - lim_w - bpm_w - 16, 140)
            tab_w = min(76, max(36, int((tabs_avail - 12) / len(TABS))))
            f_tab_b = self._fit_font("Perform", tab_w, th - 4, max_pt=10, min_pt=7, bold=True)
            f_tab_n = (f_tab_b[0], f_tab_b[1], "normal")
            cur_tx = x0
            for i, name in enumerate(TABS):
                tw = max(36, int(tab_w * 0.82)) if name == "Help" else tab_w
                tx0 = cur_tx
                tx1 = tx0 + tw
                cur_tx = tx1 + 4
                ty0, ty1 = 2, th - 2
                self.tab_boxes.append((tx0, ty0, tx1, ty1))
                active = (i == self.view)
                c.create_rectangle(
                    tx0, ty0, tx1, ty1,
                    fill="#2b3340" if active else BG_CARD,
                    outline="#ffffff" if active else DIM,
                    width=1, tags="top",
                )
                c.create_text(
                    (tx0 + tx1) / 2, (ty0 + ty1) / 2, text=name,
                    fill="#ffffff" if active else "#9aa4b2",
                    font=f_tab_b if active else f_tab_n,
                    tags="top",
                )
            # BPM + LIM badge on right
            lx1 = x1
            lx0 = lx1 - lim_w
            f_lim = self._fit_font("LIM", lim_w, th - 6, max_pt=9, min_pt=7, bold=True)
            f_bpm = self._fit_font("120 BPM", bpm_w, th - 4, max_pt=10, min_pt=7)
            self.top_lim_bg = c.create_rectangle(lx0, 3, lx1, th - 3, fill=BG_CARD, outline=DIM, tags="top")
            self.top_lim_item = c.create_text((lx0 + lx1) / 2, th / 2, text="LIM", fill="#68707c", font=f_lim, tags="top")
            self.top_bpm_item = c.create_text(lx0 - 5, th / 2, anchor="e", text="%d BPM" % bpm, fill="#cfd6e0", font=f_bpm, tags="top")

        if self.top_bpm_item:
            c.itemconfig(self.top_bpm_item, text="%d BPM" % bpm)
        if self.top_lim_bg and self.top_lim_item:
            c.itemconfig(self.top_lim_bg, fill=LIM_COLOR if lim_on else BG_CARD, outline=LIM_COLOR if lim_on else DIM)
            c.itemconfig(self.top_lim_item, fill="#000000" if lim_on else "#68707c")

    def _draw_bottom_bar(self):
        c = self.widget_canvas
        scene = min(max(int(round(self._mon("cur_scene", self._zval("scene", 0)))), 0), 3)
        smask = self._scene_mask()
        sig = (self.view, scene, smask, self.store_armed)
        if self.bottom_view == sig:
            return
        self.bottom_view = sig
        c.delete("bottom")
        self.action_boxes = {}
        self.scene_boxes = []
        self.store_box = None

        x0 = self._left_w() + 4
        x1 = max(self.width, 160) - 4
        y1 = max(self.height, 120) - 2
        y0 = y1 - self._bot_h() + 4
        avail = max(x1 - x0, 200)
        bh = max(y1 - y0, 24)

        # Left side: Steps editing actions (Rnd, <, >, Inv) — sized for easy finger tap
        cur_x = x0
        if self.view == 0:
            scale = min(1.0, avail / 470.0)
            for key, label, bw in (("rnd", "Rnd", 48), ("shl", "<", 34), ("shr", ">", 34), ("inv", "Inv", 44)):
                w_btn = max(26, int(bw * scale))
                bx0, bx1 = cur_x, cur_x + w_btn
                self.action_boxes[key] = (bx0, y0, bx1, y1)
                f_act = self._fit_font(label, w_btn, bh, max_pt=12, min_pt=7, bold=True)
                c.create_rectangle(bx0, y0, bx1, y1, fill=BG_CARD, outline=DIM, width=1, tags="bottom")
                c.create_text((bx0 + bx1) / 2, (y0 + y1) / 2, text=label, fill="#e0e6ef", font=f_act, tags="bottom")
                cur_x = bx1 + 4

        # Right side: Scene strip A | B | C | D + Store (large touch targets)
        rem = max(x1 - cur_x - 6, 180)
        if self.view == 0:
            sw = max(34, min(48, int((rem - 62) / 4.4)))
            store_w = max(48, min(66, rem - 4 * sw - 18))
        else:
            sw = max(44, min(68, int((rem - 84) / 4.4)))
            store_w = max(58, min(84, rem - 4 * sw - 20))
        gap = 4
        total_sw = len(SCENES) * sw + (len(SCENES) - 1) * gap + gap + store_w
        sx = max(cur_x + 4, x1 - total_sw)
        f_sc = self._fit_font("A", sw, bh, max_pt=13, min_pt=8, bold=True)
        f_st = self._fit_font("Store", store_w, bh, max_pt=12, min_pt=7, bold=True)
        for i, name in enumerate(SCENES):
            bx0 = sx + i * (sw + gap)
            bx1 = bx0 + sw
            self.scene_boxes.append((bx0, y0, bx1, y1))
            saved = bool((smask >> i) & 1)
            active = (i == scene) and saved
            if self.store_armed:
                fill, outline, fg, bw = "#5c3d1e", "#ffa94d", "#ffffff", 2
            elif active:
                fill, outline, fg, bw = "#4dabf7", "#ffffff", "#000000", 2
            elif saved:
                fill, outline, fg, bw = "#2a394f", "#74c0fc", "#e6ecf5", 2
            else:
                fill, outline, fg, bw = "#16181c", "#2f343c", "#505660", 1
            c.create_rectangle(bx0, y0, bx1, y1, fill=fill, outline=outline, width=bw, tags="bottom")
            c.create_text((bx0 + bx1) / 2, (y0 + y1) / 2, text=name, fill=fg, font=f_sc, tags="bottom")

        stx0 = sx + len(SCENES) * (sw + gap)
        stx1 = stx0 + store_w
        self.store_box = (stx0, y0, stx1, y1)
        c.create_rectangle(
            stx0, y0, stx1, y1,
            fill="#ffa94d" if self.store_armed else BG_CARD,
            outline="#ffa94d" if self.store_armed else "#7a828e",
            width=2 if self.store_armed else 1,
            tags="bottom",
        )
        c.create_text(
            (stx0 + stx1) / 2, (y0 + y1) / 2, text="Store",
            fill="#000000" if self.store_armed else "#e0e6ef", font=f_st, tags="bottom",
        )

    def _draw_steps_view(self):
        c = self.widget_canvas
        sel = self._slot()
        nst = self._slot_steps(sel)
        col = SLOT_INFO[sel - 1][1]
        x0, y0, x1, y1, sy0, sy1 = self._steps_rect()
        w = max(x1 - x0, 16)
        h = max(y1 - y0, 16)

        # 1. Input level silhouette
        step_x = w / float(NCOLS)
        for i, pk in enumerate(self.levels_hist):
            lx = x0 + (i + 0.5) * step_x
            lh = pk * (h - 4)
            c.coords(self.hist_items[i], lx, y1 - 1, lx, y1 - 1 - lh)

        # 2. Step bars + shape/chop row (prefer live monitors when plugin matches selected slot)
        mon_ok = bool(self.monitors) and int(round(self._mon("sel", 0))) == sel and ("cur_lvl1" in self.monitors) and not self.dragging_steps
        if mon_ok:
            lvls = [min(max(self._mon("cur_lvl%d" % (k + 1), 100.0), 0.0), 100.0) for k in range(nst)]
            shps = [min(max(int(round(self._mon("cur_shp%d" % (k + 1), 1))), 0), 5) for k in range(nst)]
        else:
            lvls = [min(max(self._zval("s%d_lvl%d" % (sel, k + 1), 100.0), 0.0), 100.0) for k in range(nst)]
            shps = [min(max(int(round(self._zval("s%d_shp%d" % (sel, k + 1), 1))), 0), 5) for k in range(nst)]
        warp = min(max(self._zval("warp", 0.0), -100.0), 100.0) * 0.01
        smooth = min(max(self._zval("smooth", 10.0), -100.0), 100.0) * 0.01
        is_chop = (sel == 1 and self._slot_mode(1) == 1)

        sig = (sel, nst, is_chop, tuple(round(v, 1) for v in lvls), tuple(shps))
        if self.steps_view != sig:
            self.steps_view = sig
            self.step_boxes = []
            self.shape_boxes = []
            sw = w / float(nst)
            pad = 2 if sw > 14 else 1
            sh_h = max(sy1 - sy0, 16)
            f_shp = self._fit_font("1/16" if is_chop else "2x", sw - 2 * pad, sh_h, max_pt=10, min_pt=6, bold=True)
            for k in range(MAX_STEPS):
                if k < nst:
                    bx0 = x0 + k * sw + pad
                    bx1 = x0 + (k + 1) * sw - pad
                    bh = (lvls[k] / 100.0) * (h - 6)
                    self.step_boxes.append((x0 + k * sw, y0, x0 + (k + 1) * sw, y1, k))
                    self.shape_boxes.append((bx0, sy0, bx1, sy1, k))
                    c.coords(self.bar_bg_items[k], bx0, y0 + 2, bx1, y1 - 2)
                    c.itemconfig(self.bar_bg_items[k], state="normal")
                    bar_col = CHOP_ACTIONS[chop_action_idx(lvls[k])][1] if is_chop else col
                    c.coords(self.bar_items[k], bx0 + 1, y1 - 2 - bh, bx1 - 1, y1 - 2)
                    c.itemconfig(self.bar_items[k], fill=bar_col, state="normal")
                    c.coords(self.shape_rect_items[k], bx0, sy0, bx1, sy1)
                    if is_chop:
                        act = chop_action_idx(lvls[k])
                        lbl, acol, _ = CHOP_ACTIONS[act]
                        c.itemconfig(self.shape_rect_items[k], outline=acol, state="normal")
                        c.coords(self.shape_txt_items[k], (bx0 + bx1) / 2, (sy0 + sy1) / 2)
                        c.itemconfig(self.shape_txt_items[k], text=lbl, fill=acol, font=f_shp, state="normal")
                    else:
                        c.itemconfig(self.shape_rect_items[k], outline=DIM, state="normal")
                        c.coords(self.shape_txt_items[k], (bx0 + bx1) / 2, (sy0 + sy1) / 2)
                        c.itemconfig(self.shape_txt_items[k], text=SHAPE_LABELS[shps[k]], fill="#dddddd", font=f_shp, state="normal")
                else:
                    c.itemconfig(self.bar_bg_items[k], state="hidden")
                    c.itemconfig(self.bar_items[k], state="hidden")
                    c.itemconfig(self.shape_rect_items[k], state="hidden")
                    c.itemconfig(self.shape_txt_items[k], state="hidden")

        # 3. Smooth curve overlay (throttled & cached)
        csig = (sig, round(warp, 2), round(smooth, 2))
        now = time.monotonic()
        if self.curve_sig != csig and now >= self.next_curve:
            self.curve_sig = csig
            self.next_curve = now + CURVE_PERIOD
            pts = []
            alpha = 1.0 if smooth <= 0.0 else max(0.08, 1.0 - smooth * 0.85)
            sm_val = (lvls[0] * 0.01) * eval_shape(shps[0], 0.0, warp)
            for i in range(CURVE_PTS + 1):
                t = i / float(CURVE_PTS)
                pos = t * nst
                idx = min(int(pos), nst - 1)
                frac = pos - idx
                raw = (lvls[idx] * 0.01) * eval_shape(shps[idx], frac, warp)
                sm_val += alpha * (raw - sm_val)
                px = x0 + t * w
                py = y1 - 2 - sm_val * (h - 6)
                pts.extend((px, py))
            c.coords(self.curve_item, *pts)

        # 4. Playhead of selected slot + thin markers of other active slots
        mask = self._on_mask()
        ph_sel = min(max(self._mon("phase%d" % sel, 0.0), 0.0), 1.0)
        px_sel = x0 + ph_sel * w
        c.coords(self.playhead_item, px_sel, y0 + 1, px_sel, y1 - 1)

        for s in range(SLOTS):
            if s + 1 != sel and ((mask >> s) & 1):
                ph = min(max(self._mon("phase%d" % (s + 1), 0.0), 0.0), 1.0)
                px = x0 + ph * w
                c.coords(self.other_head_items[s], px, y0 + 1, px, y0 + 14)
                c.itemconfig(self.other_head_items[s], state="normal")
            else:
                c.itemconfig(self.other_head_items[s], state="hidden")

    def _draw_flow_view(self):
        c = self.widget_canvas
        if not self.flow_xover_box or not self.flow_meter_box:
            return
        xx0, xy0, xx1, xy1 = self.flow_xover_box
        mx0, my0, mx1, my1 = self.flow_meter_box

        split_hz = min(max(self._zval("low_split", 0.0), 0.0), 500.0)
        duck_on = self._zval("low_duck", 0.0) >= 0.5
        thr_db = min(max(self._zval("threshold", -24.0), -60.0), 0.0)
        sig = (round(split_hz, 1), duck_on, round(thr_db, 1))

        if self.flow_sig != sig:
            self.flow_sig = sig
            top_y = xy0 + 22
            bot_y = xy1 - 16
            span_y = max(bot_y - top_y, 12)
            w = max(xx1 - xx0, 40)

            if split_hz < 60.0:
                hx = xx0 + w * 0.06
                c.coords(self.flow_low_item, xx0 + 2, bot_y, xx1 - 2, bot_y)
                c.coords(self.flow_high_item, xx0 + 2, top_y, xx1 - 2, top_y)
                c.coords(self.flow_handle_item, hx, xy0 + 18, hx, xy1 - 14)
                c.itemconfig(self.flow_split_txt, text="Low Split: Off")
                c.itemconfig(self.flow_route_txt, text="Full Band -> Slots")
            else:
                # Map 60..500 Hz logarithmically onto x fraction 0.20..0.94
                t_fc = math.log(split_hz / 60.0) / math.log(500.0 / 60.0)
                hx = xx0 + w * (0.20 + 0.74 * t_fc)
                low_pts, high_pts = [], []
                npts = 48
                for i in range(npts + 1):
                    u = i / float(npts)
                    px = xx0 + 2 + u * (w - 4)
                    # Frequency at u (log scale 30..800 Hz)
                    f = 30.0 * ((800.0 / 30.0) ** u)
                    r = f / split_hz
                    # LR4 magnitude: LP = 1 / (1 + r^4), HP = r^4 / (1 + r^4)
                    r4 = r ** 4
                    m_lp = 1.0 / (1.0 + r4)
                    m_hp = r4 / (1.0 + r4)
                    low_pts.extend((px, bot_y - m_lp * span_y))
                    high_pts.extend((px, bot_y - m_hp * span_y))
                c.coords(self.flow_low_item, *low_pts)
                c.coords(self.flow_high_item, *high_pts)
                c.coords(self.flow_handle_item, hx, xy0 + 18, hx, xy1 - 14)
                c.itemconfig(self.flow_split_txt, text="Low Split: %d Hz" % int(round(split_hz)))
                c.itemconfig(self.flow_route_txt, text="Low: %s | High: FX" % ("Gate Duck" if duck_on else "Clean"))

            c.itemconfig(self.flow_duck_rect, fill="#3a4a62" if duck_on else BG_CARD, outline="#ffa94d" if duck_on else DIM)
            c.itemconfig(self.flow_duck_txt, text="Low Duck: %s" % ("ON" if duck_on else "OFF"),
                         fill="#ffffff" if duck_on else "#9aa4b2")

            # Threshold line on meter (-60..0 dB)
            mt_y0, mt_y1 = my0 + 24, my1 - 16
            t_norm = (thr_db + 60.0) / 60.0
            ty = mt_y1 - t_norm * max(mt_y1 - mt_y0, 10)
            c.coords(self.flow_thresh_line, mx0 + 6, ty, mx1 - 6, ty)
            c.itemconfig(self.flow_thresh_txt, text="Thr %d dB" % int(round(thr_db)))

        # Live input level meter + TRIG LED
        pk = max(self._mon("peak", 0.0), 1e-6)
        pk_db = max(-60.0, min(0.0, 20.0 * math.log10(pk)))
        mt_y0, mt_y1 = my0 + 24, my1 - 16
        p_norm = (pk_db + 60.0) / 60.0
        py = mt_y1 - p_norm * max(mt_y1 - mt_y0, 10)
        c.coords(self.flow_meter_bar, mx0 + 16, py, mx1 - 16, mt_y1)
        trig_hot = pk_db >= thr_db and pk > 1e-4
        c.itemconfig(self.flow_trig_bg, fill="#ffd43b" if trig_hot else BG_CARD, outline="#ffd43b" if trig_hot else DIM)
        c.itemconfig(self.flow_trig_txt, fill="#000000" if trig_hot else "#68707c")

    def _apply_flow_drag(self, x, y):
        if self.dragging_flow == "xover" and self.flow_xover_box:
            xx0, _xy0, xx1, _xy1 = self.flow_xover_box
            frac = min(max((x - xx0) / max(xx1 - xx0, 1.0), 0.0), 1.0)
            if frac < 0.14:
                hz = 0.0
            else:
                t = min(max((frac - 0.20) / 0.74, 0.0), 1.0)
                hz = round(60.0 * ((500.0 / 60.0) ** t))
            self._zset("low_split", float(hz))
            self.flow_sig = None
            self._draw_flow_view()
        elif self.dragging_flow == "thresh" and self.flow_meter_box:
            _mx0, my0, _mx1, my1 = self.flow_meter_box
            mt_y0, mt_y1 = my0 + 24, my1 - 16
            t = min(max((mt_y1 - y) / max(mt_y1 - mt_y0, 1.0), 0.0), 1.0)
            db = round(-60.0 + t * 60.0)
            self._zset("threshold", float(db))
            self.flow_sig = None
            self._draw_flow_view()

    def _draw_help_view(self):
        sel = self._slot()
        if self.help_slot == sel:
            return
        self.help_slot = sel
        c = self.widget_canvas
        c.delete("help")
        x0, y0, x1, y1 = self._main_area()
        c.create_rectangle(x0, y0, x1, y1, fill=BG_AREA, outline=DIM, tags="help")

        title, col, _ = SLOT_INFO[sel - 1]
        meta = SLOT_META[sel - 1]
        fs = self._font_size()
        f_hdr = (zynthian_gui_config.font_family, fs, "bold")
        f_body = (zynthian_gui_config.font_family, max(8, fs - 1))
        f_dim = (zynthian_gui_config.font_family, max(7, fs - 2))

        lh = max(16, int((y1 - y0 - 16) / 9.5))
        wrap_w = max(x1 - x0 - 20, 140)
        y = y0 + 8
        c.create_text(
            x0 + 10, y, anchor="nw", width=wrap_w,
            text="%s — %s / %s / %s" % (title, meta["mode_name"], meta["p1"], meta["p2"]),
            fill=col, font=f_hdr, tags="help",
        )
        y += lh + 3
        for line in meta["help"]:
            c.create_text(x0 + 12, y, anchor="nw", width=wrap_w, text="• " + line, fill="#e6ecf5", font=f_body, tags="help")
            y += lh

        y += 4
        c.create_line(x0 + 8, y, x1 - 8, y, fill=GRID_COLOR, tags="help")
        y += 6
        tips = [
            "Slot cards (left): tap = select, re-tap / hold = ON/OFF",
            "Steps: swipe bars = level, tap row below = shape (_ \\ / /\\ 2x 3x)",
            "Trigger: Audio/MIDI = restart step 1 without transport; Env->Depth",
            "Perform: hold pads/XY = live FX; Store + A-D = save scene",
        ]
        for line in tips:
            if y + lh > y1 - 2:
                break
            c.create_text(x0 + 10, y, anchor="nw", width=wrap_w, text=line, fill="#9aa4b2", font=f_dim, tags="help")
            y += lh - 1

    def _draw_perform_view(self):
        c = self.widget_canvas
        if not self.xy_box:
            return
        cur_pad = self.active_pad if self.active_pad else int(round(self._zval("live_pad", 0)))
        for rid, pid, col in self.pad_rect_items:
            lit = (cur_pad == pid)
            c.itemconfig(rid, fill=col if lit else BG_CARD, outline="#ffffff" if lit else col)

        xx0, xy0, xx1, xy1 = self.xy_box
        xv = min(max(self._zval("xy_x", 50.0), 0.0), 100.0) * 0.01
        yv = min(max(self._zval("xy_y", 50.0), 0.0), 100.0) * 0.01
        xy_on = self.dragging_xy or (self._zval("xy_on", 0.0) >= 0.5)
        cx = xx0 + 8 + xv * max(xx1 - xx0 - 16, 10)
        cy = xy1 - 18 - yv * max(xy1 - xy0 - 38, 10)
        c.coords(self.xy_hline, xx0 + 2, cy, xx1 - 2, cy)
        c.coords(self.xy_vline, cx, xy0 + 18, cx, xy1 - 16)
        r = 10 if xy_on else 7
        c.coords(self.xy_dot_item, cx - r, cy - r, cx + r, cy + r)
        c.itemconfig(self.xy_dot_item, fill="#ffd43b" if xy_on else "#4dabf7")
        if self.xy_txt_item:
            hz = int(round(80.0 * (200.0 ** xv)))
            c.itemconfig(self.xy_txt_item, text="%d Hz  %d%%" % (hz, int(round(yv * 100))) if xy_on else "")

    def _apply_xy_drag(self, x, y):
        if not self.xy_box:
            return
        xx0, xy0, xx1, xy1 = self.xy_box
        xv = min(max((x - (xx0 + 8)) / max(xx1 - xx0 - 16, 1.0), 0.0), 1.0)
        yv = min(max(((xy1 - 18) - y) / max(xy1 - xy0 - 38, 1.0), 0.0), 1.0)
        self._zset("xy_x", round(xv * 100.0, 1))
        self._zset("xy_y", round(yv * 100.0, 1))
        self._zset("xy_on", 1)
        self._draw_perform_view()

    # ---------- touch / mouse interaction ----------
    def _hit(self, boxes, x, y):
        for i, b in enumerate(boxes):
            if b[0] <= x <= b[2] and b[1] <= y <= b[3]:
                return i
        return -1

    def _apply_step_touch(self, x, y):
        if self.view != 0:
            return
        x0, y0, x1, y1, _sy0, _sy1 = self._steps_rect()
        if not (x0 <= x <= x1):
            return
        sel = self._slot()
        nst = self._slot_steps(sel)
        w = max(x1 - x0, 1.0)
        h = max(y1 - y0, 1.0)
        k = int((x - x0) / w * nst)
        k = min(max(k, 0), nst - 1)
        lvl = round(min(max((y1 - y) / h, 0.0), 1.0) * 100.0)
        self._zset("s%d_lvl%d" % (sel, k + 1), float(lvl))
        self.steps_view = None
        self.next_curve = 0.0
        self._draw_steps_view()

    def _run_action(self, key):
        sel = self._slot()
        nst = self._slot_steps(sel)
        lvls = [self._zval("s%d_lvl%d" % (sel, k + 1), 100.0) for k in range(nst)]
        shps = [int(round(self._zval("s%d_shp%d" % (sel, k + 1), 1))) for k in range(nst)]
        if key == "rnd":
            for k in range(nst):
                lvls[k] = float(random.choice([0, 0, 40, 70, 100, 100]))
                shps[k] = random.randint(0, 4)
        elif key == "shl" and nst > 1:
            lvls = lvls[1:] + lvls[:1]
            shps = shps[1:] + shps[:1]
        elif key == "shr" and nst > 1:
            lvls = lvls[-1:] + lvls[:-1]
            shps = shps[-1:] + shps[:-1]
        elif key == "inv":
            lvls = [round(100.0 - v) for v in lvls]
        for k in range(nst):
            self._zset("s%d_lvl%d" % (sel, k + 1), float(lvls[k]))
            self._zset("s%d_shp%d" % (sel, k + 1), int(shps[k]))
        self.steps_view = None
        self.next_curve = 0.0
        self._draw_steps_view()

    def _on_press(self, event):
        x, y = event.x, event.y
        now = time.monotonic()
        self.press = None
        self.dragging_steps = False
        self.dragging_flow = None
        self.dragging_xy = False

        idx = self._hit(self.slot_boxes, x, y)
        if idx >= 0:
            self.press = ("slot", idx + 1, now, x, y)
            return

        idx = self._hit(self.tab_boxes, x, y)
        if idx >= 0:
            self.press = ("tab", idx, now, x, y)
            return

        if self.store_box:
            b = self.store_box
            if b[0] <= x <= b[2] and b[1] <= y <= b[3]:
                self.press = ("store", 0, now, x, y)
                return

        idx = self._hit(self.scene_boxes, x, y)
        if idx >= 0:
            self.press = ("scene", idx, now, x, y)
            return

        for key, b in self.action_boxes.items():
            if b[0] <= x <= b[2] and b[1] <= y <= b[3]:
                self.press = ("action", key, now, x, y)
                return

        if self.view == 0:
            idx = self._hit(self.shape_boxes, x, y)
            if idx >= 0:
                self.press = ("shape", idx, now, x, y)
                return
            x0, y0, x1, y1, _sy0, _sy1 = self._steps_rect()
            if x0 <= x <= x1 and y0 <= y <= y1:
                self.dragging_steps = True
                self._apply_step_touch(x, y)
                return
        elif self.view == 1:
            idx = self._hit(self.pad_boxes, x, y)
            if idx >= 0:
                pid = self.pad_boxes[idx][4]
                self.active_pad = pid
                if pid == 9:
                    self.saved_mix = self._zval("mix", 100.0)
                    self._zset("mix", 0.0)
                else:
                    self._zset("live_pad", pid)
                self._draw_perform_view()
                return
            if self.xy_box:
                b = self.xy_box
                if b[0] <= x <= b[2] and b[1] <= y <= b[3]:
                    self.dragging_xy = True
                    self._apply_xy_drag(x, y)
                    return
        elif self.view == 2:
            if self.flow_duck_box:
                b = self.flow_duck_box
                if b[0] <= x <= b[2] and b[1] <= y <= b[3]:
                    self.press = ("duck", 0, now, x, y)
                    return
            if self.flow_xover_box:
                b = self.flow_xover_box
                if b[0] <= x <= b[2] and b[1] <= y <= b[3]:
                    self.dragging_flow = "xover"
                    self._apply_flow_drag(x, y)
                    return
            if self.flow_meter_box:
                b = self.flow_meter_box
                if b[0] <= x <= b[2] and b[1] <= y <= b[3]:
                    self.dragging_flow = "thresh"
                    self._apply_flow_drag(x, y)
                    return

    def _on_motion(self, event):
        if self.dragging_steps:
            self._apply_step_touch(event.x, event.y)
        elif self.dragging_flow:
            self._apply_flow_drag(event.x, event.y)
        elif self.dragging_xy:
            self._apply_xy_drag(event.x, event.y)

    def _on_release(self, event):
        if self.dragging_steps:
            self.dragging_steps = False
            return
        if self.dragging_flow:
            self.dragging_flow = None
            return
        if self.dragging_xy:
            self.dragging_xy = False
            self._zset("xy_on", 0)
            self._draw_perform_view()
            return
        if self.active_pad:
            pid = self.active_pad
            self.active_pad = 0
            if pid == 9:
                if self.saved_mix is not None:
                    self._zset("mix", self.saved_mix)
                    self.saved_mix = None
            else:
                self._zset("live_pad", 0)
            self._draw_perform_view()
            return
        if not self.press:
            return
        kind, arg, t0, _x0, _y0 = self.press
        self.press = None
        dt = time.monotonic() - t0

        if kind == "slot":
            s = arg
            cur = self._slot()
            if dt >= LONG_PRESS or s == cur:
                # Long press or re-tap on selected slot toggles ON/OFF
                new_on = 0 if self._slot_on(s) else 1
                self._zset("s%d_on" % s, new_on)
            else:
                self._zset("slot", s)
                self._show_slot_page()
            self.slot_view = None
            self.steps_view = None
            self.refresh_gui()
        elif kind == "tab":
            if arg != self.view:
                self.view = arg
                self.drawn = None
                self.refresh_gui()
        elif kind == "store":
            self.store_armed = not self.store_armed
            self.bottom_view = None
            self.refresh_gui()
        elif kind == "scene":
            if self.store_armed:
                self.store_armed = False
                # Send store_scene ONLY; plugin copies current state to target scene and updates cur_scene/scene_mask
                self._zset("store_scene", arg + 1, force=True)
            else:
                # Only switch to saved scenes; ignore taps on empty (gray) scenes
                if not (self._scene_mask() & (1 << arg)):
                    return
                self._zset("scene", arg)
            self.bottom_view = None
            self.steps_view = None
            self.refresh_gui()
        elif kind == "action":
            self._run_action(arg)
        elif kind == "duck":
            new_v = 0 if self._zval("low_duck", 0.0) >= 0.5 else 1
            self._zset("low_duck", new_v)
            self.flow_sig = None
            self._draw_flow_view()
        elif kind == "shape":
            sel = self._slot()
            if sel == 1 and self._slot_mode(1) == 1:
                lsym = "s1_lvl%d" % (arg + 1)
                act = chop_action_idx(self._zval(lsym, 0.0))
                nxt = (act + 1) % len(CHOP_ACTIONS)
                self._zset(lsym, CHOP_ACTIONS[nxt][2])
            else:
                sym = "s%d_shp%d" % (sel, arg + 1)
                cur = int(round(self._zval(sym, 1)))
                self._zset(sym, (cur + 1) % 6)
            self.steps_view = None
            self.next_curve = 0.0
            self._draw_steps_view()
