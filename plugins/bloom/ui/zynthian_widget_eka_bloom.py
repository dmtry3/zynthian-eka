#!/usr/bin/python3
# -*- coding: utf-8 -*-
# Touchscreen editor of the Eka Bloom pattern steps.
# Step values live in plugin parameters (prob1..16, shift1..16, vel1..16, gate1..16, ratch1..16);
# the widget only writes them through regular Zynthian controllers, so snapshots keep them.
# The Play tab has big touch tiles for the main controls (live playing without the encoders).
# The Roll tab shows the notes Bloom has played, read from shared memory /dev/shm/eka_bloom_<pid>
# (written by the plugin, see Viz in eka_bloom.c).
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

# ---------- Roll tab ----------
ROLL = len(LANES)            # tab index of the note roll, after the rows
ROLL_COLOR = "#ffd43b"
ROLL_BARS = (1, 2, 4)        # window lengths, a tap on the roll cycles them
PPQN = 960
VIZ_MAGIC = 0x4D4F4C42
VIZ_HDR = struct.Struct("<6Id32s")       # magic, seq, head, bpb, nheld, rolling, clk, held[32]
VIZ_NOTE = struct.Struct("<ddBBB5x")     # on, off (< 0: sounding), note, vel, ch
VIZ_NOTES = 512
VIZ_SIZE = VIZ_HDR.size + VIZ_NOTES * VIZ_NOTE.size
BLACK_KEYS = (1, 3, 6, 8, 10)
NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]

