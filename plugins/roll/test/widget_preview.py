#!/usr/bin/python3
# Runs the Eka Roll widget outside Zynthian with stub modules.
#   python3 test/widget_preview.py              scripted tests (PASS/FAIL)
#   python3 test/widget_preview.py --show PAGE  open a window (loop, layers, timing, quant, edit, slots)
#   python3 test/widget_preview.py --shots DIR  also save a PNG of every page (needs Pillow and a display)
import importlib.util
import os
import struct
import sys
import tkinter
import types
from datetime import datetime, timedelta

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# ---------- stub zyngui modules ----------
cfg = types.ModuleType("zyngui.zynthian_gui_config")
cfg.font_family = "DejaVu Sans"
cfg.color_bg = "#000000"
cfg.color_tx = "#ffffff"
cfg.color_tx_off = "#707070"
cfg.color_hl = "#08a2c0"
cfg.enable_touch_controller_switches = 1
cfg.zynswitch_bold_seconds = 0.3
cfg.zynswitch_long_seconds = 2.0


class FakeZyngui:
    def after_idle(self, fn, *args):
        fn(*args)


cfg.zyngui = FakeZyngui()


class _WidgetBase(tkinter.Frame):          # same contract as zynthian_widget_base
    def __init__(self, parent):
        super().__init__(parent.main_frame, bg=cfg.color_bg)
        self.zyngui = cfg.zyngui
        self.zyngui_control = parent
        self.width = 1
        self.height = 1
        self.wide = parent.wide
        self.rows = parent.layout['rows']
        self.shown = False
        self.processor = None
        self.widget_canvas = None
        self.monitors = None
        self.bind('<Configure>', self.on_size)

    def on_size(self, event):
        if event.width == self.width and event.height == self.height:
            return False
        self.width, self.height = event.width, event.height
        try:
            self.widget_canvas.configure(width=self.width, height=self.height)
        except Exception:
            pass
        return True

    def update(self):
        self.monitors = self.processor.engine.get_monitors_dict()
        self.refresh_gui()

    def set_processor(self, processor):
        self.processor = processor


base = types.ModuleType("zyngui.zynthian_widget_base")
base.zynthian_widget_base = _WidgetBase
pkg = types.ModuleType("zyngui")
pkg.zynthian_gui_config, pkg.zynthian_widget_base = cfg, base
sys.modules.update({"zyngui": pkg, "zyngui.zynthian_gui_config": cfg, "zyngui.zynthian_widget_base": base})

PAGES = {
    "loop":   ("rec", "play", "undo", "clear"),
    "layers": ("layer", "lmute", "ldel"),
    "timing": ("length", "launch", "half", "double"),
    "quant":  ("qgrid", "qstr", "qtarget", "qapply"),
    "edit":   ("elayer", "transp", "eapply", "export"),
    "slots":  ("slot", "save", "load", "sdel"),
}
DEFAULTS = {"rec": 0, "play": 0, "undo": 0, "clear": 0, "layer": 1, "lmute": 0, "ldel": 0, "redo": 0,
            "length": 0, "launch": 2, "half": 0, "double": 0, "qgrid": 0, "qstr": 100, "qtarget": 2,
            "qapply": 0, "elayer": 1, "transp": 0, "eapply": 0, "slot": 1, "save": 0, "load": 0,
            "sdel": 0, "qmask": 255, "export": 0}


class Ctrl:
    def __init__(self, proc, sym, value):
        self.processor, self.symbol, self.value = proc, sym, value
        self.writes = 0

    def set_value(self, v, send=True):
        self.value = v
        self.writes += 1


