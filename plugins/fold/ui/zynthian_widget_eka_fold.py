#!/usr/bin/python3
# -*- coding: utf-8 -*-
# Eka Fold widget. Tabs (top left): Osc, Mod, Fold, Gate, Env, Matrix. Stage 5 adds Delay.
# The skeleton follows the Eka Mist widget:
# - Zynthian calls update() every 32 ms; monitors and marks are refreshed only every UPDATE_PERIOD.
# - Curves are computed here from the parameters and redrawn only when a parameter changed, at most every
#   CURVE_PERIOD; only small marks (notes, level) move with the live data, and only when they changed.
# - Live data from the plugin's output ports (self.monitors): note1..4 = MIDI note of each voice, -1 = idle;
#   out_peak = output peak, dB; mod_phase = Mod OSC phase 0..1 of the newest voice, -1 = none.
# - Tabs and knob pages go together: a tap on a tab selects its knob page through Zynthian's control screen;
#   turning to such a page shows its tab. Pages without a tab (Env, for now) keep the tab shown.
# - At the top right, in a small font: the name of the current knob page (screen_title of Zynthian's control screen).
# Osc:
# - Source tiles Sine / VA / FM2 / Wave (a tap selects; the Source knob). Pitch (semitones) and Fine (cents):
#   LV2 parameters set here, a finger up/down on their box, a double tap resets.
# - The Principal waveform over 2 periods for the Source and its knobs Harmonics, Color, Morph, computed with
#   the formulas of src/plaits/engines.cc (without the band-limiting); Wave reads the wave table the build
#   writes next to the plugin (build/eka-fold.lv2/engine_tables.bin). A line says what the knobs do now.
# - The sounding notes with their frequency (Pitch and Fine included) and a small output level meter.
# Mod:
# - Mod OSC block (one period of its shape) -> arrows FM / AM / Sync / Timbre -> Principal block. A tap on an
#   arrow switches that route (LV2 parameters route_fm, route_am, route_sync, route_timbre, 0/1, saved in the
#   snapshot). Timbre: the Mod OSC moves the Timbre knob of the Fold tab.
# - The Principal block shows the result: a sine Principal modulated by the Mod OSC over a few periods,
#   computed here with the plugin's formulas (linear FM, AM, hard sync), whatever the Source. In Free mode the
#   Mod frequency is absolute, so the result is drawn for C4.
# - A running mark of the Mod phase (output mod_phase) on the Mod OSC shape, only while it is slow
#   (Free mode, up to MARK_MAX_HZ): an LFO is seen "breathing"; at audio rates a mark would only flicker.
# - At the bottom: the Mod Index, large, and what the Mod Freq knob means in this Mod Mode
#   (Free: Hz, Keytrack: semitones, Ratio: x p/q), with the shape.
# Fold:
# - Left: the transfer curve of the Timbre section (input -1..1 -> output) for Timbre, Order, Symmetry, Drive.
#   Right: what it makes of the Principal (the Source's wave), 2 periods, without its DC (the plugin removes it).
#   The formulas of fold_core.c: folder (gain 1..8), Saw / M (Chebyshev sums, drawn with FC_CHEB_MAX terms,
#   as a low note gets), Drive (tanh).
# - At the bottom: the Order line Folder - Saw - M with its mark, and the knob values.
# Gate:
# - Buttons VCA / LP / Combo (the Gate Mode knob). The response of the lopass gate for the vactrol open, at 3/4,
#   1/2 and 1/4 (level x low-pass, the formulas of fold_core.c: cutoff 20 Hz..20 kHz exponential in g, a TPT
#   state-variable filter whose |H| is the analog one at w = tan(pi f / sr) / tan(pi fc / sr), at the 2x rate):
#   it shows how the gate closes in each mode. Live: the vactrol of the newest voice (output gate_level) as a
#   bar, and its cutoff as a mark on the frequency axis (LP, Combo).
# - At the bottom: Decay (ms), Resonance, Level.
# Env:
# - FG1 (-> Gate) and FG2 (-> Timbre): Rise, Hold (Gate mode), Fall drawn with log-scaled lengths (5 ms and 2 s
#   both readable), two cycles in Cycle mode; a running point from the outputs fg1, fg2 (0..1 rising, 1.5
#   holding, 2..3 falling, 3 ended). Buttons Once / Gate / Cycle (LV2 parameters fg1_mode, fg2_mode).
# - FG2 -> Timbre depth (LV2 parameter m_fg2_timbre, -100..100 %, the same cell as on the Matrix tab): a finger
#   up/down on its box, a double tap resets it.
# Matrix (no knob page; the page shown stays):
# - The mod matrix as in Eka Mist: sources (rows) x destinations (columns), LV2 parameters m_<source>_<destination>,
#   -100..100 %. Colour = sign (blue +, orange -), brightness = depth; the names of the sources in use are lit.
#   A finger up/down on a cell changes it, a double tap resets it; the line below tells what the touched cell does.

import array
import logging
import math
import os
import sys
import time
import tkinter
import tkinter.font

from zyngui import zynthian_gui_config
from zyngui import zynthian_widget_base

MULTI_INSTANCE = True

TABS = ["Osc", "Mod", "Fold", "Gate", "Env", "Matrix"]
TAB_GROUPS = ["g_osc", "g_mod", "g_timbre", "g_gate", "g_env", None]  # knob page (port group) of each tab
# Mod matrix: FC_MS_* and FC_MD_* of fold_core.h, in their order (symbol, name on the grid, what it is)
MATRIX_SRCS = [("fg1", "FG1", "FG1"), ("fg2", "FG2", "FG2"), ("mod", "Mod", "Mod OSC"), ("rnd", "Rnd", "Random"),
               ("vel", "Vel", "Velocity"), ("key", "Key", "Key"), ("wheel", "Wheel", "Mod wheel"),
               ("at", "AT", "Aftertouch")]
MATRIX_DSTS = [("pitch", "Pitch", "Pit"), ("harmonics", "Harm", "Hrm"), ("color", "Color", "Col"),
               ("morph", "Morph", "Mor"), ("index", "Index", "Idx"), ("order", "Order", "Ord"),
               ("symmetry", "Sym", "Sym"), ("timbre", "Timbre", "Tim"), ("lpg", "LPG", "LPG")]
MATRIX_DST_NAMES = ["Pitch", "Harmonics", "Color", "Morph", "Mod Index", "Order", "Symmetry", "Timbre", "LPG"]
MOD_SEMIS = 12.0                 # FC_MOD_SEMIS: Pitch 100 % = 12 semitones
MOD_POS = (77, 171, 247)         # positive cells: blue (as in Eka Mist)
MOD_NEG = (255, 146, 43)         # negative cells: orange
CELL_BG = (34, 34, 34)
SOURCES = ["Sine", "VA", "FM2", "Wave"]   # Source tiles: FC_SRC_* of fold_core.h, the scale points of Source
VA_INTERVALS = (0.0, 7.01, 12.01, 19.01, 24.01)   # src/plaits/engines.cc
WAVE_LEN = 128
WAVE_STRIDE = 132
NWAVES = 192
FM_QUANT_SIZE = 130
NVOICES = 4
TAB_COLOR = "#4dabf7"
WAVE_COLOR = "#4dabf7"
AXIS_COLOR = "#2a2a2a"
NOTE_COLOR = "#ffd43b"
METER_COLOR = "#51cf66"
METER_HOT = "#ff6b6b"            # above -6 dB
TILE_ON = "#4dabf7"
TILE_OFF = "#2a2a2a"
ROUTE_ON = "#4dabf7"
ROUTE_OFF = "#505050"
MARK_COLOR = "#ffd43b"
LABEL_COLOR = "#909090"
MARK_MAX_HZ = 8.0                # the phase mark is shown up to this Mod frequency
UPDATE_PERIOD = 0.08             # s: monitors and marks at ~12 per second, not at Zynthian's 30
CURVE_PERIOD = 0.15              # s: curves redrawn at most this often
METER_DB = 48.0                  # the meter shows -48..0 dB
NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
MOD_SHAPES = ["Triangle", "Sine", "Saw", "Square"]   # Mod Shape knob, as FC_MOD_* in fold_core.h
MOD_MODES = ["Free", "Keytrack", "Ratio"]            # Mod Mode knob, as FC_MODE_*
ROUTES = [("route_fm", "FM"), ("route_am", "AM"), ("route_sync", "Sync"), ("route_timbre", "Timbre")]
ROUTE_DEFAULTS = {"route_fm": 1, "route_am": 0, "route_sync": 0, "route_timbre": 0}
# Mod Freq knob 0..100, read by the mode: the formulas of fold_core.c
FREQ_MAX = 100.0
FREE_MIN, FREE_MAX = 0.01, 10000.0
KEY_RANGE = 48.0
RATIOS = [(1, 8), (1, 6), (1, 4), (1, 3), (1, 2), (2, 3), (3, 4), (1, 1), (5, 4), (4, 3), (3, 2), (5, 3), (2, 1),
          (5, 2), (3, 1), (7, 2), (4, 1), (5, 1), (6, 1), (7, 1), (8, 1), (9, 1), (10, 1), (12, 1), (16, 1)]
