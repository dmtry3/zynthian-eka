#!/usr/bin/python3
# Runs the Eka Grain Flow widget logic without Zynthian and without Tk: stub modules for zyngui and tkinter
# (the canvas only records items), and a fake plugin that behaves like eka_grainflow.c for the selected stream.
#   python3 test/widget_check.py
import importlib.util
import os
import sys
import time
import types

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
fails = 0


def check(cond, msg):
    global fails
    if not cond:
        fails += 1
        print("FAIL:", msg)


# ---------- stub tkinter ----------
class Canvas:
    def __init__(self, *a, **k):
        self.items, self.n = {}, 0

    def _new(self, kind, coords, k):
        self.n += 1
        tags = k.get("tags", ())
        self.items[self.n] = {"kind": kind, "coords": coords, "tags": (tags,) if isinstance(tags, str) else tags, **k}
        return self.n

    def create_rectangle(self, *c, **k): return self._new("rect", c, k)
    def create_line(self, *c, **k): return self._new("line", c, k)
    def create_text(self, *c, **k): return self._new("text", c, k)
    def create_oval(self, *c, **k): return self._new("oval", c, k)

    def _ids(self, tag):
        if isinstance(tag, int):
            return [tag]
        if tag == "all":
            return list(self.items)
        return [i for i, it in self.items.items() if tag in it["tags"]]

    def delete(self, tag):
        for i in self._ids(tag):
            del self.items[i]

    def coords(self, tag, *c):
        for i in self._ids(tag):
            self.items[i]["coords"] = c

    def itemconfig(self, tag, **k):
        for i in self._ids(tag):
            self.items[i].update(k)

    def tag_raise(self, *a): pass
    def tag_lower(self, *a): pass
    def grid(self, *a, **k): pass
    def bind(self, *a, **k): pass


class Frame:
    def __init__(self, *a, **k): pass


tk = types.ModuleType("tkinter")
tk.Canvas, tk.Frame = Canvas, Frame
tk.font = types.ModuleType("tkinter.font")
sys.modules["tkinter"], sys.modules["tkinter.font"] = tk, tk.font

cfg = types.ModuleType("zyngui.zynthian_gui_config")
cfg.font_family, cfg.color_bg, cfg.color_tx, cfg.color_tx_off = "DejaVu Sans", "#000000", "#ffffff", "#707070"


class WidgetBase(Frame):
    def __init__(self, parent):
        self.zyngui_control = parent
        self.width, self.height = 800, 300
        self.processor = None
        self.monitors = None

    def update(self):
        self.monitors = self.processor.plugin.monitors()
        self.refresh_gui()

    def on_size(self, event):
        return True


base = types.ModuleType("zyngui.zynthian_widget_base")
base.zynthian_widget_base = WidgetBase
zyngui = types.ModuleType("zyngui")
zyngui.zynthian_gui_config, zyngui.zynthian_widget_base = cfg, base
sys.modules.update({"zyngui": zyngui, "zyngui.zynthian_gui_config": cfg, "zyngui.zynthian_widget_base": base})

spec = importlib.util.spec_from_file_location("w", os.path.join(ROOT, "ui", "zynthian_widget_eka_grain_flow.py"))
W = importlib.util.module_from_spec(spec)
spec.loader.exec_module(W)


# ---------- fake plugin and Zynthian ----------
DEFAULTS = [[100, 100, 500, 12, 0, 0, 0, 0, 0, 0, 0, 0],
            [250, 120, 400, 0, 0, 0, 0, -3, 0, 0, 0, 50],
            [400, 150, 600, -12, 0, 0, 0, -3, 0, 0, 0, 50]]


