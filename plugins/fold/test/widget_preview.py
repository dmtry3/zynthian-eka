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
        self.gate_level = -1.0
        self.fg = [-1.0, -1.0]
        self.delay_level = -1.0

    def get_monitors_dict(self):
        m = {"out_peak": float(self.peak), "mod_phase": float(self.mod_phase), "gate_level": float(self.gate_level),
             "fg1": float(self.fg[0]), "fg2": float(self.fg[1]), "delay_level": float(self.delay_level)}
        for i in range(4):
            m["note%d" % (i + 1)] = float(self.notes[i]) if i < len(self.notes) else -1.0
        return m


class Processor:
    def __init__(self):
        self.engine = Engine()
        self.id = 1
        self.controllers_dict = {s: NumCtrl(s, v) for s, v in (("pitch", 0), ("fine", 0.0),
                                 ("source", 0), ("harmonics", 50.0), ("color", 50.0), ("morph", 50.0),
                                 ("gate_mode", 2), ("gate_decay", 30.0), ("gate_res", 0.0), ("level", 80.0),
                                 ("fg1_rise", 5.0), ("fg1_fall", 300.0), ("fg2_rise", 300.0), ("fg2_fall", 800.0),
                                 ("fg1_mode", 1), ("fg2_mode", 1), ("m_fg2_timbre", 0),
                                 ("mod_freq", 30.0), ("mod_index", 0.0), ("mod_shape", 1), ("mod_mode", 2),
                                 ("route_fm", 1), ("route_am", 0), ("route_sync", 0), ("route_timbre", 0),
                                 ("timbre", 0.0), ("order", 0.0), ("symmetry", 0.0), ("drive", 0.0),
                                 ("delay_mode", 0), ("delay_time", 50.0), ("delay_fb", 50.0), ("delay_mix", 30.0),
                                 ("delay_shaper", 30), ("delay_eq_freq", 50), ("delay_eq_gain", 0))}
        for src in ("fg1", "fg2", "mod", "rnd", "vel", "key", "wheel", "at"):     # the mod matrix
            for dst in ("pitch", "harmonics", "color", "morph", "index", "order", "symmetry", "timbre", "lpg", "dtime",
                        "dfb"):
                sym = "m_%s_%s" % (src, dst)
                self.controllers_dict.setdefault(sym, NumCtrl(sym, 0))


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
        for j, (g, t) in enumerate((("g_osc", "Osc"), ("g_mod", "Mod"), ("g_timbre", "Timbre"),
                                    ("g_gate", "Gate"), ("g_env", "Env"), ("g_delay", "Delay"))):
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
            held = now - state["t"]                     # a fake FG1 / vactrol: rise 0.1 s, hold, fall at 0.8 s
            if not proc.engine.notes:
                proc.engine.fg, proc.engine.gate_level = [-1.0, -1.0], -1.0
            else:
                pos = held / 0.1 if held < 0.1 else 1.5 if held < 0.8 else min(3.0, 2.0 + (held - 0.8) / 0.4)
                proc.engine.fg = [pos, (held / 1.2) * 3.0 % 3.0]
                proc.engine.gate_level = pos if pos < 1 else 1.0 if pos < 2 else max(0.0, 3.0 - pos)
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
    check("tabs Osc, Mod, Fold, Gate, Env, Delay, Matrix drawn", texts("tabs") == ["Osc", "Mod", "Fold", "Gate", "Env",
                                                                                 "Delay", "Matrix"])
    check("page title drawn", texts("title") == ["Osc"])
    check("Source tiles, Pitch and Fine", texts("tiles") == ["Sine", "VA", "FM2", "Wave", "Pitch +0", "Fine +0"])
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
    check("idle: no notes line", texts("notes") == [""])
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

    # tab <-> knob page: a tap on the tab selects its page, turning to a page shows its tab
    ctl.selected = []
    wdg._on_press(Tap(10, 5))
    check("tap on Osc selects the Osc page", ctl.selected == [1])
    ctl.select(5)
    wdg.update()
    check("Env page shows the Env tab", wdg.tab == 4 and texts("title") == ["Env"])
    ctl.select(1)
    wdg.update()
    check("back on Osc", texts("title") == ["Osc"])
    wdg._on_press(Tap(100, 100))                     # a tap below the tabs does nothing yet
    check("tap on the waveform: no error", True)

    # ---------- Osc: sources, Pitch, Fine ----------
    check("engine tables found next to the plugin", bool(mod.engine_tables()))
    def tile_center(i):
        x0, y0, x1, y1, _ = wdg.tile_boxes[i]
        return Tap((x0 + x1) / 2, (y0 + y1) / 2)
    infos = {}
    for i, name in enumerate(mod.SOURCES):
        wave_ids = c.find_withtag("wave")
        wdg._on_press(tile_center(i))
        wdg.update()
        lit = [c.itemcget(t, "text") for t in c.find_withtag("tiles")
               if c.type(t) == "text" and c.itemcget(t, "fill") == "#000000"]
        lines = [t for t in c.find_withtag("wave") if c.type(t) == "line" and len(c.coords(t)) > 8]
        x0, y0, x1, y1 = wdg._osc_layout()[1]
        ys = c.coords(lines[0])[1::2] if lines else []
        ok = proc.controllers_dict["source"].value == i and lit == [name] and len(lines) == 1 and \
            min(ys) >= y0 and max(ys) <= y1 and (i == 0 or c.find_withtag("wave") != wave_ids)
        check("tap on %s: selected, its wave drawn inside the box" % name, ok)
        infos[name] = texts("info")
    check("knobs line per source", infos["Sine"][0].startswith("Harmonics, Color, Morph: not used") and
          infos["VA"][0].startswith("2nd osc +0.0 st") and infos["FM2"][0].startswith("ratio 2.02") and
          infos["Wave"][0].startswith("bank 4"))
    # the knobs change the wave
    wdg._on_press(tile_center(1))
    wdg.update()
    before = [c.coords(t) for t in c.find_withtag("wave") if c.type(t) == "line" and len(c.coords(t)) > 8]
    proc.controllers_dict["color"].value = 0.0
    wdg.wave_drawn = 0.0
    wdg.update()
    after = [c.coords(t) for t in c.find_withtag("wave") if c.type(t) == "line" and len(c.coords(t)) > 8]
    check("Color moves the VA wave and the knobs line", before != after and "1: tri" in texts("info")[0])
    proc.controllers_dict["color"].value = 50.0
    # Pitch and Fine: a finger up / down on their box, a double tap resets
    def box_tap(sym):
        for x0, y0, x1, y1, s_, lo, hi, px in wdg.value_boxes:
            if s_ == sym:
                return Tap((x0 + x1) / 2, (y0 + y1) / 2)
    proc.controllers_dict["pitch"].value = proc.controllers_dict["fine"].value = 0
    t = box_tap("pitch")
    wdg.last_tap = (None, 0.0)
    wdg._on_press(t)
    wdg._on_motion(Tap(t.x, t.y - 5 * mod.PITCH_DRAG_PX - 1))
    wdg._on_release(t)
    wdg.update()
    check("Pitch: a finger up 5 steps", proc.controllers_dict["pitch"].value == 5 and "Pitch +5" in texts("tiles"))
    t = box_tap("fine")
    wdg.last_tap = (None, 0.0)
    wdg._on_press(t)
    wdg._on_motion(Tap(t.x, t.y + 300))
    wdg._on_release(t)
    wdg.update()
    check("Fine: a finger down, clamped to -100", proc.controllers_dict["fine"].value == -100)
    wdg._on_press(box_tap("pitch"))
    wdg._on_press(box_tap("pitch"))
    wdg._on_press(box_tap("fine"))
    wdg._on_press(box_tap("fine"))
    wdg.update()
    check("double tap resets Pitch and Fine", proc.controllers_dict["pitch"].value == 0 and
          proc.controllers_dict["fine"].value == 0 and "Pitch +0" in texts("tiles"))
    # the Fold tab draws the Source's wave through the shapers
    ctl.select(3)
    wdg.update()
    fold_va = [c.coords(t) for t in c.find_withtag("fold") if c.type(t) == "line" and c.itemcget(t, "fill") == mod.CURVE2_COLOR]
    proc.controllers_dict["source"].value = 0
    wdg.fold_drawn = 0.0
    wdg.update()
    fold_sine = [c.coords(t) for t in c.find_withtag("fold") if c.type(t) == "line" and c.itemcget(t, "fill") == mod.CURVE2_COLOR]
    check("Fold tab follows the Source", fold_va and fold_sine and fold_va != fold_sine)
    ctl.select(1)
    wdg.update()

    # the widget's engine formulas against the C engines (build_test/libengines.so, built by test.sh)
    lib_path = os.path.join(ROOT, "build_test", "libengines.so")
    if os.path.exists(lib_path):
        import ctypes
        lib = ctypes.CDLL(lib_path)
        F = ctypes.c_float

        class FeState(ctypes.Structure):
            _fields_ = [("ph", F * 2), ("p", F * 6), ("fb", F), ("w_prev", F), ("out", F), ("fresh", ctypes.c_int)]

        class FeBlock(ctypes.Structure):
            _fields_ = [("sine", ctypes.POINTER(F)), ("sine_n", ctypes.c_int), ("t", F * 6), ("glide", F),
                        ("fb_coef", F), ("wave_lp", F), ("wave_gain", F)]
        lib.fe_tick.restype = F
        lib.fe_tick.argtypes = [ctypes.POINTER(FeState), ctypes.POINTER(FeBlock), ctypes.c_int, F]
        lib.fe_block.argtypes = [ctypes.POINTER(FeBlock), ctypes.c_int, F, F, F, F, F, ctypes.POINTER(F), ctypes.c_int]
        sine_n = 2048
        table = (F * (sine_n + 1))(*[math.sin(2 * math.pi * i / sine_n) for i in range(sine_n + 1)])

        def c_engine(engine, knobs, hz, steps, count):
            st, blk = FeState(), FeBlock()
            lib.fe_reset(ctypes.byref(st))
            lib.fe_block(ctypes.byref(blk), engine, knobs[0], knobs[1], knobs[2], hz, hz * steps, table, sine_n)
            return [lib.fe_tick(ctypes.byref(st), ctypes.byref(blk), engine, 1.0 / steps) for _ in range(count)]

        for knobs in ((0.5, 1 / 3.0, 1 / 3.0), (0.622, 0.0, 0.8), (0.1, 0.95, 0.5)):
            # VA, 2048 samples a period: the same phases; only the samples next to a step differ (polyBLEP)
            cv = c_engine(0, knobs, mod.C4_HZ, 2048, 4096)
            pv = mod.principal_values(1, *knobs, 4096)[1:]
            err = sorted(abs(a - b) for a, b in zip(cv, pv))
            check("VA %s: the widget's wave is the engine's (95 %% within %.4f)" % (knobs, err[int(0.95 * len(err))]),
                  err[int(0.95 * len(err))] < 1e-3)
            # FM2, 256 samples a period, 2 periods of settling: the same steps
            cv = c_engine(1, knobs, mod.C4_HZ, 256, 4 * 256 + 1)[512:]
            pv = mod.principal_values(2, *knobs, 512)
            err = max(abs(a - b) for a, b in zip(cv, pv))
            check("FM2 %s: the widget's wave is the engine's (max %.5f)" % (knobs, err), err < 1e-3)
            # Wave, 128 samples a period (one table point a sample): the engine's mean over the sample
            hz = 48.0
            cv = c_engine(2, knobs, hz, 128, 256)[128:]
            gain = 0.95 - hz / 48000.0
            pv = [sum(mod.wave_value(mod.engine_tables()[0], mod.wave_coords(*knobs), (k + (j + 0.5) / 8) / 128.0)
                      for j in range(8)) / 8 for k in range(128, 256)]   # item k: phases k / 128 .. (k + 1) / 128
            err = max(abs(a / gain - b) for a, b in zip(cv, pv))
            check("Wave %s: the widget's wave is the engine's (max %.4f)" % (knobs, err), err < 0.01)
    else:
        print("Note: build_test/libengines.so not found (run scripts/test.sh), formulas not checked against C.")

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
    check("Timbre constants as in fold_core.h",
          (mod.FOLD_GAIN, mod.DRIVE_K) == (hdr("FC_FOLD_GAIN"), hdr("FC_DRIVE_K")) and
          mod.CHEB_MAX == int(re.search(r"#define FC_CHEB_MAX (\d+)", core_h).group(1)))
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
    check("FM lit by default, AM, Sync, Timbre not", routes() == {"route_fm": True, "route_am": False,
                                                                 "route_sync": False, "route_timbre": False})
    check("Timbre arrow drawn", "Timbre \u2192" in texts("mod"))

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
    tap_route("route_timbre")
    check("Timbre route on", proc.controllers_dict["route_timbre"].value == 1 and routes()["route_timbre"])
    tap_route("route_timbre")

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

    # ---------- Fold tab ----------
    # the shaper formulas: identity at 0, the Saw sum gives 1/n harmonics, M odd ones (a 256-point DFT)
    check("Timbre 0: no change", all(abs(mod.shape(x / 10.0, 0, 0, 0, 0) - x / 10.0) < 1e-12 for x in range(-10, 11)))

    def harmonics(fn, n=256, upto=6):
        ys = [fn(math.sin(2 * math.pi * i / n)) for i in range(n)]
        out = []
        for k in range(1, upto + 1):
            re_ = sum(y * math.cos(2 * math.pi * k * i / n) for i, y in enumerate(ys))
            im_ = sum(y * math.sin(2 * math.pi * k * i / n) for i, y in enumerate(ys))
            out.append(math.hypot(re_, im_) * 2 / n)
        return out
    hs = harmonics(lambda x: mod.shape(x, 1.0, 0.5, 0, 0))
    check("Saw: harmonics 1/n", all(abs(hs[k] / hs[0] - 1.0 / (k + 1)) < 1e-3 for k in range(1, 6)))
    hm = harmonics(lambda x: mod.shape(x, 1.0, 1.0, 0, 0))
    check("M: odd harmonics 1/n only", hm[1] < 1e-6 * hm[0] and hm[3] < 1e-6 * hm[0] and
          abs(hm[2] / hm[0] - 1 / 3) < 1e-3 and abs(hm[4] / hm[0] - 1 / 5) < 1e-3)
    hf = harmonics(lambda x: mod.shape(x, 0.5, 0.0, 0, 0))
    check("folder: odd harmonics only", hf[1] < 1e-6 and hf[3] < 1e-6 and hf[2] > 0.05)
    check("all bounded", max(abs(mod.shape(x / 50.0, 1, o / 4.0, sy, d)) for x in range(-50, 51)
                             for o in range(5) for sy in (-1, 0, 0.6) for d in (0, 1)) <= 1.0 + 1e-9)

    ctl.selected = []
    tx = (wdg.tab_boxes[2][0] + wdg.tab_boxes[2][1]) / 2
    wdg._on_press(Tap(tx, 5))
    wdg.update()
    check("tap on Fold: Fold tab and the Timbre page", wdg.tab == 2 and ctl.selected == [3] and
          texts("title") == ["Timbre"])
    ctl.select(1)
    wdg.update()
    ctl.select(3)
    wdg.update()
    check("Timbre page shows the Fold tab", wdg.tab == 2)

    def fold_lines():
        return [c.coords(i) for i in c.find_withtag("fold") if c.type(i) == "line" and len(c.coords(i)) > 8]
    ftexts = texts("fold")
    check("Fold tab: boxes, Order line, values", "In → Out" in ftexts and "Principal → Timbre" in ftexts and
          "Folder" in ftexts and "Saw" in ftexts and "M" in ftexts and
          ("Timbre 0%  Sym +0  Drive 0%" in ftexts or "T 0  S +0  D 0" in ftexts))
    curve, wave = fold_lines()
    xs, ys = curve[0::2], curve[1::2]
    # at the defaults the curve is the diagonal: y - cy == -(x - cx)
    cx, cy = (xs[0] + xs[-1]) / 2, (ys[0] + ys[-1]) / 2
    check("defaults: the transfer curve is the diagonal", max(abs((y - cy) + (x - cx)) for x, y in zip(xs, ys)) < 1.0)
    ids = c.find_withtag("fold")
    for _ in range(5):
        wdg.update()
    check("Fold tab: no redraw without changes", c.find_withtag("fold") == ids)

    def turns(v):
        d = [b - a for a, b in zip(v, v[1:]) if abs(b - a) > 1e-9]
        return sum(1 for a, b in zip(d, d[1:]) if a * b < 0)
    proc.controllers_dict["timbre"].value = 100.0
    time.sleep(mod.CURVE_PERIOD)
    wdg.update()
    curve, wave = fold_lines()
    check("Timbre 100 %: the curve folds (gain 8: turns at +-1, 3, 5, 7)", turns(curve[1::2]) == 8)
    def shown(t, sy, d):
        ft = texts("fold")
        return "Timbre %d%%  Sym %+d  Drive %d%%" % (t, sy, d) in ft or "T %d  S %+d  D %d" % (t, sy, d) in ft
    check("values follow", shown(100, 0, 0))
    proc.controllers_dict["timbre"].value = 50.0
    wdg.update()
    check("a second change at once waits", shown(100, 0, 0))
    proc.controllers_dict["symmetry"].value = 40.0
    time.sleep(mod.CURVE_PERIOD)
    wdg.update()
    curve, wave = fold_lines()
    ys = curve[1::2]
    check("Symmetry: the curve is no longer odd", abs((ys[0] - cy) + (ys[-1] - cy)) > 2.0)
    wy = wave[1::2]
    mid_y = (min(wy) + max(wy)) / 2
    x0, y0, x1, y1 = wdg._curve_box(wdg._fold_layout()[1])
    check("the wave is drawn without its DC", abs(sum(wy[:-1]) / (len(wy) - 1) - (y0 + y1) / 2) < 1.0)

    def order_mark():
        ovals = [c.coords(i) for i in c.find_withtag("fold") if c.type(i) == "oval" and c.itemcget(i, "fill") == mod.MARK_COLOR]
        return (ovals[0][0] + ovals[0][2]) / 2 if ovals else None
    ox0, oy0, ox1, oy1 = wdg._fold_layout()[2]
    check("Order mark at Folder", abs(order_mark() - ox0) < 1)
    proc.controllers_dict["order"].value = 75.0
    time.sleep(mod.CURVE_PERIOD)
    wdg.update()
    check("Order 75 %: mark between Saw and M", abs(order_mark() - (ox0 + 0.75 * (ox1 - ox0))) < 1)
    lit = [c.itemcget(i, "fill") for i in c.find_withtag("fold") if c.type(i) == "oval" and
           c.itemcget(i, "fill") != mod.MARK_COLOR]
    check("Order 75 %: Saw and M lit, Folder not", lit == [mod.ROUTE_OFF, mod.ROUTE_ON, mod.ROUTE_ON])
    for sym_, v in (("timbre", 0.0), ("order", 0.0), ("symmetry", 0.0)):
        proc.controllers_dict[sym_].value = v
    ctl.select(1)
    wdg.update()

    # ---------- Gate tab ----------
    check("lopass gate constants as in fold_core.h",
          (mod.LPG_FMIN, mod.LPG_FMAX, mod.DECAY_MIN_MS, mod.DECAY_MAX_MS) ==
          (hdr("FC_LPG_FMIN"), hdr("FC_LPG_FMAX"), hdr("FC_DECAY_MIN_MS"), hdr("FC_DECAY_MAX_MS")))
    check("response: VCA flat at the vactrol level",
          all(abs(mod.lpg_db(f, 0.5, 0, 0) - 20 * math.log10(0.5)) < 1e-9 for f in (50, 1000, 15000)))
    check("response: LP open, -3 dB at its cutoff (Butterworth)", abs(mod.lpg_db(20000, 1.0, 1, 0) + 3.01) < 0.05)
    check("response: Resonance 100 % peaks at the cutoff", mod.lpg_db(mod.lpg_cutoff(0.5, 1), 0.5, 1, 1.0) > 20)
    check("response: Combo at g 1/4 is quieter and darker than LP",
          mod.lpg_db(5000, 0.25, 2, 0) < mod.lpg_db(5000, 0.25, 1, 0) - 10)
    check("Decay 30 % = 49 ms", mod.ms_text(mod.decay_ms(0.3)) == "49 ms")

    ctl.selected = []
    tx = (wdg.tab_boxes[3][0] + wdg.tab_boxes[3][1]) / 2
    wdg._on_press(Tap(tx, 5))
    wdg.update()
    check("tap on Gate: Gate tab and page", wdg.tab == 3 and ctl.selected == [4] and texts("title") == ["Gate"])
    gtexts = texts("gate")
    check("Gate tab: buttons and values", "VCA" in gtexts and "LP" in gtexts and "Combo" in gtexts and
          "Decay 49 ms  Res 0%  Level 80%" in gtexts)

    def gate_lit():
        return [bool(c.itemcget(i, "fill")) for i in c.find_withtag("gate") if c.type(i) == "rectangle"][:3]
    check("Combo lit by default", gate_lit() == [False, False, True])
    curves = [i for i in c.find_withtag("gate") if c.type(i) == "line" and len(c.coords(i)) > 8]
    check("4 response curves", len(curves) == 4)
    ids = c.find_withtag("gate")
    for _ in range(5):
        wdg.update()
    check("Gate tab: no redraw without changes", c.find_withtag("gate") == ids)
    b = wdg.gate_buttons[0]
    wdg._on_press(Tap((b[0] + b[2]) / 2, (b[1] + b[3]) / 2))
    check("tap on VCA sets Gate Mode", proc.controllers_dict["gate_mode"].value == 0 and gate_lit() == [True, False, False])
    curves = [c.coords(i) for i in c.find_withtag("gate") if c.type(i) == "line" and len(c.coords(i)) > 8]
    check("VCA: flat curves", all(max(cv[1::2]) - min(cv[1::2]) < 0.5 for cv in curves))
    # live: the vactrol bar and the cutoff mark
    proc.engine.gate_level = 0.5
    wdg.update()
    buttons, plot, bar, bottom = wdg._gate_layout()
    live = c.find_withtag("gatelive")
    rects = [c.coords(i) for i in live if c.type(i) == "rectangle"]
    check("vactrol bar at half", len(rects) == 1 and abs((rects[0][3] - rects[0][1]) - (bar[3] - bar[1]) / 2) <= 2)
    check("VCA: no cutoff mark", not [i for i in live if c.type(i) == "polygon"])
    b = wdg.gate_buttons[2]
    wdg._on_press(Tap((b[0] + b[2]) / 2, (b[1] + b[3]) / 2))
    wdg.update()
    marks = [c.coords(i) for i in c.find_withtag("gatelive") if c.type(i) == "polygon"]
    want = wdg._fx(mod.lpg_cutoff(0.5, 2), plot[0], plot[2])
    check("Combo: the cutoff mark at g 1/2", len(marks) == 1 and abs(marks[0][4] - want) <= 1)
    proc.engine.gate_level = -1.0
    wdg.update()
    check("no voice: no bar, no mark", not c.find_withtag("gatelive"))

    # ---------- Env tab ----------
    check("FG shapes: Gate has a hold, Cycle two cycles",
          len(mod.fg_shape(5, 300, 1)) == 3 and len(mod.fg_shape(5, 300, 0)) == 2 and len(mod.fg_shape(5, 300, 2)) == 4)
    check("FG units: 1 ms = 1, 10 s = 5", mod.fg_units(1) == 1.0 and abs(mod.fg_units(10000) - 5.0) < 1e-12)
    ptr = mod.fg_point(0.5, 10, 100, 1)
    tot = 2.0 + mod.HOLD_W + 3.0
    check("FG point: half way up the rise", abs(ptr[0] - 0.5 * 2.0 / tot) < 1e-9 and ptr[1] == 0.5)
    ptf = mod.fg_point(2.25, 10, 100, 1)
    check("FG point: a quarter down the fall", abs(ptf[0] - (2.0 + mod.HOLD_W + 0.25 * 3.0) / tot) < 1e-9 and
          ptf[1] == 0.75)
    check("FG point: none when ended", mod.fg_point(3.0, 10, 100, 1) is None and mod.fg_point(-1, 10, 100, 1) is None)

    ctl.selected = []
    tx = (wdg.tab_boxes[4][0] + wdg.tab_boxes[4][1]) / 2
    wdg._on_press(Tap(tx, 5))
    wdg.update()
    check("tap on Env: Env tab and page", wdg.tab == 4 and ctl.selected == [5] and texts("title") == ["Env"])
    etexts = texts("env")
    check("Env tab: labels, times, depth", "FG1 → Gate" in etexts and "FG2 → Timbre" in etexts and
          "↗ 5 ms\n↘ 300 ms" in etexts and "Timbre +0%" in etexts)

    def env_curve(i):
        x0, y0, x1, y1 = wdg.env_curves[i]
        return [c.coords(j) for j in c.find_withtag("env") if c.type(j) == "line" and c.coords(j)[0] == x0 and
                c.coords(j)[1] == y1][0]                  # both rows start at the same x: the row's bottom too
    check("FG1 Gate: rise, hold, fall", len(env_curve(0)) == 8)

    def env_lit(row):
        return [bool(c.itemcget(j, "fill")) for j in c.find_withtag("env") if c.type(j) == "rectangle" and
                any(c.coords(j) == [float(v) for v in b[:4]] for b in wdg.env_buttons if b[4] == "fg%d_mode" % row)]
    check("Gate lit on both rows", env_lit(1) == [False, True, False] and env_lit(2) == [False, True, False])
    b = [b for b in wdg.env_buttons if b[4] == "fg1_mode" and b[5] == 0][0]
    wdg._on_press(Tap((b[0] + b[2]) / 2, (b[1] + b[3]) / 2))
    check("tap on Once: fg1_mode 0, no hold", proc.controllers_dict["fg1_mode"].value == 0 and len(env_curve(0)) == 6)
    b = [b for b in wdg.env_buttons if b[4] == "fg2_mode" and b[5] == 2][0]
    wdg._on_press(Tap((b[0] + b[2]) / 2, (b[1] + b[3]) / 2))
    check("tap on Cycle: fg2_mode 2, two cycles", proc.controllers_dict["fg2_mode"].value == 2 and len(env_curve(1)) == 10)
    # the running point
    proc.engine.fg = [0.5, -1.0]
    wdg.update()
    pts = c.find_withtag("envpt0")
    x0, y0, x1, y1 = wdg.env_curves[0]
    want = mod.fg_point(0.5, 5.0, 300.0, 0)
    check("FG1 point half way up", len(pts) == 1 and
          abs((c.coords(pts[0])[0] + c.coords(pts[0])[2]) / 2 - (x0 + want[0] * (x1 - x0))) <= 1 and
          abs((c.coords(pts[0])[1] + c.coords(pts[0])[3]) / 2 - (y1 - 0.5 * (y1 - y0))) <= 1)
    check("FG2 idle: no point", not c.find_withtag("envpt1"))
    proc.engine.fg = [2.5, 0.2]
    wdg.update()
    check("the point moves (same item)", c.find_withtag("envpt0") == pts and len(c.find_withtag("envpt1")) == 1)
    proc.engine.fg = [3.0, -1.0]
    wdg.update()
    check("ended: no points", not c.find_withtag("envpt"))
    # FG2 -> Timbre: a finger up, then a double tap
    bx = wdg.depth_box
    cx_, cy_ = (bx[0] + bx[2]) / 2, (bx[1] + bx[3]) / 2
    wdg.last_tap = (None, 0.0)
    wdg._on_press(Tap(cx_, cy_))
    wdg._on_motion(Tap(cx_, cy_ - 30))
    wdg._on_release(Tap(cx_, cy_ - 30))
    check("finger up 30 px: +20 %", proc.controllers_dict["m_fg2_timbre"].value == 20 and "Timbre +20%" in texts("env"))
    wdg.last_tap = (None, 0.0)
    wdg._on_press(Tap(cx_, cy_))
    wdg._on_release(Tap(cx_, cy_))
    wdg._on_press(Tap(cx_, cy_))
    wdg._on_release(Tap(cx_, cy_))
    check("double tap resets", proc.controllers_dict["m_fg2_timbre"].value == 0)
    for sym_, v in (("fg1_mode", 1), ("fg2_mode", 1), ("gate_mode", 2)):
        proc.controllers_dict[sym_].value = v
    ctl.select(1)
    wdg.update()

    # ---------- Matrix ----------
    ctl.selected = []
    tx = (wdg.tab_boxes[6][0] + wdg.tab_boxes[6][1]) / 2
    wdg._on_press(Tap(tx, 5))
    wdg.update()
    check("tap on Matrix: Matrix tab, the knob page stays", wdg.tab == 6 and ctl.selected == [] and
          texts("title") == ["Osc"])
    wdg.update()
    check("Matrix stays when the page did not change", wdg.tab == 6)
    mt = texts("matrix")
    check("Matrix: source and destination names", all(n in mt for n in ("FG1", "Rnd", "Wheel", "AT")) and
          (all(n in mt for n in ("Pitch", "Harm", "LPG")) or all(n in mt for n in ("Pit", "Hrm", "LPG"))))
    check("Matrix: 88 cells, Delay Time and Feedback columns", len(wdg.mx_items) == 88 and
          ("DTime" in mt or "DTm" in mt) and ("DFb" in mt))

    def cell_xy(si, di):
        gx0, gy0, gx1, gy1, cw, ch = wdg._mx_layout()[:6]
        return gx0 + (di + 0.5) * cw, gy0 + (si + 0.5) * ch

    def cell_fill(si, di):
        return c.itemcget(wdg.mx_items[(si, di)][0], "fill")

    def label_fill(si):
        return c.itemcget(wdg.mx_labels[si], "fill")
    empty = cell_fill(0, 0)
    off_label = label_fill(1)
    x, y = cell_xy(1, 7)                                 # FG2 -> Timbre
    wdg.last_tap = (None, 0.0)
    wdg._on_press(Tap(x, y))
    wdg._on_motion(Tap(x, y - 45))
    wdg._on_release(Tap(x, y - 45))
    check("Matrix: finger up 45 px on FG2 -> Timbre: +30 %", proc.controllers_dict["m_fg2_timbre"].value == 30)
    check("Matrix: the cell lit blue, its text, the source lit", cell_fill(1, 7) != empty and
          c.itemcget(wdg.mx_items[(1, 7)][1], "text") == "+30" and label_fill(1) != off_label)
    check("Matrix: the line tells the cell", "FG2 \u2192 Timbre +30%" in texts("matrix"))
    x, y = cell_xy(6, 0)                                 # Mod wheel -> Pitch, down: orange
    wdg.last_tap = (None, 0.0)
    wdg._on_press(Tap(x, y))
    wdg._on_motion(Tap(x, y + 75))
    wdg._on_release(Tap(x, y + 75))
    check("Matrix: wheel -> Pitch -50 %, orange, semitones told",
          proc.controllers_dict["m_wheel_pitch"].value == -50 and cell_fill(6, 0) != cell_fill(1, 7) and
          any("-6.0 semitones" in t for t in texts("matrix")))
    wdg._on_press(Tap(x, y))
    wdg._on_release(Tap(x, y))
    wdg._on_press(Tap(x, y))
    wdg._on_release(Tap(x, y))
    check("Matrix: double tap resets", proc.controllers_dict["m_wheel_pitch"].value == 0 and cell_fill(6, 0) == empty)
    tx = (wdg.tab_boxes[4][0] + wdg.tab_boxes[4][1]) / 2
    wdg._on_press(Tap(tx, 5))
    wdg.update()
    check("Env shows the same cell: Timbre +30%", wdg.tab == 4 and "Timbre +30%" in texts("env"))
    proc.controllers_dict["m_fg2_timbre"].value = 0
    ctl.select(1)
    wdg.update()

    # ---------- Delay ----------
    check("peaking EQ: the gain at its frequency, flat far away", abs(mod.peaking_db(1000.0, 1000.0, -6.0) + 6.0) < 1e-6
          and abs(mod.peaking_db(30.0, 1000.0, -6.0)) < 0.2 and abs(mod.peaking_db(1000.0, 1000.0, 0.0)) < 1e-9)
    check("loop shaper: straight at 0, tanh at 1, limited at 2", abs(mod.dl_shape(0.5, 0.0) - 2 * mod.fast_tanh(0.25))
          < 1e-9 and abs(mod.dl_shape(3.0, 1.0) - 2 * mod.fast_tanh(0.5)) < 1e-9 and
          abs(mod.dl_shape(0.01, 1.0) - 0.01) < 1e-5 and abs(mod.dl_shape(10, 0)) <= 2.0)
    check("Delay Time read by the mode", abs(mod.dl_semis(50) ) < 1e-9 and abs(mod.dl_semis(100) - 24) < 1e-9 and
          abs(mod.dl_ms(0) - 1) < 1e-9 and abs(mod.dl_ms(100) - 1000) < 1e-6 and abs(mod.dl_eq_hz(50) - 1000) < 1e-6)
    ctl.selected = []
    tx = (wdg.tab_boxes[5][0] + wdg.tab_boxes[5][1]) / 2
    wdg._on_press(Tap(tx, 5))
    wdg.update()
    check("tap on Delay: Delay tab and page", wdg.tab == 5 and ctl.selected == [6] and texts("title") == ["Delay"])
    dt = texts("delay")
    check("Delay: buttons, boxes, off line", all(t in dt for t in ("Off", "Tuned", "Free", "Shaper 30%", "EQ 1.00 kHz",
                                                                     "EQ +0.0 dB", "Delay off (no CPU)")))
    b = [b for b in wdg.dl_buttons if b[4] == 1][0]
    wdg._on_press(Tap((b[0] + b[2]) / 2, (b[1] + b[3]) / 2))
    proc.engine.notes = [57]
    wdg.dl_drawn = 0.0                                   # the curves are redrawn at most every CURVE_PERIOD
    wdg.update()
    dt = texts("delay")
    check("tap on Tuned: delay_mode 1, Time for the sounding note", proc.controllers_dict["delay_mode"].value == 1 and
          any(t.startswith("+0.0 st: 220") and "(A3)" in t and "Fb +50%" in t for t in dt))
    proc.controllers_dict["delay_mode"].value = 2
    proc.controllers_dict["delay_time"].value = 80.0
    wdg.dl_drawn = 0.0
    wdg.update()
    check("Free: Time in ms", any(t.startswith("251 ms") for t in texts("delay")))
    bx = [b for b in wdg.dl_boxes if b[4] == "delay_eq_gain"][0]
    cx_, cy_ = (bx[0] + bx[2]) / 2, (bx[1] + bx[3]) / 2
    wdg.last_tap = (None, 0.0)
    wdg._on_press(Tap(cx_, cy_))
    wdg._on_motion(Tap(cx_, cy_ + 5))
    wdg._on_release(Tap(cx_, cy_ + 5))
    check("EQ Gain: a finger down 5 px = -0.5 dB", proc.controllers_dict["delay_eq_gain"].value == -5 and
          "EQ -0.5 dB" in texts("delay"))
    bx = [b for b in wdg.dl_boxes if b[4] == "delay_shaper"][0]
    cx_, cy_ = (bx[0] + bx[2]) / 2, (bx[1] + bx[3]) / 2
    proc.controllers_dict["delay_shaper"].value = 80
    wdg.last_tap = (None, 0.0)
    for _ in range(2):
        wdg._on_press(Tap(cx_, cy_))
        wdg._on_release(Tap(cx_, cy_))
    check("Shaper: a double tap resets to 30 %", proc.controllers_dict["delay_shaper"].value == 30)
    proc.engine.delay_level = 0.1
    wdg.update()
    bar = [i for i in c.find_withtag("delaylive")]
    b0 = wdg._delay_layout()[3]
    check("the line's level bar: -20 dB = 2/3 up", len(bar) == 1 and
          abs((b0[3] - c.coords(bar[0])[1]) - (b0[3] - b0[1]) * 2 / 3) <= 2)
    proc.engine.delay_level = -1.0
    proc.engine.notes = []
    wdg.update()
    check("no voice: no bar", not c.find_withtag("delaylive"))
    for sym_, v in (("delay_mode", 0), ("delay_time", 50.0), ("delay_eq_gain", 0)):
        proc.controllers_dict[sym_].value = v
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
        tiles, wave, meter_box, notes_y, info_y = wdg._osc_layout()
        ok = tiles[3] < wave[1] and wave[3] < info_y - wdg._font_size() / 2 and info_y < notes_y - wdg._font_size() \
            and wave[2] < meter_box[0] and meter_box[2] <= wdg.width
        check("layout %s: tiles, wave, meter, info, notes do not overlap" % geo, ok)
        ok = wdg.tile_boxes[-1][2] < wdg.value_boxes[0][0] and wdg.value_boxes[0][2] < wdg.value_boxes[1][0] \
            and wdg.value_boxes[1][2] <= wdg.width and wdg.tile_boxes[0][2] - wdg.tile_boxes[0][0] > 20
        check("layout %s: Source tiles, Pitch and Fine side by side" % geo, ok)
        it = [i for i in c.find_withtag("info") if c.type(i) == "text"]
        check("layout %s: the knobs line fits" % geo, len(it) == 1 and c.bbox(it[0])[2] <= wdg.width)
        left, mid, right, bottom_y = wdg._mod_layout()
        ok = left[2] < mid[0] and mid[2] < right[0] and right[2] <= wdg.width and \
            max(left[3], mid[3], right[3]) < bottom_y - wdg._font_size() and left[1] >= wdg._header_h()
        check("layout %s: Mod blocks and bottom line do not overlap" % geo, ok)
        left, right, order, bottom_y = wdg._fold_layout()
        ok = left[2] < right[0] and right[2] <= wdg.width and max(left[3], right[3]) < order[1] and \
            order[3] <= wdg.height
        check("layout %s: Fold boxes and bottom line do not overlap" % geo, ok)
        buttons, plot, bar, bottom = wdg._gate_layout()
        ok = buttons[3] < plot[1] and plot[2] < bar[0] and bar[2] <= wdg.width and plot[3] < bottom - wdg._font_size() / 2
        check("layout %s: Gate buttons, plot, bar, bottom line do not overlap" % geo, ok)
        rows = wdg._env_layout()
        ok = all(lab[2] < cur[0] and cur[2] < btn[0] and btn[2] <= wdg.width for lab, cur, btn in rows) and \
            rows[0][1][3] < rows[1][1][1] and rows[1][1][3] <= wdg.height
        check("layout %s: Env rows do not overlap" % geo, ok)
        buttons, shaper, eq, bar, values, bottom = wdg._delay_layout()
        ok = buttons[3] < shaper[1] and shaper[2] < eq[0] and eq[2] < bar[0] and bar[2] <= wdg.width and \
            max(shaper[3], eq[3]) < values[1] and values[3] < bottom - wdg._font_size() / 2 and shaper[3] > shaper[1] + 10
        check("layout %s: Delay buttons, plots, bar, boxes, line do not overlap" % geo, ok)
        wdg.tab = 5                                      # the longest Delay line: Tuned, a sounding note, -100 %
        proc.controllers_dict["delay_mode"].value, proc.controllers_dict["delay_fb"].value = 1, -100.0
        proc.controllers_dict["delay_eq_gain"].value = -180
        proc.engine.notes = [61]
        wdg.next_update = 0.0
        wdg.update()
        wdg._clear()
        wdg.refresh_gui()
        lines = [i for i in c.find_withtag("delay") if c.type(i) == "text" and c.itemcget(i, "anchor") == "w"]
        boxes = [i for i in c.find_withtag("delay") if c.type(i) == "text" and c.itemcget(i, "text").startswith("EQ -")]
        ok = len(lines) == 1 and c.bbox(lines[0])[2] <= wdg.width and all(
            c.bbox(i)[2] - c.bbox(i)[0] <= (wdg.dl_boxes[2][2] - wdg.dl_boxes[2][0]) for i in boxes)
        check("layout %s: the Delay line and boxes fit" % geo, ok)
        if not ok:
            print("   ", [(c.itemcget(i, "text"), c.bbox(i)) for i in lines + boxes], wdg.dl_boxes[2])
        proc.controllers_dict["delay_mode"].value, proc.controllers_dict["delay_fb"].value = 0, 50.0
        proc.controllers_dict["delay_eq_gain"].value = 0
        proc.engine.notes = []
        wdg.tab = 0
        wdg._clear()
        gx0, gy0, gx1, gy1, cw, ch, info_y, small, names = wdg._mx_layout()
        ok = gx0 > 0 and gx1 <= wdg.width and gy0 > wdg._header_h() and gy1 < info_y and info_y < wdg.height and \
            ch >= 8 and cw >= 12
        check("layout %s: Matrix grid, names and line fit" % geo, ok)
        wdg.tab, wdg.mx_sel = 6, (7, 1)                  # the longest line: Aftertouch -> Harmonics -100 %
        proc.controllers_dict["m_at_harmonics"].value = -100
        wdg._clear()
        wdg.refresh_gui()
        ln = c.bbox(wdg.mx_line)
        wdg.mx_sel = (0, 0)
        proc.controllers_dict["m_fg1_pitch"].value = 100
        wdg.refresh_gui()
        ln2 = c.bbox(wdg.mx_line)
        wdg.mx_sel = (6, 9)                              # Mod wheel -> Delay Time +100 %
        proc.controllers_dict["m_wheel_dtime"].value = 100
        wdg.refresh_gui()
        ln3 = c.bbox(wdg.mx_line)
        proc.controllers_dict["m_wheel_dtime"].value = 0
        check("layout %s: the Matrix line fits" % geo, ln[2] <= wdg.width and ln2[2] <= wdg.width and
              ln3[2] <= wdg.width)
        if ln3[2] > wdg.width:
            print("   %s: %s" % (c.itemcget(wdg.mx_line, "text"), ln3))
        if ln2[2] > wdg.width:
            print("   %s: %s" % (c.itemcget(wdg.mx_line, "text"), ln2))
        proc.controllers_dict["m_at_harmonics"].value = proc.controllers_dict["m_fg1_pitch"].value = 0
        wdg.mx_sel = None
        wdg.tab = 0
        wdg._clear()
        # the values text stays right of the Order line, at every size, with the widest values
        ctl.select(3)
        proc.controllers_dict["timbre"].value, proc.controllers_dict["symmetry"].value = 100.0, -100.0
        proc.controllers_dict["drive"].value = 100.0
        wdg.fold_view = None
        wdg.next_update = 0.0                  # the throttle check above left a pending period
        wdg.update()
        vt = [i for i in c.find_withtag("fold") if c.type(i) == "text" and c.itemcget(i, "anchor") == "e"]
        ok = len(vt) == 1 and c.bbox(vt[0])[0] > order[2] and c.bbox(vt[0])[2] <= wdg.width
        check("layout %s: Fold values right of the Order line" % geo, ok)
        if not ok:
            print("   tab %d, values %s, order line to %d" % (wdg.tab, [(c.itemcget(i, "text"), c.bbox(i)) for i in vt],
                                                          order[2]))
        proc.controllers_dict["timbre"].value = proc.controllers_dict["symmetry"].value = 0.0
        proc.controllers_dict["drive"].value = 0.0
        ctl.select(1)
        wdg.next_update = 0.0
        wdg.update()

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
    # the Osc and Fold tabs for each Source (the wave is computed in Python; Wave reads 8 waves a point)
    times = []
    for src in range(len(mod.SOURCES)):
        proc.controllers_dict["source"].value = src
        for page in (1, 3):
            ctl.select(page)
            wdg.next_update = 0.0
            wdg.update()
            t0 = time.perf_counter()
            for i in range(10):
                wdg._clear()
                wdg.refresh_gui()
                root.update_idletasks()
            times.append((time.perf_counter() - t0) / 10 * 1000)
    print("Osc / Fold full redraw on this PC: " + ", ".join("%s %.1f / %.1f ms" % (n, times[2 * i], times[2 * i + 1])
                                                          for i, n in enumerate(mod.SOURCES)))
    proc.controllers_dict["source"].value = 0
    ctl.select(1)
    wdg.update()
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
    ctl.select(3)
    proc.controllers_dict["timbre"].value = 70.0
    proc.controllers_dict["order"].value = 30.0
    wdg.update()
    t0 = time.perf_counter()
    for i in range(20):
        wdg._clear()
        wdg.refresh_gui()
        root.update_idletasks()
    full = (time.perf_counter() - t0) / 20 * 1000
    print("Fold tab on this PC: %.3f ms full redraw (folder + Saw: the slowest curves)" % full)
    for tab_page, name in ((4, "Gate"), (5, "Env"), (6, "Delay")):
        ctl.select(tab_page)
        wdg.next_update = 0.0
        wdg.update()
        t0 = time.perf_counter()
        for i in range(20):
            wdg._clear()
            wdg.refresh_gui()
            root.update_idletasks()
        full = (time.perf_counter() - t0) / 20 * 1000
        t0 = time.perf_counter()
        for i in range(200):
            proc.engine.gate_level = (i % 40) / 40.0
            proc.engine.fg = [(i % 30) / 10.0, ((i + 7) % 30) / 10.0]
            proc.engine.delay_level = (i % 50) / 50.0
            wdg.update()
            root.update_idletasks()
        live = (time.perf_counter() - t0) / 200 * 1000
        print("%s tab on this PC: %.3f ms full redraw, %.3f ms with its live marks moving" % (name, full, live))
    # Matrix: a full redraw (72 cells), and a refresh with nothing changed (it reads the 72 values)
    wdg.tab = 6
    wdg._clear()
    for k, sym in enumerate(sorted(s for s in proc.controllers_dict if s.startswith("m_"))):
        proc.controllers_dict[sym].value = (k * 37) % 201 - 100
    t0 = time.perf_counter()
    for i in range(20):
        wdg._clear()
        wdg.refresh_gui()
        root.update_idletasks()
    full = (time.perf_counter() - t0) / 20 * 1000
    t0 = time.perf_counter()
    for i in range(200):
        wdg.refresh_gui()
        root.update_idletasks()
    idle = (time.perf_counter() - t0) / 200 * 1000
    print("Matrix tab on this PC: %.3f ms full redraw (all 88 cells set), %.3f ms without changes" % (full, idle))

    root.destroy()
    print("All widget tests passed" if not fails else "%d FAILED" % fails)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
