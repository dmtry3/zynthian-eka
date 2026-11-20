#!/usr/bin/python3
# Runs the Eka Pulse widget logic without Zynthian and without Tk.
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

spec = importlib.util.spec_from_file_location("w", os.path.join(ROOT, "ui", "zynthian_widget_eka_pulse.py"))
W = importlib.util.module_from_spec(spec)
spec.loader.exec_module(W)


DEFAULTS = [
    [0, 50, 50, 100, 20, 2, 16, 0, 0, -24, 0, 10],
    [0, 50, 50, 100, 21, 2, 5, 0, 0, -24, 0, 10],
    [0, 50, 30, 100, 22, 2, 7, 0, 0, -24, 0, 10],
    [0, 0, 50, 100, 2, 2, 8, 0, 0, -24, 0, 10],
]


class Plugin:
    def __init__(self, ctl):
        self.ctl = ctl
        self.q = [list(d) for d in DEFAULTS]
        self.on = [0, 0, 0, 1]
        self.sel = None
        self.face = None
        self.loads = 0

    def run(self):
        sel = int(self.ctl["slot"].value) - 1
        face = [self.ctl[s].value for s in W.FACES]
        if self.sel != sel or self.face is None:
            self.sel, self.face = sel, face
        else:
            for v in range(len(W.FACES)):
                if face[v] != self.face[v]:
                    self.q[sel][v] = face[v]
                    self.ctl["s%d_%s" % (sel + 1, W.FACES[v])].value = face[v]
            self.face = face
        for s in range(4):
            self.on[s] = int(self.ctl["s%d_on" % (s + 1)].value)

    def monitors(self):
        self.run()
        m = {
            "peak": 0.6, "out_peak": 0.5, "lim_gr": 0.0, "loads": self.loads,
            "bpm": 120, "sel": self.sel + 1,
            "on_mask": sum(1 << i for i in range(4) if self.on[i]),
        }
        for s in range(4):
            m["phase%d" % (s + 1)] = 0.25 * (s + 1) % 1.0
            m["val%d" % (s + 1)] = 0.5
            m["steps%d" % (s + 1)] = self.q[s][6]
            m["mode%d" % (s + 1)] = self.q[s][0]
        for v, sym in enumerate(W.FACES):
            m["cur_" + sym] = self.q[self.sel][v]
        return m


class Zctrl:
    def __init__(self, v):
        self.value = v
        self.options = {}

    def set_value(self, v):
        self.value = v

    def set_options(self, opts):
        self.options.update(opts)


class Processor:
    pass


class Control:
    def __init__(self):
        self.selected = None
        self.list_data = []
        self.screen_info = None

    def select(self, i):
        self.selected = i


ctl = {
    "slot": Zctrl(4), "mix": Zctrl(100), "low_split": Zctrl(0), "scene": Zctrl(0),
    "low_duck": Zctrl(0), "live_pad": Zctrl(0),
    "xy_x": Zctrl(50.0), "xy_y": Zctrl(50.0), "xy_on": Zctrl(0), "store_scene": Zctrl(0),
}
for v, sym in enumerate(W.FACES):
    ctl[sym] = Zctrl(DEFAULTS[3][v])
for s in range(4):
    ctl["s%d_on" % (s + 1)] = Zctrl(1 if s == 3 else 0)
    for v, sym in enumerate(W.FACES):
        ctl["s%d_%s" % (s + 1, sym)] = Zctrl(DEFAULTS[s][v])
    for k in range(16):
        ctl["s%d_lvl%d" % (s + 1, k + 1)] = Zctrl(100.0 if k % 2 == 0 else 0.0)
        ctl["s%d_shp%d" % (s + 1, k + 1)] = Zctrl(1)

proc = Processor()
proc.controllers_dict = ctl
proc.plugin = Plugin(ctl)
control = Control()
control.list_data = [
    ("g_main", 0, 0, proc, 0),
    ("g_fx", 0, 0, proc, 0),
    ("g_curve", 0, 0, proc, 0),
    ("g_trigger", 0, 0, proc, 0),
    ("g_perform", 0, 0, proc, 0),
]
control.screen_info = control.list_data[0]

w = W.zynthian_widget_eka_pulse(control)
w.processor = proc


