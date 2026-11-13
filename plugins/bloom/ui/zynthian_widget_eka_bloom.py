#!/usr/bin/python3
# -*- coding: utf-8 -*-
# Touchscreen editor of the Eka Bloom pattern steps.
# Step values live in plugin parameters (prob1..16, shift1..16, vel1..16, gate1..16, ratch1..16);
# the widget only writes them through regular Zynthian controllers, so snapshots keep them.
# The view follows Zynthian's knob page (as in Eka Fold): the Arp page shows the roll of the notes Bloom
# has played, read from shared memory /dev/shm/eka_bloom_<pid> (written by the plugin, see Viz in
# eka_bloom.c); the Play page shows pads and an XY pad for live effects (held = on, released = off);
# the Scale page a keyboard (tap = Key), the scales and the degrees (tap = the arpeggio moves to that chord
# of the key); the other pages show the step editor.
import ctypes
import glob
import logging
import math
import mmap
import os
import struct
import time
import tkinter
import tkinter.font

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
    ("ratch", "Ratch", 1, 4, 1),      # hits inside the step; drawn as that many slim bars
]
LANE_COLORS = ["#4dabf7", "#ffa94d", "#69db7c", "#9775fa", "#ff6b6b"]
KEYS = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
SCALES = ["Chromatic", "major", "minor", "dorian", "phrygian", "lydian", "mixolydian",
          "harm. minor", "mel. minor", "major pent.", "minor pent.", "blues"]
# semitones of each scale above the tonic (same table as arp_core.c)
SCALE_STEPS = [list(range(12)), [0, 2, 4, 5, 7, 9, 11], [0, 2, 3, 5, 7, 8, 10], [0, 2, 3, 5, 7, 9, 10],
               [0, 1, 3, 5, 7, 8, 10], [0, 2, 4, 6, 7, 9, 11], [0, 2, 4, 5, 7, 9, 10], [0, 2, 3, 5, 7, 8, 11],
               [0, 2, 3, 5, 7, 9, 11], [0, 2, 4, 7, 9], [0, 3, 5, 7, 10], [0, 3, 5, 6, 7, 10]]
MUT_MAX = (3, 3, 3, 4)      # drift limits per row (prob, shift, vel, gate), as in arp_core.h; Ratch doesn't drift
DOUBLE_TAP = 0.35            # seconds: second tap on the same step resets it
RATCH_TOP = 0.8              # Ratch bars use 80 % of the height: the "every N" label sits above them

# ---------- views by knob page ----------
PAGE_VIEWS = {"g_arp": "roll", "g_play": "play", "g_scale": "scale"}   # port group -> view; others: editor

# ---------- Play view ----------
# pads: (fx value, title, subtitle); held = the effect, released = Off (values as in arp_core.h ARP_FX_*)
PLAY_PADS = [(1, "Roll", "1/8"), (2, "Roll", "1/16"), (3, "Roll", "1/32"), (4, "Fill", "x3"),
             (5, "Oct", "+1"), (6, "Half", "time"), (7, "Drop", "mute"), (8, "Rev", "order")]
PAD_COLS = 4
ROLL_PAD_COLOR = "#f783ac"
FX_PAD_COLOR = "#38d9a9"
XY_COLOR = "#74c0fc"
LATCH_COLOR = "#ffd43b"

# ---------- Scale view ----------
SCALE_COLOR = "#b197fc"
SCALE_SHORT = ["Chromatic", "Major", "Minor", "Dorian", "Phrygian", "Lydian", "Mixolyd.", "Harm. m",
               "Mel. m", "Maj pent", "Min pent", "Blues"]
WHITE_PCS = [0, 2, 4, 5, 7, 9, 11]
BLACK_PCS = [(1, 1), (3, 2), (6, 4), (8, 5), (10, 6)]     # (pitch class, white key it sits before)
ROMAN = ["I", "II", "III", "IV", "V", "VI", "VII"]


def degree_chord(steps, i):
    """(label, chord suffix) of degree i of a 7-note scale: case and sign by its triad."""
    r = steps[i]
    third = (steps[(i + 2) % 7] - r) % 12
    fifth = (steps[(i + 4) % 7] - r) % 12
    if third == 4 and fifth == 7:
        return ROMAN[i], ""
    if third == 3 and fifth == 7:
        return ROMAN[i].lower(), "m"
    if third == 3 and fifth == 6:
        return ROMAN[i].lower() + "\u00b0", "\u00b0"
    if third == 4 and fifth == 8:
        return ROMAN[i] + "+", "+"
    return ROMAN[i], ""