class FakeViz:
    """Shared memory as the plugin writes it (see Viz in eka_roll.c): 2 bars, 3 layers."""
    HDR = struct.Struct("<8I8I8i2I")
    EV = struct.Struct("<IIBBBB")

    def __init__(self):
        self.pid = 900000 + os.getpid() % 90000
        self.path = "/dev/shm/eka_roll_%d" % self.pid
        L = 2 * 4 * 960
        lay1 = [(i * 960 + 37, 400, 0x90, 48 + (i % 4) * 3, 100) for i in range(8)]       # off the grid a bit
        lay2 = [(i * 480 + 15, 200, 0x90, 60 + (i % 5), 90) for i in range(16)]
        lay3 = [(i * 1920, 1800, 0x90, 67, 80) for i in range(4)]
        layers = [lay1, lay2, lay3]
        buf = bytearray(self.HDR.size + (8 * 4000 + 16384) * self.EV.size + 4 + 32 * 8)
        ns = [len(x) for x in layers] + [0] * 5
        self.HDR.pack_into(buf, 0, 0x4D4C5A31, 2, 7, L, len(layers), 4, 4, 0b100,
                           *ns, *([0] * 8), 0, 0)
        off = self.HDR.size
        for lay in layers:
            for (t, d, st, p, v) in lay:
                self.EV.pack_into(buf, off, t, d, st, p, v, 0)
                off += self.EV.size
        with open(self.path, "wb") as f:
            f.write(buf)

    def remove(self):
        try:
            os.unlink(self.path)
        except OSError:
            pass


class Proc:
    def __init__(self, pid):
        self.pid = pid


class Engine:
    def __init__(self):
        self.viz = FakeViz()
        self.proc = Proc(self.viz.pid)

    def get_monitors_dict(self):
        return {"state": 4, "run": 1, "bars": 2.0, "pos": 0.3, "layers": 3, "mute": 0b100, "confirm": 0,
                "bpm": 120.0, "beat": 1, "slots": 0b101, "msg": 0}


class Processor:
    def __init__(self):
        self.engine = Engine()
        self.id = 7
        self.controllers_dict = {k: Ctrl(self, k, v) for k, v in DEFAULTS.items()}

    def v(self, sym):
        return self.controllers_dict[sym].value


class GuiCtrl(tkinter.Canvas):
    """The bits of zynthian_gui_controller the touch hook uses."""
    def __init__(self, parent, index):
        super().__init__(parent, width=10, height=2, highlightthickness=0, bg=cfg.color_bg)
        self.pack(side="bottom")                 # mapped, so generated touch events reach it
        self.index, self.zctrl, self.enabled = index, None, True
        self.canvas_push_ts, self.active_motion_axis = None, 0
        self.orig_calls = 0
        self.bind("<ButtonRelease-1>", self.cb_canvas_release)

    def cb_canvas_release(self, event):
        self.orig_calls += 1
        if self.canvas_push_ts and self.zctrl is not None:
            dts = (datetime.now() - self.canvas_push_ts).total_seconds()
            if dts < cfg.zynswitch_bold_seconds:
                z = self.zctrl
                z.set_value(0 if z.value >= 0.5 else 1)
        self.canvas_push_ts = None


class Control:
    def __init__(self, root, proc):
        self.main_frame = root
        self.layout = {"rows": 4}
        self.wide = True
        self.shown = True
        self.proc = proc
        self.midi_learning = False
        self.mode = "control"
        self.zgui_controllers = [GuiCtrl(root, i) for i in range(4)]
        self.zcontrollers = []

    def show_page(self, page):
        syms = PAGES[page]
        self.zcontrollers = [self.proc.controllers_dict[s] for s in syms]
        for i, zc in enumerate(self.zgui_controllers):
            zc.zctrl = self.zcontrollers[i] if i < len(syms) else None

    def display_widget(self, name):
        pass

    def cuia_v5_zynpot_switch(self, params):     # as in Zynthian vangelis: bold/long push = controller options
        self.options_opened = getattr(self, "options_opened", 0) + (params[1] in ("B", "L"))
        return True


