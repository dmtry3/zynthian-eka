#!/usr/bin/python3
# -*- coding: utf-8 -*-
# Eka Pads widget for Zynthian: Bitwig Drum Machine-style 4x4 pad grid,
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
EP_SCENE_SIZE = EP_PADS * EP_STEPS * 2 + EP_PADS * 2   # 1056 bytes
EP_SHM_MAGIC = 0x45504144
EP_SHM_HDR_FMT = "<IIIiiiiiiiffiIiiii"
EP_SHM_HDR_SIZE = struct.calcsize(EP_SHM_HDR_FMT)      # 72 bytes
EP_PAD_FMT = "<ffffffiffffifiiiffffiiffiiiiiiiiffff"
EP_PAD_SIZE = struct.calcsize(EP_PAD_FMT)              # 144 bytes
EP_SHM_SIZE = EP_SHM_HDR_SIZE + EP_PADS * EP_PAD_SIZE + EP_SCENES * EP_SCENE_SIZE

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

LEN_PRESETS = (4, 8, 12, 16, 24, 32)
QUANT_NAMES = ("Off", "1/16", "1/8", "1/4")

TABS = ["Pads", "Seq", "Mod", "Kit"]
TAB_GROUPS = ["g_sound", "g_seq", "g_mod", "g_mix"]
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
    return ((op & 0xF) << 21) | ((a & 0xF) << 17) | ((b & 0xF) << 13) | ((c & 0x1F) << 8) | (val & 0xFF)


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
        self.last_hit_seq = 0
        self.len_dragged = False
        self.pads_data = None      # list of 16 tuples (pitch, decay, filter, level, start, end, mode, punch, drive, pan, vel_sens, choke, send, mute, solo, midi_note)
        self.scenes_data = None    # bytes of length 8*16*16*2

        self.sel_pad = 0
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
            self.shm_hdr = hdr
            seq = hdr[2]
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
            return 0, 0
        idx = scene * EP_SCENE_SIZE + (pad * EP_STEPS + step) * 2
        if idx + 1 >= len(self.scenes_data):
            return 0, 0
        return self.scenes_data[idx], self.scenes_data[idx + 1]

    def _step_get(self, scene, pad, step):
        vel, rb = self._step_get_raw(scene, pad, step)
        return vel, rb & 3

    def _pad_range_get(self, scene, pad):
        if not self.scenes_data:
            return 0, 16
        base = scene * EP_SCENE_SIZE + EP_PADS * EP_STEPS * 2
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
            rstr = ";".join(t[0] if t else "" for t in self.aux_targets)
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
        self.last_aux_routes_str = val
        if not val or val == "_":
            return
        parts = val.split(";")
        targets = self._list_mixbus_targets()
        by_id = {t[0]: t for t in targets}
        for ai in range(4):
            tid = parts[ai] if ai < len(parts) else ""
            if tid and tid in by_id:
                self._apply_aux_route(ai, by_id[tid], persist=False)

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
                vel, rb = self._step_get_raw(scene, p, ps + rel)
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
        self._zset("seq_cmd", pack_cmd(op, a, b, c, val))

    def _load_factory_kit(self):
        kit_dir = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "samples")
        if not os.path.isdir(kit_dir):
            return
        wavs = sorted(f for f in os.listdir(kit_dir) if f.lower().endswith(".wav"))
        for i, fn in enumerate(wavs[:EP_PADS]):
            full = os.path.join(kit_dir, fn)
            self._zset("sample%d" % (i + 1), full)

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
        if group == "g_seq" and self.tab != 1:
            self.tab = 1
        elif group == "g_mod" and self.tab != 2:
            self.tab = 2
        elif group in ("g_sound", "g_shape", "g_mix") and self.tab in (1, 2):
            self.tab = 0

    # ---------- Drawing ----------

    def _font_size(self):
        return max(8, min(max(self.height, 40) // 18, 12))

    def refresh_gui(self):
        if not self.processor:
            return
        self._follow_page()
        self._read_shm()
        self._ensure_aux_routes()

        # Sync seq_mode zctrl whenever host Transport Start/Stop changes plugin sequencer mode
        if self.shm_hdr:
            cur_mode = self.shm_hdr[6]
            if cur_mode != self.last_shm_mode:
                self._zset("seq_mode", cur_mode)
                self.last_shm_mode = cur_mode

            # In Pads / Seq / Mod tabs, hitting any physical MIDI pad switches to that pad for editing
            hit_seq = self.shm_hdr[13]
            last_hit = self.shm_hdr[14]
            if hit_seq != self.last_hit_seq:
                self.last_hit_seq = hit_seq
                if self.tab in (0, 1, 2) and 0 <= last_hit < EP_PADS and last_hit != self.sel_pad:
                    self.sel_pad = last_hit
                    self._zset("sel_pad", last_hit)
                    self.last_synced_pad = -1

        # Track Auto-Select pad & scene from shared memory / plugin monitor
        shm_pad = self.shm_hdr[3] if self.shm_hdr else int(round(self._mon("out_sel_pad", self.sel_pad)))
        if 0 <= shm_pad < EP_PADS:
            self.sel_pad = shm_pad
        cur_scene = int(round(self._mon("out_seq_scene", 0)))
        if (self.sel_pad != self.last_synced_pad or cur_scene != self.last_synced_scene) and self.pads_data is not None:
            self.last_synced_pad = self.sel_pad
            self.last_synced_scene = cur_scene
            self._sync_encoders_to_pad(self.sel_pad)

        w, h = max(int(self.width), 160), max(int(self.height), 120)
        c = self.widget_canvas
        c.delete("all")
        self.hit_boxes = []

        fam = zynthian_gui_config.font_family
        fs = self._font_size()
        top_h = max(36, fs * 3 + 8)

        self._draw_topbar(c, w, top_h, fam, fs)

        if self.tab == 0:
            self._draw_pads_tab(c, w, h, top_h, fam, fs)
        elif self.tab == 1:
            self._draw_seq_tab(c, w, h, top_h, fam, fs)
        elif self.tab == 2:
            self._draw_mod_tab(c, w, h, top_h, fam, fs)
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
        bw = max(62, int(w * 0.135))
        tab_fs = fs + 1
        for i, name in enumerate(TABS):
            sel = (i == self.tab)
            c.create_rectangle(x, 2, x + bw, top_h - 3,
                               fill=COL_ACCENT if sel else COL_PANEL,
                               outline=COL_ACCENT if sel else "#444452", width=2 if sel else 1)
            c.create_text(x + bw / 2, (top_h - 1) / 2, text=name,
                          fill="#000000" if sel else "#f0f0f5", font=(fam, tab_fs, "bold"))
            self.hit_boxes.append((x, 2, x + bw, top_h - 3, "tab", i))
            x += bw + 6

        # Status pills on top right: Scene + BPM + Seq state
        scene = int(round(self._mon("out_seq_scene", 0)))
        pend = int(round(self._mon("out_seq_pend", -1)))
        bpm = int(round(self.shm_hdr[10])) if self.shm_hdr else int(round(self._zval("seq_bpm", 120)))
        smode = self.shm_hdr[6] if self.shm_hdr else int(round(self._zval("seq_mode", 0)))
        st_txt = "REC" if smode == 2 else ("PLAY" if smode == 1 else "STOP")
        st_col = COL_REC if smode == 2 else (COL_PLAY if smode == 1 else COL_DIM)
        sc_txt = "Sc %s" % SCENE_NAMES[scene & 7]
        if 0 <= pend < EP_SCENES:
            sc_txt += "->%s" % SCENE_NAMES[pend]
        info = "%s  %d BPM  [%s]" % (sc_txt, bpm, st_txt)
        c.create_text(w - 6, top_h / 2, anchor="e", text=info, fill=st_col, font=(fam, max(8, fs - 1)))

    def _draw_pads_tab(self, c, w, h, top_h, fam, fs):
        pad_m = 4
        y0 = top_h + pad_m
        split_x = int(w * 0.52)

        # ---- Left: 4x4 Hardware Pad Grid (bottom row = Pads 1..4, top row = Pads 13..16) ----
        gw = split_x - pad_m * 2
        gh = h - y0 - pad_m
        cw = gw / 4.0
        ch = gh / 4.0
        fsmall = max(7, fs - 2)

        for p in range(EP_PADS):
            col_i = p % 4
            row_hw = p // 4               # 0 = bottom, 3 = top
            row_ui = 3 - row_hw
            px0 = pad_m + col_i * cw + 2
            py0 = y0 + row_ui * ch + 2
            px1 = pad_m + (col_i + 1) * cw - 2
            py1 = y0 + (row_ui + 1) * ch - 2

            lvl = self._mon("out_pad_lvl%d" % (p + 1), 0.0)
            is_sel = (p == self.sel_pad)
            pinfo = self.pads_data[p] if self.pads_data else None
            mute = pinfo[13] if pinfo else 0
            solo = pinfo[14] if pinfo else 0
            choke = pinfo[11] if pinfo else 0

            bg = COL_PAD_SEL if is_sel else COL_PAD
            if lvl > 0.05:
                bg = "#4c6ef5" if is_sel else "#3b4261"
            accent = ROW_COLORS[row_hw]
            outline = "#ffffff" if is_sel else ("#ffd43b" if solo else "#3e3e4a")

            c.create_rectangle(px0, py0, px1, py1, fill=bg, outline=outline, width=2 if is_sel else 1)
            # Top color bar
            c.create_rectangle(px0 + 1, py0 + 1, px1 - 1, py0 + 5, fill=accent if not mute else "#444444", width=0)
            # Activity bar on bottom
            if lvl > 0.02:
                bar_w = max(2, int((px1 - px0 - 4) * min(lvl, 1.0)))
                c.create_rectangle(px0 + 2, py1 - 4, px0 + 2 + bar_w, py1 - 1, fill="#ffd43b", width=0)

            name = short_sample_name(self._pad_path(p), p)
            c.create_text((px0 + px1) / 2, (py0 + py1) / 2 + 1, text=name,
                          fill="#888888" if mute else "#f0f0f4", font=(fam, fsmall))
            # Pad index top-left, Mute/Solo/Choke top-right
            c.create_text(px0 + 3, py0 + 7, anchor="nw", text=str(p + 1), fill="#9090a0", font=(fam, max(6, fsmall - 1)))
            badge = ("S" if solo else "") + ("M" if mute else "") + (("C%d" % choke) if choke > 0 else "")
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

        # Row 1: Action buttons (Load, Mute, Solo, Follow)
        pinfo = self.pads_data[self.sel_pad] if self.pads_data else None
        mute = pinfo[13] if pinfo else 0
        solo = pinfo[14] if pinfo else 0
        follow = self.shm_hdr[4] if self.shm_hdr else int(round(self._zval("follow", 1)))

        bw4 = (rw - 9) / 4.0
        self._add_btn(c, rx0, y0, rx0 + bw4, y0 + bh, "Load", COL_ACCENT, "#000000", fam, fsmall, "load", self.sel_pad)
        self._add_btn(c, rx0 + bw4 + 3, y0, rx0 + 2 * bw4 + 3, y0 + bh, "Mute",
                      COL_REC if mute else COL_PANEL, "#000000" if mute else "#e0e0e0", fam, fsmall, "mute", self.sel_pad)
        self._add_btn(c, rx0 + 2 * bw4 + 6, y0, rx0 + 3 * bw4 + 6, y0 + bh, "Solo",
                      COL_PEND if solo else COL_PANEL, "#000000" if solo else "#e0e0e0", fam, fsmall, "solo", self.sel_pad)
        self._add_btn(c, rx0 + 3 * bw4 + 9, y0, rx1, y0 + bh, "Auto" if follow else "Lock",
                      COL_PLAY if follow else COL_PANEL, "#000000" if follow else "#e0e0e0", fam, fsmall, "follow", 1 - follow)

        # Waveform box
        wy0 = y0 + bh + 5
        sc_h = max(24, fs * 2 + 4)
        aux_h = max(18, fs + 8)
        info_h = max(16, fs + 6)
        wy1 = max(wy0 + 28, h - pad_m - sc_h - aux_h - info_h - 12)
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
        sy0 = h - pad_m - max(sc_h, fs * 2 + 12)
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
                          bg, fg, fam, fs, "scene", s)

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

        # Row 1: 5 Scene buttons (A..E) + Quantize + Copy + Clear + ->ZSeq
        r1_h = max(28, fs * 2 + 8)
        scenes_w = (w - 2 * pad_m) * 0.48
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
            self._add_btn(c, x0, y, x1, y + r1_h, SCENE_NAMES[s], bg, fg, fam, fs, "seq_scene", s)

        tools_x = pad_m + scenes_w + 2
        tw4 = (w - pad_m - tools_x) / 4.0
        # Horizontal Quantize strength slider (0..100%)
        qx0, qx1 = tools_x, tools_x + tw4 - 3
        c.create_rectangle(qx0, y, qx1, y + r1_h, fill="#1a1c24", outline="#3c4a46" if quant > 0 else "#383844", width=1)
        if quant > 0:
            q_fill_x = qx0 + 1 + (quant / 100.0) * max(1.0, (qx1 - qx0 - 2))
            c.create_rectangle(qx0 + 1, y + 1, q_fill_x, y + r1_h - 1, fill="#24483d", outline="")
            c.create_rectangle(qx0 + 1, y + 1, q_fill_x, y + 4, fill="#63e6be", width=0)
        q_lbl = "Q%d" % quant if quant > 0 else "Q Off"
        c.create_text((qx0 + qx1) / 2, y + r1_h / 2, text=q_lbl,
                      fill="#63e6be" if quant > 0 else "#9a9aaa", font=(fam, fsmall, "bold"))
        self.hit_boxes.append((qx0, y, qx1, y + r1_h, "quant_slider", (qx0, qx1)))
        self._add_btn(c, tools_x + tw4, y, tools_x + 2 * tw4 - 3, y + r1_h,
                      "Pick" if self.copy_src >= 0 else "Copy",
                      "#e64980" if self.copy_src >= 0 else COL_PANEL, "#ffffff", fam, fsmall, "seq_copy", scene)
        self._add_btn(c, tools_x + 2 * tw4, y, tools_x + 3 * tw4 - 3, y + r1_h,
                      "Clr", COL_PANEL, "#ff8787", fam, fsmall, "seq_clr_pad", (scene, self.sel_pad))
        self._add_btn(c, tools_x + 3 * tw4, y, w - pad_m, y + r1_h,
                      "->ZSeq", "#1d4034", "#63e6be", fam, fsmall, "export_zseq", scene, outline="#38d9a9")
        y += r1_h + 4

        # Row 2: Play/Stop + Overdub + BPM + Horizontal Steps Slider (Beg..End 1..32)
        r2_h = max(26, fs * 2 + 6)
        ctrl_w = (w - 2 * pad_m) * 0.44
        b3w = ctrl_w / 3.0
        self._add_btn(c, pad_m, y, pad_m + b3w - 3, y + r2_h,
                      "Stop" if smode >= 1 else "Play",
                      COL_PLAY if smode == 1 else COL_PANEL,
                      "#000000" if smode == 1 else "#e0e0e0", fam, fsmall, "seq_play", 0 if smode >= 1 else 1)
        self._add_btn(c, pad_m + b3w, y, pad_m + 2 * b3w - 3, y + r2_h,
                      "Rec" if smode == 2 else "Dub",
                      COL_REC if smode == 2 else COL_PANEL,
                      "#ffffff" if smode == 2 else "#e0e0e0", fam, fsmall, "seq_rec", 1 if smode == 2 else 2)
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

        # Middle: 32 Styled LED Step Tiles (4 rows of 8, grouped 4 + 4) for selected pad
        strip_h = max(24, fs * 2 + 4)
        grid_h = max(72, h - y - strip_h - pad_m - 6)
        mid_gap = 6.0
        sw = (w - 2 * pad_m - mid_gap) / 8.0
        sh = grid_h / 4.0

        for st in range(EP_STEPS):
            col_i = st % 8
            row_i = st // 8
            gx_off = mid_gap if col_i >= 4 else 0.0
            sx0 = pad_m + col_i * sw + gx_off + 2
            sy0 = y + row_i * sh + 2
            sx1 = pad_m + (col_i + 1) * sw + gx_off - 2
            sy1 = y + (row_i + 1) * sh - 2
            th = max(8.0, sy1 - sy0)

            in_len = (ps <= st < pe)
            vel, roll = self._step_get(scene, self.sel_pad, st)
            is_cur = (in_len and st == cur_step)
            is_beat = ((st - ps) % 4 == 0)

            if not in_len:
                c.create_rectangle(sx0, sy0, sx1, sy1, fill="#15161c", outline="#242630", width=1)
                c.create_text(sx0 + 5, sy0 + 3, anchor="nw", text="%d" % (st + 1),
                              fill="#343744", font=(fam, max(7, fsmall - 1)))
                continue

            # Tile body with subtle beat shading & bright cursor highlight
            if is_cur:
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

            # Step number (top-left) + Ratchet badge (bottom-right)
            c.create_text(sx0 + 5, sy0 + 7, anchor="nw", text="%d" % (st + 1),
                          fill="#ffffff" if (vel > 0 or is_cur) else "#8d94a6",
                          font=(fam, max(7, fsmall - 1), "bold" if (vel > 0 or is_cur) else "normal"))
            if vel > 0 and roll > 0:
                c.create_text(sx1 - 4, sy1 - 3, anchor="se", text="x%d" % (roll + 1),
                              fill="#ffd43b", font=(fam, max(7, fsmall - 1), "bold"))

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
            wave_i = int(pinfo[20 + li]) if pinfo else (0 if li == 0 else 1)
            rate_v = float(pinfo[22 + li]) if pinfo else (1.5 if li == 0 else 4.0)
            col_hi, col_fill = lfo_cols[li]
            rlbl = "%.2fHz" % rate_v if rate_v < 1.0 else ("%.1fHz" % rate_v)
            lbl = "LFO %d: %s  %s" % (li + 1, LFO_WAVES[wave_i % len(LFO_WAVES)], rlbl)
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
        # Modal backdrop: replace hit_boxes so taps go strictly to popup or close
        self.hit_boxes = []
        col_hi, col_fill = ("#66d9e8", "#1c3d47") if li == 0 else ("#da77f2", "#3b1f47")
        wave_i = int(pinfo[20 + li]) if pinfo else (0 if li == 0 else 1)
        rate_v = float(pinfo[22 + li]) if pinfo else (1.5 if li == 0 else 4.0)

        mx = max(12, int(w * 0.06))
        my = max(top_h + 8, int(h * 0.14))
        px0, py0, px1, py1 = mx, my, w - mx, h - max(12, int(h * 0.08))
        pw = max(120, px1 - px0)
        ph = max(100, py1 - py0)

        # Dimmed border + popup panel
        c.create_rectangle(0, top_h, w, h, fill="#0b0b0f", stipple="gray50", outline="")
        c.create_rectangle(px0, py0, px1, py1, fill="#1b1d26", outline=col_hi, width=2)

        pad_i = 10
        ix0, ix1 = px0 + pad_i, px1 - pad_i
        iw = ix1 - ix0

        # Header + Close button
        hdr_h = max(28, fs * 2 + 8)
        c.create_rectangle(px0 + 1, py0 + 1, px1 - 1, py0 + hdr_h, fill=col_fill, outline="")
        c.create_text(ix0 + 2, py0 + hdr_h / 2, anchor="w",
                      text="LFO %d  —  Pad %d" % (li + 1, p + 1),
                      fill=col_hi, font=(fam, fs + 1, "bold"))
        cw = max(56, int(iw * 0.20))
        self._add_btn(c, ix1 - cw, py0 + 3, ix1, py0 + hdr_h - 3, "Close",
                      "#2b2e3b", "#ffffff", fam, fs, "lfo_close", None, outline=col_hi)

        # Wave selector row (5 big buttons)
        wy0 = py0 + hdr_h + 10
        wh = max(30, min(42, int((ph - hdr_h - 36) * 0.32)))
        nw = len(LFO_WAVES)
        ww = (iw - (nw - 1) * 4) / float(nw)
        for wi, wname in enumerate(LFO_WAVES):
            bx0 = ix0 + wi * (ww + 4)
            bx1 = bx0 + ww
            sel = (wi == wave_i % nw)
            self._add_btn(c, bx0, wy0, bx1, wy0 + wh, wname,
                          col_hi if sel else "#252836",
                          "#000000" if sel else "#e0e0ec",
                          fam, fs, "lfo_wave", (li, p, wi),
                          outline="#ffffff" if sel else "#44485a")

        # Big full-width logarithmic Rate slider
        ry0 = wy0 + wh + 10
        rh = max(32, min(46, int((ph - hdr_h - 36) * 0.34)))
        c.create_rectangle(ix0, ry0, ix1, ry0 + rh, fill="#12141c", outline=col_hi, width=2)
        t = max(0.0, min(1.0, math.log(max(0.05, rate_v) / 0.05) / math.log(400.0)))
        fx = ix0 + 2 + t * max(1.0, iw - 4)
        c.create_rectangle(ix0 + 2, ry0 + 2, fx, ry0 + rh - 2, fill=col_fill, outline="")
        c.create_rectangle(ix0 + 2, ry0 + 2, fx, ry0 + 6, fill=col_hi, width=0)
        rlbl = "Rate: %.2f Hz" % rate_v if rate_v < 1.0 else ("Rate: %.1f Hz" % rate_v)
        c.create_text((ix0 + ix1) / 2, ry0 + rh / 2, text=rlbl,
                      fill="#ffffff", font=(fam, fs + 1, "bold"))
        self.hit_boxes.append((ix0, ry0, ix1, ry0 + rh, "lfo_rate", (li, p, ix0, ix1)))

        # Quick Rate preset buttons
        qy0 = ry0 + rh + 8
        qh = max(24, py1 - qy0 - 8)
        presets = (0.2, 0.5, 1.5, 4.0, 10.0)
        qw = (iw - (len(presets) - 1) * 4) / float(len(presets))
        for qi, hz in enumerate(presets):
            bx0 = ix0 + qi * (qw + 4)
            bx1 = bx0 + qw
            plbl = "%.1fHz" % hz if hz < 10.0 else "%dHz" % int(hz)
            self._add_btn(c, bx0, qy0, bx1, qy0 + qh, plbl,
                          "#242836", "#d0d4e4", fam, max(7, fs - 1),
                          "lfo_rate_preset", (li, p, hz), outline="#3c4054")

        # Absorb taps inside popup background; tapping outside closes popup
        self.hit_boxes.append((px0, py0, px1, py1, "lfo_nop", None))
        self.hit_boxes.append((0, 0, w, h, "lfo_close", None))

    def _draw_kit_tab(self, c, w, h, top_h, fam, fs):
        pad_m = 8
        y = top_h + pad_m
        bh = max(30, fs * 2 + 10)
        fsmall = max(8, fs - 1)

        pinfo = self.pads_data[self.sel_pad] if self.pads_data else None
        midi_n = pinfo[15] if pinfo else (36 + self.sel_pad)
        learn = self.shm_hdr[5] if self.shm_hdr else int(round(self._zval("learn", 0)))
        scene = int(round(self._mon("out_seq_scene", 0)))

        # Row 1: Load Factory Kit + Pad Learn
        half = (w - 3 * pad_m) / 2.0
        self._add_btn(c, pad_m, y, pad_m + half, y + bh, "Load Factory 16-Pad Kit",
                      COL_ACCENT, "#000000", fam, fsmall, "factory_kit", None)
        self._add_btn(c, pad_m * 2 + half, y, w - pad_m, y + bh,
                      "Hit M-VAVE Pad..." if learn else ("Pad %d MIDI Learn (%s)" % (self.sel_pad + 1, note_name(midi_n))),
                      COL_REC if learn else COL_PANEL, "#ffffff", fam, fsmall, "learn", 1 - learn)
        y += bh + 10

        # Row 2: Clear Active Scene + Clear Pad Sample
        self._add_btn(c, pad_m, y, pad_m + half, y + bh,
                      "Clear Scene %s" % SCENE_NAMES[scene & 7], COL_PANEL, "#ff8787", fam, fsmall, "clr_scene", scene)
        self._add_btn(c, pad_m * 2 + half, y, w - pad_m, y + bh,
                      "Unload Pad %d Sample" % (self.sel_pad + 1), COL_PANEL, "#ffd43b", fam, fsmall, "unload", self.sel_pad)
        y += bh + 10

        # Row 3: Hardware Pad Filter (Retrigger mask, Min Velocity, Crosstalk)
        retrig = int(self.shm_hdr[15]) if (self.shm_hdr and len(self.shm_hdr) > 17) else 25
        minvel = int(self.shm_hdr[16]) if (self.shm_hdr and len(self.shm_hdr) > 17) else 0
        xtalk  = int(self.shm_hdr[17]) if (self.shm_hdr and len(self.shm_hdr) > 17) else 0

        third = (w - 4 * pad_m) / 3.0
        sliders = (
            ("retrig", CMD_SET_RETRIG, retrig, 80, 5,
             "Retrig %dms" % retrig if retrig > 0 else "Retrig Off", "#74c0fc", "#1e3a52"),
            ("minvel", CMD_SET_MINVEL, minvel, 40, 2,
             "MinVel %d" % minvel if minvel > 0 else "MinVel Off", "#63e6be", "#1e453b"),
            ("xtalk",  CMD_SET_XTALK,  xtalk,  60, 5,
             "XTalk %d%%" % xtalk if xtalk > 0 else "XTalk Off", "#ffd43b", "#473b19"),
        )
        for idx, (_key, cmd_op, val, vmax, step, lbl, col_hi, col_fill) in enumerate(sliders):
            sx0 = pad_m + idx * (third + pad_m)
            sx1 = sx0 + third
            c.create_rectangle(sx0, y, sx1, y + bh, fill="#1a1c24",
                               outline=col_hi if val > 0 else "#383844", width=1)
            if val > 0:
                fx = sx0 + 1 + (min(val, vmax) / float(vmax)) * max(1.0, (sx1 - sx0 - 2))
                c.create_rectangle(sx0 + 1, y + 1, fx, y + bh - 1, fill=col_fill, outline="")
                c.create_rectangle(sx0 + 1, y + 1, fx, y + 4, fill=col_hi, width=0)
            c.create_text((sx0 + sx1) / 2, y + bh / 2, text=lbl,
                          fill=col_hi if val > 0 else "#9a9aaa", font=(fam, fsmall, "bold"))
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
            "  • Pads 1..16 = MIDI Notes 36..51 (or tap MIDI Learn)\n"
            "  • Bank B Notes 52..56 = Quantized Scene Switch A..E\n"
            "  • Seq tab: long-press bottom pad 1..16 to Mute / Unmute"
        )
        c.create_text(pad_m, y, anchor="nw", text=guide, fill="#a0a0b4", font=(fam, fsmall))

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
                self.tab = arg
                self.copy_src = -1
                self.lfo_popup = None
                self._select_page(arg)
                self.refresh_gui()
                return

            if action == "pad":
                self.sel_pad = arg
                self._zset("sel_pad", arg)
                self._send_cmd(CMD_TRIG_PAD, b=arg, val=110)
                self.last_synced_pad = -1
                self.drag_info = ("pad_hold", arg, now, 0)
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
                self.drag_info = ("wave_trim", (pad_i, which, wx0, wx1), y, 0)
                self._apply_wave_trim(which, wx0, wx1, x)
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
                self._zset("seq_scene", arg + 1)
                self.refresh_gui()
                return

            if action == "seq_scene":
                if self.copy_src >= 0:
                    if arg != self.copy_src:
                        self._send_cmd(CMD_COPY_SCENE, a=self.copy_src, b=arg)
                    self.copy_src = -1
                else:
                    self._zset("seq_scene", arg + 1)
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
                sc, p = arg
                self._send_cmd(CMD_CLEAR_PAD, a=sc, b=p)
                self.refresh_gui()
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

            if action == "step":
                sc, p, st = arg
                vel, roll = self._step_get(sc, p, st)
                tap_key = ("step", sc, p, st)
                if self.last_tap[0] == tap_key and (now - self.last_tap[1]) < 0.35:
                    # Double-tap cycles ratchet roll (1x -> 2x -> 3x -> 4x -> 1x)
                    if vel == 0:
                        self._send_cmd(CMD_SET_VEL, a=sc, b=p, c=st, val=100)
                    self._send_cmd(CMD_SET_ROLL, a=sc, b=p, c=st, val=(roll + 1) % 4)
                    self.last_tap = (None, 0.0)
                    self.refresh_gui()
                    return
                self.last_tap = (tap_key, now)
                # Cycle Off -> Normal (100) -> Accent (127) -> Off
                new_vel = 100 if vel == 0 else (127 if vel < 120 else 0)
                self._send_cmd(CMD_SET_VEL, a=sc, b=p, c=st, val=new_vel)
                if new_vel == 0:
                    self._send_cmd(CMD_SET_ROLL, a=sc, b=p, c=st, val=0)
                self.drag_info = ("step", (sc, p, st), y, new_vel if new_vel > 0 else 100)
                self.refresh_gui()
                return

            if action == "factory_kit":
                self._load_factory_kit()
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
                self._zset("sample%d" % (arg + 1), "_")
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

            if action == "lfo_wave":
                li, p_i, nw = arg
                self._send_cmd(CMD_SET_LFO, a=li, b=p_i, c=0, val=nw)
                self.refresh_gui()
                return

            if action == "lfo_rate_preset":
                li, p_i, hz = arg
                t = max(0.0, min(1.0, math.log(max(0.05, hz) / 0.05) / math.log(400.0)))
                v200 = int(round(t * 200.0))
                self._send_cmd(CMD_SET_LFO, a=li, b=p_i, c=1, val=v200)
                self._zset("pad_lfo%d_rate" % (li + 1), round(hz, 3))
                self.refresh_gui()
                return

            if action == "lfo_rate":
                li, p_i, rx0, rx1 = arg
                self.drag_info = ("lfo_rate", arg, y, 0)
                self._apply_lfo_rate(li, p_i, rx0, rx1, x)
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

    def _apply_lfo_rate(self, li, p_i, rx0, rx1, x):
        t = max(0.0, min(1.0, (x - rx0) / max(1.0, rx1 - rx0)))
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
            self._zset("pad_start", round(min(95.0, pct), 1))
        else:
            self._zset("pad_end", int(round(max(5.0, pct))))

    def _apply_quant_slider(self, qx0, qx1, x):
        t = max(0.0, min(1.0, (x - qx0) / max(1.0, qx1 - qx0)))
        q = int(round(t * 20.0)) * 5  # clean 5% steps: 0, 5, 10 .. 100%
        self._send_cmd(CMD_SET_QUANT, val=q)

    def _on_motion(self, event):
        if not self.drag_info or event is None:
            return
        kind, arg, y0, start_val = self.drag_info
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
        if kind == "lfo_rate":
            li, p_i, rx0, rx1 = arg
            self._apply_lfo_rate(li, p_i, rx0, rx1, event.x)
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
        if kind == "wave_trim":
            _pad_i, which, wx0, wx1 = arg
            self._apply_wave_trim(which, wx0, wx1, event.x)
            self.refresh_gui()
            return
        if kind in ("seq_pad_hold", "pad_hold"):
            return
        dy = y0 - event.y
        if abs(dy) < 4:
            return
        self.last_tap = (None, 0.0)
        if kind == "bpm":
            nbpm = max(40, min(240, int(round(start_val + dy * 0.5))))
            self._zset("seq_bpm", nbpm)
            self.refresh_gui()
        elif kind == "step":
            sc, p, st = arg
            nvel = max(1, min(127, int(round(start_val + dy * 1.2))))
            self._send_cmd(CMD_SET_VEL, a=sc, b=p, c=st, val=nvel)
            self.refresh_gui()

    def _on_release(self, event):
        if self.drag_info and self.drag_info[0] in ("seq_pad_hold", "pad_hold"):
            _, pad_i, t0, _ = self.drag_info
            if (time.monotonic() - t0) >= 0.40:
                mask = int(round(self._zval("mute_mask", 0)))
                self._zset("mute_mask", mask ^ (1 << pad_i))
                self.refresh_gui()
        self.drag_info = None
