#!/usr/bin/python3
# -*- coding: utf-8 -*-
# Eka Fold widget. Tabs (top left): Osc, Mod, Fold. Later stages add Gate, Env, Delay, Matrix.
# The skeleton follows the Eka Mist widget:
# - Zynthian calls update() every 32 ms; monitors and marks are refreshed only every UPDATE_PERIOD.
# - Curves are computed here from the parameters and redrawn only when a parameter changed, at most every
#   CURVE_PERIOD; only small marks (notes, level) move with the live data, and only when they changed.
# - Live data from the plugin's output ports (self.monitors): note1..4 = MIDI note of each voice, -1 = idle;
#   out_peak = output peak, dB; mod_phase = Mod OSC phase 0..1 of the newest voice, -1 = none.
# - Tabs and knob pages go together: a tap on a tab selects its knob page through Zynthian's control screen;
#   turning to such a page shows its tab. Pages without a tab (Env, for now) keep the tab shown.
# - At the top right, in a small font: the name of the current knob page (screen_title of Zynthian's control screen).
# Osc:
# - The Principal waveform over 2 periods for the selected Source. Stage 0 has only Sine (Harmonics and Morph
#   come with the other sources), so the curve depends only on the size of the widget.
# - Source tiles: Sine only for now.
# - The sounding notes with their frequency (Pitch and Fine included) and a small output level meter.
# Mod:
# - Mod OSC block (one period of its shape) -> arrows FM / AM / Sync / Timbre -> Principal block. A tap on an
#   arrow switches that route (LV2 parameters route_fm, route_am, route_sync, route_timbre, 0/1, saved in the
#   snapshot). Timbre: the Mod OSC moves the Timbre knob of the Fold tab.
# - The Principal block shows the result: the Principal modulated by the Mod OSC over a few periods, computed
#   here with the plugin's formulas (linear FM, AM, hard sync). In Free mode the Mod frequency is absolute,
#   so the result is drawn for C4.
# - A running mark of the Mod phase (output mod_phase) on the Mod OSC shape, only while it is slow
#   (Free mode, up to MARK_MAX_HZ): an LFO is seen "breathing"; at audio rates a mark would only flicker.
# - At the bottom: the Mod Index, large, and what the Mod Freq knob means in this Mod Mode
#   (Free: Hz, Keytrack: semitones, Ratio: x p/q), with the shape.
# Fold:
# - Left: the transfer curve of the Timbre section (input -1..1 -> output) for Timbre, Order, Symmetry, Drive.
#   Right: what it makes of the Principal sine, 2 periods, without its DC (the plugin removes it).
#   The formulas of fold_core.c: folder (gain 1..8), Saw / M (Chebyshev sums, drawn with FC_CHEB_MAX terms,
#   as a low note gets), Drive (tanh).
# - At the bottom: the Order line Folder - Saw - M with its mark, and the knob values.

import logging
import math
import time
import tkinter
import tkinter.font

from zyngui import zynthian_gui_config
from zyngui import zynthian_widget_base

MULTI_INSTANCE = True

TABS = ["Osc", "Mod", "Fold"]
TAB_GROUPS = ["g_osc", "g_mod", "g_timbre"]  # knob page (port group) of each tab
SOURCES = ["Sine"]               # Source tiles; VA, FM and Wavetable come at stage 4
NVOICES = 4
TAB_COLOR = "#4dabf7"
WAVE_COLOR = "#4dabf7"
AXIS_COLOR = "#2a2a2a"
NOTE_COLOR = "#ffd43b"
METER_COLOR = "#51cf66"
METER_HOT = "#ff6b6b"            # above -6 dB
TILE_ON = "#4dabf7"
TILE_OFF = "#2a2a2a"
ROUTE_ON = "#4dabf7"
ROUTE_OFF = "#505050"
MARK_COLOR = "#ffd43b"
LABEL_COLOR = "#909090"
MARK_MAX_HZ = 8.0                # the phase mark is shown up to this Mod frequency
UPDATE_PERIOD = 0.08             # s: monitors and marks at ~12 per second, not at Zynthian's 30
CURVE_PERIOD = 0.15              # s: curves redrawn at most this often
METER_DB = 48.0                  # the meter shows -48..0 dB
NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
MOD_SHAPES = ["Triangle", "Sine", "Saw", "Square"]   # Mod Shape knob, as FC_MOD_* in fold_core.h
MOD_MODES = ["Free", "Keytrack", "Ratio"]            # Mod Mode knob, as FC_MODE_*
ROUTES = [("route_fm", "FM"), ("route_am", "AM"), ("route_sync", "Sync"), ("route_timbre", "Timbre")]
ROUTE_DEFAULTS = {"route_fm": 1, "route_am": 0, "route_sync": 0, "route_timbre": 0}
# Mod Freq knob 0..100, read by the mode: the formulas of fold_core.c
FREQ_MAX = 100.0
FREE_MIN, FREE_MAX = 0.01, 10000.0
KEY_RANGE = 48.0
RATIOS = [(1, 8), (1, 6), (1, 4), (1, 3), (1, 2), (2, 3), (3, 4), (1, 1), (5, 4), (4, 3), (3, 2), (5, 3), (2, 1),
          (5, 2), (3, 1), (7, 2), (4, 1), (5, 1), (6, 1), (7, 1), (8, 1), (9, 1), (10, 1), (12, 1), (16, 1)]
