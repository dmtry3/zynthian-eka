#!/usr/bin/python3
# -*- coding: utf-8 -*-
# Eka Pads widget for Zynthian: Bitwig Drum Machine-style 4x4 pad grid,
# waveform inspector, Auto-Select syncing Zynthian's 4 physical encoders,
# and 8-scene 16-step sequencer with Overdub and M-VAVE SMC-PAD support.

import glob
import logging
import mmap
import os
import struct
import threading
import time
import tkinter
import tkinter.font

from zyngui import zynthian_gui_config
from zyngui import zynthian_widget_base

MULTI_INSTANCE = True

EP_PADS = 16
EP_SCENES = 8
EP_UI_SCENES = 5
EP_STEPS = 32
EP_STEPS_MIN = 4
EP_SCENE_SIZE = EP_PADS * EP_STEPS * 2 + EP_PADS * 2   # 1056 bytes
EP_SHM_MAGIC = 0x45504144
EP_SHM_HDR_FMT = "<IIIiiiiiiiffiIi"
EP_SHM_HDR_SIZE = struct.calcsize(EP_SHM_HDR_FMT)      # 60 bytes
EP_PAD_FMT = "<ffffffiffffifiii"
EP_PAD_SIZE = struct.calcsize(EP_PAD_FMT)              # 64 bytes
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
LEN_PRESETS = (4, 8, 12, 16, 24, 32)
QUANT_NAMES = ("Off", "1/16", "1/8", "1/4")

