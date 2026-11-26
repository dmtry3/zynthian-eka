#!/usr/bin/python3
# -*- coding: utf-8 -*-
# Eka Pads widget for Zynthian: 4x4 pad grid,
# waveform inspector, Auto-Select syncing Zynthian's 4 physical encoders,
# and 8-scene 16-step sequencer with Overdub and M-VAVE SMC-PAD support.

import glob
import logging
import math
import mmap
import os
import struct
import threading
import time
import tkinter
import tkinter.font

from zyngui import zynthian_gui_config
from zyngui import zynthian_widget_base

try:
    from zynlibs.zynseq import zynseq as zynseq_mod
except Exception:
    zynseq_mod = None

MULTI_INSTANCE = True

EP_PADS = 16
EP_SCENES = 8
EP_UI_SCENES = 5
EP_STEPS = 32
EP_STEPS_MIN = 4
EP_SCENE_SIZE = EP_PADS * EP_STEPS * 5 + EP_PADS * 2   # 2592 bytes
EP_SHM_MAGIC = 0x45504144
EP_SHM_HDR_FMT = "<IIIiiiiiiiffiIiiiiiiiiiiiiiifIiiffif"
EP_SHM_HDR_SIZE = struct.calcsize(EP_SHM_HDR_FMT)      # 144 bytes
EP_PAD_FMT = "<ffffffiffffifiiiffffiiffiiiiiiiiffffffii"
EP_PAD_SIZE = struct.calcsize(EP_PAD_FMT)              # 160 bytes
EP_SHM_SIZE = EP_SHM_HDR_SIZE + EP_PADS * EP_PAD_SIZE + EP_SCENES * EP_SCENE_SIZE

AUDIO_EXTS = (".wav", ".flac", ".aiff", ".aif", ".ogg", ".mp3")
PLAY_MODE_NAMES = ("16L", "Chrm", "16Vl", "16Fl")
PLAY_MODE_LABELS = ("Norm", "Chrm", "16Vl", "16Fl")
REPEAT_DIV_NAMES = ("Rpt", "1/8", "1/16", "16T", "1/32")
REPEAT_DIV_LABELS = ("Off", "1/8", "1/16", "16T", "1/32")
METRO_NAMES = ("Off", "Rec", "On")

# Packed seq_cmd operations matching eka_pads.c
CMD_SET_VEL = 1
CMD_SET_ROLL = 2
CMD_CLEAR_PAD = 3
CMD_CLEAR_SCENE = 4
CMD_COPY_SCENE = 5
CMD_TRIG_PAD = 6
CMD_SET_START = 7
CMD_SET_END = 8
CMD_SET_QUANT = 9
CMD_SET_RETRIG = 10
CMD_SET_MINVEL = 11
CMD_SET_XTALK = 12
CMD_SET_LFO = 13
CMD_SET_MOD_SD = 14
CMD_SET_MOD_AMT = 15

MOD_SRCS = ("Off", "Vel", "Rand", "LFO1", "LFO2", "Env")
MOD_DSTS = ("None", "Pitch", "Decay", "Filter", "Level", "Start", "Drive", "Pan", "Width", "Aux1", "Aux2")
LFO_WAVES = ("Sine", "Tri", "Saw", "Sqr", "S&H")
LFO_SYNC_DIVS = ("4/1", "2/1", "1/1", "1/2", "1/4", "1/8", "1/16", "1/8T", "1/16T")

LEN_PRESETS = (4, 8, 12, 16, 24, 32)
QUANT_NAMES = ("Off", "1/16", "1/8", "1/4")

TABS = ["Play", "Pads", "Mod", "Seq", "Kit"]
TAB_GROUPS = ["g_seq", "g_sound", "g_mod", "g_seq", "g_mix"]
SCENE_NAMES = ["A", "B", "C", "D", "E", "F", "G", "H"]
NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]

# Row colors (bottom row = Kick/Snare, up to Perc/FX)
ROW_COLORS = ["#ff6b6b", "#ffa94d", "#4dabf7", "#9775fa"]
COL_BG = "#16161a"
COL_PANEL = "#222228"
COL_PAD = "#2a2a32"
COL_PAD_SEL = "#364fc7"
COL_ACCENT = "#4dabf7"
COL_REC = "#fa5252"
COL_PLAY = "#40c057"
COL_PEND = "#ffd43b"
COL_WAVE = "#4dabf7"
COL_DIM = "#666672"

UPDATE_PERIOD = 0.06  # ~16 fps on Pi 5
NPEAKS = 256


