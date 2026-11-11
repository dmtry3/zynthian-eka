#!/usr/bin/python3
# Runs the Eka Fold widget outside Zynthian with stub modules (same approach as Eka Mist).
#   python3 test/widget_preview.py          scripted tests (PASS/FAIL) and the time of a refresh
#   python3 test/widget_preview.py --show   open a window: fake voices play chords, the meter moves
import importlib.util
import os
import sys
import time
import tkinter
import types

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# ---------- stub zyngui modules ----------
cfg = types.ModuleType("zyngui.zynthian_gui_config")
cfg.font_family = "DejaVu Sans"
cfg.color_bg = "#000000"
cfg.color_tx = "#ffffff"
cfg.color_tx_off = "#707070"
cfg.color_hl = "#08a2c0"
cfg.zyngui = None


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
        self.refreshes = getattr(self, "refreshes", 0) + 1
        self.refresh_gui()

    def set_processor(self, processor):
        self.processor = processor


base = types.ModuleType("zyngui.zynthian_widget_base")
base.zynthian_widget_base = _WidgetBase
pkg = types.ModuleType("zyngui")
pkg.zynthian_gui_config, pkg.zynthian_widget_base = cfg, base
sys.modules.update({"zyngui": pkg, "zyngui.zynthian_gui_config": cfg, "zyngui.zynthian_widget_base": base})


class NumCtrl:
    def __init__(self, sym, value):
        self.symbol, self.value = sym, value

    def set_value(self, v, send=True):
        self.value = v


class Engine:
    def __init__(self):
        self.notes = []
        self.peak = -100.0

    def get_monitors_dict(self):
        m = {"out_peak": float(self.peak)}
        for i in range(4):
            m["note%d" % (i + 1)] = float(self.notes[i]) if i < len(self.notes) else -1.0
        return m


class Processor:
    def __init__(self):
        self.engine = Engine()
        self.id = 1
        self.controllers_dict = {s: NumCtrl(s, v) for s, v in (("pitch", 0), ("fine", 0.0), ("attack", 5.0),
                                 ("decay", 300.0), ("sustain", 70.0), ("release", 400.0))}


class Control:
    """Zynthian's control screen: list_data rows (group symbol, index, title, processor, page)."""
    def __init__(self, root):
        self.main_frame = root
        self.layout = {"rows": 4}
        self.screen_title = "Osc"
        self.list_data = []
        self.screen_info = None
        self.selected = []
        self.wide = True
        self.shown = True

    def add_pages(self, proc):
        self.list_data = [(None, None, "> Eka Fold", proc)]
        for j, (g, t) in enumerate((("g_osc", "Osc"), ("g_env", "Env"))):
            self.list_data.append((g, j + 1, t, proc, j))

    def select(self, index=None, set_zctrl=True):
        self.selected.append(index)
        self.screen_info = self.list_data[index]
        self.screen_title = self.screen_info[2]


