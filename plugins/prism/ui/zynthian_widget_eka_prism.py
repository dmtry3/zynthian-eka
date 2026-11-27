#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# SPDX-License-Identifier: GPL-3.0-or-later
# Zynthian widget for Eka Prism (class and file name: zynthian_widget_eka_prism).
# Unified Eka / GEMINI.md Touch-First UX Design:
#   1. Top interactive Dual-Filter Frequency Response Graph (Filter A, Filter B, Combined curve,
#      draggable Cutoff x Resonance nodes A & B with double-tap reset, live modulated markers).
#   2. Middle interactive Morph A & Morph B strip (height 30..38 px, double-tap reset).
#   3. Right vertical peak-hold LEVEL meters (IN | ENV | OUT) with peak hold bars.
#   4. Bottom 6 large two-line touch buttons (height 36..44 px, gap 6 px):
#      Main view: PLAY FX | MODEL A | MODEL B | ROUTE | LFO | LINK* (short tap cycles Link B, long press toggles BYPASS).
#      Play FX view: MAIN | LINK A+B | HOLD/SNAP 0 | ZERO 0 | ROUTE | PHASE B* (long press toggles BYPASS).
from collections import deque
import math
import time
import tkinter

from zyngui import zynthian_gui_config
from zyngui import zynthian_widget_base

MULTI_INSTANCE = True

REFRESH_SEC = 0.06
KNOB_ACTIVE_HOLD_SEC = 0.30
KNOB_MONITOR_SEC = 0.25
DRAG_INTERVAL = 0.033
DOUBLE_TAP_SEC = 0.35
LONG_PRESS_SEC = 0.45
CURVE_BINS = 60
TRAIL_STEPS = 28

SPRING_DT_MS = 33
SPRING_XY_OMEGA = 8.2
SPRING_XY_ZETA = 0.74
SPRING_SCREAM_OMEGA = 6.5
SPRING_SCREAM_ZETA = 1.0

PARAM_KEYS = (
    "model_a", "cutoff_a", "res_a", "morph_a",
    "model_b", "cutoff_b", "res_b", "morph_b",
    "routing", "link_mode", "ab_balance", "invert_b",
    "lfo_shape", "lfo_rate", "lfo_sync", "lfo_phase",
    "lfo_mod", "morph_mod", "env_mod", "env_speed",
    "drive", "mix", "output_db", "bypass",
    "sweep_fx", "fx_link", "morph_fx", "scream_fx", "fx_zero", "fx_smooth",
)

DEFAULTS = {
    "cutoff_a": 650.0, "res_a": 35.0, "morph_a": 0.0,
    "cutoff_b": 1800.0, "res_b": 40.0, "morph_b": 25.0,
    "sweep_fx": 0.0, "morph_fx": 0.0, "scream_fx": 0.0, "fx_smooth": 50.0,
}

F_MIN = 20.0
F_MAX = 18000.0
RATE_MIN = 0.05
RATE_MAX = 40.0
DB_SPAN = 24.0
FLOOR_DB = -60.0

FONT_6B  = ("TkDefaultFont", 6, "bold")
FONT_7B  = ("TkDefaultFont", 7, "bold")
FONT_8B  = ("TkDefaultFont", 8, "bold")
FONT_9B  = ("TkDefaultFont", 9, "bold")
FONT_10B = ("TkDefaultFont", 10, "bold")

RATE_DIVISIONS = (
    ("4 bars", 16.0),
    ("2 bars", 8.0),
    ("1 bar",  4.0),
    ("1/2.",   3.0),
    ("1/2",    2.0),
    ("1/4.",   1.5),
    ("1/2T",   4.0 / 3.0),
    ("1/4",    1.0),
    ("1/8.",   0.75),
    ("1/4T",   2.0 / 3.0),
    ("1/8",    0.5),
    ("1/8T",   1.0 / 3.0),
    ("1/16",   0.25),
    ("1/16T",  1.0 / 6.0),
    ("1/32",   0.125),
    ("1/64",   1.0 / 16.0),
)

# Semantic colors matching GEMINI.md & Eka family
MODELS = (
    ("SVF",     "#ff922b"),
    ("LADDER",  "#ffd43b"),
    ("MS-20",   "#ff6b6b"),
    ("COMB",    "#63e6be"),
    ("FORMANT", "#cc5de8"),
    ("PHASER",  "#4dabf7"),
)

ROUTINGS = (
    ("SERIAL A>B",   "#63e6be"),
    ("PARALLEL A+B", "#ffd43b"),
    ("STEREO L/R",   "#4dabf7"),
)

LINKS = (
    ("FREE",   "#8b949e"),
    ("OFFSET", "#ff922b"),
    ("MIRROR", "#cc5de8"),
)

LFO_SHAPES = (
    ("SINE",  "#ffd43b"),
    ("TRI",   "#63e6be"),
    ("SAW",   "#ff922b"),
    ("SQR",   "#4dabf7"),
    ("S&H",   "#ff8787"),
    ("CHAOS", "#cc5de8"),
    ("STEP8", "#51cf66"),
)

VOWEL_FREQS = (
    (730.0, 1090.0, 2440.0),
    (530.0, 1840.0, 2480.0),
    (270.0, 2290.0, 3010.0),
    (570.0,  840.0, 2410.0),
    (300.0,  870.0, 2240.0),
)

# Unified Zynthian / Eka dark palette
PANEL_BG   = "#16191e"
BUTTON_BG  = "#1f242c"
TRACK_BG   = "#111419"
LINE_DIM   = "#333644"
GRID_COL   = "#262833"
ZERO_COL   = "#3a4352"
LABEL_DIM  = "#8b949e"
LABEL_HI   = "#e6edf3"

COL_A      = "#ff922b"   # Filter A orange accent (GEMINI.md Filter section)
COL_B      = "#4dabf7"   # Filter B blue accent (GEMINI.md Macro/Morph section)
COL_SUM    = "#f0f6fc"   # Combined response curve
COL_ENV    = "#51cf66"   # Env green accent (GEMINI.md Env section)
COL_LFO    = "#ffd43b"   # LFO yellow accent (GEMINI.md LFO section)
COL_FX     = "#cc5de8"   # Play FX purple accent
HOT_RED    = "#ff6b6b"   # FX / clip / invert coral accent
FILL_SUM   = "#1c2d3d"
FILL_A     = "#362516"
FILL_B     = "#1b2e42"

BYP_DIM    = "#49505a"
BYP_MID    = "#68707c"
BYP_HI     = "#9aa2ae"
BYP_FILL   = "#23272e"

_LOG_F_SPAN = math.log(F_MAX / F_MIN)
_CURVE_US = tuple(i / float(CURVE_BINS) for i in range(CURVE_BINS + 1))
_CURVE_FREQS = tuple(F_MIN * math.exp(u * _LOG_F_SPAN) for u in _CURVE_US)


def _clamp(v, lo, hi):
    return lo if v < lo else (hi if v > hi else v)


def _fmt_freq(f):
    if f >= 1000.0:
        return f"{f / 1000.0:.2f}k" if f < 10000.0 else f"{f / 1000.0:.1f}k"
    return f"{f:.0f}Hz"


def _fmt_rate(hz):
    if hz >= 10.0:
        return f"{hz:.1f}Hz"
    return f"{hz:.2f}Hz"


def nearest_rate_division(hz, bpm):
    bpm = _clamp(bpm, 20.0, 400.0)
    q_sec = 60.0 / bpm
    log_t = math.log(_clamp(hz, RATE_MIN, RATE_MAX))
    best_lbl, best_hz, best_err = "1/4", hz, 1e9
    for lbl, mult in RATE_DIVISIONS:
        cand = 1.0 / (q_sec * mult)
        if cand < 0.03 or cand > 45.0:
            continue
        err = abs(math.log(cand) - log_t)
        if err < best_err:
            best_err = err
            best_lbl = lbl
            best_hz = cand
    return best_lbl, best_hz


def _morph_mode_label(model, morph_pct):
    m = _clamp(morph_pct, 0.0, 100.0)
    if model == 0:  # SVF
        if m < 18.0: return "LP"
        if m < 48.0: return "BP"
        if m < 82.0: return "HP"
        return "NOTCH"
    if model == 1:  # Ladder
        if m < 20.0: return "LP24"
        if m < 50.0: return "LP12"
        if m < 82.0: return "BP4"
        return "HP2"
    if model == 2:  # MS-20
        if m < 25.0: return "LP"
        if m < 75.0: return "BP"
        return "HP"
    if model == 3:  # Comb
        return f"COMB- {m:.0f}%" if m < 50.0 else f"COMB+ {m:.0f}%"
    if model == 4:  # Formant
        vowels = ("A", "E", "I", "O", "U")
        idx = min(4, int(round((m / 100.0) * 4.0)))
        return f"VOWEL {vowels[idx]}"
    return "PHASER-" if m < 50.0 else "PHASER+"