class Plugin:
    """Like eka_grainflow.c: knob turns go into the selected stream; a new selection copies nothing."""
    def __init__(self, ctl):
        self.ctl = ctl
        self.q = [list(d) for d in DEFAULTS]
        self.on = [1, 0, 0]
        self.sel = None
        self.face = None
        self.loads = 0

    def run(self):
        sel = int(self.ctl["stream"].value) - 1
        face = [self.ctl[s].value for s in W.FACES]
        if self.sel != sel or self.face is None:
            self.sel, self.face = sel, face
        else:
            for v in range(len(W.FACES)):
                if face[v] != self.face[v]:
                    self.q[sel][v] = face[v]
                    self.ctl["s%d_%s" % (sel + 1, W.FACES[v])].value = face[v]
            self.face = face
        for i in range(3):
            self.on[i] = int(self.ctl["s%d_on" % (i + 1)].value)

    def monitors(self):
        self.run()
        sync = self.ctl["sync"].value >= 0.5
        m = {"lfo1": 0.5, "lfo2": -0.3, "env": 0.7, "bpm": 120, "rate_div": 6 if sync else -1,
             "offset_div": 9 if sync else -1, "peak": 0.5, "active": 3, "loads": self.loads, "min_offset": 3.0, "sel": self.sel + 1,
             "on_mask": sum(1 << i for i in range(3) if self.on[i])}
        for v, s in enumerate(W.FACES):
            m["cur_" + s] = self.q[self.sel][v]
        for k in range(16):
            m["grain%d" % (k + 1)] = (k % 3) + 0.01 * k if k < 5 else -1
        return m


class Zctrl:
    def __init__(self, v):
        self.value = v

    def set_value(self, v):
        self.value = v


class Processor:
    pass


class Control:
    def __init__(self):
        self.screen_title = "Main"
        self.selected = None
        self.list_data = []
        self.screen_info = None

    def select(self, i):
        self.selected = i


ctl = {"stream": Zctrl(1), "mix": Zctrl(50), "feedback": Zctrl(0), "freeze": Zctrl(0), "buffer": Zctrl(4),
       "shape": Zctrl(0), "sync": Zctrl(0),
       "env_attack": Zctrl(0), "env_release": Zctrl(300), "env_sensitivity": Zctrl(0)}
for v, s in enumerate(W.FACES):
    ctl[s] = Zctrl(DEFAULTS[0][v])
for i in range(3):
    ctl["s%d_on" % (i + 1)] = Zctrl(1 if i == 0 else 0)
    ctl["s%d_fine" % (i + 1)] = Zctrl(0.0)
    ctl["s%d_spread" % (i + 1)] = Zctrl(0.0)
    ctl["s%d_interval" % (i + 1)] = Zctrl(0)
    ctl["s%d_scan" % (i + 1)] = Zctrl(0)
    for l in range(3):
        for d in ("pitch", "offset", "length", "level", "filter", "scan"):
            ctl["m%d_s%d_%s" % (l + 1, i + 1, d)] = Zctrl(0)
    for v, s in enumerate(W.FACES):
        ctl["s%d_%s" % (i + 1, s)] = Zctrl(DEFAULTS[i][v])
for l, (hz, shp) in enumerate(((0.3, 0), (1.0, 2))):
    ctl["lfo%d_rate" % (l + 1)] = Zctrl(hz)
    ctl["lfo%d_shape" % (l + 1)] = Zctrl(shp)
    ctl["lfo%d_mode" % (l + 1)] = Zctrl(0)
    ctl["lfo%d_rate_mod" % (l + 1)] = Zctrl(0)
    ctl["lfo%d_smooth" % (l + 1)] = Zctrl(0)
    ctl["lfo%d_sync" % (l + 1)] = Zctrl(0)
    ctl["lfo%d_div" % (l + 1)] = Zctrl(12)
for sym, value in (("freeze_amount", 0),):
    ctl[sym] = Zctrl(value)
proc = Processor()
proc.controllers_dict = ctl
proc.plugin = Plugin(ctl)
control = Control()
control.list_data = [("g_main", 0, 0, proc, 0), ("g_grain", 0, 0, proc, 0), ("g_flow", 0, 0, proc, 0), ("g_out", 0, 0, proc, 0)]
control.screen_info = control.list_data[0]

w = W.zynthian_widget_eka_grain_flow(control)
w.processor = proc


def tick(n=1):
    for _ in range(n):
        w.next_update = 0
        w.update()


class Ev:
    def __init__(self, x, y):
        self.x, self.y = x, y


