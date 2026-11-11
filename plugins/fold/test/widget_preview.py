#!/usr/bin/python3
# Runs the Eka Fold widget outside Zynthian with stub modules (same approach as Eka Mist).
#   python3 test/widget_preview.py          scripted tests (PASS/FAIL) and the time of a refresh
#   python3 test/widget_preview.py --show   open a window: fake voices play chords, the meter and the Mod phase move
import importlib.util
import math
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
        self.mod_phase = -1.0

    def get_monitors_dict(self):
        m = {"out_peak": float(self.peak), "mod_phase": float(self.mod_phase)}
        for i in range(4):
            m["note%d" % (i + 1)] = float(self.notes[i]) if i < len(self.notes) else -1.0
        return m


class Processor:
    def __init__(self):
        self.engine = Engine()
        self.id = 1
        self.controllers_dict = {s: NumCtrl(s, v) for s, v in (("pitch", 0), ("fine", 0.0), ("attack", 5.0),
                                 ("decay", 300.0), ("sustain", 70.0), ("release", 400.0),
                                 ("mod_freq", 30.0), ("mod_index", 0.0), ("mod_shape", 1), ("mod_mode", 2),
                                 ("route_fm", 1), ("route_am", 0), ("route_sync", 0))}


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
        for j, (g, t) in enumerate((("g_osc", "Osc"), ("g_mod", "Mod"), ("g_env", "Env"))):
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
            proc.engine.mod_phase = (now * mod.free_hz(proc.controllers_dict["mod_freq"].value)) % 1.0 \
                if proc.engine.notes else -1.0
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
    check("tabs Osc and Mod drawn", texts("tabs") == ["Osc", "Mod"])
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
    ctl.select(3)
    wdg.update()
    check("Env page: tab stays Osc, title Env", wdg.tab == 0 and texts("title") == ["Env"])
    ctl.select(1)
    wdg.update()
    check("back on Osc", texts("title") == ["Osc"])
    wdg._on_press(Tap(100, 100))                     # a tap below the tabs does nothing yet
    check("tap on the waveform: no error", True)

    # ---------- Mod tab ----------
    # the widget's Mod Freq formulas and ratios are those of fold_core
    import re
    core_c = open(os.path.join(ROOT, "src", "fold_core.c")).read()
    core_h = open(os.path.join(ROOT, "src", "fold_core.h")).read()
    body = core_c[core_c.index("fc_ratios[FC_NRATIOS] = {") + 25:]
    body = body[:body.index("}")]
    c_ratios = [eval(t.replace("f", "")) for t in body.replace("\n", " ").split(",")]
    check("ratios as in fold_core.c", [p / q for p, q in mod.RATIOS] == c_ratios)

    def hdr(name):
        return float(re.search(r"#define %s ([0-9.]+)f" % name, core_h).group(1))
    check("Mod Freq ranges and FM depth as in fold_core.h",
          (mod.FREQ_MAX, mod.FREE_MIN, mod.FREE_MAX, mod.KEY_RANGE, mod.FM_DEPTH) ==
          (hdr("FC_FREQ_MAX"), hdr("FC_FREE_MIN"), hdr("FC_FREE_MAX"), hdr("FC_KEY_RANGE"), hdr("FC_FM_DEPTH")))
    check("Mod Freq meaning per mode", (mod.freq_text(0, 30), mod.freq_text(1, 62.5), mod.freq_text(2, 30),
                                        mod.freq_text(2, 40), mod.freq_text(0, 100)) ==
          ("Free 0.63 Hz", "Keytrack +12.0 st", "Ratio ×1", "Ratio ×3/2", "Free 10.00 kHz"))
    plain = mod.result_values(1.0, 1, 0.0, 1, 0, 0, 40, 2.0)
    check("result: Index 0 is a plain sine",
          max(abs(y - math.sin(2 * math.pi * 2.0 * i / 40)) for i, y in enumerate(plain)) < 1e-9)
    ring = mod.result_values(1.0, 1, 1.0, 0, 1, 0, 80, 2.0)
    check("result: ring at ratio 1 never below zero", min(ring) > -1e-6)
    synced = mod.result_values(0.75, 1, 0.0, 0, 0, 1, 400, 4.0)
    unsynced = mod.result_values(0.75, 1, 0.0, 0, 0, 0, 400, 4.0)
    # ratio 3/4: the Mod OSC begins a cycle after 4/3 Principal periods (value 300 of 400), the sine restarts there
    check("result: sync restarts the Principal at the Mod cycle",
          max(abs(a - b) for a, b in zip(synced[:133], unsynced[:133])) < 1e-9 and abs(synced[136]) < 0.25 and
          abs(unsynced[136]) > 0.7)

    ctl.selected = []
    tx = (wdg.tab_boxes[1][0] + wdg.tab_boxes[1][1]) / 2
    wdg._on_press(Tap(tx, 5))
    wdg.update()
    check("tap on Mod: Mod tab and its page", wdg.tab == 1 and ctl.selected == [2] and texts("title") == ["Mod"])
    ctl.select(1)
    wdg.update()
    check("Osc page shows the Osc tab", wdg.tab == 0)
    ctl.select(2)
    wdg.update()
    check("Mod page shows the Mod tab", wdg.tab == 1)

    def routes():
        out = {}
        for x0, y0, x1, y1, sym in wdg.route_boxes:
            rect = c.find_enclosed(x0 - 1, y0 - 1, x1 + 1, y1 + 1)
            fills = [c.itemcget(i, "fill") for i in rect if c.type(i) == "rectangle"]
            out[sym] = bool(fills and fills[0])
        return out
    mtexts = texts("mod")
    check("Mod tab: blocks, arrows, Index, meaning", "Mod OSC" in mtexts and "Principal" in mtexts and
          "FM →" in mtexts and "AM →" in mtexts and "Sync →" in mtexts and "0%" in mtexts and
          "Ratio ×1 · Sine" in mtexts)
    check("FM lit by default, AM and Sync not", routes() == {"route_fm": True, "route_am": False, "route_sync": False})

    def tap_route(sym):
        for x0, y0, x1, y1, s in wdg.route_boxes:
            if s == sym:
                wdg._on_press(Tap((x0 + x1) / 2, (y0 + y1) / 2))
    tap_route("route_am")
    check("tap on AM switches it on", proc.controllers_dict["route_am"].value == 1 and routes()["route_am"])
    tap_route("route_am")
    check("a second tap switches it off", proc.controllers_dict["route_am"].value == 0 and not routes()["route_am"])
    tap_route("route_sync")
    check("Sync on", proc.controllers_dict["route_sync"].value == 1 and routes()["route_sync"])
    tap_route("route_sync")

    def result_curve():
        x0, y0, x1, y1 = wdg._curve_box(wdg._mod_layout()[2])
        lines = [i for i in c.find_withtag("mod") if c.type(i) == "line" and len(c.coords(i)) > 8
                 and c.coords(i)[0] >= x0 - 1]
        return c.coords(lines[0]) if lines else []
    before = result_curve()
    proc.controllers_dict["mod_index"].value = 45.0
    time.sleep(mod.CURVE_PERIOD)
    wdg.update()
    check("Index shown large", "45%" in texts("mod"))
    check("FM at 45 % changes the Principal curve", result_curve() != before and result_curve() != [])
    # curves follow the knobs at most every CURVE_PERIOD
    proc.controllers_dict["mod_index"].value = 80.0
    wdg.update()
    check("a second change at once waits", "45%" in texts("mod"))
    time.sleep(mod.CURVE_PERIOD)
    wdg.update()
    check("then it is drawn", "80%" in texts("mod"))
    ids = c.find_withtag("mod")
    for _ in range(5):
        wdg.update()
    check("Mod tab: no redraw without changes", c.find_withtag("mod") == ids)

    # the phase mark: only in Free mode at LFO rates; moved, not recreated
    proc.engine.mod_phase = 0.25
    wdg.update()
    check("no phase mark in Ratio mode", not c.find_withtag("modmark"))
    proc.controllers_dict["mod_mode"].value = 0
    proc.controllers_dict["mod_freq"].value = 50.0           # 10 Hz: too fast for a mark
    time.sleep(mod.CURVE_PERIOD)
    wdg.update()
    check("Free 10 Hz: no mark", not c.find_withtag("modmark") and "Free 10.0 Hz · Sine" in texts("mod"))
    check("Free mode: the result is drawn for C4", "Principal at C4" in texts("mod"))
    proc.controllers_dict["mod_freq"].value = 33.3333        # 1 Hz
    time.sleep(mod.CURVE_PERIOD)
    wdg.update()
    marks = c.find_withtag("modmark")
    x0, y0, x1, y1 = wdg._curve_box(wdg._mod_layout()[0])
    check("Free 1 Hz: the mark at the phase", len(marks) == 1 and
          abs((c.coords(marks[0])[0] + c.coords(marks[0])[2]) / 2 - (x0 + 0.25 * (x1 - x0))) <= 1.0)
    proc.engine.mod_phase = 0.6
    wdg.update()
    check("the mark moves (same item)", c.find_withtag("modmark") == marks and
          abs((c.coords(marks[0])[0] + c.coords(marks[0])[2]) / 2 - (x0 + 0.6 * (x1 - x0))) <= 1.0)
    proc.engine.mod_phase = -1.0
    wdg.update()
    check("no voice: no mark", not c.find_withtag("modmark"))
    proc.controllers_dict["mod_mode"].value = 2
    proc.controllers_dict["mod_freq"].value = 30.0
    proc.controllers_dict["mod_index"].value = 0.0
    ctl.select(1)
    wdg.update()

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
        left, mid, right, bottom_y = wdg._mod_layout()
        ok = left[2] < mid[0] and mid[2] < right[0] and right[2] <= wdg.width and \
            max(left[3], mid[3], right[3]) < bottom_y - wdg._font_size() and left[1] >= wdg._header_h()
        check("layout %s: Mod blocks and bottom line do not overlap" % geo, ok)

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
    # the Mod tab with a moving phase mark, and its full redraw (the result curve is computed in Python)
    ctl.select(2)
    proc.controllers_dict["mod_mode"].value = 0
    proc.controllers_dict["mod_freq"].value = 33.3333
    proc.controllers_dict["mod_index"].value = 60.0
    proc.controllers_dict["route_am"].value = 1
    wdg.update()
    t0 = time.perf_counter()
    for i in range(200):
        proc.engine.mod_phase = (i % 50) / 50.0
        wdg.update()
        root.update_idletasks()
    mark = (time.perf_counter() - t0) / 200 * 1000
    t0 = time.perf_counter()
    for i in range(50):
        wdg._clear()
        wdg.refresh_gui()
        root.update_idletasks()
    full = (time.perf_counter() - t0) / 50 * 1000
    print("Mod tab on this PC: %.3f ms with the phase mark moving, %.3f ms full redraw" % (mark, full))

    root.destroy()
    print("All widget tests passed" if not fails else "%d FAILED" % fails)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