ROLL_COLOR = "#ffd43b"
ROLL_BARS = (1, 2, 4)        # window lengths, a tap on the roll cycles them
PPQN = 960
VIZ_MAGIC = 0x4D4F4C42
VIZ_HDR = struct.Struct("<6Id32s")       # magic, seq, head, bpb, nheld, rolling, clk, held[32]
VIZ_NOTE = struct.Struct("<ddBBB5x")     # on, off (< 0: sounding), note, vel, ch
VIZ_NOTES = 512
VIZ_SIZE = VIZ_HDR.size + VIZ_NOTES * VIZ_NOTE.size
# Written by the widget for the engine (UiIn in eka_bloom.c, after the ring): magic, seq, zbeat, pad[5].
# zynseq stops / restarts scenes while JACK keeps rolling; the widget passes zynseq's beat to the plugin:
# 0 / -2 alternating = zynseq stopped (silent), 1..16 = the beat. (jalv drops values for notOnGUI ports.)
UIIN = struct.Struct("<IIi5I")
UIIN_OFF = VIZ_SIZE
UIIN_MAGIC = 0x55494E31
BLACK_KEYS = (1, 3, 6, 8, 10)
NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]



def dim(col, f):
    r, g, b = int(col[1:3], 16), int(col[3:5], 16), int(col[5:7], 16)
    return "#%02x%02x%02x" % (int(r * f), int(g * f), int(b * f))


def gate_label(v):
    if v <= 0:
        return "G"
    if v >= 101:
        return "Tie"
    return "%d" % v