def note_short(n):
    n = int(n) & 127
    return "%s%d" % (NOTE_NAMES[n % 12], n // 12 - 1)


def note_name(n):
    n = int(n) & 127
    return "%s%d (%d)" % (NOTE_NAMES[n % 12], n // 12 - 1, n)


def short_sample_name(path, pad_idx):
    if not path or path == "_":
        return "Pad %d" % (pad_idx + 1)
    base = os.path.splitext(os.path.basename(path))[0]
    # Strip leading "01_" numeric prefix for clean pad labels
    if len(base) > 3 and base[:2].isdigit() and base[2] in ("_", "-", " "):
        base = base[3:]
    return base[:10]


def pack_cmd(op, a=0, b=0, c=0, val=0):
    return ((op & 0xF) << 20) | ((a & 0x7) << 17) | ((b & 0xF) << 13) | ((c & 0x1F) << 8) | (val & 0xFF)


def read_peaks(path, npeaks=NPEAKS):
    """Read compact min/max waveform peaks for a drum sample."""
    try:
        import numpy as np
        import soundfile
        with soundfile.SoundFile(path) as f:
            frames, ch = f.frames, f.channels
            if frames <= 0:
                return []
            data = f.read(dtype="float32", always_2d=True)
            n = min(npeaks, frames)
            bucket = max(1, frames // n)
            peaks = []
            for i in range(0, frames, bucket):
                blk = data[i:i + bucket]
                if len(blk):
                    peaks.append((float(blk.min()), float(blk.max())))
            return peaks[:npeaks]
    except Exception:
        pass
    try:
        import array
        import wave
        with wave.open(path, "rb") as w:
            ch, frames, width = w.getnchannels(), w.getnframes(), w.getsampwidth()
            if width != 2 or frames <= 0:
                return []
            raw = array.array("h", w.readframes(frames))
        n = min(npeaks, frames)
        step = max(1, (frames // n) * ch)
        peaks = []
        for i in range(0, len(raw), step):
            s = raw[i:i + step]
            if s:
                peaks.append((min(s) / 32768.0, max(s) / 32768.0))
        return peaks[:npeaks]
    except Exception:
        return []


class zynthian_widget_eka_pads(zynthian_widget_base.zynthian_widget_base):

    def __init__(self, parent):
        super().__init__(parent)
        self.tab = 0
        self.last_group = None
        self.next_update = 0.0

        self.mm = None
        self.mm_path = None
        self.shm_seq = -1
        self.shm_hdr = None
        self.last_shm_mode = 0
        self.last_shm_learn = 0
        self.last_hit_seq = 0
        self.len_dragged = False
        self.pads_data = None      # list of 16 tuples (pitch, decay, filter, level, start, end, mode, punch, drive, pan, vel_sens, choke, send, mute, solo, midi_note)
        self.scenes_data = None    # bytes of length 8*16*16*2

        self.sel_pad = 0
        self.sel_step = 0
        self.step_popup = None     # None or (scene, pad, step)
        self.step_popup_vals = None  # [vel, chance, roll, micro, pitch_off, filt_off] optimistic UI state
        self.step_drag_override = None  # None or (scene, pad, step, vel) during vertical step drag
        self._step_hold_after = None
        self.sample_popup = None   # None or pad_idx (0..15)
        self.sample_popup_tab = 0  # 0 = Sound, 1 = Tools (Slice / Link / Copy-Swap)
        self.sample_popup_vals = None  # dict of optimistic control values while sample popup open
        self.slice_mode = 1        # 0=4 Grid, 1=8 Grid, 2=16 Grid, 3=Transients
        self.slice_sens = 50       # 0..100% transient sensitivity
        self.slice_choke = True    # put sliced pads into Choke Grp 1
        self.slice_from_p1 = True  # start slicing at P1 (or selected pad)
        self.pad_tool_op = 0       # 0 = Copy, 1 = Swap
        self._wave_hold_after = None
        self.seq_tools_popup = False
        self.euclid_hits = 4
        self.euclid_rot = 0
        self.folder_popup = False
        self.folder_list = []      # [(dir_path, label, count), ...]
        self.folder_page = 0
        self._scene_hold_after = None
        self.notes_popup = False   # True when 16-pad MIDI note mapping window is open
        self.notes_reset_confirm_until = 0.0
        self.notes_drag_override = None  # None or (pad_idx, midi_note) during vertical pad drag
        self.last_synced_row_notes = None
        self.reset_confirm_until = 0.0
        self._reset_wait_seq = None
        self._reset_wait_until = 0.0
        self._cmd_nonce = 0
        self.last_synced_pad = -1
        self.last_synced_scene = -1
        self.copy_src = -1         # >=0 when waiting for user to tap destination scene

        self.peak_cache = {}       # path -> peaks list
        self.peak_loading = set()

        self.hit_boxes = []        # list of (x0, y0, x1, y1, action, arg)
        self.drag_info = None      # ("bpm"|"step", arg, start_y, start_val)
        self.last_tap = (None, 0.0)
        self.aux_targets = [None, None, None, None]  # [(target_id, label, (port_l, port_r)) or None, ...]
        self.last_aux_routes_str = None
        self.next_aux_check = 0.0
        self._own_jclient = None
        self.toast = ""
        self.toast_until = 0.0
        self.lfo_popup = None      # None, 0 (LFO 1), or 1 (LFO 2)
        # Play tab performance state
        self.xy_hold_mode = False  # False = Momentary auto-return, True = Hold/Latch
        self.perf_xy_override = None  # None or (filt, reso)
        self.perf_fx_local = 0     # 0..4 while holding Stut / Tape Stop
        self.perf_break_local = False
        self.perf_throw_local = False
        self.choke_mute_override = None
        self.choke_solo_override = None
        self._choke_hold_after = None

        self.widget_canvas = tkinter.Canvas(
            self, highlightthickness=0, relief="flat",
            bg=zynthian_gui_config.color_bg
        )
        self.widget_canvas.grid(sticky="news")
        self.widget_canvas.bind("<ButtonPress-1>", self._on_press)
        self.widget_canvas.bind("<B1-Motion>", self._on_motion)
        self.widget_canvas.bind("<ButtonRelease-1>", self._on_release)

    def update(self):
        now = time.monotonic()
        if now < self.next_update:
            return
        self.next_update = now + UPDATE_PERIOD
        super().update()

    def on_size(self, event):
        if not super().on_size(event):
            return
        self.widget_canvas.grid(row=0, column=0, sticky="news")
        self.refresh_gui()

    # ---------- SHM & Zynthian Controller Access ----------

    def _open_mm(self):
        path = None
        try:
            pid = self.processor.engine.proc.pid
            path = "/dev/shm/eka_pads_%d" % pid
        except Exception:
            files = glob.glob("/dev/shm/eka_pads_*")
            if len(files) == 1:
                path = files[0]
        if not path or not os.path.exists(path):
            return False
        if path == self.mm_path and self.mm:
            return True
        try:
            if self.mm:
                self.mm.close()
            with open(path, "rb") as f:
                self.mm = mmap.mmap(f.fileno(), EP_SHM_SIZE, access=mmap.ACCESS_READ)
            self.mm_path = path
            return True
        except Exception as e:
            logging.debug("Eka Pads widget: mmap failed => %s", e)
            return False

    def _read_shm(self):
        if not self._open_mm():
            return False
        try:
            buf = self.mm[:EP_SHM_SIZE]
            if len(buf) < EP_SHM_SIZE:
                return False
            hdr = struct.unpack_from(EP_SHM_HDR_FMT, buf, 0)
            if hdr[0] != EP_SHM_MAGIC:
                return False
            seq = hdr[2]
            if self._reset_wait_seq is not None:
                if seq != self._reset_wait_seq or time.monotonic() >= self._reset_wait_until:
                    self._reset_wait_seq = None
                else:
                    return True
            self.shm_hdr = hdr
            if seq != self.shm_seq or self.pads_data is None:
                self.shm_seq = seq
                off = EP_SHM_HDR_SIZE
                pads = []
                for _ in range(EP_PADS):
                    pads.append(struct.unpack_from(EP_PAD_FMT, buf, off))
                    off += EP_PAD_SIZE
                self.pads_data = pads
                self.scenes_data = buf[off:off + EP_SCENES * EP_SCENE_SIZE]
            return True
        except Exception as e:
            logging.debug("Eka Pads widget: shm read error => %s", e)
            return False

    def _step_get_raw(self, scene, pad, step):
        if not self.scenes_data:
            return 0, 0, 100, 0, 0
        idx = scene * EP_SCENE_SIZE + (pad * EP_STEPS + step) * 5
        if idx + 4 >= len(self.scenes_data):
            return 0, 0, 100, 0, 0
        vel, rb, ch, po, fo = struct.unpack_from("<BBBbb", self.scenes_data, idx)
        if ch == 0 or ch > 100:
            ch = 100
        return vel, rb, ch, po, fo

    def _step_get(self, scene, pad, step):
        vel, rb, ch, _po, _fo = self._step_get_raw(scene, pad, step)
        return vel, rb & 3, ch

    def _step_get_plock(self, scene, pad, step):
        vel, rb, ch, po, fo = self._step_get_raw(scene, pad, step)
        m6 = (rb >> 2) & 0x3F
        if m6 >= 32:
            m6 -= 64
        return vel, rb & 3, ch, m6, po, fo

    def _pad_range_get(self, scene, pad):
        if not self.scenes_data:
            return 0, 16
        base = scene * EP_SCENE_SIZE + EP_PADS * EP_STEPS * 5
        if base + EP_PADS + pad >= len(self.scenes_data):
            return 0, 16
        ps = self.scenes_data[base + pad]
        pe = self.scenes_data[base + EP_PADS + pad]
        if pe == 0:
            pe = 16
        if pe <= ps:
            pe = min(EP_STEPS, ps + 1)
        return ps, pe

    def _zval(self, sym, default=0.0):
        try:
            return float(self.processor.controllers_dict[sym].value)
        except Exception:
            return default

    def _zset(self, sym, val):
        try:
            z = self.processor.controllers_dict[sym]
            if z.value != val:
                z.set_value(val)
            return True
        except Exception:
            return False

    def _mon(self, key, default=0.0):
        try:
            return float((self.monitors or {}).get(key, default))
        except Exception:
            return default

    def _pad_path(self, pad_idx):
        try:
            z = self.processor.controllers_dict["sample%d" % (pad_idx + 1)]
            p = str(z.value) if z and z.value else ""
            return "" if p == "_" else p
        except Exception:
            return ""

    def _open_file_selector(self, pad_idx):
        try:
            z = self.processor.controllers_dict["sample%d" % (pad_idx + 1)]
            z.nudge(0)
        except Exception as e:
            logging.warning("Eka Pads widget: file selector failed => %s", e)

    def _get_pad_note(self, pad_idx):
        if self.notes_drag_override and self.notes_drag_override[0] == pad_idx:
            return int(self.notes_drag_override[1]) & 127
        if self.pads_data and 0 <= pad_idx < len(self.pads_data):
            return int(self.pads_data[pad_idx][15]) & 127
        return 36 + (pad_idx & 15)

    def _sync_note_encoders(self, row):
        if not self.pads_data:
            return
        row = int(row) & 3
        notes = tuple(int(self.pads_data[row * 4 + col][15]) & 127 for col in range(4))
        for col in range(4):
            self._zset("pad_note%d" % (col + 1), notes[col])
        self.last_synced_row_notes = notes

    def _close_notes_popup(self, restore_group=True):
        if not self.notes_popup:
            return
        self.notes_popup = False
        self.notes_reset_confirm_until = 0.0
        self.notes_drag_override = None
        self._zset("learn", 0)
        if restore_group:
            self._select_page(self.tab)

    def _sync_encoders_to_pad(self, pad_idx):
        """When Auto-Select or tap switches pad, update Zynthian's 12 pad zctrls to that pad's values."""
        if not self.pads_data or pad_idx < 0 or pad_idx >= EP_PADS:
            return
        p = self.pads_data[pad_idx]
        mapping = (
            ("pad_pitch",  p[0]),
            ("pad_decay",  p[1]),
            ("pad_filter", p[2]),
            ("pad_level",  p[3]),
            ("pad_start",  p[4]),
            ("pad_end",    int(round(p[5]))),
            ("pad_mode",   p[6]),
            ("pad_punch",  p[7]),
            ("pad_drive",  p[8]),
            ("pad_pan",    p[9]),
            ("pad_vel",    p[10]),
            ("pad_choke",  p[11]),
            ("pad_width",  p[12]),
            ("pad_aux1",      p[16] if len(p) > 16 else 0.0),
            ("pad_aux2",      p[17] if len(p) > 17 else 0.0),
            ("pad_aux3",      p[18] if len(p) > 18 else 0.0),
            ("pad_aux4",      p[19] if len(p) > 19 else 0.0),
            ("pad_lfo1_rate", p[22] if len(p) > 22 else 1.5),
            ("pad_lfo2_rate", p[23] if len(p) > 23 else 4.0),
            ("pad_mod1_amt",  p[32] if len(p) > 32 else 0.0),
            ("pad_mod2_amt",  p[33] if len(p) > 33 else 0.0),
        )
        for sym, val in mapping:
            self._zset(sym, val)
        self._sync_note_encoders(pad_idx // 4)
        scene = int(round(self._mon("out_seq_scene", 0)))
        ps, pe = self._pad_range_get(scene, pad_idx)
        self._zset("pad_steps", pe)
        self._zset("pad_beg", ps + 1)

    # ---------- JACK Aux Send -> Zynthian Mixbus Routing ----------

    def _get_zyngui(self):
        zg = getattr(self, "zyngui", None)
        if zg is not None:
            return zg
        return getattr(zynthian_gui_config, "zyngui", None)

    def _get_jclient(self):
        # 1. Try Zynthian's existing JACK client in zynautoconnect
        for mod_name in ("zynautoconnect.zynthian_autoconnect", "zynautoconnect", "zyngine.zynautoconnect"):
            try:
                mod = __import__(mod_name, fromlist=["jclient"])
                jc = getattr(mod, "jclient", None)
                if jc is not None:
                    return jc
            except Exception:
                pass
        # 2. Try zyngui / state_manager
        try:
            zg = self._get_zyngui()
            if zg is not None:
                jc = getattr(zg, "jclient", None) or getattr(getattr(zg, "state_manager", None), "jclient", None)
                if jc is not None:
                    return jc
        except Exception:
            pass
        # 3. Fallback: open a lightweight non-audio JACK client directly
        if self._own_jclient is None:
            try:
                import jack
                self._own_jclient = jack.Client("eka_pads_aux", no_start_server=True)
            except Exception:
                self._own_jclient = None
        return self._own_jclient

    def _get_aux_out_ports(self, jclient, aux_idx):
        if not jclient:
            return None
        suf_l = ":aux%d_l" % (aux_idx + 1)
        suf_r = ":aux%d_r" % (aux_idx + 1)
        jn = None
        try:
            jn = self.processor.get_jackname()
        except Exception:
            try:
                jn = self.processor.engine.jackname
            except Exception:
                jn = None
        try:
            all_outs = jclient.get_ports(is_audio=True, is_output=True)
        except Exception:
            return None
        pl = pr = None
        for p in all_outs:
            nm = p.name
            if jn and not nm.startswith(jn + ":"):
                continue
            if nm.endswith(suf_l):
                pl = p
            elif nm.endswith(suf_r):
                pr = p
        if pl and pr:
            return pl, pr
        # Retry without jackname filter if jackname differed
        pl = pr = None
        for p in all_outs:
            nm = p.name
            if nm.endswith(suf_l):
                pl = p
            elif nm.endswith(suf_r):
                pr = p
        if pl and pr:
            return pl, pr
        return None

    def _find_ports_by_prefix(self, all_ins, prefix):
        pref_l = prefix.lower()
        found = [p for p in all_ins if p.name.lower().startswith(pref_l)]
        if len(found) >= 2:
            return found[0], found[1]
        if len(found) == 1:
            return found[0], found[0]
        return None

    def _resolve_chain_audio_in(self, all_ins, chain):
        # 1. Try first audio FX processor in chain (so Aux sends hit the Mixbus FX!)
        try:
            slots = getattr(chain, "audio_slots", None) or []
            for slot in slots:
                for proc in (slot if isinstance(slot, (list, tuple)) else [slot]):
                    jn = None
                    if callable(getattr(proc, "get_jackname", None)):
                        jn = proc.get_jackname()
                    elif getattr(proc, "engine", None) is not None:
                        jn = getattr(proc.engine, "jackname", None)
                    if jn:
                        pair = self._find_ports_by_prefix(all_ins, jn + ":")
                        if pair:
                            return pair
        except Exception:
            pass
        # 2. Try chain.get_audio_in() / chain.audio_in
        try:
            ain = chain.get_audio_in() if callable(getattr(chain, "get_audio_in", None)) else getattr(chain, "audio_in", None)
            if isinstance(ain, str):
                ain = [ain]
            if ain:
                for pat in ain:
                    if not pat:
                        continue
                    pair = self._find_ports_by_prefix(all_ins, str(pat))
                    if pair:
                        return pair
        except Exception:
            pass
        # 3. Try zynmixer strip input (zynmixer:input_01a / input_01b, or return_...)
        try:
            mc = getattr(chain, "mixer_chan", None)
            if mc is not None and int(mc) >= 0:
                idx = int(mc) + 1
                for fmt in ("zynmixer:input_%02d", "ZynMixer:input_%02d", "zynmixer_bus:input_%02d"):
                    pair = self._find_ports_by_prefix(all_ins, fmt % idx)
                    if pair:
                        return pair
        except Exception:
            pass
        return None

    def _list_mixbus_targets(self):
        targets = [("", "Off", None)]
        jclient = self._get_jclient()
        if not jclient:
            return targets
        try:
            all_ins = list(jclient.get_ports(is_audio=True, is_input=True, is_physical=False))
        except Exception:
            try:
                all_ins = list(jclient.get_ports(is_audio=True, is_input=True))
            except Exception:
                return targets

        seen_ports = set()
        own_cid = getattr(self.processor, "chain_id", None)
        own_jn = None
        try:
            own_jn = self.processor.get_jackname()
        except Exception:
            pass

        # 1. Discover Zynthian chains (Mixbuses & Audio FX chains) via chain_manager
        try:
            zg = self._get_zyngui()
            cm = getattr(zg, "chain_manager", None) if zg else None
            chains = getattr(cm, "chains", None) if cm else None
            if chains:
                for cid, chain in chains.items():
                    if own_cid is not None and cid == own_cid:
                        continue
                    is_aud = False
                    try:
                        is_aud = bool(chain.is_audio())
                    except Exception:
                        pass
                    if not is_aud:
                        is_aud = bool(getattr(chain, "audio_slots", None) or
                                      getattr(chain, "audio_in", None) or
                                      getattr(chain, "mixer_chan", None) is not None or
                                      cid == 0)
                    if not is_aud:
                        continue
                    pair = self._resolve_chain_audio_in(all_ins, chain)
                    if not pair:
                        continue
                    key = (pair[0].name, pair[1].name)
                    if key in seen_ports:
                        continue
                    seen_ports.add(key)
                    try:
                        title = chain.get_title() or ("Main" if cid == 0 else ("Bus %s" % cid))
                    except Exception:
                        title = "Main" if cid == 0 else ("Bus %s" % cid)
                    targets.append(("chain:%s" % cid, title[:18], key))
        except Exception as e:
            logging.debug("Eka Pads widget: chain_manager scan error => %s", e)

        # 2. Direct JACK fallback: add any zynmixer / FX stereo input pairs not yet listed
        by_group = {}
        for p in all_ins:
            nm = p.name
            if own_jn and nm.startswith(own_jn + ":"):
                continue
            if ":" not in nm:
                continue
            cli, port = nm.split(":", 1)
            cli_l = cli.lower()
            if cli_l in ("system", "mod-monitor", "zynthian_gui", "jack_capture"):
                continue
            # Group zynmixer:input_01a & input_01b under "zynmixer:input_01"
            if cli_l.startswith("zynmixer") and len(port) > 1 and port[-1] in ("a", "b", "L", "R"):
                grp = "%s:%s" % (cli, port[:-1])
                label = port[:-1].replace("input_", "Mix ").replace("return_", "Ret ")
            else:
                grp = cli
                label = cli
            by_group.setdefault(grp, [label, []])[1].append(p)

        for grp, (label, plist) in by_group.items():
            if len(plist) >= 2:
                key = (plist[0].name, plist[1].name)
            elif len(plist) == 1:
                key = (plist[0].name, plist[0].name)
            else:
                continue
            if key in seen_ports:
                continue
            seen_ports.add(key)
            targets.append(("jack:%s" % grp, label[:18], key))

        return targets

    def _apply_aux_route(self, aux_idx, target, persist=True):
        self.aux_targets[aux_idx] = target if (target and target[2]) else None
        if persist:
            parts = [t[0].replace(" ", "%20").replace("=", "") if t else "" for t in self.aux_targets]
            rstr = ";".join(parts) if any(parts) else "_"
            self.last_aux_routes_str = rstr
            self._zset("aux_routes", rstr)
        jclient = self._get_jclient()
        if not jclient:
            return
        outs = self._get_aux_out_ports(jclient, aux_idx)
        if not outs:
            logging.warning("Eka Pads widget: aux%d output ports not found in JACK", aux_idx + 1)
            return
        for p in outs:
            try:
                for dst in jclient.get_all_connections(p):
                    jclient.disconnect(p, dst)
            except Exception:
                pass
        if target and target[2]:
            dst_l, dst_r = target[2]
            try:
                jclient.connect(outs[0], dst_l)
                jclient.connect(outs[1], dst_r)
                logging.info("Eka Pads widget: Aux %d -> %s (%s, %s)", aux_idx + 1, target[1], dst_l, dst_r)
            except Exception as e:
                logging.warning("Eka Pads widget: aux%d connect failed => %s", aux_idx + 1, e)

    def _cycle_aux_target(self, aux_idx):
        targets = self._list_mixbus_targets()
        cur = self.aux_targets[aux_idx]
        cur_id = cur[0] if cur else ""
        idx = 0
        for i, t in enumerate(targets):
            if t[0] == cur_id:
                idx = i
                break
        nxt = targets[(idx + 1) % len(targets)]
        self._apply_aux_route(aux_idx, nxt)

    def _sync_saved_aux_routes(self):
        try:
            z = self.processor.controllers_dict.get("aux_routes")
            val = str(z.value) if (z and z.value is not None) else ""
        except Exception:
            return
        if val == self.last_aux_routes_str:
            return
        if not val or val == "_":
            self.last_aux_routes_str = val
            return
        parts = val.split(";")
        targets = self._list_mixbus_targets()
        by_id = {t[0]: t for t in targets}
        all_matched = True
        for ai in range(4):
            tid = parts[ai].replace("%20", " ") if ai < len(parts) else ""
            if tid:
                if tid in by_id:
                    self._apply_aux_route(ai, by_id[tid], persist=False)
                else:
                    all_matched = False
        # Only mark as synced once all saved Mixbus targets exist in JACK (retry if chains still loading)
        if all_matched:
            self.last_aux_routes_str = val

    def _ensure_aux_routes(self):
        self._sync_saved_aux_routes()
        now = time.monotonic()
        if now < self.next_aux_check:
            return
        self.next_aux_check = now + 2.0
        if not any(self.aux_targets):
            return
        jclient = self._get_jclient()
        if not jclient:
            return
        for aux_idx, target in enumerate(self.aux_targets):
            if not target or not target[2]:
                continue
            outs = self._get_aux_out_ports(jclient, aux_idx)
            if not outs:
                continue
            dst_l, dst_r = target[2]
            try:
                conns_l = [c.name for c in jclient.get_all_connections(outs[0])]
                if dst_l not in conns_l:
                    jclient.connect(outs[0], dst_l)
                    jclient.connect(outs[1], dst_r)
            except Exception:
                pass

    # ---------- Export Active Scene -> ZynSeq (polyrhythm LCM-aligned) ----------

    def _toast(self, text, secs=3.5):
        self.toast = text
        self.toast_until = time.monotonic() + secs

    @staticmethod
    def _is_zynseq_playing(state):
        names = []
        if zynseq_mod:
            for n in ("SEQ_PLAYING", "SEQ_STARTING", "SEQ_STOPPING", "SEQ_RESTARTING"):
                v = getattr(zynseq_mod, n, None)
                if v is not None:
                    names.append(v)
        if names:
            return state in names
        stopped = getattr(zynseq_mod, "SEQ_STOPPED", 0) if zynseq_mod else 0
        return state != stopped

    @staticmethod
    def _lcm(a, b):
        if a <= 0 or b <= 0:
            return max(1, a, b)
        return (a // math.gcd(a, b)) * b

    def _build_export_events(self, scene, max_steps=256):
        """Collect all audible active pads in `scene`, compute polyrhythmic LCM length
        (aligned to whole beats = multiples of 4 steps, capped at `max_steps`),
        and unroll all hits including swing, micro-timing and ratchet rolls."""
        if not self.scenes_data:
            return 0, [], False
        swing = float(self.shm_hdr[11]) if (self.shm_hdr and len(self.shm_hdr) > 11) else 50.0
        swing = max(50.0, min(75.0, swing))
        swing_shift = (swing - 50.0) / 50.0

        any_solo = bool(self.pads_data and any(p[14] for p in self.pads_data))
        active_pads = []
        for p in range(EP_PADS):
            pinfo = self.pads_data[p] if self.pads_data else None
            mute = pinfo[13] if pinfo else 0
            solo = pinfo[14] if pinfo else 0
            if any_solo:
                if not solo:
                    continue
            elif mute:
                continue
            ps, pe = self._pad_range_get(scene, p)
            plen = max(1, pe - ps)
            has_hit = any(self._step_get_raw(scene, p, st)[0] > 0 for st in range(ps, pe))
            if not has_hit:
                continue
            midi_note = int(pinfo[15]) if pinfo else (36 + p)
            active_pads.append((p, midi_note & 127, ps, plen))

        if not active_pads:
            return 0, [], False

        total = 4  # ZynSeq works in integer beats (4 steps per beat)
        capped = False
        for (_p, _mn, _ps, plen) in active_pads:
            nxt = self._lcm(total, plen)
            if nxt > max_steps:
                capped = True
                total = max_steps
                break
            total = nxt

        events = []  # list of (pos_in_steps, midi_note, vel, dur_steps)
        for (p, midi_note, ps, plen) in active_pads:
            for g in range(total):
                rel = g % plen
                vel, rb, _ch, _po, _fo = self._step_get_raw(scene, p, ps + rel)
                if vel <= 0:
                    continue
                roll = rb & 3
                m6 = (rb >> 2) & 0x3F
                if m6 >= 32:
                    m6 -= 64
                micro = m6 / 64.0
                sw = swing_shift if (rel & 1) else 0.0
                base_pos = (g + sw + micro) % float(total)
                if roll > 0:
                    sub = roll + 1
                    dur = max(0.15, 0.8 / float(sub))
                    for k in range(sub):
                        pos = (base_pos + k / float(sub)) % float(total)
                        events.append((pos, midi_note, max(1, min(127, vel)), dur))
                else:
                    events.append((base_pos, midi_note, max(1, min(127, vel)), 0.5))

        return total, events, capped

    def _export_to_zynseq(self, scene):
        self._read_shm()
        total_steps, events, capped = self._build_export_events(scene)
        if total_steps <= 0 or not events:
            self._toast("Export: Scene %s is empty" % SCENE_NAMES[scene & 7])
            return
        try:
            zg = self._get_zyngui()
            zs = zg.state_manager.zynseq
            lib = zs.libseq
        except Exception as e:
            logging.error("Eka Pads: ZynSeq unavailable => %s", e)
            self._toast("ZynSeq unavailable")
            return

        try:
            chan = int(self.processor.midi_chan)
        except Exception:
            chan = 0

        try:
            zscene, zphrase = zs.scene, zs.phrase
            try:
                cell_pat = int(lib.getPattern(zscene, zphrase, chan, 0, 0))
            except Exception:
                cell_pat = -1
            try:
                playing = self._is_zynseq_playing(lib.getPlayState(zscene, zphrase, chan))
            except Exception:
                playing = False

            assign = False
            if playing and cell_pat >= 1:
                target = cell_pat
            else:
                target = None
                for n in range(1, 1000):
                    try:
                        empty = zs.is_pattern_empty(n)
                    except Exception:
                        empty = lib.isPatternEmpty(n)
                    if empty:
                        target = n
                        break
                if target is None:
                    raise RuntimeError("no free pattern")
                assign = not (cell_pat >= 1)

            spb = 4
            beats = max(1, total_steps // spb)
            nsteps = beats * spb
            lib.selectPattern(target)
            lib.clearPattern(target)
            lib.setStepsPerBeat(spb)
            lib.setBeatsInPattern(target, beats)

            for (pos, note, vel, dur) in events:
                step = int(pos)
                off = pos - step
                if off < 0.02:
                    off = 0.0
                elif off > 0.98:
                    step += 1
                    off = 0.0
                lib.addNote(step % nsteps, note, vel, dur, off)

            if assign:
                try:
                    lib.setChannel(zscene, zphrase, chan, 0, chan)
                except Exception:
                    pass
                lib.addPattern(zscene, zphrase, chan, 0, 0, target)

            bars_f = beats / 4.0
            bars_s = ("%d bar%s" % (int(bars_f), "s" if int(bars_f) != 1 else "")) if (beats % 4 == 0) else ("%d beats" % beats)
            cap_s = " (max)" if capped else ""
            msg = "ZynSeq Pat %d: %s (%d st)%s" % (target, bars_s, nsteps, cap_s)
            logging.info("Eka Pads: exported Sc %s -> %s", SCENE_NAMES[scene & 7], msg)
            self._toast(msg)
        except Exception as e:
            logging.error("Eka Pads: ZynSeq export failed => %s", e)
            self._toast("ZynSeq export failed")

    def _send_cmd(self, op, a=0, b=0, c=0, val=0):
        try:
            z = self.processor.controllers_dict["seq_cmd"]
            if op == CMD_TRIG_PAD:
                # Nonce in unused `c` bits ensures repeated hits on the same pad always trigger
                self._cmd_nonce = (self._cmd_nonce + 1) & 0x1F
                c = self._cmd_nonce
            packed = pack_cmd(op, a, b, c, val)
            if z.value == packed:
                z.set_value(0)
            z.set_value(packed)
            # Reset one-shot / destructive commands to 0 so Zynthian snapshots never replay them on restore
            if op in (CMD_TRIG_PAD, CMD_CLEAR_PAD, CMD_CLEAR_SCENE, CMD_COPY_SCENE) or (op == CMD_SET_MOD_AMT and a >= 4):
                z.set_value(0)
        except Exception:
            pass

    @staticmethod
    def _is_audio_file(fn):
        return fn.lower().endswith(AUDIO_EXTS)

    def _sample_step(self, pad_idx, delta):
        """Step to previous/next audio file in the pad's current folder (or factory/user kit dir)."""
        cur = self._pad_path(pad_idx)
        folder = os.path.dirname(cur) if cur else ""
        if not folder or not os.path.isdir(folder):
            for cand in (
                "/zynthian/zynthian-my-data/files/Samples/eka-pads-user",
                "/zynthian/zynthian-my-data/files/Samples/eka-pads",
                os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "samples"),
            ):
                if os.path.isdir(cand):
                    folder = cand
                    break
        if not folder or not os.path.isdir(folder):
            self._toast("No sample folder found", secs=2.0)
            return
        try:
            files = sorted(f for f in os.listdir(folder) if self._is_audio_file(f))
        except Exception:
            files = []
        if not files:
            self._toast("Folder has no audio files", secs=2.0)
            return
        cur_base = os.path.basename(cur) if cur else ""
        try:
            idx = files.index(cur_base)
            nxt = files[(idx + delta) % len(files)]
        except ValueError:
            nxt = files[0] if delta >= 0 else files[-1]
        full = os.path.join(folder, nxt)
        self._zset("sample%d" % (pad_idx + 1), full)
        self._ensure_peaks(full)
        self._send_cmd(CMD_TRIG_PAD, b=pad_idx, val=110)
        self._toast("P%d: %s" % (pad_idx + 1, short_sample_name(full, pad_idx)), secs=1.8)

    def _scan_kit_folders(self):
        """Find directories containing audio samples for 1-tap Folder Kit loading."""
        roots = [
            "/zynthian/zynthian-my-data/files/Samples/eka-pads-user",
            "/zynthian/zynthian-my-data/files/Samples/eka-pads",
            "/zynthian/zynthian-my-data/files/Samples",
            os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "samples"),
        ]
        cur = self._pad_path(self.sel_pad)
        if cur:
            pdir = os.path.dirname(cur)
            if pdir and pdir not in roots:
                roots.insert(0, pdir)
        found = []
        seen = set()
        for root in roots:
            if not root or not os.path.isdir(root):
                continue
            rreal = os.path.realpath(root)
            for dirpath, dirnames, filenames in os.walk(root):
                depth = dirpath[len(root):].count(os.sep)
                if depth > 2:
                    dirnames[:] = []
                    continue
                dirnames[:] = sorted(d for d in dirnames if not d.startswith("."))
                rp = os.path.realpath(dirpath)
                if rp in seen:
                    continue
                aud = [f for f in filenames if self._is_audio_file(f)]
                if aud:
                    seen.add(rp)
                    label = os.path.basename(dirpath) or dirpath
                    if rp == rreal and label == "samples":
                        label = "Factory Kit"
                    found.append((dirpath, label[:22], len(aud)))
                if len(found) >= 48:
                    break
        self.folder_list = found
        self.folder_page = 0

    def _load_folder_kit(self, folder_path):
        if not folder_path or not os.path.isdir(folder_path):
            return
        try:
            files = sorted(f for f in os.listdir(folder_path) if self._is_audio_file(f))
        except Exception:
            files = []
        if not files:
            self._toast("No audio files in folder", secs=2.5)
            return
        n = min(EP_PADS, len(files))
        for i in range(EP_PADS):
            if i < n:
                full = os.path.join(folder_path, files[i])
                self._zset("sample%d" % (i + 1), full)
            else:
                self._zset("sample%d" % (i + 1), "_")
        self._toast("Loaded %d pads from %s" % (n, os.path.basename(folder_path)), secs=2.5)

    def _compute_slices(self, path, mode, sens):
        """Return list of (start_pct, end_pct) tuples in [0..100] for Auto-Slice."""
        if mode in (0, 1, 2):
            n = (4, 8, 16)[mode]
            return [(i * 100.0 / n, (i + 1) * 100.0 / n) for i in range(n)]
        # Mode 3: Transient onset detection from cached peaks
        peaks = self.peak_cache.get(path) or read_peaks(path)
        if not peaks or len(peaks) < 8:
            return [(i * 100.0 / 8.0, (i + 1) * 100.0 / 8.0) for i in range(8)]
        env = [max(abs(mn), abs(mx)) for (mn, mx) in peaks]
        mx_e = max(env) or 1.0
        env = [e / mx_e for e in env]
        # Positive energy flux (onset curve)
        n = len(env)
        flux = [0.0] * n
        for i in range(1, n):
            d = env[i] - env[i - 1]
            flux[i] = d if d > 0.0 else 0.0
        # Threshold inversely controlled by sensitivity (0..100%)
        thr = 0.42 - 0.36 * (max(0, min(100, sens)) / 100.0)
        min_gap = max(3, n // 24)
        starts = [0]
        last = 0
        for i in range(2, n - 2):
            if (i - last) >= min_gap and flux[i] >= thr and env[i] >= thr * 0.65:
                if flux[i] >= flux[i - 1] and flux[i] >= flux[i + 1]:
                    starts.append(i)
                    last = i
                    if len(starts) >= EP_PADS:
                        break
        if len(starts) < 2:
            return [(i * 100.0 / 8.0, (i + 1) * 100.0 / 8.0) for i in range(8)]
        out = []
        for idx, st in enumerate(starts):
            en = starts[idx + 1] if idx + 1 < len(starts) else n
            s_pct = (st * 100.0) / float(n)
            e_pct = (en * 100.0) / float(n)
            if e_pct > s_pct + 0.5:
                out.append((s_pct, e_pct))
        return out[:EP_PADS]

    def _apply_auto_slice(self, src_pad):
        path = self._pad_path(src_pad)
        if not path:
            self._toast("Load a sample first", secs=2.0)
            return
        slices = self._compute_slices(path, self.slice_mode, self.slice_sens)
        base_pad = 0 if self.slice_from_p1 else src_pad
        applied = 0
        for i, (sp, ep) in enumerate(slices):
            tp = base_pad + i
            if tp >= EP_PADS:
                break
            self._zset("sample%d" % (tp + 1), path)
            sv = max(0, min(238, int(round(sp * 2.4))))
            ev = max(sv + 1, min(240, int(round(ep * 2.4))))
            self._send_cmd(CMD_SET_MOD_AMT, a=5, b=tp, c=4, val=sv)
            self._send_cmd(CMD_SET_MOD_AMT, a=5, b=tp, c=5, val=ev)
            if self.slice_choke:
                self._send_cmd(CMD_SET_MOD_AMT, a=5, b=tp, c=6, val=1)
            applied += 1
        self.last_synced_pad = -1
        self._toast("Sliced %d pads (P%d..P%d)" % (applied, base_pad + 1, base_pad + applied), secs=2.5)

    def _load_factory_kit(self):
        kit_dir = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "samples")
        if not os.path.isdir(kit_dir):
            return
        wavs = sorted(f for f in os.listdir(kit_dir) if f.lower().endswith(".wav"))
        for i, fn in enumerate(wavs[:EP_PADS]):
            full = os.path.join(kit_dir, fn)
            try:
                z = self.processor.controllers_dict["sample%d" % (i + 1)]
                z.set_value(full)
            except Exception:
                pass

    @staticmethod
    def _default_pad_tuple(pad_idx):
        return (
            0.0, 100.0, 0.0, 80.0,
            0.0, 100.0, 0,
            0.0, 0.0, 0.0, 75.0,
            0, 0.0, 0, 0,
            36 + (pad_idx & 15),
            0.0, 0.0, 0.0, 0.0,
            0, 1, 1.5, 4.0,
            0, 0, 0, 0,
            0, 0, 0, 0,
            0.0, 0.0, 0.0, 0.0,
            0.0, 20.0, 0, 0,
        )

    def _reset_all_defaults(self):
        """Clear all samples, scenes, pad params, modulation, filters, and Aux routes to factory defaults."""
        self._cancel_step_hold()
        self._cancel_wave_hold()
        self.step_popup = None
        self.step_popup_vals = None
        self.step_drag_override = None
        self.sample_popup = None
        self.sample_popup_vals = None
        self.seq_tools_popup = False
        self.folder_popup = False
        self.lfo_popup = None
        self.notes_popup = False
        self.notes_reset_confirm_until = 0.0
        self.notes_drag_override = None
        self.copy_src = -1
        self.xy_hold_mode = False
        self.perf_xy_override = None
        self.perf_fx_local = 0
        self.perf_break_local = False
        self.perf_throw_local = False
        self.choke_mute_override = None
        self.choke_solo_override = None
        self._cancel_choke_hold()

        # 1. Disconnect & clear all 4 Aux routes in JACK
        for ai in range(4):
            self._apply_aux_route(ai, None, persist=False)
        self.last_aux_routes_str = "_"
        self._zset("aux_routes", "_")

        # 2. Clear sample paths & reset Zynthian control ports to factory defaults BEFORE C full reset
        for i in range(EP_PADS):
            self._zset("sample%d" % (i + 1), "_")

        defaults = (
            ("sel_pad", 0),
            ("follow", 1),
            ("learn", 0),
            ("mute_mask", 0),
            ("solo_mask", 0),
            ("pad_end", 100),
            ("seq_bpm", 120),
            ("seq_mode", 0),
            ("seq_scene", 1),
            ("seq_swing", 50.0),
            ("master_vol", 85.0),
            ("stereo_width", 100.0),
            ("pad_steps", 16),
            ("pad_beg", 1),
            ("pad_pitch", 0.0),
            ("pad_decay", 100.0),
            ("pad_filter", 0.0),
            ("pad_level", 80.0),
            ("pad_start", 0.0),
            ("pad_mode", 0),
            ("pad_punch", 0.0),
            ("pad_drive", 0.0),
            ("pad_pan", 0.0),
            ("pad_vel", 75.0),
            ("pad_choke", 0),
            ("pad_width", 0.0),
            ("pad_aux1", 0.0),
            ("pad_aux2", 0.0),
            ("pad_aux3", 0.0),
            ("pad_aux4", 0.0),
            ("pad_lfo1_rate", 1.5),
            ("pad_lfo2_rate", 4.0),
            ("pad_mod1_amt", 0.0),
            ("pad_mod2_amt", 0.0),
            ("pad_note1", 36),
            ("pad_note2", 37),
            ("pad_note3", 38),
            ("pad_note4", 39),
            ("bus_glue", 0.0),
        )
        for sym, val in defaults:
            self._zset(sym, val)

        # 3. Trigger atomic full reset in C DSP core LAST (kills voices, unloads all 16 samples,
        #    resets all 16 pad params + mod/LFOs + 8 scenes + filters + reports clean state back)
        self._send_cmd(CMD_CLEAR_SCENE, val=0xFF)

        # 4. Hold optimistic default UI state until C bumps shm seq (prevents stale SHM echo!)
        if self.shm_seq >= 0:
            self._reset_wait_seq = self.shm_seq
            self._reset_wait_until = time.monotonic() + 1.0
        self.pads_data = [self._default_pad_tuple(i) for i in range(EP_PADS)]
        sc_buf = bytearray(EP_SCENES * EP_SCENE_SIZE)
        for sc in range(EP_SCENES):
            base = sc * EP_SCENE_SIZE + EP_PADS * EP_STEPS * 5
            for p in range(EP_PADS):
                sc_buf[base + EP_PADS + p] = 16
        self.scenes_data = bytes(sc_buf)
        if self.shm_hdr and len(self.shm_hdr) >= 36:
            h = list(self.shm_hdr)
            h[3] = 0; h[4] = 1; h[5] = 0; h[6] = 0; h[7] = 0; h[8] = -1; h[9] = -1
            h[10] = 120.0; h[11] = 50.0; h[12] = 100
            h[15] = 25; h[16] = 0; h[17] = 0
            h[18] = 0; h[19] = 0; h[20] = 0; h[21] = 0; h[22] = 0
            h[23] = 0; h[24] = -1; h[25] = 0; h[26] = 4; h[27] = 0
            h[28] = 0.0; h[29] = 0x32102100
            h[30] = 0; h[31] = 0; h[32] = 0.0; h[33] = 20.0; h[34] = 0; h[35] = 0.0
            self.shm_hdr = tuple(h)
        self.last_shm_mode = 0
        self.last_shm_learn = 0
        self.last_synced_row_notes = (36, 37, 38, 39)
        self.sel_pad = 0
        self.last_synced_pad = 0
        self.last_synced_scene = 0
        self.peak_cache.clear()
        self._toast("All reset to default")

    def _ensure_peaks(self, path):
        if not path or path in self.peak_cache or path in self.peak_loading:
            return
        self.peak_loading.add(path)

        def worker():
            pk = read_peaks(path)
            self.peak_cache[path] = pk
            self.peak_loading.discard(path)

        threading.Thread(target=worker, daemon=True, name="eka_pads_peaks").start()

    # ---------- Tab <-> Knob Page Sync ----------

    def _select_group(self, target):
        try:
            for i, row in enumerate(self.zyngui_control.list_data):
                if len(row) >= 5 and row[0] == target and row[3] is self.processor:
                    self.zyngui_control.select(i)
                    return
        except Exception:
            pass

    def _select_page(self, tab):
        self._select_group(TAB_GROUPS[tab])

    def _follow_page(self):
        try:
            info = self.zyngui_control.screen_info
            group = info[0] if (info and len(info) >= 5 and info[3] is self.processor) else None
        except Exception:
            group = None
        if group == self.last_group:
            return
        self.last_group = group
        if group == "g_mod" and self.tab != 2:
            self.tab = 2
        elif group == "g_seq" and self.tab in (1, 2):
            self.tab = 3
        elif group in ("g_sound", "g_shape", "g_mix") and self.tab in (2, 3):
            self.tab = 1

    # ---------- Drawing ----------

    def _font_size(self):
        return max(8, min(max(self.height, 40) // 18, 12))

    def refresh_gui(self):
        if not self.processor:
            return
        self._follow_page()
        self._read_shm()
        self._ensure_aux_routes()

        # Sync seq_mode & learn zctrls whenever plugin updates them (transport / MIDI Learn completion)
        if self.shm_hdr:
            cur_mode = self.shm_hdr[6]
            if cur_mode != self.last_shm_mode:
                self._zset("seq_mode", cur_mode)
                self.last_shm_mode = cur_mode

            cur_learn = self.shm_hdr[5]
            if cur_learn != self.last_shm_learn:
                prev_learn = self.last_shm_learn
                self._zset("learn", cur_learn)
                self.last_shm_learn = cur_learn
                if prev_learn == 1 and cur_learn == 0 and self.notes_popup and self.pads_data:
                    lp = self.shm_hdr[3]
                    if 0 <= lp < EP_PADS:
                        mn = int(self.pads_data[lp][15]) & 127
                        self._toast("Pad %d -> %s" % (lp + 1, note_name(mn)), secs=2.0)

            # In Play / Pads / Mod / Seq tabs, hitting any physical MIDI pad switches to that pad for editing / 16L
            hit_seq = self.shm_hdr[13]
            last_hit = self.shm_hdr[14]
            if hit_seq != self.last_hit_seq:
                self.last_hit_seq = hit_seq
                if self.tab in (0, 1, 2, 3) and 0 <= last_hit < EP_PADS and last_hit != self.sel_pad:
                    self.sel_pad = last_hit
                    self._zset("sel_pad", last_hit)
                    self.last_synced_pad = -1

        # Track Auto-Select pad & scene from shared memory / plugin monitor
        shm_pad = self.shm_hdr[3] if self.shm_hdr else int(round(self._mon("out_sel_pad", self.sel_pad)))
        if 0 <= shm_pad < EP_PADS:
            self.sel_pad = shm_pad
        cur_scene = 0 if self._reset_wait_seq is not None else int(round(self._mon("out_seq_scene", 0)))
        if self._reset_wait_seq is None and (self.sel_pad != self.last_synced_pad or cur_scene != self.last_synced_scene) and self.pads_data is not None:
            self.last_synced_pad = self.sel_pad
            self.last_synced_scene = cur_scene
            self._sync_encoders_to_pad(self.sel_pad)
        elif self._reset_wait_seq is None and self.pads_data is not None:
            row = (self.sel_pad >> 2) & 3
            row_notes = tuple(int(self.pads_data[row * 4 + col][15]) & 127 for col in range(4))
            if row_notes != self.last_synced_row_notes:
                self._sync_note_encoders(row)

        w, h = max(int(self.width), 160), max(int(self.height), 120)
        c = self.widget_canvas
        c.delete("all")
        self.hit_boxes = []

        fam = zynthian_gui_config.font_family
        fs = self._font_size()
        top_h = max(36, fs * 3 + 8)

        self._draw_topbar(c, w, top_h, fam, fs)

        if self.tab == 0:
            self._draw_play_tab(c, w, h, top_h, fam, fs)
        elif self.tab == 1:
            self._draw_pads_tab(c, w, h, top_h, fam, fs)
        elif self.tab == 2:
            self._draw_mod_tab(c, w, h, top_h, fam, fs)
        elif self.tab == 3:
            self._draw_seq_tab(c, w, h, top_h, fam, fs)
        else:
            self._draw_kit_tab(c, w, h, top_h, fam, fs)

        if self.toast and time.monotonic() < self.toast_until:
            tw = min(w - 16, max(180, len(self.toast) * (fs - 1) + 24))
            tx0 = (w - tw) / 2
            ty0 = h - max(28, fs * 2 + 10)
            c.create_rectangle(tx0, ty0, tx0 + tw, h - 4, fill="#1b3a2b", outline="#40c057", width=2)
            c.create_text(w / 2, (ty0 + h - 4) / 2, text=self.toast, fill="#e6fcf5", font=(fam, fs, "bold"))

    def _add_btn(self, c, x0, y0, x1, y1, text, fill, fg, fam, fs, action, arg=None, outline="#444450"):
        c.create_rectangle(x0, y0, x1, y1, fill=fill, outline=outline)
        c.create_text((x0 + x1) / 2, (y0 + y1) / 2, text=text, fill=fg, font=(fam, fs))
        self.hit_boxes.append((x0, y0, x1, y1, action, arg))

    def _draw_topbar(self, c, w, top_h, fam, fs):
        c.create_rectangle(0, 0, w, top_h, fill=COL_BG, outline="")
        x = 4
        bw = max(48, int(w * 0.094))
        tab_fs = fs + 1
        for i, name in enumerate(TABS):
            sel = (i == self.tab)
            tab_col = "#40c057" if (i == 0 and sel) else (COL_ACCENT if sel else COL_PANEL)
            out_col = "#69db7c" if (i == 0 and sel) else (COL_ACCENT if sel else "#444452")
            c.create_rectangle(x, 2, x + bw, top_h - 3,
                               fill=tab_col, outline=out_col, width=2 if sel else 1)
            c.create_text(x + bw / 2, (top_h - 1) / 2, text=name,
                          fill="#000000" if sel else "#f0f0f5", font=(fam, tab_fs, "bold"))
            self.hit_boxes.append((x, 2, x + bw, top_h - 3, "tab", i))
            x += bw + 4

        # Compact performance pills on non-Play tabs: 16 Levels/Chromatic + Note Repeat
        play_mode = int(self.shm_hdr[18]) if (self.shm_hdr and len(self.shm_hdr) >= 30) else 0
        rpt_div = int(self.shm_hdr[19]) if (self.shm_hdr and len(self.shm_hdr) >= 30) else 0
        fsmall = max(7, fs - 1)
        if self.tab != 0:
            pw_p = max(40, int(w * 0.068))
            x += 2
            pm_on = (play_mode > 0)
            self._add_btn(c, x, 3, x + pw_p, top_h - 4,
                          PLAY_MODE_NAMES[play_mode % len(PLAY_MODE_NAMES)],
                          "#5f3dc4" if pm_on else COL_PANEL,
                          "#ffffff" if pm_on else "#a0a4b8", fam, fsmall,
                          "cycle_play_mode", (play_mode + 1) % len(PLAY_MODE_NAMES),
                          outline="#b197fc" if pm_on else "#3e3e4c")
            x += pw_p + 4
            rp_on = (rpt_div > 0)
            self._add_btn(c, x, 3, x + pw_p, top_h - 4,
                          REPEAT_DIV_NAMES[rpt_div % len(REPEAT_DIV_NAMES)],
                          "#d9480f" if rp_on else COL_PANEL,
                          "#ffffff" if rp_on else "#a0a4b8", fam, fsmall,
                          "cycle_repeat_div", (rpt_div + 1) % len(REPEAT_DIV_NAMES),
                          outline="#ffa94d" if rp_on else "#3e3e4c")

        # Status pills on top right: Scene / Fill / Chain / Count-in + BPM + Seq state
        scene = int(round(self._mon("out_seq_scene", 0)))
        pend = int(round(self._mon("out_seq_pend", -1)))
        bpm = int(round(self.shm_hdr[10])) if self.shm_hdr else int(round(self._zval("seq_bpm", 120)))
        smode = self.shm_hdr[6] if self.shm_hdr else int(round(self._zval("seq_mode", 0)))
        cnt_rem = int(self.shm_hdr[22]) if (self.shm_hdr and len(self.shm_hdr) >= 30) else 0
        fill_ret = int(self.shm_hdr[24]) if (self.shm_hdr and len(self.shm_hdr) >= 30) else -1
        chain_on = int(self.shm_hdr[25]) if (self.shm_hdr and len(self.shm_hdr) >= 30) else 0
        if cnt_rem > 0:
            beat_left = ((cnt_rem - 1) // 4) + 1
            info = "COUNT %d  %d BPM" % (beat_left, bpm)
            st_col = COL_PEND
        else:
            st_txt = "REC" if smode == 2 else ("PLAY" if smode == 1 else "STOP")
            st_col = COL_REC if smode == 2 else (COL_PLAY if smode == 1 else COL_DIM)
            if fill_ret >= 0:
                sc_txt = "Fill %s->%s" % (SCENE_NAMES[scene & 7], SCENE_NAMES[fill_ret & 7])
            else:
                pref = "Ch " if chain_on else "Sc "
                sc_txt = "%s%s" % (pref, SCENE_NAMES[scene & 7])
                if 0 <= pend < EP_SCENES:
                    sc_txt += "->%s" % SCENE_NAMES[pend]
            info = "%s %d [%s]" % (sc_txt, bpm, st_txt)
        c.create_text(w - 6, top_h / 2, anchor="e", text=info, fill=st_col, font=(fam, max(8, fs - 1)))

    def _get_choke_masks(self):
        now = time.monotonic()
        shm_m = int(self.shm_hdr[30]) & 0xF if (self.shm_hdr and len(self.shm_hdr) >= 36) else 0
        shm_s = int(self.shm_hdr[31]) & 0xF if (self.shm_hdr and len(self.shm_hdr) >= 36) else 0
        if self.choke_mute_override is not None:
            mv, until = self.choke_mute_override
            if now < until and shm_m != mv:
                shm_m = mv
            else:
                self.choke_mute_override = None
        if self.choke_solo_override is not None:
            sv, until = self.choke_solo_override
            if now < until and shm_s != sv:
                shm_s = sv
            else:
                self.choke_solo_override = None
        return shm_m, shm_s

    def _set_choke_masks(self, mute_mask=None, solo_mask=None):
        now = time.monotonic()
        if mute_mask is not None:
            m = int(mute_mask) & 0xF
            self.choke_mute_override = (m, now + 0.30)
            self._send_cmd(CMD_SET_MOD_AMT, a=6, c=11, val=m)
        if solo_mask is not None:
            s = int(solo_mask) & 0xF
            self.choke_solo_override = (s, now + 0.30)
            self._send_cmd(CMD_SET_MOD_AMT, a=6, c=12, val=s)

    def _get_perf_xy(self):
        now = time.monotonic()
        pf = float(self.shm_hdr[32]) if (self.shm_hdr and len(self.shm_hdr) >= 36) else 0.0
        pr = float(self.shm_hdr[33]) if (self.shm_hdr and len(self.shm_hdr) >= 36) else 20.0
        if self.perf_xy_override is not None:
            ov_f, ov_r, until = self.perf_xy_override
            if until is None or now < until:
                return ov_f, ov_r
            self.perf_xy_override = None
        return pf, pr

    def _send_perf_xy(self, filt, reso, persistent=False):
        fv = max(-100, min(100, int(round(filt))))
        rv = max(0, min(100, int(round(reso))))
        until = None if persistent else (time.monotonic() + 0.30)
        self.perf_xy_override = (float(fv), float(rv), until)
        self._send_cmd(CMD_SET_MOD_AMT, a=6, c=13, val=fv + 100)
        self._send_cmd(CMD_SET_MOD_AMT, a=6, c=14, val=rv)
        return fv, rv

    def _apply_perf_xy(self, x0, y0, x1, y1, x, y):
        tx = max(0.0, min(1.0, (x - x0) / max(1.0, x1 - x0)))
        ty = max(0.0, min(1.0, (y - y0) / max(1.0, y1 - y0)))
        filt = int(round((tx * 200.0 - 100.0) / 2.0)) * 2
        if abs(filt) <= 3:
            filt = 0
        reso = int(round(((1.0 - ty) * 100.0) / 2.0)) * 2
        return self._send_perf_xy(filt, reso, persistent=True)

    def _send_perf_flags(self):
        val = (1 if self.perf_break_local else 0) | (2 if self.perf_throw_local else 0)
        self._send_cmd(CMD_SET_MOD_AMT, a=6, c=16, val=val)

    def _release_momentary_fx(self):
        if self.perf_fx_local != 0:
            self.perf_fx_local = 0
            self._send_cmd(CMD_SET_MOD_AMT, a=6, c=15, val=0)
        if self.perf_break_local or self.perf_throw_local:
            self.perf_break_local = False
            self.perf_throw_local = False
            self._send_perf_flags()

    def _cancel_choke_hold(self):
        if self._choke_hold_after is not None:
            try:
                self.after_cancel(self._choke_hold_after)
            except Exception:
                pass
            self._choke_hold_after = None

    def _on_choke_hold_timeout(self, grp):
        self._choke_hold_after = None
        if self.drag_info and self.drag_info[0] == "choke_hold" and self.drag_info[1] == grp:
            self.drag_info = None
            _cm, cs = self._get_choke_masks()
            new_s = cs ^ (1 << grp)
            self._set_choke_masks(solo_mask=new_s)
            self._toast("Solo G%d" % (grp + 1) if (new_s & (1 << grp)) else "Solo G%d Off" % (grp + 1), secs=1.5)
            self.refresh_gui()

    def _draw_play_tab(self, c, w, h, top_h, fam, fs):
        pad_m = 6
        y0 = top_h + pad_m
        fsmall = max(7, fs - 1)
        ftiny = max(6, fs - 2)

        scene = int(round(self._mon("out_seq_scene", 0)))
        pend = int(round(self._mon("out_seq_pend", -1)))
        smode = self.shm_hdr[6] if self.shm_hdr else int(round(self._zval("seq_mode", 0)))
        play_mode = int(self.shm_hdr[18]) if (self.shm_hdr and len(self.shm_hdr) >= 30) else 0
        rpt_div = int(self.shm_hdr[19]) if (self.shm_hdr and len(self.shm_hdr) >= 30) else 0
        fill_ret = int(self.shm_hdr[24]) if (self.shm_hdr and len(self.shm_hdr) >= 30) else -1
        bar_phase = float(self.shm_hdr[35]) if (self.shm_hdr and len(self.shm_hdr) >= 36) else 0.0
        choke_mute, choke_solo = self._get_choke_masks()
        perf_filt, perf_reso = self._get_perf_xy()
        fx_flags = int(self.shm_hdr[34]) if (self.shm_hdr and len(self.shm_hdr) >= 36) else 0
        cur_fx = self.perf_fx_local if self.perf_fx_local else (fx_flags & 0xF)
        is_break = self.perf_break_local or bool(fx_flags & 0x10)
        is_throw = self.perf_throw_local or bool(fx_flags & 0x20)

        # ---- Bottom Full-Width: 4-Beat Bar Progress + 5 Large Scene Buttons (A..E, hold = 1-Bar Fill) ----
        prog_h = 8
        sc_h = max(46, fs * 3 + 16)
        sc_y0 = h - pad_m - sc_h
        prog_y0 = sc_y0 - prog_h - 4
        beat_w = (w - 2 * pad_m - 12) / 4.0
        for b in range(4):
            bx0 = pad_m + b * (beat_w + 4)
            bx1 = bx0 + beat_w
            c.create_rectangle(bx0, prog_y0, bx1, prog_y0 + prog_h, fill="#1a1c24", outline="#2f3342", width=1)
            if smode >= 1:
                b_start = b * 4.0
                if bar_phase >= b_start + 4.0:
                    c.create_rectangle(bx0 + 1, prog_y0 + 1, bx1 - 1, prog_y0 + prog_h - 1, fill="#2b6cb0", outline="")
                elif bar_phase > b_start:
                    frac = max(0.0, min(1.0, (bar_phase - b_start) / 4.0))
                    fx = bx0 + 1 + frac * max(1.0, bx1 - bx0 - 2)
                    c.create_rectangle(bx0 + 1, prog_y0 + 1, fx, prog_y0 + prog_h - 1,
                                       fill=COL_PEND if fill_ret >= 0 else COL_ACCENT, outline="")

        sbw = (w - 2 * pad_m + 4) / float(EP_UI_SCENES)
        for s in range(EP_UI_SCENES):
            sx0 = pad_m + s * sbw
            sx1 = sx0 + sbw - 4
            is_fill = (fill_ret >= 0 and s == scene)
            if is_fill:
                bg, fg, out_c = "#e64980", "#ffffff", "#ff8787"
                lbl = "Fill %s->%s" % (SCENE_NAMES[s], SCENE_NAMES[fill_ret & 7])
            elif s == scene:
                bg, fg, out_c = COL_ACCENT, "#000000", "#a5d8ff"
                lbl = "Scene " + SCENE_NAMES[s]
            elif s == pend:
                bg, fg, out_c = COL_PEND, "#000000", "#fff3bf"
                lbl = "Next " + SCENE_NAMES[s]
            else:
                bg, fg, out_c = COL_PANEL, "#f0f0f5", "#444452"
                lbl = "Scene " + SCENE_NAMES[s]
            self._add_btn(c, sx0, sc_y0, sx1, h - pad_m, lbl, bg, fg, fam, fs + 1, "scene", s, outline=out_c)

        # ---- Main Two-Column Performance Area ----
        body_y1 = prog_y0 - 6
        body_h = max(140, body_y1 - y0)
        mid_x = w // 2
        lx0, lx1 = pad_m, mid_x - 4
        rx0, rx1 = mid_x + 4, w - pad_m
        lw = max(100, lx1 - lx0)
        rw = max(100, rx1 - rx0)

        # ==== LEFT COLUMN: Choke Groups (top) + Note Repeat (mid) + 16 Levels (bot) ====
        gap_y = 6
        ch_h = int(body_h * 0.44)
        rpt_h = int(body_h * 0.27)
        l16_h = max(44, body_h - ch_h - rpt_h - 2 * gap_y)

        # 1) CHOKE GROUP MUTE (4 tall buttons G1..G4 + All On)
        hdr_h = max(18, fs + 8)
        c.create_text(lx0 + 2, y0 + hdr_h / 2, anchor="w",
                      text="CHOKE MUTE  (hold=Solo)", fill="#a8aebf", font=(fam, ftiny, "bold"))
        any_choke_mod = bool(choke_mute or choke_solo)
        all_w = max(56, int(lw * 0.22))
        self._add_btn(c, lx1 - all_w, y0, lx1, y0 + hdr_h, "All On",
                      "#e03131" if any_choke_mod else COL_PANEL,
                      "#ffffff" if any_choke_mod else "#787c8c",
                      fam, ftiny, "choke_all_on", None,
                      outline="#ff8787" if any_choke_mod else "#383844")

        gy0 = y0 + hdr_h + 3
        gy1 = y0 + ch_h
        gw4 = (lw - 12) / 4.0
        for g in range(4):
            gx0 = lx0 + g * (gw4 + 4)
            gx1 = gx0 + gw4
            gpads = [p for p in range(EP_PADS) if self.pads_data and int(self.pads_data[p][11]) == (g + 1)]
            glvl = max((self._mon("out_pad_lvl%d" % (p + 1), 0.0) for p in gpads), default=0.0)
            is_m = bool(choke_mute & (1 << g))
            is_s = bool(choke_solo & (1 << g))
            accent = ROW_COLORS[g]

            if is_s:
                bg, out_c, out_w = "#473b19", "#ffd43b", 2
            elif is_m:
                bg, out_c, out_w = "#421a22", "#fa5252", 2
            elif glvl > 0.06:
                bg, out_c, out_w = "#2b3852", accent, 2
            else:
                bg, out_c, out_w = COL_PAD if gpads else "#1b1b22", "#444858" if gpads else "#2d2e38", 1

            c.create_rectangle(gx0, gy0, gx1, gy1, fill=bg, outline=out_c, width=out_w)
            c.create_rectangle(gx0 + 1, gy0 + 1, gx1 - 1, gy0 + 6,
                               fill="#555560" if is_m else accent, width=0)
            if glvl > 0.02 and not is_m:
                bw_m = max(2, int((gx1 - gx0 - 6) * min(glvl, 1.0)))
                c.create_rectangle(gx0 + 3, gy1 - 6, gx0 + 3 + bw_m, gy1 - 2, fill="#ffd43b", width=0)

            gh_box = gy1 - gy0
            c.create_text((gx0 + gx1) / 2, gy0 + gh_box * 0.36, text="G%d" % (g + 1),
                          fill="#ff8787" if is_m else ("#ffd43b" if is_s else ("#ffffff" if gpads else "#686c7c")),
                          font=(fam, fs + 3, "bold"))
            if is_s:
                sub, sub_col = "SOLO", "#ffd43b"
            elif is_m:
                sub, sub_col = "MUTE", "#ff8787"
            elif len(gpads) == 1:
                sub, sub_col = short_sample_name(self._pad_path(gpads[0]), gpads[0])[:8], "#c0c6d8"
            elif len(gpads) > 1:
                sub, sub_col = "%d pads" % len(gpads), "#c0c6d8"
            else:
                sub, sub_col = "—", "#555866"
            c.create_text((gx0 + gx1) / 2, gy0 + gh_box * 0.68, text=sub, fill=sub_col,
                          font=(fam, ftiny, "bold" if (is_m or is_s) else "normal"))
            self.hit_boxes.append((gx0, gy0, gx1, gy1, "choke_grp", g))

        # 2) NOTE REPEAT (5 direct-tap buttons: Off | 1/8 | 1/16 | 16T | 1/32)
        ry_sec = y0 + ch_h + gap_y
        lbl_h = max(14, ftiny + 6)
        c.create_text(lx0 + 2, ry_sec + lbl_h / 2, anchor="w",
                      text="NOTE REPEAT", fill="#ffa94d" if rpt_div > 0 else "#a8aebf", font=(fam, ftiny, "bold"))
        rby0 = ry_sec + lbl_h + 2
        rby1 = ry_sec + rpt_h
        bw5 = (lw - 12) / 5.0
        for di, dlab in enumerate(REPEAT_DIV_LABELS):
            bx0 = lx0 + di * (bw5 + 3)
            bx1 = bx0 + bw5
            sel = (rpt_div == di)
            if sel and di > 0:
                bg, fg, out_c = "#d9480f", "#ffffff", "#ffa94d"
            elif sel:
                bg, fg, out_c = "#323646", "#f0f0f5", "#6c7288"
            else:
                bg, fg, out_c = COL_PANEL, "#b0b4c4", "#3a3d4c"
            self._add_btn(c, bx0, rby0, bx1, rby1, dlab, bg, fg, fam, fsmall,
                          "set_repeat_div", di, outline=out_c)

        # 3) 16 LEVELS (4 direct-tap buttons: Norm | Chrm | 16Vl | 16Fl)
        ly_sec = ry_sec + rpt_h + gap_y
        sel_name = short_sample_name(self._pad_path(self.sel_pad), self.sel_pad)
        c.create_text(lx0 + 2, ly_sec + lbl_h / 2, anchor="w",
                      text="16 LEVELS  •  P%d %s" % (self.sel_pad + 1, sel_name),
                      fill="#b197fc" if play_mode > 0 else "#a8aebf", font=(fam, ftiny, "bold"))
        lby0 = ly_sec + lbl_h + 2
        lby1 = body_y1
        bw4 = (lw - 9) / 4.0
        for pi, plab in enumerate(PLAY_MODE_LABELS):
            bx0 = lx0 + pi * (bw4 + 3)
            bx1 = bx0 + bw4
            sel = (play_mode == pi)
            if sel and pi > 0:
                bg, fg, out_c = "#5f3dc4", "#ffffff", "#b197fc"
            elif sel:
                bg, fg, out_c = "#323646", "#f0f0f5", "#6c7288"
            else:
                bg, fg, out_c = COL_PANEL, "#b0b4c4", "#3a3d4c"
            self._add_btn(c, bx0, lby0, bx1, lby1, plab, bg, fg, fam, fsmall,
                          "set_play_mode", pi, outline=out_c)

        # ==== RIGHT COLUMN: Master Filter XY-Pad (top) + Punch-in FX 2x3 (bot) ====
        xy_sec_h = int(body_h * 0.56)
        fx_y0 = y0 + xy_sec_h + gap_y

        # 1) MASTER FILTER XY-PAD (X = LP/HP, Y = Reso, Mom / Hold toggle)
        filt_active = abs(perf_filt) >= 2.0
        if not filt_active:
            f_txt = "Open"
        elif perf_filt < 0:
            f_txt = "LP %d%% · R%d" % (abs(int(round(perf_filt))), int(round(perf_reso)))
        else:
            f_txt = "HP %d%% · R%d" % (int(round(perf_filt)), int(round(perf_reso)))
        c.create_text(rx0 + 2, y0 + hdr_h / 2, anchor="w",
                      text="MASTER FILTER  •  %s" % f_txt,
                      fill="#da77f2" if (filt_active and perf_filt < 0) else ("#66d9e8" if filt_active else "#a8aebf"),
                      font=(fam, ftiny, "bold"))
        mw_btn = max(58, int(rw * 0.20))
        self._add_btn(c, rx1 - mw_btn, y0, rx1, y0 + hdr_h,
                      "Hold" if self.xy_hold_mode else "Mom",
                      "#5f3dc4" if self.xy_hold_mode else "#1f3d2b",
                      "#ffffff" if self.xy_hold_mode else "#63e6be",
                      fam, ftiny, "xy_mode_toggle", None,
                      outline="#b197fc" if self.xy_hold_mode else "#40c057")

        xy0 = y0 + hdr_h + 3
        xy1 = y0 + xy_sec_h
        xymid = (rx0 + rx1) * 0.5
        xy_col = "#da77f2" if perf_filt < -2.0 else ("#66d9e8" if perf_filt > 2.0 else "#4dabf7")
        c.create_rectangle(rx0, xy0, rx1, xy1, fill="#12131a",
                           outline=xy_col if filt_active else "#3a3e4e", width=2 if filt_active else 1)
        # Subtle quadrant grid
        for frac in (0.25, 0.5, 0.75):
            gx = rx0 + frac * rw
            gy = xy0 + frac * (xy1 - xy0)
            c.create_line(gx, xy0 + 2, gx, xy1 - 2,
                          fill="#2e3244" if frac == 0.5 else "#1d202b", width=1,
                          dash=(3, 3) if frac == 0.5 else ())
            c.create_line(rx0 + 2, gy, rx1 - 2, gy, fill="#1d202b", width=1)
        # Edge labels
        c.create_text(rx0 + 6, (xy0 + xy1) / 2, anchor="w", text="LP", fill="#6a5478", font=(fam, ftiny, "bold"))
        c.create_text(rx1 - 6, (xy0 + xy1) / 2, anchor="e", text="HP", fill="#466e78", font=(fam, ftiny, "bold"))
        c.create_text(xymid, xy1 - 4, anchor="s", text="OPEN", fill="#4a5064", font=(fam, max(6, ftiny - 1)))
        c.create_text(rx0 + 6, xy0 + 4, anchor="nw", text="RESO", fill="#4a5064", font=(fam, max(6, ftiny - 1)))

        # Shaded sweep fill + crosshair + glowing puck
        px = rx0 + ((max(-100.0, min(100.0, perf_filt)) + 100.0) / 200.0) * rw
        py = xy1 - (max(0.0, min(100.0, perf_reso)) / 100.0) * max(1.0, xy1 - xy0)
        if filt_active:
            xa, xb = (xymid, px) if px >= xymid else (px, xymid)
            c.create_rectangle(xa, xy0 + 2, xb, xy1 - 2,
                               fill="#2b1938" if perf_filt < 0 else "#152f38", outline="")
        c.create_line(px, xy0 + 2, px, xy1 - 2, fill=xy_col, width=1)
        c.create_line(rx0 + 2, py, rx1 - 2, py, fill=xy_col, width=1)
        r_pk = 8
        c.create_oval(px - r_pk, py - r_pk, px + r_pk, py + r_pk,
                      fill=xy_col if filt_active else "#2e3548", outline="#ffffff", width=2)
        self.hit_boxes.append((rx0, xy0, rx1, xy1, "perf_xy", (rx0, xy0, rx1, xy1)))

        # 2) PUNCH-IN FX (2 rows x 3 large momentary hold buttons)
        c.create_text(rx0 + 2, fx_y0 + lbl_h / 2, anchor="w",
                      text="PUNCH-IN FX  (hold)",
                      fill="#66d9e8" if (cur_fx or is_break or is_throw) else "#a8aebf",
                      font=(fam, ftiny, "bold"))
        fb_y0 = fx_y0 + lbl_h + 2
        fb_h = max(60, body_y1 - fb_y0)
        row_h = (fb_h - 4) / 2.0
        bw3 = (rw - 8) / 3.0

        # Row 1: Stut 1/4 | Stut 1/8 | Stut 1/16
        for ci, (fx_id, lbl) in enumerate(((1, "Stut 1/4"), (2, "Stut 1/8"), (3, "Stut 1/16"))):
            bx0 = rx0 + ci * (bw3 + 4)
            bx1 = bx0 + bw3
            on = (cur_fx == fx_id)
            self._add_btn(c, bx0, fb_y0, bx1, fb_y0 + row_h, lbl,
                          "#1098ad" if on else "#1a2832",
                          "#ffffff" if on else "#99e9f2",
                          fam, fsmall, "perf_fx_hold", fx_id,
                          outline="#ffffff" if on else "#2b6c7a")

        # Row 2: Tape Stop | Break (Drop) | Aux1 Throw
        r2y0 = fb_y0 + row_h + 4
        on_ts = (cur_fx == 4)
        self._add_btn(c, rx0, r2y0, rx0 + bw3, body_y1, "Tape Stop",
                      "#e67700" if on_ts else "#2e2418",
                      "#000000" if on_ts else "#ffd43b",
                      fam, fsmall, "perf_fx_hold", 4,
                      outline="#ffffff" if on_ts else "#8c5c1a")

        bx_br = rx0 + bw3 + 4
        self._add_btn(c, bx_br, r2y0, bx_br + bw3, body_y1, "Break",
                      "#e03131" if is_break else "#2e1a1f",
                      "#ffffff" if is_break else "#ffa8a8",
                      fam, fsmall, "perf_break_hold", None,
                      outline="#ffffff" if is_break else "#8c2b36")

        bx_th = rx0 + 2 * (bw3 + 4)
        self._add_btn(c, bx_th, r2y0, rx1, body_y1, "Aux1 Throw",
                      "#7048e8" if is_throw else "#241d36",
                      "#ffffff" if is_throw else "#d0bfff",
                      fam, fsmall, "perf_throw_hold", None,
                      outline="#ffffff" if is_throw else "#5b4494")

    def _draw_pads_tab(self, c, w, h, top_h, fam, fs):
        pad_m = 4
        y0 = top_h + pad_m
        split_x = int(w * 0.52)
        play_mode = int(self.shm_hdr[18]) if (self.shm_hdr and len(self.shm_hdr) >= 30) else 0

        # ---- Left: 4x4 Hardware Pad Grid (bottom row = Pads 1..4, top row = Pads 13..16) ----
        gw = split_x - pad_m * 2
        gh = h - y0 - pad_m
        cw = gw / 4.0
        ch = gh / 4.0
        fsmall = max(7, fs - 2)
        sel_pinfo = self.pads_data[self.sel_pad] if self.pads_data else None
        base_note = int(sel_pinfo[15]) if sel_pinfo else (36 + self.sel_pad)

        for p in range(EP_PADS):
            col_i = p % 4
            row_hw = p // 4               # 0 = bottom, 3 = top
            row_ui = 3 - row_hw
            px0 = pad_m + col_i * cw + 2
            py0 = y0 + row_ui * ch + 2
            px1 = pad_m + (col_i + 1) * cw - 2
            py1 = y0 + (row_ui + 1) * ch - 2

            if play_mode > 0:
                # 16 Levels / Chromatic mode: all 16 pads play selected pad at different Pitch / Vel / Filter
                is_root = (p == 0 or p == 12) if play_mode == 1 else (p == 15)
                if play_mode == 1:
                    bg = "#364fc7" if is_root else "#22253a"
                    accent = "#74c0fc" if is_root else "#9775fa"
                    lbl = "%s (%+d)" % (note_short(base_note + p), p)
                    sub = "P%d" % (self.sel_pad + 1)
                elif play_mode == 2:
                    vv = 8 + (p * 119) // 15
                    bg = "#1f2e3a"
                    accent = "#63e6be"
                    lbl = "Vel %d" % vv
                    sub = "%d%%" % int(round(vv * 100.0 / 127.0))
                else:
                    fo = -90 + p * 12
                    bg = "#2b1f38" if fo < 0 else "#1f2c38"
                    accent = "#da77f2" if fo < 0 else "#66d9e8"
                    lbl = ("LP %d" % abs(fo)) if fo < -2 else (("HP %d" % fo) if fo > 2 else "Open")
                    sub = "Flt"
                c.create_rectangle(px0, py0, px1, py1, fill=bg, outline=accent if is_root else "#3e3e52", width=2 if is_root else 1)
                c.create_rectangle(px0 + 1, py0 + 1, px1 - 1, py0 + 5, fill=accent, width=0)
                if play_mode == 2:
                    bw_v = max(2, int((px1 - px0 - 4) * ((p + 1) / 16.0)))
                    c.create_rectangle(px0 + 2, py1 - 5, px0 + 2 + bw_v, py1 - 1, fill=accent, width=0)
                c.create_text(px0 + 3, py0 + 7, anchor="nw", text=str(p + 1), fill="#9090a4", font=(fam, max(6, fsmall - 1)))
                c.create_text((px0 + px1) / 2, (py0 + py1) / 2, text=lbl, fill="#f8f9fa", font=(fam, fsmall, "bold"))
                c.create_text((px0 + px1) / 2, py1 - 10, text=sub, fill="#9ea4b8", font=(fam, max(6, fsmall - 1)))
                self.hit_boxes.append((px0, py0, px1, py1, "pad_16lvl", p))
                continue

            lvl = self._mon("out_pad_lvl%d" % (p + 1), 0.0)
            is_sel = (p == self.sel_pad)
            pinfo = self.pads_data[p] if self.pads_data else None
            mute = pinfo[13] if pinfo else 0
            solo = pinfo[14] if pinfo else 0
            choke = pinfo[11] if pinfo else 0
            lpad = int(pinfo[38]) if (pinfo and len(pinfo) > 39) else 0
            lmode = int(pinfo[39]) if (pinfo and len(pinfo) > 39) else 0

            bg = COL_PAD_SEL if is_sel else COL_PAD
            if lvl > 0.05:
                bg = "#4c6ef5" if is_sel else "#3b4261"
            accent = ROW_COLORS[row_hw]
            outline = "#ffffff" if is_sel else ("#ffd43b" if solo else "#3e3e4a")

            c.create_rectangle(px0, py0, px1, py1, fill=bg, outline=outline, width=2 if is_sel else 1)
            c.create_rectangle(px0 + 1, py0 + 1, px1 - 1, py0 + 5, fill=accent if not mute else "#444444", width=0)
            if lvl > 0.02:
                bar_w = max(2, int((px1 - px0 - 4) * min(lvl, 1.0)))
                c.create_rectangle(px0 + 2, py1 - 4, px0 + 2 + bar_w, py1 - 1, fill="#ffd43b", width=0)

            name = short_sample_name(self._pad_path(p), p)
            c.create_text((px0 + px1) / 2, (py0 + py1) / 2 + 1, text=name,
                          fill="#888888" if mute else "#f0f0f4", font=(fam, fsmall))
            c.create_text(px0 + 3, py0 + 7, anchor="nw", text=str(p + 1), fill="#9090a0", font=(fam, max(6, fsmall - 1)))
            link_b = (("R%d" if lmode else "L%d") % lpad) if lpad > 0 else ""
            badge = ("S" if solo else "") + ("M" if mute else "") + (("C%d" % choke) if choke > 0 else "") + link_b
            if badge:
                c.create_text(px1 - 3, py0 + 7, anchor="ne", text=badge,
                              fill="#ffd43b" if solo else ("#ff6b6b" if mute else "#66d9e8"),
                              font=(fam, max(6, fsmall - 1)))
            self.hit_boxes.append((px0, py0, px1, py1, "pad", p))

        # ---- Right: Selected Pad Inspector + Waveform + Scene Bar ----
        rx0 = split_x + 4
        rx1 = w - pad_m
        rw = max(80, rx1 - rx0)
        bh = max(22, fs * 2 + 4)

        # Row 1: Action buttons (< | Load | > | Mute | Solo | Auto)
        pinfo = self.pads_data[self.sel_pad] if self.pads_data else None
        mute = pinfo[13] if pinfo else 0
        solo = pinfo[14] if pinfo else 0
        follow = self.shm_hdr[4] if self.shm_hdr else int(round(self._zval("follow", 1)))

        arrow_w = max(22.0, rw * 0.08)
        rem_w = max(40.0, rw - 2 * arrow_w - 13.0)
        bw4 = rem_w / 4.0
        cx = rx0
        self._add_btn(c, cx, y0, cx + arrow_w, y0 + bh, "<", COL_PANEL, "#e0e0f0", fam, fsmall, "sample_step", (self.sel_pad, -1))
        cx += arrow_w + 2
        self._add_btn(c, cx, y0, cx + bw4, y0 + bh, "Load", COL_ACCENT, "#000000", fam, fsmall, "load", self.sel_pad)
        cx += bw4 + 2
        self._add_btn(c, cx, y0, cx + arrow_w, y0 + bh, ">", COL_PANEL, "#e0e0f0", fam, fsmall, "sample_step", (self.sel_pad, 1))
        cx += arrow_w + 3
        self._add_btn(c, cx, y0, cx + bw4, y0 + bh, "Mute",
                      COL_REC if mute else COL_PANEL, "#000000" if mute else "#e0e0e0", fam, fsmall, "mute", self.sel_pad)
        cx += bw4 + 3
        self._add_btn(c, cx, y0, cx + bw4, y0 + bh, "Solo",
                      COL_PEND if solo else COL_PANEL, "#000000" if solo else "#e0e0e0", fam, fsmall, "solo", self.sel_pad)
        cx += bw4 + 3
        self._add_btn(c, cx, y0, rx1, y0 + bh, "Auto" if follow else "Lock",
                      COL_PLAY if follow else COL_PANEL, "#000000" if follow else "#e0e0e0", fam, fsmall, "follow", 1 - follow)

        # Waveform box
        wy0 = y0 + bh + 5
        sc_h = max(44, fs * 3 + 16)
        aux_h = max(18, fs + 8)
        info_h = max(16, fs + 6)
        wy1 = max(wy0 + 28, h - pad_m - sc_h - aux_h - info_h - 14)
        c.create_rectangle(rx0, wy0, rx1, wy1, fill="#111116", outline="#383844")
        path = self._pad_path(self.sel_pad)
        if path:
            self._ensure_peaks(path)
            peaks = self.peak_cache.get(path)
            if peaks:
                n = len(peaks)
                mid = (wy0 + wy1) * 0.5
                amp = (wy1 - wy0) * 0.44
                step_x = (rx1 - rx0 - 4) / float(max(1, n))
                for i, (mn, mx) in enumerate(peaks):
                    x = rx0 + 2 + i * step_x
                    c.create_line(x, mid - mx * amp, x, mid - mn * amp, fill=COL_WAVE)
            else:
                c.create_text((rx0 + rx1) / 2, (wy0 + wy1) / 2, text="Reading...", fill=COL_DIM, font=(fam, fsmall))
            # Start & End markers
            if pinfo:
                sx = rx0 + 2 + (pinfo[4] / 100.0) * (rx1 - rx0 - 4)
                ex = rx0 + 2 + (pinfo[5] / 100.0) * (rx1 - rx0 - 4)
                c.create_line(sx, wy0 + 1, sx, wy1 - 1, fill=COL_PEND, width=2)
                c.create_line(ex, wy0 + 1, ex, wy1 - 1, fill=COL_PEND, width=1, dash=(3, 2))
            # Playhead
            ph = self._mon("out_pad_playhead", -1.0)
            if 0.0 <= ph <= 1.0:
                px = rx0 + 2 + ph * (rx1 - rx0 - 4)
                c.create_line(px, wy0 + 1, px, wy1 - 1, fill="#ffffff", width=2)
            st_pct = pinfo[4] if pinfo else 0.0
            en_pct = pinfo[5] if pinfo else 100.0
            self.hit_boxes.append((rx0, wy0, rx1, wy1, "wave_trim", (self.sel_pad, st_pct, en_pct, rx0 + 2, rx1 - 2)))
        else:
            c.create_text((rx0 + rx1) / 2, (wy0 + wy1) / 2, text="Tap to Load WAV", fill=COL_DIM, font=(fam, fs))
            self.hit_boxes.append((rx0, wy0, rx1, wy1, "load", self.sel_pad))

        # Parameter summary readout + compact Aux 1 / Aux 2 / Width sliders for selected pad
        iy0 = wy1 + 3
        if pinfo:
            line1 = "P%d  Pt %+.0f  Dec %d%%  Flt %+d  Lvl %d%%  Wid %+d%%" % (
                self.sel_pad + 1, pinfo[0], int(round(pinfo[1])), int(round(pinfo[2])),
                int(round(pinfo[3])), int(round(pinfo[12]))
            )
            c.create_text(rx0 + 2, iy0, anchor="nw", text=line1, fill="#d0d0dc", font=(fam, fsmall))

        ay0 = iy0 + info_h + 2
        ay1 = ay0 + aux_h
        aw4 = (rw - 9) / 4.0
        aux_cols = (
            ("#66d9e8", "#1c3d47"),
            ("#da77f2", "#3b1f47"),
            ("#ffd43b", "#473b19"),
            ("#63e6be", "#1e453b"),
        )
        for ai in range(4):
            sym = "pad_aux%d" % (ai + 1)
            val = pinfo[16 + ai] if (pinfo and len(pinfo) > 16 + ai) else self._zval(sym, 0.0)
            col_hi, col_fill = aux_cols[ai]
            tgt = self.aux_targets[ai]
            tname = tgt[1][:4] if tgt else ("A%d" % (ai + 1))
            lbl = "%s %d" % (tname, int(round(val)))
            ax0 = rx0 + ai * (aw4 + 3)
            ax1 = ax0 + aw4
            c.create_rectangle(ax0, ay0, ax1, ay1, fill="#1a1c24",
                               outline=col_hi if val > 0.5 else "#383844", width=1)
            if val > 0.5:
                fx = ax0 + 1 + (min(val, 100.0) / 100.0) * max(1.0, (ax1 - ax0 - 2))
                c.create_rectangle(ax0 + 1, ay0 + 1, fx, ay1 - 1, fill=col_fill, outline="")
                c.create_rectangle(ax0 + 1, ay0 + 1, fx, ay0 + 3, fill=col_hi, width=0)
            c.create_text((ax0 + ax1) / 2, (ay0 + ay1) / 2,
                          text=lbl,
                          fill=col_hi if val > 0.5 else "#9090a4", font=(fam, max(6, fsmall - 1), "bold"))
            self.hit_boxes.append((ax0, ay0, ax1, ay1, "aux_slider", (sym, 0.0, 100.0, ax0, ax1)))

        # Quick 5-Scene Launch Bar at bottom right (large A..E buttons)
        sy0 = h - pad_m - sc_h
        sc_btn_h = h - pad_m - sy0
        scene = int(round(self._mon("out_seq_scene", 0)))
        pend = int(round(self._mon("out_seq_pend", -1)))
        sbw = rw / float(EP_UI_SCENES)
        for s in range(EP_UI_SCENES):
            sx0 = rx0 + s * sbw
            sx1 = sx0 + sbw - 3
            bg = COL_ACCENT if s == scene else (COL_PEND if s == pend else COL_PANEL)
            fg = "#000000" if (s == scene or s == pend) else "#f0f0f5"
            self._add_btn(c, sx0, sy0, sx1, sy0 + sc_btn_h, "Sc " + SCENE_NAMES[s],
                          bg, fg, fam, fs + 1, "scene", s)

        # Sample Settings popup overlay when waveform is long-pressed
        if self.sample_popup is not None:
            self._draw_sample_popup(c, w, h, top_h, fam, fs)

    def _draw_sample_popup(self, c, w, h, top_h, fam, fs):
        self.hit_boxes = []
        p = self.sample_popup
        pinfo = self.pads_data[p] if (self.pads_data and 0 <= p < len(self.pads_data)) else None
        pad_col = ROW_COLORS[(p // 4) & 3]
        fsmall = max(7, fs - 1)
        vals = self.sample_popup_vals or {}

        def _val(sym, idx, default):
            if sym in vals:
                return float(vals[sym])
            if pinfo and idx is not None and idx < len(pinfo):
                return float(pinfo[idx])
            return float(self._zval(sym, default))

        pw = max(300, min(int(w * 0.90), 700))
        ph = max(230, min(int((h - top_h) * 0.90), 380))
        px0 = (w - pw) // 2
        py0 = top_h + max(4, (h - top_h - ph) // 2)
        px1, py1 = px0 + pw, py0 + ph

        # Dimmed backdrop + modal card
        c.create_rectangle(0, top_h, w, h, fill="#0b0b0f", stipple="gray50", outline="")
        c.create_rectangle(px0, py0, px1, py1, fill=COL_BG, outline=pad_col, width=2)

        pad_i = 8
        ix0, ix1 = px0 + pad_i, px1 - pad_i
        iw = max(120, ix1 - ix0)

        # Header bar: Pad name + [Sound | Tools] toggle + Play + Load + Close
        hdr_h = max(26, fs * 2 + 6)
        c.create_rectangle(px0 + 1, py0 + 1, px1 - 1, py0 + hdr_h, fill=COL_PANEL, outline="")
        c.create_rectangle(px0 + 1, py0 + 1, px1 - 1, py0 + 4, fill=pad_col, width=0)
        pname = short_sample_name(self._pad_path(p), p)
        c.create_text(ix0 + 2, py0 + hdr_h / 2 + 1, anchor="w",
                      text="P%d — %s" % (p + 1, pname),
                      fill=pad_col, font=(fam, fs, "bold"))

        bw_h = max(42, int(iw * 0.115))
        bx_close = ix1 - bw_h
        bx_load = bx_close - bw_h - 4
        bx_play = bx_load - bw_h - 4
        bx_tools = bx_play - bw_h - 8
        bx_sound = bx_tools - bw_h - 2
        self._add_btn(c, bx_sound, py0 + 4, bx_sound + bw_h, py0 + hdr_h - 3, "Sound",
                      pad_col if self.sample_popup_tab == 0 else "#1a1c24",
                      "#000000" if self.sample_popup_tab == 0 else "#a0a4b8",
                      fam, fsmall, "sample_popup_tab", 0, outline=pad_col if self.sample_popup_tab == 0 else "#3c3f50")
        self._add_btn(c, bx_tools, py0 + 4, bx_tools + bw_h, py0 + hdr_h - 3, "Tools",
                      pad_col if self.sample_popup_tab == 1 else "#1a1c24",
                      "#000000" if self.sample_popup_tab == 1 else "#a0a4b8",
                      fam, fsmall, "sample_popup_tab", 1, outline=pad_col if self.sample_popup_tab == 1 else "#3c3f50")
        self._add_btn(c, bx_play, py0 + 4, bx_play + bw_h, py0 + hdr_h - 3, "Play",
                      "#1f3d2b", "#63e6be", fam, fsmall, "sample_popup_play", p, outline="#40c057")
        self._add_btn(c, bx_load, py0 + 4, bx_load + bw_h, py0 + hdr_h - 3, "Load",
                      COL_ACCENT, "#000000", fam, fsmall, "sample_popup_load", p, outline=COL_ACCENT)
        self._add_btn(c, bx_close, py0 + 4, ix1, py0 + hdr_h - 3, "Close",
                      "#2a2a34", "#f0f0f5", fam, fsmall, "sample_popup_close", None, outline="#444452")

        body_y = py0 + hdr_h + 6
        body_h = max(150, py1 - body_y - 6)

        if self.sample_popup_tab == 1:
            self._draw_sample_tools_body(c, ix0, ix1, body_y, body_h, p, pinfo, pad_col, fam, fs, fsmall)
        else:
            # 7 clean rows: 6 rows of 2 sliders (incl. Attack & Reso) + 1 bottom row of Mode (incl. Loop) & Choke
            gap = 4
            row_h = max(21.0, (body_h - 6 * gap) / 7.0)
            col_gap = 6
            col_w = (iw - col_gap) / 2.0

            # (sym, pinfo_idx, label, vmin, vmax, step, default, bipolar, fmt_fn, col_hi, col_fill)
            slider_rows = (
                (
                    ("pad_pitch",  0, "Pitch",  -24.0,  24.0, 1.0,   0.0, True,  lambda v: "%+.0fst" % v, "#74c0fc", "#1e3a52"),
                    ("pad_level",  3, "Level",    0.0, 100.0, 2.0,  80.0, False, lambda v: "%d%%" % round(v), "#63e6be", "#1e453b"),
                ),
                (
                    ("_pad_attack", 36, "Attack", 0.0, 100.0, 2.0,   0.0, False, lambda v: "0ms" if v < 1 else ("%dms" % round(500.0 ** (v / 100.0))), "#a9e34b", "#2a3d1c"),
                    ("pad_decay",  1, "Decay",    0.0, 100.0, 2.0, 100.0, False, lambda v: "Full" if v >= 99.5 else ("%d%%" % round(v)), "#ffd43b", "#473b19"),
                ),
                (
                    ("pad_filter", 2, "Filter", -100.0, 100.0, 5.0,  0.0, True,  lambda v: "Open" if abs(v) < 2 else (("LP %d" % abs(round(v))) if v < 0 else ("HP %d" % round(v))), "#da77f2", "#3b1f47"),
                    ("_pad_reso", 37, "Reso",     0.0, 100.0, 5.0,  20.0, False, lambda v: "%d%%" % round(v), "#f783ac", "#4a1f32"),
                ),
                (
                    ("pad_start",  4, "Start",    0.0,  99.0, 1.0,   0.0, False, lambda v: "%d%%" % round(v), "#ffd43b", "#3d3318"),
                    ("pad_end",    5, "End",      1.0, 100.0, 1.0, 100.0, False, lambda v: "%d%%" % round(v), "#ffd43b", "#3d3318"),
                ),
                (
                    ("pad_punch",  7, "Punch", -100.0, 100.0, 5.0,   0.0, True,  lambda v: "%+d%%" % round(v), "#ff922b", "#4a2c14"),
                    ("pad_drive",  8, "Drive",    0.0, 100.0, 2.0,   0.0, False, lambda v: "%d%%" % round(v), "#ff6b6b", "#4a1e24"),
                ),
                (
                    ("pad_pan",    9, "Pan",   -100.0, 100.0, 5.0,   0.0, True,  lambda v: "C" if abs(v) < 2 else (("L%d" % abs(round(v))) if v < 0 else ("R%d" % round(v))), "#66d9e8", "#1c3d47"),
                    ("pad_width", 12, "Width", -100.0, 100.0, 5.0,   0.0, True,  lambda v: "%+d%%" % round(v), "#9775fa", "#2f224f"),
                ),
            )

            for r_i, pair in enumerate(slider_rows):
                ry0 = body_y + r_i * (row_h + gap)
                ry1 = ry0 + row_h
                for c_i, (sym, idx, lbl, vmin, vmax, step, dflt, bip, fmt_fn, col_hi, col_fill) in enumerate(pair):
                    sx0 = ix0 + c_i * (col_w + col_gap)
                    sx1 = sx0 + col_w
                    v = _val(sym, idx, dflt)
                    active = (abs(v) > 0.5) if bip else (v > vmin + 0.5)
                    c.create_rectangle(sx0, ry0, sx1, ry1, fill="#1a1c24",
                                       outline=col_hi if active else "#383844", width=1)
                    if bip:
                        mid_x = (sx0 + sx1) * 0.5
                        c.create_line(mid_x, ry0 + 2, mid_x, ry1 - 2, fill="#3a3f50", width=1)
                        if abs(v) > 0.5:
                            frac = max(-1.0, min(1.0, v / max(1.0, vmax)))
                            fx = mid_x + frac * (max(1.0, sx1 - sx0 - 2) * 0.5)
                            xa, xb = (mid_x, fx) if fx >= mid_x else (fx, mid_x)
                            c.create_rectangle(xa, ry0 + 1, xb, ry1 - 1, fill=col_fill, outline="")
                            c.create_rectangle(xa, ry0 + 1, xb, ry0 + 4, fill=col_hi, width=0)
                    else:
                        frac = max(0.0, min(1.0, (v - vmin) / max(1.0, vmax - vmin)))
                        if frac > 0.005:
                            fx = sx0 + 1 + frac * max(1.0, sx1 - sx0 - 2)
                            c.create_rectangle(sx0 + 1, ry0 + 1, fx, ry1 - 1, fill=col_fill, outline="")
                            c.create_rectangle(sx0 + 1, ry0 + 1, fx, ry0 + 4, fill=col_hi, width=0)
                    c.create_text((sx0 + sx1) / 2, (ry0 + ry1) / 2 + 1,
                                  text="%s: %s" % (lbl, fmt_fn(v)),
                                  fill="#f0f6fc" if active else "#9a9aaa", font=(fam, fsmall, "bold"))
                    self.hit_boxes.append((sx0, ry0, sx1, ry1, "sample_slider", (sym, vmin, vmax, step, bip, sx0, sx1)))

            # Row 7: Mode (1-Shot | Gate | Rev | RevG | Loop) + Choke (Off | 1 | 2 | 3 | 4)
            r7y0 = body_y + 6 * (row_h + gap)
            r7y1 = r7y0 + row_h
            mode_w = iw * 0.58
            mx0, mx1 = ix0, ix0 + mode_w
            mode_v = max(0, min(4, int(round(_val("pad_mode", 6, 0.0)))))
            mode_names = ("1-Shot", "Gate", "Rev", "RevG", "Loop")
            mw_seg = (mx1 - mx0) / 5.0
            c.create_rectangle(mx0, r7y0, mx1, r7y1, fill="#1a1c24", outline=pad_col, width=1)
            for mi, mname in enumerate(mode_names):
                ax0 = mx0 + mi * mw_seg
                ax1 = ax0 + mw_seg
                if mi == mode_v:
                    c.create_rectangle(ax0 + 1, r7y0 + 1, ax1 - 1, r7y1 - 1, fill="#22384c", outline="")
                    c.create_rectangle(ax0 + 1, r7y0 + 1, ax1 - 1, r7y0 + 4, fill=pad_col, width=0)
                if mi > 0:
                    c.create_line(ax0, r7y0 + 3, ax0, r7y1 - 3, fill="#2e3240", width=1)
                c.create_text((ax0 + ax1) / 2, (r7y0 + r7y1) / 2 + 1, text=mname,
                              fill="#ffffff" if mi == mode_v else "#8c92a4",
                              font=(fam, max(7, fsmall - 1), "bold" if mi == mode_v else "normal"))
            self.hit_boxes.append((mx0, r7y0, mx1, r7y1, "sample_mode_slider", (mx0, mx1)))

            kx0, kx1 = mx1 + col_gap, ix1
            choke_v = max(0, min(4, int(round(_val("pad_choke", 11, 0.0)))))
            choke_names = ("Ch:Off", "1", "2", "3", "4")
            kw_seg = (kx1 - kx0) / 5.0
            c.create_rectangle(kx0, r7y0, kx1, r7y1, fill="#1a1c24", outline="#66d9e8" if choke_v > 0 else "#383844", width=1)
            for ki, kname in enumerate(choke_names):
                ax0 = kx0 + ki * kw_seg
                ax1 = ax0 + kw_seg
                if ki == choke_v:
                    c.create_rectangle(ax0 + 1, r7y0 + 1, ax1 - 1, r7y1 - 1, fill="#1c3d47", outline="")
                    c.create_rectangle(ax0 + 1, r7y0 + 1, ax1 - 1, r7y0 + 4, fill="#66d9e8", width=0)
                if ki > 0:
                    c.create_line(ax0, r7y0 + 3, ax0, r7y1 - 3, fill="#2e3240", width=1)
                c.create_text((ax0 + ax1) / 2, (r7y0 + r7y1) / 2 + 1, text=kname,
                              fill="#66d9e8" if (ki == choke_v and ki > 0) else ("#ffffff" if ki == choke_v else "#8c92a4"),
                              font=(fam, max(7, fsmall - 1), "bold" if ki == choke_v else "normal"))
            self.hit_boxes.append((kx0, r7y0, kx1, r7y1, "sample_choke_slider", (kx0, kx1)))

        # Absorb taps inside popup; tapping outside closes popup
        self.hit_boxes.append((px0, py0, px1, py1, "sample_popup_nop", None))
        self.hit_boxes.append((0, 0, w, h, "sample_popup_close", None))

    def _draw_sample_tools_body(self, c, ix0, ix1, body_y, body_h, p, pinfo, pad_col, fam, fs, fsmall):
        iw = max(120, ix1 - ix0)
        y = body_y
        # 1. Auto-Slice waveform preview + slice markers
        wh = max(46, int(body_h * 0.25))
        c.create_rectangle(ix0, y, ix1, y + wh, fill="#111116", outline="#383844")
        path = self._pad_path(p)
        slices = self._compute_slices(path, self.slice_mode, self.slice_sens) if path else []
        if path:
            self._ensure_peaks(path)
            peaks = self.peak_cache.get(path)
            if peaks:
                n = len(peaks)
                mid = y + wh * 0.5
                amp = wh * 0.42
                sx = (iw - 4) / float(max(1, n))
                for i, (mn, mx) in enumerate(peaks):
                    xx = ix0 + 2 + i * sx
                    c.create_line(xx, mid - mx * amp, xx, mid - mn * amp, fill=COL_WAVE)
            for (sp, _ep) in slices:
                mx = ix0 + 2 + (sp / 100.0) * (iw - 4)
                c.create_line(mx, y + 1, mx, y + wh - 1, fill=COL_PEND, width=1)
        else:
            c.create_text((ix0 + ix1) / 2, y + wh / 2, text="Load sample to Auto-Slice", fill=COL_DIM, font=(fam, fsmall))
        y += wh + 6

        rh = max(26, int((body_h - wh - 24) / 4.0))
        # 2. Slice mode selector + Sensitivity / From P1 + Choke + Apply Slice
        sm_w = iw * 0.46
        sm_names = ("4 Grid", "8 Grid", "16 Grid", "Trans")
        seg_w = sm_w / 4.0
        c.create_rectangle(ix0, y, ix0 + sm_w, y + rh, fill="#1a1c24", outline=COL_PEND, width=1)
        for mi, mnm in enumerate(sm_names):
            ax0 = ix0 + mi * seg_w
            ax1 = ax0 + seg_w
            if mi == self.slice_mode:
                c.create_rectangle(ax0 + 1, y + 1, ax1 - 1, y + rh - 1, fill="#3d3318", outline="")
                c.create_rectangle(ax0 + 1, y + 1, ax1 - 1, y + 4, fill=COL_PEND, width=0)
            if mi > 0:
                c.create_line(ax0, y + 3, ax0, y + rh - 3, fill="#2e3240", width=1)
            c.create_text((ax0 + ax1) / 2, y + rh / 2 + 1, text=mnm,
                          fill="#ffffff" if mi == self.slice_mode else "#8c92a4",
                          font=(fam, max(7, fsmall - 1), "bold" if mi == self.slice_mode else "normal"))
        self.hit_boxes.append((ix0, y, ix0 + sm_w, y + rh, "slice_mode", (ix0, ix0 + sm_w)))

        rx = ix0 + sm_w + 5
        rem = ix1 - rx
        bw_a = rem * 0.36
        bw_c = rem * 0.28
        if self.slice_mode == 3:
            # Sensitivity slider
            c.create_rectangle(rx, y, rx + bw_a, y + rh, fill="#1a1c24", outline="#ffd43b", width=1)
            fx = rx + 1 + (self.slice_sens / 100.0) * max(1.0, bw_a - 2)
            c.create_rectangle(rx + 1, y + 1, fx, y + rh - 1, fill="#3d3318", outline="")
            c.create_text(rx + bw_a / 2, y + rh / 2, text="Sens %d%%" % self.slice_sens, fill="#fff3bf", font=(fam, max(7, fsmall - 1), "bold"))
            self.hit_boxes.append((rx, y, rx + bw_a, y + rh, "slice_sens", (rx, rx + bw_a)))
        else:
            self._add_btn(c, rx, y, rx + bw_a, y + rh,
                          "From P1" if self.slice_from_p1 else ("From P%d" % (p + 1)),
                          "#232838", "#e0e4f0", fam, max(7, fsmall - 1), "slice_from_toggle", None)
        rx2 = rx + bw_a + 4
        self._add_btn(c, rx2, y, rx2 + bw_c, y + rh,
                      "Choke 1" if self.slice_choke else "No Choke",
                      "#1c3d47" if self.slice_choke else COL_PANEL,
                      "#66d9e8" if self.slice_choke else "#8c92a4", fam, max(7, fsmall - 1), "slice_choke_toggle", None,
                      outline="#66d9e8" if self.slice_choke else "#383844")
        rx3 = rx2 + bw_c + 4
        self._add_btn(c, rx3, y, ix1, y + rh, "Slice (%d)" % len(slices),
                      "#e67700", "#000000", fam, fsmall, "slice_apply", p, outline=COL_PEND)
        y += rh + 8

        # 3. Pad Link (Layer / Round-Robin)
        lpad = int(pinfo[38]) if (pinfo and len(pinfo) > 39) else 0
        lmode = int(pinfo[39]) if (pinfo and len(pinfo) > 39) else 0
        lm_w = iw * 0.34
        self._add_btn(c, ix0, y, ix0 + lm_w, y + rh,
                      "Link: Round-Robin" if lmode else "Link: Layer",
                      "#2b2248" if lpad > 0 else COL_PANEL,
                      "#d0bfff" if lpad > 0 else "#b0b4c4", fam, fsmall, "pad_link_mode", (p, 1 - lmode),
                      outline="#9775fa" if lpad > 0 else "#3c3f50")
        lt_x0 = ix0 + lm_w + 6
        c.create_rectangle(lt_x0, y, ix1, y + rh, fill="#1a1c24", outline="#9775fa" if lpad > 0 else "#383844", width=1)
        if lpad > 0:
            fx = lt_x0 + 1 + (lpad / 16.0) * max(1.0, (ix1 - lt_x0 - 2))
            c.create_rectangle(lt_x0 + 1, y + 1, fx, y + rh - 1, fill="#2f224f", outline="")
            c.create_rectangle(lt_x0 + 1, y + 1, fx, y + 4, fill="#9775fa", width=0)
        lt_txt = ("Target: Pad %d (%s)" % (lpad, short_sample_name(self._pad_path(lpad - 1), lpad - 1))) if lpad > 0 else "Target: Off (drag to link pad)"
        c.create_text((lt_x0 + ix1) / 2, y + rh / 2 + 1, text=lt_txt,
                      fill="#e5dbff" if lpad > 0 else "#9090a4", font=(fam, fsmall, "bold"))
        self.hit_boxes.append((lt_x0, y, ix1, y + rh, "pad_link_target", (p, lt_x0, ix1)))
        y += rh + 8

        # 4. Copy / Swap Pad (toggle + 16 quick pad buttons)
        op_w = iw * 0.24
        self._add_btn(c, ix0, y, ix0 + op_w, y + rh,
                      "Copy P%d ->" % (p + 1) if self.pad_tool_op == 0 else "Swap P%d <-> " % (p + 1),
                      "#1c3d47" if self.pad_tool_op == 0 else "#4a2c14",
                      "#66d9e8" if self.pad_tool_op == 0 else "#ffa94d",
                      fam, fsmall, "pad_tool_op_toggle", None,
                      outline="#66d9e8" if self.pad_tool_op == 0 else "#ffa94d")
        gx0 = ix0 + op_w + 6
        gw16 = max(80.0, ix1 - gx0)
        pw16 = gw16 / 16.0
        for dst in range(EP_PADS):
            bx0 = gx0 + dst * pw16
            bx1 = bx0 + pw16 - 2
            is_self = (dst == p)
            bg = pad_col if is_self else "#222634"
            fg = "#000000" if is_self else "#e0e4f0"
            self._add_btn(c, bx0, y, bx1, y + rh, str(dst + 1), bg, fg, fam, max(6, fsmall - 1),
                          "pad_copy_swap_dst", (p, dst), outline=pad_col if is_self else "#3a3f50")

    def _draw_seq_tab(self, c, w, h, top_h, fam, fs):
        pad_m = 4
        y = top_h + pad_m
        bh = max(24, fs * 2 + 4)
        fsmall = max(7, fs - 2)

        scene = int(round(self._mon("out_seq_scene", 0)))
        pend = int(round(self._mon("out_seq_pend", -1)))
        cur_step = int(round(self._mon("out_seq_step", -1)))
        smode = self.shm_hdr[6] if self.shm_hdr else int(round(self._zval("seq_mode", 0)))
        bpm = int(round(self.shm_hdr[10])) if self.shm_hdr else int(round(self._zval("seq_bpm", 120)))
        quant = int(self.shm_hdr[12]) if self.shm_hdr else 100
        if quant < 0 or quant > 100:
            quant = 100
        ps, pe = self._pad_range_get(scene, self.sel_pad)

        # Row 1: 5 Large Scene buttons (A..E) + Vel + Quantize + Copy + Clear + ->ZSeq
        r1_h = max(40, fs * 3 + 12)
        scenes_w = (w - 2 * pad_m) * 0.46
        sc_w = scenes_w / float(EP_UI_SCENES)
        for s in range(EP_UI_SCENES):
            x0 = pad_m + s * sc_w
            x1 = x0 + sc_w - 3
            if self.copy_src == s:
                bg, fg = "#e64980", "#ffffff"
            elif s == scene:
                bg, fg = COL_ACCENT, "#000000"
            elif s == pend:
                bg, fg = COL_PEND, "#000000"
            else:
                bg, fg = COL_PANEL, "#f0f0f5"
            self._add_btn(c, x0, y, x1, y + r1_h, SCENE_NAMES[s], bg, fg, fam, fs + 1, "seq_scene", s)

        tools_x = pad_m + scenes_w + 2
        tw5 = (w - pad_m - tools_x) / 5.0
        # Horizontal Quantize strength slider (0..100%)
        qx0, qx1 = tools_x, tools_x + tw5 - 3
        c.create_rectangle(qx0, y, qx1, y + r1_h, fill="#1a1c24", outline="#3c4a46" if quant > 0 else "#383844", width=1)
        if quant > 0:
            q_fill_x = qx0 + 1 + (quant / 100.0) * max(1.0, (qx1 - qx0 - 2))
            c.create_rectangle(qx0 + 1, y + 1, q_fill_x, y + r1_h - 1, fill="#24483d", outline="")
            c.create_rectangle(qx0 + 1, y + 1, q_fill_x, y + 4, fill="#63e6be", width=0)
        q_lbl = "Q%d" % quant if quant > 0 else "Q Off"
        c.create_text((qx0 + qx1) / 2, y + r1_h / 2, text=q_lbl,
                      fill="#63e6be" if quant > 0 else "#9a9aaa", font=(fam, fsmall, "bold"))
        self.hit_boxes.append((qx0, y, qx1, y + r1_h, "quant_slider", (qx0, qx1)))
        tools_active = bool(self.shm_hdr and len(self.shm_hdr) >= 30 and (self.shm_hdr[20] or self.shm_hdr[21] or self.shm_hdr[25]))
        self._add_btn(c, tools_x + tw5, y, tools_x + 2 * tw5 - 3, y + r1_h,
                      "Tools", "#2b2248" if tools_active else COL_PANEL,
                      "#d0bfff" if tools_active else "#e0e0f0", fam, fsmall, "seq_tools_open", None,
                      outline="#9775fa" if tools_active else "#444450")
        self._add_btn(c, tools_x + 2 * tw5, y, tools_x + 3 * tw5 - 3, y + r1_h,
                      "Pick" if self.copy_src >= 0 else "Copy",
                      "#e64980" if self.copy_src >= 0 else COL_PANEL, "#ffffff", fam, fsmall, "seq_copy", scene)
        self._add_btn(c, tools_x + 3 * tw5, y, tools_x + 4 * tw5 - 3, y + r1_h,
                      "Clr", COL_PANEL, "#ff8787", fam, fsmall, "seq_clr_pad", (scene, self.sel_pad))
        self._add_btn(c, tools_x + 4 * tw5, y, w - pad_m, y + r1_h,
                      "->ZSeq", "#1d4034", "#63e6be", fam, fsmall, "export_zseq", scene, outline="#38d9a9")
        y += r1_h + 4

        # Row 2: Play/Stop + Overdub + BPM + Horizontal Steps Slider (Beg..End 1..32)
        r2_h = max(38, fs * 3 + 10)
        ctrl_w = (w - 2 * pad_m) * 0.44
        b3w = ctrl_w / 3.0
        self._add_btn(c, pad_m, y, pad_m + b3w - 3, y + r2_h,
                      "Stop" if smode >= 1 else "Play",
                      COL_PLAY if smode == 1 else COL_PANEL,
                      "#000000" if smode == 1 else "#e0e0e0", fam, fs, "seq_play", 0 if smode >= 1 else 1)
        self._add_btn(c, pad_m + b3w, y, pad_m + 2 * b3w - 3, y + r2_h,
                      "Rec" if smode == 2 else "Dub",
                      COL_REC if smode == 2 else COL_PANEL,
                      "#ffffff" if smode == 2 else "#e0e0e0", fam, fs, "seq_rec", 1 if smode == 2 else 2)
        self._add_btn(c, pad_m + 2 * b3w, y, pad_m + ctrl_w - 4, y + r2_h,
                      "%d BPM" % bpm, "#263040", "#ffffff", fam, fsmall, "bpm_drag", bpm)

        # Horizontal Steps Slider (Beg..End 1..32) on the right half of Row 2
        sl_x0 = pad_m + ctrl_w + 2
        sl_x1 = w - pad_m
        sl_w = max(40.0, sl_x1 - sl_x0)
        pad_col = ROW_COLORS[(self.sel_pad // 4) & 3]
        c.create_rectangle(sl_x0, y, sl_x1, y + r2_h, fill="#1a1c24", outline="#3c4252", width=1)
        # Quarter-note ticks at 4, 8, 12, 16, 20, 24, 28
        for tick in range(4, EP_STEPS, 4):
            tx = sl_x0 + (tick / float(EP_STEPS)) * sl_w
            c.create_line(tx, y + 2, tx, y + r2_h - 2, fill="#2b303e", width=1)
        # Highlighted active range [ps .. pe]
        rx_a = sl_x0 + (ps / float(EP_STEPS)) * sl_w
        rx_b = sl_x0 + (pe / float(EP_STEPS)) * sl_w
        c.create_rectangle(rx_a, y + 2, rx_b, y + r2_h - 2, fill="#22384c", outline="")
        c.create_rectangle(rx_a, y + 2, rx_b, y + 5, fill=pad_col, width=0)
        # Left (Beg) & Right (End) slider thumbs
        c.create_rectangle(rx_a, y + 1, min(sl_x1, rx_a + 4), y + r2_h - 1, fill="#ffd43b", width=0)
        c.create_rectangle(max(sl_x0, rx_b - 4), y + 1, rx_b, y + r2_h - 1, fill="#74c0fc", width=0)
        sl_txt = "P%d: %d..%d (%d st)" % (self.sel_pad + 1, ps + 1, pe, pe - ps)
        c.create_text((sl_x0 + sl_x1) / 2, y + r2_h / 2, text=sl_txt,
                      fill="#f0f6fc", font=(fam, fsmall, "bold"))
        self.hit_boxes.append((sl_x0, y, sl_x1, y + r2_h, "steps_slider", (scene, self.sel_pad, ps, pe, sl_x0, sl_x1)))
        y += r2_h + 6

        # Middle: 32 Styled LED Step Tiles (4 rows of 8, uniform spacing) for selected pad
        # Short tap = toggle on/off, Drag up/down = Velocity, Long tap = Velocity / Chance / Roll popup
        strip_h = max(24, fs * 2 + 4)
        grid_h = max(72, h - y - strip_h - pad_m - 6)
        sw = (w - 2 * pad_m) / 8.0
        sh = grid_h / 4.0

        for st in range(EP_STEPS):
            col_i = st % 8
            row_i = st // 8
            sx0 = pad_m + col_i * sw + 2
            sy0 = y + row_i * sh + 2
            sx1 = pad_m + (col_i + 1) * sw - 2
            sy1 = y + (row_i + 1) * sh - 2
            th = max(8.0, sy1 - sy0)

            in_len = (ps <= st < pe)
            vel, roll, chance, micro, po, fo = self._step_get_plock(scene, self.sel_pad, st)
            has_plock = (vel > 0 and (po != 0 or fo != 0 or micro != 0))
            is_dragging_step = bool(
                self.step_drag_override and self.step_drag_override[:3] == (scene, self.sel_pad, st)
            )
            if is_dragging_step:
                vel = self.step_drag_override[3]
                if vel == 0:
                    roll = 0
            is_cur = (in_len and st == cur_step)
            is_beat = ((st - ps) % 4 == 0)

            if not in_len:
                c.create_rectangle(sx0, sy0, sx1, sy1, fill="#15161c", outline="#242630", width=1)
                c.create_text(sx0 + 5, sy0 + 3, anchor="nw", text="%d" % (st + 1),
                              fill="#343744", font=(fam, max(7, fsmall - 1)))
                continue

            # Tile body with subtle beat shading & bright cursor highlight
            if is_dragging_step:
                bg = "#2b3648"
                out_c = "#ffffff"
                out_w = 2
            elif is_cur:
                bg = "#2d3a4d"
                out_c = "#ffffff"
                out_w = 2
            else:
                bg = "#282b36" if is_beat else "#1e2028"
                out_c = pad_col if vel > 0 else ("#43485a" if is_beat else "#303442")
                out_w = 1
            c.create_rectangle(sx0, sy0, sx1, sy1, fill=bg, outline=out_c, width=out_w)

            # Velocity fill bar + bright top cap
            step_col = "#ff922b" if vel >= 120 else pad_col
            if vel > 0:
                frac = vel / 127.0
                bar_top = sy1 - 2 - frac * max(2.0, th - 8.0)
                c.create_rectangle(sx0 + 2, bar_top, sx1 - 2, sy1 - 2, fill="#243242" if vel < 120 else "#4a2c14", width=0)
                c.create_line(sx0 + 2, bar_top, sx1 - 2, bar_top, fill=step_col, width=2)

            # Top LED indicator strip
            led_col = "#ffffff" if is_cur else (step_col if vel > 0 else ("#4a5064" if is_beat else "#2b2f3d"))
            c.create_rectangle(sx0 + 3, sy0 + 2, sx1 - 3, sy0 + 5, fill=led_col, width=0)

            # Step number (top-left) + Chance badge (bottom-left) + Ratchet badge (bottom-right)
            c.create_text(sx0 + 5, sy0 + 7, anchor="nw", text="%d" % (st + 1),
                          fill="#ffffff" if (vel > 0 or is_cur) else "#8d94a6",
                          font=(fam, max(7, fsmall - 1), "bold" if (vel > 0 or is_cur) else "normal"))
            if is_dragging_step:
                c.create_text((sx0 + sx1) / 2, (sy0 + sy1) / 2 + 2,
                              text="%d" % vel if vel > 0 else "Off",
                              fill="#ffffff", font=(fam, fs, "bold"))
            if vel > 0 and chance < 100:
                c.create_text(sx0 + 4, sy1 - 3, anchor="sw", text="%d%%" % chance,
                              fill="#63e6be", font=(fam, max(6, fsmall - 1), "bold"))
            if vel > 0 and roll > 0:
                c.create_text(sx1 - 4, sy1 - 3, anchor="se", text="x%d" % (roll + 1),
                              fill="#ffd43b", font=(fam, max(7, fsmall - 1), "bold"))
            if has_plock:
                c.create_oval(sx1 - 9, sy0 + 8, sx1 - 3, sy0 + 14, fill="#da77f2", outline="")

            self.hit_boxes.append((sx0, sy0, sx1, sy1, "step", (scene, self.sel_pad, st)))

        # Bottom: 16-Pad selector strip (tap = select lane, long-press = Mute/Unmute)
        py0 = h - pad_m - strip_h
        pw = (w - 2 * pad_m) / float(EP_PADS)
        for p in range(EP_PADS):
            px0 = pad_m + p * pw
            px1 = px0 + pw - 2
            p_s, p_e = self._pad_range_get(scene, p)
            has_steps = any(self._step_get(scene, p, st)[0] > 0 for st in range(p_s, p_e))
            pinfo = self.pads_data[p] if self.pads_data else None
            mute = pinfo[13] if pinfo else 0
            solo = pinfo[14] if pinfo else 0
            lvl = self._mon("out_pad_lvl%d" % (p + 1), 0.0)
            is_sel = (p == self.sel_pad)
            accent = ROW_COLORS[(p // 4) & 3]

            if mute:
                bg = "#3b1d22" if not is_sel else "#5c2630"
            elif is_sel:
                bg = COL_PAD_SEL
            elif lvl > 0.06:
                bg = "#3b4866"
            elif has_steps:
                bg = "#253445"
            else:
                bg = COL_PANEL

            out_c = "#ffffff" if is_sel else ("#ffd43b" if solo else ("#fa5252" if mute else "#3a3a44"))
            c.create_rectangle(px0, py0, px1, py0 + strip_h, fill=bg, outline=out_c, width=2 if is_sel else 1)
            c.create_rectangle(px0 + 1, py0 + 1, px1 - 1, py0 + 4,
                               fill="#555560" if mute else accent, width=0)
            lbl = "M" if mute else ("S" if solo else str(p + 1))
            fg = "#ff8787" if mute else ("#ffd43b" if solo else ("#ffffff" if (is_sel or has_steps) else "#787888"))
            c.create_text((px0 + px1) / 2, py0 + strip_h / 2 + 2, text=lbl,
                          fill=fg, font=(fam, max(6, fsmall - 1), "bold" if (is_sel or mute or solo) else "normal"))
            self.hit_boxes.append((px0, py0, px1, py0 + strip_h, "seq_sel_pad", p))

        # Popup overlays when a step tile is long-pressed or Seq Tools is open
        if self.step_popup is not None:
            self._draw_step_popup(c, w, h, top_h, fam, fs)
        elif self.seq_tools_popup:
            self._draw_seq_tools_popup(c, w, h, top_h, fam, fs, scene, ps, pe)

    def _draw_step_popup(self, c, w, h, top_h, fam, fs):
        self.hit_boxes = []
        sc, p, st = self.step_popup
        if self.step_popup_vals is not None:
            s_vel, s_chance, s_roll, s_micro, s_pitch, s_filt = self.step_popup_vals
        else:
            s_vel, s_roll, s_chance, s_micro, s_pitch, s_filt = self._step_get_plock(sc, p, st)
        pad_col = ROW_COLORS[(p // 4) & 3]
        fsmall = max(7, fs - 1)

        pw = max(270, min(int(w * 0.72), 520))
        ph = max(210, min(int((h - top_h) * 0.76), 286))
        px0 = (w - pw) // 2
        py0 = top_h + max(6, (h - top_h - ph) // 2)
        px1, py1 = px0 + pw, py0 + ph

        # Dimmed backdrop + compact Eka Pads modal panel
        c.create_rectangle(0, top_h, w, h, fill="#0b0b0f", stipple="gray50", outline="")
        c.create_rectangle(px0, py0, px1, py1, fill=COL_BG, outline=pad_col, width=2)

        pad_i = 8
        ix0, ix1 = px0 + pad_i, px1 - pad_i
        iw = max(80, ix1 - ix0)

        # Header bar
        hdr_h = max(24, fs * 2 + 4)
        c.create_rectangle(px0 + 1, py0 + 1, px1 - 1, py0 + hdr_h, fill=COL_PANEL, outline="")
        c.create_rectangle(px0 + 1, py0 + 1, px1 - 1, py0 + 4, fill=pad_col, width=0)
        pname = short_sample_name(self._pad_path(p), p)
        c.create_text(ix0 + 2, py0 + hdr_h / 2 + 1, anchor="w",
                      text="Step %d — P%d %s" % (st + 1, p + 1, pname),
                      fill=pad_col, font=(fam, fs, "bold"))
        cw = max(48, int(iw * 0.16))
        self._add_btn(c, ix1 - cw, py0 + 4, ix1, py0 + hdr_h - 3, "Close",
                      "#2a2a34", "#f0f0f5", fam, fsmall, "step_popup_close", None, outline="#444452")

        # 5 compact rows: Velocity, Chance, Roll, Pitch|Filter p-locks, Microtiming
        body_y = py0 + hdr_h + 6
        body_h = max(140, py1 - body_y - 6)
        gap = 5
        sl_h = max(24.0, (body_h - 4 * gap) / 5.0)

        # Row 1: Velocity (0..127)
        y1 = body_y
        v_col = "#ff922b" if s_vel >= 120 else pad_col
        c.create_rectangle(ix0, y1, ix1, y1 + sl_h, fill="#1a1c24",
                           outline=v_col if s_vel > 0 else "#4a5568", width=1)
        if s_vel > 0:
            vf_x = ix0 + 1 + (s_vel / 127.0) * max(1.0, (iw - 2))
            c.create_rectangle(ix0 + 1, y1 + 1, vf_x, y1 + sl_h - 1,
                               fill="#4a2c14" if s_vel >= 120 else "#22384c", outline="")
            c.create_rectangle(ix0 + 1, y1 + 1, vf_x, y1 + 4, fill=v_col, width=0)
        v_txt = "Velocity: %d" % s_vel if s_vel > 0 else "Velocity: Off"
        c.create_text((ix0 + ix1) / 2, y1 + sl_h / 2 + 1, text=v_txt,
                      fill="#ffffff" if s_vel > 0 else "#9a9aaa", font=(fam, fsmall, "bold"))
        self.hit_boxes.append((ix0, y1, ix1, y1 + sl_h, "step_vel_slider", (sc, p, st, ix0, ix1)))

        # Row 2: Chance (10..100%)
        y2 = y1 + sl_h + gap
        c.create_rectangle(ix0, y2, ix1, y2 + sl_h, fill="#1a1c24", outline="#63e6be", width=1)
        for tick in (25, 50, 75):
            tx = ix0 + ((tick - 10.0) / 90.0) * iw
            c.create_line(tx, y2 + 3, tx, y2 + sl_h - 3, fill="#2b3634", width=1)
        cf_x = ix0 + 1 + (max(0.0, min(1.0, (s_chance - 10.0) / 90.0))) * max(1.0, (iw - 2))
        c.create_rectangle(ix0 + 1, y2 + 1, cf_x, y2 + sl_h - 1, fill="#1e453b", outline="")
        c.create_rectangle(ix0 + 1, y2 + 1, cf_x, y2 + 4, fill="#63e6be", width=0)
        c.create_text((ix0 + ix1) / 2, y2 + sl_h / 2 + 1, text="Chance: %d%%" % s_chance,
                      fill="#e6fcf5", font=(fam, fsmall, "bold"))
        self.hit_boxes.append((ix0, y2, ix1, y2 + sl_h, "step_chance_slider", (sc, p, st, ix0, ix1)))

        # Row 3: Roll (1x | 2x | 3x | 4x)
        y3 = y2 + sl_h + gap
        rw = max(1.0, iw / 4.0)
        c.create_rectangle(ix0, y3, ix1, y3 + sl_h, fill="#1a1c24", outline="#4a5568", width=1)
        for ri in range(4):
            seg_x0 = ix0 + ri * rw
            seg_x1 = ix0 + (ri + 1) * rw
            is_r = (s_vel > 0 and s_roll == ri)
            if is_r:
                c.create_rectangle(seg_x0 + 1, y3 + 1, seg_x1 - 1, y3 + sl_h - 1, fill="#3b3218", outline="")
                c.create_rectangle(seg_x0 + 1, y3 + 1, seg_x1 - 1, y3 + 4, fill="#ffd43b", width=0)
            if ri > 0:
                c.create_line(seg_x0, y3 + 3, seg_x0, y3 + sl_h - 3, fill="#2e3240", width=1)
            c.create_text((seg_x0 + seg_x1) / 2, y3 + sl_h / 2 + 1,
                          text=("Roll 1x" if ri == 0 else "%dx" % (ri + 1)),
                          fill="#ffd43b" if is_r else "#8c92a4",
                          font=(fam, fsmall, "bold" if is_r else "normal"))
        self.hit_boxes.append((ix0, y3, ix1, y3 + sl_h, "step_roll_slider", (sc, p, st, ix0, ix1)))

        # Row 4: Pitch Lock (-24..+24 st) | Filter Lock (-100..+100%)
        y4 = y3 + sl_h + gap
        half_w = (iw - 6) / 2.0
        for ci, (lbl, val, vmax, act_name, col_hi, col_fill, fmt) in enumerate((
            ("Pitch", s_pitch, 24.0, "step_pitch_slider", "#74c0fc", "#1e3a52", "%+dst" % s_pitch if s_pitch else "0st"),
            ("Filter", s_filt, 100.0, "step_filt_slider", "#da77f2", "#3b1f47", "%+d%%" % s_filt if s_filt else "0%"),
        )):
            bx0 = ix0 + ci * (half_w + 6)
            bx1 = bx0 + half_w
            c.create_rectangle(bx0, y4, bx1, y4 + sl_h, fill="#1a1c24",
                               outline=col_hi if val != 0 else "#383844", width=1)
            mid_x = (bx0 + bx1) * 0.5
            c.create_line(mid_x, y4 + 2, mid_x, y4 + sl_h - 2, fill="#3a3f50", width=1)
            if val != 0:
                fx = mid_x + (max(-1.0, min(1.0, val / vmax))) * (max(1.0, half_w - 2) * 0.5)
                xa, xb = (mid_x, fx) if fx >= mid_x else (fx, mid_x)
                c.create_rectangle(xa, y4 + 1, xb, y4 + sl_h - 1, fill=col_fill, outline="")
                c.create_rectangle(xa, y4 + 1, xb, y4 + 4, fill=col_hi, width=0)
            c.create_text((bx0 + bx1) / 2, y4 + sl_h / 2 + 1, text="%s: %s" % (lbl, fmt),
                          fill="#f0f6fc" if val != 0 else "#9a9aaa", font=(fam, fsmall, "bold"))
            self.hit_boxes.append((bx0, y4, bx1, y4 + sl_h, act_name, (sc, p, st, bx0, bx1)))

        # Row 5: Microtiming (-30..+30 / 64 step)
        y5 = y4 + sl_h + gap
        c.create_rectangle(ix0, y5, ix1, y5 + sl_h, fill="#1a1c24",
                           outline="#66d9e8" if s_micro != 0 else "#383844", width=1)
        mid_x = (ix0 + ix1) * 0.5
        c.create_line(mid_x, y5 + 2, mid_x, y5 + sl_h - 2, fill="#3a3f50", width=1)
        if s_micro != 0:
            fx = mid_x + (max(-1.0, min(1.0, s_micro / 30.0))) * (max(1.0, iw - 2) * 0.5)
            xa, xb = (mid_x, fx) if fx >= mid_x else (fx, mid_x)
            c.create_rectangle(xa, y5 + 1, xb, y5 + sl_h - 1, fill="#1c3d47", outline="")
            c.create_rectangle(xa, y5 + 1, xb, y5 + 4, fill="#66d9e8", width=0)
        mpct = int(round(s_micro * 100.0 / 64.0))
        m_txt = "Microtiming: %+d%%" % mpct if s_micro != 0 else "Microtiming: On Grid"
        c.create_text((ix0 + ix1) / 2, y5 + sl_h / 2 + 1, text=m_txt,
                      fill="#e3fafc" if s_micro != 0 else "#9a9aaa", font=(fam, fsmall, "bold"))
        self.hit_boxes.append((ix0, y5, ix1, y5 + sl_h, "step_micro_slider", (sc, p, st, ix0, ix1)))

        # Absorb taps inside popup background; tapping outside closes popup
        self.hit_boxes.append((px0, py0, px1, py1, "step_popup_nop", None))
        self.hit_boxes.append((0, 0, w, h, "step_popup_close", None))

    def _draw_seq_tools_popup(self, c, w, h, top_h, fam, fs, scene, ps, pe):
        self.hit_boxes = []
        p = self.sel_pad
        plen = max(1, pe - ps)
        pad_col = ROW_COLORS[(p // 4) & 3]
        fsmall = max(7, fs - 1)
        metro = int(self.shm_hdr[20]) if (self.shm_hdr and len(self.shm_hdr) >= 30) else 0
        countin = int(self.shm_hdr[21]) if (self.shm_hdr and len(self.shm_hdr) >= 30) else 0
        undo_av = int(self.shm_hdr[23]) if (self.shm_hdr and len(self.shm_hdr) >= 30) else 0
        ch_on = int(self.shm_hdr[25]) if (self.shm_hdr and len(self.shm_hdr) >= 30) else 0
        ch_len = max(1, min(8, int(self.shm_hdr[26]))) if (self.shm_hdr and len(self.shm_hdr) >= 30) else 4
        ch_pos = int(self.shm_hdr[27]) if (self.shm_hdr and len(self.shm_hdr) >= 30) else 0
        ch_pack = int(self.shm_hdr[29]) if (self.shm_hdr and len(self.shm_hdr) >= 30) else 0x32102100

        pw = max(300, min(int(w * 0.88), 680))
        ph = max(210, min(int((h - top_h) * 0.80), 310))
        px0 = (w - pw) // 2
        py0 = top_h + max(6, (h - top_h - ph) // 2)
        px1, py1 = px0 + pw, py0 + ph

        c.create_rectangle(0, top_h, w, h, fill="#0b0b0f", stipple="gray50", outline="")
        c.create_rectangle(px0, py0, px1, py1, fill=COL_BG, outline="#9775fa", width=2)

        pad_i = 8
        ix0, ix1 = px0 + pad_i, px1 - pad_i
        iw = max(120, ix1 - ix0)

        hdr_h = max(26, fs * 2 + 6)
        c.create_rectangle(px0 + 1, py0 + 1, px1 - 1, py0 + hdr_h, fill=COL_PANEL, outline="")
        c.create_rectangle(px0 + 1, py0 + 1, px1 - 1, py0 + 4, fill="#9775fa", width=0)
        c.create_text(ix0 + 2, py0 + hdr_h / 2 + 1, anchor="w",
                      text="Seq Tools — P%d (%d st)  Sc %s" % (p + 1, plen, SCENE_NAMES[scene & 7]),
                      fill="#d0bfff", font=(fam, fs, "bold"))
        bw_c = max(48, int(iw * 0.14))
        bw_u = max(68, int(iw * 0.18))
        self._add_btn(c, ix1 - bw_c - bw_u - 6, py0 + 4, ix1 - bw_c - 6, py0 + hdr_h - 3, "Undo Dub",
                      "#3b2d18" if undo_av else "#1d1e26",
                      "#ffd43b" if undo_av else "#606474", fam, fsmall, "seq_undo", None,
                      outline="#ffd43b" if undo_av else "#343644")
        self._add_btn(c, ix1 - bw_c, py0 + 4, ix1, py0 + hdr_h - 3, "Close",
                      "#2a2a34", "#f0f0f5", fam, fsmall, "seq_tools_close", None, outline="#444452")

        body_y = py0 + hdr_h + 7
        body_h = max(130, py1 - body_y - 7)
        gap = 7
        rh = max(26.0, (body_h - 3 * gap) / 4.0)

        # Row 1: Euclidean Rhythm Generator (Hits + Rot live sliders)
        y1 = body_y
        eh = min(self.euclid_hits, plen)
        er = self.euclid_rot % plen
        w_hits = iw * 0.58
        c.create_rectangle(ix0, y1, ix0 + w_hits, y1 + rh, fill="#1a1c24", outline=pad_col, width=1)
        if eh > 0:
            fx = ix0 + 1 + (eh / float(plen)) * max(1.0, w_hits - 2)
            c.create_rectangle(ix0 + 1, y1 + 1, fx, y1 + rh - 1, fill="#22384c", outline="")
            c.create_rectangle(ix0 + 1, y1 + 1, fx, y1 + 4, fill=pad_col, width=0)
        c.create_text(ix0 + w_hits / 2, y1 + rh / 2 + 1, text="Euclid Hits: %d / %d" % (eh, plen),
                      fill="#f0f6fc", font=(fam, fsmall, "bold"))
        self.hit_boxes.append((ix0, y1, ix0 + w_hits, y1 + rh, "euclid_hits", (p, plen, ix0, ix0 + w_hits)))

        rx0 = ix0 + w_hits + 6
        c.create_rectangle(rx0, y1, ix1, y1 + rh, fill="#1a1c24", outline="#66d9e8", width=1)
        if plen > 1 and er > 0:
            fx = rx0 + 1 + (er / float(plen - 1)) * max(1.0, (ix1 - rx0 - 2))
            c.create_rectangle(rx0 + 1, y1 + 1, fx, y1 + rh - 1, fill="#1c3d47", outline="")
            c.create_rectangle(rx0 + 1, y1 + 1, fx, y1 + 4, fill="#66d9e8", width=0)
        c.create_text((rx0 + ix1) / 2, y1 + rh / 2 + 1, text="Rot: %d" % er,
                      fill="#e3fafc", font=(fam, fsmall, "bold"))
        self.hit_boxes.append((rx0, y1, ix1, y1 + rh, "euclid_rot", (p, plen, rx0, ix1)))

        # Row 2: Shift Pattern Left/Right + Metronome + Count-in
        y2 = y1 + rh + gap
        bw4 = (iw - 15) / 4.0
        self._add_btn(c, ix0, y2, ix0 + bw4, y2 + rh, "< Shift", "#222a38", "#74c0fc", fam, fsmall, "seq_rotate", (p, -1), outline="#4dabf7")
        self._add_btn(c, ix0 + bw4 + 5, y2, ix0 + 2 * bw4 + 5, y2 + rh, "Shift >", "#222a38", "#74c0fc", fam, fsmall, "seq_rotate", (p, 1), outline="#4dabf7")
        self._add_btn(c, ix0 + 2 * bw4 + 10, y2, ix0 + 3 * bw4 + 10, y2 + rh,
                      "Metro: %s" % METRO_NAMES[metro % 3],
                      "#1e453b" if metro > 0 else COL_PANEL,
                      "#63e6be" if metro > 0 else "#b0b4c4", fam, fsmall, "seq_metro", (metro + 1) % 3,
                      outline="#63e6be" if metro > 0 else "#444450")
        self._add_btn(c, ix0 + 3 * bw4 + 15, y2, ix1, y2 + rh,
                      "Count-in: 1B" if countin else "Count-in: Off",
                      "#473b19" if countin else COL_PANEL,
                      "#ffd43b" if countin else "#b0b4c4", fam, fsmall, "seq_countin", 1 - countin,
                      outline="#ffd43b" if countin else "#444450")

        # Row 3: Scene Chain On/Off + Chain Length slider
        y3 = y2 + rh + gap
        cw_on = iw * 0.34
        self._add_btn(c, ix0, y3, ix0 + cw_on, y3 + rh,
                      "Scene Chain: ON" if ch_on else "Scene Chain: Off",
                      "#2f224f" if ch_on else COL_PANEL,
                      "#d0bfff" if ch_on else "#b0b4c4", fam, fsmall, "chain_toggle", 1 - ch_on,
                      outline="#9775fa" if ch_on else "#444450")
        cl_x0 = ix0 + cw_on + 6
        c.create_rectangle(cl_x0, y3, ix1, y3 + rh, fill="#1a1c24", outline="#9775fa" if ch_on else "#383844", width=1)
        fx = cl_x0 + 1 + (ch_len / 8.0) * max(1.0, (ix1 - cl_x0 - 2))
        c.create_rectangle(cl_x0 + 1, y3 + 1, fx, y3 + rh - 1, fill="#2f224f", outline="")
        c.create_rectangle(cl_x0 + 1, y3 + 1, fx, y3 + 4, fill="#9775fa", width=0)
        c.create_text((cl_x0 + ix1) / 2, y3 + rh / 2 + 1, text="Chain Length: %d bars  (long-press A..E = 1-bar Fill)" % ch_len,
                      fill="#e5dbff", font=(fam, max(7, fsmall - 1), "bold"))
        self.hit_boxes.append((cl_x0, y3, ix1, y3 + rh, "chain_len_slider", (cl_x0, ix1)))

        # Row 4: 8 clickable Scene Chain slots
        y4 = y3 + rh + gap
        sw8 = iw / 8.0
        for si in range(8):
            sx0 = ix0 + si * sw8
            sx1 = sx0 + sw8 - 3
            in_ch = (si < ch_len)
            sc_i = (ch_pack >> (si * 4)) & 0xF
            sc_i = sc_i % EP_UI_SCENES
            is_cur = (ch_on and si == ch_pos and in_ch)
            bg = COL_ACCENT if is_cur else ("#25283a" if in_ch else "#16171d")
            fg = "#000000" if is_cur else ("#f0f0f8" if in_ch else "#4a4d5c")
            out_c = "#ffffff" if is_cur else ("#9775fa" if in_ch else "#2a2c38")
            self._add_btn(c, sx0, y4, sx1, y4 + rh, "%d:%s" % (si + 1, SCENE_NAMES[sc_i]),
                          bg, fg, fam, fsmall, "chain_slot", (si, (sc_i + 1) % EP_UI_SCENES), outline=out_c)

        self.hit_boxes.append((px0, py0, px1, py1, "seq_tools_nop", None))
        self.hit_boxes.append((0, 0, w, h, "seq_tools_close", None))

    def _draw_mod_tab(self, c, w, h, top_h, fam, fs):
        pad_m = 4
        y = top_h + pad_m
        fsmall = max(7, fs - 2)
        p = self.sel_pad
        pinfo = self.pads_data[p] if (self.pads_data and len(self.pads_data[p]) >= 36) else None

        # Row 1: 2 Large LFO 1 & LFO 2 buttons (tap to open LFO popup settings)
        r1_h = max(34, fs * 2 + 12)
        half = (w - 3 * pad_m) / 2.0
        lfo_cols = (("#66d9e8", "#1c3d47"), ("#da77f2", "#3b1f47"))
        for li in range(2):
            lx0 = pad_m + li * (half + pad_m)
            lx1 = lx0 + half
            wf = int(pinfo[20 + li]) if pinfo else (0 if li == 0 else 1)
            wave_i = (wf & 7) % len(LFO_WAVES)
            is_sync = bool(wf & 16)
            rate_v = float(pinfo[22 + li]) if pinfo else (1.5 if li == 0 else 4.0)
            col_hi, col_fill = lfo_cols[li]
            if is_sync:
                t = max(0.0, min(1.0, math.log(max(0.05, rate_v) / 0.05) / math.log(400.0)))
                didx = max(0, min(len(LFO_SYNC_DIVS) - 1, int(round(t * (len(LFO_SYNC_DIVS) - 1)))))
                rlbl = LFO_SYNC_DIVS[didx]
            else:
                rlbl = "%.2fHz" % rate_v if rate_v < 1.0 else ("%.1fHz" % rate_v)
            lbl = "LFO %d: %s  %s" % (li + 1, LFO_WAVES[wave_i], rlbl)
            c.create_rectangle(lx0, y, lx1, y + r1_h, fill=col_fill, outline=col_hi, width=2)
            c.create_rectangle(lx0 + 1, y + 1, lx1 - 1, y + 5, fill=col_hi, width=0)
            c.create_text((lx0 + lx1) / 2, y + r1_h / 2 + 2, text=lbl,
                          fill="#f0f6fc", font=(fam, fs, "bold"))
            self.hit_boxes.append((lx0, y, lx1, y + r1_h, "lfo_open", li))
        y += r1_h + 6

        # Middle: 4 Modulation Matrix rows (Source | Dest | Bipolar Amount slider)
        strip_h = max(24, fs * 2 + 4)
        mat_h = max(84, h - y - strip_h - pad_m - 6)
        row_h = (mat_h - 9) / 4.0
        w_avail = w - 2 * pad_m
        src_w = w_avail * 0.23
        dst_w = w_avail * 0.25
        amt_x0 = pad_m + src_w + dst_w + 6
        amt_x1 = w - pad_m

        for sl in range(4):
            ry0 = y + sl * (row_h + 3)
            ry1 = ry0 + row_h
            src_i = int(pinfo[24 + sl]) if pinfo else 0
            dst_i = int(pinfo[28 + sl]) if pinfo else 0
            amt_v = float(pinfo[32 + sl]) if pinfo else 0.0
            active = (src_i > 0 and dst_i > 0 and abs(amt_v) > 0.5)

            s_col = "#74c0fc" if src_i in (1, 2) else ("#66d9e8" if src_i == 3 else ("#da77f2" if src_i == 4 else ("#ffd43b" if src_i == 5 else "#888896")))
            self._add_btn(c, pad_m, ry0, pad_m + src_w - 3, ry1,
                          "%d:%s" % (sl + 1, MOD_SRCS[src_i % len(MOD_SRCS)]),
                          "#232838" if src_i > 0 else COL_PANEL, s_col, fam, fsmall,
                          "mod_src", (sl, p, (src_i + 1) % len(MOD_SRCS), dst_i),
                          outline=s_col if src_i > 0 else "#383844")
            self._add_btn(c, pad_m + src_w, ry0, pad_m + src_w + dst_w - 3, ry1,
                          "->%s" % MOD_DSTS[dst_i % len(MOD_DSTS)],
                          "#232838" if dst_i > 0 else COL_PANEL,
                          "#e6fcf5" if dst_i > 0 else "#888896", fam, fsmall,
                          "mod_dst", (sl, p, src_i, (dst_i + 1) % len(MOD_DSTS)),
                          outline="#63e6be" if dst_i > 0 else "#383844")

            # Bipolar Amount slider (-100..+100%)
            c.create_rectangle(amt_x0, ry0, amt_x1, ry1, fill="#1a1c24",
                               outline="#63e6be" if active else "#383844", width=1)
            mid_x = (amt_x0 + amt_x1) * 0.5
            c.create_line(mid_x, ry0 + 2, mid_x, ry1 - 2, fill="#404558", width=1)
            if abs(amt_v) > 0.5:
                fx = mid_x + (max(-100.0, min(100.0, amt_v)) / 100.0) * (max(1.0, amt_x1 - amt_x0 - 2) * 0.5)
                x_a, x_b = (mid_x, fx) if fx >= mid_x else (fx, mid_x)
                fill_c = "#1e453b" if amt_v > 0 else "#4a2530"
                top_c = "#63e6be" if amt_v > 0 else "#ff8787"
                c.create_rectangle(x_a, ry0 + 1, x_b, ry1 - 1, fill=fill_c, outline="")
                c.create_rectangle(x_a, ry0 + 1, x_b, ry0 + 4, fill=top_c, width=0)
            c.create_text((amt_x0 + amt_x1) / 2, (ry0 + ry1) / 2,
                          text="%+d%%" % int(round(amt_v)),
                          fill="#f0f6fc" if active else "#9090a4", font=(fam, fsmall, "bold"))
            self.hit_boxes.append((amt_x0, ry0, amt_x1, ry1, "mod_amt", (sl, p, amt_x0, amt_x1)))

        # Bottom: 16-Pad selector strip (same as Seq tab)
        py0 = h - pad_m - strip_h
        pw = (w - 2 * pad_m) / float(EP_PADS)
        for pi in range(EP_PADS):
            px0 = pad_m + pi * pw
            px1 = px0 + pw - 2
            pi_info = self.pads_data[pi] if (self.pads_data and len(self.pads_data[pi]) >= 36) else None
            has_mod = bool(pi_info and any(pi_info[24 + s] > 0 and pi_info[28 + s] > 0 and abs(pi_info[32 + s]) > 0.5 for s in range(4)))
            mute = pi_info[13] if pi_info else 0
            solo = pi_info[14] if pi_info else 0
            is_sel = (pi == self.sel_pad)
            accent = ROW_COLORS[(pi // 4) & 3]
            bg = COL_PAD_SEL if is_sel else ("#243b38" if has_mod else COL_PANEL)
            out_c = "#ffffff" if is_sel else ("#63e6be" if has_mod else "#3a3a44")
            c.create_rectangle(px0, py0, px1, py0 + strip_h, fill=bg, outline=out_c, width=2 if is_sel else 1)
            c.create_rectangle(px0 + 1, py0 + 1, px1 - 1, py0 + 4, fill="#555560" if mute else accent, width=0)
            lbl = "M" if mute else ("S" if solo else str(pi + 1))
            fg = "#ff8787" if mute else ("#ffd43b" if solo else ("#ffffff" if (is_sel or has_mod) else "#787888"))
            c.create_text((px0 + px1) / 2, py0 + strip_h / 2 + 2, text=lbl,
                          fill=fg, font=(fam, max(6, fsmall - 1), "bold" if (is_sel or has_mod) else "normal"))
            self.hit_boxes.append((px0, py0, px1, py0 + strip_h, "seq_sel_pad", pi))

        # Popup overlay when LFO 1 or LFO 2 button is tapped
        if self.lfo_popup in (0, 1):
            self._draw_lfo_popup(c, w, h, top_h, fam, fs, self.lfo_popup, p, pinfo)

    def _draw_lfo_popup(self, c, w, h, top_h, fam, fs, li, p, pinfo):
        # Modal overlay matching Eka Pads dark flat style
        self.hit_boxes = []
        col_hi, col_fill = ("#66d9e8", "#1c3d47") if li == 0 else ("#da77f2", "#3b1f47")
        wf = int(pinfo[20 + li]) if pinfo else (0 if li == 0 else 1)
        wave_i = (wf & 7) % len(LFO_WAVES)
        is_sync = bool(wf & 16)
        rate_v = float(pinfo[22 + li]) if pinfo else (1.5 if li == 0 else 4.0)

        mx = max(10, int(w * 0.05))
        my = max(top_h + 6, int(h * 0.12))
        px0, py0, px1, py1 = mx, my, w - mx, h - max(10, int(h * 0.06))
        ph = max(120, py1 - py0)

        # Dimmed backdrop + Eka Pads panel
        c.create_rectangle(0, top_h, w, h, fill="#0b0b0f", stipple="gray50", outline="")
        c.create_rectangle(px0, py0, px1, py1, fill=COL_BG, outline=col_hi, width=2)

        pad_i = 8
        ix0, ix1 = px0 + pad_i, px1 - pad_i
        iw = ix1 - ix0

        # Header bar
        hdr_h = max(26, fs * 2 + 6)
        c.create_rectangle(px0 + 1, py0 + 1, px1 - 1, py0 + hdr_h, fill=COL_PANEL, outline="")
        c.create_rectangle(px0 + 1, py0 + 1, px1 - 1, py0 + 4, fill=col_hi, width=0)
        c.create_text(ix0 + 2, py0 + hdr_h / 2 + 1, anchor="w",
                      text="LFO %d  —  Pad %d" % (li + 1, p + 1),
                      fill=col_hi, font=(fam, fs + 1, "bold"))
        cw = max(54, int(iw * 0.18))
        self._add_btn(c, ix1 - cw, py0 + 4, ix1, py0 + hdr_h - 3, "Close",
                      "#2a2a34", "#f0f0f5", fam, max(8, fs - 1), "lfo_close", None, outline="#444452")

        # Waveform preview box (same style as Pads tab waveform inspector)
        sl_h = max(28, min(36, int((ph - hdr_h) * 0.22)))
        wy0 = py0 + hdr_h + 6
        wy1 = max(wy0 + 36, py1 - sl_h * 2 - 22)
        c.create_rectangle(ix0, wy0, ix1, wy1, fill="#111116", outline="#383844", width=1)
        ymid = (wy0 + wy1) * 0.5
        yamp = (wy1 - wy0) * 0.38
        c.create_line(ix0 + 2, ymid, ix1 - 2, ymid, fill="#262834", width=1, dash=(3, 3))
        npts = 96
        pts = []
        sh_vals = (0.7, -0.4, 0.9, -0.8, 0.2, 0.6, -0.6, -0.1)
        for k in range(npts + 1):
            u = k / float(npts)  # 0..1 across 2 full LFO cycles
            ph_c = (u * 2.0) % 1.0
            if wave_i == 0:    # Sine
                v = math.sin(ph_c * 2.0 * math.pi)
            elif wave_i == 1:  # Tri
                v = 1.0 - 4.0 * abs(ph_c - 0.5)
            elif wave_i == 2:  # Saw
                v = 2.0 * ph_c - 1.0
            elif wave_i == 3:  # Square
                v = 1.0 if ph_c < 0.5 else -1.0
            else:              # S&H
                v = sh_vals[int(u * len(sh_vals)) % len(sh_vals)]
            pts.append(ix0 + 4 + u * (iw - 8))
            pts.append(ymid - v * yamp)
        c.create_line(*pts, fill=col_hi, width=2)

        # Row 1: Horizontal Mode slider (Free | Sync) + Horizontal Wave slider (Sine..S&H)
        r1y0 = wy1 + 7
        r1y1 = r1y0 + sl_h
        sync_w = max(84.0, iw * 0.30)
        sx0, sx1 = ix0, ix0 + sync_w
        c.create_rectangle(sx0, r1y0, sx1, r1y1, fill="#1a1c24", outline=col_hi, width=1)
        smid = (sx0 + sx1) * 0.5
        if not is_sync:
            c.create_rectangle(sx0 + 1, r1y0 + 1, smid, r1y1 - 1, fill=col_fill, outline="")
            c.create_rectangle(sx0 + 1, r1y0 + 1, smid, r1y0 + 4, fill=col_hi, width=0)
        else:
            c.create_rectangle(smid, r1y0 + 1, sx1 - 1, r1y1 - 1, fill=col_fill, outline="")
            c.create_rectangle(smid, r1y0 + 1, sx1 - 1, r1y0 + 4, fill=col_hi, width=0)
        c.create_line(smid, r1y0 + 2, smid, r1y1 - 2, fill="#383844", width=1)
        c.create_text((sx0 + smid) / 2, (r1y0 + r1y1) / 2 + 1, text="Free",
                      fill="#ffffff" if not is_sync else "#787888", font=(fam, fs, "bold" if not is_sync else "normal"))
        c.create_text((smid + sx1) / 2, (r1y0 + r1y1) / 2 + 1, text="Sync",
                      fill="#ffffff" if is_sync else "#787888", font=(fam, fs, "bold" if is_sync else "normal"))
        self.hit_boxes.append((sx0, r1y0, sx1, r1y1, "lfo_sync_slider", (li, p, sx0, sx1)))

        wx0, wx1 = sx1 + 6, ix1
        ww_tot = max(40.0, wx1 - wx0)
        nw = len(LFO_WAVES)
        seg_w = ww_tot / float(nw)
        c.create_rectangle(wx0, r1y0, wx1, r1y1, fill="#1a1c24", outline=col_hi, width=1)
        ax0 = wx0 + wave_i * seg_w
        ax1 = ax0 + seg_w
        c.create_rectangle(ax0 + 1, r1y0 + 1, ax1 - 1, r1y1 - 1, fill=col_fill, outline="")
        c.create_rectangle(ax0 + 1, r1y0 + 1, ax1 - 1, r1y0 + 4, fill=col_hi, width=0)
        for wi, wname in enumerate(LFO_WAVES):
            if wi > 0:
                tx = wx0 + wi * seg_w
                c.create_line(tx, r1y0 + 3, tx, r1y1 - 3, fill="#303240", width=1)
            c.create_text(wx0 + (wi + 0.5) * seg_w, (r1y0 + r1y1) / 2 + 1, text=wname,
                          fill="#ffffff" if wi == wave_i else "#888898",
                          font=(fam, max(7, fs - 1), "bold" if wi == wave_i else "normal"))
        self.hit_boxes.append((wx0, r1y0, wx1, r1y1, "lfo_wave_slider", (li, p, wx0, wx1)))

        # Row 2: Full-width horizontal Rate / Sync Division slider
        r2y0 = r1y1 + 7
        r2y1 = r2y0 + sl_h
        c.create_rectangle(ix0, r2y0, ix1, r2y1, fill="#1a1c24", outline=col_hi, width=1)
        t = max(0.0, min(1.0, math.log(max(0.05, rate_v) / 0.05) / math.log(400.0)))
        if is_sync:
            nd = len(LFO_SYNC_DIVS)
            didx = max(0, min(nd - 1, int(round(t * (nd - 1)))))
            for di in range(1, nd):
                tx = ix0 + (di / float(nd - 1)) * iw
                c.create_line(tx, r2y0 + 3, tx, r2y1 - 3, fill="#2b2e3c", width=1)
            fx = ix0 + 1 + (didx / float(nd - 1)) * max(1.0, iw - 2)
            rlbl = "Rate (Sync):  %s" % LFO_SYNC_DIVS[didx]
        else:
            fx = ix0 + 1 + t * max(1.0, iw - 2)
            rlbl = "Rate:  %.2f Hz" % rate_v if rate_v < 1.0 else ("Rate:  %.1f Hz" % rate_v)
        c.create_rectangle(ix0 + 1, r2y0 + 1, fx, r2y1 - 1, fill=col_fill, outline="")
        c.create_rectangle(ix0 + 1, r2y0 + 1, fx, r2y0 + 4, fill=col_hi, width=0)
        c.create_text((ix0 + ix1) / 2, (r2y0 + r2y1) / 2 + 1, text=rlbl,
                      fill="#f0f6fc", font=(fam, fs, "bold"))
        self.hit_boxes.append((ix0, r2y0, ix1, r2y1, "lfo_rate", (li, p, ix0, ix1, is_sync)))

        # Absorb taps inside popup background; tapping outside closes popup
        self.hit_boxes.append((px0, py0, px1, py1, "lfo_nop", None))
        self.hit_boxes.append((0, 0, w, h, "lfo_close", None))

    def _draw_kit_tab(self, c, w, h, top_h, fam, fs):
        pad_m = 8
        y = top_h + pad_m
        bh = max(30, fs * 2 + 10)
        fsmall = max(8, fs - 1)

        scene = int(round(self._mon("out_seq_scene", 0)))

        # Row 1: Factory Kit + Folder Kit + Pad MIDI Notes window
        half = (w - 3 * pad_m) / 2.0
        third = (w - 4 * pad_m) / 3.0
        self._add_btn(c, pad_m, y, pad_m + third, y + bh, "Factory 16-Pad Kit",
                      COL_ACCENT, "#000000", fam, fsmall, "factory_kit", None)
        self._add_btn(c, pad_m * 2 + third, y, pad_m * 2 + 2 * third, y + bh, "Load Folder Kit",
                      "#1e453b", "#63e6be", fam, fsmall, "open_folder_kit", None, outline="#38d9a9")
        self._add_btn(c, pad_m * 3 + 2 * third, y, w - pad_m, y + bh,
                      "Pad MIDI Notes",
                      "#1c2a3a", "#74c0fc", fam, fsmall, "open_notes", None,
                      outline=COL_ACCENT)
        y += bh + 10

        # Row 2: Clear Active Scene + Unload Pad Sample + Reset All to Default
        self._add_btn(c, pad_m, y, pad_m + third, y + bh,
                      "Clear Scene %s" % SCENE_NAMES[scene & 7], COL_PANEL, "#ff8787", fam, fsmall, "clr_scene", scene)
        self._add_btn(c, pad_m * 2 + third, y, pad_m * 2 + 2 * third, y + bh,
                      "Unload Pad %d" % (self.sel_pad + 1), COL_PANEL, "#ffd43b", fam, fsmall, "unload", self.sel_pad)
        confirming = time.monotonic() < self.reset_confirm_until
        self._add_btn(c, pad_m * 3 + 2 * third, y, w - pad_m, y + bh,
                      "Confirm Reset?" if confirming else "Reset All",
                      COL_REC if confirming else "#3b1d22",
                      "#ffffff" if confirming else "#ff8787", fam, fsmall, "reset_all", None,
                      outline="#ff6b6b")
        y += bh + 10

        # Row 3: Hardware Pad Filter (Retrig, MinVel, XTalk) + Master Bus Glue Compressor
        retrig = int(self.shm_hdr[15]) if (self.shm_hdr and len(self.shm_hdr) > 17) else 25
        minvel = int(self.shm_hdr[16]) if (self.shm_hdr and len(self.shm_hdr) > 17) else 0
        xtalk  = int(self.shm_hdr[17]) if (self.shm_hdr and len(self.shm_hdr) > 17) else 0
        glue   = int(round(self.shm_hdr[28])) if (self.shm_hdr and len(self.shm_hdr) >= 30) else int(round(self._zval("bus_glue", 0.0)))

        q_w = (w - 5 * pad_m) / 4.0
        sliders = (
            ("retrig", CMD_SET_RETRIG, retrig, 80, 5,
             "Retrig %dms" % retrig if retrig > 0 else "Retrig Off", "#74c0fc", "#1e3a52"),
            ("minvel", CMD_SET_MINVEL, minvel, 40, 2,
             "MinVel %d" % minvel if minvel > 0 else "MinVel Off", "#63e6be", "#1e453b"),
            ("xtalk",  CMD_SET_XTALK,  xtalk,  60, 5,
             "XTalk %d%%" % xtalk if xtalk > 0 else "XTalk Off", "#ffd43b", "#473b19"),
            ("glue",   None,           glue,  100, 5,
             "Glue %d%%" % glue if glue > 0 else "Glue Off", "#ff922b", "#4a2c14"),
        )
        for idx, (_key, cmd_op, val, vmax, step, lbl, col_hi, col_fill) in enumerate(sliders):
            sx0 = pad_m + idx * (q_w + pad_m)
            sx1 = sx0 + q_w
            c.create_rectangle(sx0, y, sx1, y + bh, fill="#1a1c24",
                               outline=col_hi if val > 0 else "#383844", width=1)
            if val > 0:
                fx = sx0 + 1 + (min(val, vmax) / float(vmax)) * max(1.0, (sx1 - sx0 - 2))
                c.create_rectangle(sx0 + 1, y + 1, fx, y + bh - 1, fill=col_fill, outline="")
                c.create_rectangle(sx0 + 1, y + 1, fx, y + 4, fill=col_hi, width=0)
            c.create_text((sx0 + sx1) / 2, y + bh / 2, text=lbl,
                          fill=col_hi if val > 0 else "#9a9aaa", font=(fam, fsmall, "bold"))
            if cmd_op is None:
                self.hit_boxes.append((sx0, y, sx1, y + bh, "aux_slider", ("bus_glue", 0.0, 100.0, sx0, sx1)))
            else:
                self.hit_boxes.append((sx0, y, sx1, y + bh, "flt_slider", (cmd_op, vmax, step, sx0, sx1)))
        y += bh + 10

        # Row 4 & 5: Aux 1..4 -> Zynthian Mixbus routing buttons (2x2 grid, tap to cycle Mixbuses)
        aux_styles = (
            ("#66d9e8", "#22343d"),
            ("#da77f2", "#32203d"),
            ("#ffd43b", "#3d3318"),
            ("#63e6be", "#1c3b32"),
        )
        for row_i in range(2):
            for col_i in range(2):
                ai = row_i * 2 + col_i
                col_hi, col_bg = aux_styles[ai]
                bx0 = pad_m + col_i * (half + pad_m)
                bx1 = bx0 + half
                tgt = self.aux_targets[ai]
                lbl = "Aux %d -> %s" % (ai + 1, tgt[1] if tgt else "Off")
                self._add_btn(c, bx0, y, bx1, y + bh, lbl,
                              col_bg if tgt else COL_PANEL,
                              col_hi if tgt else "#b0b0c0", fam, fsmall, "aux_route", ai,
                              outline=col_hi if tgt else "#444450")
            y += bh + 8
        y += 4

        # Helpful M-VAVE SMC-PAD Pocket guide
        guide = (
            "M-VAVE SMC-PAD Pocket:\n"
            "  • Pads 1..16 = MIDI Notes 36..51 (configure in Pad MIDI Notes)\n"
            "  • Bank B Notes 52..56 = Quantized Scene Switch A..E\n"
            "  • Seq tab: long-press bottom pad 1..16 to Mute / Unmute"
        )
        c.create_text(pad_m, y, anchor="nw", text=guide, fill="#a0a0b4", font=(fam, fsmall))

        # Popup overlays on Kit tab
        if self.notes_popup:
            self._draw_notes_popup(c, w, h, top_h, fam, fs)
        elif self.folder_popup:
            self._draw_folder_popup(c, w, h, top_h, fam, fs)

    def _draw_folder_popup(self, c, w, h, top_h, fam, fs):
        self.hit_boxes = []
        fsmall = max(7, fs - 1)
        pw = max(300, min(int(w * 0.86), 660))
        ph = max(210, min(int((h - top_h) * 0.86), 340))
        px0 = (w - pw) // 2
        py0 = top_h + max(6, (h - top_h - ph) // 2)
        px1, py1 = px0 + pw, py0 + ph

        c.create_rectangle(0, top_h, w, h, fill="#0b0b0f", stipple="gray50", outline="")
        c.create_rectangle(px0, py0, px1, py1, fill=COL_BG, outline="#38d9a9", width=2)

        pad_i = 8
        ix0, ix1 = px0 + pad_i, px1 - pad_i
        iw = max(120, ix1 - ix0)

        hdr_h = max(26, fs * 2 + 6)
        c.create_rectangle(px0 + 1, py0 + 1, px1 - 1, py0 + hdr_h, fill=COL_PANEL, outline="")
        c.create_rectangle(px0 + 1, py0 + 1, px1 - 1, py0 + 4, fill="#38d9a9", width=0)
        per_page = 8
        total_pages = max(1, (len(self.folder_list) + per_page - 1) // per_page)
        page = max(0, min(total_pages - 1, self.folder_page))
        self.folder_page = page
        c.create_text(ix0 + 2, py0 + hdr_h / 2 + 1, anchor="w",
                      text="Load Folder as Kit (%d/%d)" % (page + 1, total_pages),
                      fill="#63e6be", font=(fam, fs, "bold"))
        bw_c = max(46, int(iw * 0.13))
        bx_cl = ix1 - bw_c
        if total_pages > 1:
            self._add_btn(c, bx_cl - 2 * bw_c - 8, py0 + 4, bx_cl - bw_c - 8, py0 + hdr_h - 3, "Prev",
                          COL_PANEL, "#e0e0f0", fam, fsmall, "folder_page", -1)
            self._add_btn(c, bx_cl - bw_c - 4, py0 + 4, bx_cl - 4, py0 + hdr_h - 3, "Next",
                          COL_PANEL, "#e0e0f0", fam, fsmall, "folder_page", 1)
        self._add_btn(c, bx_cl, py0 + 4, ix1, py0 + hdr_h - 3, "Close",
                      "#2a2a34", "#f0f0f5", fam, fsmall, "folder_close", None, outline="#444452")

        body_y = py0 + hdr_h + 8
        body_h = max(120, py1 - body_y - 8)
        items = self.folder_list[page * per_page:(page + 1) * per_page]
        if not items:
            c.create_text((ix0 + ix1) / 2, body_y + body_h / 2,
                          text="No sample folders found in Samples/", fill=COL_DIM, font=(fam, fs))
        else:
            cols = 2
            rows = 4
            cw = (iw - 6) / float(cols)
            rh = (body_h - (rows - 1) * 6) / float(rows)
            for idx, (dpath, label, cnt) in enumerate(items):
                r_i = idx // cols
                c_i = idx % cols
                bx0 = ix0 + c_i * (cw + 6)
                by0 = body_y + r_i * (rh + 6)
                self._add_btn(c, bx0, by0, bx0 + cw, by0 + rh,
                              "%s  (%d)" % (label, cnt),
                              "#1b2d2a", "#e6fcf5", fam, fsmall, "folder_pick", dpath, outline="#38d9a9")

        self.hit_boxes.append((px0, py0, px1, py1, "folder_nop", None))
        self.hit_boxes.append((0, 0, w, h, "folder_close", None))

    def _draw_notes_popup(self, c, w, h, top_h, fam, fs):
        self.hit_boxes = []
        sel_p = self.sel_pad
        active_row = (sel_p >> 2) & 3
        row_col = ROW_COLORS[active_row]
        learn = self.shm_hdr[5] if self.shm_hdr else int(round(self._zval("learn", 0)))
        sel_note = self._get_pad_note(sel_p)
        fsmall = max(7, fs - 1)
        ftiny = max(6, fs - 2)

        mx = max(6, int(w * 0.02))
        my = top_h + 4
        px0, py0, px1, py1 = mx, my, w - mx, h - 4

        # Dimmed backdrop + modal card
        c.create_rectangle(0, top_h, w, h, fill="#0b0b0f", stipple="gray50", outline="")
        c.create_rectangle(px0, py0, px1, py1, fill=COL_BG,
                           outline=COL_REC if learn else row_col, width=2)

        pad_i = 7
        ix0, ix1 = px0 + pad_i, px1 - pad_i
        iw = max(120, ix1 - ix0)

        # Header bar: status title + Learn + Reset (Def) + Close
        hdr_h = max(28, fs * 2 + 8)
        c.create_rectangle(px0 + 1, py0 + 1, px1 - 1, py0 + hdr_h, fill=COL_PANEL, outline="")
        c.create_rectangle(px0 + 1, py0 + 1, px1 - 1, py0 + 4,
                           fill=COL_REC if learn else row_col, width=0)
        if learn:
            title_txt = "Hit MIDI pad for P%d..." % (sel_p + 1)
            title_col = "#ff8787"
        else:
            title_txt = "Pad Notes  •  P%d: %s" % (sel_p + 1, note_name(sel_note))
            title_col = row_col
        c.create_text(ix0 + 2, py0 + hdr_h / 2 + 1, anchor="w",
                      text=title_txt, fill=title_col, font=(fam, fs, "bold"))

        bw_close = max(46, int(iw * 0.12))
        bw_rst = max(44, int(iw * 0.11))
        bw_lrn = max(62, int(iw * 0.17))
        bx_close = ix1 - bw_close
        bx_rst = bx_close - bw_rst - 4
        bx_lrn = bx_rst - bw_lrn - 4

        self._add_btn(c, bx_lrn, py0 + 4, bx_lrn + bw_lrn, py0 + hdr_h - 3,
                      "Learn ON" if learn else "Learn",
                      COL_REC if learn else "#251c22",
                      "#ffffff" if learn else "#ff8787",
                      fam, fsmall, "notes_learn", 1 - learn,
                      outline="#ffffff" if learn else COL_REC)
        confirming = time.monotonic() < self.notes_reset_confirm_until
        self._add_btn(c, bx_rst, py0 + 4, bx_rst + bw_rst, py0 + hdr_h - 3,
                      "Sure?" if confirming else "Def",
                      "#e67700" if confirming else COL_PANEL,
                      "#000000" if confirming else "#ffd43b",
                      fam, fsmall, "notes_reset", None,
                      outline="#ffd43b" if confirming else "#444452")
        self._add_btn(c, bx_close, py0 + 4, ix1, py0 + hdr_h - 3, "Close",
                      "#2a2a34", "#f0f0f5", fam, fsmall, "notes_close", None,
                      outline="#444452")

        # Body: Row selector column (left) + 4x4 Pad Note grid (right)
        body_y = py0 + hdr_h + 5
        body_h = max(132, py1 - body_y - 5)
        row_w = max(36, int(iw * 0.105))
        grid_x0 = ix0 + row_w + 5
        grid_w = max(120, ix1 - grid_x0)
        cw = grid_w / 4.0
        ch = body_h / 4.0

        # Active row highlight frame spanning row selector + 4 pads (shows encoder mapping)
        ar_ui = 3 - active_row
        ary0 = body_y + ar_ui * ch
        ary1 = ary0 + ch
        c.create_rectangle(ix0 - 2, ary0, ix1 + 2, ary1,
                           fill="#1b2130", outline=row_col, width=1)

        # Left column: 4 Row buttons (R4 top .. R1 bottom)
        for row_hw in range(4):
            row_ui = 3 - row_hw
            ry0 = body_y + row_ui * ch + 2
            ry1 = body_y + (row_ui + 1) * ch - 2
            r_col = ROW_COLORS[row_hw]
            is_act = (row_hw == active_row)
            c.create_rectangle(ix0, ry0, ix0 + row_w, ry1,
                               fill=r_col if is_act else COL_PANEL,
                               outline="#ffffff" if is_act else "#3e3e4a",
                               width=2 if is_act else 1)
            if not is_act:
                c.create_rectangle(ix0 + 1, ry0 + 1, ix0 + 4, ry1 - 1, fill=r_col, width=0)
            rmid = (ry0 + ry1) / 2.0
            if ch >= 42:
                c.create_text(ix0 + row_w / 2, rmid - fs * 0.55,
                              text="R%d" % (row_hw + 1),
                              fill="#000000" if is_act else "#f0f0f5",
                              font=(fam, fsmall, "bold"))
                c.create_text(ix0 + row_w / 2, rmid + fs * 0.65,
                              text="%d-%d" % (row_hw * 4 + 1, row_hw * 4 + 4),
                              fill="#141418" if is_act else "#9090a4",
                              font=(fam, ftiny))
            else:
                c.create_text(ix0 + row_w / 2, rmid,
                              text="R%d" % (row_hw + 1),
                              fill="#000000" if is_act else "#f0f0f5",
                              font=(fam, fsmall, "bold"))
            self.hit_boxes.append((ix0, ry0, ix0 + row_w, ry1, "notes_row", row_hw))

        # Right 4x4 Grid: 16 Pads (bottom row = Pads 1..4, top row = Pads 13..16)
        for p in range(EP_PADS):
            col_i = p % 4
            row_hw = p // 4
            row_ui = 3 - row_hw
            gx0 = grid_x0 + col_i * cw + 2
            gy0 = body_y + row_ui * ch + 2
            gx1 = grid_x0 + (col_i + 1) * cw - 2
            gy1 = body_y + (row_ui + 1) * ch - 2

            is_sel = (p == sel_p)
            is_ar = (row_hw == active_row)
            mn = self._get_pad_note(p)
            lvl = self._mon("out_pad_lvl%d" % (p + 1), 0.0)
            accent = ROW_COLORS[row_hw]

            if is_sel and learn:
                bg = "#4a1c24"
                out_c = COL_REC
                out_w = 2
            elif is_sel:
                bg = COL_PAD_SEL
                out_c = "#ffffff"
                out_w = 2
            elif lvl > 0.05:
                bg = "#3b4866"
                out_c = accent if is_ar else "#6c7a9c"
                out_w = 1
            elif is_ar:
                bg = "#252b3b"
                out_c = accent
                out_w = 1
            else:
                bg = COL_PAD
                out_c = "#3a3a46"
                out_w = 1

            c.create_rectangle(gx0, gy0, gx1, gy1, fill=bg, outline=out_c, width=out_w)
            c.create_rectangle(gx0 + 1, gy0 + 1, gx1 - 1, gy0 + 4,
                               fill=COL_REC if (is_sel and learn) else accent, width=0)

            if lvl > 0.02:
                bar_w = max(2, int((gx1 - gx0 - 4) * min(lvl, 1.0)))
                c.create_rectangle(gx0 + 2, gy1 - 4, gx0 + 2 + bar_w, gy1 - 1, fill="#ffd43b", width=0)

            # Top-left: Pad number; Top-right: Encoder badge (E1..E4) or LRN
            c.create_text(gx0 + 4, gy0 + 6, anchor="nw", text="P%d" % (p + 1),
                          fill="#c0c4d4" if is_ar else "#88889a", font=(fam, ftiny))
            if is_sel and learn:
                c.create_text(gx1 - 4, gy0 + 6, anchor="ne", text="LRN",
                              fill="#ff8787", font=(fam, ftiny, "bold"))
            elif is_ar:
                c.create_text(gx1 - 4, gy0 + 6, anchor="ne", text="E%d" % (col_i + 1),
                              fill=accent, font=(fam, ftiny, "bold"))

            # Center: Note name + MIDI number
            note_txt = ("%s (%d)" % (note_short(mn), mn)) if cw >= 92 else ("%s %d" % (note_short(mn), mn))
            cy = (gy0 + gy1) / 2.0 + (0 if ch < 50 else -2)
            c.create_text((gx0 + gx1) / 2.0, cy, text=note_txt,
                          fill="#ffffff" if (is_sel or is_ar) else "#e0e0ea",
                          font=(fam, fs if cw >= 86 else fsmall, "bold"))

            # Bottom: short sample name
            if ch >= 48:
                sname = short_sample_name(self._pad_path(p), p)
                c.create_text((gx0 + gx1) / 2.0, gy1 - max(9, ftiny + 3),
                              text=sname,
                              fill="#b8c0d4" if is_sel else "#8c90a4",
                              font=(fam, ftiny))

            self.hit_boxes.append((gx0, gy0, gx1, gy1, "notes_pad", p))

        # Absorb taps inside popup background; tapping outside closes popup
        self.hit_boxes.append((px0, py0, px1, py1, "notes_nop", None))
        self.hit_boxes.append((0, 0, w, h, "notes_close", None))

    # ---------- Touch Interaction ----------

    def _on_press(self, event):
        if event is None:
            return
        x, y = event.x, event.y
        now = time.monotonic()

        for x0, y0, x1, y1, action, arg in self.hit_boxes:
            if not (x0 <= x <= x1 and y0 <= y <= y1):
                continue

            if action == "tab":
                self._release_momentary_fx()
                self._cancel_choke_hold()
                self.tab = arg
                self.copy_src = -1
                self.lfo_popup = None
                self.seq_tools_popup = False
                self.folder_popup = False
                self._cancel_step_hold()
                self._cancel_wave_hold()
                self.step_popup = None
                self.step_popup_vals = None
                self.step_drag_override = None
                self.sample_popup = None
                self.sample_popup_vals = None
                self._close_notes_popup(restore_group=False)
                self.reset_confirm_until = 0.0
                self._select_page(arg)
                self.refresh_gui()
                return

            if action == "choke_grp":
                self._cancel_choke_hold()
                self.drag_info = ("choke_hold", arg, now, 0)
                try:
                    self._choke_hold_after = self.after(350, lambda g=arg: self._on_choke_hold_timeout(g))
                except Exception:
                    self._choke_hold_after = None
                return

            if action == "choke_all_on":
                self._cancel_choke_hold()
                self._set_choke_masks(mute_mask=0, solo_mask=0)
                self._toast("All Choke Groups On", secs=1.4)
                self.refresh_gui()
                return

            if action == "set_repeat_div":
                self._send_cmd(CMD_SET_MOD_AMT, a=6, c=1, val=arg)
                self.refresh_gui()
                return

            if action == "set_play_mode":
                self._send_cmd(CMD_SET_MOD_AMT, a=6, c=0, val=arg)
                self.refresh_gui()
                return

            if action == "xy_mode_toggle":
                self.xy_hold_mode = not self.xy_hold_mode
                if not self.xy_hold_mode:
                    self._send_perf_xy(0, 20, persistent=False)
                self.refresh_gui()
                return

            if action == "perf_xy":
                px0, py0, px1, py1 = arg
                if self.xy_hold_mode and self.last_tap[0] == "perf_xy" and (now - self.last_tap[1]) < 0.35:
                    self.last_tap = (None, 0.0)
                    self._send_perf_xy(0, 20, persistent=False)
                    self.refresh_gui()
                    return
                self.last_tap = ("perf_xy", now)
                fv, rv = self._apply_perf_xy(px0, py0, px1, py1, x, y)
                self.drag_info = ("perf_xy", [px0, py0, px1, py1, fv, rv], y, 0)
                self.refresh_gui()
                return

            if action == "perf_fx_hold":
                self.perf_fx_local = arg
                self._send_cmd(CMD_SET_MOD_AMT, a=6, c=15, val=arg)
                self.drag_info = ("perf_fx_hold", arg, now, 0)
                self.refresh_gui()
                return

            if action == "perf_break_hold":
                self.perf_break_local = True
                self._send_perf_flags()
                self.drag_info = ("perf_break_hold", None, now, 0)
                self.refresh_gui()
                return

            if action == "perf_throw_hold":
                self.perf_throw_local = True
                self._send_perf_flags()
                self.drag_info = ("perf_throw_hold", None, now, 0)
                self.refresh_gui()
                return

            if action == "cycle_play_mode":
                self._send_cmd(CMD_SET_MOD_AMT, a=6, c=0, val=arg)
                nm = ("Normal Pads", "Chromatic (P%d)" % (self.sel_pad + 1),
                      "16 Levels: Velocity", "16 Levels: Filter")[arg % 4]
                self._toast(nm, secs=1.8)
                self.refresh_gui()
                return

            if action == "cycle_repeat_div":
                self._send_cmd(CMD_SET_MOD_AMT, a=6, c=1, val=arg)
                nm = ("Note Repeat Off", "Repeat 1/8", "Repeat 1/16", "Repeat 1/16T", "Repeat 1/32")[arg % 5]
                self._toast(nm, secs=1.8)
                self.refresh_gui()
                return

            if action == "pad":
                self.sel_pad = arg
                self._zset("sel_pad", arg)
                self._send_cmd(CMD_TRIG_PAD, b=arg, val=110)
                self._send_cmd(CMD_SET_MOD_AMT, a=5, b=arg, c=9, val=110)
                self.last_synced_pad = -1
                self.drag_info = ("pad_hold", arg, now, 0)
                self.refresh_gui()
                return

            if action == "pad_16lvl":
                self._send_cmd(CMD_SET_MOD_AMT, a=6, b=arg, c=9, val=110)
                self.drag_info = ("pad_16lvl_hold", arg, now, 0)
                self.refresh_gui()
                return

            if action == "sample_step":
                p_i, delta = arg
                self._sample_step(p_i, delta)
                self.refresh_gui()
                return

            if action == "load":
                self._open_file_selector(arg)
                return

            if action == "wave_trim":
                pad_i, st_pct, en_pct, wx0, wx1 = arg
                ww = max(1.0, wx1 - wx0)
                sx = wx0 + (st_pct / 100.0) * ww
                ex = wx0 + (en_pct / 100.0) * ww
                which = "start" if abs(x - sx) <= abs(x - ex) else "end"
                self._cancel_wave_hold()
                self.drag_info = ("wave_hold", (pad_i, which, wx0, wx1, x, y), now, 0)
                try:
                    self._wave_hold_after = self.after(380, lambda pi=pad_i: self._on_wave_hold_timeout(pi))
                except Exception:
                    self._wave_hold_after = None
                return

            if action == "sample_popup_tab":
                self.sample_popup_tab = arg
                self.refresh_gui()
                return

            if action == "sample_popup_nop":
                return

            if action == "sample_popup_close":
                self.sample_popup = None
                self.sample_popup_vals = None
                self.refresh_gui()
                return

            if action == "sample_popup_play":
                self._send_cmd(CMD_TRIG_PAD, b=arg, val=110)
                return

            if action == "sample_popup_load":
                self.sample_popup = None
                self.sample_popup_vals = None
                self._open_file_selector(arg)
                self.refresh_gui()
                return

            if action == "sample_slider":
                sym, vmin, vmax, step, bip, sx0, sx1 = arg
                tap_key = ("sample_slider", sym)
                if bip and self.last_tap[0] == tap_key and (now - self.last_tap[1]) < 0.35:
                    self._apply_sample_val(sym, 0.0)
                    self.last_tap = (None, 0.0)
                    self.refresh_gui()
                    return
                self.last_tap = (tap_key, now)
                nv = self._apply_sample_slider(sym, vmin, vmax, step, sx0, sx1, x)
                self.drag_info = ("sample_slider", [sym, vmin, vmax, step, sx0, sx1, nv], y, 0)
                self.refresh_gui()
                return

            if action == "sample_mode_slider":
                mx0, mx1 = arg
                nv = self._apply_sample_seg("pad_mode", 5, mx0, mx1, x)
                self.drag_info = ("sample_mode_slider", [mx0, mx1, nv], y, 0)
                self.refresh_gui()
                return

            if action == "sample_choke_slider":
                kx0, kx1 = arg
                nv = self._apply_sample_seg("pad_choke", 5, kx0, kx1, x)
                self.drag_info = ("sample_choke_slider", [kx0, kx1, nv], y, 0)
                self.refresh_gui()
                return

            if action == "slice_mode":
                sx0, sx1 = arg
                t = max(0.0, min(0.9999, (x - sx0) / max(1.0, sx1 - sx0)))
                self.slice_mode = int(t * 4)
                self.refresh_gui()
                return

            if action == "slice_sens":
                sx0, sx1 = arg
                self.drag_info = ("slice_sens", (sx0, sx1), y, 0)
                t = max(0.0, min(1.0, (x - sx0) / max(1.0, sx1 - sx0)))
                self.slice_sens = int(round(t * 20.0)) * 5
                self.refresh_gui()
                return

            if action == "slice_from_toggle":
                self.slice_from_p1 = not self.slice_from_p1
                self.refresh_gui()
                return

            if action == "slice_choke_toggle":
                self.slice_choke = not self.slice_choke
                self.refresh_gui()
                return

            if action == "slice_apply":
                self._apply_auto_slice(arg)
                self.sample_popup = None
                self.sample_popup_vals = None
                self.refresh_gui()
                return

            if action == "pad_link_mode":
                p_i, nmode = arg
                self._send_cmd(CMD_SET_MOD_AMT, a=5, b=p_i, c=3, val=nmode)
                self.refresh_gui()
                return

            if action == "pad_link_target":
                p_i, lx0, lx1 = arg
                self.drag_info = ("pad_link_target", (p_i, lx0, lx1), y, 0)
                self._apply_pad_link_target(p_i, lx0, lx1, x)
                self.refresh_gui()
                return

            if action == "pad_tool_op_toggle":
                self.pad_tool_op = 1 - self.pad_tool_op
                self.refresh_gui()
                return

            if action == "pad_copy_swap_dst":
                src_p, dst_p = arg
                if dst_p != src_p:
                    src_path = self._pad_path(src_p)
                    dst_path = self._pad_path(dst_p)
                    if self.pad_tool_op == 0:
                        self._send_cmd(CMD_SET_MOD_AMT, a=5, b=src_p, c=7, val=dst_p)
                        self._zset("sample%d" % (dst_p + 1), src_path if src_path else "_")
                        self._toast("Copied P%d -> P%d" % (src_p + 1, dst_p + 1), secs=2.0)
                    else:
                        self._send_cmd(CMD_SET_MOD_AMT, a=5, b=src_p, c=8, val=dst_p)
                        self._zset("sample%d" % (src_p + 1), dst_path if dst_path else "_")
                        self._zset("sample%d" % (dst_p + 1), src_path if src_path else "_")
                        self._toast("Swapped P%d <-> P%d" % (src_p + 1, dst_p + 1), secs=2.0)
                    self.last_synced_pad = -1
                    self.sample_popup = None
                    self.sample_popup_vals = None
                    self.refresh_gui()
                return

            if action == "mute":
                mask = int(round(self._zval("mute_mask", 0)))
                self._zset("mute_mask", mask ^ (1 << arg))
                self.refresh_gui()
                return

            if action == "solo":
                mask = int(round(self._zval("solo_mask", 0)))
                self._zset("solo_mask", mask ^ (1 << arg))
                self.refresh_gui()
                return

            if action == "follow":
                self._zset("follow", arg)
                self.refresh_gui()
                return

            if action == "scene":
                self.drag_info = ("scene_hold", arg, now, 0)
                return

            if action == "seq_scene":
                if self.copy_src >= 0:
                    if arg != self.copy_src:
                        self._send_cmd(CMD_COPY_SCENE, a=self.copy_src, b=arg)
                    self.copy_src = -1
                    self.refresh_gui()
                else:
                    self.drag_info = ("scene_hold", arg, now, 0)
                return

            if action == "seq_tools_open":
                self.seq_tools_popup = True
                self.refresh_gui()
                return

            if action == "seq_tools_nop":
                return

            if action == "seq_tools_close":
                self.seq_tools_popup = False
                self.refresh_gui()
                return

            if action == "seq_undo":
                self._send_cmd(CMD_SET_MOD_AMT, a=6, c=4, val=0)
                self._toast("Undo applied", secs=1.8)
                self.refresh_gui()
                return

            if action == "euclid_hits":
                p_i, plen, ex0, ex1 = arg
                self.drag_info = ("euclid_hits", [p_i, plen, ex0, ex1, -1], y, 0)
                self._apply_euclid_hits(p_i, plen, ex0, ex1, x)
                self.refresh_gui()
                return

            if action == "euclid_rot":
                p_i, plen, rx0, rx1 = arg
                self.drag_info = ("euclid_rot", [p_i, plen, rx0, rx1, -1], y, 0)
                self._apply_euclid_rot(p_i, plen, rx0, rx1, x)
                self.refresh_gui()
                return

            if action == "seq_rotate":
                p_i, direction = arg
                self._send_cmd(CMD_SET_MOD_AMT, a=7, b=p_i, c=0, val=0xFF if direction > 0 else 0xFE)
                self.refresh_gui()
                return

            if action == "seq_metro":
                self._send_cmd(CMD_SET_MOD_AMT, a=6, c=2, val=arg)
                self.refresh_gui()
                return

            if action == "seq_countin":
                self._send_cmd(CMD_SET_MOD_AMT, a=6, c=3, val=arg)
                self.refresh_gui()
                return

            if action == "chain_toggle":
                self._send_cmd(CMD_SET_MOD_AMT, a=6, c=6, val=arg)
                self.refresh_gui()
                return

            if action == "chain_len_slider":
                cx0, cx1 = arg
                self.drag_info = ("chain_len_slider", [cx0, cx1, -1], y, 0)
                self._apply_chain_len(cx0, cx1, x)
                self.refresh_gui()
                return

            if action == "chain_slot":
                slot_i, nsc = arg
                self._send_cmd(CMD_SET_MOD_AMT, a=6, b=slot_i, c=8, val=nsc)
                self.refresh_gui()
                return

            if action == "seq_play":
                self._zset("seq_mode", arg)
                self.refresh_gui()
                return

            if action == "seq_rec":
                self._zset("seq_mode", arg)
                self.refresh_gui()
                return

            if action == "seq_copy":
                self.copy_src = -1 if self.copy_src >= 0 else arg
                self.refresh_gui()
                return

            if action == "seq_clr_pad":
                self.drag_info = ("clr_hold", arg, now, 0)
                return

            if action == "export_zseq":
                self._export_to_zynseq(arg)
                self.refresh_gui()
                return

            if action == "quant_slider":
                qx0, qx1 = arg
                self.drag_info = ("quant_slider", (qx0, qx1), y, 0)
                self._apply_quant_slider(qx0, qx1, x)
                self.refresh_gui()
                return

            if action == "steps_slider":
                sc, p, ps, pe, sl_x0, sl_x1 = arg
                sl_w = max(1.0, sl_x1 - sl_x0)
                rx_a = sl_x0 + (ps / float(EP_STEPS)) * sl_w
                rx_b = sl_x0 + (pe / float(EP_STEPS)) * sl_w
                # Drag Beg if finger lands near left thumb; otherwise drag End (steps count)
                which = "beg" if (abs(x - rx_a) < 22 and abs(x - rx_a) < abs(x - rx_b)) else "end"
                cur_val = ps if which == "beg" else pe
                thumb_x = rx_a if which == "beg" else rx_b
                self._select_group("g_stereo")
                if abs(x - thumb_x) < 22:
                    # Grabbed thumb: relative fine drag from current step (no sudden jump!)
                    self.drag_info = ("steps_slider", [sc, p, which, sl_x0, sl_x1, x, cur_val, cur_val], y, 0)
                else:
                    # Tapped elsewhere on bar: jump there, then fine-drag from that point
                    nv = self._calc_steps_val(which, sl_x0, sl_x1, x)
                    self._commit_steps_val(sc, p, which, nv)
                    self.drag_info = ("steps_slider", [sc, p, which, sl_x0, sl_x1, x, nv, nv], y, 0)
                    self.refresh_gui()
                return

            if action == "bpm_drag":
                self.drag_info = ("bpm", None, y, arg)
                return

            if action == "seq_sel_pad":
                self.sel_pad = arg
                self._zset("sel_pad", arg)
                self.last_synced_pad = -1
                self.drag_info = ("seq_pad_hold", arg, now, 0)
                self.refresh_gui()
                return

            if action == "step_popup_nop":
                return

            if action == "step_popup_close":
                self.step_popup = None
                self.step_popup_vals = None
                self.refresh_gui()
                return

            if action == "step_vel_slider":
                sc, p, st, vx0, vx1 = arg
                nv = self._apply_seq_vel_slider(sc, p, st, vx0, vx1, x)
                self.drag_info = ("step_vel_slider", [sc, p, st, vx0, vx1, nv], y, 0)
                self.refresh_gui()
                return

            if action == "step_chance_slider":
                sc, p, st, cx0, cx1 = arg
                nc = self._apply_step_chance_slider(sc, p, st, cx0, cx1, x)
                self.drag_info = ("step_chance_slider", [sc, p, st, cx0, cx1, nc], y, 0)
                self.refresh_gui()
                return

            if action == "step_roll_slider":
                sc, p, st, rx0, rx1 = arg
                nr = self._apply_seq_roll_slider(sc, p, st, rx0, rx1, x)
                self.drag_info = ("step_roll_slider", [sc, p, st, rx0, rx1, nr], y, 0)
                self.refresh_gui()
                return

            if action == "step_pitch_slider":
                sc, p, st, bx0, bx1 = arg
                tk = ("step_pitch", sc, p, st)
                if self.last_tap[0] == tk and (now - self.last_tap[1]) < 0.35:
                    self._commit_step_pitch(sc, p, st, 0)
                    self.last_tap = (None, 0.0)
                    self.refresh_gui()
                    return
                self.last_tap = (tk, now)
                nv = self._apply_step_pitch_slider(sc, p, st, bx0, bx1, x)
                self.drag_info = ("step_pitch_slider", [sc, p, st, bx0, bx1, nv], y, 0)
                self.refresh_gui()
                return

            if action == "step_filt_slider":
                sc, p, st, bx0, bx1 = arg
                tk = ("step_filt", sc, p, st)
                if self.last_tap[0] == tk and (now - self.last_tap[1]) < 0.35:
                    self._commit_step_filt(sc, p, st, 0)
                    self.last_tap = (None, 0.0)
                    self.refresh_gui()
                    return
                self.last_tap = (tk, now)
                nv = self._apply_step_filt_slider(sc, p, st, bx0, bx1, x)
                self.drag_info = ("step_filt_slider", [sc, p, st, bx0, bx1, nv], y, 0)
                self.refresh_gui()
                return

            if action == "step_micro_slider":
                sc, p, st, mx0, mx1 = arg
                tk = ("step_micro", sc, p, st)
                if self.last_tap[0] == tk and (now - self.last_tap[1]) < 0.35:
                    self._commit_step_micro(sc, p, st, 0)
                    self.last_tap = (None, 0.0)
                    self.refresh_gui()
                    return
                self.last_tap = (tk, now)
                nv = self._apply_step_micro_slider(sc, p, st, mx0, mx1, x)
                self.drag_info = ("step_micro_slider", [sc, p, st, mx0, mx1, nv], y, 0)
                self.refresh_gui()
                return

            if action == "step":
                sc, p, st = arg
                self.sel_step = st
                self._cancel_step_hold()
                self.drag_info = ("step_hold", (sc, p, st, x, y), now, 0)
                try:
                    self._step_hold_after = self.after(380, lambda s=sc, pi=p, ti=st: self._on_step_hold_timeout(s, pi, ti))
                except Exception:
                    self._step_hold_after = None
                return

            if action == "factory_kit":
                self._load_factory_kit()
                self.refresh_gui()
                return

            if action == "open_folder_kit":
                self._scan_kit_folders()
                self.folder_popup = True
                self.refresh_gui()
                return

            if action == "folder_nop":
                return

            if action == "folder_close":
                self.folder_popup = False
                self.refresh_gui()
                return

            if action == "folder_page":
                self.folder_page = max(0, self.folder_page + arg)
                self.refresh_gui()
                return

            if action == "folder_pick":
                self._load_folder_kit(arg)
                self.folder_popup = False
                self.refresh_gui()
                return

            if action == "open_notes":
                self.notes_popup = True
                self.notes_reset_confirm_until = 0.0
                self.notes_drag_override = None
                self.last_synced_row_notes = None
                self._select_group("g_notes")
                self.refresh_gui()
                return

            if action == "notes_nop":
                return

            if action == "notes_close":
                self._close_notes_popup(restore_group=True)
                self.refresh_gui()
                return

            if action == "notes_learn":
                self._zset("learn", arg)
                self.refresh_gui()
                return

            if action == "notes_reset":
                if now < self.notes_reset_confirm_until:
                    self.notes_reset_confirm_until = 0.0
                    self._send_cmd(CMD_SET_MOD_AMT, a=4, c=1)
                    self.last_synced_row_notes = None
                    self._toast("Pad notes reset to 36..51", secs=2.5)
                else:
                    self.notes_reset_confirm_until = now + 3.0
                    self._toast("Tap Def again to reset notes (36..51)", secs=3.0)
                self.refresh_gui()
                return

            if action == "notes_row":
                row_hw = arg & 3
                new_pad = row_hw * 4 + (self.sel_pad % 4)
                self.sel_pad = new_pad
                self._zset("sel_pad", new_pad)
                self.last_synced_pad = -1
                self.last_synced_row_notes = None
                self._select_group("g_notes")
                self.refresh_gui()
                return

            if action == "notes_pad":
                p = arg & 15
                cur_note = self._get_pad_note(p)
                self.sel_pad = p
                self._zset("sel_pad", p)
                self.last_synced_pad = -1
                self.last_synced_row_notes = None
                self._select_group("g_notes")
                self.drag_info = ("notes_pad_drag", [p, cur_note, False], y, cur_note)
                self.refresh_gui()
                return

            if action == "learn":
                self._zset("learn", arg)
                self.refresh_gui()
                return

            if action == "clr_scene":
                self._send_cmd(CMD_CLEAR_SCENE, a=arg)
                self.refresh_gui()
                return

            if action == "unload":
                self._send_cmd(CMD_CLEAR_PAD, b=arg, val=0xFF)
                self._zset("sample%d" % (arg + 1), "_")
                self.refresh_gui()
                return

            if action == "reset_all":
                if now < self.reset_confirm_until:
                    self.reset_confirm_until = 0.0
                    self._reset_all_defaults()
                else:
                    self.reset_confirm_until = now + 3.0
                    self._toast("Tap Reset All again to confirm", secs=3.0)
                self.refresh_gui()
                return

            if action == "flt_slider":
                cmd_op, vmax, step, sx0, sx1 = arg
                self.drag_info = ("flt_slider", arg, y, 0)
                self._apply_flt_slider(cmd_op, vmax, step, sx0, sx1, x)
                self.refresh_gui()
                return

            if action == "aux_slider":
                sym, vmin, vmax, ax0, ax1 = arg
                self.drag_info = ("aux_slider", arg, y, 0)
                self._apply_aux_slider(sym, vmin, vmax, ax0, ax1, x)
                self.refresh_gui()
                return

            if action == "aux_route":
                self._cycle_aux_target(arg)
                self.refresh_gui()
                return

            if action == "lfo_open":
                self.lfo_popup = arg
                self.refresh_gui()
                return

            if action == "lfo_nop":
                return

            if action == "lfo_close":
                self.lfo_popup = None
                self.refresh_gui()
                return

            if action == "lfo_sync_slider":
                li, p_i, sx0, sx1 = arg
                self.drag_info = ("lfo_sync_slider", arg, y, 0)
                self._apply_lfo_sync_slider(li, p_i, sx0, sx1, x)
                self.refresh_gui()
                return

            if action == "lfo_wave_slider":
                li, p_i, wx0, wx1 = arg
                self.drag_info = ("lfo_wave_slider", arg, y, 0)
                self._apply_lfo_wave_slider(li, p_i, wx0, wx1, x)
                self.refresh_gui()
                return

            if action == "lfo_rate":
                li, p_i, rx0, rx1, is_sync = arg
                self.drag_info = ("lfo_rate", arg, y, 0)
                self._apply_lfo_rate(li, p_i, rx0, rx1, x, is_sync)
                self.refresh_gui()
                return

            if action == "mod_src":
                sl, p_i, nsrc, dst_i = arg
                if nsrc > 0 and dst_i == 0:
                    dst_i = 2  # default to Decay when turning on a fresh slot!
                self._send_cmd(CMD_SET_MOD_SD, a=sl, b=p_i, c=nsrc, val=dst_i)
                self.refresh_gui()
                return

            if action == "mod_dst":
                sl, p_i, src_i, ndst = arg
                if ndst > 0 and src_i == 0:
                    src_i = 1  # default to Vel when picking a destination on an Off slot!
                self._send_cmd(CMD_SET_MOD_SD, a=sl, b=p_i, c=src_i, val=ndst)
                self.refresh_gui()
                return

            if action == "mod_amt":
                sl, p_i, ax0, ax1 = arg
                tap_key = ("mod_amt", sl, p_i)
                if self.last_tap[0] == tap_key and (now - self.last_tap[1]) < 0.35:
                    self._send_cmd(CMD_SET_MOD_AMT, a=sl, b=p_i, val=100)
                    if sl in (0, 1):
                        self._zset("pad_mod%d_amt" % (sl + 1), 0.0)
                    self.last_tap = (None, 0.0)
                    self.refresh_gui()
                    return
                self.last_tap = (tap_key, now)
                self.drag_info = ("mod_amt", [sl, p_i, ax0, ax1, -999], y, 0)
                self._apply_mod_amt(sl, p_i, ax0, ax1, x)
                self.refresh_gui()
                return

    def _apply_lfo_sync_slider(self, li, p_i, sx0, sx1, x):
        is_sync = 1 if x >= (sx0 + sx1) * 0.5 else 0
        self._send_cmd(CMD_SET_LFO, a=li, b=p_i, c=2, val=is_sync)

    def _apply_lfo_wave_slider(self, li, p_i, wx0, wx1, x):
        t = max(0.0, min(0.9999, (x - wx0) / max(1.0, wx1 - wx0)))
        wi = int(t * len(LFO_WAVES))
        self._send_cmd(CMD_SET_LFO, a=li, b=p_i, c=0, val=wi)

    def _apply_lfo_rate(self, li, p_i, rx0, rx1, x, is_sync=False):
        t = max(0.0, min(1.0, (x - rx0) / max(1.0, rx1 - rx0)))
        if is_sync:
            nd = len(LFO_SYNC_DIVS) - 1
            t = round(t * nd) / float(nd)
        v200 = int(round(t * 200.0))
        hz = 0.05 * (400.0 ** (v200 / 200.0))
        self._send_cmd(CMD_SET_LFO, a=li, b=p_i, c=1, val=v200)
        self._zset("pad_lfo%d_rate" % (li + 1), round(hz, 3))

    def _apply_mod_amt(self, sl, p_i, ax0, ax1, x):
        t = max(0.0, min(1.0, (x - ax0) / max(1.0, ax1 - ax0)))
        amt = int(round((t * 200.0 - 100.0) / 5.0)) * 5
        amt = max(-100, min(100, amt))
        self._send_cmd(CMD_SET_MOD_AMT, a=sl, b=p_i, val=amt + 100)
        if sl in (0, 1):
            self._zset("pad_mod%d_amt" % (sl + 1), float(amt))
        return amt

    def _apply_aux_slider(self, sym, vmin, vmax, ax0, ax1, x):
        t = max(0.0, min(1.0, (x - ax0) / max(1.0, ax1 - ax0)))
        step = 10.0 if (vmax - vmin) > 100.0 else 5.0
        raw = vmin + t * (vmax - vmin)
        val = float(round(raw / step) * step)
        val = max(vmin, min(vmax, val))
        self._zset(sym, val)

    def _apply_flt_slider(self, cmd_op, vmax, step, sx0, sx1, x):
        t = max(0.0, min(1.0, (x - sx0) / max(1.0, sx1 - sx0)))
        nsteps = max(1, vmax // step)
        val = int(round(t * nsteps)) * step
        val = max(0, min(vmax, val))
        self._send_cmd(cmd_op, val=val)

    def _calc_steps_val(self, which, sl_x0, sl_x1, x):
        t = max(0.0, min(1.0, (x - sl_x0) / max(1.0, sl_x1 - sl_x0)))
        if which == "beg":
            return max(0, min(EP_STEPS - 1, int(round(t * (EP_STEPS - 1)))))
        return max(1, min(EP_STEPS, 1 + int(round(t * (EP_STEPS - 1)))))

    def _commit_steps_val(self, sc, p, which, val):
        if which == "beg":
            self._send_cmd(CMD_SET_START, a=sc, b=p, val=val)
            self._zset("pad_beg", val + 1)
        else:
            self._send_cmd(CMD_SET_END, a=sc, b=p, val=val)
            self._zset("pad_steps", val)

    def _apply_wave_trim(self, which, wx0, wx1, x):
        pct = max(0.0, min(100.0, ((x - wx0) / max(1.0, wx1 - wx0)) * 100.0))
        if which == "start":
            self._zset("pad_start", round(min(99.0, pct), 1))
        else:
            self._zset("pad_end", int(round(max(1.0, pct))))

    def _cancel_wave_hold(self):
        if self._wave_hold_after is not None:
            try:
                self.after_cancel(self._wave_hold_after)
            except Exception:
                pass
            self._wave_hold_after = None

    def _open_sample_popup(self, pad_i):
        self._cancel_wave_hold()
        self.sel_pad = pad_i
        self._zset("sel_pad", pad_i)
        self.sample_popup = pad_i
        self.sample_popup_tab = 0
        self.sample_popup_vals = {}
        self.drag_info = None
        self.refresh_gui()

    def _on_wave_hold_timeout(self, pad_i):
        self._wave_hold_after = None
        if self.drag_info and self.drag_info[0] == "wave_hold":
            if self.drag_info[1][0] == pad_i:
                self._open_sample_popup(pad_i)

    def _calc_sample_slider(self, vmin, vmax, step, sx0, sx1, x):
        t = max(0.0, min(1.0, (x - sx0) / max(1.0, sx1 - sx0)))
        raw = vmin + t * (vmax - vmin)
        val = round(raw / step) * step
        return max(vmin, min(vmax, val))

    def _apply_sample_val(self, sym, val):
        p = self.sample_popup if self.sample_popup is not None else self.sel_pad
        if sym == "_pad_attack":
            self._send_cmd(CMD_SET_MOD_AMT, a=5, b=p, c=0, val=int(round(val)))
        elif sym == "_pad_reso":
            self._send_cmd(CMD_SET_MOD_AMT, a=5, b=p, c=1, val=int(round(val)))
        else:
            if sym == "pad_end":
                val = int(round(val))
            self._zset(sym, val)
        if self.sample_popup_vals is not None:
            self.sample_popup_vals[sym] = val
        return val

    def _apply_sample_slider(self, sym, vmin, vmax, step, sx0, sx1, x):
        val = self._calc_sample_slider(vmin, vmax, step, sx0, sx1, x)
        return self._apply_sample_val(sym, val)

    def _apply_pad_link_target(self, p_i, lx0, lx1, x):
        t = max(0.0, min(1.0, (x - lx0) / max(1.0, lx1 - lx0)))
        tgt = int(round(t * 16.0))
        if tgt == p_i + 1:
            tgt = 0 if t < 0.08 else (tgt + 1 if tgt < 16 else tgt - 1)
        tgt = max(0, min(16, tgt))
        self._send_cmd(CMD_SET_MOD_AMT, a=5, b=p_i, c=2, val=tgt)
        return tgt

    def _apply_euclid_hits(self, p_i, plen, ex0, ex1, x):
        t = max(0.0, min(1.0, (x - ex0) / max(1.0, ex1 - ex0)))
        hits = max(0, min(plen, int(round(t * plen))))
        self.euclid_hits = hits
        self._send_cmd(CMD_SET_MOD_AMT, a=7, b=p_i, c=self.euclid_rot % max(1, plen), val=hits)
        return hits

    def _apply_euclid_rot(self, p_i, plen, rx0, rx1, x):
        t = max(0.0, min(1.0, (x - rx0) / max(1.0, rx1 - rx0)))
        rot = max(0, min(max(0, plen - 1), int(round(t * max(0, plen - 1)))))
        self.euclid_rot = rot
        hits = min(self.euclid_hits, plen)
        self._send_cmd(CMD_SET_MOD_AMT, a=7, b=p_i, c=rot, val=hits)
        return rot

    def _apply_chain_len(self, cx0, cx1, x):
        t = max(0.0, min(1.0, (x - cx0) / max(1.0, cx1 - cx0)))
        cl = max(1, min(8, 1 + int(round(t * 7.0))))
        self._send_cmd(CMD_SET_MOD_AMT, a=6, c=7, val=cl)
        return cl

    def _apply_sample_seg(self, sym, count, sx0, sx1, x):
        t = max(0.0, min(0.9999, (x - sx0) / max(1.0, sx1 - sx0)))
        val = int(t * count)
        self._zset(sym, val)
        if self.sample_popup_vals is not None:
            self.sample_popup_vals[sym] = val
        return val

    def _apply_quant_slider(self, qx0, qx1, x):
        t = max(0.0, min(1.0, (x - qx0) / max(1.0, qx1 - qx0)))
        q = int(round(t * 20.0)) * 5  # clean 5% steps: 0, 5, 10 .. 100%
        self._send_cmd(CMD_SET_QUANT, val=q)

    def _cancel_step_hold(self):
        if self._step_hold_after is not None:
            try:
                self.after_cancel(self._step_hold_after)
            except Exception:
                pass
            self._step_hold_after = None

    def _open_step_popup(self, sc, p, st):
        self._cancel_step_hold()
        vel, roll, chance, micro, po, fo = self._step_get_plock(sc, p, st)
        if vel == 0:
            vel = 100
            self._send_cmd(CMD_SET_VEL, a=sc, b=p, c=st, val=vel)
        self.step_popup = (sc, p, st)
        self.step_popup_vals = [vel, chance, roll, micro, po, fo]
        self.step_drag_override = None
        self.drag_info = None
        self.refresh_gui()

    def _on_step_hold_timeout(self, sc, p, st):
        self._step_hold_after = None
        if self.drag_info and self.drag_info[0] == "step_hold":
            d_sc, d_p, d_st, _x0, _y0 = self.drag_info[1]
            if (d_sc, d_p, d_st) == (sc, p, st):
                self._open_step_popup(sc, p, st)

    def _calc_seq_vel(self, vx0, vx1, x):
        t = max(0.0, min(1.0, (x - vx0) / max(1.0, vx1 - vx0)))
        v = int(round(t * 127.0))
        return 0 if v <= 2 else min(127, v)

    def _apply_seq_vel_slider(self, sc, p, st, vx0, vx1, x):
        v = self._calc_seq_vel(vx0, vx1, x)
        self._send_cmd(CMD_SET_VEL, a=sc, b=p, c=st, val=v)
        if v == 0:
            self._send_cmd(CMD_SET_ROLL, a=sc, b=p, c=st, val=0)
        if self.step_popup_vals is not None:
            self.step_popup_vals[0] = v
            if v == 0:
                self.step_popup_vals[2] = 0
        return v

    def _calc_step_chance(self, cx0, cx1, x):
        t = max(0.0, min(1.0, (x - cx0) / max(1.0, cx1 - cx0)))
        ch = 10 + int(round(t * 18.0)) * 5  # 10..100% in 5% steps
        return max(10, min(100, ch))

    def _apply_step_chance_slider(self, sc, p, st, cx0, cx1, x):
        ch = self._calc_step_chance(cx0, cx1, x)
        if self.step_popup_vals is not None and self.step_popup_vals[0] == 0:
            self.step_popup_vals[0] = 100
            self._send_cmd(CMD_SET_VEL, a=sc, b=p, c=st, val=100)
        self._send_cmd(CMD_SET_ROLL, a=sc, b=p, c=st, val=0x80 | ch)
        if self.step_popup_vals is not None:
            self.step_popup_vals[1] = ch
        return ch

    def _calc_seq_roll(self, rx0, rx1, x):
        t = max(0.0, min(0.9999, (x - rx0) / max(1.0, rx1 - rx0)))
        return int(t * 4)

    def _apply_seq_roll_slider(self, sc, p, st, rx0, rx1, x):
        r = self._calc_seq_roll(rx0, rx1, x)
        vel = self.step_popup_vals[0] if self.step_popup_vals is not None else self._step_get(sc, p, st)[0]
        if vel == 0:
            self._send_cmd(CMD_SET_VEL, a=sc, b=p, c=st, val=100)
            if self.step_popup_vals is not None:
                self.step_popup_vals[0] = 100
        self._send_cmd(CMD_SET_ROLL, a=sc, b=p, c=st, val=r)
        if self.step_popup_vals is not None:
            self.step_popup_vals[2] = r
        return r

    def _commit_step_pitch(self, sc, p, st, po):
        po = max(-24, min(24, int(round(po))))
        self._send_cmd(CMD_SET_VEL, a=sc, b=p, c=st, val=0x80 | ((po + 24) & 0x3F))
        if self.step_popup_vals is not None and len(self.step_popup_vals) >= 6:
            self.step_popup_vals[4] = po
        return po

    def _apply_step_pitch_slider(self, sc, p, st, bx0, bx1, x):
        t = max(0.0, min(1.0, (x - bx0) / max(1.0, bx1 - bx0)))
        po = int(round(t * 48.0 - 24.0))
        return self._commit_step_pitch(sc, p, st, po)

    def _commit_step_filt(self, sc, p, st, fo):
        fo = max(-100, min(100, int(round(fo / 2.0)) * 2))
        self._send_cmd(CMD_SET_ROLL, a=sc, b=p, c=st, val=60 + (fo // 2))
        if self.step_popup_vals is not None and len(self.step_popup_vals) >= 6:
            self.step_popup_vals[5] = fo
        return fo

    def _apply_step_filt_slider(self, sc, p, st, bx0, bx1, x):
        t = max(0.0, min(1.0, (x - bx0) / max(1.0, bx1 - bx0)))
        fo = int(round((t * 200.0 - 100.0) / 4.0)) * 4
        return self._commit_step_filt(sc, p, st, fo)

    def _commit_step_micro(self, sc, p, st, m):
        m = max(-30, min(30, int(round(m))))
        self._send_cmd(CMD_SET_VEL, a=sc, b=p, c=st, val=0xC0 | ((m + 30) & 0x3F))
        if self.step_popup_vals is not None and len(self.step_popup_vals) >= 6:
            self.step_popup_vals[3] = m
        return m

    def _apply_step_micro_slider(self, sc, p, st, mx0, mx1, x):
        t = max(0.0, min(1.0, (x - mx0) / max(1.0, mx1 - mx0)))
        m = int(round(t * 60.0 - 30.0))
        return self._commit_step_micro(sc, p, st, m)

    def _on_motion(self, event):
        if not self.drag_info or event is None:
            return
        kind, arg, y0, start_val = self.drag_info
        if kind == "notes_pad_drag":
            p, last_n, moved = arg
            dy = y0 - event.y
            if not moved and abs(dy) < 10:
                return
            arg[2] = True
            nn = max(0, min(127, int(round(start_val + dy / 8.0))))
            if nn != last_n:
                arg[1] = nn
                self.notes_drag_override = (p, nn)
                self._send_cmd(CMD_SET_MOD_AMT, a=4, b=p, c=0, val=nn)
                if self.shm_hdr and ((self.shm_hdr[3] >> 2) & 3) == ((p >> 2) & 3):
                    self._zset("pad_note%d" % ((p % 4) + 1), nn)
                self.refresh_gui()
            return
        if kind == "step_hold":
            sc, p, st, x0, hy0 = arg
            dx = abs(event.x - x0)
            dy = abs(event.y - hy0)
            if dy >= 10 and dy >= dx:
                self._cancel_step_hold()
                cur_vel, _r, _ch = self._step_get(sc, p, st)
                self.drag_info = ("step_vel_drag", [sc, p, st, cur_vel], event.y, cur_vel)
                self.step_drag_override = (sc, p, st, cur_vel)
                self.refresh_gui()
            elif dx > 18:
                self._cancel_step_hold()
                self.drag_info = None
            return
        if kind == "step_vel_drag":
            sc, p, st, last_v = arg
            dy = y0 - event.y
            nv = max(0, min(127, int(round(start_val + dy * 1.5))))
            if nv != last_v:
                arg[3] = nv
                self.step_drag_override = (sc, p, st, nv)
                self._send_cmd(CMD_SET_VEL, a=sc, b=p, c=st, val=nv)
                if nv == 0:
                    self._send_cmd(CMD_SET_ROLL, a=sc, b=p, c=st, val=0)
                self.refresh_gui()
            return
        if kind == "step_vel_slider":
            sc, p, st, vx0, vx1, last_v = arg
            nv = self._calc_seq_vel(vx0, vx1, event.x)
            if nv != last_v:
                arg[5] = nv
                self._apply_seq_vel_slider(sc, p, st, vx0, vx1, event.x)
                self.refresh_gui()
            return
        if kind == "step_chance_slider":
            sc, p, st, cx0, cx1, last_c = arg
            nc = self._calc_step_chance(cx0, cx1, event.x)
            if nc != last_c:
                arg[5] = nc
                self._apply_step_chance_slider(sc, p, st, cx0, cx1, event.x)
                self.refresh_gui()
            return
        if kind == "step_roll_slider":
            sc, p, st, rx0, rx1, last_r = arg
            nr = self._calc_seq_roll(rx0, rx1, event.x)
            if nr != last_r:
                arg[5] = nr
                self._apply_seq_roll_slider(sc, p, st, rx0, rx1, event.x)
                self.refresh_gui()
            return
        if kind == "step_pitch_slider":
            sc, p, st, bx0, bx1, last_v = arg
            t = max(0.0, min(1.0, (event.x - bx0) / max(1.0, bx1 - bx0)))
            nv = int(round(t * 48.0 - 24.0))
            if nv != last_v:
                arg[5] = nv
                self._commit_step_pitch(sc, p, st, nv)
                self.refresh_gui()
            return
        if kind == "step_filt_slider":
            sc, p, st, bx0, bx1, last_v = arg
            t = max(0.0, min(1.0, (event.x - bx0) / max(1.0, bx1 - bx0)))
            nv = int(round((t * 200.0 - 100.0) / 4.0)) * 4
            if nv != last_v:
                arg[5] = nv
                self._commit_step_filt(sc, p, st, nv)
                self.refresh_gui()
            return
        if kind == "step_micro_slider":
            sc, p, st, mx0, mx1, last_v = arg
            t = max(0.0, min(1.0, (event.x - mx0) / max(1.0, mx1 - mx0)))
            nv = int(round(t * 60.0 - 30.0))
            if nv != last_v:
                arg[5] = nv
                self._commit_step_micro(sc, p, st, nv)
                self.refresh_gui()
            return
        if kind == "slice_sens":
            sx0, sx1 = arg
            t = max(0.0, min(1.0, (event.x - sx0) / max(1.0, sx1 - sx0)))
            self.slice_sens = int(round(t * 20.0)) * 5
            self.refresh_gui()
            return
        if kind == "pad_link_target":
            p_i, lx0, lx1 = arg
            self._apply_pad_link_target(p_i, lx0, lx1, event.x)
            self.refresh_gui()
            return
        if kind == "euclid_hits":
            p_i, plen, ex0, ex1, last_h = arg
            t = max(0.0, min(1.0, (event.x - ex0) / max(1.0, ex1 - ex0)))
            nh = max(0, min(plen, int(round(t * plen))))
            if nh != last_h:
                arg[4] = nh
                self._apply_euclid_hits(p_i, plen, ex0, ex1, event.x)
                self.refresh_gui()
            return
        if kind == "euclid_rot":
            p_i, plen, rx0, rx1, last_r = arg
            t = max(0.0, min(1.0, (event.x - rx0) / max(1.0, rx1 - rx0)))
            nr = max(0, min(max(0, plen - 1), int(round(t * max(0, plen - 1)))))
            if nr != last_r:
                arg[4] = nr
                self._apply_euclid_rot(p_i, plen, rx0, rx1, event.x)
                self.refresh_gui()
            return
        if kind == "chain_len_slider":
            cx0, cx1, last_l = arg
            t = max(0.0, min(1.0, (event.x - cx0) / max(1.0, cx1 - cx0)))
            nl = max(1, min(8, 1 + int(round(t * 7.0))))
            if nl != last_l:
                arg[2] = nl
                self._apply_chain_len(cx0, cx1, event.x)
                self.refresh_gui()
            return
        if kind == "quant_slider":
            qx0, qx1 = arg
            self._apply_quant_slider(qx0, qx1, event.x)
            self.refresh_gui()
            return
        if kind == "flt_slider":
            cmd_op, vmax, step, sx0, sx1 = arg
            self._apply_flt_slider(cmd_op, vmax, step, sx0, sx1, event.x)
            self.refresh_gui()
            return
        if kind == "aux_slider":
            sym, vmin, vmax, ax0, ax1 = arg
            self._apply_aux_slider(sym, vmin, vmax, ax0, ax1, event.x)
            self.refresh_gui()
            return
        if kind == "lfo_sync_slider":
            li, p_i, sx0, sx1 = arg
            self._apply_lfo_sync_slider(li, p_i, sx0, sx1, event.x)
            self.refresh_gui()
            return
        if kind == "lfo_wave_slider":
            li, p_i, wx0, wx1 = arg
            self._apply_lfo_wave_slider(li, p_i, wx0, wx1, event.x)
            self.refresh_gui()
            return
        if kind == "lfo_rate":
            li, p_i, rx0, rx1, is_sync = arg
            self._apply_lfo_rate(li, p_i, rx0, rx1, event.x, is_sync)
            self.refresh_gui()
            return
        if kind == "mod_amt":
            sl, p_i, ax0, ax1, last_a = arg
            t = max(0.0, min(1.0, (event.x - ax0) / max(1.0, ax1 - ax0)))
            amt = max(-100, min(100, int(round((t * 200.0 - 100.0) / 5.0)) * 5))
            if amt != last_a:
                arg[4] = amt
                self._apply_mod_amt(sl, p_i, ax0, ax1, event.x)
                self.refresh_gui()
            return
        if kind == "steps_slider":
            sc, p, which, sl_x0, sl_x1, x0, start_st, last_st = arg
            px_per_step = max(8.0, (sl_x1 - sl_x0) / float(EP_STEPS - 1))
            delta = int(round((event.x - x0) / px_per_step))
            if which == "beg":
                nv = max(0, min(EP_STEPS - 1, start_st + delta))
            else:
                nv = max(1, min(EP_STEPS, start_st + delta))
            if nv != last_st:
                arg[7] = nv
                self._commit_steps_val(sc, p, which, nv)
                self.refresh_gui()
            return
        if kind == "wave_hold":
            pad_i, which, wx0, wx1, x0, hy0 = arg
            if abs(event.x - x0) >= 8:
                self._cancel_wave_hold()
                self.drag_info = ("wave_trim", (pad_i, which, wx0, wx1), hy0, 0)
                self._apply_wave_trim(which, wx0, wx1, event.x)
                self.refresh_gui()
            elif abs(event.y - hy0) > 18:
                self._cancel_wave_hold()
                self.drag_info = None
            return
        if kind == "wave_trim":
            _pad_i, which, wx0, wx1 = arg
            self._apply_wave_trim(which, wx0, wx1, event.x)
            self.refresh_gui()
            return
        if kind == "sample_slider":
            sym, vmin, vmax, step, sx0, sx1, last_v = arg
            nv = self._calc_sample_slider(vmin, vmax, step, sx0, sx1, event.x)
            if sym == "pad_end":
                nv = int(round(nv))
            if nv != last_v:
                arg[6] = nv
                self._apply_sample_slider(sym, vmin, vmax, step, sx0, sx1, event.x)
                self.refresh_gui()
            return
        if kind == "sample_mode_slider":
            mx0, mx1, last_v = arg
            t = max(0.0, min(0.9999, (event.x - mx0) / max(1.0, mx1 - mx0)))
            nv = int(t * 5)
            if nv != last_v:
                arg[2] = nv
                self._apply_sample_seg("pad_mode", 5, mx0, mx1, event.x)
                self.refresh_gui()
            return
        if kind == "sample_choke_slider":
            kx0, kx1, last_v = arg
            t = max(0.0, min(0.9999, (event.x - kx0) / max(1.0, kx1 - kx0)))
            nv = int(t * 5)
            if nv != last_v:
                arg[2] = nv
                self._apply_sample_seg("pad_choke", 5, kx0, kx1, event.x)
                self.refresh_gui()
            return
        if kind == "perf_xy":
            px0, py0, px1, py1, last_f, last_r = arg
            tx = max(0.0, min(1.0, (event.x - px0) / max(1.0, px1 - px0)))
            ty = max(0.0, min(1.0, (event.y - py0) / max(1.0, py1 - py0)))
            nf = int(round((tx * 200.0 - 100.0) / 2.0)) * 2
            if abs(nf) <= 3:
                nf = 0
            nr = int(round(((1.0 - ty) * 100.0) / 2.0)) * 2
            if nf != last_f or nr != last_r:
                arg[4] = nf
                arg[5] = nr
                self._send_perf_xy(nf, nr, persistent=True)
                self.refresh_gui()
            return
        if kind in ("seq_pad_hold", "pad_hold", "pad_16lvl_hold", "scene_hold", "clr_hold",
                    "choke_hold", "perf_fx_hold", "perf_break_hold", "perf_throw_hold"):
            return
        dy = y0 - event.y
        if abs(dy) < 4:
            return
        self.last_tap = (None, 0.0)
        if kind == "bpm":
            nbpm = max(40, min(240, int(round(start_val + dy * 0.5))))
            self._zset("seq_bpm", nbpm)
            self.refresh_gui()

    def _on_release(self, event):
        self._cancel_step_hold()
        self._cancel_wave_hold()
        self._cancel_choke_hold()
        if self.drag_info:
            kind = self.drag_info[0]
            if kind == "choke_hold":
                _, grp, t0, _ = self.drag_info
                self.drag_info = None
                cm, cs = self._get_choke_masks()
                if (time.monotonic() - t0) >= 0.35:
                    new_s = cs ^ (1 << grp)
                    self._set_choke_masks(solo_mask=new_s)
                    self._toast("Solo G%d" % (grp + 1) if (new_s & (1 << grp)) else "Solo G%d Off" % (grp + 1), secs=1.5)
                else:
                    new_m = cm ^ (1 << grp)
                    if cs & (1 << grp):
                        self._set_choke_masks(mute_mask=new_m, solo_mask=cs & ~(1 << grp))
                    else:
                        self._set_choke_masks(mute_mask=new_m)
                self.refresh_gui()
                return
            if kind == "perf_xy":
                _, (_px0, _py0, _px1, _py1, last_f, last_r), _y0, _ = self.drag_info
                self.drag_info = None
                if not self.xy_hold_mode:
                    self._send_perf_xy(0, 20, persistent=False)
                else:
                    self.perf_xy_override = (float(last_f), float(last_r), time.monotonic() + 0.30)
                self.refresh_gui()
                return
            if kind == "perf_fx_hold":
                self.drag_info = None
                self.perf_fx_local = 0
                self._send_cmd(CMD_SET_MOD_AMT, a=6, c=15, val=0)
                self.refresh_gui()
                return
            if kind == "perf_break_hold":
                self.drag_info = None
                self.perf_break_local = False
                self._send_perf_flags()
                self.refresh_gui()
                return
            if kind == "perf_throw_hold":
                self.drag_info = None
                self.perf_throw_local = False
                self._send_perf_flags()
                self.refresh_gui()
                return
            if kind == "notes_pad_drag":
                _, (p, _last_n, moved), _y0, _sv = self.drag_info
                self.drag_info = None
                self.notes_drag_override = None
                if not moved:
                    self._send_cmd(CMD_TRIG_PAD, b=p, val=110)
                self.refresh_gui()
                return
            if kind == "wave_hold":
                _, (pad_i, which, wx0, wx1, x0, _y0), t0, _ = self.drag_info
                self.drag_info = None
                if (time.monotonic() - t0) >= 0.38:
                    self._open_sample_popup(pad_i)
                else:
                    self._apply_wave_trim(which, wx0, wx1, x0)
                    self.refresh_gui()
                return
            if kind == "step_vel_drag":
                self.drag_info = None
                self.step_drag_override = None
                self.refresh_gui()
                return
            if kind == "step_hold":
                _, (sc, p, st, _x0, _y0), t0, _ = self.drag_info
                self.drag_info = None
                if (time.monotonic() - t0) >= 0.38:
                    self._open_step_popup(sc, p, st)
                else:
                    vel, _roll, _ch = self._step_get(sc, p, st)
                    new_vel = 100 if vel == 0 else 0
                    self._send_cmd(CMD_SET_VEL, a=sc, b=p, c=st, val=new_vel)
                    if new_vel == 0:
                        self._send_cmd(CMD_SET_ROLL, a=sc, b=p, c=st, val=0)
                        self._send_cmd(CMD_SET_ROLL, a=sc, b=p, c=st, val=0x80 | 100)
                        self._send_cmd(CMD_SET_VEL, a=sc, b=p, c=st, val=0x80 | 24)  # pitch 0
                        self._send_cmd(CMD_SET_ROLL, a=sc, b=p, c=st, val=60)        # filter 0
                        self._send_cmd(CMD_SET_VEL, a=sc, b=p, c=st, val=0xC0 | 30)  # micro 0
                    self.refresh_gui()
                return
            if kind == "pad_16lvl_hold":
                _, slot_i, _t0, _ = self.drag_info
                self.drag_info = None
                self._send_cmd(CMD_SET_MOD_AMT, a=6, b=slot_i, c=10, val=0)
                return
            if kind == "scene_hold":
                _, sc_idx, t0, _ = self.drag_info
                self.drag_info = None
                smode = self.shm_hdr[6] if self.shm_hdr else int(round(self._zval("seq_mode", 0)))
                if (time.monotonic() - t0) >= 0.35 and smode >= 1:
                    self._send_cmd(CMD_SET_MOD_AMT, a=6, c=5, val=sc_idx)
                    self._toast("1-Bar Fill: Sc %s" % SCENE_NAMES[sc_idx & 7], secs=1.8)
                else:
                    self._zset("seq_scene", sc_idx + 1)
                self.refresh_gui()
                return
            if kind == "clr_hold":
                _, (sc, p), t0, _ = self.drag_info
                self.drag_info = None
                undo_av = int(self.shm_hdr[23]) if (self.shm_hdr and len(self.shm_hdr) >= 30) else 0
                if (time.monotonic() - t0) >= 0.40 and undo_av:
                    self._send_cmd(CMD_SET_MOD_AMT, a=6, c=4, val=0)
                    self._toast("Undo last dub", secs=1.8)
                else:
                    self._send_cmd(CMD_CLEAR_PAD, a=sc, b=p)
                self.refresh_gui()
                return
            if kind == "pad_hold":
                _, pad_i, t0, _ = self.drag_info
                self.drag_info = None
                self._send_cmd(CMD_SET_MOD_AMT, a=5, b=pad_i, c=10, val=0)
                rpt_div = int(self.shm_hdr[19]) if (self.shm_hdr and len(self.shm_hdr) >= 30) else 0
                if rpt_div == 0 and (time.monotonic() - t0) >= 0.40:
                    mask = int(round(self._zval("mute_mask", 0)))
                    self._zset("mute_mask", mask ^ (1 << pad_i))
                    self.refresh_gui()
                return
            if kind == "seq_pad_hold":
                _, pad_i, t0, _ = self.drag_info
                if (time.monotonic() - t0) >= 0.40:
                    mask = int(round(self._zval("mute_mask", 0)))
                    self._zset("mute_mask", mask ^ (1 << pad_i))
                    self.refresh_gui()
        self.drag_info = None
