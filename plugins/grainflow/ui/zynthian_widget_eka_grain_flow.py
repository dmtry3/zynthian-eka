#!/usr/bin/python3
# -*- coding: utf-8 -*-
# Zynthian finds this widget by the plugin's name: "Eka Grain Flow" -> zynthian_widget_eka_grain_flow
# (lower case, spaces to _); the file and the class must both have that name.
# Eka Grain Flow widget. One screen:
# - Left column: tall stream buttons 1 / 2 / 3. Top line: Freeze, Mod, page title and grain count.
#   A tap on a tab selects that stream (the Stream knob) and shows the Grain page; a long press turns the
#   stream on or off (LV2 parameter sN_on). A stream that is off has a dim, outlined tab.
# - The buffer: the input level of the last Buffer seconds, the newest at the right (the write head).
#   It is drawn from the plugin's "peak" output, sampled here; it stands still and turns blue while frozen.
#   Over it the sounding grains (outputs grain1..16: stream + seconds behind the head / 20), one colour per
#   stream, higher for a higher Pitch. The Offset of the selected stream is a dashed line; the part closer to
#   the head than the shortest Offset (pitch up needs room, output min_offset) is shaded.
# - Bottom row: continuous grain Shape, Buffer length (- / +), Sync.
#   Per-stream Scan, Fine, Intervals and Spread live on the Flow encoder page.
# - Mod (button in the top line) shows the modulation page instead of the buffer: LFO 1 and LFO 2 (a tap on
#   the shape changes it: Sine, Tri, Drift = smooth random, S&H; Rate: finger up / down, double tap = default;
#   a dot runs on the shape from the plugin's outputs lfo1, lfo2), and the depth grid of the selected stream:
#   LFO 1 / LFO 2 onto Pitch, Offset, Length and Level (LV2 parameters m<lfo>_s<stream>_<destination>,
#   -100..100 %): finger up / down (up increases), double tap = 0.
#
# The Grain, Flow and Out knob pages act on the SELECTED stream. The plugin keeps the values of all streams; when
# the selection changes (here or with the Stream knob) or a preset is loaded (output "loads" counts up), this
# widget moves the knobs to the values the plugin shows for the selected stream (outputs sel, cur_*). It waits
# until the plugin's "sel" matches the Stream knob, so it never copies the values of the previous stream.
#
# Zynthian calls update() every 32 ms; the monitors and the drawing are refreshed every UPDATE_PERIOD only,
# and canvas items are moved, not recreated.
import logging
import math
import time
import tkinter
import tkinter.font

from zyngui import zynthian_gui_config
from zyngui import zynthian_widget_base

MULTI_INSTANCE = True

UPDATE_PERIOD = 0.08            # s
STREAMS = 3
POOL = 48                       # GX_POOL in grain_fx.h
NGRAINS = 16                    # grain output ports
NCOLS = 96                      # columns of the level profile
LONG_PRESS = 0.6                # s: a long press on a tab turns the stream on / off
STREAM_COLORS = ["#4dabf7", "#ffd43b", "#ff8787"]
DIM = "#505050"
FREEZE_COLOR = "#74c0fc"
LEVEL_COLOR = "#3a4a5a"
FROZEN_LEVEL_COLOR = "#1c7ed6"
HEAD_COLOR = "#ffffff"
FACES = ["offset", "rate", "length", "pitch", "reverse", "filter", "spray", "level",
         "scan", "fine", "interval", "spread"]  # port order; pages Grain, Out, Flow
DIV_NAMES = ["1/64", "1/32T", "1/64.", "1/32", "1/16T", "1/32.", "1/16", "1/8T", "1/16.", "1/8", "1/4T", "1/8.",
             "1/4", "1/2T", "1/4.", "1/2", "1/2.", "1 bar", "2 bars", "4 bars"]          # gx_div_names
STREAM_GROUPS = ("g_grain", "g_flow", "g_out")
MAX_PITCH = 24.0
BUFFER_MIN, BUFFER_MAX = 1, 8
LFO_SHAPES = ["Sine", "Tri", "Drift", "S&H"]   # GX_LFO_*
LFO_MIN_HZ, LFO_MAX_HZ = 0.02, 20.0
LFO_DEFAULT_HZ = (0.3, 1.0)
LFO_COLORS = ["#b197fc", "#63e6be", "#ffa94d"]   # LFO 1, LFO 2, Env
SRC_NAMES = ["LFO 1", "LFO 2", "Env"]
MOD_DSTS = [("pitch", "Pitch"), ("offset", "Offset"), ("length", "Length"), ("level", "Level"), ("filter", "Filter"), ("scan", "Scan")]
CELL_PER_PX = 1 / 1.5           # % per px
RATE_OCT_PER_PX = 1 / 18.0      # octaves of LFO rate per vertical px
DOUBLE_TAP = 0.35               # s