def tap(box_x, box_y, hold=0.0):
    e = Ev(box_x, box_y)
    w._on_press(e)
    if hold:
        w.press = (w.press[0], w.press[1] - hold)
    w._on_release(e)


tick(2)
check(len(w.tab_boxes) == 3, "three tabs drawn")
check([row[3] for row in DEFAULTS] == [12, 0, -12], "Init Pitch values: +12, 0, -12")
check(all(b[3] - b[1] > b[2] - b[0] for b in w.tab_boxes), "stream buttons are taller than they are wide")
check(w.tab_boxes[0][0] == w.tab_boxes[1][0] == w.tab_boxes[2][0] and
      w.tab_boxes[0][3] < w.tab_boxes[1][1] < w.tab_boxes[2][1], "stream buttons form a left column")
check(w._area()[0] > w.tab_boxes[0][2], "buffer starts to the right of the stream column")
tab_ids = w.widget_canvas._ids("tabs")
w.monitors["active"] = 4
w._draw_top()
check(w.widget_canvas._ids("tabs") == tab_ids, "changing grain count does not redraw the stream column")
lh = w._line_h()

# a turn on stream 1, then select stream 2 by a tap: the knobs show stream 2, stream 1 keeps its turn
ctl["pitch"].value = 7
tick()
check(proc.plugin.q[0][3] == 7, "knob turn went into stream 1")
x0, y0, x1, y1 = w.tab_boxes[1]
tap((x0 + x1) / 2, (y0 + y1) / 2)
check(ctl["stream"].value == 2, "tap on tab 2 selects stream 2")
check(control.selected == 1, "and shows the Grain page")
tick(2)
check(ctl["pitch"].value == 0 and ctl["offset"].value == 250, "knobs moved to stream 2: %s" %
      [ctl[s].value for s in W.FACES])
check(proc.plugin.q[0][3] == 7 and proc.plugin.q[1][3] == 0, "stream 1 keeps pitch 7, stream 2 has 0")

# the Stream knob (not the widget) goes back to 1: the knobs follow
ctl["stream"].value = 1
tick(2)
check(ctl["pitch"].value == 7 and ctl["offset"].value == 100, "Stream knob back to 1: knobs show stream 1")

# a preset load (loads counts up) changes stream 1 in the plugin: the knobs follow
proc.plugin.q[0][2] = 1234
proc.plugin.loads += 1
proc.plugin.face = None
tick(2)
check(ctl["length"].value == 1234, "after a preset load the knobs show the loaded values")

# long press on tab 3 turns stream 3 on; Freeze; Buffer +; Sharp
x0, y0, x1, y1 = w.tab_boxes[2]
tap((x0 + x1) / 2, (y0 + y1) / 2, hold=1.0)
check(ctl["s3_on"].value == 1 and ctl["stream"].value == 1, "long press turns stream 3 on, selection stays")
tick()
check(proc.plugin.on[2] == 1, "plugin sees stream 3 on")
fx0, fx1 = w.freeze_box
tap((fx0 + fx1) / 2, lh / 2)
check(ctl["freeze"].value == 1, "Freeze button")
tick()
bx = w.bottom_boxes["buf+"]
tap((bx[0] + bx[2]) / 2, (bx[1] + bx[3]) / 2)
check(ctl["buffer"].value == 5, "Buffer +")
bx = w.bottom_boxes["shape"]
sx, sy = (bx[0] + bx[2]) / 2, (bx[1] + bx[3]) / 2
w._on_press(Ev(sx, sy)); w._on_motion(Ev(sx + 50, sy)); w._on_release(Ev(sx + 50, sy))
check(ctl["shape"].value == 0.5, "Shape drag reaches intermediate envelope")
tick()
bx = w.bottom_boxes["sync"]
tap((bx[0] + bx[2]) / 2, (bx[1] + bx[3]) / 2)
check(ctl["sync"].value == 1, "Sync on")
tick()
check(any("Rate 1/16 · Offset 1/8" in str(w.widget_canvas.items[i].get("text")) for i in w.widget_canvas.items),
      "Sync shows the note values")