def _eval_model_db(model, f_arr, fc, res_pct, morph_pct):
    """Analytic frequency response magnitude in dB for each of the 6 filter models."""
    fc = _clamp(fc, F_MIN, F_MAX)
    res = _clamp(res_pct, 0.0, 100.0) * 0.01
    morph = _clamp(morph_pct, 0.0, 100.0) * 0.01
    q = 0.65 + 14.0 * (res ** 1.6)
    out = []

    if model == 0:  # SVF Morph (LP -> BP -> HP -> Notch)
        m3 = morph * 3.0
        for f in f_arr:
            r = f / fc
            r2 = r * r
            den = math.sqrt((1.0 - r2) ** 2 + (r / q) ** 2) + 1e-6
            lp = 1.0 / den
            bp = (r / max(q * 0.45, 0.7)) / den
            hp = r2 / den
            notch = abs(1.0 - r2) / den
            if m3 <= 1.0:
                mag = lp * (1.0 - m3) + bp * m3
            elif m3 <= 2.0:
                t = m3 - 1.0
                mag = bp * (1.0 - t) + hp * t
            else:
                t = m3 - 2.0
                mag = hp * (1.0 - t) + notch * t
            out.append(_clamp(20.0 * math.log10(max(mag, 0.025)), -DB_SPAN, DB_SPAN))
        return out

    if model == 1:  # Ladder 24 dB
        m3 = morph * 3.0
        peak_q = 0.6 + 16.0 * (res ** 1.5)
        for f in f_arr:
            r = f / fc
            r2 = r * r
            r4 = r2 * r2
            res_bump = (1.0 + 3.2 * res) / (math.sqrt((1.0 - r2) ** 2 + (r / peak_q) ** 2) + 1e-5)
            lp24 = (1.0 / math.sqrt(1.0 + r4)) * (0.55 + 0.45 * res_bump)
            lp12 = (1.0 / math.sqrt(1.0 + r2)) * (0.60 + 0.40 * res_bump)
            bp = (r2 / (1.0 + r4)) * (1.2 + 0.6 * res_bump)
            hp = (r2 / math.sqrt(1.0 + r4)) * (0.65 + 0.35 * res_bump)
            if m3 <= 1.0:
                mag = lp24 * (1.0 - m3) + lp12 * m3
            elif m3 <= 2.0:
                t = m3 - 1.0
                mag = lp12 * (1.0 - t) + bp * t
            else:
                t = m3 - 2.0
                mag = bp * (1.0 - t) + hp * t
            out.append(_clamp(20.0 * math.log10(max(mag, 0.025)), -DB_SPAN, DB_SPAN))
        return out

    if model == 2:  # MS-20 Screamer
        m2 = morph * 2.0
        in_lp = _clamp(1.0 - m2, 0.0, 1.0)
        in_bp = 1.0 - abs(m2 - 1.0)
        in_hp = _clamp(m2 - 1.0, 0.0, 1.0)
        sq = 0.7 + 18.0 * (res ** 1.35)
        for f in f_arr:
            r = f / fc
            r2 = r * r
            den = math.sqrt((1.0 - r2) ** 2 + (r / sq) ** 2) + 1e-6
            growl = 1.0 + 0.35 * res * math.exp(-((r - 0.72) ** 2) * 14.0)
            mag = ((in_lp + in_bp * r * 1.15 + in_hp * r2) / den) * growl
            out.append(_clamp(20.0 * math.log10(max(mag, 0.025)), -DB_SPAN, DB_SPAN))
        return out

    if model == 3:  # Comb Resonator
        pol = (morph - 0.5) * 2.0
        sign = 1.0 if pol >= 0.0 else -1.0
        fb = sign * (0.25 + 0.68 * (res ** 0.65))
        fc_comb = _clamp(fc, 30.0, 4000.0)
        for f in f_arr:
            phase = 2.0 * math.pi * min(f / fc_comb, 14.0)
            c_ph = math.cos(phase)
            s_ph = math.sin(phase)
            den = math.sqrt((1.0 - fb * c_ph) ** 2 + (fb * s_ph) ** 2) + 1e-5
            mag = 0.45 + 0.55 / den
            out.append(_clamp(20.0 * math.log10(max(mag, 0.025)), -DB_SPAN, DB_SPAN))
        return out

    if model == 4:  # Formant Vowel A-E-I-O-U
        vpos = morph * 4.0
        v0 = min(int(vpos), 3)
        v1 = v0 + 1
        vf = vpos - v0
        shift = _clamp((fc / 1000.0) ** 0.55, 0.35, 2.8)
        fq = 3.2 + 14.0 * (res ** 1.3)
        peaks = [
            (VOWEL_FREQS[v0][b] * (1.0 - vf) + VOWEL_FREQS[v1][b] * vf) * shift
            for b in range(3)
        ]
        gains = (1.15, 0.90, 0.65)
        for f in f_arr:
            mag = 0.14
            for b in range(3):
                r = f / max(peaks[b], 40.0)
                r2 = r * r
                den = math.sqrt((1.0 - r2) ** 2 + (r / fq) ** 2) + 1e-5
                mag += gains[b] * (r / max(fq * 0.35, 0.8)) / den
            out.append(_clamp(20.0 * math.log10(max(mag, 0.025)), -DB_SPAN, DB_SPAN))
        return out

    # Model 5: 6-Stage Phaser
    spread = 0.65 + 0.75 * abs(morph - 0.5) * 2.0
    depth = 0.45 + 0.50 * res
    notches = (fc * (spread ** -0.9), fc, fc * (spread ** 0.9))
    for f in f_arr:
        mag = 1.0 + 0.35 * res
        for nf in notches:
            dist = math.log(max(f, 20.0) / max(nf, 20.0))
            if morph < 0.5:
                mag *= 1.0 - depth * math.exp(-(dist * dist) * 18.0)
            else:
                mag += depth * 1.15 * math.exp(-(dist * dist) * 18.0)
        out.append(_clamp(20.0 * math.log10(max(mag, 0.025)), -DB_SPAN, DB_SPAN))
    return out


