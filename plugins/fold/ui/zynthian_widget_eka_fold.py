#!/usr/bin/python3
# -*- coding: utf-8 -*-
# Eka Fold widget. Tabs (top left): Osc. Later stages add Mod, Fold, Gate, Env, Delay, Matrix.
# The skeleton follows the Eka Mist widget:
# - Zynthian calls update() every 32 ms; monitors and marks are refreshed only every UPDATE_PERIOD.
# - Curves are computed here from the parameters and redrawn only when a parameter changed, at most every
#   CURVE_PERIOD; only small marks (notes, level) move with the live data, and only when they changed.
# - Live data from the plugin's output ports (self.monitors): note1..4 = MIDI note of each voice, -1 = idle;
#   out_peak = output peak, dB.
# - Tabs and knob pages go together: a tap on a tab selects its knob page through Zynthian's control screen;
#   turning to such a page shows its tab. Pages without a tab (Env, for now) keep the tab shown.
# - At the top right, in a small font: the name of the current knob page (screen_title of Zynthian's control screen).
# Osc:
# - The Principal waveform over 2 periods for the selected Source. Stage 0 has only Sine (Harmonics and Morph
#   come with the other sources), so the curve depends only on the size of the widget.
# - Source tiles: Sine only for now.
# - The sounding notes with their frequency (Pitch and Fine included) and a small output level meter.

import logging
import math
import time
import tkinter
import tkinter.font

from zyngui import zynthian_gui_config
from zyngui import zynthian_widget_base

MULTI_INSTANCE = True

TABS = ["Osc"]
TAB_GROUPS = ["g_osc"]           # knob page (port group) of each tab
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
UPDATE_PERIOD = 0.08             # s: monitors and marks at ~12 per second, not at Zynthian's 30
CURVE_PERIOD = 0.15              # s: curves redrawn at most this often
METER_DB = 48.0                  # the meter shows -48..0 dB
NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]


def note_name(n):
    return "%s%d" % (NOTE_NAMES[n % 12], n // 12 - 1)


def note_freq(n, pitch, fine):
    return 440.0 * 2.0 ** ((n - 69 + pitch + fine / 100.0) / 12.0)


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
        # Source tiles: only Sine for now, nothing to switch

    # ---------- drawing ----------
    def refresh_gui(self):
        if not self.processor:
            return
        self._follow_page()
        self._draw_tabs()
        self._draw_title()
        if self.tab == 0:
            self._draw_osc()

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
