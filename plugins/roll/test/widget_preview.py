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


class FakeFn:
    def __init__(self, fn):
        self.fn, self.restype, self.argtypes = fn, None, None

    def __call__(self, *a):
        return self.fn(*a)


class FakeLibseq:
    """zynseq as built in vangelis-2610: getTransportState only under its C++ name"""
    def __init__(self):
        self.state, self.beat = 0, 1

    def __getitem__(self, name):
        if name == "_Z17getTransportStatev":
            return FakeFn(lambda: self.state)
        raise AttributeError(name)

    def getBeat(self):
        return self.beat


class FakeZyngui:
    def __init__(self):
        self.state_manager = types.SimpleNamespace(zynseq=types.SimpleNamespace(libseq=FakeLibseq()))

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
    "edit":   ("elayer", "transp", "shift", "eapply"),
    "slots":  ("load", "save", "sdel", "export"),
}
DEFAULTS = {"rec": 0, "play": 0, "undo": 0, "clear": 0, "layer": 1, "lmute": 0, "ldel": 0, "redo": 0,
            "length": 0, "launch": 2, "half": 0, "double": 0, "qgrid": 0, "qstr": 100, "qtarget": 2,
            "qapply": 0, "elayer": 1, "transp": 0, "eapply": 0, "slot": 1, "save": 0, "load": 0, "shift": 0, "sslot": 0,
            "sdel": 0, "qmask": 255, "export": 0, "zbeat": -1}


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
        buf = bytearray(self.HDR.size + (8 * 4000 + 16384) * self.EV.size + 4 + 32 * 8 + 128)   # + UiIn
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
        self.msg = 0

    def get_monitors_dict(self):
        return {"state": 4, "run": 1, "bars": 2.0, "pos": 0.3, "layers": 3, "mute": 0b100, "confirm": 0,
                "bpm": 120.0, "beat": 1, "slots": 0b101, "msg": self.msg}


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