def load_widget():
    path = os.path.join(ROOT, "ui", "zynthian_widget_eka_fold.py")
    spec = importlib.util.spec_from_file_location("zynthian_widget_eka_fold", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class Tap:
    def __init__(self, x, y):
        self.x, self.y = x, y


def main():
    show = "--show" in sys.argv
    mod = load_widget()
    throttle = mod.UPDATE_PERIOD
    if not show:
        mod.UPDATE_PERIOD = 0.0          # scripted checks see every update; the throttle has its own check
    try:
        root = tkinter.Tk()
    except tkinter.TclError as e:
        print("Note: no display (%s), widget tests skipped." % e)
        return 0
    root.geometry("480x300")
    proc = Processor()
    ctl = Control(root)
    ctl.add_pages(proc)
    wdg = mod.zynthian_widget_eka_fold(ctl)
    wdg.set_processor(proc)
    wdg.pack(fill="both", expand=True)
    root.update()
    wdg.update()
    root.update()
    c = wdg.widget_canvas

    if show:
        chords = [[], [60], [60, 64, 67], [48, 55, 60, 64], [62, 65, 69]]
        state = {"i": 0, "t": 0.0}

        def tick():
            now = time.monotonic()
            if now - state["t"] > 1.2:
                state["t"] = now
                state["i"] = (state["i"] + 1) % len(chords)
                proc.engine.notes = chords[state["i"]]
            proc.engine.peak = -100.0 if not proc.engine.notes else -4.0 - 20.0 * ((now - state["t"]) / 1.2)
            wdg.update()
            root.after(32, tick)        # like Zynthian
        root.after(32, tick)
        root.mainloop()
        return 0

    fails = 0

    def check(name, cond):
        nonlocal fails
        print("%s: %s" % ("PASS" if cond else "FAIL", name))
        fails += 0 if cond else 1

    def texts(tag):
        return [c.itemcget(i, "text") for i in c.find_withtag(tag) if c.type(i) == "text"]

    # tabs, title, tiles, waveform
    check("tab Osc drawn", texts("tabs") == ["Osc"])
    check("page title drawn", texts("title") == ["Osc"])
    check("Source tile Sine", texts("tiles") == ["Sine"])
    lines = [i for i in c.find_withtag("wave") if c.type(i) == "line" and len(c.coords(i)) > 8]
    check("waveform drawn", len(lines) == 1)
    if lines:
        pts = c.coords(lines[0])
        ys = pts[1::2]
        x0, y0, x1, y1 = wdg._osc_layout()[1]
        check("waveform inside its box", min(ys) >= y0 and max(ys) <= y1 and pts[0] >= x0 and pts[-2] <= x1)
        signs = [1 if y < (y0 + y1) / 2 else -1 for y in ys if abs(y - (y0 + y1) / 2) > 0.5]   # points on the axis skipped
        crossings = sum(1 for a, b in zip(signs, signs[1:]) if a != b)
        check("2 periods (3 crossings inside)", crossings == 3)

    # nothing changed: the waveform and the tiles are not redrawn
    ids = (c.find_withtag("wave"), c.find_withtag("tiles"), c.find_withtag("notes"), c.find_withtag("meter"))
    for _ in range(5):
        wdg.update()
    check("no redraw without changes", ids == (c.find_withtag("wave"), c.find_withtag("tiles"),
                                               c.find_withtag("notes"), c.find_withtag("meter")))

    # notes and the meter follow the monitors; the waveform stays
    wave_ids = c.find_withtag("wave")
    check("idle: Pitch and Fine shown", texts("notes") == ["Pitch +0 st  Fine +0 ct"])
    proc.engine.notes = [64, 60, 67]
    proc.engine.peak = -10.0
    wdg.update()
    check("sounding notes shown in order", texts("notes") == ["C4 261.6 Hz   E4 329.6 Hz   G4 392.0 Hz"])
    meter = [i for i in c.find_withtag("meter") if c.itemcget(i, "fill")]
    check("meter shows the level", len(meter) == 1)
    check("waveform not redrawn by the live data", c.find_withtag("wave") == wave_ids)
    proc.controllers_dict["pitch"].value = 12
    proc.controllers_dict["fine"].value = -50.0
    proc.engine.notes = [69]
    wdg.update()
    check("frequency with Pitch and Fine", texts("notes") == ["A4 854.9 Hz"])
    proc.engine.peak = -2.0
    wdg.update()
    check("meter red near 0 dB", any(c.itemcget(i, "fill") == mod.METER_HOT for i in c.find_withtag("meter")))
    proc.engine.notes, proc.engine.peak = [], -100.0
    wdg.update()
    check("meter empty in silence", not [i for i in c.find_withtag("meter") if c.itemcget(i, "fill")])

    # tab <-> knob page: a tap on the tab selects its page; the Env page (no tab) keeps Osc shown
    ctl.selected = []
    wdg._on_press(Tap(10, 5))
    check("tap on Osc selects the Osc page", ctl.selected == [1])
    ctl.select(2)
    wdg.update()
    check("Env page: tab stays Osc, title Env", wdg.tab == 0 and texts("title") == ["Env"])
    ctl.select(1)
    wdg.update()
    check("back on Osc", texts("title") == ["Osc"])
    wdg._on_press(Tap(100, 100))                     # a tap below the tabs does nothing yet
    check("tap on the waveform: no error", True)

    # throttle: two updates in a row, only one reaches refresh_gui
    mod.UPDATE_PERIOD = throttle
    wdg.next_update = 0.0
    n = getattr(wdg, "refreshes", 0)
    wdg.update()
    wdg.update()
    check("update() throttled to UPDATE_PERIOD", wdg.refreshes == n + 1)
    mod.UPDATE_PERIOD = 0.0

    # sizes: Zynthian V5 (800x480 screen, widget smaller), a small one
    for geo in ("800x400", "320x180", "480x300"):
        root.geometry(geo)
        root.update()
        wdg.update()
        root.update()
        tiles, wave, meter_box, notes_y = wdg._osc_layout()
        ok = tiles[3] < wave[1] and wave[3] < notes_y and wave[2] < meter_box[0] and meter_box[2] <= wdg.width
        check("layout %s: tiles, wave, meter, notes do not overlap" % geo, ok)

    # time of a refresh with live data changing every time (the case of 4 voices playing)
    chords = [[60, 64, 67, 72], [62, 65, 69], [48], []]
    t0 = time.perf_counter()
    for i in range(200):
        proc.engine.notes = chords[i % 4]
        proc.engine.peak = -30.0 + (i % 20)
        wdg.update()
        root.update_idletasks()
    live = (time.perf_counter() - t0) / 200 * 1000
    t0 = time.perf_counter()
    for i in range(200):
        wdg.update()
        root.update_idletasks()
    idle = (time.perf_counter() - t0) / 200 * 1000
    t0 = time.perf_counter()
    for i in range(50):
        wdg._clear()
        wdg.refresh_gui()
        root.update_idletasks()
    full = (time.perf_counter() - t0) / 50 * 1000
    print("refresh on this PC: %.3f ms with live data changing, %.3f ms without changes, %.3f ms full redraw"
          % (live, idle, full))

    root.destroy()
    print("All widget tests passed" if not fails else "%d FAILED" % fails)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
