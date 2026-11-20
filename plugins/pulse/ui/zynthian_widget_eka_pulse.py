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
SHAPE_LABELS = ["_", "\\", "/", "/\\", "2x", "3x"]
TABS = ["Steps", "Perform", "Flow"]
SCENES = ["A", "B", "C", "D"]

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

        self.slot_view = None
        self.top_view = None
        self.steps_view = None
        self.curve_sig = None
        self.bottom_view = None

        self.press = None                 # (kind, arg, t0, x0, y0)
        self.dragging_steps = False

        self.sync_pending = True
        self.last_slot = None
        self.last_loads = None

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

    def _zset(self, sym, value):
        try:
            z = self.processor.controllers_dict[sym]
            if z.value != value:
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

    def _sync_faces(self):
        slot = self._slot()
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
        self.sync_pending = False

    # ---------- layout ----------
    def _font_size(self):
        return max(8, min(max(self.height, 20) // 18, 12))

    def _top_h(self):
        return max(26, self._font_size() * 2 + 8)

    def _bot_h(self):
        return max(26, self._font_size() * 2 + 8)

    def _left_w(self):
        return max(84, min(int(self.width * 0.17), 130))

    def _main_area(self):
        x0 = self._left_w() + 4
        y0 = self._top_h() + 3
        x1 = max(self.width, 160) - 4
        y1 = max(self.height, 120) - self._bot_h() - 3
        return x0, y0, x1, y1

    def _steps_rect(self):
        x0, y0, x1, y1 = self._main_area()
        shape_h = max(20, int((y1 - y0) * 0.17))
        return x0, y0, x1, y1 - shape_h - 3, y1 - shape_h, y1

    # ---------- drawing ----------
    def _reset_canvas(self):
        c = self.widget_canvas
        c.delete("all")
        self.slot_view = self.top_view = self.steps_view = self.bottom_view = None
        self.curve_sig = None
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
        else:
            x0, y0, x1, y1 = self._main_area()
            c.create_rectangle(x0, y0, x1, y1, fill=BG_AREA, outline=DIM, tags="placeholder")
            msg = "Perform (Pads + XY + Scenes) — stage M3" if self.view == 1 else "Flow (Low Split + Audio Trig) — stage M1"
            c.create_text(
                (x0 + x1) / 2, (y0 + y1) / 2, text=msg,
                fill="#9aa4b2", font=(zynthian_gui_config.font_family, self._font_size() + 1),
                tags="placeholder",
            )

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
            fs = self._font_size()
            f_title = (zynthian_gui_config.font_family, fs, "bold")
            f_sub = (zynthian_gui_config.font_family, max(7, fs - 2))

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
                c.create_text(x0 + 9, y0 + 4, anchor="nw", text=title, fill=col if is_on else "#7a828e", font=f_title, tags="slots")

                m_idx = modes_tuple[s]
                m_lbl = mode_names[m_idx] if m_idx < len(mode_names) else str(m_idx)
                sub = "%s  %d/16" % (m_lbl, steps_tuple[s])
                c.create_text(x0 + 9, y0 + card_h * 0.48, anchor="nw", text=sub, fill="#cfd6e0" if is_on else "#666c75", font=f_sub, tags="slots")

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
            fs = self._font_size()
            tab_w = min(96, max(54, int((x1 - x0 - 120) / len(TABS))))
            for i, name in enumerate(TABS):
                tx0 = x0 + i * (tab_w + 4)
                tx1 = tx0 + tab_w
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
                    font=(zynthian_gui_config.font_family, fs, "bold" if active else "normal"),
                    tags="top",
                )
            # BPM + LIM badge on right
            lx1 = x1
            lx0 = lx1 - 42
            self.top_lim_bg = c.create_rectangle(lx0, 3, lx1, th - 3, fill=BG_CARD, outline=DIM, tags="top")
            self.top_lim_item = c.create_text(
                (lx0 + lx1) / 2, th / 2, text="LIM", fill="#68707c",
                font=(zynthian_gui_config.font_family, max(8, fs - 1), "bold"), tags="top",
            )
            self.top_bpm_item = c.create_text(
                lx0 - 8, th / 2, anchor="e", text="%d BPM" % bpm, fill="#cfd6e0",
                font=(zynthian_gui_config.font_family, fs), tags="top",
            )

        if self.top_bpm_item:
            c.itemconfig(self.top_bpm_item, text="%d BPM" % bpm)
        if self.top_lim_bg and self.top_lim_item:
            c.itemconfig(self.top_lim_bg, fill=LIM_COLOR if lim_on else BG_CARD, outline=LIM_COLOR if lim_on else DIM)
            c.itemconfig(self.top_lim_item, fill="#000000" if lim_on else "#68707c")

    def _draw_bottom_bar(self):
        c = self.widget_canvas
        scene = min(max(int(round(self._zval("scene", 0))), 0), 3)
        sig = (self.view, scene)
        if self.bottom_view == sig:
            return
        self.bottom_view = sig
        c.delete("bottom")
        self.action_boxes = {}
        self.scene_boxes = []

        x0 = self._left_w() + 4
        x1 = max(self.width, 160) - 4
        y1 = max(self.height, 120) - 2
        y0 = y1 - self._bot_h() + 4
        fs = self._font_size()
        f_btn = (zynthian_gui_config.font_family, max(8, fs - 1), "bold")

        # Left side: Steps editing actions (Rnd, <, >, Inv)
        cur_x = x0
        if self.view == 0:
            for key, label, bw in (("rnd", "Rnd", 44), ("shl", "<", 30), ("shr", ">", 30), ("inv", "Inv", 40)):
                bx0, bx1 = cur_x, cur_x + bw
                self.action_boxes[key] = (bx0, y0, bx1, y1)
                c.create_rectangle(bx0, y0, bx1, y1, fill=BG_CARD, outline=DIM, tags="bottom")
                c.create_text((bx0 + bx1) / 2, (y0 + y1) / 2, text=label, fill="#e0e6ef", font=f_btn, tags="bottom")
                cur_x = bx1 + 4

        # Right side: Scene strip A | B | C | D
        sw = 34
        total_sw = len(SCENES) * sw + (len(SCENES) - 1) * 3
        sx = max(cur_x + 8, x1 - total_sw)
        for i, name in enumerate(SCENES):
            bx0, bx1 = sx + i * (sw + 3), sx + i * (sw + 3) + sw
            self.scene_boxes.append((bx0, y0, bx1, y1))
            active = (i == scene)
            c.create_rectangle(
                bx0, y0, bx1, y1,
                fill="#3a4a62" if active else BG_CARD,
                outline="#ffffff" if active else DIM,
                tags="bottom",
            )
            c.create_text((bx0 + bx1) / 2, (y0 + y1) / 2, text=name, fill="#ffffff" if active else "#9aa4b2", font=f_btn, tags="bottom")

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

        # 2. Step bars + shape row
        lvls = [min(max(self._zval("s%d_lvl%d" % (sel, k + 1), 100.0), 0.0), 100.0) for k in range(nst)]
        shps = [min(max(int(round(self._zval("s%d_shp%d" % (sel, k + 1), 1))), 0), 5) for k in range(nst)]
        warp = min(max(self._zval("warp", 0.0), -100.0), 100.0) * 0.01
        smooth = min(max(self._zval("smooth", 10.0), -100.0), 100.0) * 0.01

        sig = (sel, nst, tuple(round(v, 1) for v in lvls), tuple(shps))
        if self.steps_view != sig:
            self.steps_view = sig
            self.step_boxes = []
            self.shape_boxes = []
            sw = w / float(nst)
            pad = 2 if sw > 12 else 1
            fs = max(7, min(self._font_size(), int(sw * 0.45)))
            f_shp = (zynthian_gui_config.font_family, fs, "bold")
            for k in range(MAX_STEPS):
                if k < nst:
                    bx0 = x0 + k * sw + pad
                    bx1 = x0 + (k + 1) * sw - pad
                    bh = (lvls[k] / 100.0) * (h - 6)
                    self.step_boxes.append((x0 + k * sw, y0, x0 + (k + 1) * sw, y1, k))
                    self.shape_boxes.append((bx0, sy0, bx1, sy1, k))
                    c.coords(self.bar_bg_items[k], bx0, y0 + 2, bx1, y1 - 2)
                    c.itemconfig(self.bar_bg_items[k], state="normal")
                    c.coords(self.bar_items[k], bx0 + 1, y1 - 2 - bh, bx1 - 1, y1 - 2)
                    c.itemconfig(self.bar_items[k], fill=col, state="normal")
                    c.coords(self.shape_rect_items[k], bx0, sy0, bx1, sy1)
                    c.itemconfig(self.shape_rect_items[k], state="normal")
                    c.coords(self.shape_txt_items[k], (bx0 + bx1) / 2, (sy0 + sy1) / 2)
                    c.itemconfig(self.shape_txt_items[k], text=SHAPE_LABELS[shps[k]], font=f_shp, state="normal")
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

        idx = self._hit(self.slot_boxes, x, y)
        if idx >= 0:
            self.press = ("slot", idx + 1, now, x, y)
            return

        idx = self._hit(self.tab_boxes, x, y)
        if idx >= 0:
            self.press = ("tab", idx, now, x, y)
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

    def _on_motion(self, event):
        if self.dragging_steps:
            self._apply_step_touch(event.x, event.y)

    def _on_release(self, event):
        if self.dragging_steps:
            self.dragging_steps = False
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
        elif kind == "scene":
            self._zset("scene", arg)
            self.bottom_view = None
            self.refresh_gui()
        elif kind == "action":
            self._run_action(arg)
        elif kind == "shape":
            sel = self._slot()
            sym = "s%d_shp%d" % (sel, arg + 1)
            cur = int(round(self._zval(sym, 1)))
            self._zset(sym, (cur + 1) % 6)
            self.steps_view = None
            self.next_curve = 0.0
            self._draw_steps_view()
