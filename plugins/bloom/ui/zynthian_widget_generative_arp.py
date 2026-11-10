#!/usr/bin/python3
# -*- coding: utf-8 -*-
# Touchscreen editor of the Generative Arp pattern steps.
# Step values live in hidden plugin ports (prob1..16, shift1..16, vel1..16, gate1..16);
# the widget only writes them through regular Zynthian controllers, so snapshots keep them.
import logging
import time
import tkinter

from zyngui import zynthian_gui_config
from zyngui import zynthian_widget_base

MULTI_INSTANCE = True

NSTEPS = 16
GATE_VALUES = [0] + list(range(5, 101, 5)) + [101]      # Global, 5..100 %, Tie
# key, tab title, min, max, default; each row has its own length port len_<key> (polyrhythm)
LANES = [
    ("prob", "Prob", 0, 100, 100),
    ("shift", "Shift", -24, 24, 0),
    ("vel", "Vel", 0, 200, 100),
    ("gate", "Gate", 0, 101, 0),
]
LANE_COLORS = ["#4dabf7", "#ffa94d", "#69db7c", "#9775fa"]
KEYS = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
SCALES = ["Chromatic", "major", "minor", "dorian", "phrygian", "lydian", "mixolydian",
          "harm. minor", "mel. minor", "major pent.", "minor pent.", "blues"]
# semitones of each scale above the tonic (same table as arp_core.c)
SCALE_STEPS = [list(range(12)), [0, 2, 4, 5, 7, 9, 11], [0, 2, 3, 5, 7, 8, 10], [0, 2, 3, 5, 7, 9, 10],
               [0, 1, 3, 5, 7, 8, 10], [0, 2, 4, 6, 7, 9, 11], [0, 2, 4, 5, 7, 9, 10], [0, 2, 3, 5, 7, 8, 11],
               [0, 2, 3, 5, 7, 9, 11], [0, 2, 4, 7, 9], [0, 3, 5, 7, 10], [0, 3, 5, 6, 7, 10]]
MUT_MAX = (3, 3, 3, 4)      # drift limits per row (prob, shift, vel, gate), as in arp_core.h
DOUBLE_TAP = 0.35            # seconds: second tap on the same step resets it


def dim(col, f):
    r, g, b = int(col[1:3], 16), int(col[3:5], 16), int(col[5:7], 16)
    return "#%02x%02x%02x" % (int(r * f), int(g * f), int(b * f))


def gate_label(v):
    if v <= 0:
        return "G"
    if v >= 101:
        return "Tie"
    return "%d" % v