# Flow values use the same selected-stream synchronization as Grain and Out.
check(set(w.bottom_boxes) == {"shape", "buf-", "buf+", "sync"}, "widget footer contains global controls")
control.screen_info = control.list_data[2]  # Flow is already open
control.selected = None
ctl["scan"].value = 25
ctl["fine"].value = 10
ctl["interval"].value = 2
ctl["spread"].value = 70
tick()
check(proc.plugin.q[0][8:] == [25, 10, 2, 70], "all four Flow encoders update the selected stream")
x0, y0, x1, y1 = w.tab_boxes[1]
tap((x0 + x1) / 2, (y0 + y1) / 2)
tick(2)
check(control.selected is None, "selecting a stream keeps the Flow page open")
check([ctl[s].value for s in W.FACES[8:]] == [0, 0, 0, 50], "Flow encoders show stream 2 values")
check(proc.plugin.q[0][8:] == [25, 10, 2, 70], "stale Flow encoders did not overwrite stream 1")
ctl["scan"].value = -15
ctl["fine"].value = -8
ctl["interval"].value = 3
ctl["spread"].value = 90
tick()
ctl["stream"].value = 1
tick(2)
check([ctl[s].value for s in W.FACES[8:]] == [25, 10, 2, 70], "switching back restores stream 1 Flow values")
check(proc.plugin.q[1][8:] == [-15, -8, 3, 90], "stream 2 keeps its own Flow values")
# Loading State while Flow is open must refresh these controls too.
proc.plugin.q[0][8:] = [40, 5, 1, 60]
proc.plugin.loads += 1
proc.plugin.face = None
tick(2)
check([ctl[s].value for s in W.FACES[8:]] == [40, 5, 1, 60], "preset restore refreshes all Flow encoders")