FM_DEPTH = 4.0                   # FC_FM_DEPTH: Mod Index 100 % swings the Principal frequency by 4 x itself
FOLD_GAIN = 8.0                  # FC_FOLD_GAIN, FC_CHEB_MAX, FC_DRIVE_K of fold_core.h
CHEB_MAX = 32
DRIVE_K = 4.0
ORDER_NAMES = ["Folder", "Saw", "M"]
CURVE2_COLOR = "#ffd43b"         # the wave over the transfer curve
C4_HZ = 261.626


def note_name(n):
    return "%s%d" % (NOTE_NAMES[n % 12], n // 12 - 1)


def note_freq(n, pitch, fine):
    return 440.0 * 2.0 ** ((n - 69 + pitch + fine / 100.0) / 12.0)


def _clamp(u, lo, hi):
    return lo if u < lo else hi if u > hi else u


def free_hz(u):
    return FREE_MIN * (FREE_MAX / FREE_MIN) ** (_clamp(u, 0.0, FREQ_MAX) / FREQ_MAX)


def key_semis(u):
    return (_clamp(u, 0.0, FREQ_MAX) / FREQ_MAX * 2.0 - 1.0) * KEY_RANGE


def ratio_index(u):
    # lrintf rounds halves to even; the knob rarely sits on a half step, round() does the same
    return int(round(_clamp(u, 0.0, FREQ_MAX) * (len(RATIOS) - 1) / FREQ_MAX))


def ratio_text(i):
    p, q = RATIOS[i]
    return "%d" % p if q == 1 else "%d/%d" % (p, q)


def hz_text(hz):
    if hz >= 1000.0:
        return "%.2f kHz" % (hz / 1000.0)
    if hz >= 100.0:
        return "%.0f Hz" % hz
    if hz >= 10.0:
        return "%.1f Hz" % hz
    return "%.2f Hz" % hz


def freq_text(mode, u):
    """What the Mod Freq knob means in this mode."""
    if mode == 0:
        return "Free " + hz_text(free_hz(u))
    if mode == 1:
        return "Keytrack %+.1f st" % key_semis(u)
    return "Ratio \u00d7" + ratio_text(ratio_index(u))


def mod_ratio(mode, u):
    """Mod frequency over the Principal's (Free: for C4)."""
    if mode == 0:
        return free_hz(u) / C4_HZ
    if mode == 1:
        return 2.0 ** (key_semis(u) / 12.0)
    p, q = RATIOS[ratio_index(u)]
    return p / q


def mod_value(shape, ph):
    """Mod OSC -1..1 at phase 0..1 (without the band-limiting of the plugin)."""
    if shape == 0:
        return 4.0 * ph if ph < 0.25 else 2.0 - 4.0 * ph if ph < 0.75 else 4.0 * ph - 4.0
    if shape == 2:
        return 2.0 * ph - 1.0
    if shape == 3:
        return 1.0 if ph < 0.5 else -1.0
    return math.sin(2.0 * math.pi * ph)


def result_values(rel, shape, index, fm, am, sync, n, cycles, sub=8):
    """The Principal modulated by the Mod OSC, n + 1 values over `cycles` Principal periods.
    rel = Mod frequency / Principal frequency. Linear FM, AM and hard sync as in fold_core.c."""
    out = []
    ph = mph = 0.0
    dt = cycles / (n * sub)
    fmd = FM_DEPTH * index if fm else 0.0
    ama = index if am else 0.0
    for i in range(n + 1):
        m = mod_value(shape, mph)
        out.append(math.sin(2.0 * math.pi * ph) * ((1.0 - ama) + ama * m))
        for _ in range(sub):
            m = mod_value(shape, mph)
            ph = (ph + dt * (1.0 + fmd * m)) % 1.0
            mph += dt * rel
            if mph >= 1.0:
                mph -= math.floor(mph)
                if sync:
                    ph = 0.0
    return out


def fold(u):
    s = (u + 1.0) * 0.25
    s -= math.floor(s)
    return 4.0 * s - 1.0 if s < 0.5 else 3.0 - 4.0 * s


def fast_tanh(x):
    if x > 3.0:
        return 1.0
    if x < -3.0:
        return -1.0
    x2 = x * x
    return x * (27.0 + x2) / (27.0 + 9.0 * x2)


def order_weights(o):
    """(folder, Saw, M) shares of Order 0..1."""
    if o < 0.5:
        return 1.0 - 2.0 * o, 2.0 * o, 0.0
    return 0.0, 2.0 - 2.0 * o, 2.0 * o - 1.0


def cheb_coefs(ws, wm, n=CHEB_MAX):
    hs = sum(1.0 / k for k in range(1, n + 1))
    hm = sum(1.0 / k for k in range(1, n + 1, 2))
    tot = ws + wm if ws + wm > 1e-6 else 1.0
    return [0.0] + [(ws / hs + (wm / hm if k & 1 else 0.0)) / (k * tot) for k in range(1, n + 1)]


def chebyshev(a, u):
    b1 = b2 = 0.0
    for k in range(len(a) - 1, 0, -1):
        b1, b2 = a[k] + 2.0 * u * b1 - b2, b1
    return u * b1 - b2


def shape(x, timbre, order, sym, drive, a=None):
    """The Timbre section of fold_core.c for one input value (without ADAA and the DC blocker)."""
    wf, ws, wm = order_weights(order)
    sh = wf * fold((1.0 + (FOLD_GAIN - 1.0) * timbre) * x + sym) if wf > 0.0 else 0.0
    if wf < 1.0:
        uc = max(-1.0, min(1.0, x + sym))
        ch = chebyshev(a if a is not None else cheb_coefs(ws, wm), uc) if timbre > 0.0 else 0.0
        sh += (1.0 - wf) * ((1.0 - timbre) * x + timbre * ch)
    if drive > 1e-4:
        kd = 1.0 + DRIVE_K * drive
        sh = (1.0 - drive) * sh + drive * fast_tanh(kd * sh) / fast_tanh(kd)
    return sh


def wave_points(source, x0, x1, y0, y1, periods=2):
    """Points of the Principal waveform over the box, one every 2 px."""
    mid, amp = (y0 + y1) / 2.0, (y1 - y0) / 2.0 * 0.9
    pts = []
    n = max(int((x1 - x0) / 2), 8)
    for i in range(n + 1):
        t = i / n
        y = math.sin(2.0 * math.pi * periods * t)           # Sine; other sources at stage 4
        pts += [x0 + t * (x1 - x0), mid - y * amp]
    return pts


class zynthian_widget_eka_fold(zynthian_widget_base.zynthian_widget_base):

    def __init__(self, parent):
        super().__init__(parent)
        self.header_h = {}           # font size -> line height
        self.next_update = 0.0
        self.tab = 0
        self.tab_boxes = []          # (x0, x1) of each tab
        self.last_group = None       # knob page (port group symbol) seen on the previous refresh
        self.widget_canvas = tkinter.Canvas(
            self, highlightthickness=0, relief='flat',
            bg=zynthian_gui_config.color_bg)
        self.widget_canvas.grid(sticky='news')
        self.widget_canvas.bind("<ButtonPress-1>", self._on_press)
        self._clear()

    def update(self):
        """Zynthian calls this every 32 ms: each call also asks jalv for all monitors. Slow it down."""
        now = time.monotonic()
        if now < self.next_update:
            return
        self.next_update = now + UPDATE_PERIOD
        super().update()

    def on_size(self, event):
        if not super().on_size(event):
            return
        self.widget_canvas.grid(row=0, column=0, sticky='news')
        self._clear()
        self.refresh_gui()

    def _clear(self):
        """Empty canvas: everything is drawn again on the next refresh (tab switch, resize)."""
        self.widget_canvas.delete("all")
        self.title = None            # knob page name shown now
        self.tabs_drawn = None       # (tab, size, title) the tab bar was drawn for
        self.wave_view = None        # values the waveform was drawn for
        self.wave_drawn = 0.0        # when it was drawn
        self.tiles_view = None
        self.tile_boxes = []         # (x0, y0, x1, y1, source index)
        self.notes_view = None       # text of the sounding notes shown now
        self.meter_view = None       # meter height shown now
        self.mod_view = None         # values the Mod tab curves were drawn for
        self.mod_drawn = 0.0
        self.route_boxes = []        # (x0, y0, x1, y1, symbol)
        self.mark_view = None        # where the phase mark is shown, None = hidden
        self.mark_item = None
        self.fold_view = None        # values the Fold tab was drawn for
        self.fold_drawn = 0.0

    # ---------- plugin access ----------
    def _page_title(self):
        try:
            return str(self.zyngui_control.screen_title or "")
        except Exception:
            return ""

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
            logging.debug("Eka Fold widget: can't set %s => %s", sym, e)
            return False

    def _route(self, sym):
        return 1 if self._zval(sym, ROUTE_DEFAULTS[sym]) >= 0.5 else 0

    def _mon(self, key, default=0.0):
        try:
            return float((self.monitors or {}).get(key, default))
        except Exception:
            return default

    def _font_size(self):
        return max(8, min(max(self.height, 20) // 14, 13))

    def _font(self, size=None):
        return (zynthian_gui_config.font_family, size or self._font_size())

    def _header_h(self):
        """Height of the page name line, from the real font metrics (Tk sizes are points, not pixels)."""
        fs = self._font_size()
        if fs not in self.header_h:
            try:
                f = tkinter.font.Font(family=zynthian_gui_config.font_family, size=fs)
                self.header_h[fs] = f.metrics("linespace") + 4
            except Exception:
                self.header_h[fs] = 2 * fs
        return self.header_h[fs]

    # ---------- tab <-> knob page ----------
    def _page_rows(self):
        """Zynthian's list of control pages: (index, group symbol) of this processor's pages."""
        try:
            rows = self.zyngui_control.list_data
        except Exception:
            return []
        out = []
        for i, row in enumerate(rows):
            if len(row) >= 5 and row[0] and row[3] is self.processor:
                out.append((i, row[0]))
        return out

    def _select_page(self, tab):
        for i, group in self._page_rows():
            if group == TAB_GROUPS[tab]:
                try:
                    self.zyngui_control.select(i)
                except Exception as e:
                    logging.debug("Eka Fold widget: can't select page %s => %s", group, e)
                return

    def _current_group(self):
        try:
            info = self.zyngui_control.screen_info
            if info and len(info) >= 5 and info[3] is self.processor:
                return info[0]
        except Exception:
            pass
        return None

    def _follow_page(self):
        """The knob page changed (a knob or the menu): show its tab. Pages without a tab keep the tab."""
        group = self._current_group()
        if group == self.last_group:
            return
        self.last_group = group
        if group in TAB_GROUPS and TAB_GROUPS.index(group) != self.tab:
            self.tab = TAB_GROUPS.index(group)
            self._clear()

    # ---------- touch ----------
    def _on_press(self, event):
        if event.y < self._header_h():                              # tab bar
            for i, (x0, x1) in enumerate(self.tab_boxes):
                if x0 <= event.x < x1:
                    if i != self.tab:
                        self.tab = i
                        self._clear()
                    self.last_group = TAB_GROUPS[i]
                    self._select_page(i)                            # a tap on the shown tab also brings its page back
                    self.refresh_gui()
            return
        if self.tab == 1:
            for x0, y0, x1, y1, sym in self.route_boxes:
                if x0 <= event.x <= x1 and y0 <= event.y <= y1:
                    self._zset(sym, 1 - self._route(sym))
                    self.mod_view = None
                    self.mod_drawn = 0.0                    # a tap shows at once
                    self.refresh_gui()
                    return
        # Osc: Source tiles, only Sine for now, nothing to switch

    # ---------- drawing ----------
    def refresh_gui(self):
        if not self.processor:
            return
        self._follow_page()
        self._draw_tabs()
        self._draw_title()
        if self.tab == 0:
            self._draw_osc()
        elif self.tab == 1:
            self._draw_mod()
        elif self.tab == 2:
            self._draw_fold()

    def _tab_metrics(self, title):
        """(font size, padding, widths, title shown): the tabs and the page title share the top line. The padding
        shrinks first, then the font; if the tabs still do not fit beside the title, the title goes."""
        fam, fs = zynthian_gui_config.font_family, self._font_size()

        def measure(size, text):
            try:
                return tkinter.font.Font(family=fam, size=size).measure(text)
            except Exception:
                return len(text) * size
        w = max(self.width, 80)
        for show in ((True, False) if title else (False,)):
            avail = w - 4 - (measure(fs, title) + 14 if show else 0)
            for size in range(fs, max(7, fs - 4) - 1, -1):
                for pad in (2 * size, size, size // 2):
                    widths = [measure(size, t) + pad for t in TABS]
                    if sum(widths) + 4 * len(TABS) <= avail:
                        return size, pad, widths, show
        size = max(7, fs - 4)
        return size, size // 2, [measure(size, t) + size // 2 for t in TABS], False

    def _draw_tabs(self):
        title = self._page_title()
        key = (self.tab, self.width, self.height, title)
        if key == self.tabs_drawn:
            return
        self.tabs_drawn = key
        c = self.widget_canvas
        c.delete("tabs")
        hh = self._header_h()
        size, pad, widths, show = self._tab_metrics(title)
        self.title_shown = show
        self.title = None                                 # the title follows (shown or not)
        x = 2
        self.tab_boxes = []
        for i, (t, tw) in enumerate(zip(TABS, widths)):
            sel = i == self.tab
            c.create_rectangle(x, 1, x + tw, hh - 2, outline=TAB_COLOR, fill=(TAB_COLOR if sel else ""), tags="tabs")
            c.create_text(x + tw / 2, hh / 2, text=t, font=self._font(size), tags="tabs",
                          fill=("#000000" if sel else TAB_COLOR))
            self.tab_boxes.append((x, x + tw + 2))
            x += tw + 4

    def _draw_title(self):
        title = self._page_title()
        if not getattr(self, "title_shown", True):
            title = ""                                    # no room beside the tabs
        if title == self.title:
            return
        self.title = title
        c = self.widget_canvas
        c.delete("title")
        c.create_text(max(self.width, 20) - 6, 2, anchor="ne", text=title, tags="title",
                      fill=zynthian_gui_config.color_tx_off, font=self._font())

    # ---------- Osc tab ----------
    def _osc_layout(self):
        """Boxes: Source tiles (a row under the tabs), the waveform, the meter at its right, the notes line."""
        w, h = max(int(self.width), 60), max(int(self.height), 60)
        hh, fs = self._header_h(), self._font_size()
        row = 2 * fs + 6
        tiles = (4, hh + 4, w - 4, hh + 4 + row)
        meter_w = max(6, fs // 2 + 2)
        notes_y = h - row // 2 - 2
        wave = (4, tiles[3] + 6, w - meter_w - 12, h - row - 6)
        meter = (w - meter_w - 4, wave[1], w - 4, wave[3])
        return tiles, wave, meter, notes_y

    def _draw_osc(self):
        tiles, wave, meter, notes_y = self._osc_layout()
        self._draw_tiles(tiles)
        self._draw_wave(wave)
        self._draw_meter(meter)
        self._draw_notes(notes_y)

    def _draw_tiles(self, box):
        source = 0
        view = (box, source)
        if view == self.tiles_view:
            return
        self.tiles_view = view
        c = self.widget_canvas
        c.delete("tiles")
        self.tile_boxes = []
        x0, y0, x1, y1 = box
        tw = min((x1 - x0) / max(len(SOURCES), 4), 140)
        for i, name in enumerate(SOURCES):
            bx = x0 + i * tw
            on = i == source
            c.create_rectangle(bx, y0, bx + tw - 4, y1, outline=TILE_ON, fill=(TILE_ON if on else TILE_OFF),
                               tags="tiles")
            c.create_text(bx + (tw - 4) / 2, (y0 + y1) / 2, text=name, font=self._font(),
                          fill=("#000000" if on else zynthian_gui_config.color_tx), tags="tiles")
            self.tile_boxes.append((bx, y0, bx + tw - 4, y1, i))

    def _draw_wave(self, box):
        source = 0
        view = (box, source)
        if view == self.wave_view:
            return
        now = time.monotonic()
        if self.wave_view is not None and now - self.wave_drawn < CURVE_PERIOD:
            return                                        # changed again: drawn on a later refresh
        self.wave_view = view
        self.wave_drawn = now
        c = self.widget_canvas
        c.delete("wave")
        x0, y0, x1, y1 = box
        mid = (y0 + y1) / 2
        c.create_rectangle(x0, y0, x1, y1, outline=AXIS_COLOR, tags="wave")
        c.create_line(x0, mid, x1, mid, fill=AXIS_COLOR, tags="wave")
        c.create_line(*wave_points(source, x0, x1, y0, y1), fill=WAVE_COLOR, width=2, tags="wave")

    def _draw_meter(self, box):
        x0, y0, x1, y1 = box
        db = self._mon("out_peak", -100.0)
        frac = min(max((db + METER_DB) / METER_DB, 0.0), 1.0)
        hpx = int(round(frac * (y1 - y0)))
        view = (box, hpx, db > -6.0)
        if view == self.meter_view:
            return
        self.meter_view = view
        c = self.widget_canvas
        c.delete("meter")
        c.create_rectangle(x0, y0, x1, y1, outline=AXIS_COLOR, tags="meter")
        if hpx > 0:
            c.create_rectangle(x0 + 1, y1 - hpx, x1 - 1, y1 - 1, outline="",
                               fill=(METER_HOT if db > -6.0 else METER_COLOR), tags="meter")

    def _notes_text(self):
        pitch, fine = self._zval("pitch", 0.0), self._zval("fine", 0.0)
        notes = []
        for i in range(NVOICES):
            n = int(round(self._mon("note%d" % (i + 1), -1.0)))
            if 0 <= n <= 127 and n not in notes:
                notes.append(n)
        if not notes:
            return "Pitch %+d st  Fine %+d ct" % (round(pitch), round(fine))
        return "   ".join("%s %.1f Hz" % (note_name(n), note_freq(n, pitch, fine)) for n in sorted(notes))

    def _draw_notes(self, y):
        text = self._notes_text()
        view = (y, self.width, text)
        if view == self.notes_view:
            return
        self.notes_view = view
        c = self.widget_canvas
        c.delete("notes")
        c.create_text(6, y, anchor="w", text=text, font=self._font(), fill=NOTE_COLOR, tags="notes")

    # ---------- Mod tab ----------
    def _mod_layout(self):
        """Boxes: Mod OSC (left), the route arrows (middle), Principal (right); the bottom line (Index, mode)."""
        w, h = max(int(self.width), 60), max(int(self.height), 60)
        hh, fs = self._header_h(), self._font_size()
        bottom = 2 * fs + 10                            # the Index is drawn larger than the rest
        y0, y1 = hh + 4, h - bottom - 4
        xa, xb = int(w * 0.36), int(w * 0.64)
        left = (4, y0, xa - 4, y1)
        mid = (xa + 4, y0, xb - 4, y1)
        right = (xb + 4, y0, w - 4, y1)
        return left, mid, right, h - bottom / 2 - 2

    def _mod_values(self):
        return (int(round(self._zval("mod_shape", 1))), int(round(self._zval("mod_mode", 2))),
                self._zval("mod_freq", 30.0), self._zval("mod_index", 0.0) / 100.0,
                self._route("route_fm"), self._route("route_am"), self._route("route_sync"),
                self._route("route_timbre"))

    def _draw_mod(self):
        left, mid, right, bottom_y = self._mod_layout()
        vals = self._mod_values()
        view = (left, mid, right, vals)
        if view != self.mod_view:
            now = time.monotonic()
            if self.mod_view is None or now - self.mod_drawn >= CURVE_PERIOD:
                self.mod_view = view
                self.mod_drawn = now
                self._draw_mod_static(left, mid, right, bottom_y, vals)
                self.mark_view = None
        self._draw_mod_mark(left, vals)

    def _box_label(self, box, text, tag):
        self.widget_canvas.create_text(box[0] + 4, box[1] + 2, anchor="nw", text=text, fill=LABEL_COLOR,
                                       font=self._font(max(7, self._font_size() - 2)), tags=tag)

    def _curve_box(self, box):
        """The part of a block under its label."""
        lh = self._font_size() + 6
        return box[0] + 4, box[1] + lh, box[2] - 4, box[3] - 4

    def _draw_mod_static(self, left, mid, right, bottom_y, vals):
        shape, mode, u, index, fm, am, sync, timb = vals
        c = self.widget_canvas
        c.delete("mod")
        fs = self._font_size()
        # Mod OSC block: one period of its shape
        c.create_rectangle(*left, outline=AXIS_COLOR, tags="mod")
        self._box_label(left, "Mod OSC", "mod")
        x0, y0, x1, y1 = self._curve_box(left)
        mid_y, amp = (y0 + y1) / 2.0, (y1 - y0) / 2.0 * 0.85
        n = max(int((x1 - x0) / 2), 8)
        pts = []
        for i in range(n + 1):
            pts += [x0 + i * (x1 - x0) / n, mid_y - mod_value(shape, i / n) * amp]
        c.create_line(x0, mid_y, x1, mid_y, fill=AXIS_COLOR, tags="mod")
        c.create_line(*pts, fill=WAVE_COLOR, width=2, tags="mod")
        # the route arrows
        self.route_boxes = []
        mx0, my0, mx1, my1 = mid
        gap = 6
        bh = (my1 - my0 - (len(ROUTES) - 1) * gap) / float(len(ROUTES))
        for i, (sym, name) in enumerate(ROUTES):
            on = (fm, am, sync, timb)[i]
            by0 = my0 + i * (bh + gap)
            by1 = by0 + bh
            col = ROUTE_ON if on else ROUTE_OFF
            c.create_rectangle(mx0, by0, mx1, by1, outline=col, fill=(col if on else ""), tags="mod")
            c.create_text((mx0 + mx1) / 2, (by0 + by1) / 2, text=name + " \u2192", font=self._font(),
                          fill=("#000000" if on else zynthian_gui_config.color_tx), tags="mod")
            self.route_boxes.append((mx0, by0, mx1, by1, sym))
        # Principal block: the result
        c.create_rectangle(*right, outline=AXIS_COLOR, tags="mod")
        self._box_label(right, "Principal" + (" at C4" if mode == 0 else ""), "mod")
        x0, y0, x1, y1 = self._curve_box(right)
        mid_y, amp = (y0 + y1) / 2.0, (y1 - y0) / 2.0 * 0.85
        rel = mod_ratio(mode, u)
        cycles = 2.0
        if rel >= 0.25:                                 # show whole Mod periods when the Mod OSC is slower
            cycles = min(max(2.0, 2.0 / rel), 8.0)
        n = max(int((x1 - x0) / 2), 8)
        vals_y = result_values(rel, shape, index, fm, am, sync, n, cycles)   # before the Timbre section
        pts = []
        for i, y in enumerate(vals_y):
            pts += [x0 + i * (x1 - x0) / n, mid_y - y * amp]
        c.create_line(x0, mid_y, x1, mid_y, fill=AXIS_COLOR, tags="mod")
        c.create_line(*pts, fill=WAVE_COLOR, width=2, tags="mod")
        # bottom: the Index (large) and the Mod Freq meaning
        c.create_text(6, bottom_y, anchor="w", text="Index", fill=LABEL_COLOR, font=self._font(), tags="mod")
        try:
            iw = tkinter.font.Font(family=zynthian_gui_config.font_family, size=fs).measure("Index ")
        except Exception:
            iw = 6 * fs
        c.create_text(6 + iw, bottom_y, anchor="w", text="%d%%" % round(index * 100), fill=NOTE_COLOR,
                      font=(zynthian_gui_config.font_family, int(fs * 1.6), "bold"), tags="mod")
        c.create_text(max(self.width, 60) - 6, bottom_y, anchor="e",
                      text="%s \u00b7 %s" % (freq_text(mode, u), MOD_SHAPES[shape % len(MOD_SHAPES)]),
                      fill=zynthian_gui_config.color_tx, font=self._font(), tags="mod")

    def _draw_mod_mark(self, left, vals):
        """The Mod phase on the Mod OSC shape, while the Mod OSC is slow; moved, not recreated."""
        mshape, mode, u = vals[0], vals[1], vals[2]
        ph = self._mon("mod_phase", -1.0)
        show = mode == 0 and free_hz(u) <= MARK_MAX_HZ and 0.0 <= ph <= 1.0
        x0, y0, x1, y1 = self._curve_box(left)
        if show:
            mid_y, amp = (y0 + y1) / 2.0, (y1 - y0) / 2.0 * 0.85
            x = int(round(x0 + ph * (x1 - x0)))
            y = int(round(mid_y - mod_value(mshape, ph) * amp))
            view = (x, y)
        else:
            view = None
        if view == self.mark_view:
            return
        self.mark_view = view
        c = self.widget_canvas
        if view is None:
            c.delete("modmark")
            self.mark_item = None
            return
        r = max(3, self._font_size() // 3)
        if self.mark_item is None or not c.find_withtag("modmark"):
            self.mark_item = c.create_oval(x - r, y - r, x + r, y + r, fill=MARK_COLOR, outline="", tags="modmark")
        else:
            c.coords(self.mark_item, x - r, y - r, x + r, y + r)
        c.tag_raise("modmark")

    # ---------- Fold tab ----------
    def _fold_layout(self):
        """Boxes: transfer curve (left), wave (right); the bottom: Order line (left half), values (right half)."""
        w, h = max(int(self.width), 60), max(int(self.height), 60)
        hh, fs = self._header_h(), self._font_size()
        bottom = 2 * fs + 12
        y0, y1 = hh + 4, h - bottom - 4
        xa = int(w * 0.45)
        left = (4, y0, xa - 4, y1)
        right = (xa + 4, y0, w - 4, y1)
        order = (10, h - bottom + 2, int(w * 0.42), h - 4)
        return left, right, order, h - bottom / 2 - 2

    def _fold_values(self):
        return (self._zval("timbre", 0.0) / 100.0, self._zval("order", 0.0) / 100.0,
                self._zval("symmetry", 0.0) / 100.0, self._zval("drive", 0.0) / 100.0)

    def _draw_fold(self):
        left, right, order, bottom_y = self._fold_layout()
        vals = self._fold_values()
        view = (left, right, vals)
        if view == self.fold_view:
            return
        now = time.monotonic()
        if self.fold_view is not None and now - self.fold_drawn < CURVE_PERIOD:
            return                                        # changed again: drawn on a later refresh
        self.fold_view = view
        self.fold_drawn = now
        timbre, o, sym, drive = vals
        c = self.widget_canvas
        c.delete("fold")
        wf, ws, wm = order_weights(o)
        a = cheb_coefs(ws, wm) if wf < 1.0 and timbre > 0.0 else None
        # transfer curve, with the diagonal (no change) behind it
        c.create_rectangle(*left, outline=AXIS_COLOR, tags="fold")
        self._box_label(left, "In \u2192 Out", "fold")
        x0, y0, x1, y1 = self._curve_box(left)
        side = min(x1 - x0, y1 - y0)
        cx, cy = (x0 + x1) / 2.0, (y0 + y1) / 2.0
        half = side / 2.0 * 0.92
        c.create_line(cx - half, cy, cx + half, cy, fill=AXIS_COLOR, tags="fold")
        c.create_line(cx, cy - half, cx, cy + half, fill=AXIS_COLOR, tags="fold")
        c.create_line(cx - half, cy + half, cx + half, cy - half, fill=AXIS_COLOR, dash=(2, 3), tags="fold")
        n = max(int(side / 2), 16)
        pts = []
        for i in range(n + 1):
            xin = -1.0 + 2.0 * i / n
            pts += [cx + xin * half, cy - shape(xin, timbre, o, sym, drive, a) * half]
        c.create_line(*pts, fill=WAVE_COLOR, width=2, tags="fold")
        # the Principal sine through it, 2 periods, DC removed
        c.create_rectangle(*right, outline=AXIS_COLOR, tags="fold")
        self._box_label(right, "Principal \u2192 Timbre", "fold")
        x0, y0, x1, y1 = self._curve_box(right)
        mid_y, amp = (y0 + y1) / 2.0, (y1 - y0) / 2.0 * 0.85
        n = max(int((x1 - x0) / 2), 16)
        ys = [shape(math.sin(4.0 * math.pi * i / n), timbre, o, sym, drive, a) for i in range(n + 1)]
        mean = sum(ys[:-1]) / n
        pts = []
        for i, y in enumerate(ys):
            pts += [x0 + i * (x1 - x0) / n, mid_y - (y - mean) * amp]
        c.create_line(x0, mid_y, x1, mid_y, fill=AXIS_COLOR, tags="fold")
        c.create_line(*pts, fill=CURVE2_COLOR, width=2, tags="fold")
        # Order line: Folder - Saw - M, the mark at Order
        ox0, oy0, ox1, oy1 = order
        ly = oy0 + (oy1 - oy0) * 0.35
        fs = self._font_size()
        small = self._font(max(7, fs - 2))
        c.create_line(ox0, ly, ox1, ly, fill=ROUTE_OFF, width=2, tags="fold")
        for k, name in enumerate(ORDER_NAMES):
            x = ox0 + k * (ox1 - ox0) / 2.0
            share = (wf, ws, wm)[k]
            c.create_oval(x - 3, ly - 3, x + 3, ly + 3, fill=(ROUTE_ON if share > 0.0 else ROUTE_OFF), outline="",
                          tags="fold")
            c.create_text(x, oy1, anchor=("sw" if k == 0 else "se" if k == 2 else "s"), text=name, font=small,
                          fill=(zynthian_gui_config.color_tx if share > 0.0 else LABEL_COLOR), tags="fold")
        mx = ox0 + o * (ox1 - ox0)
        r = max(4, fs // 2)
        c.create_oval(mx - r, ly - r, mx + r, ly + r, fill=MARK_COLOR, outline="", tags="fold")
        vals_t = (round(timbre * 100), round(sym * 100), round(drive * 100))
        text = "Timbre %d%%  Sym %+d  Drive %d%%" % vals_t
        room = max(self.width, 60) - 6 - ox1 - 2 * fs   # right of the Order line
        try:
            if tkinter.font.Font(family=zynthian_gui_config.font_family, size=fs).measure(text) > room:
                text = "T %d  S %+d  D %d" % vals_t
        except Exception:
            pass
        c.create_text(max(self.width, 60) - 6, bottom_y, anchor="e", text=text,
                      fill=zynthian_gui_config.color_tx, font=self._font(), tags="fold")