def load_widget():
    path = os.path.join(ROOT, "ui", "zynthian_widget_eka_roll.py")
    spec = importlib.util.spec_from_file_location("zynthian_widget_eka_roll", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class Ev:
    def __init__(self, x, y):
        self.x, self.y = x, y


def main():
    mod = load_widget()
    root = tkinter.Tk()
    root.geometry("480x300")
    proc = Processor()
    ctl = Control(root, proc)
    wdg = mod.zynthian_widget_eka_roll(ctl)
    wdg.set_processor(proc)
    wdg.pack(fill="both", expand=True)
    c = wdg.widget_canvas

    def show(page):
        ctl.show_page(page)
        wdg.update()
        root.update()
        wdg.update()
        root.update()

    if "--show" in sys.argv:
        page = sys.argv[sys.argv.index("--show") + 1] if len(sys.argv) > sys.argv.index("--show") + 1 else "loop"
        show(page)

        def tick():
            wdg.update()
            root.after(50, tick)
        tick()
        root.mainloop()
        proc.engine.viz.remove()
        return 0

    shots = sys.argv[sys.argv.index("--shots") + 1] if "--shots" in sys.argv else None
    fails = 0

    def check(name, cond):
        nonlocal fails
        print("%s: %s" % ("PASS" if cond else "FAIL", name))
        fails += 0 if cond else 1

    def snap(name):
        if not shots:
            return
        # redraw the canvas items with Pillow (screen grabs don't work under WSLg)
        from PIL import Image, ImageDraw, ImageFont
        os.makedirs(shots, exist_ok=True)
        img = Image.new("RGB", (c.winfo_width(), c.winfo_height()), cfg.color_bg)
        dr = ImageDraw.Draw(img, "RGBA")
        for i in c.find_all():
            t, xy = c.type(i), c.coords(i)
            if c.itemcget(i, "state") == "hidden":
                continue
            if t == "rectangle":
                fill, out = c.itemcget(i, "fill") or None, c.itemcget(i, "outline") or None
                wd = int(float(c.itemcget(i, "width") or 1))
                if fill and c.itemcget(i, "stipple"):
                    r, g, b = [v // 256 for v in c.winfo_rgb(fill)]
                    fill = (r, g, b, 40)
                dr.rectangle(xy, fill=fill, outline=out, width=wd if out else 0)
            elif t == "line":
                dr.line(xy, fill=c.itemcget(i, "fill"), width=int(float(c.itemcget(i, "width"))))
            elif t == "polygon":
                r, g, b = [v // 256 for v in c.winfo_rgb(c.itemcget(i, "fill"))]
                dr.polygon(list(zip(xy[::2], xy[1::2])), fill=(r, g, b, 60))
            elif t == "text":
                size = abs(int(c.itemcget(i, "font").split()[-1].strip("{}")))
                try:
                    font = ImageFont.truetype("DejaVuSans.ttf", size)
                except OSError:
                    font = ImageFont.load_default()
                anchor = {"nw": "la", "n": "ma", "ne": "ra", "s": "md", "center": "mm"}.get(c.itemcget(i, "anchor"), "mm")
                dr.text(xy, c.itemcget(i, "text"), fill=c.itemcget(i, "fill"), font=font, anchor=anchor)
        img.save(os.path.join(shots, name + ".png"))

    def grid_lines():
        return [i for i in c.find_withtag("roll")
                if c.type(i) == "line" and c.itemcget(i, "fill") in (mod.GRID_COLOR, mod.dim(mod.GRID_COLOR, 0.6))]

    # every page: 3 rows (Z2/V5 layout), notes everywhere but Slots
    for page in PAGES:
        show(page)
        check("%s: widget is 3 rows" % page, wdg.rows == 3)
        notes = [i for i in c.find_withtag("roll") if c.type(i) == "rectangle"]
        if page == "slots":
            check("slots: no piano roll, 16 slot cells", not notes and
                  sum(1 for hh in wdg.hits if hh[4] == "s") == 16)
        else:
            check("%s: piano roll drawn" % page, len(notes) > 20)
        snap(page)

    # Layers: tap on the strip selects the layer
    show("layers")
    x0, y0, x1, y1, kind, i = [hh for hh in wdg.hits if hh[4] == "l" and hh[5] == 1][0]
    wdg.last_click = 0
    wdg._on_click(Ev((x0 + x1) // 2, (y0 + y1) // 2))
    check("layers: tap on strip 2 selects layer 2", proc.v("layer") == 2)

    # Quantize: grid off -> no grid lines; 1/16 -> lines (bright right after the change); tap toggles qmask
    show("quant")
    check("quant: grid Off draws no grid", not grid_lines())
    proc.controllers_dict["qgrid"].value = 3
    show("quant")
    gl = grid_lines()
    check("quant: grid 1/16 drawn (%d lines), bright after the change" % len(gl),
          len(gl) >= 28 and any(c.itemcget(i, "fill") == mod.GRID_COLOR for i in gl))
    snap("quant_grid")
    wdg.qgrid_t = 0
    show("quant")
    check("quant: grid stays, dimmer later", grid_lines() and
          all(c.itemcget(i, "fill") == mod.dim(mod.GRID_COLOR, 0.6) for i in grid_lines()))
    qh = [hh for hh in wdg.hits if hh[4] == "q"]
    check("quant: strip taps toggle quantize layers (%d)" % len(qh), len(qh) == 3)
    x0, y0, x1, y1, kind, i = qh[0]
    wdg.last_click = 0
    wdg._on_click(Ev((x0 + x1) // 2, (y0 + y1) // 2))
    check("quant: tap on layer 1 removes it from qmask", proc.v("qmask") == 254)
    proc.controllers_dict["qmask"].value = 255

    # Undo/Redo knob (Loop page, knob 3)
    show("loop")
    zc = ctl.zgui_controllers[2]
    check("loop: touch hook installed on the knobs", getattr(zc, "_eka_roll_hook", False))
    zc.canvas_push_ts = datetime.now()
    zc.event_generate("<ButtonRelease-1>")
    check("touch: short tap = Undo", proc.v("undo") == 1 and proc.v("redo") == 0)
    zc.canvas_push_ts = datetime.now() - timedelta(seconds=0.8)
    zc.event_generate("<ButtonRelease-1>")
    check("touch: long press = Redo (undo untouched, MIDI-learn options not opened)",
          proc.v("undo") == 1 and proc.v("redo") == 1 and zc.orig_calls == 1)
    z0 = ctl.zgui_controllers[0]
    z0.canvas_push_ts = datetime.now() - timedelta(seconds=0.8)
    z0.event_generate("<ButtonRelease-1>")
    check("touch: long press on another knob keeps Zynthian's action", z0.orig_calls == 1 and proc.v("redo") == 1)
    check("encoder: short push = Undo", wdg.cuia_v5_zynpot_switch((2, "S")) and proc.v("undo") == 0)
    check("encoder: bold push = Redo", wdg.cuia_v5_zynpot_switch((2, "B")) and proc.v("redo") == 0)
    check("encoder: other knob not taken", not wdg.cuia_v5_zynpot_switch((0, "S")))
    ctl.midi_learning = True
    check("encoder: MIDI learn mode not taken", not wdg.cuia_v5_zynpot_switch((2, "S")))
    ctl.midi_learning = False
    show("layers")
    check("encoder: Layers page knob 3 (L Delete) not taken", not wdg.cuia_v5_zynpot_switch((2, "B")))

    # newer Zynthian (vangelis): pushes and long touches reach the control screen, not the widget
    show("loop")
    ctl.options_opened = 0
    ctl.cuia_v5_zynpot_switch((2, "B"))
    check("vangelis: bold push on Undo/Redo = Redo, no options menu", proc.v("redo") == 1 and ctl.options_opened == 0)
    ctl.cuia_v5_zynpot_switch((2, "L"))
    check("vangelis: long push on Undo/Redo = Redo", proc.v("redo") == 0 and ctl.options_opened == 0)
    ctl.cuia_v5_zynpot_switch((2, "S"))
    check("vangelis: short push on Undo/Redo = Undo", proc.v("undo") == 1)
    ctl.cuia_v5_zynpot_switch((0, "B"))
    check("vangelis: bold push on Rec keeps the options menu", ctl.options_opened == 1)
    show("loop")
    ctl.cuia_v5_zynpot_switch((2, "B"))
    check("vangelis: hook installed only once (redraws)", proc.v("redo") == 1 and ctl.options_opened == 1)

    proc.engine.viz.remove()
    print("%d failed" % fails)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