# Mod page: open it, change LFO 2's shape, drag a cell of the selected stream (1), drag LFO 1's rate
mx0, mx1 = w.mod_box
tap((mx0 + mx1) / 2, lh / 2)
tick()
check(w.view == 1 and len(w.mod_boxes) == 2 * 6 + 18 + 1, "mod page with LFO controls and 18 cells: %d" % len(w.mod_boxes))
check(w._mod_layout()[0][0] > w.tab_boxes[0][2], "Mod controls leave room for the stream column")
eb = [b for b in w.mod_boxes if b[4] == "env_view"][0]
tap((eb[0] + eb[2]) / 2, (eb[1] + eb[3]) / 2)
check(w.env_view and len([b for b in w.mod_boxes if b[4] == "env_param"]) == 3, "Env controls open from matrix")
eb = [b for b in w.mod_boxes if b[4] == "env_param" and b[5] == 0][0]
ex, ey = (eb[0] + eb[2]) / 2, (eb[1] + eb[3]) / 2
w.last_tap = (None, 0)
w._on_press(Ev(ex, ey)); w._on_motion(Ev(ex, ey - 24)); w._on_release(Ev(ex, ey - 24))
check(ctl["env_attack"].value == 10, "Env attack changes vertically")
eb = [b for b in w.mod_boxes if b[4] == "env_view" and b[0] < w._mod_layout()[0][2]][0]
tap((eb[0] + eb[2]) / 2, (eb[1] + eb[3]) / 2)
check(not w.env_view and len(w.lfo_dots) == 2, "return to LFO controls")
box = [b for b in w.mod_boxes if b[4] == "lshape" and b[5] == 1][0]
tap((box[0] + box[2]) / 2, (box[1] + box[3]) / 2)
check(ctl["lfo2_shape"].value == 3, "LFO 2 shape Drift -> S&H")
box = [b for b in w.mod_boxes if b[4] == "lon" and b[5] == 0][0]
tap((box[0] + box[2]) / 2, (box[1] + box[3]) / 2)
check(ctl["lfo1_mode"].value == 1, "LFO 1 changed to positive mode")
box = [b for b in w.mod_boxes if b[4] == "cell" and b[5] == (0, 0)][0]
cx, cy = (box[0] + box[2]) / 2, (box[1] + box[3]) / 2
w.last_tap = (None, 0)
w._on_press(Ev(cx, cy))
w._on_motion(Ev(cx + 30, cy))
check(ctl["m1_s1_pitch"].value == 0, "horizontal motion does not change a modulation cell")
w._on_motion(Ev(cx, cy - 30))
check(ctl["m1_s1_pitch"].value == 20 and ctl["m1_s2_pitch"].value == 0, "LFO 1 -> stream 1 Pitch: %s" % ctl["m1_s1_pitch"].value)
w._on_motion(Ev(cx, cy + 30))
check(ctl["m1_s1_pitch"].value == -20, "downward motion decreases modulation depth")
w._on_release(Ev(cx, cy + 30))
box = [b for b in w.mod_boxes if b[4] == "lrate" and b[5] == 0][0]
cx, cy = (box[0] + box[2]) / 2, (box[1] + box[3]) / 2
w.last_tap = (None, 0)
w._on_press(Ev(cx, cy)); w._on_motion(Ev(cx, cy - 18)); w._on_release(Ev(cx, cy - 18))
check(abs(ctl["lfo1_rate"].value - 0.6) < 0.011, "LFO 1 rate doubled: %s" % ctl["lfo1_rate"].value)
w.last_tap = (None, 0)
w._on_press(Ev(cx, cy)); w._on_release(Ev(cx, cy)); w._on_press(Ev(cx, cy)); w._on_release(Ev(cx, cy))
check(ctl["lfo1_rate"].value == 0.3, "double tap resets the rate")
box = [b for b in w.mod_boxes if b[4] == "lsmooth" and b[5] == 0][0]
sx, sy = (box[0] + box[2]) / 2, (box[1] + box[3]) / 2
w.last_tap = (None, 0)
w._on_press(Ev(sx, sy)); w._on_motion(Ev(sx + 60, sy)); w._on_release(Ev(sx + 60, sy))
check(ctl["lfo1_smooth"].value == 40, "Smooth adjusts independently")
box = [b for b in w.mod_boxes if b[4] == "lsync" and b[5] == 0][0]
tap((box[0] + box[2]) / 2, (box[1] + box[3]) / 2)
check(ctl["lfo1_sync"].value == 1 and ctl["lfo2_sync"].value == 0, "independent LFO Sync toggle")
box = [b for b in w.mod_boxes if b[4] == "lrate" and b[5] == 0][0]
sx, sy = (box[0] + box[2]) / 2, (box[1] + box[3]) / 2
w.last_tap = (None, 0)
w._on_press(Ev(sx, sy)); w._on_motion(Ev(sx, sy - 36)); w._on_release(Ev(sx, sy - 36))
check(ctl["lfo1_div"].value == 9 and ctl["lfo1_rate"].value == 0.3, "Sync selects 1/8 while preserving free Hz")
box = [b for b in w.mod_boxes if b[4] == "lsync" and b[5] == 0][0]
tap((box[0] + box[2]) / 2, (box[1] + box[3]) / 2)
check(ctl["lfo1_sync"].value == 0, "return to free LFO rate")
box = [b for b in w.mod_boxes if b[4] == "cell" and b[5] == (0, 5)][0]
sx, sy = (box[0] + box[2]) / 2, (box[1] + box[3]) / 2
w.last_tap = (None, 0)
w._on_press(Ev(sx, sy)); w._on_motion(Ev(sx, sy - 30)); w._on_release(Ev(sx, sy - 30))
check(ctl["m1_s1_scan"].value == 20 and ctl["m1_s2_scan"].value == 0, "Scan matrix routes to selected stream")
box = [b for b in w.mod_boxes if b[4] == "lrate_mod" and b[5] == 0][0]
cx, cy = (box[0] + box[2]) / 2, (box[1] + box[3]) / 2
w.last_tap = (None, 0)
w._on_press(Ev(cx, cy)); w._on_motion(Ev(cx + 60, cy)); w._on_release(Ev(cx + 60, cy))
check(ctl["lfo1_rate_mod"].value == 40, "drag changes LFO 2 -> LFO 1 rate depth")
w.last_tap = (None, 0)
w._on_press(Ev(cx, cy)); w._on_release(Ev(cx, cy)); w._on_press(Ev(cx, cy)); w._on_release(Ev(cx, cy))
check(ctl["lfo1_rate_mod"].value == 0, "double tap removes the rate modulation")
ctl["stream"].value = 2
tick(2)
check(any(w.widget_canvas.items[i].get("text") == "Stream 2" for i in w.widget_canvas.items), "grid follows the stream")
tap((mx0 + mx1) / 2, lh / 2)
tick()
check(w.view == 0 and len(w.grain_items) == 16, "back to the buffer")
ctl["interval"].value = 9
w.view = 1; w.drawn = None
w.refresh_gui()
check(any("±24 st (Q)" in str(it.get("text", "")) for it in w.widget_canvas.items.values()),
      "quantized Intervals show the wider Pitch modulation span")
