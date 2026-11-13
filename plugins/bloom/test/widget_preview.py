#!/usr/bin/python3
# Runs the Eka Bloom widget outside Zynthian with stub modules.
#   python3 test/widget_preview.py          scripted touch tests (PASS/FAIL)
#   python3 test/widget_preview.py --show   open a window to try it by hand (needs a display)
import importlib.util
import math
import os
import struct
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
        self.refresh_gui()

    def set_processor(self, processor):
        self.processor = processor


base = types.ModuleType("zyngui.zynthian_widget_base")
base.zynthian_widget_base = _WidgetBase
pkg = types.ModuleType("zyngui")
pkg.zynthian_gui_config, pkg.zynthian_widget_base = cfg, base
sys.modules.update({"zyngui": pkg, "zyngui.zynthian_gui_config": cfg, "zyngui.zynthian_widget_base": base})


class Ctrl:
    def __init__(self, sym, value, lo, hi, ticks=None):
        self.symbol, self.value, self.lo, self.hi, self.ticks = sym, value, lo, hi, ticks
        self.writes = 0

    def set_value(self, v, send=True):
        assert self.ticks is None or v in self.ticks, "%s: %r not a valid tick" % (self.symbol, v)
        assert self.lo <= v <= self.hi, "%s: %r out of range" % (self.symbol, v)
        assert isinstance(v, int), "%s: %r is not int" % (self.symbol, v)
        self.value = v
        self.writes += 1


class FakeViz:
    """Shared memory as the plugin writes it (see Viz in eka_bloom.c), with a toy arpeggio."""
    HDR = struct.Struct("<6Id32s")
    NOTE = struct.Struct("<ddBBB5x")

    def __init__(self):
        self.pid = 900000 + os.getpid() % 90000
        self.path = "/dev/shm/eka_bloom_%d" % self.pid
        self.clk, self.held, self.notes, self.seq = 0.0, [], [], 0
        self.write()

    def write(self, magic=0x4D4F4C42):
        buf = bytearray(self.HDR.size + 512 * self.NOTE.size)
        head = len(self.notes)
        self.HDR.pack_into(buf, 0, magic, self.seq, head, 4, len(self.held), 1, self.clk,
                           bytes(self.held + [0] * (32 - len(self.held))))
        for i, (on, off, note, vel) in enumerate(self.notes[-512:]):
            k = (head - min(head, 512) + i) % 512
            self.NOTE.pack_into(buf, self.HDR.size + k * self.NOTE.size, on, off, note, vel, 0)
        with open(self.path, "wb") as f:
            f.write(buf)

    def play_to(self, clk, chord=(60, 64, 67), step=240, ratch=None):
        """Up arpeggio (gate 50 %) up to clk; ratch: {step number: hits}."""
        self.held = list(chord)
        t = 0.0 if not self.notes else self.notes[-1][0]
        k = int(round(t / step)) + (1 if self.notes else 0)
        while k * step <= clk:
            hits = (ratch or {}).get(k % 16, 1)
            for j in range(hits):
                on = k * step + j * step / hits
                if on > clk:
                    break
                off = on + step / hits / 2
                self.notes.append((on, off if off <= clk else -1.0, chord[k % len(chord)], 100))
            k += 1
        self.clk = clk
        self.write()

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
        self.step_count = 7
        self.dev = {}             # step -> (prob, shift, vel, gate) drift in notches

    def get_monitors_dict(self):
        m = {"step_count": self.step_count}
        for i in range(1, 17):
            p, s, v, g = self.dev.get(i, (0, 0, 0, 0))
            m["dev%d" % i] = float((p + 3) + 7 * (s + 3) + 49 * (v + 3) + 343 * (g + 4))   # as pack_dev()
        return m