FM_DEPTH = 4.0                   # FC_FM_DEPTH: Mod Index 100 % swings the Principal frequency by 4 x itself
FOLD_GAIN = 8.0                  # FC_FOLD_GAIN, FC_CHEB_MAX, FC_DRIVE_K of fold_core.h
CHEB_MAX = 32
DRIVE_K = 4.0
ORDER_NAMES = ["Folder", "Saw", "M"]
CURVE2_COLOR = "#ffd43b"         # the wave over the transfer curve
GATE_MODES = ["VCA", "LP", "Combo"]   # FC_GATE_* of fold_core.h, Gate Mode knob
FG_MODES = ["Once", "Gate", "Cycle"]  # FC_FG_*
LPG_FMIN, LPG_FMAX = 20.0, 20000.0    # FC_LPG_FMIN, FC_LPG_FMAX, FC_DECAY_MIN_MS, FC_DECAY_MAX_MS of fold_core.h
DECAY_MIN_MS, DECAY_MAX_MS = 10.0, 2000.0
VOICE_SR = 96000.0               # the voices' rate (2x of 48 kHz): the filter's warping
VAC_LEVELS = (1.0, 0.75, 0.5, 0.25)   # vactrol states drawn on the Gate tab
DB_TOP, DB_BOTTOM = 30.0, -48.0
HOLD_W = 1.5                     # Env: width of the hold, in the units of log10(ms) + 1
DOUBLE_TAP = 0.35                # s
DRAG_PX = 1.5                    # px of finger movement per step (1 %, 1 cent)
PITCH_DRAG_PX = 6.0              # px per semitone
C4_HZ = 261.626