class zynthian_widget_generative_arp(zynthian_widget_base.zynthian_widget_base):

    def __init__(self, parent):
        super().__init__(parent)
        self.rows = max(2, self.rows - 1)
        self.lane = 0
        self.drag = None             # (lane, last column, last y) while a finger is on the grid
        self.last_tap = (-1, -1, 0.0)  # (lane, column, time) for double tap
        self.last_scale = None       # scale seen on the previous redraw (None until the first one)
        self.geo = None
        self.widget_canvas = tkinter.Canvas(
            self, highlightthickness=0, relief='flat',
            bg=zynthian_gui_config.color_bg)
        self.widget_canvas.grid(sticky='news')
        self.widget_canvas.bind("<ButtonPress-1>", self._on_press)
        self.widget_canvas.bind("<B1-Motion>", self._on_motion)
        self.widget_canvas.bind("<ButtonRelease-1>", self._on_release)

    def on_size(self, event):
        if not super().on_size(event):
            return
        self.widget_canvas.grid(row=0, column=0, sticky='news')
        self._draw()

    # ---------- controller access ----------
    def _zval(self, sym, default):
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
            logging.debug("Generative Arp widget: can't set %s => %s", sym, e)
            return False

    def _lane_len(self, lane):
        return min(max(int(round(self._zval("len_" + LANES[lane][0], NSTEPS))), 1), NSTEPS)

    def _step_val(self, lane, i):
        key, _, lo, hi, dflt = LANES[lane]
        return int(round(self._zval("%s%d" % (key, i + 1), dflt)))

    # ---------- geometry ----------
    def _layout(self):
        w, h = max(self.width, 80), max(self.height, 60)
        pad = 4
        tab_h = max(18, int(h * 0.17))
        num_h = max(14, int(h * 0.12))
        x0, x1 = pad, w - pad
        y0, y1 = tab_h + pad, h - num_h - 2
        cw = (x1 - x0) / float(NSTEPS)
        self.geo = (w, h, pad, tab_h, num_h, x0, x1, y0, y1, cw)
        return self.geo

    def _frac(self, lane, v):
        key, _, lo, hi, _ = LANES[lane]
        if key == "gate":
            v = min(GATE_VALUES, key=lambda g: abs(g - v))
            return GATE_VALUES.index(v) / float(len(GATE_VALUES) - 1)
        return (v - lo) / float(hi - lo)

    def _value_at(self, lane, y):
        w, h, pad, tab_h, num_h, x0, x1, y0, y1, cw = self.geo
        f = min(max((y1 - y) / float(max(1, y1 - y0)), 0.0), 1.0)
        key, _, lo, hi, _ = LANES[lane]
        if key == "gate":
            return GATE_VALUES[int(round(f * (len(GATE_VALUES) - 1)))]
        v = lo + f * (hi - lo)
        if key == "shift":
            return self._snap_shift(v)
        return int(round(v))

    def _scale(self):
        sc = int(round(self._zval("scale", 0)))
        return sc if 0 <= sc < len(SCALE_STEPS) else 0

    def _snap_shift(self, v):
        """With a scale, Shift values stick to the scale intervals above the tonic (0, 2, 3, 5 ... for minor)."""
        steps = SCALE_STEPS[self._scale()]
        allowed = [x for x in range(-24, 25) if x % 12 in steps]
        return min(allowed, key=lambda x: (abs(x - v), abs(x)))

    def _remap_shift(self, v, old, new):
        """Shift value after the scale changed from old to new: between 7-note scales the degree
        and octave are kept (minor +3 -> major +4), otherwise the nearest allowed semitone."""
        so, sn = SCALE_STEPS[old], SCALE_STEPS[new]
        if new == 0:
            return v
        if len(so) == 7 and len(sn) == 7 and v % 12 in so:
            r = (v // 12) * 12 + sn[so.index(v % 12)]
            if -24 <= r <= 24:
                return r
        return self._snap_shift(v)

    def _follow_scale(self):
        """When the scale changes, move every Shift step into it (the values are saved in the snapshot).
        Not on the first redraw: a loading snapshot already holds values that fit its scale."""
        sc = self._scale()
        if self.last_scale is not None and sc != self.last_scale:
            for i in range(NSTEPS):
                v = self._step_val(1, i)
                self._zset("shift%d" % (i + 1), self._remap_shift(v, self.last_scale, sc))
        self.last_scale = sc

    def _drift(self, lane, i):
        """Drift of row `lane` of step i in notches, from the dev<i> output port of the plugin."""
        try:
            packed = int(round(float((self.monitors or {}).get("dev%d" % (i + 1), 0))))
        except Exception:
            return 0
        if packed <= 0:
            return 0
        for r in range(lane):
            packed //= 2 * MUT_MAX[r] + 1
        return packed % (2 * MUT_MAX[lane] + 1) - MUT_MAX[lane]

    def _drifted(self, lane, v, d):
        """The value that plays: drawn value v moved by d notches (same rules as arp_drifted)."""
        if d == 0:
            return v
        key = LANES[lane][0]
        if key == "prob":
            return min(max(v + 20 * d, 0), 100)
        if key == "vel":
            return min(max(v + 20 * d, 0), 200)
        if key == "gate":
            if v >= 101:
                n = 20
            else:
                pct = self._zval("gate", 50) if v <= 0 else v
                n = min(max(int(pct + 2) // 5 - 1, 0), 19)
            n = min(max(n + d, 0), 20)
            return 101 if n == 20 else (n + 1) * 5
        # shift: d scale notes (semitones with Chromatic); the plugin moves the played note,
        # here it is shown from the tonic, which is exact for Chromatic and close otherwise
        steps = SCALE_STEPS[self._scale()]
        allowed = [x for x in range(-27, 28) if x % 12 in steps]
        k = min(range(len(allowed)), key=lambda j: abs(allowed[j] - v))
        return allowed[min(max(k + d, 0), len(allowed) - 1)]

    def _col_at(self, x):
        w, h, pad, tab_h, num_h, x0, x1, y0, y1, cw = self.geo
        return min(max(int((x - x0) / cw), 0), NSTEPS - 1)

    # ---------- touch ----------
    def _on_press(self, event):
        if not self.geo:
            self._layout()
        w, h, pad, tab_h, num_h, x0, x1, y0, y1, cw = self.geo
        if event.y < tab_h:                                  # lane tabs
            self.last_tap = (-1, -1, 0.0)
            tw = w / float(len(LANES))
            i = int(event.x / tw)
            if 0 <= i < len(LANES):
                self.lane = i
                self._draw()
            return
        col = self._col_at(event.x)
        if event.y > y1:                                     # step numbers: length of this row
            self.last_tap = (-1, -1, 0.0)
            self._zset("len_" + LANES[self.lane][0], col + 1)
            self._draw()
            return
        now = time.monotonic()
        if self.last_tap[:2] == (self.lane, col) and now - self.last_tap[2] < DOUBLE_TAP:
            key, _, lo, hi, dflt = LANES[self.lane]
            self._zset("%s%d" % (key, col + 1), dflt)
            self.last_tap = (-1, -1, 0.0)
            self.drag = None
            self._draw()
            return
        self.last_tap = (self.lane, col, now)
        self.drag = (self.lane, col, event.y)
        self._paint(col, event.y)

    def _on_motion(self, event):
        if not self.drag:
            return
        lane, last_col, last_y = self.drag
        col = self._col_at(event.x)
        if col != self.last_tap[1]:
            self.last_tap = (-1, -1, 0.0)                     # moved to another step: not a double tap
        # fill the columns crossed by a fast swipe, interpolating the height
        step = 1 if col >= last_col else -1
        n = abs(col - last_col)
        for k in range(1, n + 1):
            c = last_col + k * step
            y = last_y + (event.y - last_y) * k / float(n)
            self._paint(c, y, redraw=False)
        if n == 0:
            self._paint(col, event.y, redraw=False)
        self.drag = (lane, col, event.y)
        self._draw()

    def _on_release(self, event):
        self.drag = None

    def _paint(self, col, y, redraw=True):
        key = LANES[self.lane][0]
        self._zset("%s%d" % (key, col + 1), self._value_at(self.lane, y))
        if redraw:
            self._draw()

    # ---------- drawing ----------
    def refresh_gui(self):
        self._draw()

    def _draw(self):
        if not self.processor:
            return
        self._follow_scale()
        c = self.widget_canvas
        c.delete("dyn")
        w, h, pad, tab_h, num_h, x0, x1, y0, y1, cw = self._layout()
        fam = zynthian_gui_config.font_family
        tx = zynthian_gui_config.color_tx
        tx_off = zynthian_gui_config.color_tx_off
        fs_tab = max(7, int(tab_h * 0.4))
        fs_val = max(7, min(int(cw * 0.32), (y1 - y0) // 7))
        fs_num = max(7, num_h // 2 + 1)

        m = self.monitors or {}
        try:
            count = int(m.get("step_count", 0))
        except Exception:
            count = 0
        lane = self.lane
        length = self._lane_len(lane)
        cur = (count - 1) % length + 1 if count > 0 else 0     # step playing in this row
        key, title, lo, hi, dflt = LANES[lane]
        col = LANE_COLORS[lane]

        # tabs
        tw = w / float(len(LANES))
        for i, (_, t, _, _, _) in enumerate(LANES):
            sel = (i == lane)
            c.create_rectangle(int(i * tw) + 1, 1, int((i + 1) * tw) - 2, tab_h - 2, tags="dyn",
                               fill=(LANE_COLORS[i] if sel else ""), outline=LANE_COLORS[i])
            c.create_text(int((i + 0.5) * tw), tab_h // 2, text="%s %d" % (t, self._lane_len(i)), tags="dyn",
                          fill=("#000000" if sel else LANE_COLORS[i]), font=(fam, fs_tab))

        # reference lines
        def yv(v):
            return y1 - self._frac(lane, v) * (y1 - y0)
        if key == "shift":
            base = yv(0)
            for v in (-12, 12):
                c.create_line(x0, yv(v), x1, yv(v), fill="#303030", dash=(2, 4), tags="dyn")
            c.create_line(x0, base, x1, base, fill="#606060", tags="dyn")
        else:
            base = y1
        if key == "vel":
            c.create_line(x0, yv(100), x1, yv(100), fill="#606060", dash=(3, 3), tags="dyn")
        if key == "shift":                                   # Shift sticks to this scale (nothing for Chromatic)
            sc = self._scale()
            if sc > 0:
                k = int(round(self._zval("key", 0)))
                unit = "%s %s" % (KEYS[k] if 0 <= k < 12 else "Auto", SCALES[sc])   # 12 = Auto (lowest key)
                c.create_text(x1 - 2, y0 + 1, anchor="ne", text=unit, tags="dyn", fill=tx_off,
                              font=(fam, max(6, int(tab_h * 0.22))))

        # bars
        for i in range(NSTEPS):
            v = self._step_val(lane, i)
            active = i < length
            bx0, bx1 = x0 + i * cw + 1, x0 + (i + 1) * cw - 2
            if i + 1 == cur:
                c.create_rectangle(bx0 - 1, y0, bx1 + 1, y1, fill="#2a2a2a", outline="#ffffff", tags="dyn")
            fill = col if active else dim(col, 0.3)
            vy = yv(v)
            if key == "gate" and v <= 0:
                c.create_rectangle(bx0, y1 - 3, bx1, y1, fill=dim(fill, 0.6), width=0, tags="dyn")
            elif vy != base:
                c.create_rectangle(bx0, min(vy, base), bx1, max(vy, base), fill=fill, width=0, tags="dyn")
            if key == "gate":
                label = gate_label(v)
            elif key == "shift":
                label = "%+d" % v if v else "0"
            else:
                label = "%d" % v
            ty = (min(vy, base) - 2) if not (key == "shift" and v < 0) else (max(vy, base) + 2)
            anchor = "s" if not (key == "shift" and v < 0) else "n"
            ty = min(max(ty, y0 + fs_val + 2), y1 - 1) if anchor == "s" else min(ty, y1 - fs_val - 2)
            c.create_text((bx0 + bx1) / 2, ty, anchor=anchor, text=label, tags="dyn",
                          fill=(tx if active else tx_off), font=(fam, fs_val))
            d = self._drift(lane, i) if active else 0
            if d:                                            # mutation: where this step has drifted to
                my = min(max(yv(self._drifted(lane, v, d)), y0 + 1), y1 - 1)
                c.create_line(bx0 - 1, my, bx1 + 1, my, fill="#ffffff", width=3, tags=("dyn", "drift"))

        # step numbers (tap = Length)
        for i in range(NSTEPS):
            cx = x0 + (i + 0.5) * cw
            c.create_text(cx, h - num_h // 2 - 1, text=str(i + 1), tags="dyn",
                          fill=(tx if i < length else tx_off),
                          font=(fam, fs_num, "bold" if i + 1 == length else "normal"))