class VGuiCtrl(tkinter.Canvas):
    """zynthian_gui_controller as in vangelis: push starts a long-press timer, a short release toggles."""
    def __init__(self, parent, index):
        super().__init__(parent, width=10, height=2, highlightthickness=0, bg=cfg.color_bg)
        self.index, self.zctrl, self.press_id, self.canvas_push_event = index, None, None, None
        self.active_motion_axis, self.toggles = 0, 0
        self.bind("<ButtonRelease-1>", self.cb_canvas_release)

    def cb_canvas_push(self, event):
        self.canvas_push_event = event
        self.press_id = self.after(2000, lambda: None)

    def cb_canvas_release(self, event):
        if self.press_id:
            self.after_cancel(self.press_id)
            self.press_id = None
        else:
            return
        if (event.time - self.canvas_push_event.time) / 1000 < cfg.zynswitch_bold_seconds:
            self.toggles += 1
            self.zctrl.set_value(0 if self.zctrl.value >= 0.5 else 1)



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

    def ui():
        with open(proc.engine.viz.path, "rb") as f:
            return mod.UIIN.unpack_from(f.read(), mod.UIIN_OFF)

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

    # Undo/Redo knob (Loop page, knob 3). Undo = a quick turn-and-back of the undo port (the plugin times it),
    # Redo = the hidden redo port.
    import time as _t
    zu = proc.controllers_dict["undo"]

    def settle(secs=0.15):
        end = _t.monotonic() + secs
        while _t.monotonic() < end:
            root.update()
            _t.sleep(0.01)

    def undo_pulse(fn):
        w0, r0 = zu.writes, proc.v("redo")
        res = fn()
        settle()
        return (res is not False) and zu.writes == w0 + 2 and proc.v("undo") == 0

    def redo_flip(fn):
        w0, r0 = zu.writes, ui()[4]
        res = fn()
        return (res is not False) and ui()[4] == r0 + 1 and zu.writes == w0

    show("loop")
    zc = ctl.zgui_controllers[2]
    check("loop: touch hook installed on the knobs", getattr(zc, "_eka_roll_hook", False))

    def tap(age):
        zc.canvas_push_ts = datetime.now() - timedelta(seconds=age)
        zc.event_generate("<ButtonRelease-1>")
    check("touch: short tap = Undo (turn-and-back, no toggle left behind)", undo_pulse(lambda: tap(0)))
    check("touch: long press = Redo (MIDI-learn options not opened)", redo_flip(lambda: tap(0.8)) and zc.orig_calls == 0)
    z0 = ctl.zgui_controllers[0]
    z0.canvas_push_ts = datetime.now() - timedelta(seconds=0.8)
    z0.event_generate("<ButtonRelease-1>")
    check("touch: long press on another knob keeps Zynthian's action", z0.orig_calls == 1)
    check("encoder: short push = Undo", undo_pulse(lambda: wdg.cuia_v5_zynpot_switch((2, "S"))))
    check("encoder: bold push = Redo", redo_flip(lambda: wdg.cuia_v5_zynpot_switch((2, "B"))))
    check("encoder: other knob not taken", not wdg.cuia_v5_zynpot_switch((0, "S")))
    ctl.midi_learning = True
    check("encoder: MIDI learn mode not taken", not wdg.cuia_v5_zynpot_switch((2, "S")))
    ctl.midi_learning = False
    show("layers")
    check("encoder: Layers page knob 3 (L Delete) not taken", not wdg.cuia_v5_zynpot_switch((2, "B")))

    # the knob turned and left there (a Redo in the plugin) goes back to 0 by itself
    show("loop")
    zu.set_value(1)
    wdg._undo_return()
    settle(0.8)
    wdg._undo_return()
    check("knob left turned: back to 0 after 0.7 s", proc.v("undo") == 0)

    # classic layout: knob pushes arrive as the 4 standard switches -> widget.switch()
    check("switch: short push of knob 3 = Undo", undo_pulse(lambda: wdg.switch(2, "S")))
    check("switch: bold push of knob 3 = Redo", redo_flip(lambda: wdg.switch(2, "B")))
    check("switch: knob 1 push not taken", not wdg.switch(0, "S"))

    # newer Zynthian (vangelis): pushes and long touches reach the control screen, not the widget
    ctl.options_opened = 0
    check("vangelis: bold push on Undo/Redo = Redo, no options menu",
          redo_flip(lambda: ctl.cuia_v5_zynpot_switch((2, "B"))) and ctl.options_opened == 0)
    check("vangelis: long push on Undo/Redo = Redo",
          redo_flip(lambda: ctl.cuia_v5_zynpot_switch((2, "L"))) and ctl.options_opened == 0)
    check("vangelis: short push on Undo/Redo = Undo", undo_pulse(lambda: ctl.cuia_v5_zynpot_switch((2, "S"))))
    ctl.cuia_v5_zynpot_switch((0, "B"))
    check("vangelis: bold push on Rec keeps the options menu", ctl.options_opened == 1)
    show("loop")
    check("vangelis: hook installed only once (redraws)",
          redo_flip(lambda: ctl.cuia_v5_zynpot_switch((2, "B"))) and ctl.options_opened == 1)

    # vangelis touch: a short tap would toggle the knob; the hook makes it a turn-and-back instead
    vz = VGuiCtrl(root, 2)
    vz.zctrl = zu
    mod._hook_touch_release(vz)

    class TEv:
        def __init__(self, t):
            self.time = t
    vz.cb_canvas_push(TEv(1000))
    check("vangelis touch: short tap = Undo, no toggle", undo_pulse(lambda: vz._eka_roll_release(TEv(1100))) and vz.toggles == 0)

    # zynseq state through the C++ name; stopped -> 0 / -2 alternating, playing -> the beat
    lib = cfg.zyngui.state_manager.zynseq.libseq
    lib.state = 0
    seen = set()
    for _ in range(4):
        wdg._sync_zynseq()
        wdg._ui_put()
        seen.add(ui()[3])
        _t.sleep(0.5)
    check("zynseq stopped: zbeat alternates 0 / -2 (%r)" % sorted(seen), seen == {0, -2})
    lib.state, lib.beat = 1, 3
    wdg._sync_zynseq()
    wdg._ui_put()
    check("zynseq playing: zbeat = beat", ui()[3] == 3)
    lib.state = 2
    wdg._sync_zynseq()
    wdg._ui_put()
    check("zynseq starting: zbeat kept", ui()[3] == 3)

    # qmask chosen on the strip reaches the engine through shared memory
    proc.controllers_dict["qmask"].value = 253
    wdg._ui_put()
    check("qmask written for the engine (253)", ui()[0] == mod.UIIN_MAGIC and ui()[2] == 253)

    # undo / redo messages from the engine appear at the bottom of the screen
    def bottom_text():
        return [c.itemcget(i, "text") for i in c.find_withtag("dyn")
                if c.type(i) == "text" and c.coords(i)[1] > wdg.height * 0.9]
    show("loop")
    for code, text in ((30, "Undo: Quantize"), (40, "Redo: Quantize"), (52, "Undo: layer 2"), (62, "Redo: layer 2"),
                       (20, "Undo: overdub"), (25, "Nothing to undo"), (26, "Nothing to redo"), (33, "Undo: Transpose")):
        proc.engine.msg += 100
        proc.engine.msg = proc.engine.msg // 100 * 100 + code
        show("loop")
        check("message %d shown at the bottom: %r" % (code, text), text in bottom_text())

    # Edit page: Shift knob -> slider; each click = one slider step; Apply hides it
    show("edit")
    check("edit: no slider before Shift is turned", not [hh for hh in wdg.hits if hh[4] == "sh"])
    zs = proc.controllers_dict["shift"]
    zs.value = 1
    show("edit")
    shh = [hh for hh in wdg.hits if hh[4] == "sh"]
    check("edit: Shift turned -> slider with 5 steps", len(shh) == 5)
    check("edit: one click = 1/16 (240 ticks) sent to the engine", wdg.shift_ticks == 240 and ui()[13] == 240)
    x0, y0, x1, y1, kind, i = shh[0]
    wdg.last_click = 0
    wdg._on_click(Ev((x0 + x1) // 2, (y0 + y1) // 2))
    zs.value = 3
    show("edit")
    check("edit: step 1/4 picked on the slider, two more clicks = 240 + 2*960", ui()[13] == 240 + 2 * 960)
    texts = [c.itemcget(i, "text") for i in c.find_withtag("dyn") if c.type(i) == "text"]
    check("edit: pending shift shown as +9/16 (%r)" % [t for t in texts if "/" in t][-1:], "+9/16" in texts)
    ghosts = [i for i in c.find_withtag("roll") if c.type(i) == "rectangle" and c.itemcget(i, "outline") == "#ffffff"]
    check("edit: shifted layer previewed on the roll (%d)" % len(ghosts), len(ghosts) >= 8)
    snap("edit_shift")
    zs.value = 1
    show("edit")
    check("edit: turning back takes 2 steps of 1/4 off", ui()[13] == 240)
    proc.engine.msg = proc.engine.msg // 100 * 100 + 100 + 21           # engine: "Layer shifted"
    show("edit")
    check("edit: after Apply the slider is gone, knob and shift back to 0",
          not [hh for hh in wdg.hits if hh[4] == "sh"] and zs.value == 0 and ui()[13] == 0)
    show("edit")
    check("edit: knob reset is not a new turn", not wdg.shift_show and wdg.shift_ticks == 0)
    check("edit: 'Layer shifted' shown first", "Layer shifted" in bottom_text())
    wdg.toast_until = 0
    show("edit")
    check("edit: then the hint about deleting notes", "Tap a note of layer 1 to delete it" in bottom_text())

    # tap on a note of the selected layer (layer 1: 8 notes, t = i*960+37, len 400) -> delete request
    gx0, gy0, gny1, gw, gL, glo, ghi, grh = wdg._geo
    t, p = 2 * 960 + 37, 48 + 2 * 3
    xn = gx0 + gw * (t + 200) / gL
    yn = gny1 - (p - glo + 1) * grh + grh / 2
    n0 = ui()[17]
    wdg.last_click = 0
    wdg._on_click(Ev(int(xn), int(yn)))
    u = ui()
    check("edit: tap on a note sends it for deletion (%r)" % (u[14:18],), u[14:18] == (0, t, p, n0 + 1))
    wdg.last_click = 0
    wdg._on_click(Ev(int(gx0 + gw * 0.99), int(gy0 + 2)))
    check("edit: tap on an empty place sends nothing", ui()[17] == n0 + 1)
    proc.controllers_dict["elayer"].value = 2
    show("edit")
    wdg.last_click = 0
    wdg._on_click(Ev(int(xn), int(yn)))
    check("edit: notes of other layers are not deleted", ui()[17] == n0 + 1)
    proc.controllers_dict["elayer"].value = 1

    # Slots: tap selects (parameter sslot), the selected slot is light green
    show("slots")
    sh_ = [hh for hh in wdg.hits if hh[4] == "s"]
    x0, y0, x1, y1, kind, i = sh_[2]
    wdg.last_click = 0
    wdg._on_click(Ev((x0 + x1) // 2, (y0 + y1) // 2))
    show("slots")
    check("slots: tap on slot 3 selects it", proc.v("sslot") == 3)
    green = [i for i in c.find_withtag("dyn") if c.type(i) == "rectangle" and c.itemcget(i, "outline") == mod.SLOT_SEL]
    check("slots: selected slot drawn light green", len(green) == 1 and c.itemcget(green[0], "fill") == mod.SLOT_SEL)
    snap("slots_sel")

    proc.engine.viz.remove()
    print("%d failed" % fails)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