class Processor:
    def __init__(self):
        gate_ticks = [0] + list(range(5, 101, 5)) + [101]
        self.engine = Engine()
        self.id = 1
        self.controllers_dict = {k: Ctrl(k, 16, 1, 16) for k in ("len_prob", "len_shift", "len_vel", "len_gate", "len_ratch")}
        self.controllers_dict["key"] = Ctrl("key", 0, 0, 12)
        self.controllers_dict["scale"] = Ctrl("scale", 0, 0, 11)
        self.controllers_dict["gate"] = Ctrl("gate", 50, 5, 100)
        for k, v, lo, hi in (("latch", 0, 0, 1), ("rate", 3, 0, 5), ("order", 0, 0, 6), ("octaves", 1, 1, 4),
                             ("transpose", 0, -2, 2), ("swing", 50, 50, 75), ("mut_steps", 0, 0, 100),
                             ("mut_notes", 0, 0, 100), ("reset", 0, 0, 1)):
            self.controllers_dict[k] = Ctrl(k, v, lo, hi)
        for i in range(1, 17):
            self.controllers_dict["prob%d" % i] = Ctrl("prob%d" % i, 100, 0, 100)
            self.controllers_dict["shift%d" % i] = Ctrl("shift%d" % i, 0, -24, 24)
            self.controllers_dict["vel%d" % i] = Ctrl("vel%d" % i, 100, 0, 200)
            self.controllers_dict["gate%d" % i] = Ctrl("gate%d" % i, 0, 0, 101, gate_ticks)
            self.controllers_dict["ratch%d" % i] = Ctrl("ratch%d" % i, 1, 1, 4)

    def v(self, sym):
        return self.controllers_dict[sym].value


class Control:
    def __init__(self, root):
        self.main_frame = root
        self.layout = {"rows": 4}
        self.wide = True
        self.shown = True