def note_name(n):
    return "%s%d" % (NOTE_NAMES[n % 12], n // 12 - 1)


def note_freq(n, pitch, fine):
    return 440.0 * 2.0 ** ((n - 69 + pitch + fine / 100.0) / 12.0)


def _clamp(u, lo, hi):
    return lo if u < lo else hi if u > hi else u


def free_hz(u):
    return FREE_MIN * (FREE_MAX / FREE_MIN) ** (_clamp(u, 0.0, FREQ_MAX) / FREQ_MAX)


def key_semis(u):
    return (_clamp(u, 0.0, FREQ_MAX) / FREQ_MAX * 2.0 - 1.0) * KEY_RANGE


def ratio_index(u):
    # lrintf rounds halves to even; the knob rarely sits on a half step, round() does the same
    return int(round(_clamp(u, 0.0, FREQ_MAX) * (len(RATIOS) - 1) / FREQ_MAX))


def ratio_text(i):
    p, q = RATIOS[i]
    return "%d" % p if q == 1 else "%d/%d" % (p, q)


def hz_text(hz):
    if hz >= 1000.0:
        return "%.2f kHz" % (hz / 1000.0)
    if hz >= 100.0:
        return "%.0f Hz" % hz
    if hz >= 10.0:
        return "%.1f Hz" % hz
    return "%.2f Hz" % hz


def freq_text(mode, u):
    """What the Mod Freq knob means in this mode."""
    if mode == 0:
        return "Free " + hz_text(free_hz(u))
    if mode == 1:
        return "Keytrack %+.1f st" % key_semis(u)
    return "Ratio \u00d7" + ratio_text(ratio_index(u))


def mod_ratio(mode, u):
    """Mod frequency over the Principal's (Free: for C4)."""
    if mode == 0:
        return free_hz(u) / C4_HZ
    if mode == 1:
        return 2.0 ** (key_semis(u) / 12.0)
    p, q = RATIOS[ratio_index(u)]
    return p / q


def mod_value(shape, ph):
    """Mod OSC -1..1 at phase 0..1 (without the band-limiting of the plugin)."""
    if shape == 0:
        return 4.0 * ph if ph < 0.25 else 2.0 - 4.0 * ph if ph < 0.75 else 4.0 * ph - 4.0
    if shape == 2:
        return 2.0 * ph - 1.0
    if shape == 3:
        return 1.0 if ph < 0.5 else -1.0
    return math.sin(2.0 * math.pi * ph)


def result_values(rel, shape, index, fm, am, sync, n, cycles, sub=8):
    """The Principal modulated by the Mod OSC, n + 1 values over `cycles` Principal periods.
    rel = Mod frequency / Principal frequency. Linear FM, AM and hard sync as in fold_core.c."""
    out = []
    ph = mph = 0.0
    dt = cycles / (n * sub)
    fmd = FM_DEPTH * index if fm else 0.0
    ama = index if am else 0.0
    for i in range(n + 1):
        m = mod_value(shape, mph)
        out.append(math.sin(2.0 * math.pi * ph) * ((1.0 - ama) + ama * m))
        for _ in range(sub):
            m = mod_value(shape, mph)
            ph = (ph + dt * (1.0 + fmd * m)) % 1.0
            mph += dt * rel
            if mph >= 1.0:
                mph -= math.floor(mph)
                if sync:
                    ph = 0.0
    return out


def fold(u):
    s = (u + 1.0) * 0.25
    s -= math.floor(s)
    return 4.0 * s - 1.0 if s < 0.5 else 3.0 - 4.0 * s


def fast_tanh(x):
    if x > 3.0:
        return 1.0
    if x < -3.0:
        return -1.0
    x2 = x * x
    return x * (27.0 + x2) / (27.0 + 9.0 * x2)


def order_weights(o):
    """(folder, Saw, M) shares of Order 0..1."""
    if o < 0.5:
        return 1.0 - 2.0 * o, 2.0 * o, 0.0
    return 0.0, 2.0 - 2.0 * o, 2.0 * o - 1.0


def cheb_coefs(ws, wm, n=CHEB_MAX):
    hs = sum(1.0 / k for k in range(1, n + 1))
    hm = sum(1.0 / k for k in range(1, n + 1, 2))
    tot = ws + wm if ws + wm > 1e-6 else 1.0
    return [0.0] + [(ws / hs + (wm / hm if k & 1 else 0.0)) / (k * tot) for k in range(1, n + 1)]


def chebyshev(a, u):
    b1 = b2 = 0.0
    for k in range(len(a) - 1, 0, -1):
        b1, b2 = a[k] + 2.0 * u * b1 - b2, b1
    return u * b1 - b2


def shape(x, timbre, order, sym, drive, a=None):
    """The Timbre section of fold_core.c for one input value (without ADAA and the DC blocker)."""
    wf, ws, wm = order_weights(order)
    sh = wf * fold((1.0 + (FOLD_GAIN - 1.0) * timbre) * x + sym) if wf > 0.0 else 0.0
    if wf < 1.0:
        uc = max(-1.0, min(1.0, x + sym))
        ch = chebyshev(a if a is not None else cheb_coefs(ws, wm), uc) if timbre > 0.0 else 0.0
        sh += (1.0 - wf) * ((1.0 - timbre) * x + timbre * ch)
    if drive > 1e-4:
        kd = 1.0 + DRIVE_K * drive
        sh = (1.0 - drive) * sh + drive * fast_tanh(kd * sh) / fast_tanh(kd)
    return sh


def decay_ms(d):
    return DECAY_MIN_MS * (DECAY_MAX_MS / DECAY_MIN_MS) ** max(0.0, min(1.0, d))


def lpg_db(f, g, mode, res, sr=VOICE_SR):
    """Level of the lopass gate at f Hz for the vactrol state g, dB (fold_core.c: lpg_coefs and the gain)."""
    wv = 1.0 if mode != 1 else 0.0          # VCA share (VCA, Combo)
    wl = 1.0 if mode != 0 else 0.0          # LP share (LP, Combo)
    gain = 1.0 - wv + wv * g
    h = 1.0
    if wl > 0.0:
        fmax = min(LPG_FMAX, 0.45 * sr / 2)
        fc = LPG_FMIN * (fmax / LPG_FMIN) ** (1.0 - wl + wl * g)
        w = math.tan(math.pi * min(f, 0.49 * sr) / sr) / math.tan(math.pi * fc / sr)
        k = math.sqrt(2.0) * (1.0 - 0.96 * res)
        h = 1.0 / math.sqrt((1.0 - w * w) ** 2 + (k * w) ** 2)
    return 20.0 * math.log10(max(gain * h, 1e-9))


def lpg_cutoff(g, mode, sr=VOICE_SR):
    if mode == 0:
        return None
    fmax = min(LPG_FMAX, 0.45 * sr / 2)
    return LPG_FMIN * (fmax / LPG_FMIN) ** g


def fg_units(ms):
    return math.log10(max(1.0, min(10000.0, ms))) + 1.0


def fg_shape(rise, fall, mode):
    """Segments of a function generator curve, [(width units, level from, level to)]."""
    r, f = fg_units(rise), fg_units(fall)
    if mode == 1:
        return [(r, 0.0, 1.0), (HOLD_W, 1.0, 1.0), (f, 1.0, 0.0)]
    if mode == 2:
        return [(r, 0.0, 1.0), (f, 1.0, 0.0), (r, 0.0, 1.0), (f, 1.0, 0.0)]
    return [(r, 0.0, 1.0), (f, 1.0, 0.0)]


def fg_point(pos, rise, fall, mode):
    """(x as a share of the curve width, level) of an fg output, or None."""
    if pos < 0.0 or pos >= 3.0:
        return None
    segs = fg_shape(rise, fall, mode)
    total = sum(w for w, a, b in segs)
    r, f = segs[0][0], fg_units(fall)
    if pos < 1.0:
        return pos * r / total, pos
    if pos < 2.0:
        if mode == 1:
            return (r + HOLD_W / 2.0) / total, 1.0
        return r / total, 1.0
    lvl = 3.0 - pos
    start = r + (HOLD_W if mode == 1 else 0.0)
    return (start + (1.0 - lvl) * f) / total, lvl


def ms_text(ms):
    return "%.1f s" % (ms / 1000.0) if ms >= 1000.0 else "%d ms" % round(ms)


# ---------- the Principal sources (src/plaits/engines.cc, without its band-limiting) ----------
_TABLES = None                   # (waves, fm quantizer) from build/eka-fold.lv2/engine_tables.bin, or False


def engine_tables():
    """The tables the build writes next to the plugin (scripts/dump_waves.c); False if missing."""
    global _TABLES
    if _TABLES is None:
        _TABLES = False
        try:
            here = os.path.dirname(os.path.realpath(__file__))
            path = os.path.join(here, "..", "build", "eka-fold.lv2", "engine_tables.bin")
            with open(path, "rb") as f:
                data = f.read()
            waves = array.array("h")
            waves.frombytes(data[:2 * NWAVES * WAVE_STRIDE])
            fmq = array.array("f")
            fmq.frombytes(data[2 * NWAVES * WAVE_STRIDE:2 * NWAVES * WAVE_STRIDE + 4 * FM_QUANT_SIZE])
            if sys.byteorder == "big":
                waves.byteswap()
                fmq.byteswap()
            if len(waves) == NWAVES * WAVE_STRIDE and len(fmq) == FM_QUANT_SIZE:
                _TABLES = (waves, fmq)
        except Exception as e:
            logging.debug("Eka Fold widget: no engine tables => %s", e)
    return _TABLES


def va_detune(h):
    """Interval of the second VA oscillator, semitones (ComputeDetuning)."""
    d = _clamp(2.05 * h - 1.025, -1.0, 1.0)
    sign = -1.0 if d < 0.0 else 1.0
    d = d * sign * 3.9999
    i = int(d)
    f = d - i
    sq = lambda x: x * x * (3.0 - 2.0 * x)
    return (VA_INTERVALS[i] + (VA_INTERVALS[i + 1] - VA_INTERVALS[i]) * sq(sq(f))) * sign


def va_shape(k):
    """(waveshape, pulse width) of a VA oscillator for its knob 0..1."""
    return _clamp(k * 1.5, 0.0, 1.0), _clamp(0.5 + (k - 0.66) * 1.4, 0.5, 0.99)


def va_shape_name(k):
    ws, pw = va_shape(k)
    if pw > 0.505:
        return "pulse %d%%" % round(pw * 100)
    if ws < 0.02:
        return "tri"
    if abs(ws - 0.5) < 0.02:
        return "saw"
    if ws > 0.98:
        return "square"
    return "tri-saw" if ws < 0.5 else "saw-sq"


def va_value(ph, ws, pw):
    sq = (ws - 0.5) * 2.0 if ws > 0.5 else 0.0
    ta = 1.0 - 2.0 * ws if ws < 0.5 else 0.0
    square = 0.0 if ph < pw else 1.0
    tri = ph / pw if ph < pw else 1.0 - (ph - pw) / (1.0 - pw)
    v = ph + (square - ph) * sq
    v += (tri - v) * ta
    return 2.0 * v - 1.0


def fm_semis(h, fmq):
    x = _clamp(h, 0.0, 1.0) * 128.0
    i = min(int(x), FM_QUANT_SIZE - 2)
    return fmq[i] + (fmq[i + 1] - fmq[i]) * (x - i)


def fm_params(h, c, m, fmq, note=60):
    """(ratio, index, carrier -> modulator frequency, modulator -> itself) as in fe_block."""
    semis = fm_semis(h, fmq)
    taming = _clamp(1.0 - (note + semis - 72.0) * 0.025, 0.0, 1.0) ** 2
    fb = 2.0 * m - 1.0
    return (2.0 ** (semis / 12.0), 2.0 * c * c * taming, 0.5 * fb * fb if fb < 0.0 else 0.0,
            0.25 * fb * fb if fb > 0.0 else 0.0)


def wave_coords(h, c, m):
    """(column, row, bank) of Wave, snapped in the upper half of Harmonics, as in fe_block."""
    out = []
    v = (c * 6.9999, m * 6.9999, h * 6.9999)
    q = _clamp(v[2] - 3.0, 0.0, 1.0)
    for x in v:
        i = math.floor(x)
        f = x - i
        f += q * (_clamp((f - 0.5) * 16.0, -0.5, 0.5) + 0.5 - f)
        out.append(i + f)
    return out


def _wave_offset(x, y, z):
    if z >= 4:
        z = 7 - z
    i = x + y * 8 + z * 64
    if z == 3:
        i = i * 101 % NWAVES
    return i * WAVE_STRIDE


def _hermite_slope(t, o, i, f):
    xm1, x0, x1, x2 = t[o + i], t[o + i + 1], t[o + i + 2], t[o + i + 3]
    c = (x1 - xm1) * 0.5
    v = x0 - x1
    w = c + v
    a = w + v + (x2 - x0) * 0.5
    b = w + a
    return (3.0 * a * f - 2.0 * b) * f + c


def wave_value(waves, coords, ph):
    """The blended wave at phase ph, -1..1 (the slope of its integral)."""
    p = (ph % 1.0) * WAVE_LEN
    pi = min(int(p), WAVE_LEN - 1)
    pf = p - pi
    x, y, z = coords
    xi, yi, zi = int(x), int(y), int(z)
    xf, yf, zf = x - xi, y - yi, z - zi
    out = 0.0
    for k in range(8):
        dx, dy, dz = k & 1, (k >> 1) & 1, k >> 2
        w = (xf if dx else 1.0 - xf) * (yf if dy else 1.0 - yf) * (zf if dz else 1.0 - zf)
        if w:
            out += w * _hermite_slope(waves, _wave_offset(xi + dx, yi + dy, zi + dz), pi, pf)
    return out / 1024.0


def principal_values(source, h, c, m, n, periods=2.0):
    """The Principal, n + 1 values over `periods` periods, for Source and its knobs 0..1."""
    tables = engine_tables()
    if source == 1:
        r = 2.0 ** (va_detune(h) / 12.0)
        s1, s2 = va_shape(c), va_shape(m)
        return [0.5 * (va_value((periods * i / n) % 1.0, *s1) + va_value((periods * i / n * r) % 1.0, *s2))
                for i in range(n + 1)]
    if source == 2 and tables:
        ratio, amount, pfb, mfb = fm_params(h, c, m, tables[1])
        steps = 256                                          # per period; 2 periods of settling first
        coef = 1.0 - math.exp(-2.0 * math.pi * 1530.0 / (steps * C4_HZ))
        cph = mph = fb = 0.0
        ys = []
        total = int((periods + 2.0) * steps)
        for k in range(total + 1):
            mph += ratio * (1.0 + fb * pfb) / steps
            cph += 1.0 / steps
            mod = math.sin(2.0 * math.pi * (mph + mfb * fb))
            y = math.sin(2.0 * math.pi * (cph + amount * mod))
            fb += (y - fb) * coef
            ys.append(y)
        ys = ys[2 * steps:]
        return [ys[min(int(i * (len(ys) - 1) / n), len(ys) - 1)] for i in range(n + 1)]
    if source == 3 and tables:
        coords = wave_coords(h, c, m)
        return [wave_value(tables[0], coords, periods * i / n) for i in range(n + 1)]
    return [math.sin(2.0 * math.pi * periods * i / n) for i in range(n + 1)]


def source_text(source, h, c, m):
    """What Harmonics, Color and Morph do now, short."""
    tables = engine_tables()
    if source == 1:
        return "2nd osc %+.1f st   1: %s   2: %s" % (va_detune(h), va_shape_name(c), va_shape_name(m))
    if source == 2 and tables:
        ratio, amount, pfb, mfb = fm_params(h, c, m, tables[1])
        fb = round((2.0 * m - 1.0) * 100)
        return "ratio %.2f   index %.2f   feedback %+d%%" % (ratio, amount, fb)
    if source == 3:
        x, y, z = wave_coords(h, c, m)
        zz = int(z)
        bank = (7 - zz if zz >= 4 else zz) + 1
        return "bank %d%s   column %.1f   row %.1f" % (bank, " stepped" if z >= 4.0 else "", x + 1, y + 1)
    return "Harmonics, Color, Morph: not used by Sine"


def wave_points(values, x0, x1, y0, y1):
    """Points of values over the box, DC removed (the plugin removes it)."""
    mid, amp = (y0 + y1) / 2.0, (y1 - y0) / 2.0 * 0.9
    n = len(values) - 1
    mean = sum(values[:-1]) / n
    pts = []
    for i, y in enumerate(values):
        pts += [x0 + i * (x1 - x0) / n, mid - _clamp(y - mean, -1.1, 1.1) * amp]
    return pts


def mix(rgb, f, bg=CELL_BG):
    """rgb blended into the cell background: f = 0 background, 1 the colour."""
    f = max(0.0, min(1.0, f))
    return "#%02x%02x%02x" % tuple(int(b + (c - b) * f) for c, b in zip(rgb, bg))


def cell_sym(si, di):
    return "m_%s_%s" % (MATRIX_SRCS[si][0], MATRIX_DSTS[di][0])


def cell_text(si, di, v):
    """What a cell does, for the line under the grid."""
    head = "%s \u2192 %s %+d%%" % (MATRIX_SRCS[si][2], MATRIX_DST_NAMES[di], v)
    if di == 0 and v:
        head += " (%+.1f semitones at the top)" % (MOD_SEMIS * v / 100.0)
    elif MATRIX_DSTS[di][0] == "lpg" and v:
        head += " (the gate %s)" % ("opens wider" if v > 0 else "closes")
    return head


class zynthian_widget_eka_fold(zynthian_widget_base.zynthian_widget_base):

    def __init__(self, parent):
        super().__init__(parent)
        self.header_h = {}           # font size -> line height
        self.next_update = 0.0
        self.tab = 0
        self.tab_boxes = []          # (x0, x1) of each tab
        self.last_group = None       # knob page (port group symbol) seen on the previous refresh
        self.widget_canvas = tkinter.Canvas(
            self, highlightthickness=0, relief='flat',
            bg=zynthian_gui_config.color_bg)
        self.widget_canvas.grid(sticky='news')
        self.widget_canvas.bind("<ButtonPress-1>", self._on_press)
        self.widget_canvas.bind("<B1-Motion>", self._on_motion)
        self.widget_canvas.bind("<ButtonRelease-1>", self._on_release)
        self.drag = None             # (symbol, start value, start y, min, max, px per step) on a value box
        self.last_tap = (None, 0.0)  # (symbol, time) for a double tap
        self.mx_sel = None           # Matrix: the cell touched last (si, di)
        self._clear()

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
        self._clear()
        self.refresh_gui()

    def _clear(self):
        """Empty canvas: everything is drawn again on the next refresh (tab switch, resize)."""
        self.widget_canvas.delete("all")
        self.title = None            # knob page name shown now
        self.tabs_drawn = None       # (tab, size, title) the tab bar was drawn for
        self.wave_view = None        # values the waveform was drawn for
        self.wave_drawn = 0.0        # when it was drawn
        self.tiles_view = None
        self.tile_boxes = []         # (x0, y0, x1, y1, source index)
        self.value_boxes = []        # Osc: (x0, y0, x1, y1, symbol, min, max, px per step): Pitch, Fine
        self.info_view = None
        self.notes_view = None       # text of the sounding notes shown now
        self.meter_view = None       # meter height shown now
        self.mod_view = None         # values the Mod tab curves were drawn for
        self.mod_drawn = 0.0
        self.route_boxes = []        # (x0, y0, x1, y1, symbol)
        self.mark_view = None        # where the phase mark is shown, None = hidden
        self.mark_item = None
        self.fold_view = None        # values the Fold tab was drawn for
        self.fold_drawn = 0.0
        self.gate_view = None        # values the Gate tab was drawn for
        self.gate_drawn = 0.0
        self.gate_live = None        # bar height and cutoff mark drawn
        self.gate_buttons = []       # (x0, y0, x1, y1, value)
        self.env_view = None
        self.env_drawn = 0.0
        self.env_points = [None, None]   # point position drawn per FG
        self.env_buttons = []        # (x0, y0, x1, y1, symbol, value)
        self.depth_box = None        # FG2 -> Timbre box
        self.env_curves = [None, None]   # (x0, y0, x1, y1) of each curve area
        self.mx_items = None         # Matrix: (si, di) -> (rectangle, text)
        self.mx_vals = {}            # (si, di) -> value drawn
        self.mx_labels = []          # source name items
        self.mx_info = None          # text of the line under the grid

    # ---------- plugin access ----------
    def _page_title(self):
        try:
            return str(self.zyngui_control.screen_title or "")
        except Exception:
            return ""

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
            logging.debug("Eka Fold widget: can't set %s => %s", sym, e)
            return False

    def _route(self, sym):
        return 1 if self._zval(sym, ROUTE_DEFAULTS[sym]) >= 0.5 else 0

    def _mon(self, key, default=0.0):
        try:
            return float((self.monitors or {}).get(key, default))
        except Exception:
            return default

    def _font_size(self):
        return max(8, min(max(self.height, 20) // 14, 13))

    def _font(self, size=None):
        return (zynthian_gui_config.font_family, size or self._font_size())

    def _header_h(self):
        """Height of the page name line, from the real font metrics (Tk sizes are points, not pixels)."""
        fs = self._font_size()
        if fs not in self.header_h:
            try:
                f = tkinter.font.Font(family=zynthian_gui_config.font_family, size=fs)
                self.header_h[fs] = f.metrics("linespace") + 4
            except Exception:
                self.header_h[fs] = 2 * fs
        return self.header_h[fs]

    # ---------- tab <-> knob page ----------
    def _page_rows(self):
        """Zynthian's list of control pages: (index, group symbol) of this processor's pages."""
        try:
            rows = self.zyngui_control.list_data
        except Exception:
            return []
        out = []
        for i, row in enumerate(rows):
            if len(row) >= 5 and row[0] and row[3] is self.processor:
                out.append((i, row[0]))
        return out

    def _select_page(self, tab):
        for i, group in self._page_rows():
            if group == TAB_GROUPS[tab]:
                try:
                    self.zyngui_control.select(i)
                except Exception as e:
                    logging.debug("Eka Fold widget: can't select page %s => %s", group, e)
                return

    def _current_group(self):
        try:
            info = self.zyngui_control.screen_info
            if info and len(info) >= 5 and info[3] is self.processor:
                return info[0]
        except Exception:
            pass
        return None

    def _follow_page(self):
        """The knob page changed (a knob or the menu): show its tab. Pages without a tab keep the tab."""
        group = self._current_group()
        if group == self.last_group:
            return
        self.last_group = group
        if group and group in TAB_GROUPS and TAB_GROUPS.index(group) != self.tab:
            self.tab = TAB_GROUPS.index(group)
            self._clear()

    # ---------- touch ----------
    def _on_press(self, event):
        if event.y < self._header_h():                              # tab bar
            for i, (x0, x1) in enumerate(self.tab_boxes):
                if x0 <= event.x < x1:
                    if i != self.tab:
                        self.tab = i
                        self._clear()
                    if TAB_GROUPS[i]:                                # Matrix has no page: the page stays
                        self.last_group = TAB_GROUPS[i]
                        self._select_page(i)                        # a tap on the shown tab also brings its page back
                    self.refresh_gui()
            return
        if self.tab == 1:
            for x0, y0, x1, y1, sym in self.route_boxes:
                if x0 <= event.x <= x1 and y0 <= event.y <= y1:
                    self._zset(sym, 1 - self._route(sym))
                    self.mod_view = None
                    self.mod_drawn = 0.0                    # a tap shows at once
                    self.refresh_gui()
                    return
        if self.tab == 3:
            for x0, y0, x1, y1, value in self.gate_buttons:
                if x0 <= event.x <= x1 and y0 <= event.y <= y1:
                    self._zset("gate_mode", value)
                    self.gate_view = None
                    self.gate_drawn = 0.0
                    self.refresh_gui()
                    return
        if self.tab == 4:
            for x0, y0, x1, y1, sym, value in self.env_buttons:
                if x0 <= event.x <= x1 and y0 <= event.y <= y1:
                    self._zset(sym, value)
                    self.env_view = None
                    self.env_drawn = 0.0
                    self.refresh_gui()
                    return
            b = self.depth_box
            if b and b[0] <= event.x <= b[2] and b[1] <= event.y <= b[3]:
                now = time.monotonic()
                sym = "m_fg2_timbre"
                if self.last_tap[0] == sym and now - self.last_tap[1] < DOUBLE_TAP:
                    self._zset(sym, 0)                      # double tap: reset
                    self.last_tap = (None, 0.0)
                    self.drag = None
                    self.env_view = None
                    self.env_drawn = 0.0
                    self.refresh_gui()
                    return
                self.last_tap = (sym, now)
                self.drag = (sym, int(round(self._zval(sym, 0))), event.y, -100, 100, DRAG_PX)
        if self.tab == 5:
            cell = self._mx_cell(event.x, event.y)
            if cell:
                sym = cell_sym(*cell)
                self.mx_sel = cell
                now = time.monotonic()
                if self.last_tap[0] == sym and now - self.last_tap[1] < DOUBLE_TAP:
                    self._zset(sym, 0)                      # double tap: reset
                    self.last_tap = (None, 0.0)
                    self.drag = None
                else:
                    self.last_tap = (sym, now)
                    self.drag = (sym, int(round(self._zval(sym, 0))), event.y, -100, 100, DRAG_PX)
                self.refresh_gui()
            return
        if self.tab == 0:
            for x0, y0, x1, y1, i in self.tile_boxes:
                if x0 <= event.x <= x1 and y0 <= event.y <= y1:
                    self._zset("source", i)
                    self.tiles_view = self.wave_view = self.info_view = None
                    self.wave_drawn = 0.0
                    self.refresh_gui()
                    return
            for x0, y0, x1, y1, sym, lo, hi, px in self.value_boxes:
                if x0 <= event.x <= x1 and y0 <= event.y <= y1:
                    now = time.monotonic()
                    if self.last_tap[0] == sym and now - self.last_tap[1] < DOUBLE_TAP:
                        self._zset(sym, 0)                  # double tap: reset
                        self.last_tap = (None, 0.0)
                        self.drag = None
                        self.tiles_view = self.notes_view = None
                        self.refresh_gui()
                        return
                    self.last_tap = (sym, now)
                    self.drag = (sym, int(round(self._zval(sym, 0))), event.y, lo, hi, px)
                    return

    def _on_motion(self, event):
        if not self.drag:
            return
        sym, start, y0, lo, hi, px = self.drag
        value = max(lo, min(hi, start + int((y0 - event.y) / px)))
        if value != int(round(self._zval(sym, 0))):
            self._zset(sym, value)
            self.env_view = self.tiles_view = self.notes_view = None
            self.refresh_gui()

    def _on_release(self, event):
        self.drag = None

    # ---------- drawing ----------
    def refresh_gui(self):
        if not self.processor:
            return
        self._follow_page()
        self._draw_tabs()
        self._draw_title()
        if self.tab == 0:
            self._draw_osc()
        elif self.tab == 1:
            self._draw_mod()
        elif self.tab == 2:
            self._draw_fold()
        elif self.tab == 3:
            self._draw_gate()
        elif self.tab == 4:
            self._draw_env()
        elif self.tab == 5:
            self._draw_matrix()

    def _tab_metrics(self, title):
        """(font size, padding, widths, title shown): the tabs and the page title share the top line. The padding
        shrinks first, then the font; if the tabs still do not fit beside the title, the title goes."""
        fam, fs = zynthian_gui_config.font_family, self._font_size()

        def measure(size, text):
            try:
                return tkinter.font.Font(family=fam, size=size).measure(text)
            except Exception:
                return len(text) * size
        w = max(self.width, 80)
        for show in ((True, False) if title else (False,)):
            avail = w - 4 - (measure(fs, title) + 14 if show else 0)
            for size in range(fs, max(7, fs - 4) - 1, -1):
                for pad in (2 * size, size, size // 2):
                    widths = [measure(size, t) + pad for t in TABS]
                    if sum(widths) + 4 * len(TABS) <= avail:
                        return size, pad, widths, show
        size = max(7, fs - 4)
        return size, size // 2, [measure(size, t) + size // 2 for t in TABS], False

    def _draw_tabs(self):
        title = self._page_title()
        key = (self.tab, self.width, self.height, title)
        if key == self.tabs_drawn:
            return
        self.tabs_drawn = key
        c = self.widget_canvas
        c.delete("tabs")
        hh = self._header_h()
        size, pad, widths, show = self._tab_metrics(title)
        self.title_shown = show
        self.title = None                                 # the title follows (shown or not)
        x = 2
        self.tab_boxes = []
        for i, (t, tw) in enumerate(zip(TABS, widths)):
            sel = i == self.tab
            c.create_rectangle(x, 1, x + tw, hh - 2, outline=TAB_COLOR, fill=(TAB_COLOR if sel else ""), tags="tabs")
            c.create_text(x + tw / 2, hh / 2, text=t, font=self._font(size), tags="tabs",
                          fill=("#000000" if sel else TAB_COLOR))
            self.tab_boxes.append((x, x + tw + 2))
            x += tw + 4

    def _draw_title(self):
        title = self._page_title()
        if not getattr(self, "title_shown", True):
            title = ""                                    # no room beside the tabs
        if title == self.title:
            return
        self.title = title
        c = self.widget_canvas
        c.delete("title")
        c.create_text(max(self.width, 20) - 6, 2, anchor="ne", text=title, tags="title",
                      fill=zynthian_gui_config.color_tx_off, font=self._font())

    # ---------- Osc tab ----------
    def _osc_layout(self):
        """Boxes: the top row (Source tiles, then Pitch and Fine), the waveform, the meter at its right, the line
        of what the knobs do, the notes line."""
        w, h = max(int(self.width), 60), max(int(self.height), 60)
        hh, fs = self._header_h(), self._font_size()
        row = 2 * fs + 6
        tiles = (4, hh + 4, w - 4, hh + 4 + row)
        meter_w = max(6, fs // 2 + 2)
        notes_y = h - row // 2 - 2
        info_y = notes_y - fs - 6
        wave = (4, tiles[3] + 6, w - meter_w - 12, info_y - fs // 2 - 4)
        meter = (w - meter_w - 4, wave[1], w - 4, wave[3])
        return tiles, wave, meter, notes_y, info_y

    def _source(self):
        return int(_clamp(round(self._zval("source", 0)), 0, len(SOURCES) - 1))

    def _knobs(self):
        return tuple(_clamp(self._zval(k, 50.0) / 100.0, 0.0, 1.0) for k in ("harmonics", "color", "morph"))

    def _draw_osc(self):
        tiles, wave, meter, notes_y, info_y = self._osc_layout()
        self._draw_tiles(tiles)
        self._draw_wave(wave)
        self._draw_info(info_y)
        self._draw_meter(meter)
        self._draw_notes(notes_y)

    def _draw_tiles(self, box):
        source = self._source()
        pitch, fine = int(round(self._zval("pitch", 0))), int(round(self._zval("fine", 0)))
        view = (box, source, pitch, fine)
        if view == self.tiles_view:
            return
        self.tiles_view = view
        c = self.widget_canvas
        c.delete("tiles")
        self.tile_boxes = []
        self.value_boxes = []
        x0, y0, x1, y1 = box
        fs = self._font_size()
        try:
            bw = tkinter.font.Font(family=zynthian_gui_config.font_family, size=fs).measure("Fine +100") + fs
        except Exception:
            bw = 9 * fs
        tw = min((x1 - x0 - 2 * bw - 8) / len(SOURCES), 140)
        for i, name in enumerate(SOURCES):
            bx = x0 + i * tw
            on = i == source
            c.create_rectangle(bx, y0, bx + tw - 4, y1, outline=TILE_ON, fill=(TILE_ON if on else TILE_OFF),
                               tags="tiles")
            c.create_text(bx + (tw - 4) / 2, (y0 + y1) / 2, text=name, font=self._font(),
                          fill=("#000000" if on else zynthian_gui_config.color_tx), tags="tiles")
            self.tile_boxes.append((bx, y0, bx + tw - 4, y1, i))
        for k, (sym, text, lo, hi, px) in enumerate((("pitch", "Pitch %+d" % pitch, -24, 24, PITCH_DRAG_PX),
                                                    ("fine", "Fine %+d" % fine, -100, 100, DRAG_PX))):
            bx1 = x1 - (1 - k) * (bw + 4)
            bx0 = bx1 - bw
            c.create_rectangle(bx0, y0, bx1, y1, outline=ROUTE_OFF, tags="tiles")
            c.create_text((bx0 + bx1) / 2, (y0 + y1) / 2, text=text, font=self._font(),
                          fill=zynthian_gui_config.color_tx, tags="tiles")
            self.value_boxes.append((bx0, y0, bx1, y1, sym, lo, hi, px))

    def _draw_wave(self, box):
        source, knobs = self._source(), self._knobs()
        view = (box, source, knobs if source else None)
        if view == self.wave_view:
            return
        now = time.monotonic()
        if self.wave_view is not None and now - self.wave_drawn < CURVE_PERIOD:
            return                                        # changed again: drawn on a later refresh
        self.wave_view = view
        self.wave_drawn = now
        c = self.widget_canvas
        c.delete("wave")
        x0, y0, x1, y1 = box
        mid = (y0 + y1) / 2
        c.create_rectangle(x0, y0, x1, y1, outline=AXIS_COLOR, tags="wave")
        c.create_line(x0, mid, x1, mid, fill=AXIS_COLOR, tags="wave")
        n = max(int((x1 - x0) / 2), 8)
        values = principal_values(source, *knobs, n)
        c.create_line(*wave_points(values, x0, x1, y0, y1), fill=WAVE_COLOR, width=2, tags="wave")

    def _draw_info(self, y):
        text = source_text(self._source(), *self._knobs())
        view = (y, self.width, text)
        if view == self.info_view:
            return
        self.info_view = view
        c = self.widget_canvas
        c.delete("info")
        c.create_text(6, y, anchor="w", text=text, font=self._font(max(7, self._font_size() - 2)),
                      fill=LABEL_COLOR, tags="info")

    def _draw_meter(self, box):
        x0, y0, x1, y1 = box
        db = self._mon("out_peak", -100.0)
        frac = min(max((db + METER_DB) / METER_DB, 0.0), 1.0)
        hpx = int(round(frac * (y1 - y0)))
        view = (box, hpx, db > -6.0)
        if view == self.meter_view:
            return
        self.meter_view = view
        c = self.widget_canvas
        c.delete("meter")
        c.create_rectangle(x0, y0, x1, y1, outline=AXIS_COLOR, tags="meter")
        if hpx > 0:
            c.create_rectangle(x0 + 1, y1 - hpx, x1 - 1, y1 - 1, outline="",
                               fill=(METER_HOT if db > -6.0 else METER_COLOR), tags="meter")

    def _notes_text(self):
        pitch, fine = self._zval("pitch", 0.0), self._zval("fine", 0.0)
        notes = []
        for i in range(NVOICES):
            n = int(round(self._mon("note%d" % (i + 1), -1.0)))
            if 0 <= n <= 127 and n not in notes:
                notes.append(n)
        if not notes:
            return ""
        return "   ".join("%s %.1f Hz" % (note_name(n), note_freq(n, pitch, fine)) for n in sorted(notes))

    def _draw_notes(self, y):
        text = self._notes_text()
        view = (y, self.width, text)
        if view == self.notes_view:
            return
        self.notes_view = view
        c = self.widget_canvas
        c.delete("notes")
        c.create_text(6, y, anchor="w", text=text, font=self._font(), fill=NOTE_COLOR, tags="notes")

    # ---------- Mod tab ----------
    def _mod_layout(self):
        """Boxes: Mod OSC (left), the route arrows (middle), Principal (right); the bottom line (Index, mode)."""
        w, h = max(int(self.width), 60), max(int(self.height), 60)
        hh, fs = self._header_h(), self._font_size()
        bottom = 2 * fs + 10                            # the Index is drawn larger than the rest
        y0, y1 = hh + 4, h - bottom - 4
        xa, xb = int(w * 0.36), int(w * 0.64)
        left = (4, y0, xa - 4, y1)
        mid = (xa + 4, y0, xb - 4, y1)
        right = (xb + 4, y0, w - 4, y1)
        return left, mid, right, h - bottom / 2 - 2

    def _mod_values(self):
        return (int(round(self._zval("mod_shape", 1))), int(round(self._zval("mod_mode", 2))),
                self._zval("mod_freq", 30.0), self._zval("mod_index", 0.0) / 100.0,
                self._route("route_fm"), self._route("route_am"), self._route("route_sync"),
                self._route("route_timbre"))

    def _draw_mod(self):
        left, mid, right, bottom_y = self._mod_layout()
        vals = self._mod_values()
        view = (left, mid, right, vals)
        if view != self.mod_view:
            now = time.monotonic()
            if self.mod_view is None or now - self.mod_drawn >= CURVE_PERIOD:
                self.mod_view = view
                self.mod_drawn = now
                self._draw_mod_static(left, mid, right, bottom_y, vals)
                self.mark_view = None
        self._draw_mod_mark(left, vals)

    def _box_label(self, box, text, tag):
        self.widget_canvas.create_text(box[0] + 4, box[1] + 2, anchor="nw", text=text, fill=LABEL_COLOR,
                                       font=self._font(max(7, self._font_size() - 2)), tags=tag)

    def _curve_box(self, box):
        """The part of a block under its label."""
        lh = self._font_size() + 6
        return box[0] + 4, box[1] + lh, box[2] - 4, box[3] - 4

    def _draw_mod_static(self, left, mid, right, bottom_y, vals):
        shape, mode, u, index, fm, am, sync, timb = vals
        c = self.widget_canvas
        c.delete("mod")
        fs = self._font_size()
        # Mod OSC block: one period of its shape
        c.create_rectangle(*left, outline=AXIS_COLOR, tags="mod")
        self._box_label(left, "Mod OSC", "mod")
        x0, y0, x1, y1 = self._curve_box(left)
        mid_y, amp = (y0 + y1) / 2.0, (y1 - y0) / 2.0 * 0.85
        n = max(int((x1 - x0) / 2), 8)
        pts = []
        for i in range(n + 1):
            pts += [x0 + i * (x1 - x0) / n, mid_y - mod_value(shape, i / n) * amp]
        c.create_line(x0, mid_y, x1, mid_y, fill=AXIS_COLOR, tags="mod")
        c.create_line(*pts, fill=WAVE_COLOR, width=2, tags="mod")
        # the route arrows
        self.route_boxes = []
        mx0, my0, mx1, my1 = mid
        gap = 6
        bh = (my1 - my0 - (len(ROUTES) - 1) * gap) / float(len(ROUTES))
        for i, (sym, name) in enumerate(ROUTES):
            on = (fm, am, sync, timb)[i]
            by0 = my0 + i * (bh + gap)
            by1 = by0 + bh
            col = ROUTE_ON if on else ROUTE_OFF
            c.create_rectangle(mx0, by0, mx1, by1, outline=col, fill=(col if on else ""), tags="mod")
            c.create_text((mx0 + mx1) / 2, (by0 + by1) / 2, text=name + " \u2192", font=self._font(),
                          fill=("#000000" if on else zynthian_gui_config.color_tx), tags="mod")
            self.route_boxes.append((mx0, by0, mx1, by1, sym))
        # Principal block: the result
        c.create_rectangle(*right, outline=AXIS_COLOR, tags="mod")
        self._box_label(right, "Principal" + (" at C4" if mode == 0 else ""), "mod")
        x0, y0, x1, y1 = self._curve_box(right)
        mid_y, amp = (y0 + y1) / 2.0, (y1 - y0) / 2.0 * 0.85
        rel = mod_ratio(mode, u)
        cycles = 2.0
        if rel >= 0.25:                                 # show whole Mod periods when the Mod OSC is slower
            cycles = min(max(2.0, 2.0 / rel), 8.0)
        n = max(int((x1 - x0) / 2), 8)
        vals_y = result_values(rel, shape, index, fm, am, sync, n, cycles)   # before the Timbre section
        pts = []
        for i, y in enumerate(vals_y):
            pts += [x0 + i * (x1 - x0) / n, mid_y - y * amp]
        c.create_line(x0, mid_y, x1, mid_y, fill=AXIS_COLOR, tags="mod")
        c.create_line(*pts, fill=WAVE_COLOR, width=2, tags="mod")
        # bottom: the Index (large) and the Mod Freq meaning
        c.create_text(6, bottom_y, anchor="w", text="Index", fill=LABEL_COLOR, font=self._font(), tags="mod")
        try:
            iw = tkinter.font.Font(family=zynthian_gui_config.font_family, size=fs).measure("Index ")
        except Exception:
            iw = 6 * fs
        c.create_text(6 + iw, bottom_y, anchor="w", text="%d%%" % round(index * 100), fill=NOTE_COLOR,
                      font=(zynthian_gui_config.font_family, int(fs * 1.6), "bold"), tags="mod")
        c.create_text(max(self.width, 60) - 6, bottom_y, anchor="e",
                      text="%s \u00b7 %s" % (freq_text(mode, u), MOD_SHAPES[shape % len(MOD_SHAPES)]),
                      fill=zynthian_gui_config.color_tx, font=self._font(), tags="mod")

    def _draw_mod_mark(self, left, vals):
        """The Mod phase on the Mod OSC shape, while the Mod OSC is slow; moved, not recreated."""
        mshape, mode, u = vals[0], vals[1], vals[2]
        ph = self._mon("mod_phase", -1.0)
        show = mode == 0 and free_hz(u) <= MARK_MAX_HZ and 0.0 <= ph <= 1.0
        x0, y0, x1, y1 = self._curve_box(left)
        if show:
            mid_y, amp = (y0 + y1) / 2.0, (y1 - y0) / 2.0 * 0.85
            x = int(round(x0 + ph * (x1 - x0)))
            y = int(round(mid_y - mod_value(mshape, ph) * amp))
            view = (x, y)
        else:
            view = None
        if view == self.mark_view:
            return
        self.mark_view = view
        c = self.widget_canvas
        if view is None:
            c.delete("modmark")
            self.mark_item = None
            return
        r = max(3, self._font_size() // 3)
        if self.mark_item is None or not c.find_withtag("modmark"):
            self.mark_item = c.create_oval(x - r, y - r, x + r, y + r, fill=MARK_COLOR, outline="", tags="modmark")
        else:
            c.coords(self.mark_item, x - r, y - r, x + r, y + r)
        c.tag_raise("modmark")

    # ---------- Fold tab ----------
    def _fold_layout(self):
        """Boxes: transfer curve (left), wave (right); the bottom: Order line (left half), values (right half)."""
        w, h = max(int(self.width), 60), max(int(self.height), 60)
        hh, fs = self._header_h(), self._font_size()
        bottom = 2 * fs + 12
        y0, y1 = hh + 4, h - bottom - 4
        xa = int(w * 0.45)
        left = (4, y0, xa - 4, y1)
        right = (xa + 4, y0, w - 4, y1)
        order = (10, h - bottom + 2, int(w * 0.42), h - 4)
        return left, right, order, h - bottom / 2 - 2

    def _fold_values(self):
        return (self._zval("timbre", 0.0) / 100.0, self._zval("order", 0.0) / 100.0,
                self._zval("symmetry", 0.0) / 100.0, self._zval("drive", 0.0) / 100.0)

    def _fold_source(self):
        source = self._source()
        return (source, self._knobs() if source else None)

    def _draw_fold(self):
        left, right, order, bottom_y = self._fold_layout()
        vals = self._fold_values()
        src = self._fold_source()
        view = (left, right, vals, src)
        if view == self.fold_view:
            return
        now = time.monotonic()
        if self.fold_view is not None and now - self.fold_drawn < CURVE_PERIOD:
            return                                        # changed again: drawn on a later refresh
        self.fold_view = view
        self.fold_drawn = now
        timbre, o, sym, drive = vals
        c = self.widget_canvas
        c.delete("fold")
        wf, ws, wm = order_weights(o)
        a = cheb_coefs(ws, wm) if wf < 1.0 and timbre > 0.0 else None
        # transfer curve, with the diagonal (no change) behind it
        c.create_rectangle(*left, outline=AXIS_COLOR, tags="fold")
        self._box_label(left, "In \u2192 Out", "fold")
        x0, y0, x1, y1 = self._curve_box(left)
        side = min(x1 - x0, y1 - y0)
        cx, cy = (x0 + x1) / 2.0, (y0 + y1) / 2.0
        half = side / 2.0 * 0.92
        c.create_line(cx - half, cy, cx + half, cy, fill=AXIS_COLOR, tags="fold")
        c.create_line(cx, cy - half, cx, cy + half, fill=AXIS_COLOR, tags="fold")
        c.create_line(cx - half, cy + half, cx + half, cy - half, fill=AXIS_COLOR, dash=(2, 3), tags="fold")
        n = max(int(side / 2), 16)
        pts = []
        for i in range(n + 1):
            xin = -1.0 + 2.0 * i / n
            pts += [cx + xin * half, cy - shape(xin, timbre, o, sym, drive, a) * half]
        c.create_line(*pts, fill=WAVE_COLOR, width=2, tags="fold")
        # the Principal (the Source's wave) through it, 2 periods, DC removed
        c.create_rectangle(*right, outline=AXIS_COLOR, tags="fold")
        self._box_label(right, "Principal \u2192 Timbre", "fold")
        x0, y0, x1, y1 = self._curve_box(right)
        mid_y, amp = (y0 + y1) / 2.0, (y1 - y0) / 2.0 * 0.85
        n = max(int((x1 - x0) / 2), 16)
        ys = [shape(x, timbre, o, sym, drive, a) for x in principal_values(src[0], *(src[1] or (0, 0, 0)), n)]
        mean = sum(ys[:-1]) / n
        pts = []
        for i, y in enumerate(ys):
            pts += [x0 + i * (x1 - x0) / n, mid_y - (y - mean) * amp]
        c.create_line(x0, mid_y, x1, mid_y, fill=AXIS_COLOR, tags="fold")
        c.create_line(*pts, fill=CURVE2_COLOR, width=2, tags="fold")
        # Order line: Folder - Saw - M, the mark at Order
        ox0, oy0, ox1, oy1 = order
        ly = oy0 + (oy1 - oy0) * 0.35
        fs = self._font_size()
        small = self._font(max(7, fs - 2))
        c.create_line(ox0, ly, ox1, ly, fill=ROUTE_OFF, width=2, tags="fold")
        for k, name in enumerate(ORDER_NAMES):
            x = ox0 + k * (ox1 - ox0) / 2.0
            share = (wf, ws, wm)[k]
            c.create_oval(x - 3, ly - 3, x + 3, ly + 3, fill=(ROUTE_ON if share > 0.0 else ROUTE_OFF), outline="",
                          tags="fold")
            c.create_text(x, oy1, anchor=("sw" if k == 0 else "se" if k == 2 else "s"), text=name, font=small,
                          fill=(zynthian_gui_config.color_tx if share > 0.0 else LABEL_COLOR), tags="fold")
        mx = ox0 + o * (ox1 - ox0)
        r = max(4, fs // 2)
        c.create_oval(mx - r, ly - r, mx + r, ly + r, fill=MARK_COLOR, outline="", tags="fold")
        vals_t = (round(timbre * 100), round(sym * 100), round(drive * 100))
        text = "Timbre %d%%  Sym %+d  Drive %d%%" % vals_t
        room = max(self.width, 60) - 6 - ox1 - 2 * fs   # right of the Order line
        try:
            if tkinter.font.Font(family=zynthian_gui_config.font_family, size=fs).measure(text) > room:
                text = "T %d  S %+d  D %d" % vals_t
        except Exception:
            pass
        c.create_text(max(self.width, 60) - 6, bottom_y, anchor="e", text=text,
                      fill=zynthian_gui_config.color_tx, font=self._font(), tags="fold")

    # ---------- Gate tab ----------
    def _gate_layout(self):
        """Buttons row (left), the response box, the vactrol bar at its right, the bottom line."""
        w, h = max(int(self.width), 60), max(int(self.height), 60)
        hh, fs = self._header_h(), self._font_size()
        row = 2 * fs + 6
        buttons = (4, hh + 4, min(w - 4, int(w * 0.6)), hh + 4 + row)
        bar_w = max(8, fs)
        bottom = h - row // 2 - 2
        plot = (4, buttons[3] + 6, w - bar_w - 14, h - row - 6)
        bar = (w - bar_w - 4, plot[1], w - 4, plot[3])
        return buttons, plot, bar, bottom

    def _gate_values(self):
        return (int(round(self._zval("gate_mode", 2))), self._zval("gate_decay", 30.0) / 100.0,
                self._zval("gate_res", 0.0) / 100.0, self._zval("level", 80.0) / 100.0)

    @staticmethod
    def _fx(f, x0, x1):
        return x0 + (x1 - x0) * math.log10(f / LPG_FMIN) / math.log10(LPG_FMAX / LPG_FMIN)

    @staticmethod
    def _dby(db, y0, y1):
        db = max(DB_BOTTOM, min(DB_TOP, db))
        return y0 + (y1 - y0) * (DB_TOP - db) / (DB_TOP - DB_BOTTOM)

    def _draw_gate(self):
        buttons, plot, bar, bottom = self._gate_layout()
        vals = self._gate_values()
        view = (buttons, plot, bar, vals)
        if view != self.gate_view:
            now = time.monotonic()
            if self.gate_view is None or now - self.gate_drawn >= CURVE_PERIOD:
                self.gate_view = view
                self.gate_drawn = now
                self._draw_gate_static(buttons, plot, bar, bottom, vals)
                self.gate_live = None
        self._draw_gate_live(plot, bar, vals[0])

    def _draw_gate_static(self, buttons, plot, bar, bottom, vals):
        mode, decay, res, level = vals
        c = self.widget_canvas
        c.delete("gate")
        c.delete("gatelive")
        # mode buttons
        self.gate_buttons = []
        bx0, by0, bx1, by1 = buttons
        bw = (bx1 - bx0) / 3.0
        for i, name in enumerate(GATE_MODES):
            on = i == mode
            x0 = bx0 + i * bw
            c.create_rectangle(x0, by0, x0 + bw - 4, by1, outline=ROUTE_ON if on else ROUTE_OFF,
                               fill=(ROUTE_ON if on else ""), tags="gate")
            c.create_text(x0 + (bw - 4) / 2, (by0 + by1) / 2, text=name, font=self._font(),
                          fill=("#000000" if on else zynthian_gui_config.color_tx), tags="gate")
            self.gate_buttons.append((x0, by0, x0 + bw - 4, by1, i))
        # response for a few vactrol states, brightest = open
        x0, y0, x1, y1 = plot
        c.create_rectangle(*plot, outline=AXIS_COLOR, tags="gate")
        for f in (100.0, 1000.0, 10000.0):
            x = self._fx(f, x0, x1)
            c.create_line(x, y0, x, y1, fill=AXIS_COLOR, tags="gate")
            c.create_text(x + 2, y1 - 2, anchor="sw", text=("%dk" % (f / 1000) if f >= 1000 else "%d" % f),
                          fill=LABEL_COLOR, font=self._font(max(7, self._font_size() - 3)), tags="gate")
        y = self._dby(0.0, y0, y1)
        c.create_line(x0, y, x1, y, fill=AXIS_COLOR, dash=(2, 3), tags="gate")
        n = max(int((x1 - x0) / 3), 16)
        shades = ["#4dabf7", "#3a82bd", "#2c6190", "#204766"]
        for k, g in enumerate(VAC_LEVELS):
            pts = []
            for i in range(n + 1):
                f = LPG_FMIN * (LPG_FMAX / LPG_FMIN) ** (i / n)
                pts += [x0 + (x1 - x0) * i / n, self._dby(lpg_db(f, g, mode, res), y0, y1)]
            c.create_line(*pts, fill=shades[k], width=(2 if k == 0 else 1), tags="gate")
        self._box_label(plot, "g = 1, 3/4, 1/2, 1/4", "gate")
        # the vactrol bar frame
        c.create_rectangle(*bar, outline=AXIS_COLOR, tags="gate")
        # bottom: Decay, Resonance, Level
        c.create_text(6, bottom, anchor="w", text="Decay %s  Res %d%%  Level %d%%" % (
            ms_text(decay_ms(decay)), round(res * 100), round(level * 100)),
            fill=zynthian_gui_config.color_tx, font=self._font(), tags="gate")

    def _draw_gate_live(self, plot, bar, mode):
        """The vactrol bar and the cutoff mark: redrawn only when they changed (two small items)."""
        g = self._mon("gate_level", -1.0)
        bx0, by0, bx1, by1 = bar
        hpx = int(round(max(0.0, min(1.0, g)) * (by1 - by0))) if g >= 0.0 else 0
        fcut = lpg_cutoff(g, mode) if g >= 0.0 else None
        mx = int(round(self._fx(max(LPG_FMIN, min(LPG_FMAX, fcut)), plot[0], plot[2]))) if fcut else None
        view = (hpx, mx)
        if view == self.gate_live:
            return
        self.gate_live = view
        c = self.widget_canvas
        c.delete("gatelive")
        if hpx > 0:
            c.create_rectangle(bx0 + 1, by1 - hpx, bx1 - 1, by1 - 1, fill=MARK_COLOR, outline="", tags="gatelive")
        if mx is not None:
            r = max(3, self._font_size() // 3)
            c.create_polygon(mx - r, plot[1], mx + r, plot[1], mx, plot[1] + 2 * r, fill=MARK_COLOR,
                             outline="", tags="gatelive")

    # ---------- Env tab ----------
    def _env_layout(self):
        """Two rows (FG1, FG2): label column, curve, mode buttons stacked at the right."""
        w, h = max(int(self.width), 60), max(int(self.height), 60)
        hh, fs = self._header_h(), self._font_size()
        top = hh + 4
        rh = (h - top - 4) / 2.0
        label_w = int(w * 0.24)
        btn_w = max(int(w * 0.16), 5 * fs)
        rows = []
        for i in range(2):
            y0 = top + i * rh
            y1 = y0 + rh - 6
            rows.append(((4, y0, label_w, y1), (label_w + 6, y0, w - btn_w - 10, y1), (w - btn_w - 4, y0, w - 4, y1)))
        return rows

    def _env_values(self):
        return (self._zval("fg1_rise", 5.0), self._zval("fg1_fall", 300.0), int(round(self._zval("fg1_mode", 1))),
                self._zval("fg2_rise", 300.0), self._zval("fg2_fall", 800.0), int(round(self._zval("fg2_mode", 1))),
                int(round(self._zval("m_fg2_timbre", 0))))

    def _draw_env(self):
        rows = self._env_layout()
        vals = self._env_values()
        view = (rows, vals)
        if view != self.env_view:
            now = time.monotonic()
            if self.env_view is None or now - self.env_drawn >= CURVE_PERIOD or self.drag:
                self.env_view = view
                self.env_drawn = now
                self._draw_env_static(rows, vals)
                self.env_points = [None, None]
        self._draw_env_points(vals)

    def _draw_env_static(self, rows, vals):
        c = self.widget_canvas
        c.delete("env")
        c.delete("envpt")
        self.env_buttons = []
        self.env_curves = [None, None]
        fs = self._font_size()
        small = self._font(max(7, fs - 2))
        for i in range(2):
            rise, fall, mode = vals[3 * i], vals[3 * i + 1], vals[3 * i + 2]
            lab, curve, btns = rows[i]
            # label column
            c.create_text(lab[0] + 2, lab[1] + 2, anchor="nw", text="FG%d → %s" % (i + 1, "Gate" if i == 0 else "Timbre"),
                          fill=zynthian_gui_config.color_tx, font=self._font(), tags="env")
            c.create_text(lab[0] + 2, lab[1] + fs + 10, anchor="nw",
                          text="↗ %s\n↘ %s" % (ms_text(rise), ms_text(fall)),
                          fill=LABEL_COLOR, font=small, tags="env")
            if i == 1:
                depth = vals[6]
                bh = 2 * fs + 6
                box = (lab[0] + 2, lab[3] - bh, lab[2] - 4, lab[3])
                col = ROUTE_ON if depth else ROUTE_OFF
                c.create_rectangle(*box, outline=col, tags="env")
                c.create_text((box[0] + box[2]) / 2, (box[1] + box[3]) / 2, text="Timbre %+d%%" % depth,
                              fill=(NOTE_COLOR if depth else LABEL_COLOR), font=small, tags="env")
                self.depth_box = box
            # curve
            c.create_rectangle(*curve, outline=AXIS_COLOR, tags="env")
            x0, y0, x1, y1 = curve[0] + 4, curve[1] + 6, curve[2] - 4, curve[3] - 4
            self.env_curves[i] = (x0, y0, x1, y1)
            segs = fg_shape(rise, fall, mode)
            total = sum(wd for wd, a, b in segs)
            x = x0
            pts = [x0, y1]
            for wd, a, b in segs:
                x += (x1 - x0) * wd / total
                pts += [x, y1 - (y1 - y0) * b]
            c.create_line(*pts, fill=WAVE_COLOR, width=2, tags="env")
            # mode buttons, stacked
            bx0, by0, bx1, by1 = btns
            bh = (by1 - by0 - 8) / 3.0
            for k, name in enumerate(FG_MODES):
                on = k == mode
                yy = by0 + k * (bh + 4)
                c.create_rectangle(bx0, yy, bx1, yy + bh, outline=ROUTE_ON if on else ROUTE_OFF,
                                   fill=(ROUTE_ON if on else ""), tags="env")
                c.create_text((bx0 + bx1) / 2, yy + bh / 2, text=name, font=small,
                              fill=("#000000" if on else zynthian_gui_config.color_tx), tags="env")
                self.env_buttons.append((bx0, yy, bx1, yy + bh, "fg%d_mode" % (i + 1), k))

    def _draw_env_points(self, vals):
        c = self.widget_canvas
        r = max(3, self._font_size() // 3)
        for i in range(2):
            box = self.env_curves[i]
            pos = self._mon("fg%d" % (i + 1), -1.0)
            pt = fg_point(pos, vals[3 * i], vals[3 * i + 1], vals[3 * i + 2]) if box else None
            view = (int(round(box[0] + pt[0] * (box[2] - box[0]))), int(round(box[3] - pt[1] * (box[3] - box[1])))) \
                if pt else None
            if view == self.env_points[i]:
                continue
            self.env_points[i] = view
            tag = "envpt%d" % i
            items = c.find_withtag(tag)
            if view is None:
                c.delete(tag)
                continue
            x, y = view
            if items:
                c.coords(items[0], x - r, y - r, x + r, y + r)
            else:
                c.create_oval(x - r, y - r, x + r, y + r, fill=MARK_COLOR, outline="", tags=(tag, "envpt"))

    # ---------- Matrix tab ----------
    def _mx_layout(self):
        """(gx0, gy0, gx1, gy1, cw, ch, info_y, small font, column names): the grid under the column names, the
        source names at its left, a line under it."""
        w, h = max(self.width, 80), max(self.height, 60)
        pad = 6
        fam, fs = zynthian_gui_config.font_family, self._font_size()
        small = max(6, fs - 3)
        key = (w, h, small)
        if getattr(self, "mx_metrics", (None,))[0] != key:
            try:
                f = tkinter.font.Font(family=fam, size=small)
                label_w = max(f.measure(n) for _, n, _ in MATRIX_SRCS) + 8
                line = f.metrics("linespace")
                cw = (w - pad - pad - label_w) / float(len(MATRIX_DSTS))
                names = [d[1] for d in MATRIX_DSTS]
                if max(f.measure(n) for n in names) > cw - 3:
                    names = [d[2] for d in MATRIX_DSTS]
            except Exception:
                label_w, line, names = small * 5, small * 2, [d[2] for d in MATRIX_DSTS]
            self.mx_metrics = (key, label_w, line, names)
        _, label_w, line, names = self.mx_metrics
        top = self._header_h() + 2
        gx0, gx1 = pad + label_w, w - pad
        gy0 = top + line + 2
        info_y = h - pad - line / 2.0
        gy1 = info_y - line / 2.0 - 4
        cw = (gx1 - gx0) / float(len(MATRIX_DSTS))
        ch = (gy1 - gy0) / float(len(MATRIX_SRCS))
        return gx0, gy0, gx1, gy1, cw, ch, info_y, small, names

    def _mx_cell(self, x, y):
        gx0, gy0, gx1, gy1, cw, ch = self._mx_layout()[:6]
        if not (gx0 <= x < gx1 and gy0 <= y < gy1):
            return None
        return (min(int((y - gy0) / ch), len(MATRIX_SRCS) - 1), min(int((x - gx0) / cw), len(MATRIX_DSTS) - 1))

    def _draw_matrix(self):
        c = self.widget_canvas
        gx0, gy0, gx1, gy1, cw, ch, info_y, small, names = self._mx_layout()
        fam = zynthian_gui_config.font_family
        if self.mx_items is None:                        # the static part: once per size / tab switch
            c.delete("matrix")
            self.mx_items, self.mx_vals, self.mx_labels, self.mx_info = {}, {}, [], None
            for di, name in enumerate(names):
                c.create_text(gx0 + (di + 0.5) * cw, gy0 - 2, anchor="s", text=name, font=(fam, small),
                              fill=zynthian_gui_config.color_tx_off, tags="matrix")
            for si, (_, name, _) in enumerate(MATRIX_SRCS):
                self.mx_labels.append(c.create_text(gx0 - 4, gy0 + (si + 0.5) * ch, anchor="e", text=name,
                                                    font=(fam, small), fill="#606060", tags="matrix"))
                for di in range(len(MATRIX_DSTS)):
                    x0, y0 = gx0 + di * cw + 1, gy0 + si * ch + 1
                    r = c.create_rectangle(x0, y0, x0 + cw - 2, y0 + ch - 2, fill=mix(MOD_POS, 0), width=0,
                                           tags="matrix")
                    t = c.create_text(x0 + cw / 2 - 1, y0 + ch / 2 - 1, text="", font=(fam, small), fill="#000000",
                                      tags="matrix")
                    self.mx_items[(si, di)] = (r, t)
            self.mx_line = c.create_text(6, info_y, anchor="w", text="",
                                         font=(fam, small), fill=zynthian_gui_config.color_tx_off, tags="matrix")
        used = [False] * len(MATRIX_SRCS)                # the cells: only the changed ones are touched
        for (si, di), (r, t) in self.mx_items.items():
            v = int(round(self._zval(cell_sym(si, di), 0)))
            used[si] = used[si] or v != 0
            if self.mx_vals.get((si, di)) == v:
                continue
            self.mx_vals[(si, di)] = v
            f = min(abs(v), 100) / 100.0
            c.itemconfigure(r, fill=mix(MOD_POS if v >= 0 else MOD_NEG, 0.25 + 0.75 * f if v else 0))
            c.itemconfigure(t, text=("%+d" % v) if v else "", fill=("#000000" if f > 0.5 else "#e0e0e0"))
        for si, item in enumerate(self.mx_labels):
            want = zynthian_gui_config.color_tx if used[si] else "#606060"
            if c.itemcget(item, "fill") != want:
                c.itemconfigure(item, fill=want)
        if self.mx_sel:
            si, di = self.mx_sel
            text = cell_text(si, di, self.mx_vals.get((si, di), 0))
        else:
            text = "A finger up / down on a cell: its depth; a double tap: 0"
        if text != self.mx_info:
            self.mx_info = text
            c.itemconfigure(self.mx_line, text=text)