class zynthian_widget_eka_grain_flow(zynthian_widget_base.zynthian_widget_base):

    def __init__(self, parent):
        super().__init__(parent)
        self.next_update = 0.0
        self.levels = [0.0] * NCOLS     # level profile, oldest first
        self.last_sample = 0.0          # when the profile last moved
        self.drawn = None               # (width, height) the static items were drawn for
        self.cols = []                  # level profile items
        self.grain_items = []
        self.top_view = None            # values the top line was drawn for
        self.bottom_view = None         # values the bottom row was drawn for
        self.offset_view = None         # values the Offset line was drawn for
        self.tab_boxes = []             # (x0, y0, x1, y1), vertical stream buttons
        self.tab_view = None
        self.freeze_box = None
        self.bottom_boxes = {}          # name -> (x0, y0, x1, y1)
        self.press = None               # (what, time) of the finger down
        self.drag = None                # (symbol, start value, start coordinate, kind) while dragging
        self.last_tap = (None, 0.0)     # (symbol, time) of the last tap on a value, for a double tap
        self.env_view = False
        self.view = 0                   # 0 buffer, 1 mod
        self.mod_box = None             # the Mod button
        self.mod_view = None            # values the mod page was drawn for
        self.mod_boxes = []             # mod page: (x0, y0, x1, y1, kind, arg)
        self.lfo_dots = []
        self.freeze_slid = False         # distinguish a horizontal slide from a full-capture tap
        self.sync_pending = True        # move the Grain/Flow/Out knobs to the selected stream when the plugin is ready
        self.last_stream = None
        self.last_loads = None
        self.widget_canvas = tkinter.Canvas(self, highlightthickness=0, relief='flat',
                                            bg=zynthian_gui_config.color_bg)
        self.widget_canvas.grid(sticky='news')
        self.widget_canvas.bind("<ButtonPress-1>", self._on_press)
        self.widget_canvas.bind("<B1-Motion>", self._on_motion)
        self.widget_canvas.bind("<ButtonRelease-1>", self._on_release)

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
        self.drawn = None
        self.refresh_gui()

    # ---------- plugin access ----------
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
            logging.debug("Eka Grain Flow widget: can't set %s => %s", sym, e)
            return False

    def _mon(self, key, default=0.0):
        try:
            return float((self.monitors or {}).get(key, default))
        except Exception:
            return default

    def _stream(self):
        return min(max(int(round(self._zval("stream", 1))), 1), STREAMS)

    def _on_mask(self):
        return int(round(self._mon("on_mask", 1)))

    def _page_title(self):
        try:
            return str(self.zyngui_control.screen_title or "")
        except Exception:
            return ""

    # ---------- knob pages ----------
    def _page_rows(self):
        """(index, group symbol) of this processor's pages in Zynthian's list of control pages."""
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

    def _show_grain_page(self):
        """After choosing a stream: show Grain unless a per-stream page is already shown."""
        if self._current_group() in STREAM_GROUPS:
            return
        for i, group in self._page_rows():
            if group == "g_grain":
                try:
                    self.zyngui_control.select(i)
                except Exception as e:
                    logging.debug("Eka Grain Flow widget: can't select the Grain page => %s", e)
                return

    def _sync_faces(self):
        """Move the Grain/Flow/Out knobs to the selected stream, once the plugin shows that stream."""
        stream, loads = self._stream(), self._mon("loads", 0)
        if stream != self.last_stream or loads != self.last_loads:
            self.last_stream, self.last_loads = stream, loads
            self.sync_pending = True
        if not self.sync_pending or not self.monitors:
            return
        if int(round(self._mon("sel", 0))) != stream:
            return                                       # the plugin has not seen the new selection yet
        for sym in FACES:
            key = "cur_" + sym
            if key in self.monitors:
                self._zset(sym, self._mon(key))
        self.sync_pending = False

    # ---------- layout ----------
    def _font_size(self):
        return max(8, min(max(self.height, 20) // 14, 13))

    def _fit_font(self, text, room, size=None):
        fam = zynthian_gui_config.font_family
        top = size or self._font_size()
        key = (text, int(room), top)
        cache = self.__dict__.setdefault("fit_cache", {})
        if key in cache:
            return cache[key]
        if len(cache) > 400:
            cache.clear()
        fonts = self.__dict__.setdefault("fit_fonts", {})
        out = (fam, 7)
        for fs in range(top, 6, -1):
            try:
                if fs not in fonts:
                    fonts[fs] = tkinter.font.Font(family=fam, size=fs)
                if fonts[fs].measure(text) <= room:
                    out = (fam, fs)
                    break
            except Exception:
                out = (fam, fs)
                break
        cache[key] = out
        return out

    def _header_h(self):
        """Height of the top control bar, sized for fingers (matches Eka Fold)."""
        fs = self._font_size()
        h = max(int(self.height), 60)
        return max(2 * fs + 6, min(54, int(h * 0.12)))

    def _btn_h(self):
        """Height of touch buttons and sliders, sized for fingers (matches Eka Fold)."""
        fs = self._font_size()
        h = max(int(self.height), 60)
        return max(2 * fs + 6, min(60, int(h * 0.135)))

    def _btn_font_size(self):
        fs = self._font_size()
        return min(15, fs + 2) if self._btn_h() >= 46 else fs

    def _tab_w(self):
        """Width of the left vertical stream tabs, sized for fingers."""
        w = max(int(self.width), 80)
        return max(44, min(68, int(w * 0.11)))

    def _line_h(self):
        return self._header_h()

    def _content_x(self):
        return self._tab_w() + 8

    def _area(self):
        """x0, y0, x1, y1 of the buffer view."""
        return self._content_x(), self._header_h() + 4, max(self.width, 80) - 2, max(self.height, 60) - self._btn_h() - 6

    def _buffer_s(self):
        return min(max(int(round(self._zval("buffer", 4))), BUFFER_MIN), BUFFER_MAX)

    def _x_of(self, seconds_back):
        x0, _, x1, _ = self._area()
        span = float(self._buffer_s())
        return x1 - (x1 - x0) * min(max(seconds_back / span, 0.0), 1.0)

    # ---------- drawing ----------
    def refresh_gui(self):
        if not self.processor:
            return
        self._sync_faces()
        c = self.widget_canvas
        size = (self.width, self.height, self.view)
        if self.view == 1:
            if self.drawn != size:
                self.drawn = size
                c.delete("all")
                self.tab_view = None
                self.top_view = self.bottom_view = self.offset_view = self.mod_view = None
                self.cols, self.grain_items, self.lfo_dots = [], [], []
                self.bottom_boxes = {}
            self._draw_top()
            self._sample_levels()
            self._draw_mod()
            return
        if self.drawn != size:
            self.drawn = size
            c.delete("all")
            self.tab_view = None
            self.mod_boxes = []
            self.cols, self.grain_items = [], []
            self.top_view = self.bottom_view = self.offset_view = None
            x0, y0, x1, y1 = self._area()
            c.create_rectangle(x0, y0, x1, y1, outline=DIM, tags="frame")
            for i in range(NCOLS):
                self.cols.append(c.create_line(0, 0, 0, 0, fill=LEVEL_COLOR, width=1, tags="level"))
            c.create_line(x1, y0, x1, y1, fill=HEAD_COLOR, width=2, tags="head")
            c.create_rectangle(0, 0, 0, 0, fill="#2b2b2b", outline="", tags="minoff")
            c.create_line(0, 0, 0, 0, fill=HEAD_COLOR, dash=(3, 3), tags="offset")
            for i in range(NGRAINS):
                self.grain_items.append(c.create_rectangle(0, 0, 0, 0, outline="", state="hidden", tags="grain"))
            c.tag_lower("minoff")
            c.tag_raise("level")
            c.tag_raise("offset")
            c.tag_raise("grain")
        self._draw_top()
        self._draw_levels()
        self._draw_offset()
        self._draw_grains()
        self._draw_bottom()

    def _draw_tabs(self):
        stream, mask = self._stream(), self._on_mask()
        view = (self.width, self.height, stream, mask)
        if view == self.tab_view:
            return
        self.tab_view = view
        c = self.widget_canvas
        c.delete("tabs")
        bfs = self._btn_font_size()
        width = self._tab_w()
        height = (max(self.height, 60) - 12) / 3.0
        self.tab_boxes = []
        for i in range(STREAMS):
            x0, y0 = 2, 2 + i * (height + 4)
            x1, y1 = x0 + width, y0 + height
            on = bool(mask & (1 << i))
            col = STREAM_COLORS[i] if on else DIM
            selected = i + 1 == stream
            c.create_rectangle(x0, y0, x1, y1, outline=col, width=2 if selected else 1,
                               fill=col if selected else "", tags="tabs")
            label = str(i + 1) if on else "%d\noff" % (i + 1)
            font = self._fit_font("off" if not on else str(i + 1), width - 8, bfs)
            c.create_text((x0 + x1) / 2, (y0 + y1) / 2,
                          text=label, font=font,
                          fill=("#000000" if on else zynthian_gui_config.color_tx) if selected else col, tags="tabs")
            self.tab_boxes.append((x0, y0, x1, y1))

    def _draw_top(self):
        self._draw_tabs()
        stream, mask = self._stream(), self._on_mask()
        frozen = self._zval("freeze", 0) >= 0.5 or self._zval("freeze_amount", 0) >= 100
        active = int(self._mon("active", 0))
        title = self._page_title()
        amount = int(round(self._zval("freeze_amount", 0)))
        view = (self.width, self.height, stream, mask, frozen, amount, active, title, self.view)
        if view == self.top_view:
            return
        self.top_view = view
        c = self.widget_canvas
        c.delete("top")
        fs = self._font_size()
        bfs = self._btn_font_size()
        hh = self._header_h()
        fx0 = self._content_x()
        avail = max(self.width, 80) - fx0 - 4
        fw = max(84, min(140, int(avail * 0.30)))
        mw = max(68, min(110, int(avail * 0.22)))
        self.freeze_box = (fx0, fx0 + fw)
        ftext = "Frz %d%%" % amount
        c.create_rectangle(fx0, 2, fx0 + fw, hh - 2, outline=FREEZE_COLOR, fill=(FREEZE_COLOR if frozen else ""), tags="top")
        c.create_text(fx0 + fw / 2, hh / 2, text=ftext, font=self._fit_font(ftext, fw - 10, bfs),
                      fill=("#000000" if frozen else FREEZE_COLOR), tags="top")
        mx0 = fx0 + fw + 6
        self.mod_box = (mx0, mx0 + mw)
        on = self.view == 1
        c.create_rectangle(mx0, 2, mx0 + mw, hh - 2, outline=LFO_COLORS[0], fill=(LFO_COLORS[0] if on else ""), tags="top")
        c.create_text(mx0 + mw / 2, hh / 2, text="Mod", font=self._fit_font("Mod", mw - 10, bfs),
                      fill=("#000000" if on else LFO_COLORS[0]), tags="top")
        small = (zynthian_gui_config.font_family, max(7, fs - 2))
        right = "%s · %d/%d" % (title, active, POOL) if title else "%d/%d" % (active, POOL)
        c.create_text(max(self.width, 80) - 4, hh / 2, text=right, anchor="e", font=small,
                      fill=zynthian_gui_config.color_tx_off, tags="top")

    def _sample_levels(self):
        """Keep the level profile going (also while the mod page is shown)."""
        frozen = self._zval("freeze", 0) >= 0.5 or self._zval("freeze_amount", 0) >= 100
        now = time.monotonic()
        # one column per (buffer / NCOLS) seconds: the profile scrolls at the speed of the buffer
        step = self._buffer_s() / float(NCOLS)
        pk = min(max(self._mon("peak", 0.0), 0.0), 1.0)
        loop = self._mon("freeze_phase", -1) >= 0
        if loop:
            pk = min(max(self._mon("record_peak", pk), 0.0), 1.0)
        if frozen and not loop:
            self.last_sample = now
        elif now - self.last_sample >= step:
            n = min(int((now - self.last_sample) / step), NCOLS)
            self.levels = self.levels[n:] + [pk] * n
            self.last_sample += n * step

    def _draw_levels(self):
        self._sample_levels()
        frozen = self._zval("freeze", 0) >= 0.5 or self._zval("freeze_amount", 0) >= 100
        x0, y0, x1, y1 = self._area()
        mid, half = (y0 + y1) / 2.0, (y1 - y0) / 2.0 - 2
        w = (x1 - x0) / float(NCOLS)
        loop = self._mon("freeze_phase", -1) >= 0
        col = FROZEN_LEVEL_COLOR if frozen and not loop else LEVEL_COLOR
        c = self.widget_canvas
        lw = max(1, int(w))
        for i, item in enumerate(self.cols):
            h = max(1.0, self.levels[i] ** 0.5 * half)   # square root: quiet input still shows
            x = x0 + (i + 0.5) * w
            c.coords(item, x, mid - h, x, mid + h)
            c.itemconfig(item, fill=col, width=lw)

    def _draw_offset(self):
        s = self._stream()
        off = self._zval("offset", 100) / 1000.0
        minoff = self._mon("min_offset", 0) / 1000.0
        view = (self.width, self.height, s, round(off, 3), round(minoff, 3), self._buffer_s())
        if view == self.offset_view:
            return
        self.offset_view = view
        x0, y0, x1, y1 = self._area()
        c = self.widget_canvas
        xo = self._x_of(max(off, minoff))
        c.coords("offset", xo, y0 + 1, xo, y1 - 1)
        c.itemconfig("offset", fill=STREAM_COLORS[s - 1])
        xm = self._x_of(minoff)
        c.coords("minoff", xm, y0 + 1, x1, y1 - 1)

    def _draw_grains(self):
        x0, y0, x1, y1 = self._area()
        mid, half = (y0 + y1) / 2.0, (y1 - y0) / 2.0 - 6
        gw = max(3, (x1 - x0) // 120)
        gh = max(4, (y1 - y0) // 20)
        c = self.widget_canvas
        for i, item in enumerate(self.grain_items):
            v = self._mon("grain%d" % (i + 1), -1)
            if v < 0:
                c.itemconfig(item, state="hidden")
                continue
            s = min(int(v), STREAMS - 1)
            back = (v - int(v)) * 20.0
            pitch = self._zval("s%d_pitch" % (s + 1), 0)
            x = self._x_of(back)
            y = mid - pitch / MAX_PITCH * half
            c.coords(item, x - gw / 2, y - gh / 2, x + gw / 2, y + gh / 2)
            c.itemconfig(item, fill=STREAM_COLORS[s], state="normal")

    def _draw_bottom(self):
        shape = min(max(self._zval("shape", 0), 0), 1)
        buf = self._buffer_s()
        minoff = int(round(self._mon("min_offset", 0)))
        stream = self._stream()
        sync = self._zval("sync", 0) >= 0.5
        bpm = int(round(self._mon("bpm", 120)))
        rdiv, odiv = int(round(self._mon("rate_div", -1))), int(round(self._mon("offset_div", -1)))
        view = (self.width, self.height, shape, buf, minoff, stream, sync, bpm, rdiv, odiv, self._zval("length", 500))
        if view == self.bottom_view:
            return
        self.bottom_view = view
        c = self.widget_canvas
        c.delete("bottom")
        fs = self._font_size()
        bfs = self._btn_font_size()
        bh = self._btn_h()
        y1 = max(self.height, 60) - 2
        y0 = y1 - bh
        x = self._content_x()
        avail = max(self.width, 80) - 2 - x
        self.bottom_boxes = {}

        def button(name, text, on, width):
            nonlocal x
            col = zynthian_gui_config.color_tx if on else zynthian_gui_config.color_tx_off
            outline = FREEZE_COLOR if on else col
            fill = FREEZE_COLOR if on else ""
            tcol = "#000000" if on else col
            c.create_rectangle(x, y0, x + width, y1, outline=outline, fill=fill, tags="bottom")
            c.create_text(x + width / 2, (y0 + y1) / 2, text=text, font=self._fit_font(text, width - 8, bfs),
                          fill=tcol, tags="bottom")
            self.bottom_boxes[name] = (x, y0, x + width, y1)
            x += width + 4

        gap_total = 4 * 4 + 4
        usable = max(avail - gap_total, 180)
        pm_w = max(36, min(bh, int(usable * 0.13)))
        buf_lbl_w = max(48, min(80, int(usable * 0.17)))
        rest = max(usable - 2 * pm_w - buf_lbl_w, 80)
        shp_w = int(rest * 0.56)
        sync_w = rest - shp_w

        button("shape", "Shp %d%%" % round(shape * 100), False, shp_w)
        sx0, sy0, sx1, sy1 = self.bottom_boxes["shape"]
        # Separate thumbnail above the control: full button height remains available for its label.
        pic_h = max(24, min(36, int(bh * 0.65)))
        pic_bottom, pic_top = sy0 - 5, sy0 - 5 - pic_h
        c.create_rectangle(sx0, pic_top, sx1, pic_bottom, outline=DIM, fill="#181818", tags="bottom")
        length_ms = max(10, self._zval("length", 500))
        taper = min(1, max(1.0 - 0.98 * shape, 2.0 / length_ms))
        # Include the exact knees even at 100%, where a uniform miniature would miss them.
        phases = sorted(set([i / 48.0 for i in range(49)] + [taper / 2, 1 - taper / 2]))
        pts = []
        for ph in phases:
            edge = min(ph, 1.0 - ph)
            amp = 0.5 - 0.5 * math.cos(math.pi * min(2 * edge / taper, 1))
            pts += [sx0 + 4 + (sx1 - sx0 - 8) * ph, pic_bottom - 3 - amp * (pic_h - 6)]
        c.create_line(*pts, fill=zynthian_gui_config.color_tx, width=2, tags="bottom")
        button("buf-", "-", False, pm_w)
        btext = "Buf %d s" % buf
        c.create_text(x + buf_lbl_w / 2, (y0 + y1) / 2, text=btext,
                      font=self._fit_font(btext, buf_lbl_w - 4, bfs),
                      fill=zynthian_gui_config.color_tx, tags="bottom")
        x += buf_lbl_w + 4
        button("buf+", "+", False, pm_w)
        x += 4
        button("sync", "Sync %d" % bpm if sync else "Sync", sync, sync_w)
        # over the buffer
        ax0, ay0, ax1, _ = self._area()
        small = (zynthian_gui_config.font_family, max(7, fs - 3))
        info = []
        if sync and 0 <= rdiv < len(DIV_NAMES) and 0 <= odiv < len(DIV_NAMES):
            info.append("Rate %s · Offset %s" % (DIV_NAMES[rdiv], DIV_NAMES[odiv]))
        if minoff > 0:
            info.append("min Offset %d ms" % minoff)
        if info:
            c.create_text(ax0 + 6, ay0 + fs, text="   ".join(info), anchor="w", font=small,
                          fill=zynthian_gui_config.color_tx_off, tags="bottom")

    # ---------- mod page ----------
    def _lfo_rate(self, l):
        return min(max(self._zval("lfo%d_rate" % (l + 1), LFO_DEFAULT_HZ[l]), LFO_MIN_HZ), LFO_MAX_HZ)

    def _lfo_shape(self, l):
        return min(max(int(round(self._zval("lfo%d_shape" % (l + 1), 0))), 0), len(LFO_SHAPES) - 1)

    def _cell_sym(self, l, s, d):
        return "m%d_s%d_%s" % (l + 1, s, MOD_DSTS[d][0])

    @staticmethod
    def _shape_value(shape, ph):
        """The LFO's shape for the little picture (drift and S&H drawn as a fixed example)."""
        if shape == 1:
            return 4 * ph - 1 if ph < 0.5 else 3 - 4 * ph
        if shape == 2:
            return 0.7 * math.sin(2 * math.pi * ph) * math.cos(5 * ph)
        if shape == 3:
            return [0.6, -0.4, 0.9, -0.8, 0.1, 0.5, -0.6, 0.3][min(int(ph * 8), 7)]
        return math.sin(2 * math.pi * ph)

    def _mod_layout(self):
        """(lfo panels, grid) boxes inside the area below the top line."""
        hh = self._header_h()
        w, h = max(self.width, 80), max(self.height, 60)
        y0, y1 = hh + 6, h - 4
        left = self._content_x()
        split = left + int((w - left) * 0.43)
        return (left + 2, y0, split - 4, y1), (split + 4, y0, w - 2, y1)

    def _draw_mod(self):
        s = self._stream()
        cells = tuple(int(round(self._zval(self._cell_sym(l, s, d), 0))) for l in range(3) for d in range(len(MOD_DSTS)))
        lfos = tuple((round(self._lfo_rate(l), 3), self._lfo_shape(l),
                      int(round(self._zval("lfo%d_mode" % (l + 1), 0))),
                      int(round(self._zval("lfo%d_rate_mod" % (l + 1), 0))),
                      int(round(self._zval("lfo%d_smooth" % (l + 1), 0))),
                      int(round(self._zval("lfo%d_sync" % (l + 1), 0))),
                      min(max(int(round(self._zval("lfo%d_div" % (l + 1), 12))), 0), len(DIV_NAMES) - 1)) for l in range(2))
        quantized = 9 <= int(round(self._zval("interval", 0))) <= 13
        env_values = tuple(self._zval(key, default) for key, default in
                           (("env_attack", 0), ("env_release", 300), ("env_sensitivity", 0)))
        view = (self.width, self.height, s, cells, lfos, quantized, self.env_view, env_values)
        c = self.widget_canvas
        (lx0, ly0, lx1, ly1), (gx0, gy0, gx1, gy1) = self._mod_layout()
        fs = self._font_size()
        font = (zynthian_gui_config.font_family, max(7, fs - 1))
        small = (zynthian_gui_config.font_family, max(7, fs - 3))
        matrix_header = (zynthian_gui_config.font_family, max(6, fs - 5))
        matrix_value = (zynthian_gui_config.font_family, max(6, fs - 4))
        ph_h = (ly1 - ly0) / 2.0
        bh = min(self._btn_h(), max(28, int(ph_h * 0.28)))
        bfs = self._btn_font_size()
        if view != self.mod_view:
            self.mod_view = view
            c.delete("mod")
            self.mod_boxes = []
            self.lfo_dots = []
            if self.env_view:
                c.create_rectangle(lx0, ly0, lx1, ly1, outline=DIM, tags="mod")
                c.create_text(lx0 + 6, ly0 + bh / 2 + 3, text="Env · Input", anchor="w", font=font,
                              fill=LFO_COLORS[2], tags="mod")
                back_w = max(68, int((lx1 - lx0) * 0.34))
                back = (lx1 - back_w - 3, ly0 + 3, lx1 - 3, ly0 + bh + 3)
                c.create_rectangle(*back, outline=LFO_COLORS[0], tags="mod")
                c.create_text((back[0] + back[2]) / 2, (back[1] + back[3]) / 2,
                              text="LFO", font=self._fit_font("LFO", back_w - 8, bfs), fill=LFO_COLORS[0], tags="mod")
                self.mod_boxes.append((*back, "env_view", None))
                top = ly0 + bh + 10
                row_h = (ly1 - top - 8) / 3
                for e, label in enumerate(("Attack", "Release", "Sensitivity")):
                    box = (lx0 + 5, top + row_h * e, lx1 - 5, top + row_h * (e + 1) - 5)
                    c.create_rectangle(*box, outline=LFO_COLORS[2], fill="#202020", tags="mod")
                    value = ("%+d dB" % round(env_values[e])) if e == 2 else ("%d ms" % round(env_values[e]))
                    etext = label + "  " + value
                    c.create_text((box[0] + box[2]) / 2, (box[1] + box[3]) / 2,
                                  text=etext, font=self._fit_font(etext, box[2] - box[0] - 10, bfs),
                                  fill=LFO_COLORS[2], tags="mod")
                    self.mod_boxes.append((*box, "env_param", e))
            else:
                panel_w = lx1 - lx0 - 6
                for l in range(2):
                    py0 = ly0 + l * ph_h
                    py1 = py0 + ph_h - 6
                    col = LFO_COLORS[l]
                    c.create_rectangle(lx0, py0, lx1, py1, outline=DIM, tags="mod")
                    sync_on = bool(lfos[l][5])
                    gap = 3
                    uw = max(panel_w - 3 * gap, 80)
                    w_sync = int(uw * 0.24)
                    w_pol = int(uw * 0.16)
                    w_shp = int(uw * 0.28)
                    w_rate = uw - w_sync - w_pol - w_shp
                    bx = lx0 + 3
                    sync_box = (bx, py0 + 3, bx + w_sync, py0 + 3 + bh)
                    stxt = "Sync" if sync_on else "Free"
                    c.create_rectangle(*sync_box, outline=col, fill=col if sync_on else "", tags="mod")
                    c.create_text((sync_box[0] + sync_box[2]) / 2, (sync_box[1] + sync_box[3]) / 2,
                                  text=stxt, font=self._fit_font(stxt, w_sync - 4, bfs),
                                  fill="#000000" if sync_on else col, tags="mod")
                    self.mod_boxes.append((*sync_box, "lsync", l))
                    bx += w_sync + gap
                    positive = bool(lfos[l][2])
                    pol_box = (bx, py0 + 3, bx + w_pol, py0 + 3 + bh)
                    ptxt = "+" if positive else "−+"
                    c.create_rectangle(*pol_box, outline=col, fill=(col if positive else ""), tags="mod")
                    c.create_text((pol_box[0] + pol_box[2]) / 2, (pol_box[1] + pol_box[3]) / 2,
                                  text=ptxt, font=self._fit_font(ptxt, w_pol - 4, bfs),
                                  fill="#000000" if positive else col, tags="mod")
                    self.mod_boxes.append((*pol_box, "lon", l))
                    bx += w_pol + gap
                    shp_box = (bx, py0 + 3, bx + w_shp, py0 + 3 + bh)
                    shptxt = LFO_SHAPES[lfos[l][1]]
                    c.create_rectangle(*shp_box, outline=col, tags="mod")
                    c.create_text((shp_box[0] + shp_box[2]) / 2, (shp_box[1] + shp_box[3]) / 2,
                                  text=shptxt, font=self._fit_font(shptxt, w_shp - 4, bfs), fill=col, tags="mod")
                    self.mod_boxes.append((*shp_box, "lshape", l))
                    bx += w_shp + gap
                    hz = lfos[l][0]
                    rate_box = (bx, py0 + 3, lx1 - 3, py0 + 3 + bh)
                    rtxt = DIV_NAMES[lfos[l][6]] if sync_on else (("%.2f Hz" % hz) if hz < 10 else ("%.1f Hz" % hz))
                    c.create_rectangle(*rate_box, outline=col, fill="#202020", tags="mod")
                    c.create_text((rate_box[0] + rate_box[2]) / 2, (rate_box[1] + rate_box[3]) / 2,
                                  text=rtxt, font=self._fit_font(rtxt, w_rate - 4, bfs),
                                  fill=zynthian_gui_config.color_tx, tags="mod")
                    self.mod_boxes.append((*rate_box, "lrate", l))
                    # the shape
                    wy0, wy1 = py0 + bh + 8, py1 - bh - 8
                    mid, amp = (wy0 + wy1) / 2.0, max((wy1 - wy0) / 2.0, 2.0)
                    pts = []
                    smooth = lfos[l][4] / 100.0
                    follow = 1 - math.exp(-1 / (40 * 0.5 * smooth * smooth)) if smooth > 0 else 1
                    sv = 0.0
                    # Static preview only redrawn on parameter changes; warm up the filter first.
                    for i in range(-160, 41):
                        ph = (i % 40) / 40.0
                        sv += follow * (self._shape_value(lfos[l][1], ph) - sv)
                        if i < 0:
                            continue
                        shown = (sv + 1.0) * 0.5 if positive else sv
                        y = wy1 - (wy1 - wy0) * shown if positive else mid - amp * shown
                        pts += [lx0 + 8 + (lx1 - lx0 - 16) * i / 40.0, y]
                    c.create_line(*pts, fill=col, width=2, tags="mod")
                    zero_y = wy1 if positive else mid
                    c.create_line(lx0 + 8, zero_y, lx1 - 8, zero_y, fill=DIM, dash=(2, 3), tags="mod")
                    self.lfo_dots.append(c.create_oval(0, 0, 0, 0, fill=col, outline="", tags="mod"))
                    ry0 = py1 - bh - 3
                    split_x = (lx0 + lx1) / 2
                    depth_box = (lx0 + 3, ry0, split_x - 2, py1 - 3)
                    dtxt = "%d→Rate %+d%%" % (2 - l, lfos[l][3])
                    c.create_rectangle(*depth_box, outline=DIM, fill="#202020", tags="mod")
                    c.create_text((depth_box[0] + depth_box[2]) / 2, (ry0 + py1 - 3) / 2,
                                  text=dtxt, font=self._fit_font(dtxt, depth_box[2] - depth_box[0] - 6, bfs),
                                  fill=col, tags="mod")
                    self.mod_boxes.append((*depth_box, "lrate_mod", l))
                    smooth_box = (split_x + 2, ry0, lx1 - 3, py1 - 3)
                    smtxt = "Smooth %d%%" % lfos[l][4]
                    c.create_rectangle(*smooth_box, outline=DIM, fill="#202020", tags="mod")
                    c.create_text((smooth_box[0] + smooth_box[2]) / 2, (ry0 + py1 - 3) / 2,
                                  text=smtxt, font=self._fit_font(smtxt, smooth_box[2] - smooth_box[0] - 6, bfs),
                                  fill=col, tags="mod")
                    self.mod_boxes.append((*smooth_box, "lsmooth", l))
            # depth grid of the selected stream
            scol = STREAM_COLORS[s - 1]
            c.create_text(gx0, gy0 + bh / 2, text="Stream %d" % s, anchor="w", font=font, fill=scol, tags="mod")
            hdr = gy0 + bh + 4
            row_w = (gx1 - gx0) * 0.18
            cw = (gx1 - gx0 - row_w) / len(MOD_DSTS)
            for d, (_, name) in enumerate(MOD_DSTS):
                c.create_text(gx0 + row_w + cw * (d + 0.5), hdr + bh / 2, text=name, font=matrix_header,
                              fill=zynthian_gui_config.color_tx_off, tags="mod")
            hint_h = fs * 2 + 4
            gtop = hdr + bh + 2
            ch = (gy1 - hint_h - gtop) / 3.0
            for l in range(3):
                cy0 = gtop + l * ch
                if l == 2:
                    env_box = (gx0, cy0 + 2, gx0 + row_w - 2, cy0 + ch - 2)
                    c.create_rectangle(*env_box, outline=LFO_COLORS[2], tags="mod")
                    self.mod_boxes.append((*env_box, "env_view", None))
                label = "Env" if l == 2 else "LFO\n%d" % (l + 1)
                c.create_text(gx0 + (row_w - 2) / 2, cy0 + ch / 2, text=label, font=small,
                              justify="center", fill=LFO_COLORS[l], tags="mod")
                for d in range(len(MOD_DSTS)):
                    v = cells[l * len(MOD_DSTS) + d]
                    x0 = gx0 + row_w + cw * d + 2
                    k = abs(v) / 100.0
                    base = (77, 171, 247) if v > 0 else (255, 146, 43)
                    fill = "#%02x%02x%02x" % tuple(int(32 + (b - 32) * k) for b in base) if v else "#202020"
                    c.create_rectangle(x0, cy0 + 2, x0 + cw - 4, cy0 + ch - 2, outline=DIM, fill=fill, tags="mod")
                    c.create_text(x0 + (cw - 4) / 2, cy0 + ch / 2, text=("%+d" % v) if v else "0", font=matrix_value,
                                  fill=("#000000" if k > 0.6 else zynthian_gui_config.color_tx), tags="mod")
                    self.mod_boxes.append((x0, cy0 + 2, x0 + cw - 4, cy0 + ch - 2, "cell", (l, d)))
            c.create_text(gx0, gy1 - hint_h / 2, anchor="w", font=small, fill=zynthian_gui_config.color_tx_off,
                          text="100: Pitch ±%d st%s · Offset/Length ×4\nLevel: to silence · Env = input level" %
                               (24 if quantized else 2, " (Q)" if quantized else ""), tags="mod")
        # dots: the LFOs now (x: phase is not known, so the dot sits in the middle; y: the value)
        for l, dot in enumerate(self.lfo_dots):
            py0 = ly0 + l * ph_h
            py1 = py0 + ph_h - 6
            wy0, wy1 = py0 + bh + 8, py1 - bh - 8
            mid, amp = (wy0 + wy1) / 2.0, max((wy1 - wy0) / 2.0, 2.0)
            v = min(max(self._mon("lfo%d" % (l + 1), 0.0), -1.0), 1.0)
            if lfos[l][2]:
                v = min(max(v, 0.0), 1.0)
                y = wy1 - (wy1 - wy0) * v
            else:
                y = mid - amp * v
            x = lx1 - 14
            r = 5
            c.coords(dot, x - r, y - r, x + r, y + r)

    # ---------- touch ----------
    def _hit(self, event):
        hh = self._header_h()
        for pad in (0, 2):
            for i, (x0, y0, x1, y1) in enumerate(self.tab_boxes):
                if x0 - pad <= event.x <= x1 + pad and y0 - pad <= event.y <= y1 + pad:
                    return ("tab", i)
            if event.y <= hh + pad:
                if self.freeze_box and self.freeze_box[0] - pad <= event.x <= self.freeze_box[1] + pad:
                    return ("freeze", None)
                if self.mod_box and self.mod_box[0] - pad <= event.x <= self.mod_box[1] + pad:
                    return ("mod", None)
                if pad == 0:
                    continue
                return None
            for name, (x0, y0, x1, y1) in self.bottom_boxes.items():
                if x0 - pad <= event.x <= x1 + pad and y0 - pad <= event.y <= y1 + pad:
                    return (name, None)
            if self.view == 1:
                for x0, y0, x1, y1, kind, arg in self.mod_boxes:
                    if x0 - pad <= event.x <= x1 + pad and y0 - pad <= event.y <= y1 + pad:
                        return (kind, arg)
        return None

    def _drag_target(self, what):
        """(symbol, kind, reset value) of a value set by dragging, or None."""
        name, arg = what
        if name == "env_param":
            return (("env_attack", "env_time", 0), ("env_release", "env_time", 300),
                    ("env_sensitivity", "env_sens", 0))[arg]
        if name == "shape":
            return "shape", "shape_value", 0
        if name == "freeze":
            return "freeze_amount", "freeze_value", 0
        if name == "cell":
            return self._cell_sym(arg[0], self._stream(), arg[1]), "cell", 0
        if name == "lrate" and self._zval("lfo%d_sync" % (arg + 1), 0) >= 0.5:
            return "lfo%d_div" % (arg + 1), "note", 12
        if name == "lrate":
            return "lfo%d_rate" % (arg + 1), "rate", LFO_DEFAULT_HZ[arg]
        if name == "lsmooth":
            return "lfo%d_smooth" % (arg + 1), "smooth", 0
        if name == "lrate_mod":
            return "lfo%d_rate_mod" % (arg + 1), "depth", 0
        return None

    def _on_press(self, event):
        self.drag = None
        self.freeze_slid = False
        self.press = (self._hit(event), time.monotonic())
        target = self._drag_target(self.press[0]) if self.press[0] else None
        if not target:
            return
        sym, kind, reset = target
        if kind == "freeze_value":
            self.drag = (sym, self._zval(sym, reset), event.x, kind)
            self.last_tap = (None, 0.0)
            return
        now = time.monotonic()
        if self.last_tap[0] == sym and now - self.last_tap[1] < DOUBLE_TAP:
            self._zset(sym, reset)
            self.press = None
            self.last_tap = (None, 0.0)
            self.bottom_view = self.mod_view = None
            self.refresh_gui()
            return
        self.last_tap = (sym, now)
        start = event.y if kind in ("cell", "rate", "env_time", "env_sens", "note") else event.x
        self.drag = (sym, self._zval(sym, reset), start, kind)

    def _on_motion(self, event):
        if not self.drag:
            return
        sym, v0, start, kind = self.drag
        delta = start - event.y if kind in ("cell", "rate", "env_time", "env_sens", "note") else event.x - start  # up / right: more
        if kind in ("cell", "depth"):
            v = int(min(max(round(v0 + delta * CELL_PER_PX), -100), 100))
        elif kind == "note":
            v = min(max(round(v0 - delta / 12.0), 0), len(DIV_NAMES) - 1)
        elif kind == "smooth":
            v = min(max(round(v0 + delta * CELL_PER_PX), 0), 100)
        elif kind == "env_time":
            lo, hi = (0, 1000) if sym == "env_attack" else (10, 5000)
            v = min(max(round((v0 + 10) * 2 ** (delta / 24.0) - 10), lo), hi)
        elif kind == "env_sens":
            v = min(max(round(v0 + delta / 3.0), -24), 24)
        elif kind == "shape_value":
            v = min(max(round(v0 + delta / 100.0, 2), 0), 1)
        elif kind == "freeze_value":
            if not self.freeze_slid and abs(delta) < 6:
                return
            self.freeze_slid = True
            v = float(min(max(round(v0 + delta * CELL_PER_PX), 0), 100))
        else:
            v = min(max(v0 * 2 ** (delta * RATE_OCT_PER_PX), LFO_MIN_HZ), LFO_MAX_HZ)
            v = round(v, 2 if v < 10 else 1)
        if self._zset(sym, v):
            if kind == "freeze_value":
                self.top_view = None
                self._draw_top()
            elif kind == "shape_value":
                self.bottom_view = None
                self._draw_bottom()
            else:
                self.mod_view = None
                self._draw_mod()

    def _on_release(self, event):
        if not self.press:
            return
        what, t0 = self.press
        self.press = None
        self.drag = None
        if self.freeze_slid:
            self.freeze_slid = False
            return
        if not what or what != self._hit(event):
            return
        name, i = what
        if name == "tab":
            if time.monotonic() - t0 >= LONG_PRESS:
                on = bool(self._on_mask() & (1 << i))
                self._zset("s%d_on" % (i + 1), 0 if on else 1)
            else:
                self._zset("stream", i + 1)
                self._show_grain_page()
        elif name == "mod":
            self.view = 0 if self.view == 1 else 1
            self.drawn = None
        elif name == "lsync":
            sym = "lfo%d_sync" % (i + 1)
            self._zset(sym, 0 if self._zval(sym, 0) >= 0.5 else 1)
            self.mod_view = None
        elif name == "env_view":
            self.env_view = not self.env_view
            self.mod_view = None
        elif name == "lshape":
            self._zset("lfo%d_shape" % (i + 1), (self._lfo_shape(i) + 1) % len(LFO_SHAPES))
            self.mod_view = None
        elif name == "freeze":
            self._zset("freeze", 0 if self._zval("freeze", 0) >= 0.5 else 1)
        elif name == "sync":
            self._zset("sync", 0 if self._zval("sync", 0) >= 0.5 else 1)
        elif name == "lon":
            sym = "lfo%d_mode" % (i + 1)
            self._zset(sym, 0 if self._zval(sym, 0) >= 0.5 else 1)
        elif name in ("buf-", "buf+"):
            b = self._buffer_s() + (1 if name == "buf+" else -1)
            self._zset("buffer", min(max(b, BUFFER_MIN), BUFFER_MAX))
        self.top_view = self.bottom_view = None
        self.refresh_gui()