# ---------- Play tab ----------
PLAY = ROLL + 1
NTABS = PLAY + 1
PLAY_COLOR = "#38d9a9"
FLASH = 0.4                  # s: a pressed action tile stays lit this long
# (symbol, title, kind, values, labels): toggle = tap switches; enum / num = left half -, right half +
# (no wrap); action = widget actions ("_freeze", "_reset"). Values are what the controllers take.
PLAY_TILES = [
    ("latch", "Latch", "toggle", None, None),
    ("rate", "Rate", "enum", list(range(6)), ["1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32"]),
    ("order", "Order", "enum", list(range(7)), ["Up", "Down", "Up-Dn", "Dn-Up", "Played", "Random", "Loop"]),
    ("octaves", "Octaves", "enum", [1, 2, 3, 4], ["1", "2", "3", "4"]),
    ("transpose", "Transpose", "enum", [-2, -1, 0, 1, 2], ["-2 oct", "-1 oct", "0", "+1 oct", "+2 oct"]),
    ("gate", "Gate", "num", list(range(5, 101, 5)), None),
    ("swing", "Swing", "num", list(range(50, 76, 5)), None),
    ("mut_steps", "Mutate", "num", list(range(0, 101, 10)), None),
    ("_freeze", "Freeze", "action", None, None),
    ("_reset", "New loop", "action", None, None),
]
PLAY_COLS = 5


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
        self.frozen = None           # Play/Freeze: (mut_steps, mut_notes) before freezing
        self.flash = (None, 0.0)     # Play: (tile symbol, time) of the last action tap
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
        if key == "ratch":                                   # 1 hit = a quarter, 4 hits = full height
            return v / float(hi)
        return (v - lo) / float(hi - lo)

    def _value_at(self, lane, y):
        w, h, pad, tab_h, num_h, x0, x1, y0, y1, cw = self.geo
        f = min(max((y1 - y) / float(max(1, y1 - y0)), 0.0), 1.0)
        key, _, lo, hi, _ = LANES[lane]
        if key == "gate":
            return GATE_VALUES[int(round(f * (len(GATE_VALUES) - 1)))]
        if key == "ratch":
            return min(max(int(f * hi + 0.5), lo), hi)
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
        if event.y < tab_h:                                  # lane tabs, Roll, Play
            self.last_tap = (-1, -1, 0.0)
            tw = w / float(NTABS)
            i = int(event.x / tw)
            if 0 <= i < NTABS:
                self.lane = i
                self._draw()
            return
        if self.lane == PLAY:
            hit = self._tile_at(event.x, event.y)
            if hit:
                self._play_tap(*hit)
                self._draw()
            return
        if self.lane == ROLL:                                # tap on the roll: 1 / 2 / 4 bars
            if event.y <= y1:
                k = ROLL_BARS.index(self.roll_bars) if self.roll_bars in ROLL_BARS else 0
                self.roll_bars = ROLL_BARS[(k + 1) % len(ROLL_BARS)]
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

        # tabs: the rows, then Roll and Play; one font size that fits the widest label
        tw = w / float(NTABS)
        labels = ["%s %d" % (LANES[i][1], self._lane_len(i)) for i in range(len(LANES))] + ["Roll", "Play"]
        colors = LANE_COLORS + [ROLL_COLOR, PLAY_COLOR]
        fs_tab = min(self._fit_size(fam, t, tw - 8, fs_tab) for t in labels)
        for i in range(NTABS):
            sel = (i == lane)
            tcol = colors[i]
            text = labels[i]
            c.create_rectangle(int(i * tw) + 1, 1, int((i + 1) * tw) - 2, tab_h - 2, tags="dyn",
                               fill=(tcol if sel else ""), outline=tcol)
            c.create_text(int((i + 0.5) * tw), tab_h // 2, text=text, tags="dyn",
                          fill=("#000000" if sel else tcol), font=(fam, fs_tab))
        if lane == ROLL:
            self._draw_roll()
            return
        if lane == PLAY:
            self._draw_play()
            return

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

    def _text_w(self, fam, size, text):
        k = (fam, size, text)
        if k not in self.font_cache:
            try:
                self.font_cache[k] = tkinter.font.Font(family=fam, size=size).measure(text)
            except Exception:
                self.font_cache[k] = int(len(text) * size * 0.75)
        return self.font_cache[k]

    def _fit_size(self, fam, text, maxw, start):
        """Largest font size <= start (down to 6) at which text is at most maxw pixels wide."""
        size = start
        while size > 6 and self._text_w(fam, size, text) > maxw:
            size -= 1
        return size

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
        c.create_text(x1 - 2, h - num_h // 2 - 1, anchor="e", text="%d bar%s" % (self.roll_bars, "s" if self.roll_bars > 1 else ""),
                      tags="dyn", fill=ROLL_COLOR, font=(fam, max(7, num_h // 2 + 1)))
        if not vis:
            c.create_text((nx0 + nx1) // 2, (y0 + y1) // 2, text="play a chord", tags="dyn",
                          fill=tx_off, font=(fam, fs + 2))

    # ---------- Play tab: big tiles for live playing ----------
    def _tile_rects(self):
        w, h, pad, tab_h, num_h, x0, x1, y0, y1, cw = self.geo
        rows = (len(PLAY_TILES) + PLAY_COLS - 1) // PLAY_COLS
        gap = 4
        bottom = h - pad
        tw_ = (x1 - x0 - gap * (PLAY_COLS - 1)) / float(PLAY_COLS)
        th = (bottom - y0 - gap * (rows - 1)) / float(rows)
        rects = []
        for k in range(len(PLAY_TILES)):
            r, col = divmod(k, PLAY_COLS)
            tx0 = x0 + col * (tw_ + gap)
            ty0 = y0 + r * (th + gap)
            rects.append((tx0, ty0, tx0 + tw_, ty0 + th))
        return rects

    def _tile_at(self, x, y):
        """(tile index, +1 for the right half / -1 for the left half) or None."""
        for k, (a, b, c_, d) in enumerate(self._tile_rects()):
            if a <= x <= c_ and b <= y <= d:
                return k, (1 if x >= (a + c_) / 2.0 else -1)
        return None

    def _play_value(self, sym, values):
        v = int(round(self._zval(sym, values[0] if values else 0)))
        if values and v not in values:                       # e.g. Gate 52 set by an encoder
            v = min(values, key=lambda x: abs(x - v))
        return v

    def _play_tap(self, k, side):
        sym, title, kind, values, labels = PLAY_TILES[k]
        if kind == "toggle":
            self._zset(sym, 0 if self._zval(sym, 0) >= 0.5 else 1)
        elif kind in ("enum", "num"):
            i = values.index(self._play_value(sym, values))
            i = min(max(i + side, 0), len(values) - 1)
            self._zset(sym, values[i])
        elif sym == "_reset":                                # any change of Reset = a new loop and no drift
            self._zset("reset", 0 if self._zval("reset", 0) >= 0.5 else 1)
            self.flash = (sym, time.monotonic())
        elif sym == "_freeze":
            if self.frozen is not None:
                st, nt = self.frozen
                self._zset("mut_steps", st)
                self._zset("mut_notes", nt)
                self.frozen = None
            else:
                st = int(round(self._zval("mut_steps", 0)))
                nt = int(round(self._zval("mut_notes", 0)))
                if st or nt:
                    self.frozen = (st, nt)
                    self._zset("mut_steps", 0)
                    self._zset("mut_notes", 0)
                else:
                    self.flash = (sym, time.monotonic())     # nothing mutates: just blink

    def _draw_play(self):
        c = self.widget_canvas
        fam = zynthian_gui_config.font_family
        tx = zynthian_gui_config.color_tx
        tx_off = zynthian_gui_config.color_tx_off
        if self.frozen is not None and (self._zval("mut_steps", 0) > 0 or self._zval("mut_notes", 0) > 0):
            self.frozen = None                               # mutation turned on again from elsewhere
        now = time.monotonic()
        rects = self._tile_rects()
        tw_ = rects[0][2] - rects[0][0]
        th = rects[0][3] - rects[0][1]
        titles = [t[1] for t in PLAY_TILES]
        fs_title = min(self._fit_size(fam, t, tw_ - 8, max(7, int(th * 0.16))) for t in titles)
        for k, (sym, title, kind, values, labels) in enumerate(PLAY_TILES):
            a, b, c_, d = rects[k]
            lit = False
            if kind == "toggle":
                lit = self._zval(sym, 0) >= 0.5
                text = "On" if lit else "Off"
            elif kind in ("enum", "num"):
                v = self._play_value(sym, values)
                text = labels[values.index(v)] if labels else ("%d%%" % v)
            elif sym == "_freeze":
                lit = self.frozen is not None
                text = "On" if lit else "Off"
            else:
                text = "Reset"
            if self.flash[0] == sym and now - self.flash[1] < FLASH:
                lit = True
            c.create_rectangle(a, b, c_, d, fill=(PLAY_COLOR if lit else "#161616"), outline=PLAY_COLOR,
                               width=1, tags=("dyn", "tile"))
            fg = "#000000" if lit else tx
            c.create_text((a + c_) / 2.0, b + 3, anchor="n", text=title, tags="dyn",
                          fill=("#000000" if lit else tx_off), font=(fam, fs_title))
            fs_v = self._fit_size(fam, text, tw_ - 24, max(8, int(th * 0.3)))
            c.create_text((a + c_) / 2.0, (b + d) / 2.0 + th * 0.08, text=text, tags=("dyn", "tileval"),
                          fill=fg, font=(fam, fs_v, "bold"))
            if kind in ("enum", "num"):                      # halves: - on the left, + on the right
                i = values.index(v)
                fs_a = max(7, int(fs_v * 0.8))
                for (xx, anc, ch, ok) in ((a + 3, "w", "\u2039", i > 0), (c_ - 3, "e", "\u203a", i < len(values) - 1)):
                    c.create_text(xx, (b + d) / 2.0 + th * 0.08, anchor=anc, text=ch, tags="dyn",
                                  fill=(PLAY_COLOR if ok else "#333333"), font=(fam, fs_a))
            if kind == "num":                                # how far along the range
                f = (v - values[0]) / float(values[-1] - values[0])
                c.create_rectangle(a + 4, d - 6, a + 4 + (c_ - a - 8) * f, d - 3, fill=PLAY_COLOR, width=0,
                                   tags="dyn")