class zynthian_widget_eka_bloom(zynthian_widget_base.zynthian_widget_base):

    def __init__(self, parent):
        super().__init__(parent)
        self.mmw = None
        self.mmw_path = None
        self.latch_box = (0, 0, 0, 0)
        self._was_rolling = None
        self.after(300, self._zs_tick)
        self.rows = max(2, self.rows - 1)
        self.lane = 0
        self.drag = None             # (lane, last column, last y) while a finger is on the grid
        self.last_tap = (-1, -1, 0.0)  # (lane, column, time) for double tap
        self.last_scale = None       # scale seen on the previous redraw (None until the first one)
        self.geo = None
        self.roll_bars = 2           # Roll window in bars
        self.mm = None               # shared memory of the plugin (Roll tab)
        self.mm_path = None
        self.font_cache = {}         # (family, size, text) -> width in pixels
        self.view = "steps"          # "steps" (editor), "roll" or "play", by the knob page
        self.touch = None            # Play: "pad" or "xy" while a finger is down
        self.scale_boxes = []        # Scale: (x0, y0, x1, y1, kind, value) of what a tap can hit
        self.last_group = None       # knob page (port group symbol) seen on the previous redraw
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
            logging.debug("Eka Bloom widget: can't set %s => %s", sym, e)
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
        if key == "ratch":                                   # 1 hit = a quarter of RATCH_TOP, 4 hits = RATCH_TOP
            return v / float(hi) * RATCH_TOP
        return (v - lo) / float(hi - lo)

    def _value_at(self, lane, y):
        w, h, pad, tab_h, num_h, x0, x1, y0, y1, cw = self.geo
        f = min(max((y1 - y) / float(max(1, y1 - y0)), 0.0), 1.0)
        key, _, lo, hi, _ = LANES[lane]
        if key == "gate":
            return GATE_VALUES[int(round(f * (len(GATE_VALUES) - 1)))]
        if key == "ratch":
            return min(max(int(f / RATCH_TOP * hi + 0.5), lo), hi)
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
        if lane >= len(MUT_MAX):
            return 0
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
        if self.view == "play":
            self._play_press(event.x, event.y)
            return
        if self.view == "scale":
            self._scale_press(event.x, event.y)
            return
        if self.view == "roll":                              # tap on the roll: 1 / 2 / 4 bars
            k = ROLL_BARS.index(self.roll_bars) if self.roll_bars in ROLL_BARS else 0
            self.roll_bars = ROLL_BARS[(k + 1) % len(ROLL_BARS)]
            self._draw()
            return
        if event.y < tab_h:                                  # row tabs
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
        if self.view == "play":
            self._play_move(event.x, event.y)
            return
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
        self._play_release()

    def _paint(self, col, y, redraw=True):
        key = LANES[self.lane][0]
        self._zset("%s%d" % (key, col + 1), self._value_at(self.lane, y))
        if redraw:
            self._draw()

    # ---------- drawing ----------
    def refresh_gui(self):
        self._draw()

    def _current_group(self):
        """Port group symbol of Zynthian's knob page now shown, if it is a page of this plugin."""
        try:
            info = self.zyngui_control.screen_info
            if info and len(info) >= 5 and info[3] is self.processor:
                return info[0]
        except Exception:
            pass
        return None

    def _follow_page(self):
        """The knob page changed (encoder, menu): show its view. Unknown pages keep the view."""
        group = self._current_group()
        if group == self.last_group:
            return
        self.last_group = group
        if group:
            if self.view == "play":
                self._play_release()                         # never leave an effect held
            self.view = PAGE_VIEWS.get(group, "steps")
            self.drag = None

    def _draw(self):
        if not self.processor:
            return
        self._follow_page()
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

        if self.view == "roll":
            self._draw_roll()
            return
        if self.view == "play":
            self._draw_play()
            return
        if self.view == "scale":
            self._draw_scale()
            return

        # tabs of the rows; one font size that fits the widest label
        tw = w / float(len(LANES))
        labels = ["%s %d" % (LANES[i][1], self._lane_len(i)) for i in range(len(LANES))]
        colors = LANE_COLORS
        fs_tab = min(self._fit_size(fam, t, tw - 8, fs_tab) for t in labels)
        for i in range(len(LANES)):
            sel = (i == lane)
            tcol = colors[i]
            text = labels[i]
            c.create_rectangle(int(i * tw) + 1, 1, int((i + 1) * tw) - 2, tab_h - 2, tags="dyn",
                               fill=(tcol if sel else ""), outline=tcol)
            c.create_text(int((i + 0.5) * tw), tab_h // 2, text=text, tags="dyn",
                          fill=("#000000" if sel else tcol), font=(fam, fs_tab))

        length = self._lane_len(lane)
        cur = (count - 1) % length + 1 if count > 0 else 0     # step playing in this row
        key, title, lo, hi, dflt = LANES[lane]
        col = LANE_COLORS[lane]

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
        rat_off = False                                      # Ratch every N: this pass plays one hit per step
        if key == "ratch":
            every = min(max(int(round(self._zval("ratch_every", 1))), 1), 8)
            if every > 1:
                k = ((count - 1) // length) % every + 1 if count > 0 else 0
                rat_off = count > 0 and k != every
                unit = "every %d \u00b7 pass %d/%d" % (every, k, every) if k else "every %d passes" % every
                c.create_text(x1 - 2, y0 + 1, anchor="ne", text=unit, tags=("dyn", "every"),
                              fill=(LANE_COLORS[lane] if not rat_off else tx_off),
                              font=(fam, max(6, int(tab_h * 0.26))))
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
            if rat_off and v > 1:                            # hits wait for the N-th pass
                fill = dim(col, 0.45)
            vy = yv(v)
            if key == "gate" and v <= 0:
                c.create_rectangle(bx0, y1 - 3, bx1, y1, fill=dim(fill, 0.6), width=0, tags="dyn")
            elif key == "ratch":                             # one slim bar per hit
                n = min(max(v, 1), hi)
                gap = 2 if (bx1 - bx0) > 4 * n else 1
                sw = (bx1 - bx0 - gap * (n - 1)) / float(n)
                for k in range(n):
                    sx = bx0 + k * (sw + gap)
                    c.create_rectangle(sx, vy, sx + sw, base, fill=fill, width=0, tags=("dyn", "hit"))
            elif vy != base:
                c.create_rectangle(bx0, min(vy, base), bx1, max(vy, base), fill=fill, width=0, tags="dyn")
            if key == "gate":
                label = gate_label(v)
            elif key == "ratch":
                label = "x%d" % v if v > 1 else "1"
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

    def _text_w(self, fam, size, text, weight="normal"):
        k = (fam, size, text, weight)
        if k not in self.font_cache:
            try:
                self.font_cache[k] = tkinter.font.Font(family=fam, size=size, weight=weight).measure(text)
            except Exception:
                self.font_cache[k] = int(len(text) * size * 0.75)
        return self.font_cache[k]

    def _fit_size(self, fam, text, maxw, start, weight="normal"):
        """Largest font size <= start (down to 6) at which text is at most maxw pixels wide."""
        size = start
        while size > 6 and self._text_w(fam, size, text, weight) > maxw:
            size -= 1
        return size

    # ---------- zynseq -> plugin (shared memory) ----------
    _zs_fn = None

    @classmethod
    def _zs_state(cls, lib):
        """zynseq's transport state (0 stopped, 1 playing, 2 starting, 3 stopping). In vangelis-2610
        getTransportState is exported without extern "C": try the C++ name, then the global."""
        if cls._zs_fn is None:
            for name in ("getTransportState", "_Z17getTransportStatev"):
                try:
                    f = lib[name]
                    f.restype = ctypes.c_uint8
                    f.argtypes = []
                    cls._zs_fn = f
                    break
                except Exception:
                    pass
            if cls._zs_fn is None:
                var = ctypes.c_uint8.in_dll(lib, "g_nTransportState")
                cls._zs_fn = lambda: var.value
        return int(cls._zs_fn())

    def _zs_beat(self):
        """zynseq beat for the plugin: 1..16 playing, 0 / -2 alternating stopped, None = unknown / starting."""
        try:
            lib = self.zyngui.state_manager.zynseq.libseq
            st = self._zs_state(lib)
            beat = int(lib.getBeat())
        except Exception:
            return None
        if st == 1 and 1 <= beat <= 16:
            return beat
        if st in (0, 3, 5):
            return 0 if int(time.monotonic() * 2) % 2 == 0 else -2
        return None

    def _open_mmw(self):
        if not self._open_mm():
            return None
        if self.mmw is not None and self.mmw_path == self.mm_path:
            return self.mmw
        try:
            if self.mmw is not None:
                self.mmw.close()
            with open(self.mm_path, "r+b") as f:
                mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_WRITE)
            if len(mm) < UIIN_OFF + UIIN.size:              # an older plugin without the block
                mm.close()
                return None
            self.mmw, self.mmw_path = mm, self.mm_path
            return mm
        except Exception:
            self.mmw = None
            return None

    def _latch_follow(self):
        """Transport stopped (the plugin went from playing to silent): Latch off."""
        if not self._open_mm():
            return
        rolling = struct.unpack_from("<I", self.mm, 20)[0] != 0     # Viz.rolling: playing on the transport
        if self._was_rolling and not rolling and self._zval("latch", 0) >= 0.5:
            self._zset("latch", 0)
        self._was_rolling = rolling

    def _zs_tick(self):
        try:
            if self.processor is not None:
                zb = self._zs_beat()
                mm = self._open_mmw() if zb is not None else None
                if mm is not None:
                    seq = UIIN.unpack_from(mm, UIIN_OFF)[1]
                    UIIN.pack_into(mm, UIIN_OFF, UIIN_MAGIC, (seq + 1) & 0xFFFFFFFF, zb, 0, 0, 0, 0, 0)
                self._latch_follow()
        except Exception as e:
            logging.debug("Eka Bloom zynseq sync: %s", e)
        self.after(33, self._zs_tick)

    # ---------- Roll tab: the notes Bloom has played ----------
    def _open_mm(self):
        path = None
        try:
            path = "/dev/shm/eka_bloom_%d" % int(self.processor.engine.proc.pid)
        except Exception:
            files = glob.glob("/dev/shm/eka_bloom_*")
            if len(files) == 1:
                path = files[0]
        if not path or not os.path.exists(path):
            return False
        if path == self.mm_path and self.mm:
            return True
        try:
            if self.mm:
                self.mm.close()
            with open(path, "rb") as f:
                self.mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
            if len(self.mm) < VIZ_SIZE:
                raise ValueError("short")
            self.mm_path = path
            return True
        except Exception:
            self.mm = None
            self.mm_path = None
            return False

    def _read_viz(self):
        """(clk, bpb, held notes, [(on, off, note, vel)]) from the plugin, or None."""
        if not self._open_mm():
            return None
        mm = self.mm
        for _ in range(3):
            try:
                h = VIZ_HDR.unpack_from(mm, 0)
                raw = mm[VIZ_HDR.size:VIZ_SIZE]
                seq2 = struct.unpack_from("<I", mm, 4)[0]
            except Exception:
                return None
            magic, seq, head, bpb, nheld = h[:5]
            if magic != VIZ_MAGIC:
                return None
            if (seq & 1) or seq2 != seq:                     # the plugin is writing: try again
                continue
            n = min(head, VIZ_NOTES)
            notes = []
            for k in range(n):
                on, off, note, vel, ch = VIZ_NOTE.unpack_from(raw, ((head - n + k) % VIZ_NOTES) * VIZ_NOTE.size)
                notes.append((on, off, note, vel))
            held = list(h[7][:min(nheld, 32)])
            return h[6], (bpb or 4), held, notes
        return None

    def _scale_set(self, held):
        """Pitch classes of the current scale (None for Chromatic); Key Auto follows the lowest held key."""
        sc = self._scale()
        if sc == 0:
            return None
        k = int(round(self._zval("key", 0)))
        if k >= 12:
            if not held:
                return None
            k = min(held) % 12
        return set((k + x) % 12 for x in SCALE_STEPS[sc])

    def _draw_roll(self):
        c = self.widget_canvas
        w, h, pad, tab_h, num_h, x0, x1, y0, y1, cw = self.geo
        fam = zynthian_gui_config.font_family
        tx_off = zynthian_gui_config.color_tx_off
        fs = max(6, int(tab_h * 0.3))
        fs_head = self._fit_size(fam, "Played notes", (x1 - x0) / 2.0 - 8, max(7, int(tab_h * 0.4)))
        bars_text = "%d bar%s" % (self.roll_bars, "s" if self.roll_bars > 1 else "")
        c.create_text(x0 + 2, tab_h // 2, anchor="w", text="Played notes", tags="dyn", fill=ROLL_COLOR,
                      font=(fam, fs_head))
        c.create_text(x1 - 2, tab_h // 2, anchor="e", text=bars_text, tags=("dyn", "bars"), fill=ROLL_COLOR,
                      font=(fam, fs_head))
        viz = self._read_viz()
        if viz is None:
            c.create_text((x0 + x1) // 2, (y0 + y1) // 2, text="no data from the plugin", tags="dyn",
                          fill=tx_off, font=(fam, fs + 2))
            return
        clk, bpb, held, notes = viz
        bar_t = bpb * PPQN
        W = self.roll_bars * bar_t
        sweep0 = math.floor(clk / W) * W                    # start of the window the playhead is in
        start = clk - W
        vis = [nt for nt in notes if start < (nt[1] if nt[1] >= 0 else clk) and nt[0] <= clk]

        # pitch range: whole octaves around the shown and held notes (changes only across an octave)
        pitches = [nt[2] for nt in vis] + held
        if pitches:
            lo = 12 * (min(pitches) // 12)
            hi = 12 * (max(pitches) // 12) + 11
        else:
            lo, hi = 48, 71
        span = hi - lo + 1
        kw = max(8, int(cw * 0.6))                           # key strip on the left
        nx0, nx1 = x0 + kw + 2, x1
        nw = float(nx1 - nx0)
        rh = (y1 - y0) / float(span)

        def ry(p):
            return y1 - (p - lo + 1) * rh

        scale = self._scale_set(held)
        held_set = set(held)
        for p in range(lo, hi + 1):
            pc = p % 12
            in_sc = (pc in scale) if scale is not None else (pc not in BLACK_KEYS)
            if in_sc:                                        # rows of the scale (white keys for Chromatic)
                c.create_rectangle(nx0, ry(p), nx1, ry(p) + rh, fill="#161616", width=0, tags="dyn")
            key_col = "#3a3a3a" if pc in BLACK_KEYS else "#9a9a9a"
            if p in held_set:
                key_col = ROLL_COLOR
            c.create_rectangle(x0, ry(p) + 0.5, x0 + kw, ry(p) + rh - 0.5, fill=key_col, width=0,
                               tags=("dyn", "held") if p in held_set else "dyn")
            if pc == 0 and rh * 12 >= 3 * fs:                # octave label on C
                c.create_text(nx0 + 2, ry(p) + rh / 2.0, anchor="w", text="C%d" % (p // 12 - 1), tags="dyn",
                              fill=tx_off, font=(fam, fs))
        # beats and bars
        nbeats = self.roll_bars * bpb
        for b in range(nbeats + 1):
            x = nx0 + nw * b / nbeats
            c.create_line(x, y0, x, y1, fill=("#5a5a5a" if b % bpb == 0 else "#2a2a2a"), tags="dyn")

        def tx(t, prev):
            return nx0 + nw * (t - (sweep0 - W if prev else sweep0)) / W

        # notes: this pass bright, the previous pass dim ahead of the playhead (overwritten as it moves)
        n_drawn = 0
        for (on, off, note, vel) in vis:
            if not (lo <= note <= hi):
                continue
            end = off if off >= 0 else clk
            f = 0.35 + 0.65 * min(vel, 127) / 127.0
            yt, yb = ry(note) + 0.5, ry(note) + max(2.0, rh - 0.5)
            for (a, b, prev) in ((max(on, start), min(end, sweep0), True), (max(on, sweep0), end, False)):
                if b <= a and not (b == a and not prev and off < 0):
                    continue
                xa, xb = tx(a, prev), tx(b, prev)
                c.create_rectangle(xa, yt, max(xb, xa + 2), yb, width=0, tags=("dyn", "note"),
                                   fill=dim(ROLL_COLOR, f * (0.35 if prev else 1.0)))
                n_drawn += 1
        # playhead
        px = tx(clk, False)
        c.create_line(px, y0, px, y1, fill="#ffffff", width=2, tags=("dyn", "playhead"))
        c.create_rectangle(nx0, y0, nx1, y1, outline="#555555", tags="dyn")
        # bar numbers of the song under the roll, window length in the corner
        first_bar = int(sweep0 // bar_t)
        for k in range(self.roll_bars):
            c.create_text(nx0 + nw * k / self.roll_bars + 3, h - num_h // 2 - 1, anchor="w",
                          text=str(first_bar + k + 1), tags="dyn", fill=tx_off, font=(fam, max(7, num_h // 2 + 1)))
        if not vis:
            c.create_text((nx0 + nx1) // 2, (y0 + y1) // 2, text="play a chord", tags="dyn",
                          fill=tx_off, font=(fam, fs + 2))


    # ---------- Play view: pads and XY pad, held = on ----------
    def _play_geo(self):
        w, h, pad, tab_h, num_h, x0, x1, y0, y1, cw = self.geo
        gap = 4
        bottom = h - pad
        split = x0 + (x1 - x0) * 0.6                        # pads on the left, XY pad on the right
        lh = max(24, int((bottom - y0) * 0.17))             # Latch button under the pads
        pbot = bottom - lh - gap
        rows = (len(PLAY_PADS) + PAD_COLS - 1) // PAD_COLS
        pw = (split - x0 - gap * PAD_COLS) / float(PAD_COLS)
        ph = (pbot - y0 - gap * (rows - 1)) / float(rows)
        pads = []
        for k in range(len(PLAY_PADS)):
            r, col = divmod(k, PAD_COLS)
            a, b = x0 + col * (pw + gap), y0 + r * (ph + gap)
            pads.append((a, b, a + pw, b + ph))
        xy = (split + gap, y0, x1, bottom)
        self.latch_box = (x0, bottom - lh, split - gap, bottom)
        return pads, xy

    def _pad_at(self, x, y):
        pads, xy = self._play_geo()
        for k, (a, b, c_, d) in enumerate(pads):
            if a <= x <= c_ and b <= y <= d:
                return k
        return None

    def _xy_set(self, x, y):
        pads, (a, b, c_, d) = self._play_geo()
        fx_ = min(max((x - a) / float(c_ - a), 0.0), 1.0)
        fy = min(max((d - y) / float(d - b), 0.0), 1.0)
        self._zset("density", int(round(fx_ * 100)))
        self._zset("dynamics", int(round(fy * 100)))

    def _play_press(self, x, y):
        if not self.geo:
            self._layout()
        pads, (a, b, c_, d) = self._play_geo()
        k = self._pad_at(x, y)
        la, lb, lc, ld = self.latch_box
        if k is not None:
            self.touch = "pad"
            self._zset("fx", PLAY_PADS[k][0])
        elif la <= x <= lc and lb <= y <= ld:                # Latch: tap toggles
            self._zset("latch", 0 if self._zval("latch", 0) >= 0.5 else 1)
        elif a <= x <= c_ and b <= y <= d:
            self.touch = "xy"
            self._xy_set(x, y)
        self._draw()

    def _play_move(self, x, y):
        if self.touch == "pad":                             # slide to another pad: its effect
            k = self._pad_at(x, y)
            if k is not None:
                self._zset("fx", PLAY_PADS[k][0])
        elif self.touch == "xy":
            self._xy_set(x, y)
        else:
            return
        self._draw()

    def _play_release(self):
        if self.touch == "pad":
            self._zset("fx", 0)
        elif self.touch == "xy":
            self._zset("density", 50)
            self._zset("dynamics", 50)
        if self.touch:
            self.touch = None
            self._draw()

    def _draw_play(self):
        c = self.widget_canvas
        w, h, pad, tab_h, num_h, x0, x1, y0, y1, cw = self.geo
        fam = zynthian_gui_config.font_family
        tx = zynthian_gui_config.color_tx
        tx_off = zynthian_gui_config.color_tx_off
        fs_head = self._fit_size(fam, "Play: hold a pad", (x1 - x0) * 0.6 - 8, max(7, int(tab_h * 0.4)))
        c.create_text(x0 + 2, tab_h // 2, anchor="w", text="Play: hold a pad", tags="dyn", fill=FX_PAD_COLOR,
                      font=(fam, fs_head))
        cur = int(round(self._zval("fx", 0)))
        dens = int(round(self._zval("density", 50)))
        dyn = int(round(self._zval("dynamics", 50)))
        pads, (a, b, c_, d) = self._play_geo()
        pw, ph = pads[0][2] - pads[0][0], pads[0][3] - pads[0][1]
        fs_t = min(self._fit_size(fam, t, pw - 8, max(8, int(ph * 0.28)), "bold") for (_, t, _) in PLAY_PADS)
        fs_s = min(self._fit_size(fam, st, pw - 6, max(7, int(ph * 0.18))) for (_, _, st) in PLAY_PADS)
        for k, (val, title, sub) in enumerate(PLAY_PADS):
            pa, pb, pc, pd = pads[k]
            col = ROLL_PAD_COLOR if val <= 3 else FX_PAD_COLOR
            on = (val == cur)
            c.create_rectangle(pa, pb, pc, pd, fill=(col if on else "#161616"), outline=col, width=(2 if on else 1),
                               tags=("dyn", "pad"))
            c.create_text((pa + pc) / 2.0, (pb + pd) / 2.0 - ph * 0.1, text=title, tags="dyn",
                          fill=("#000000" if on else col), font=(fam, fs_t, "bold"))
            c.create_text((pa + pc) / 2.0, (pb + pd) / 2.0 + ph * 0.2, text=sub, tags="dyn",
                          fill=("#000000" if on else tx_off), font=(fam, fs_s))
        # Latch: keys stay on after release; switched off when the transport stops
        la, lb, lc, ld = self.latch_box
        lon = self._zval("latch", 0) >= 0.5
        c.create_rectangle(la, lb, lc, ld, fill=(LATCH_COLOR if lon else "#161616"), outline=LATCH_COLOR,
                           width=(2 if lon else 1), tags=("dyn", "latch"))
        fs_lt = self._fit_size(fam, "LATCH  on", (lc - la) - 8, max(8, int((ld - lb) * 0.45)), "bold")
        c.create_text((la + lc) / 2.0, (lb + ld) / 2.0, text="LATCH  %s" % ("on" if lon else "off"), tags="dyn",
                      fill=("#000000" if lon else LATCH_COLOR), font=(fam, fs_lt, "bold"))
        # XY pad: x = density (sparse .. dense), y = dynamics (soft and short .. loud and long)
        live = self.touch == "xy" or dens != 50 or dyn != 50
        c.create_rectangle(a, b, c_, d, fill="#101010", outline=XY_COLOR, width=(2 if live else 1), tags=("dyn", "xy"))
        mx, my = (a + c_) / 2.0, (b + d) / 2.0
        c.create_line(mx, b + 1, mx, d - 1, fill="#2a2a2a", dash=(2, 4), tags="dyn")
        c.create_line(a + 1, my, c_ - 1, my, fill="#2a2a2a", dash=(2, 4), tags="dyn")
        fs_l = self._fit_size(fam, "sparse", (c_ - a) / 2.0 - 6, max(6, int(tab_h * 0.28)))
        for (xx, yy, anc, t) in ((a + 3, d - 2, "sw", "sparse"), (c_ - 3, d - 2, "se", "dense"),
                                 (a + 3, b + 2, "nw", "loud"), (a + 3, my + 2, "nw", "soft")):
            c.create_text(xx, yy, anchor=anc, text=t, tags="dyn", fill=tx_off, font=(fam, fs_l))
        px = a + (c_ - a) * dens / 100.0
        py = d - (d - b) * dyn / 100.0
        r = max(5, int(min(c_ - a, d - b) * 0.06))
        c.create_oval(px - r, py - r, px + r, py + r, fill=(XY_COLOR if live else ""), outline=XY_COLOR, width=2,
                      tags=("dyn", "xydot"))
        c.create_text(c_ - 2, tab_h // 2, anchor="e", text="dens %d  dyn %d" % (dens, dyn), tags="dyn",
                      fill=(XY_COLOR if live else tx_off), font=(fam, fs_l))

    # ---------- Scale view: keyboard = Key, scales, degrees ----------
    def _scale_press(self, x, y):
        for (a, b, c_, d, kind, val) in self.scale_boxes:
            if a <= x <= c_ and b <= y <= d:
                if kind == "key":
                    self._zset("key", val)
                elif kind == "scale":
                    self._zset("scale", val)
                elif kind == "degree":
                    self._zset("degree", val)
                self._draw()
                return

    def _draw_scale(self):
        c = self.widget_canvas
        w, h, pad, tab_h, num_h, x0, x1, y0, y1, cw = self.geo
        fam = zynthian_gui_config.font_family
        tx = zynthian_gui_config.color_tx
        tx_off = zynthian_gui_config.color_tx_off
        self.scale_boxes = []
        sc = self._scale()
        steps = SCALE_STEPS[sc]
        key_v = int(round(self._zval("key", 0)))
        auto = key_v >= 12
        viz = self._read_viz()
        held = viz[2] if viz else []
        sounding = None
        if viz:
            for (on, off, note, vel) in reversed(viz[3][-8:]):
                if off < 0:
                    sounding = note % 12
                    break
        key = (min(held) % 12 if held else None) if auto else key_v
        deg = min(max(int(round(self._zval("degree", 0))), 0), 6)
        in_scale = set(((key or 0) + x) % 12 for x in steps) if key is not None else set()

        # header: what is chosen
        if sc == 0:
            head = "Scale: Chromatic"
        else:
            kname = ("Auto (%s)" % NOTE_NAMES[key] if key is not None else "Auto") if auto else NOTE_NAMES[key]
            head = "Scale: %s %s" % (kname, SCALE_SHORT[sc].lower())
            if deg and len(steps) == 7:
                head += "  \u00b7  on %s" % degree_chord(steps, deg)[0]
            elif deg:
                head += "  \u00b7  degree %d" % (deg + 1)
        fs_head = self._fit_size(fam, head, x1 - x0 - 4, max(7, int(tab_h * 0.4)))
        c.create_text(x0 + 2, tab_h // 2, anchor="w", text=head, tags=("dyn", "scalehead"), fill=SCALE_COLOR,
                      font=(fam, fs_head))

        gap = 4
        bottom = h - pad
        total = bottom - y0
        kb_h = total * 0.40
        sc_h = (total * 0.34 - gap) / 2.0
        dg_y = y0 + kb_h + gap + 2 * (sc_h + gap)
        dg_h = bottom - dg_y

        # keyboard: 7 white keys and the Auto button
        auto_w = (x1 - x0) / 8.5 * 1.5
        kx1 = x1 - auto_w - gap
        ww = (kx1 - x0) / 7.0
        ky0, ky1 = y0, y0 + kb_h
        fs_k = self._fit_size(fam, "C#", ww * 0.6 - 2, max(7, int(kb_h * 0.16)))

        def key_fill(pc, white):
            if key is not None and pc == key:
                return SCALE_COLOR
            if pc in in_scale and sc != 0:
                return "#d6ccff" if white else "#5f4b9a"
            return "#9a9a9a" if white else "#2a2a2a"

        def marks(pc, cx_, by):
            if pc in set(n % 12 for n in held):              # held key: a dot
                c.create_oval(cx_ - 4, by - 12, cx_ + 4, by - 4, fill=ROLL_COLOR, outline="", tags=("dyn", "heldpc"))
            if sounding == pc:                               # the note sounding now: a ring
                c.create_oval(cx_ - 7, by - 15, cx_ + 7, by - 1, outline="#ffffff", width=2, tags=("dyn", "nowpc"))

        for j, pc in enumerate(WHITE_PCS):
            a = x0 + j * ww
            c.create_rectangle(a + 1, ky0, a + ww - 1, ky1, fill=key_fill(pc, True), outline="#000000",
                               tags=("dyn", "key"))
            c.create_text(a + ww / 2.0, ky1 - 3, anchor="s", text=NOTE_NAMES[pc], tags="dyn", fill="#000000",
                          font=(fam, fs_k))
            marks(pc, a + ww / 2.0, ky1 - fs_k * 1.6)
            self.scale_boxes.append((a, ky0 + kb_h * 0.6, a + ww, ky1, "key", pc))
        bw = ww * 0.6
        for pc, before in BLACK_PCS:
            a = x0 + before * ww - bw / 2.0
            c.create_rectangle(a, ky0, a + bw, ky0 + kb_h * 0.6, fill=key_fill(pc, False), outline="#000000",
                               tags=("dyn", "key"))
            marks(pc, a + bw / 2.0, ky0 + kb_h * 0.6 - 2)
            self.scale_boxes.insert(0, (a, ky0, a + bw, ky0 + kb_h * 0.6, "key", pc))   # black keys win
        for j, pc in enumerate(WHITE_PCS):                   # upper part of a white key between the black ones
            a = x0 + j * ww
            self.scale_boxes.append((a, ky0, a + ww, ky0 + kb_h * 0.6, "key", pc))
        aa = kx1 + gap
        c.create_rectangle(aa, ky0, x1, ky1, fill=(SCALE_COLOR if auto else "#161616"), outline=SCALE_COLOR,
                           tags=("dyn", "auto"))
        fs_a = self._fit_size(fam, "Auto", auto_w - 6, max(7, int(kb_h * 0.2)))
        c.create_text((aa + x1) / 2.0, (ky0 + ky1) / 2.0, text="Auto", tags="dyn",
                      fill=("#000000" if auto else SCALE_COLOR), font=(fam, fs_a))
        self.scale_boxes.insert(0, (aa, ky0, x1, ky1, "key", 12))

        # scales: 2 rows of 6
        sw = (x1 - x0 - gap * 5) / 6.0
        fs_s = min(self._fit_size(fam, t, sw - 6, max(7, int(sc_h * 0.4))) for t in SCALE_SHORT)
        for i, name in enumerate(SCALE_SHORT):
            r, col = divmod(i, 6)
            a = x0 + col * (sw + gap)
            b = y0 + kb_h + gap + r * (sc_h + gap)
            sel = (i == sc)
            c.create_rectangle(a, b, a + sw, b + sc_h, fill=(SCALE_COLOR if sel else "#161616"), outline=SCALE_COLOR,
                               tags=("dyn", "scalebtn"))
            c.create_text(a + sw / 2.0, b + sc_h / 2.0, text=name, tags="dyn",
                          fill=("#000000" if sel else tx), font=(fam, fs_s))
            self.scale_boxes.append((a, b, a + sw, b + sc_h, "scale", i))

        # degrees: the arpeggio moves to that chord of the key
        if sc == 0:
            c.create_text((x0 + x1) / 2.0, dg_y + dg_h / 2.0, text="choose a scale to move by degrees", tags="dyn",
                          fill=tx_off, font=(fam, fs_s))
            return
        n = len(steps)
        dw = (x1 - x0 - gap * (n - 1)) / float(n)
        labels = []
        for i in range(n):
            if n == 7:
                lab, suf = degree_chord(steps, i)
                chord = (NOTE_NAMES[(key + steps[i]) % 12] + suf) if key is not None else ""
            else:
                lab, chord = str(i + 1), (NOTE_NAMES[(key + steps[i]) % 12] if key is not None else "")
            labels.append((lab, chord))
        fs_d = min(self._fit_size(fam, l_, dw - 6, max(8, int(dg_h * 0.34)), "bold") for (l_, _) in labels)
        fs_c = min([self._fit_size(fam, ch, dw - 6, max(7, int(dg_h * 0.22))) for (_, ch) in labels if ch] or [7])
        for i, (lab, chord) in enumerate(labels):
            a = x0 + i * (dw + gap)
            sel = (i == deg)
            c.create_rectangle(a, dg_y, a + dw, bottom, fill=(SCALE_COLOR if sel else "#161616"), outline=SCALE_COLOR,
                               tags=("dyn", "degbtn"))
            c.create_text(a + dw / 2.0, dg_y + dg_h * 0.38, text=lab, tags="dyn",
                          fill=("#000000" if sel else tx), font=(fam, fs_d, "bold"))
            if chord:
                c.create_text(a + dw / 2.0, dg_y + dg_h * 0.75, text=chord, tags="dyn",
                              fill=("#000000" if sel else tx_off), font=(fam, fs_c))
            self.scale_boxes.append((a, dg_y, a + dw, bottom, "degree", i))