class Ev:
    def __init__(self, x, y):
        self.x, self.y = x, y


def tick(n=1):
    for _ in range(n):
        w.next_update = 0
        w.update()


def tap(x, y, hold=0.0):
    e = Ev(x, y)
    w._on_press(e)
    if hold and w.press:
        w.press = (w.press[0], w.press[1], w.press[2] - hold, w.press[3], w.press[4])
    w._on_release(e)


tick(2)
check(len(w.slot_boxes) == 4, "4 slot cards drawn")
check(len(w.step_boxes) == 8, "slot 4 shows 8 steps: got %d" % len(w.step_boxes))
check(len(w.tab_boxes) == 4, "4 top tabs drawn (Steps, Perform, Flow, ?)")
check(ctl["mode"].options.get("name") == "Gate Mode" and ctl["param1"].options.get("name") == "Floor %",
      "slot 4 sets dynamic encoder names: %s / %s" % (ctl["mode"].options.get("name"), ctl["param1"].options.get("name")))

# Tap on slot 3 selects slot 3 and switches page to FX, syncing steps to 7
x0, y0, x1, y1 = w.slot_boxes[2]
tap((x0 + x1) / 2, (y0 + y1) / 2)
check(ctl["slot"].value == 3, "tap on card 3 selects slot 3")
check(control.selected == 1, "switches to FX page")
tick(2)
check(ctl["steps"].value == 7, "steps knob synced to slot 3 (7)")
check(len(w.step_boxes) == 7, "7 step bars drawn for slot 3")
check(ctl["mode"].options.get("name") == "Filter Mode" and ctl["param1"].options.get("name") == "Cutoff",
      "slot 3 sets Filter encoder names: %s / %s" % (ctl["mode"].options.get("name"), ctl["param1"].options.get("name")))

# Long press on slot 3 toggles s3_on
tap((x0 + x1) / 2, (y0 + y1) / 2, hold=0.8)
check(ctl["s3_on"].value == 1, "long press enables slot 3")
tick()
check(proc.plugin.on[2] == 1, "plugin sees slot 3 enabled")

# Swipe on step bars changes step level
sb = w.step_boxes[1]
sx, sy = (sb[0] + sb[2]) / 2, (sb[1] + sb[3]) / 2
w._on_press(Ev(sx, sy))
w._on_release(Ev(sx, sy))
check(40.0 <= ctl["s3_lvl2"].value <= 60.0, "touch on middle of step 2 sets ~50%%: %s" % ctl["s3_lvl2"].value)

# Tap on shape cell cycles shape 1 -> 2
shb = w.shape_boxes[1]
tap((shb[0] + shb[2]) / 2, (shb[1] + shb[3]) / 2)
check(ctl["s3_shp2"].value == 2, "shape tap cycled 1 -> 2")

# Inv button inverts levels
before = ctl["s3_lvl1"].value
ib = w.action_boxes["inv"]
tap((ib[0] + ib[2]) / 2, (ib[1] + ib[3]) / 2)
check(ctl["s3_lvl1"].value == 100.0 - before, "Inv button inverted step 1")

# Switch tabs Perform / Flow / Steps and test Perform + Flow controls
tb = w.tab_boxes[1]
tap((tb[0] + tb[2]) / 2, (tb[1] + tb[3]) / 2)
check(w.view == 1 and len(w.pad_boxes) == 9 and w.xy_box is not None, "Perform tab has 9 pads + XY pad")

# Hold TapeStop pad -> live_pad = 1, release -> live_pad = 0
pb0 = w.pad_boxes[0]
w._on_press(Ev((pb0[0] + pb0[2]) / 2, (pb0[1] + pb0[3]) / 2))
check(ctl["live_pad"].value == 1, "holding TapeStop sets live_pad=1")
w._on_release(Ev((pb0[0] + pb0[2]) / 2, (pb0[1] + pb0[3]) / 2))
check(ctl["live_pad"].value == 0, "releasing TapeStop resets live_pad=0")

# Hold Bypass pad -> mix = 0, release -> restores mix = 100
pb8 = w.pad_boxes[8]
w._on_press(Ev((pb8[0] + pb8[2]) / 2, (pb8[1] + pb8[3]) / 2))
check(ctl["mix"].value == 0.0, "holding Bypass sets mix=0")
w._on_release(Ev((pb8[0] + pb8[2]) / 2, (pb8[1] + pb8[3]) / 2))
check(ctl["mix"].value == 100.0, "releasing Bypass restores mix=100")