class zynthian_widget_eka_prism(zynthian_widget_base.zynthian_widget_base):
    def __init__(self, parent):
        super().__init__(parent)
        self._last_monitor_ts = 0.0
        self._last_param_ts = 0.0
        self._last_update_params = None
        self.trail = deque(maxlen=TRAIL_STEPS)
        self._last_trail_pt = None

        self.selected_chain = "a"  # "a" or "b", synced with Zynthian page selection
        self.last_group = None
        self.last_tap = (None, 0.0)

        self.hold_in = FLOOR_DB
        self.hold_out = FLOOR_DB
        self.hold_env = 0.0
        self._telemetry = (FLOOR_DB, FLOOR_DB, 650.0, 1800.0, 0.0, 25.0, 0.0, 0.0, 0.0, 120.0)

        self.graph_box = (0.0, 0.0, 1.0, 1.0)
        self.morph_a_box = (0.0, 0.0, 1.0, 1.0)
        self.morph_b_box = (0.0, 0.0, 1.0, 1.0)
        self.xy_box = (0.0, 0.0, 1.0, 1.0)
        self.fx_sliders = {}   # sym -> (x0, y0, x1, y1, bipolar)
        self.fx_view = False   # False = Dual Filter Graph, True = Play / Live Effects screen
        self.fx_snap = False   # False = HOLD, True = SNAP 0 (elastic 2nd-order spring-return)
        self._spring = {}      # sym -> {"pos", "vel", "last_sent", "omega", "zeta", "bipolar"}
        self._spring_job = None
        self._spring_last_t = None
        self.node_pos = {}
        self.hit_rects = {}
        self._last_ctrl_titles = {}
        self.drag_mode = None
        self._last_drag_ts = 0.0
        self._pending_drag = None

        self._scene_key = None
        self._geom = {}
        self._ui = {}
        self._cache = {}

        self.widget_canvas = tkinter.Canvas(
            self,
            highlightthickness=0,
            relief="flat",
            bg=zynthian_gui_config.color_bg,
        )
        self.widget_canvas.grid(row=0, column=0, sticky="news")
        self.widget_canvas.bind("<ButtonPress-1>", self._on_press)
        self.widget_canvas.bind("<B1-Motion>", self._on_motion)
        self.widget_canvas.bind("<ButtonRelease-1>", self._on_release)
        self.widget_canvas.bind("<Destroy>", self._on_destroy)

    def _on_destroy(self, _event=None):
        self._cancel_spring()

    def _cancel_spring(self, syms=None):
        if syms is None:
            self._spring.clear()
        else:
            for s in syms:
                self._spring.pop(s, None)
        if not self._spring and self._spring_job is not None:
            try:
                if hasattr(self.widget_canvas, "after_cancel"):
                    self.widget_canvas.after_cancel(self._spring_job)
            except Exception:
                pass
            self._spring_job = None
            self._spring_last_t = None

    def _start_spring(self, syms):
        started = False
        for sym in syms:
            bipolar = sym in ("sweep_fx", "morph_fx")
            cur = self._get(sym, 0.0)
            if abs(cur) < 0.35:
                self._spring.pop(sym, None)
                self._set(sym, 0.0)
                continue
            self._spring[sym] = {
                "pos": float(cur),
                "vel": 0.0,
                "last_sent": float(cur),
                "omega": SPRING_XY_OMEGA if bipolar else SPRING_SCREAM_OMEGA,
                "zeta": SPRING_XY_ZETA if bipolar else SPRING_SCREAM_ZETA,
                "bipolar": bipolar,
            }
            started = True
        if started:
            self._spring_last_t = time.monotonic()
            self._schedule_spring()
        elif not self._spring:
            self._cancel_spring()

    def _schedule_spring(self):
        if self._spring_job is not None or not self._spring:
            return
        if not hasattr(self.widget_canvas, "after"):
            return
        try:
            self._spring_job = self.widget_canvas.after(SPRING_DT_MS, self._on_spring_tick)
        except Exception:
            self._spring_job = None

    def _step_spring(self, dt):
        if not self._spring:
            return False
        dt = _clamp(float(dt), 0.005, 0.08)
        sub_dt = dt / 4.0
        done = []
        changed = False
        for sym, st in list(self._spring.items()):
            cur = self._get(sym, st["last_sent"])
            if abs(cur - st["last_sent"]) > 2.0:
                done.append(sym)
                continue
            pos = st["pos"]
            vel = st["vel"]
            omega = st["omega"]
            zeta = st["zeta"]
            bipolar = st["bipolar"]
            for _ in range(4):
                acc = -(omega * omega) * pos - 2.0 * zeta * omega * vel
                vel += acc * sub_dt
                pos += vel * sub_dt
                if not bipolar and pos < 0.0:
                    pos = 0.0
                    vel = 0.0
                    break
            if abs(pos) < 0.35 and abs(vel) < 1.5:
                self._set(sym, 0.0)
                done.append(sym)
                changed = True
            else:
                lo = -100.0 if bipolar else 0.0
                val = round(_clamp(pos, lo, 100.0), 1)
                st["pos"] = pos
                st["vel"] = vel
                st["last_sent"] = val
                self._set(sym, val)
                changed = True
        for sym in done:
            self._spring.pop(sym, None)
        if not self._spring:
            self._spring_last_t = None
        return changed

    def _on_spring_tick(self):
        self._spring_job = None
        if not self._spring:
            return
        now = time.monotonic()
        dt = (now - self._spring_last_t) if self._spring_last_t is not None else (SPRING_DT_MS * 0.001)
        self._spring_last_t = now
        self._step_spring(dt)
        try:
            self.refresh_gui(sample_telemetry=False)
        except Exception:
            self._cancel_spring()
            return
        if self._spring:
            self._schedule_spring()

    def _read_param_snapshot(self):
        proc = self.processor
        if proc is None:
            return None
        ctrls = getattr(proc, "controllers_dict", None)
        if not ctrls:
            return None
        return tuple(ctrls[k].value if k in ctrls else None for k in PARAM_KEYS)

    def update(self):
        if self.processor is None:
            return
        now = time.monotonic()
        snap = self._read_param_snapshot()
        params_changed = (snap != self._last_update_params)
        if params_changed:
            self._last_update_params = snap
            self._last_param_ts = now

        knob_active = (self.drag_mode is not None) or ((now - self._last_param_ts) < KNOB_ACTIVE_HOLD_SEC)
        interval = KNOB_MONITOR_SEC if knob_active else REFRESH_SEC

        if (now - self._last_monitor_ts) >= interval:
            self._last_monitor_ts = now
            super().update()
        elif params_changed and self._scene_key is not None:
            self.refresh_gui(sample_telemetry=False)

    def set_processor(self, processor):
        if hasattr(super(), "set_processor"):
            super().set_processor(processor)
        else:
            self.processor = processor
        self._last_update_params = None
        self._last_monitor_ts = 0.0

    def on_size(self, event):
        if not super().on_size(event):
            return
        self.widget_canvas.grid(row=0, column=0, sticky="news")
        self._scene_key = None
        self.refresh_gui(sample_telemetry=False)

    def _get_zctrl(self, sym):
        if not self.processor:
            return None
        ctrls = getattr(self.processor, "controllers_dict", None)
        if not ctrls:
            return None
        if sym in ctrls:
            return ctrls[sym]
        # Fallback suffix/name match per GEMINI.md rule 6
        for k, zc in ctrls.items():
            if k.endswith("_" + sym) or getattr(zc, "symbol", "") == sym:
                return zc
        return None

    def _get(self, sym, fallback):
        zctrl = self._get_zctrl(sym)
        if zctrl is None or zctrl.value is None:
            return fallback
        try:
            return float(zctrl.value)
        except (TypeError, ValueError):
            return fallback

    def _get_mon(self, sym, fallback):
        if not isinstance(self.monitors, dict):
            return fallback
        val = self.monitors.get(sym)
        if val is None:
            return fallback
        try:
            v = float(val)
            return v if math.isfinite(v) else fallback
        except (TypeError, ValueError):
            return fallback

    def _set(self, sym, val):
        zctrl = self._get_zctrl(sym)
        if zctrl is None:
            return False
        if zctrl.value != val:
            if hasattr(zctrl, "set_value"):
                zctrl.set_value(val)
            else:
                zctrl.value = val
            return True
        return False

    def _select_group(self, group_sym, group_name=None):
        self.last_group = group_sym
        if group_sym == "g_flt_a":
            self.selected_chain = "a"
        elif group_sym == "g_flt_b":
            self.selected_chain = "b"
        ctrl_screen = getattr(self, "zyngui_control", None)
        if ctrl_screen is None:
            return
        list_data = getattr(ctrl_screen, "list_data", None)
        if not list_data:
            return
        for idx, row in enumerate(list_data):
            if not row:
                continue
            if len(row) >= 4 and row[3] not in (None, self.processor):
                continue
            if row[0] == group_sym or (group_name and len(row) >= 3 and row[2] == group_name):
                if getattr(ctrl_screen, "selected_index", -1) != idx and hasattr(ctrl_screen, "select"):
                    try:
                        ctrl_screen.select(idx)
                    except Exception:
                        pass
                return

    def _follow_page(self):
        """Two-way sync with Zynthian page selector per GEMINI.md rule 3."""
        ctrl_screen = getattr(self, "zyngui_control", None)
        if ctrl_screen is None:
            return
        try:
            info = getattr(ctrl_screen, "screen_info", None)
            if info and len(info) >= 4 and info[3] is self.processor:
                grp = info[0]
                if grp != self.last_group:
                    self.last_group = grp
                    if grp == "g_flt_a":
                        self.selected_chain = "a"
                    elif grp == "g_flt_b":
                        self.selected_chain = "b"
        except Exception:
            pass

    def _sync_encoder_titles(self, sync_on, div_lbl):
        ctrl_screen = getattr(self, "zyngui_control", None)
        if ctrl_screen is None:
            return
        zctrls = getattr(ctrl_screen, "zgui_controllers", None)
        if not zctrls:
            return
        target = f"Rate {div_lbl}" if sync_on else "Rate"
        rate_zc = self._get_zctrl("lfo_rate")
        for zgui in zctrls:
            zc = getattr(zgui, "zctrl", None)
            if zc is not None and (zc is rate_zc or getattr(zc, "short_name", "") in ("Rate", "lfo_rate")):
                if self._last_ctrl_titles.get("lfo_rate") != target and hasattr(zgui, "set_title"):
                    zgui.set_title(target)
                    self._last_ctrl_titles["lfo_rate"] = target

    def _cfg(self, key, **kwargs):
        iid = self._ui.get(key)
        if iid is None:
            return
        prev = self._cache.get(key)
        if prev == kwargs:
            return
        if prev is None:
            self._cache[key] = dict(kwargs)
            self.widget_canvas.itemconfig(iid, **kwargs)
        else:
            diff = {k: v for k, v in kwargs.items() if prev.get(k) != v}
            if diff:
                prev.update(diff)
                self.widget_canvas.itemconfig(iid, **diff)

    def _coords(self, key, *pts):
        iid = self._ui.get(key)
        if iid is None:
            return
        ck = ("_c", key)
        if self._cache.get(ck) != pts:
            self._cache[ck] = pts
            self.widget_canvas.coords(iid, *pts)

    def _freq_to_x(self, f, gx0, gx1):
        u = math.log(_clamp(f, F_MIN, F_MAX) / F_MIN) / _LOG_F_SPAN
        return gx0 + u * (gx1 - gx0)

    def _x_to_freq(self, x, gx0, gx1):
        u = _clamp((x - gx0) / max(1.0, gx1 - gx0), 0.0, 1.0)
        return F_MIN * math.exp(u * _LOG_F_SPAN)

    def _db_to_y(self, db, gy0, gy1):
        u = 0.5 - (_clamp(db, -DB_SPAN, DB_SPAN) / (2.0 * DB_SPAN))
        return gy0 + u * (gy1 - gy0)

    def _res_to_y(self, res_pct, gy0, gy1):
        u = 1.0 - _clamp(res_pct, 0.0, 100.0) * 0.01
        return gy0 + (0.12 + 0.76 * u) * (gy1 - gy0)

    def _y_to_res(self, y, gy0, gy1):
        u = (y - gy0) / max(1.0, gy1 - gy0)
        norm = 1.0 - _clamp((u - 0.12) / 0.76, 0.0, 1.0)
        return norm * 100.0

    # ---------- static scene builders (canonical Eka layout + GEMINI.md dimensions) ----------

    def _ensure_scene(self, w, h):
        key = (int(w), int(h), bool(self.fx_view))
        if self._scene_key == key:
            return
        self._scene_key = key
        cv = self.widget_canvas
        cv.delete("all")
        self._ui.clear()
        self._cache.clear()
        self.hit_rects.clear()
        self.fx_sliders.clear()

        pad = 6
        btn_h = max(36, min(44, int(h * 0.12)))
        strip_h = max(32, min(40, int(h * 0.10)))
        meter_w = 68

        gx0 = pad
        gy0 = pad
        gx1 = w - pad - meter_w - 6
        gy1 = h - pad - btn_h - strip_h - 12
        fy1 = gy1 + 6 + strip_h
        self.graph_box = (gx0, gy0, gx1, gy1)

        self._geom = {
            "w": w, "h": h, "pad": pad,
            "gx0": gx0, "gy0": gy0, "gx1": gx1, "gy1": gy1, "fy1": fy1,
        }

        if self.fx_view:
            self._build_fx_screen(cv, gx0, gy0, gx1, fy1)
            self._build_meters(cv, gx1, gy0, fy1, w, pad)
            self._build_buttons(cv, pad, fy1 + 6, w - pad, h - pad,
                                ("fx", "fx_link", "snap", "zero", "routing", "inv_byp"))
        else:
            self._build_graph(cv, gx0, gy0, gx1, gy1)
            self._build_morph_strip(cv, gx0, gy1 + 6, gx1, fy1)
            self._build_meters(cv, gx1, gy0, fy1, w, pad)
            self._build_buttons(cv, pad, fy1 + 6, w - pad, h - pad,
                                ("fx", "model_a", "model_b", "routing", "shape", "link"))

    def _build_graph(self, cv, gx0, gy0, gx1, gy1):
        cv.create_rectangle(gx0, gy0, gx1, gy1, fill=PANEL_BG, outline=LINE_DIM, width=1)

        # Log frequency grid lines with physical Hz / kHz units
        for gf, glbl in ((50.0, "50"), (100.0, "100"), (200.0, "200"), (500.0, "500"),
                         (1000.0, "1k"), (2000.0, "2k"), (5000.0, "5k"), (10000.0, "10k")):
            xg = self._freq_to_x(gf, gx0, gx1)
            cv.create_line(xg, gy0 + 20, xg, gy1 - 16, fill=GRID_COL, width=1)
            cv.create_text(xg, gy1 - 3, text=glbl, anchor="s", fill=LABEL_DIM, font=FONT_7B)

        # Horizontal dB grid lines
        for gdb in (-12.0, 0.0, 12.0):
            yg = self._db_to_y(gdb, gy0, gy1)
            col = ZERO_COL if gdb == 0.0 else GRID_COL
            cv.create_line(gx0 + 1, yg, gx1 - 1, yg, fill=col, width=1.4 if gdb == 0.0 else 1.0)
            cv.create_text(gx0 + 6, yg - 2, text=f"{gdb:+.0f}dB" if gdb else "0dB",
                           anchor="sw", fill=LABEL_DIM, font=FONT_6B)

        ui = self._ui
        ymid = (gy0 + gy1) * 0.5
        flat_poly = (gx0, gy1, gx0, ymid, gx1, ymid, gx1, gy1)
        flat_line = (gx0, ymid, gx1, ymid)

        ui["poly_sum"]  = cv.create_polygon(*flat_poly, fill=FILL_SUM, outline="")
        ui["curve_a"]   = cv.create_line(*flat_line, fill=COL_A, width=2)
        ui["curve_b"]   = cv.create_line(*flat_line, fill=COL_B, width=2)
        ui["curve_sum"] = cv.create_line(*flat_line, fill=COL_SUM, width=2.4)

        # Modulation spans & live modulated cutoff lines/dots
        ui["span_a"]     = cv.create_line(gx0, ymid, gx0, ymid, fill=COL_A, width=3)
        ui["span_b"]     = cv.create_line(gx0, ymid, gx0, ymid, fill=COL_B, width=3)
        ui["eff_line_a"] = cv.create_line(gx0, gy0 + 20, gx0, gy1 - 16, fill=COL_A, width=1, dash=(3, 3))
        ui["eff_line_b"] = cv.create_line(gx0, gy0 + 20, gx0, gy1 - 16, fill=COL_B, width=1, dash=(3, 3))
        ui["eff_dot_a"]  = cv.create_oval(0, 0, 0, 0, fill=COL_A, outline=PANEL_BG, width=1)
        ui["eff_dot_b"]  = cv.create_oval(0, 0, 0, 0, fill=COL_B, outline=PANEL_BG, width=1)

        # Interactive 2D nodes for Filter A and Filter B (large touch target, selection halo)
        ui["node_a_halo"] = cv.create_oval(0, 0, 0, 0, fill="", outline=COL_A, width=2)
        ui["node_a"]      = cv.create_oval(0, 0, 0, 0, fill=COL_A, outline=LABEL_HI, width=2)
        ui["node_a_lbl"]  = cv.create_text(0, 0, text="A", fill="#101318", font=FONT_9B)

        ui["node_b_halo"] = cv.create_oval(0, 0, 0, 0, fill="", outline=COL_B, width=2)
        ui["node_b"]      = cv.create_oval(0, 0, 0, 0, fill=COL_B, outline=LABEL_HI, width=2)
        ui["node_b_lbl"]  = cv.create_text(0, 0, text="B", fill="#101318", font=FONT_9B)

        # Top-left & top-right header readouts inside graph (like Eka Echo / Modls / Q-4)
        ui["hdr_l"]    = cv.create_text(gx0 + 10, gy0 + 5, anchor="nw", text="", fill=LABEL_HI, font=FONT_8B)
        ui["hdr_r"]    = cv.create_text(gx1 - 10, gy0 + 5, anchor="ne", text="", fill=COL_A, font=FONT_8B)
        ui["foot_mod"] = cv.create_text(gx0 + 10, gy1 - 16, anchor="sw", text="", fill=LABEL_DIM, font=FONT_7B)

    def _build_morph_strip(self, cv, x0, y0, x1, y1):
        """Middle interactive strip: Morph A bar (left) & Morph B bar (right), 6 px gap."""
        gap = 6
        mid = (x0 + x1) * 0.5
        ma_x0, ma_x1 = x0, mid - gap * 0.5
        mb_x0, mb_x1 = mid + gap * 0.5, x1
        self.morph_a_box = (ma_x0, y0, ma_x1, y1)
        self.morph_b_box = (mb_x0, y0, mb_x1, y1)

        ui = self._ui
        for key, box, fill_c, line_c in (
            ("morph_a", self.morph_a_box, FILL_A, COL_A),
            ("morph_b", self.morph_b_box, FILL_B, COL_B),
        ):
            bx0, by0, bx1, by1 = box
            cv.create_rectangle(bx0, by0, bx1, by1, fill=PANEL_BG, outline=LINE_DIM, width=1)
            ui[f"fill_{key}"] = cv.create_rectangle(bx0 + 2, by0 + 2, bx0 + 2, by1 - 2, fill=fill_c, outline="")
            ui[f"base_{key}"] = cv.create_line(bx0 + 2, by0 + 2, bx0 + 2, by1 - 2, fill=LABEL_HI, width=2)
            ui[f"eff_{key}"]  = cv.create_line(bx0 + 2, by0 + 2, bx0 + 2, by1 - 2, fill=line_c, width=2.4)
            ui[f"txt_{key}"]  = cv.create_text((bx0 + bx1) * 0.5, (by0 + by1) * 0.5, text="", fill=LABEL_HI, font=FONT_8B)

    def _build_meters(self, cv, gx1, y0, y1, w, pad):
        mx0 = gx1 + 6
        mx1 = w - pad
        cv.create_rectangle(mx0, y0, mx1, y1, fill=PANEL_BG, outline=LINE_DIM, width=1)

        top = y0 + 18
        bot = y1 - 16
        mh = max(20.0, bot - top)
        bw = (mx1 - mx0 - 12) / 3.0

        ui = self._ui
        ui["m_title"] = cv.create_text((mx0 + mx1) * 0.5, y0 + 9, text="LEVELS", fill=LABEL_DIM, font=FONT_6B)
        labels = ("IN", "ENV", "OUT")
        colors = (COL_B, COL_ENV, COL_A)
        m_items = []
        for i in range(3):
            bx0 = mx0 + 4 + i * (bw + 2.0)
            bx1 = bx0 + bw - 1.5
            cv.create_rectangle(bx0, top, bx1, bot, fill=TRACK_BG, outline=GRID_COL, width=1)
            bar_id = cv.create_rectangle(bx0 + 1, bot, bx1 - 1, bot, fill=colors[i], outline="")
            hold_id = cv.create_line(bx0 + 1, bot, bx1 - 1, bot, fill=LABEL_HI, width=2)
            lbl_id = cv.create_text((bx0 + bx1) * 0.5, y1 - 8, text=labels[i], fill=LABEL_DIM, font=FONT_6B)
            m_items.append((bx0, bx1, bar_id, hold_id, lbl_id, colors[i]))

        self._geom["m_top"] = top
        self._geom["m_bot"] = bot
        self._geom["m_h"] = mh
        ui["meters"] = tuple(m_items)

    def _build_buttons(self, cv, x0, y0, x1, y1, keys):
        """6 large two-line touch buttons at the bottom (height 36..44 px, gap 6 px)."""
        gap = 6
        bw = (x1 - x0 - gap * (len(keys) - 1)) / float(len(keys))
        cy = (y0 + y1) * 0.5
        slots = []
        for i, key in enumerate(keys):
            bx0 = x0 + i * (bw + gap)
            bx1 = bx0 + bw
            self.hit_rects[key] = (bx0, y0, bx1, y1)
            cx = (bx0 + bx1) * 0.5
            r_id = cv.create_rectangle(bx0, y0, bx1, y1, fill=BUTTON_BG, outline=LINE_DIM, width=1.5)
            t_id = cv.create_text(cx, cy - 7, text="", fill=LABEL_HI, font=FONT_7B)
            s_id = cv.create_text(cx, cy + 7, text="", fill=COL_A, font=FONT_8B)
            slots.append((r_id, t_id, s_id))
        self._ui["btns"] = tuple(slots)

    def _build_fx_screen(self, cv, x0, y0, x1, y1):
        """Play / Live Effects screen matching Eka Echo & Modls."""
        cv.create_rectangle(x0, y0, x1, y1, fill=PANEL_BG, outline=LINE_DIM, width=1)
        ui = self._ui
        ui["fx_hdr_l"] = cv.create_text(x0 + 10, y0 + 5, text="", anchor="nw", fill=LABEL_HI, font=FONT_8B)
        ui["fx_hdr_r"] = cv.create_text(x1 - 10, y0 + 5, text="", anchor="ne", fill=COL_A, font=FONT_8B)

        inner_x0 = x0 + 10
        inner_x1 = x1 - 10
        sl_h = max(34, min(40, int((y1 - y0) * 0.17)))
        pad_y0 = y0 + 24
        pad_y1 = y1 - sl_h - 14
        self.xy_box = (inner_x0, pad_y0, inner_x1, pad_y1)

        cv.create_rectangle(inner_x0, pad_y0, inner_x1, pad_y1, fill=TRACK_BG, outline=LINE_DIM, width=1.2)
        cx = (inner_x0 + inner_x1) * 0.5
        cy = (pad_y0 + pad_y1) * 0.5
        pw = max(1.0, inner_x1 - inner_x0)

        for frac, lbl in ((-0.5, "-1.8 oct"), (0.0, "0 (BASE)"), (0.5, "+1.8 oct")):
            gx = cx + frac * pw * 0.5
            cv.create_line(gx, pad_y0 + 1, gx, pad_y1 - 1,
                           fill=ZERO_COL if frac == 0.0 else GRID_COL,
                           width=1.5 if frac == 0.0 else 1.0)
            cv.create_text(gx, pad_y1 - 3, text=lbl, anchor="s", fill=LABEL_DIM, font=FONT_7B)
        cv.create_line(inner_x0 + 1, cy, inner_x1 - 1, cy, fill=ZERO_COL, width=1.5)

        cv.create_text(cx, pad_y0 + 4, text="▲ MORPH + (HP / NOTCH / BRIGHT)", anchor="n", fill=COL_B, font=FONT_7B)
        cv.create_text(cx, pad_y1 - 15, text="▼ MORPH - (LP / VOWEL A / SUB)", anchor="s", fill=COL_A, font=FONT_7B)
        cv.create_text(inner_x0 + 6, cy - 4, text="◄ -3.5 oct", anchor="sw", fill=LABEL_DIM, font=FONT_7B)
        cv.create_text(inner_x1 - 6, cy - 4, text="+3.5 oct ►", anchor="se", fill=LABEL_DIM, font=FONT_7B)

        ui["xy_cross_v"] = cv.create_line(cx, pad_y0 + 2, cx, pad_y1 - 2, fill=GRID_COL, width=1)
        ui["xy_cross_h"] = cv.create_line(inner_x0 + 2, cy, inner_x1 - 2, cy, fill=GRID_COL, width=1)
        ui["xy_vec"]     = cv.create_line(cx, cy, cx, cy, fill=COL_FX, width=2.2)
        ui["xy_eff_a"]   = cv.create_oval(cx - 5, cy - 5, cx + 5, cy + 5, fill=COL_A, outline=PANEL_BG, width=1)
        ui["xy_eff_b"]   = cv.create_oval(cx - 5, cy - 5, cx + 5, cy + 5, fill=COL_B, outline=PANEL_BG, width=1)
        ui["xy_puck"]    = cv.create_oval(cx - 12, cy - 12, cx + 12, cy + 12, fill=LABEL_HI, outline=LABEL_HI, width=2)
        ui["xy_puck_dot"]= cv.create_oval(cx - 3, cy - 3, cx + 3, cy + 3, fill="#101318", outline="")
        ui["xy_readout"] = cv.create_text(inner_x0 + 6, pad_y0 + 4, text="", anchor="nw", fill=LABEL_HI, font=FONT_8B)
        ui["xy_spring_tag"] = cv.create_text(inner_x1 - 6, pad_y0 + 4, text="", anchor="ne", fill=COL_ENV, font=FONT_7B)

        # Bottom row inside Play panel: SCREAM FX slider + "0" button (left) & SMOOTH slider (right)
        wy0 = pad_y1 + 6
        wy1 = y1 - 8
        z_btn_w = 38
        smooth_w = max(96, min(180, int(pw * 0.32)))
        sm_x0 = inner_x1 - smooth_w
        zx1 = sm_x0 - 8
        zx0 = zx1 - z_btn_w
        bar_x1 = zx0 - 6

        self.fx_sliders["scream_fx"] = (inner_x0, wy0, bar_x1, wy1, False)
        cv.create_rectangle(inner_x0, wy0, bar_x1, wy1, fill=BUTTON_BG, outline=LINE_DIM, width=1.2)
        ui["fill_scream_fx"] = cv.create_rectangle(inner_x0 + 1, wy0 + 2, inner_x0 + 1, wy1 - 2, fill=COL_FX, outline="")
        ui["line_scream_fx"] = cv.create_line(inner_x0, wy0 + 2, inner_x0, wy1 - 2, fill=LABEL_HI, width=2.4)
        ui["txt_scream_fx"]  = cv.create_text((inner_x0 + bar_x1) * 0.5, (wy0 + wy1) * 0.5, text="SCREAM FX 0%", fill=LABEL_HI, font=FONT_9B)

        self.hit_rects["z_scream_fx"] = (zx0, wy0, zx1, wy1)
        ui["btn_z_scream"] = cv.create_rectangle(zx0, wy0, zx1, wy1, fill=BUTTON_BG, outline=COL_FX, width=1.4)
        ui["txt_z_scream"] = cv.create_text((zx0 + zx1) * 0.5, (wy0 + wy1) * 0.5, text="0", fill=LABEL_HI, font=FONT_10B)

        self.fx_sliders["fx_smooth"] = (sm_x0, wy0, inner_x1, wy1, False)
        cv.create_rectangle(sm_x0, wy0, inner_x1, wy1, fill=BUTTON_BG, outline=LINE_DIM, width=1.2)
        ui["fill_fx_smooth"] = cv.create_rectangle(sm_x0 + 1, wy0 + 2, sm_x0 + 1, wy1 - 2, fill=FILL_B, outline="")
        ui["line_fx_smooth"] = cv.create_line(sm_x0, wy0 + 2, sm_x0, wy1 - 2, fill=COL_B, width=2.4)
        ui["txt_fx_smooth"]  = cv.create_text((sm_x0 + inner_x1) * 0.5, (wy0 + wy1) * 0.5, text="SMOOTH 50%", fill=LABEL_HI, font=FONT_8B)

    # ---------- incremental refresh ----------

    def _update_buttons(self, specs, byp):
        prev = self._cache.get("btns")
        if prev is None or len(prev) != len(specs):
            prev = [None] * len(specs)
            self._cache["btns"] = prev
        slots = self._ui["btns"]
        cv = self.widget_canvas
        for i, (t, s, act, col) in enumerate(specs):
            skey = (t, s, act, col, byp)
            if prev[i] == skey:
                continue
            prev[i] = skey
            r_id, t_id, s_id = slots[i]
            fill = (BYP_FILL if byp else col) if act else BUTTON_BG
            out = BYP_MID if byp else (col if act else LINE_DIM)
            fg_t = (BYP_HI if byp else "#101318") if act else (BYP_HI if byp else LABEL_HI)
            fg_s = (BYP_MID if byp else "#16191e") if act else (BYP_MID if byp else col)
            cv.itemconfig(r_id, fill=fill, outline=out)
            cv.itemconfig(t_id, text=t, fill=fg_t)
            cv.itemconfig(s_id, text=s, fill=fg_s)

    def _update_meters(self, byp, in_db, env_lv, out_db):
        self.hold_in  = in_db  if in_db  >= self.hold_in  else max(in_db,  self.hold_in  - 1.2)
        self.hold_out = out_db if out_db >= self.hold_out else max(out_db, self.hold_out - 1.2)
        self.hold_env = env_lv if env_lv >= self.hold_env else max(env_lv, self.hold_env - 0.04)

        bot = self._geom["m_bot"]
        mh = self._geom["m_h"]
        cv = self.widget_canvas
        meters = self._ui["meters"]

        vals = (
            (_clamp((in_db - FLOOR_DB) / (6.0 - FLOOR_DB), 0.0, 1.0),
             _clamp((self.hold_in - FLOOR_DB) / (6.0 - FLOOR_DB), 0.0, 1.0)),
            (_clamp(env_lv, 0.0, 1.0), _clamp(self.hold_env, 0.0, 1.0)),
            (_clamp((out_db - FLOOR_DB) / (6.0 - FLOOR_DB), 0.0, 1.0),
             _clamp((self.hold_out - FLOOR_DB) / (6.0 - FLOOR_DB), 0.0, 1.0)),
        )
        for i, ((val, hold), (bx0, bx1, bar_id, hold_id, lbl_id, col)) in enumerate(zip(vals, meters)):
            fy = bot - val * mh
            hy = bot - hold * mh
            bcol = BYP_MID if byp else (HOT_RED if (i != 1 and val > 0.92) else col)
            cv.coords(bar_id, bx0 + 1, fy, bx1 - 1, bot - 1)
            cv.itemconfig(bar_id, fill=bcol)
            cv.coords(hold_id, bx0 + 1, hy, bx1 - 1, hy)
            cv.itemconfig(hold_id, fill=BYP_HI if byp else LABEL_HI)
            cv.itemconfig(lbl_id, fill=BYP_MID if byp else LABEL_DIM)

    def refresh_gui(self, sample_telemetry=True):
        self._follow_page()
        w = max(240, int(self.width or self.widget_canvas.winfo_width() or 640))
        h = max(180, int(self.height or self.widget_canvas.winfo_height() or 380))
        self._ensure_scene(w, h)

        if self._get("fx_zero", 0.0) >= 0.5:
            self._cancel_spring()
            self._set("sweep_fx", 0.0)
            self._set("morph_fx", 0.0)
            self._set("scream_fx", 0.0)
            self._set("fx_zero", 0)

        model_a = int(round(_clamp(self._get("model_a", 0), 0, 5)))
        cut_a   = _clamp(self._get("cutoff_a", 650.0), F_MIN, F_MAX)
        res_a   = _clamp(self._get("res_a", 35.0), 0.0, 100.0)
        morph_a = _clamp(self._get("morph_a", 0.0), 0.0, 100.0)

        model_b = int(round(_clamp(self._get("model_b", 1), 0, 5)))
        cut_b   = _clamp(self._get("cutoff_b", 1800.0), F_MIN, F_MAX)
        res_b   = _clamp(self._get("res_b", 40.0), 0.0, 100.0)
        morph_b = _clamp(self._get("morph_b", 25.0), 0.0, 100.0)

        routing = int(round(_clamp(self._get("routing", 0), 0, 2)))
        link_m  = int(round(_clamp(self._get("link_mode", 0), 0, 2)))
        ab_bal  = _clamp(self._get("ab_balance", 0.0), -100.0, 100.0)
        inv_b   = int(round(_clamp(self._get("invert_b", 0), 0, 1)))

        shape   = int(round(_clamp(self._get("lfo_shape", 0), 0, 6)))
        rate    = _clamp(self._get("lfo_rate", 0.80), RATE_MIN, RATE_MAX)
        sync_on = int(round(_clamp(self._get("lfo_sync", 0), 0, 1))) == 1
        phase_b = _clamp(self._get("lfo_phase", 90.0), 0.0, 180.0)

        lfo_mod   = _clamp(self._get("lfo_mod", 25.0), -100.0, 100.0)
        morph_mod = _clamp(self._get("morph_mod", 0.0), -100.0, 100.0)
        env_mod   = _clamp(self._get("env_mod", 0.0), -100.0, 100.0)
        drive     = _clamp(self._get("drive", 15.0), 0.0, 100.0)
        mix       = _clamp(self._get("mix", 100.0), 0.0, 100.0)
        byp       = int(round(_clamp(self._get("bypass", 0), 0, 1))) == 1

        sweep_fx  = _clamp(self._get("sweep_fx", 0.0), -100.0, 100.0)
        fx_link   = int(round(_clamp(self._get("fx_link", 1), 0, 1))) == 1
        morph_fx  = _clamp(self._get("morph_fx", 0.0), -100.0, 100.0)
        scream_fx = _clamp(self._get("scream_fx", 0.0), 0.0, 100.0)
        fx_smooth = _clamp(self._get("fx_smooth", 50.0), 0.0, 100.0)

        if sample_telemetry:
            in_pk  = self._get_mon("in_peak", FLOOR_DB)
            out_pk = self._get_mon("out_peak", FLOOR_DB)
            eff_ca = _clamp(self._get_mon("eff_cut_a", cut_a), F_MIN, F_MAX)
            eff_cb = _clamp(self._get_mon("eff_cut_b", cut_b), F_MIN, F_MAX)
            eff_ma = _clamp(self._get_mon("eff_morph_a", morph_a), 0.0, 100.0)
            eff_mb = _clamp(self._get_mon("eff_morph_b", morph_b), 0.0, 100.0)
            lfo_a  = _clamp(self._get_mon("lfo_a", 0.0), -1.0, 1.0)
            lfo_b  = _clamp(self._get_mon("lfo_b", 0.0), -1.0, 1.0)
            env_lv = _clamp(self._get_mon("env_lvl", 0.0), 0.0, 1.0)
            bpm    = _clamp(self._get_mon("host_bpm", 120.0), 20.0, 400.0)
            self._telemetry = (in_pk, out_pk, eff_ca, eff_cb, eff_ma, eff_mb, lfo_a, lfo_b, env_lv, bpm)
        else:
            in_pk, out_pk, _, _, _, _, lfo_a, lfo_b, env_lv, bpm = self._telemetry
            oct_a = math.log2(cut_a)
            oct_b = math.log2(cut_b)
            if link_m == 1:
                oct_b = oct_a + (oct_b - 9.965784)
            elif link_m == 2:
                oct_b = 2.0 * 9.965784 - oct_a + (oct_b - 9.965784) * 0.5
            sw_a = (sweep_fx * 0.01) * 3.5
            sw_b = sw_a * (1.0 if fx_link else 0.0) * (-1.0 if link_m == 2 else 1.0)
            env_oct_a = (env_mod * 0.01) * env_lv * 4.5
            env_oct_b = -env_oct_a if link_m == 2 else env_oct_a
            eff_ca = _clamp(2.0 ** (oct_a + sw_a + (lfo_mod * 0.01) * lfo_a * 3.8 + env_oct_a), F_MIN, F_MAX)
            eff_cb = _clamp(2.0 ** (oct_b + sw_b + (lfo_mod * 0.01) * lfo_b * 3.8 + env_oct_b), F_MIN, F_MAX)
            mfx_a = morph_fx * 0.75
            mfx_b = mfx_a * (1.0 if fx_link else 0.25)
            eff_ma = _clamp(morph_a + mfx_a + morph_mod * (0.65 * lfo_a + 0.55 * (env_lv - 0.3)), 0.0, 100.0)
            eff_mb = _clamp(morph_b + mfx_b + morph_mod * (0.65 * lfo_b + 0.55 * (env_lv - 0.3)), 0.0, 100.0)

        div_lbl, _ = nearest_rate_division(rate, bpm)
        self._sync_encoder_titles(sync_on, div_lbl)
        self._update_meters(byp, in_pk, env_lv, out_pk)

        fx_nonzero = (abs(sweep_fx) >= 0.75 or abs(morph_fx) >= 0.75 or scream_fx >= 0.75)

        if self.fx_view:
            self._refresh_fx_view(
                model_a, model_b, cut_a, cut_b, eff_ca, eff_cb, eff_ma, eff_mb,
                routing, link_m, inv_b, sweep_fx, fx_link, morph_fx, scream_fx, fx_smooth,
                fx_nonzero, bpm, byp
            )
            return

        # 1. Frequency Response Graph
        gx0, gy0, gx1, gy1 = self.graph_box
        scr_norm = scream_fx * 0.01
        eff_res_a = _clamp(res_a + scr_norm * (93.0 - res_a) * 0.85, 0.0, 100.0)
        eff_res_b = _clamp(res_b + scr_norm * (93.0 - res_b) * (0.85 if fx_link else 0.35), 0.0, 100.0)

        db_a = _eval_model_db(model_a, _CURVE_FREQS, eff_ca, eff_res_a, eff_ma)
        db_b = _eval_model_db(model_b, _CURVE_FREQS, eff_cb, eff_res_b, eff_mb)

        bal = ab_bal * 0.01
        ga = _clamp(1.0 - max(0.0, bal), 0.0, 1.0)
        gb = _clamp(1.0 + min(0.0, bal), 0.0, 1.0)

        pts_a = []
        pts_b = []
        pts_sum = []
        gw = gx1 - gx0
        for idx, u in enumerate(_CURVE_US):
            x = gx0 + u * gw
            da = db_a[idx]
            db = db_b[idx]
            pts_a.extend((x, self._db_to_y(da, gy0, gy1)))
            pts_b.extend((x, self._db_to_y(db, gy0, gy1)))

            if routing == 0:
                d_sum = da * ga + db * gb
            elif routing == 1:
                la = (10.0 ** (da / 20.0)) * ga
                lb = (10.0 ** (db / 20.0)) * gb
                comb = abs(la - lb) if inv_b else (la + lb) * 0.65
                d_sum = 20.0 * math.log10(max(comb, 0.025))
            else:
                d_sum = max(da * ga, db * gb)
            d_sum = _clamp(d_sum * (mix * 0.01), -DB_SPAN, DB_SPAN)
            pts_sum.extend((x, self._db_to_y(d_sum, gy0, gy1)))

        col_a = BYP_MID if byp else COL_A
        col_b = BYP_MID if byp else COL_B
        col_sum = BYP_HI if byp else COL_SUM

        self._coords("curve_a", *pts_a)
        self._coords("curve_b", *pts_b)
        self._coords("curve_sum", *pts_sum)
        self._coords("poly_sum", gx0, gy1, *pts_sum, gx1, gy1)

        self._cfg("curve_a", fill=col_a, width=2.2 if self.selected_chain == "a" else 1.5)
        self._cfg("curve_b", fill=col_b, width=2.2 if self.selected_chain == "b" else 1.5)
        self._cfg("curve_sum", fill=col_sum)
        self._cfg("poly_sum", fill=BYP_FILL if byp else FILL_SUM)

        # Base handles & live modulated indicators
        xa_base = self._freq_to_x(cut_a, gx0, gx1)
        ya_base = self._res_to_y(res_a, gy0, gy1)
        xb_base = self._freq_to_x(cut_b, gx0, gx1)
        yb_base = self._res_to_y(res_b, gy0, gy1)
        self.node_pos["a"] = (xa_base, ya_base)
        self.node_pos["b"] = (xb_base, yb_base)

        xa_eff = self._freq_to_x(eff_ca, gx0, gx1)
        xb_eff = self._freq_to_x(eff_cb, gx0, gx1)

        self._coords("span_a", xa_base, ya_base, xa_eff, ya_base)
        self._coords("span_b", xb_base, yb_base, xb_eff, yb_base)
        self._coords("eff_line_a", xa_eff, gy0 + 20, xa_eff, gy1 - 16)
        self._coords("eff_line_b", xb_eff, gy0 + 20, xb_eff, gy1 - 16)
        self._coords("eff_dot_a", xa_eff - 4, ya_base - 4, xa_eff + 4, ya_base + 4)
        self._coords("eff_dot_b", xb_eff - 4, yb_base - 4, xb_eff + 4, yb_base + 4)

        ra = 13 if self.selected_chain == "a" else 10
        rb = 13 if self.selected_chain == "b" else 10
        self._coords("node_a_halo", xa_base - ra - 4, ya_base - ra - 4, xa_base + ra + 4, ya_base + ra + 4)
        self._coords("node_a", xa_base - ra, ya_base - ra, xa_base + ra, ya_base + ra)
        self._coords("node_a_lbl", xa_base, ya_base)
        self._cfg("node_a", fill=col_a, outline=LABEL_HI if self.selected_chain == "a" else col_a)

        self._coords("node_b_halo", xb_base - rb - 4, yb_base - rb - 4, xb_base + rb + 4, yb_base + rb + 4)
        self._coords("node_b", xb_base - rb, yb_base - rb, xb_base + rb, yb_base + rb)
        self._coords("node_b_lbl", xb_base, yb_base)
        self._cfg("node_b", fill=col_b, outline=LABEL_HI if self.selected_chain == "b" else col_b)

        # Header & footer readouts inside graph
        m_a_name, m_a_col = MODELS[model_a]
        m_b_name, m_b_col = MODELS[model_b]
        r_name, r_col = ROUTINGS[routing]
        fx_tag = " · [PLAY FX]" if fx_nonzero else ""
        inv_tag = " · ØB INV" if inv_b else ""
        hdr_l = (
            f"A {m_a_name} {_fmt_freq(eff_ca)} Q{eff_res_a:.0f}%  ·  "
            f"B {m_b_name} {_fmt_freq(eff_cb)} Q{eff_res_b:.0f}%{inv_tag}{fx_tag}"
        )
        self._cfg("hdr_l", text=hdr_l, fill=BYP_HI if byp else LABEL_HI)

        if byp:
            badge_txt, badge_col = "BYPASS", BYP_HI
        else:
            badge_txt, badge_col = f"{r_name} · DRV {drive:.0f}% · MIX {mix:.0f}%", r_col
        self._cfg("hdr_r", text=badge_txt, fill=badge_col)

        sh_name, _ = LFO_SHAPES[shape]
        rate_str = f"{div_lbl} ({bpm:.0f})" if sync_on else _fmt_rate(rate)
        foot_txt = (
            f"LFO {sh_name} {rate_str} Δφ{phase_b:.0f}° → CUT {lfo_mod:+.0f}%  ·  "
            f"MOD→MORPH {morph_mod:+.0f}%  ·  ENV→CUT {env_mod:+.0f}%"
        )
        self._cfg("foot_mod", text=foot_txt, fill=BYP_MID if byp else LABEL_DIM)

        # 2. Middle Morph A & Morph B strip
        for key, box, model, val, eff_v, prefix, fcol in (
            ("morph_a", self.morph_a_box, model_a, morph_a, eff_ma, "MORPH A", FILL_A),
            ("morph_b", self.morph_b_box, model_b, morph_b, eff_mb, "MORPH B", FILL_B),
        ):
            bx0, by0, bx1, by1 = box
            span = max(1.0, bx1 - bx0 - 4)
            xb = bx0 + 2 + span * (val * 0.01)
            xe = bx0 + 2 + span * (eff_v * 0.01)
            self._coords(f"fill_{key}", bx0 + 2, by0 + 2, xe, by1 - 2)
            self._cfg(f"fill_{key}", fill=BYP_FILL if byp else fcol)
            self._coords(f"base_{key}", xb, by0 + 2, xb, by1 - 2)
            self._coords(f"eff_{key}", xe, by0 + 2, xe, by1 - 2)
            mlbl = _morph_mode_label(model, eff_v)
            self._cfg(f"txt_{key}", text=f"{prefix}: {mlbl} ({val:.0f}%)", fill=BYP_HI if byp else LABEL_HI)

        # 3. Bottom 6 two-line touch buttons (GEMINI.md height 36..44 px, gap 6 px)
        lnk_name, lnk_col = LINKS[link_m]
        sh_name, sh_col = LFO_SHAPES[shape]
        lfo_sub = f"{sh_name} {div_lbl}" if sync_on else f"{sh_name} {_fmt_rate(rate)}"
        specs = (
            ("PLAY",    "FX *" if fx_nonzero else "XY PAD", fx_nonzero, COL_FX),
            ("FLT A",   m_a_name,                           self.selected_chain == "a", m_a_col),
            ("FLT B",   m_b_name,                           self.selected_chain == "b", m_b_col),
            ("ROUTE",   r_name.split()[0],                  False, r_col),
            ("LFO",     lfo_sub,                            sync_on, sh_col),
            ("LINK B*", "BYPASS" if byp else lnk_name,      byp or (link_m > 0), HOT_RED if byp else lnk_col),
        )
        self._update_buttons(specs, byp)

    def _refresh_fx_view(self, model_a, model_b, cut_a, cut_b, eff_ca, eff_cb, eff_ma, eff_mb,
                         routing, link_m, inv_b, sweep_fx, fx_link, morph_fx, scream_fx, fx_smooth,
                         fx_nonzero, bpm, byp):
        lnk_name, _ = LINKS[link_m]
        link_txt = f"LINK ({lnk_name})" if fx_link else "A ONLY"
        hdr_l = f"PLAY FX · {_fmt_freq(cut_a)}/{_fmt_freq(cut_b)} → {_fmt_freq(eff_ca)}/{_fmt_freq(eff_cb)} · {link_txt}"
        self._cfg("fx_hdr_l", text=hdr_l, fill=BYP_HI if byp else LABEL_HI)

        m_a_name, m_a_col = MODELS[model_a]
        m_b_name, _ = MODELS[model_b]
        if byp:
            badge, badge_col = "BYPASS", BYP_HI
        else:
            badge, badge_col = f"{m_a_name}+{m_b_name} · {bpm:.0f} BPM", m_a_col
        self._cfg("fx_hdr_r", text=badge, fill=badge_col)

        inner_x0, pad_y0, inner_x1, pad_y1 = self.xy_box
        cx = (inner_x0 + inner_x1) * 0.5
        cy = (pad_y0 + pad_y1) * 0.5
        pw = max(1.0, inner_x1 - inner_x0)
        ph = max(1.0, pad_y1 - pad_y0)

        px = cx + _clamp(sweep_fx / 100.0, -1.0, 1.0) * (pw * 0.5)
        py = cy - _clamp(morph_fx / 100.0, -1.0, 1.0) * (ph * 0.5)

        if byp:
            p_col = BYP_MID
        elif abs(sweep_fx) >= abs(morph_fx) and abs(sweep_fx) >= 0.75:
            p_col = COL_B if sweep_fx < 0 else COL_A
        elif abs(morph_fx) >= 0.75:
            p_col = COL_ENV if morph_fx > 0 else COL_LFO
        else:
            p_col = LABEL_HI

        self._coords("xy_cross_v", px, pad_y0 + 2, px, pad_y1 - 2)
        self._coords("xy_cross_h", inner_x0 + 2, py, inner_x1 - 2, py)
        self._coords("xy_vec", cx, cy, px, py)
        self._cfg("xy_vec", fill=p_col)

        ux_a = math.log(_clamp(eff_ca, F_MIN, F_MAX) / F_MIN) / _LOG_F_SPAN
        ux_b = math.log(_clamp(eff_cb, F_MIN, F_MAX) / F_MIN) / _LOG_F_SPAN
        xa_dot = inner_x0 + ux_a * pw
        ya_dot = pad_y1 - (eff_ma * 0.01) * ph
        xb_dot = inner_x0 + ux_b * pw
        yb_dot = pad_y1 - (eff_mb * 0.01) * ph
        self._coords("xy_eff_a", xa_dot - 5, ya_dot - 5, xa_dot + 5, ya_dot + 5)
        self._coords("xy_eff_b", xb_dot - 5, yb_dot - 5, xb_dot + 5, yb_dot + 5)

        r = 12.0
        self._coords("xy_puck", px - r, py - r, px + r, py + r)
        self._cfg("xy_puck", fill=p_col, outline=BYP_HI if byp else LABEL_HI)
        self._coords("xy_puck_dot", px - 3, py - 3, px + 3, py + 3)

        oct_shift = (sweep_fx * 0.01) * 3.5
        m_txt = f"MORPH {morph_fx:+.0f}%" if abs(morph_fx) >= 0.5 else "MORPH 0%"
        self._cfg("xy_readout", text=f"SWEEP {sweep_fx:+.0f}% ({oct_shift:+.1f} oct) · {m_txt}", fill=BYP_HI if byp else LABEL_HI)
        self._cfg("xy_spring_tag", text="SPRING (SNAP 0)" if self.fx_snap else "HOLD", fill=COL_ENV if self.fx_snap else LABEL_DIM)

        # Scream FX slider + "0" button
        sx0, wy0, bar_x1, wy1, _ = self.fx_sliders["scream_fx"]
        norm = _clamp(scream_fx / 100.0, 0.0, 1.0)
        vx = sx0 + norm * (bar_x1 - sx0)
        self._coords("fill_scream_fx", sx0 + 1, wy0 + 2, max(sx0 + 1, vx), wy1 - 2)
        self._coords("line_scream_fx", vx, wy0 + 2, vx, wy1 - 2)
        self._cfg("txt_scream_fx", text=f"SCREAM FX  {scream_fx:.0f}%")

        active_z = (scream_fx >= 0.75) and not byp
        self._cfg("btn_z_scream", fill=COL_FX if active_z else BUTTON_BG)
        self._cfg("txt_z_scream", fill="#101318" if active_z else LABEL_HI)

        # FX Smooth slider
        sm_x0, wy0, inner_x1, wy1, _ = self.fx_sliders["fx_smooth"]
        snorm = _clamp(fx_smooth / 100.0, 0.0, 1.0)
        svx = sm_x0 + snorm * (inner_x1 - sm_x0)
        self._coords("fill_fx_smooth", sm_x0 + 1, wy0 + 2, max(sm_x0 + 1, svx), wy1 - 2)
        self._coords("line_fx_smooth", svx, wy0 + 2, svx, wy1 - 2)
        self._cfg("txt_fx_smooth", text=f"SMOOTH {fx_smooth:.0f}%")

        # Bottom 6 two-line Play buttons
        r_name, r_col = ROUTINGS[routing]
        specs = (
            ("VIEW",     "MAIN",                        True,  COL_B),
            ("FX LINK",  "A+B" if fx_link else "A ONLY", fx_link, COL_FX),
            ("SPRING",   "SNAP 0" if self.fx_snap else "HOLD", self.fx_snap, COL_ENV),
            ("RESET",    "ZERO 0",                      fx_nonzero, COL_LFO),
            ("ROUTE",    r_name.split()[0],             False, r_col),
            ("PHASE B*", "BYPASS" if byp else ("INVERT" if inv_b else "NORM"), byp or bool(inv_b), HOT_RED),
        )
        self._update_buttons(specs, byp)

    # ---------- touch / drag interaction & double-tap reset ----------

    @staticmethod
    def _in_rect(x, y, r):
        return r[0] <= x <= r[2] and r[1] <= y <= r[3]

    def _check_double_tap(self, target_key, now):
        prev_k, prev_t = self.last_tap
        self.last_tap = (target_key, now)
        if prev_k == target_key and (now - prev_t) <= DOUBLE_TAP_SEC:
            self.last_tap = (None, 0.0)
            return True
        return False

    def _apply_xy_drag(self, x, y):
        px0, py0, px1, py1 = self.xy_box
        tx = _clamp((x - px0) / max(1.0, px1 - px0), 0.0, 1.0)
        ty = _clamp((y - py0) / max(1.0, py1 - py0), 0.0, 1.0)
        sfx = (tx * 2.0 - 1.0) * 100.0
        mfx = (1.0 - ty * 2.0) * 100.0
        if abs(sfx) < 4.0:
            sfx = 0.0
        if abs(mfx) < 4.0:
            mfx = 0.0
        self._set("sweep_fx", round(_clamp(sfx, -100.0, 100.0), 1))
        self._set("morph_fx", round(_clamp(mfx, -100.0, 100.0), 1))

    def _apply_fx_slider_drag(self, sym, x):
        info = self.fx_sliders.get(sym)
        if not info:
            return
        sx0, _, sx1, _, bipolar = info
        t = _clamp((x - sx0) / max(1.0, sx1 - sx0), 0.0, 1.0)
        if bipolar:
            v = (t * 2.0 - 1.0) * 100.0
            if abs(v) < 3.0:
                v = 0.0
            self._set(sym, round(_clamp(v, -100.0, 100.0), 1))
        else:
            self._set(sym, round(_clamp(t * 100.0, 0.0, 100.0), 1))

    def _apply_drag(self, x, y):
        if not self.drag_mode:
            return
        kind = self.drag_mode[0]
        if kind == "node":
            which = self.drag_mode[1]
            gx0, gy0, gx1, gy1 = self.graph_box
            freq = round(self._x_to_freq(x, gx0, gx1), 1)
            res = round(self._y_to_res(y, gy0, gy1), 1)
            if which == "a":
                self._set("cutoff_a", freq)
                self._set("res_a", res)
            else:
                self._set("cutoff_b", freq)
                self._set("res_b", res)
        elif kind == "morph_a":
            x0, _, x1, _ = self.morph_a_box
            u = _clamp((x - x0) / max(1.0, x1 - x0), 0.0, 1.0)
            self._set("morph_a", round(u * 100.0, 1))
        elif kind == "morph_b":
            x0, _, x1, _ = self.morph_b_box
            u = _clamp((x - x0) / max(1.0, x1 - x0), 0.0, 1.0)
            self._set("morph_b", round(u * 100.0, 1))
        elif kind == "xy":
            self._apply_xy_drag(x, y)
        elif kind == "fx_slider":
            self._apply_fx_slider_drag(self.drag_mode[1], x)

    def _on_press(self, event):
        x, y = float(event.x), float(event.y)
        now = time.monotonic()
        self.drag_mode = None
        self._pending_drag = None

        # 1. Bottom buttons + FX reset buttons
        for key, rect in self.hit_rects.items():
            if self._in_rect(x, y, rect):
                if key == "fx":
                    self.fx_view = not self.fx_view
                    self._scene_key = None
                    if self.fx_view:
                        self._select_group("g_fx", "Effects")
                    else:
                        self._select_group("g_flt_a" if self.selected_chain == "a" else "g_flt_b")
                    self.refresh_gui(sample_telemetry=False)
                    return
                if key == "fx_link":
                    cur = self._get("fx_link", 1.0)
                    self._set("fx_link", 0 if cur >= 0.5 else 1)
                    self._select_group("g_fx", "Effects")
                    self.refresh_gui(sample_telemetry=False)
                    return
                if key == "snap":
                    self.fx_snap = not self.fx_snap
                    self._select_group("g_fx", "Effects")
                    self.refresh_gui(sample_telemetry=False)
                    return
                if key == "zero":
                    if any(s in self._spring for s in ("sweep_fx", "morph_fx", "scream_fx")):
                        self._cancel_spring(("sweep_fx", "morph_fx", "scream_fx"))
                        self._set("sweep_fx", 0.0)
                        self._set("morph_fx", 0.0)
                        self._set("scream_fx", 0.0)
                    else:
                        self._start_spring(("sweep_fx", "morph_fx", "scream_fx"))
                    self._select_group("g_fx", "Effects")
                    self.refresh_gui(sample_telemetry=False)
                    return
                if key.startswith("z_"):
                    sym = key[2:]
                    if sym in self._spring:
                        self._cancel_spring((sym,))
                        self._set(sym, 0.0)
                    else:
                        self._start_spring((sym,))
                    self._select_group("g_fx", "Effects")
                    self.refresh_gui(sample_telemetry=False)
                    return
                if key == "model_a":
                    if self.selected_chain == "a":
                        cur = int(round(self._get("model_a", 0)))
                        self._set("model_a", (cur + 1) % len(MODELS))
                    self.selected_chain = "a"
                    self._select_group("g_flt_a", "Filter A")
                    self.refresh_gui(sample_telemetry=False)
                    return
                if key == "model_b":
                    if self.selected_chain == "b":
                        cur = int(round(self._get("model_b", 1)))
                        self._set("model_b", (cur + 1) % len(MODELS))
                    self.selected_chain = "b"
                    self._select_group("g_flt_b", "Filter B")
                    self.refresh_gui(sample_telemetry=False)
                    return
                if key == "routing":
                    cur = int(round(self._get("routing", 0)))
                    self._set("routing", (cur + 1) % len(ROUTINGS))
                    self._select_group("g_route", "Routing")
                    self.refresh_gui(sample_telemetry=False)
                    return
                if key == "shape":
                    cur = int(round(self._get("lfo_shape", 0)))
                    self._set("lfo_shape", (cur + 1) % len(LFO_SHAPES))
                    self._select_group("g_lfo", "LFO")
                    self.refresh_gui(sample_telemetry=False)
                    return
                if key == "link":
                    self.drag_mode = ("link_byp", now)
                    return
                if key == "inv_byp":
                    self.drag_mode = ("inv_byp", now)
                    return

        # 2. Play / Live FX screen interactions
        if self.fx_view:
            if self._in_rect(x, y, self.xy_box):
                self._cancel_spring(("sweep_fx", "morph_fx"))
                if self._check_double_tap("xy_pad", now):
                    self._set("sweep_fx", 0.0)
                    self._set("morph_fx", 0.0)
                    self._select_group("g_fx", "Effects")
                    self.refresh_gui(sample_telemetry=False)
                    return
                self.drag_mode = ("xy",)
                self._select_group("g_fx", "Effects")
                self._apply_xy_drag(x, y)
                self._last_drag_ts = now
                self.refresh_gui(sample_telemetry=False)
                return

            for sym, rect in self.fx_sliders.items():
                if self._in_rect(x, y, rect[:4]):
                    self._cancel_spring((sym,))
                    if self._check_double_tap(f"fxs_{sym}", now):
                        self._set(sym, DEFAULTS.get(sym, 0.0))
                        self._select_group("g_fx", "Effects")
                        self.refresh_gui(sample_telemetry=False)
                        return
                    self.drag_mode = ("fx_slider", sym)
                    self._select_group("g_fx", "Effects")
                    self._apply_fx_slider_drag(sym, x)
                    self._last_drag_ts = now
                    self.refresh_gui(sample_telemetry=False)
                    return
            return

        # 3. Middle Morph A & Morph B strip (with double-tap reset per GEMINI.md rule 1)
        if self._in_rect(x, y, self.morph_a_box):
            self.selected_chain = "a"
            self._select_group("g_flt_a", "Filter A")
            if self._check_double_tap("morph_a", now):
                self._set("morph_a", DEFAULTS["morph_a"])
                self.refresh_gui(sample_telemetry=False)
                return
            self.drag_mode = ("morph_a",)
            self._apply_drag(x, y)
            self._last_drag_ts = now
            self.refresh_gui(sample_telemetry=False)
            return

        if self._in_rect(x, y, self.morph_b_box):
            self.selected_chain = "b"
            self._select_group("g_flt_b", "Filter B")
            if self._check_double_tap("morph_b", now):
                self._set("morph_b", DEFAULTS["morph_b"])
                self.refresh_gui(sample_telemetry=False)
                return
            self.drag_mode = ("morph_b",)
            self._apply_drag(x, y)
            self._last_drag_ts = now
            self.refresh_gui(sample_telemetry=False)
            return

        # 4. Graph area: pick nearest node A or B (with double-tap reset to default Cutoff & Resonance)
        if self._in_rect(x, y, self.graph_box):
            xa, ya = self.node_pos.get("a", (0.0, 0.0))
            xb, yb = self.node_pos.get("b", (0.0, 0.0))
            da = (x - xa) ** 2 + (y - ya) ** 2
            db = (x - xb) ** 2 + (y - yb) ** 2
            which = "a" if da <= db else "b"
            self.selected_chain = which
            self._select_group("g_flt_a" if which == "a" else "g_flt_b")
            if self._check_double_tap(f"node_{which}", now):
                self._set(f"cutoff_{which}", DEFAULTS[f"cutoff_{which}"])
                self._set(f"res_{which}", DEFAULTS[f"res_{which}"])
                self.refresh_gui(sample_telemetry=False)
                return
            self.drag_mode = ("node", which)
            self._apply_drag(x, y)
            self._last_drag_ts = now
            self.refresh_gui(sample_telemetry=False)
            return

    def _on_motion(self, event):
        if not self.drag_mode or self.drag_mode[0] in ("link_byp", "inv_byp"):
            return
        x, y = float(event.x), float(event.y)
        now = time.monotonic()
        if (now - self._last_drag_ts) < DRAG_INTERVAL:
            self._pending_drag = (x, y)
            return
        self._pending_drag = None
        self._last_drag_ts = now
        self._apply_drag(x, y)
        self.refresh_gui(sample_telemetry=False)

    def _on_release(self, event):
        if not self.drag_mode:
            return
        kind = self.drag_mode[0]
        if kind == "link_byp":
            t0 = self.drag_mode[1]
            self.drag_mode = None
            if (time.monotonic() - t0) >= LONG_PRESS_SEC:
                cur = int(round(self._get("bypass", 0)))
                self._set("bypass", 0 if cur else 1)
                self._select_group("g_master", "Master")
            else:
                if int(round(self._get("bypass", 0))) == 1:
                    self._set("bypass", 0)
                    self._select_group("g_master", "Master")
                else:
                    cur = int(round(self._get("link_mode", 0)))
                    self._set("link_mode", (cur + 1) % len(LINKS))
                    self._select_group("g_route", "Routing")
            self.refresh_gui(sample_telemetry=False)
            return

        if kind == "inv_byp":
            t0 = self.drag_mode[1]
            self.drag_mode = None
            if (time.monotonic() - t0) >= LONG_PRESS_SEC:
                cur = int(round(self._get("bypass", 0)))
                self._set("bypass", 0 if cur else 1)
                self._select_group("g_master", "Master")
            else:
                cur = int(round(self._get("invert_b", 0)))
                self._set("invert_b", 0 if cur else 1)
                self._select_group("g_route", "Routing")
            self.refresh_gui(sample_telemetry=False)
            return

        if self._pending_drag is not None:
            px, py = self._pending_drag
            self._pending_drag = None
            self._apply_drag(px, py)
        else:
            self._apply_drag(float(event.x), float(event.y))

        if kind == "xy" and self.fx_snap:
            self._start_spring(("sweep_fx", "morph_fx"))
        elif kind == "fx_slider" and self.fx_snap and self.drag_mode[1] != "fx_smooth":
            self._start_spring((self.drag_mode[1],))

        self.drag_mode = None
        self.refresh_gui(sample_telemetry=False)