def load_widget():
    path = os.path.join(ROOT, "ui", "zynthian_widget_eka_bloom.py")
    spec = importlib.util.spec_from_file_location("zynthian_widget_eka_bloom", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class Ev:
    def __init__(self, x, y):
        self.x, self.y = x, y


def main():
    show = "--show" in sys.argv
    mod = load_widget()
    root = tkinter.Tk()
    root.geometry("480x300")
    proc = Processor()
    wdg = mod.zynthian_widget_eka_bloom(Control(root))
    wdg.set_processor(proc)
    wdg.pack(fill="both", expand=True)
    root.update()
    wdg.update()
    root.update()

    if show:
        def tick():
            proc.engine.step_count += 1
            v = proc.engine.viz
            v.play_to(v.clk + 60, ratch={3: 2, 7: 3, 13: 4})
            wdg.update()
            root.after(62, tick)
        tick()
        root.mainloop()
        proc.engine.viz.remove()
        return 0

    fails = 0

    def check(name, cond):
        nonlocal fails
        print("%s: %s" % ("PASS" if cond else "FAIL", name))
        fails += 0 if cond else 1

    w, h, pad, tab_h, num_h, x0, x1, y0, y1, cw = wdg.geo
    cx = lambda i: int(x0 + (i + 0.5) * cw)
    tab = lambda i: int(w / 7.0 * (i + 0.5))

    check("widget sized to the window", w >= 400 and h >= 250)

    # Prob: tap at the bottom of step 1 -> 0, top of step 2 -> 100, middle of step 3 -> ~50
    wdg._on_press(Ev(cx(0), y1)); wdg._on_release(Ev(cx(0), y1))
    wdg._on_press(Ev(cx(1), y0)); wdg._on_release(Ev(cx(1), y0))
    wdg._on_press(Ev(cx(2), (y0 + y1) // 2)); wdg._on_release(None)
    check("prob: tap sets value by height (0 / 100 / ~50)",
          proc.v("prob1") == 0 and proc.v("prob2") == 100 and 45 <= proc.v("prob3") <= 55)

    # swipe across steps 5..9 at 25% height (fast: press at 5, one motion event at 9)
    y25 = int(y1 - 0.25 * (y1 - y0))
    wdg._on_press(Ev(cx(4), y25)); wdg._on_motion(Ev(cx(8), y25)); wdg._on_release(None)
    check("prob: fast swipe paints every crossed step",
          all(20 <= proc.v("prob%d" % i) <= 30 for i in range(5, 10)) and proc.v("prob10") == 100)

    # double tap resets to default
    time.sleep(0.01)
    time.sleep(DT)
    wdg._on_press(Ev(cx(0), y1)); wdg._on_release(None)
    first = proc.v("prob1")
    wdg._on_press(Ev(cx(0), y1)); wdg._on_release(None)
    check("double tap resets the step to default (1st tap 0, 2nd -> 100)", first == 0 and proc.v("prob1") == 100)
    time.sleep(DT)
    wdg._on_press(Ev(cx(0), y1)); wdg._on_release(None)
    time.sleep(DT)
    wdg._on_press(Ev(cx(0), y1)); wdg._on_release(None)
    check("two slow taps are not a double tap", proc.v("prob1") == 0)

    # Shift tab: middle = 0, top = +24, bottom = -24
    wdg._on_press(Ev(tab(1), tab_h // 2))
    check("tab switches lane", wdg.lane == 1)
    wdg._on_press(Ev(cx(0), y0)); wdg._on_release(None)            # right after a tap on step 1 in Prob
    check("tap on another lane right after is not a double tap", proc.v("shift1") == 24 and proc.v("prob1") == 0)
    time.sleep(DT)
    wdg._on_press(Ev(cx(1), y1)); wdg._on_release(None)
    time.sleep(DT)
    wdg._on_press(Ev(cx(2), (y0 + y1) / 2.0)); wdg._on_release(None)
    check("shift: top +24, bottom -24, middle 0",
          proc.v("shift1") == 24 and proc.v("shift2") == -24 and proc.v("shift3") == 0)

    # Gate tab: bottom = Global (0), top = Tie (101), values snap to valid ticks
    wdg._on_press(Ev(tab(3), tab_h // 2))
    wdg._on_press(Ev(cx(0), y0)); wdg._on_release(None)
    time.sleep(DT)
    wdg._on_press(Ev(cx(1), y1)); wdg._on_release(None)
    time.sleep(DT)
    wdg._on_press(Ev(cx(2), (y0 + y1) // 2)); wdg._on_release(None)
    g3 = proc.v("gate3")
    check("gate: top Tie, bottom Global, middle a 5% tick",
          proc.v("gate1") == 101 and proc.v("gate2") == 0 and g3 in range(5, 101, 5))

    # Vel tab
    wdg._on_press(Ev(tab(2), tab_h // 2))
    wdg._on_press(Ev(cx(0), (y0 + y1) / 2.0)); wdg._on_release(None)
    check("vel: middle = 100%", proc.v("vel1") == 100)

    # step number row sets the length of the open row only
    wdg._on_press(Ev(tab(1), tab_h // 2))                         # Shift
    wdg._on_press(Ev(cx(2), h - num_h // 2))
    check("tap on step number 3 in Shift sets Shift length 3, others unchanged",
          proc.v("len_shift") == 3 and proc.v("len_prob") == 16 and proc.v("len_vel") == 16 and proc.v("len_gate") == 16)
    wdg._on_press(Ev(tab(2), tab_h // 2))                         # Vel
    wdg._on_press(Ev(cx(4), h - num_h // 2))
    check("tap on step number 5 in Vel sets Vel length 5", proc.v("len_vel") == 5)

    # no redundant writes: same value written again does nothing
    before = proc.controllers_dict["len_vel"].writes
    wdg._on_press(Ev(cx(4), h - num_h // 2))
    check("writing the same value again sends nothing", proc.controllers_dict["len_vel"].writes == before)

    # playhead per row: step_count 7 -> Vel (len 5): step 2, Shift (len 3): step 1, Prob (len 16): step 7
    proc.engine.step_count = 7
    texts = lambda: [wdg.widget_canvas.itemcget(t, "text") for t in wdg.widget_canvas.find_withtag("dyn")
                     if wdg.widget_canvas.type(t) == "text"]
    def playhead_col():
        c = wdg.widget_canvas
        for t in c.find_withtag("dyn"):
            if c.type(t) == "rectangle" and c.itemcget(t, "outline") == "#ffffff":
                x = c.coords(t)[0]
                return int((x + 2 - x0) / cw) + 1
        return 0
    wdg.update(); p_vel = playhead_col()
    wdg._on_press(Ev(tab(1), tab_h // 2)); wdg.update(); p_shift = playhead_col()
    wdg._on_press(Ev(tab(0), tab_h // 2)); wdg.update(); p_prob = playhead_col()
    check("playhead per row from step_count 7: Vel(5)=2, Shift(3)=1, Prob(16)=7 (got %d %d %d)" % (p_vel, p_shift, p_prob),
          (p_vel, p_shift, p_prob) == (2, 1, 7))
    check("tabs show every row length", {"Prob 16", "Shift 3", "Vel 5", "Gate 16"} <= set(texts()))
    proc.engine.step_count = 0; wdg.update()
    check("no playhead when idle", playhead_col() == 0)

    # Shift tab shows the scale it sticks to (nothing for Chromatic)
    wdg._on_press(Ev(tab(1), tab_h // 2)); wdg.update()
    check("Shift tab, Chromatic: no scale label", not any("minor" in t or "major" in t or "semitones" in t for t in texts()))
    proc.controllers_dict["key"].value = 9; proc.controllers_dict["scale"].value = 2; wdg.update()
    check("Shift tab, A minor: 'A minor'", "A minor" in texts())
    proc.controllers_dict["key"].value = 12; wdg.update()
    check("Shift tab, Key Auto: 'Auto minor'", "Auto minor" in texts())
    proc.controllers_dict["key"].value = 9; wdg.update()
    # with a scale, Shift values stick to the scale intervals (minor: 0 2 3 5 7 8 10 12 ...)
    ys = lambda v: y1 - (v + 24) / 48.0 * (y1 - y0)
    got = []
    for raw in (1, 4, 6, 11, -1, -4, 13):
        time.sleep(DT)
        wdg._on_press(Ev(cx(5), ys(raw))); wdg._on_release(None)
        got.append(proc.v("shift6"))
    check("A minor: Shift sticks to scale intervals (1,4,6,11,-1,-4,13 -> %s)" % got,
          all(v % 12 in (0, 2, 3, 5, 7, 8, 10) for v in got) and got[1] in (3, 5) and got[3] in (10, 12))
    proc.controllers_dict["scale"].value = 0
    time.sleep(DT)
    wdg._on_press(Ev(cx(5), ys(1))); wdg._on_release(None)
    check("Chromatic: any semitone (1)", proc.v("shift6") == 1)
    proc.controllers_dict["scale"].value = 2
    wdg._on_press(Ev(tab(0), tab_h // 2)); wdg.update()
    check("other tabs don't show the scale", "A minor" not in texts())

    # changing the scale moves Shift steps into the new scale
    def set_shifts(vals):
        for i, v in enumerate(vals):
            proc.controllers_dict["shift%d" % (i + 1)].value = v
    def shifts(n):
        return [proc.v("shift%d" % (i + 1)) for i in range(n)]
    def to_scale(sc):
        proc.controllers_dict["scale"].value = sc; wdg.update()
    to_scale(2); set_shifts([3, 8, -2, 12, 24, -24, 0, 10, 5, -9])
    to_scale(1)   # minor -> major: degree kept
    got = shifts(10)
    check("minor -> major keeps degrees (%s)" % got, got == [4, 9, -1, 12, 24, -24, 0, 11, 5, -8])
    to_scale(2)   # back: major -> minor
    got = shifts(10)
    check("major -> minor keeps degrees (%s)" % got, got == [3, 8, -2, 12, 24, -24, 0, 10, 5, -9])
    to_scale(3)   # minor -> dorian: only b6 changes
    got = shifts(10)
    check("minor -> dorian (%s)" % got, got == [3, 9, -2, 12, 24, -24, 0, 10, 5, -9])
    to_scale(9)   # dorian -> major pent.: nearest
    got = shifts(10)
    check("-> major pent. snaps to 0 2 4 7 9 (%s)" % got,
          all(v % 12 in (0, 2, 4, 7, 9) for v in got) and got[:4] == [2, 9, -3, 12])
    to_scale(0)
    set_shifts([1, 6, -1, 11]); before = shifts(10)
    to_scale(0); check("Chromatic -> Chromatic: unchanged", shifts(10) == before)
    to_scale(2)
    got = shifts(4)
    check("Chromatic -> minor: nearest (%s)" % got, got == [0, 5, 0, 10])
    to_scale(0); set_shifts([1, 6])
    check("-> Chromatic: unchanged", shifts(2) == [1, 6])
    # a new widget (e.g. after a snapshot load) doesn't rewrite on its first redraw
    proc.controllers_dict["scale"].value = 2
    w2 = mod.zynthian_widget_eka_bloom(Control(root)); w2.set_processor(proc)
    w2.width, w2.height = wdg.width, wdg.height; w2.update()
    check("first redraw of a new widget doesn't touch Shift", shifts(2) == [1, 6])
    w2.destroy(); wdg.last_scale = 2

    # mutation: a white mark where a drifted step plays now
    c = wdg.widget_canvas
    def marks():
        return [c.coords(t) for t in c.find_withtag("drift")]
    def mark_y_of(col):
        for x_0, y_, x_1, _ in marks():
            if x_0 <= cx(col) <= x_1:
                return y_
        return None
    yv = lambda lane, v: y1 - wdg._frac(lane, v) * (y1 - y0)
    for k in range(1, 17):
        proc.controllers_dict["prob%d" % k].value = 100
        proc.controllers_dict["vel%d" % k].value = 100
        proc.controllers_dict["gate%d" % k].value = 0
        proc.controllers_dict["shift%d" % k].value = 0
    for k in ("len_prob", "len_shift", "len_vel", "len_gate"):
        proc.controllers_dict[k].value = 16
    proc.engine.dev = {}
    wdg._on_press(Ev(tab(2), tab_h // 2)); wdg.update()
    check("no drift: no marks", marks() == [])
    proc.engine.dev = {3: (-2, 1, 2, -1), 5: (0, 0, -3, 4)}
    wdg.update()
    check("Vel tab: marks on drifted steps 3 and 5 only", len(marks()) == 2)
    check("Vel step 3: +2 notches -> 140%", abs(mark_y_of(2) - yv(2, 140)) < 1.5)
    check("Vel step 5: -3 notches -> 40%", abs(mark_y_of(4) - yv(2, 40)) < 1.5)
    wdg._on_press(Ev(tab(0), tab_h // 2)); wdg.update()
    check("Prob tab: step 3 -2 -> 60%, step 5 has no prob drift", len(marks()) == 1 and abs(mark_y_of(2) - yv(0, 60)) < 1.5)
    wdg._on_press(Ev(tab(3), tab_h // 2)); wdg.update()
    check("Gate tab: Global 50% -1 -> 45%, Global +4 -> 70%",
          abs(mark_y_of(2) - yv(3, 45)) < 1.5 and abs(mark_y_of(4) - yv(3, 70)) < 1.5)
    wdg._on_press(Ev(tab(1), tab_h // 2))
    proc.controllers_dict["scale"].value = 0; wdg.update()
    check("Shift tab, Chromatic: +1 notch -> +1", abs(mark_y_of(2) - yv(1, 1)) < 1.5)
    proc.controllers_dict["shift3"].value = 3
    proc.controllers_dict["scale"].value = 2; wdg.last_scale = 2; wdg.update()
    check("Shift tab, minor: +3 with +1 notch -> +5", abs(mark_y_of(2) - yv(1, 5)) < 1.5)
    proc.controllers_dict["len_shift"].value = 2; wdg.update()
    check("steps beyond the row length show no marks", marks() == [])
    proc.controllers_dict["len_shift"].value = 16
    proc.engine.dev = {}
    proc.controllers_dict["scale"].value = 0; wdg.last_scale = 0
    proc.controllers_dict["shift3"].value = 0

    # Ratch tab: bottom = 1 hit, top = 4 hits, each step drawn as that many slim bars
    wdg._on_press(Ev(tab(4), tab_h // 2)); wdg.update()
    check("Ratch tab selected", wdg.lane == 4)
    time.sleep(DT); wdg._on_press(Ev(cx(0), y0 + 1)); wdg._on_release(None)
    time.sleep(DT); wdg._on_press(Ev(cx(1), y1 - 1)); wdg._on_release(None)
    time.sleep(DT); wdg._on_press(Ev(cx(2), int(y0 + (y1 - y0) * 0.5))); wdg._on_release(None)
    check("ratch: top 4, bottom 1, middle 2 (got %d %d %d)" % (proc.v("ratch1"), proc.v("ratch2"), proc.v("ratch3")),
          (proc.v("ratch1"), proc.v("ratch2"), proc.v("ratch3")) == (4, 1, 2))
    wdg.update()
    hits = len(wdg.widget_canvas.find_withtag("hit"))
    check("ratch: one bar per hit (4 + 1 + 2 + 13 x 1 = 20, got %d)" % hits, hits == 20)
    check("ratch: labels x4 and x2", {"x4", "x2"} <= set(texts()))
    time.sleep(DT); wdg._on_press(Ev(cx(0), y1 - 1)); wdg._on_press(Ev(cx(0), y1 - 1)); wdg._on_release(None)
    check("ratch: double tap resets to 1", proc.v("ratch1") == 1)
    wdg._on_press(Ev(cx(5), h - 3))
    check("ratch: tap on step number 6 sets Ratch length 6", proc.v("len_ratch") == 6)
    check("Ratch tab shows its length", "Ratch 6" in texts())
    proc.engine.dev = {3: (1, 1, 1, 1)}; wdg.update()
    check("Ratch tab: no drift marks", marks() == [])
    proc.engine.dev = {}
    proc.controllers_dict["len_ratch"].value = 16

    # Roll tab: notes from shared memory
    viz = proc.engine.viz
    notes = lambda: wdg.widget_canvas.find_withtag("note")
    wdg._on_press(Ev(tab(5), tab_h // 2)); wdg.update()
    check("Roll tab selected, empty: 'play a chord'", wdg.lane == 5 and "play a chord" in texts() and len(notes()) == 0)
    check("tabs: 5 rows and Roll", "Roll" in texts() and "Ratch 16" in texts())
    viz.play_to(1919, ratch={3: 2, 5: 4})            # 8 steps of 1/16 (half a bar), step 3 x2, step 5 x4
    wdg.update()
    check("Roll: 8 steps + 1 + 3 extra hits = 12 notes (got %d)" % len(notes()), len(notes()) == 12)
    xs = sorted(wdg.widget_canvas.coords(n)[0] for n in notes())
    nx0 = min(xs)
    check("Roll: first note at the window start, 2 bars", "2 bars" in texts() and abs(nx0 - xs[0]) < 1e-6)
    check("Roll: held keys lit on the key strip", len(wdg.widget_canvas.find_withtag("held")) == 3)
    check("Roll: octave label C4", "C4" in texts())
    px = wdg.widget_canvas.coords(wdg.widget_canvas.find_withtag("playhead")[0])[0]
    check("Roll: playhead at 1/4 after half a bar of 2", abs((px - nx0) / (wdg.geo[6] - nx0) - 0.25) < 0.02)
    # second window: the notes of the last pass stay dim ahead of the playhead
    viz.play_to(7680 + 960)                           # a beat into bars 3-4: steps 32..36
    wdg.update()
    bright = mod.dim(mod.ROLL_COLOR, 0.35 + 0.65 * 100 / 127.0)
    dimc = mod.dim(mod.ROLL_COLOR, (0.35 + 0.65 * 100 / 127.0) * 0.35)
    px = wdg.widget_canvas.coords(wdg.widget_canvas.find_withtag("playhead")[0])[0]
    br = [n for n in notes() if wdg.widget_canvas.itemcget(n, "fill") == bright]
    dm = [n for n in notes() if wdg.widget_canvas.itemcget(n, "fill") == dimc]
    check("Roll: new pass bright (5 notes) behind the playhead, last pass dim ahead of it (got %d / %d)"
          % (len(br), len(dm)), len(br) == 5 and len(dm) == 31 and   # steps 4..31 after tick 960, step 5 x4
          all(wdg.widget_canvas.coords(n)[0] <= px for n in br) and
          all(wdg.widget_canvas.coords(n)[0] >= px - 1 for n in dm))
    check("Roll: bar numbers of the song (3, 4)", {"3", "4"} <= set(texts()))
    wdg._on_press(Ev(cx(4), int((y0 + y1) / 2))); wdg.update()
    check("Roll: tap -> 4 bars", wdg.roll_bars == 4 and "4 bars" in texts())
    wdg._on_press(Ev(cx(4), int((y0 + y1) / 2)))
    wdg._on_press(Ev(cx(4), int((y0 + y1) / 2))); wdg.update()
    check("Roll: tap -> 1 bar -> 2 bars", wdg.roll_bars == 2)
    check("Roll: taps don't touch the steps", proc.v("prob5") == 100 and proc.v("ratch5") == 1)
    viz.notes.append((viz.clk - 10, -1.0, 72, 127)); viz.held = [60, 64, 67, 72]; viz.write(); wdg.update()
    check("Roll: a sounding note is drawn up to the playhead, range grows to C5", "C5" in texts())
    viz.seq = 1; viz.write(); wdg.update()
    check("Roll: plugin busy (odd seq): no crash", True)
    viz.seq = 2; viz.write(magic=0); wdg.update()
    check("Roll: wrong magic: 'no data from the plugin'", "no data from the plugin" in texts())
    viz.remove(); wdg.mm = None; wdg.mm_path = None; wdg.update()
    check("Roll: no shared memory: 'no data from the plugin'", "no data from the plugin" in texts())
    wdg._on_press(Ev(tab(0), tab_h // 2)); wdg.update()

    # Play tab: big tiles; left half -, right half +
    wdg._on_press(Ev(tab(6), tab_h // 2)); wdg.update()
    tiles = wdg._tile_rects()
    def tap(k, side):
        a, b, c_, d = tiles[k]
        wdg._on_press(Ev(int(a + (c_ - a) * (0.25 if side < 0 else 0.75)), int((b + d) / 2))); wdg.update()
    check("Play tab: 10 tiles", wdg.lane == 6 and len(wdg.widget_canvas.find_withtag("tile")) == 10)
    tap(0, 1); check("Latch tap: on", proc.v("latch") == 1 and "On" in texts())
    tap(0, -1); check("Latch tap again: off", proc.v("latch") == 0)
    tap(1, 1); check("Rate +: 1/16 -> 1/16T", proc.v("rate") == 4 and "1/16T" in texts())
    tap(1, 1); tap(1, 1); check("Rate + stops at 1/32", proc.v("rate") == 5)
    tap(1, -1); check("Rate -: 1/16T", proc.v("rate") == 4)
    tap(2, -1); check("Order - at Up stays Up", proc.v("order") == 0)
    tap(2, 1); check("Order +: Down", proc.v("order") == 1 and "Down" in texts())
    tap(3, 1); check("Octaves +: 2", proc.v("octaves") == 2)
    tap(4, 1); check("Transpose +: +1 oct", proc.v("transpose") == 1 and "+1 oct" in texts())
    tap(5, 1); check("Gate +: 55%", proc.v("gate") == 55 and "55%" in texts())
    proc.controllers_dict["gate"].value = 52; wdg.update()
    tap(5, 1); check("Gate 52 (from an encoder) +: snaps to 50 then 55", proc.v("gate") == 55)
    tap(6, 1); check("Swing +: 55%", proc.v("swing") == 55)
    tap(7, 1); tap(7, 1); check("Mutate +2: 20%", proc.v("mut_steps") == 20)
    proc.controllers_dict["mut_notes"].value = 30
    tap(8, 1); check("Freeze: mutation 0, remembered", proc.v("mut_steps") == 0 and proc.v("mut_notes") == 0
                     and wdg.frozen == (20, 30))
    tap(8, 1); check("Freeze again: back to 20 / 30", proc.v("mut_steps") == 20 and proc.v("mut_notes") == 30
                     and wdg.frozen is None)
    tap(8, 1); proc.controllers_dict["mut_steps"].value = 40; wdg.update()
    check("Freeze is dropped when mutation is turned on elsewhere", wdg.frozen is None)
    r0 = proc.v("reset"); tap(9, 1)
    check("New loop: Reset toggles (plugin resets on any change)", proc.v("reset") == 1 - r0)
    check("New loop tile flashes", any(wdg.widget_canvas.itemcget(i, "fill") == mod.PLAY_COLOR
                                       for i in wdg.widget_canvas.find_withtag("tile")))
    before = {k: proc.v(k) for k in ("prob1", "ratch1", "len_prob")}
    wdg._on_press(Ev(cx(3), h - 3)); wdg.update()
    check("Play: taps don't touch steps or lengths", before == {k: proc.v(k) for k in before})
    for k in ("latch", "rate", "order", "octaves", "transpose", "gate", "swing", "mut_steps", "mut_notes"):
        proc.controllers_dict[k].value = {"latch": 0, "rate": 3, "order": 0, "octaves": 1, "transpose": 0,
                                          "gate": 50, "swing": 50}.get(k, 0)
    wdg._on_press(Ev(tab(0), tab_h // 2)); wdg.update()

    # drawing does not crash with the playing step and resizes
    for cs in (0, 1, 16, 1000):
        proc.engine.step_count = cs
        wdg.update()
    root.geometry("320x180"); root.update(); wdg.update(); root.update()
    check("redraw at another size and step counts 0/1/16/1000", True)

    # tab labels fit their tabs at every size
    for geo in ("320x180", "480x300", "800x480"):
        root.geometry(geo); root.update(); wdg.update(); root.update()
        cv = wdg.widget_canvas
        tw6 = wdg.geo[0] / 7.0
        labs = [i for i in cv.find_withtag("dyn") if cv.type(i) == "text" and cv.coords(i)[1] < wdg.geo[3]]
        widths = [cv.bbox(i)[2] - cv.bbox(i)[0] for i in labs]
        check("%s: 7 tab labels fit their tabs (widest %d of %d px)" % (geo, max(widths), tw6),
              len(labs) == 7 and max(widths) <= tw6 - 4)
        wdg.lane = 6; wdg.update(); root.update()
        bad = 0
        for (ta, tb, tc, td) in wdg._tile_rects():
            for i in cv.find_overlapping(ta + 1, tb + 1, tc - 1, td - 1):
                if cv.type(i) == "text":
                    bx = cv.bbox(i)
                    o = bx[0] < ta or bx[2] > tc or bx[1] < tb - 1 or bx[3] > td + 1
                    bad += o
                    if o: print("   out:", repr(cv.itemcget(i, "text")), bx, (ta, tb, tc, td))
        check("%s: Play tile texts stay inside their tiles" % geo, bad == 0)
        wdg.lane = 0

    proc.engine.viz.remove()
    print("\n%s" % ("ALL PASSED" if fails == 0 else "%d FAILED" % fails))
    root.destroy()
    return 1 if fails else 0


DT = 0.4    # pause longer than the double-tap window between taps on different lanes/steps

if __name__ == "__main__":
    sys.exit(main())