w.view = 0; w.drawn = None
w.refresh_gui()

# Freeze: horizontal slide changes Amount, a tap independently toggles full capture.
fx0, fx1 = w.freeze_box
cx, cy = (fx0 + fx1) / 2, lh / 2
before = ctl["freeze"].value
w._on_press(Ev(cx, cy)); w._on_motion(Ev(cx + 60, cy)); w._on_release(Ev(cx + 60, cy))
check(ctl["freeze_amount"].value == 40 and ctl["freeze"].value == before,
      "slide on Freeze changes Amount without toggling capture, even when released outside")
check(any(it.get("text") == "Frz 40%" for it in w.widget_canvas.items.values()), "short Freeze label shows Amount")
tap(cx, cy)
check(ctl["freeze"].value == 0 and ctl["freeze_amount"].value == 40, "tap releases to the saved Amount")
tap(cx, cy)
check(ctl["freeze"].value == 1 and ctl["freeze_amount"].value == 40, "rapid second tap captures without resetting Amount")
w._on_press(Ev(cx, cy)); w._on_motion(Ev(cx - 300, cy)); w._on_release(Ev(cx - 300, cy))
check(ctl["freeze_amount"].value == 0 and ctl["freeze"].value == 1, "slide clamps at 0 without changing the button")
w._on_press(Ev(cx, cy)); w._on_motion(Ev(cx + 300, cy)); w._on_motion(Ev(cx, cy)); w._on_release(Ev(cx, cy))
check(ctl["freeze_amount"].value == 0 and ctl["freeze"].value == 1, "slide back to origin still does not count as a tap")
w._on_press(Ev(cx, cy)); w._on_motion(Ev(cx + 3, cy)); w._on_release(Ev(cx + 3, cy))
check(ctl["freeze"].value == 0 and ctl["freeze_amount"].value == 0, "small touch jitter is a tap")
w._on_press(Ev(cx, cy))
w.press = (w.press[0], time.monotonic() - W.LONG_PRESS - 0.1)
w._on_release(Ev(cx, cy))
check(w.view == 0 and ctl["freeze"].value == 1, "long press opens no menu")

for width, height in ((800, 480), (480, 240), (800, 300)):
    w.width, w.height = width, height
    w.drawn = None
    w.refresh_gui()
    check(all(b[0] >= 0 and b[3] <= height and b[3] - b[1] > b[2] - b[0] for b in w.tab_boxes),
          "vertical stream buttons fit %dx%d" % (width, height))
    check(w.freeze_box[0] > w.tab_boxes[0][2] and w._area()[0] > w.tab_boxes[0][2],
          "main controls stay to the right of stream buttons at %dx%d" % (width, height))

shown = [i for i in w.grain_items if w.widget_canvas.items[i].get("state") == "normal"]
check(len(shown) == 5, "five grains shown, got %d" % len(shown))
t0 = time.monotonic()
tick(50)
check(time.monotonic() - t0 < 2.0, "50 refreshes take %.2f s" % (time.monotonic() - t0))

print("%s: widget, %d failed" % ("FAIL" if fails else "OK", fails))
sys.exit(1 if fails else 0)