# Drag XY pad -> xy_on=1, release -> xy_on=0
xb = w.xy_box
w._on_press(Ev((xb[0] + xb[2]) / 2, (xb[1] + xb[3]) / 2))
check(ctl["xy_on"].value == 1, "XY press sets xy_on=1")
w._on_release(Ev((xb[0] + xb[2]) / 2, (xb[1] + xb[3]) / 2))
check(ctl["xy_on"].value == 0, "XY release sets xy_on=0")

# Store + Scene B arms and stores scene 2
stb = w.store_box
tap((stb[0] + stb[2]) / 2, (stb[1] + stb[3]) / 2)
check(w.store_armed, "Store arms")
scb = w.scene_boxes[1]
tap((scb[0] + scb[2]) / 2, (scb[1] + scb[3]) / 2)
check(not w.store_armed and ctl["store_scene"].value == 2 and ctl["scene"].value == 1, "Store + B saves scene 2")

tb = w.tab_boxes[2]
tap((tb[0] + tb[2]) / 2, (tb[1] + tb[3]) / 2)
check(w.view == 2 and w.flow_xover_box is not None, "switched to Flow tab")

# Drag Low Split frequency on crossover box
xb = w.flow_xover_box
w._on_press(Ev(xb[0] + (xb[2] - xb[0]) * 0.5, (xb[1] + xb[3]) / 2))
w._on_release(Ev(xb[0] + (xb[2] - xb[0]) * 0.5, (xb[1] + xb[3]) / 2))
check(60.0 <= ctl["low_split"].value <= 500.0, "Flow drag sets Low Split Hz: %s" % ctl["low_split"].value)

# Toggle Low Duck button
db = w.flow_duck_box
tap((db[0] + db[2]) / 2, (db[1] + db[3]) / 2)
check(ctl["low_duck"].value == 1, "Flow Low Duck toggled ON")

# Drag Threshold on meter box
mb = w.flow_meter_box
w._on_press(Ev((mb[0] + mb[2]) / 2, (mb[1] + mb[3]) / 2))
w._on_release(Ev((mb[0] + mb[2]) / 2, (mb[1] + mb[3]) / 2))
check(-45.0 <= ctl["threshold"].value <= -15.0, "Flow meter drag sets threshold: %s" % ctl["threshold"].value)

tb = w.tab_boxes[3]
tap((tb[0] + tb[2]) / 2, (tb[1] + tb[3]) / 2)
check(w.view == 3 and w.help_slot == 3, "opened ? Help tab for slot 3")
check(any("Filter Mode" in str(it.get("text", "")) for it in w.widget_canvas.items.values()), "Help shows Filter Mode info")

tb = w.tab_boxes[0]
tap((tb[0] + tb[2]) / 2, (tb[1] + tb[3]) / 2)
check(w.view == 0, "returned to Steps tab")

# TIME: Chop mode displays action labels (THRU/1/8/1/16/1/2x/REV/STOP) and cycles on tap
ctl["slot"].value = 1
ctl["s1_mode"].value = 1
proc.plugin.q[0][0] = 1
ctl["s1_lvl1"].value = 0.0
tick(2)
check(any(it.get("text") == "THRU" for it in w.widget_canvas.items.values()), "TIME: Chop shows THRU badge")
shb0 = w.shape_boxes[0]
tap((shb0[0] + shb0[2]) / 2, (shb0[1] + shb0[3]) / 2)
check(ctl["s1_lvl1"].value == 15.0, "tap on THRU cycles to 1/8 (level 15): got %s" % ctl["s1_lvl1"].value)
check(any(it.get("text") == "1/8" for it in w.widget_canvas.items.values()), "TIME: Chop shows 1/8 badge after tap")

# Performance check
t0 = time.monotonic()
tick(50)
check(time.monotonic() - t0 < 2.0, "50 refreshes take %.2f s" % (time.monotonic() - t0))

print("%s: widget, %d failed" % ("FAIL" if fails else "OK", fails))
sys.exit(1 if fails else 0)