TABS = ["Pads", "Seq", "Kit"]
TAB_GROUPS = ["g_sound", "g_seq", "g_mix"]
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

    def _step_get(self, scene, pad, step):
        if not self.scenes_data:
            return 0, 0
        idx = scene * EP_SCENE_SIZE + (pad * EP_STEPS + step) * 2
        if idx + 1 >= len(self.scenes_data):
            return 0, 0
        return self.scenes_data[idx], self.scenes_data[idx + 1] & 3

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
            ("pad_send",   p[12]),
        )
        for sym, val in mapping:
            self._zset(sym, val)
        scene = int(round(self._mon("out_seq_scene", 0)))
        ps, pe = self._pad_range_get(scene, pad_idx)
        self._zset("pad_steps", pe)
        self._zset("pad_beg", ps + 1)

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
        elif group in ("g_sound", "g_shape", "g_mix") and self.tab == 1:
            self.tab = 0

    # ---------- Drawing ----------

    def _font_size(self):
        return max(8, min(max(self.height, 40) // 18, 12))

    def refresh_gui(self):
        if not self.processor:
            return
        self._follow_page()
        self._read_shm()

        # Sync seq_mode zctrl whenever host Transport Start/Stop changes plugin sequencer mode
        if self.shm_hdr:
            cur_mode = self.shm_hdr[6]
            if cur_mode != self.last_shm_mode:
                self._zset("seq_mode", cur_mode)
                self.last_shm_mode = cur_mode

            # In Seq tab, hitting any physical MIDI pad switches the sequencer lane to that pad
            hit_seq = self.shm_hdr[13]
            last_hit = self.shm_hdr[14]
            if hit_seq != self.last_hit_seq:
                self.last_hit_seq = hit_seq
                if self.tab == 1 and 0 <= last_hit < EP_PADS and last_hit != self.sel_pad:
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
        else:
            self._draw_kit_tab(c, w, h, top_h, fam, fs)

    def _add_btn(self, c, x0, y0, x1, y1, text, fill, fg, fam, fs, action, arg=None, outline="#444450"):
        c.create_rectangle(x0, y0, x1, y1, fill=fill, outline=outline)
        c.create_text((x0 + x1) / 2, (y0 + y1) / 2, text=text, fill=fg, font=(fam, fs))
        self.hit_boxes.append((x0, y0, x1, y1, action, arg))

    def _draw_topbar(self, c, w, top_h, fam, fs):
        c.create_rectangle(0, 0, w, top_h, fill=COL_BG, outline="")
        x = 4
        bw = max(84, int(w * 0.16))
        tab_fs = fs + 2
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
        info_h = max(32, fs * 3 + 4)
        wy1 = max(wy0 + 30, h - pad_m - sc_h - info_h - 8)
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

        # Parameter summary readout
        iy0 = wy1 + 4
        if pinfo:
            line1 = "P%d  Pitch %+.1f  Dec %d%%  Filt %+d  Lvl %d%%" % (
                self.sel_pad + 1, pinfo[0], int(round(pinfo[1])), int(round(pinfo[2])), int(round(pinfo[3]))
            )
            line2 = "Start %d%%  Punch %+d  Drv %d%%  Spc %d%%" % (
                int(round(pinfo[4])), int(round(pinfo[7])), int(round(pinfo[8])), int(round(pinfo[12]))
            )
            c.create_text(rx0 + 2, iy0, anchor="nw", text=line1, fill="#d0d0dc", font=(fam, fsmall))
            c.create_text(rx0 + 2, iy0 + fs + 4, anchor="nw", text=line2, fill="#9090a4", font=(fam, fsmall))

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

        # Row 1: 5 Large Scene buttons (A..E) + Quantize + Copy + Clear
        r1_h = max(28, fs * 2 + 8)
        scenes_w = (w - 2 * pad_m) * 0.56
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
            self._add_btn(c, x0, y, x1, y + r1_h, "Sc " + SCENE_NAMES[s], bg, fg, fam, fs, "seq_scene", s)

        tools_x = pad_m + scenes_w + 2
        tw3 = (w - pad_m - tools_x) / 3.0
        # Horizontal Quantize strength slider (0..100%)
        qx0, qx1 = tools_x, tools_x + tw3 - 3
        c.create_rectangle(qx0, y, qx1, y + r1_h, fill="#1a1c24", outline="#3c4a46" if quant > 0 else "#383844", width=1)
        if quant > 0:
            q_fill_x = qx0 + 1 + (quant / 100.0) * max(1.0, (qx1 - qx0 - 2))
            c.create_rectangle(qx0 + 1, y + 1, q_fill_x, y + r1_h - 1, fill="#24483d", outline="")
            c.create_rectangle(qx0 + 1, y + 1, q_fill_x, y + 4, fill="#63e6be", width=0)
        q_lbl = "Q %d%%" % quant if quant > 0 else "Q Off"
        c.create_text((qx0 + qx1) / 2, y + r1_h / 2, text=q_lbl,
                      fill="#63e6be" if quant > 0 else "#9a9aaa", font=(fam, fsmall, "bold"))
        self.hit_boxes.append((qx0, y, qx1, y + r1_h, "quant_slider", (qx0, qx1)))
        self._add_btn(c, tools_x + tw3, y, tools_x + 2 * tw3 - 3, y + r1_h,
                      "Pick" if self.copy_src >= 0 else "Copy",
                      "#e64980" if self.copy_src >= 0 else COL_PANEL, "#ffffff", fam, fsmall, "seq_copy", scene)
        self._add_btn(c, tools_x + 2 * tw3, y, w - pad_m, y + r1_h,
                      "Clr P%d" % (self.sel_pad + 1), COL_PANEL, "#ff8787", fam, fsmall, "seq_clr_pad", (scene, self.sel_pad))
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
        y += bh + 14

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
                self._select_page(arg)
                self.refresh_gui()
                return

            if action == "pad":
                self.sel_pad = arg
                self._zset("sel_pad", arg)
                self._send_cmd(CMD_TRIG_PAD, b=arg, val=110)
                self.last_synced_pad = -1
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
                self.drag_info = ("steps_slider", (sc, p, which, sl_x0, sl_x1), y, 0)
                self._select_group("g_stereo")
                self._apply_steps_slider(sc, p, which, sl_x0, sl_x1, x)
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

    def _apply_steps_slider(self, sc, p, which, sl_x0, sl_x1, x):
        t = max(0.0, min(1.0, (x - sl_x0) / max(1.0, sl_x1 - sl_x0)))
        if which == "beg":
            nbeg = max(0, min(EP_STEPS - 1, int(round(t * EP_STEPS))))
            self._send_cmd(CMD_SET_START, a=sc, b=p, val=nbeg)
            self._zset("pad_beg", nbeg + 1)
        else:
            nend = max(1, min(EP_STEPS, int(round(t * EP_STEPS))))
            self._send_cmd(CMD_SET_END, a=sc, b=p, val=nend)
            self._zset("pad_steps", nend)

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
        if kind == "steps_slider":
            sc, p, which, sl_x0, sl_x1 = arg
            self._apply_steps_slider(sc, p, which, sl_x0, sl_x1, event.x)
            self.refresh_gui()
            return
        if kind == "wave_trim":
            _pad_i, which, wx0, wx1 = arg
            self._apply_wave_trim(which, wx0, wx1, event.x)
            self.refresh_gui()
            return
        if kind == "seq_pad_hold":
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
        if self.drag_info and self.drag_info[0] == "seq_pad_hold":
            _, pad_i, t0, _ = self.drag_info
            if (time.monotonic() - t0) >= 0.40:
                mask = int(round(self._zval("mute_mask", 0)))
                self._zset("mute_mask", mask ^ (1 << pad_i))
                self.refresh_gui()
        self.drag_info = None
