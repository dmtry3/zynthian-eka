#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Eka Weave-4: selected-voice controllers and a lightweight stereo map."""
import logging
import math
import time
import tkinter
import tkinter.font

from zyngui import zynthian_gui_config
from zyngui import zynthian_widget_base
from zyngine.zynthian_controller import zynthian_controller

MULTI_INSTANCE = True
UPDATE_PERIOD = .08
COLOURS = ("#4dabf7", "#ffd43b", "#ff8787", "#b197fc")
DIM = "#505050"
GRID = "#303030"
ACCENT = COLOURS[0]
CHORD_NAMES = ("Major", "Minor", "7", "maj7", "m7", "maj9", "m9",
               "6/9", "9", "13", "7#9", "sus2", "sus4", "dim7")
CHORDS = ((4, 7), (3, 7), (4, 7, 10), (4, 7, 11), (3, 7, 10),
          (4, 7, 11, 14), (3, 7, 10, 14), (4, 7, 9, 14),
          (4, 7, 10, 14), (4, 10, 14, 21), (4, 7, 10, 15),
          (2, 7), (5, 7), (3, 6, 9))
VOICINGS = ("Close", "Open", "Low", "Drop 2", "Lead")
SCALE_NAMES = ("Major", "Minor", "Dorian", "Phrygian", "Lydian", "Mixolydian", "Locrian", "Harm Minor", "Mel Minor")
KEY_NAMES = ("C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B")
PITCH = ("pitch", "pitch2", "pitch3", "pitch4")
LEVEL = ("wet", "level2", "level3", "level4")
SCENE_PARAMS = ('pitch', 'dry', 'wet', 'mode', 'chord', 'voicing', 'voices', 'pan1', 'pitch2', 'level2', 'pan2', 'pitch3', 'level3', 'pan3', 'pitch4', 'level4', 'pan4', 'enable1', 'enable2', 'enable3', 'enable4', 'octave1', 'octave2', 'octave3', 'octave4', 'delay_ms1', 'feedback1', 'sync1', 'division1', 'delay_ms2', 'feedback2', 'sync2', 'division2', 'delay_ms3', 'feedback3', 'sync3', 'division3', 'delay_ms4', 'feedback4', 'sync4', 'division4')
SCENE_PARAMS += tuple("%s%d" % (kind, voice) for voice in range(1, 5)
                      for kind in ("fine", "humanize", "human_speed"))
SCENE_PARAMS += tuple("%s%d" % (kind, voice) for voice in range(1, 5)
                      for kind in ("hpf", "lpf", "tone_on", "tone_res", "hpf_slope", "lpf_slope", "hpf_res_on", "lpf_res_on"))
SCENE_PARAMS += ("scale_tonic", "scale_mode", "degree1", "degree2", "degree3", "degree4")
PAGES = ("Harmony", "Voice", "Time", "Character", "Tone", "Output", "Scenes")
DIV_NAMES = ("1/64", "1/32T", "1/64.", "1/32", "1/16T", "1/32.", "1/16",
             "1/8T", "1/16.", "1/8", "1/4T", "1/8.", "1/4", "1/2T", "1/4.",
             "1/2", "1/2.", "1 bar", "2 bars", "4 bars")
QUARTERS = (1/16, 1/12, 3/32, 1/8, 1/6, 3/16, 1/4, 1/3, 3/8,
            1/2, 2/3, 3/4, 1, 4/3, 1.5, 2, 3)


class zynthian_widget_eka_weave4(zynthian_widget_base.zynthian_widget_base):
    def __init__(self, parent):
        super().__init__(parent)
        self.next_update = 0
        self.drawn = None
        self.items = {}
        self.boxes = {}
        self.markers = []
        self.ghosts = []
        self.view = "stereo"
        self.tone_drawn = None
        self.time_span = 500
        self.press = None
        self.store_armed = False
        self.store_source = None
        self.store_snapshot = None
        self.scene_message = ""
        self.virtual = {}
        self.controller_source = None
        self.pages_queued = False
        self.page_job = None
        self.pending_page = None
        self.widget_canvas = tkinter.Canvas(
            self, highlightthickness=0, relief="flat",
            bg=zynthian_gui_config.color_bg)
        self.widget_canvas.grid(row=0, column=0, sticky="news")
        self.widget_canvas.bind("<ButtonPress-1>", self._on_press)
        self.widget_canvas.bind("<ButtonRelease-1>", self._on_release)

    def set_processor(self, processor):
        super().set_processor(processor)
        self._bind_pages()

    def update(self):
        now = time.monotonic()
        if now < self.next_update:
            return
        self.next_update = now + UPDATE_PERIOD
        super().update()

    def on_size(self, event):
        if not super().on_size(event):
            return
        self.drawn = None
        self.refresh_gui()

    def _scene(self):
        try:
            controllers = self.processor.controllers_dict
            scene = max(0, min(3, round(float(controllers["scene"].value))))
            if scene and controllers["scene_%s_ready" % "abcd"[scene]].value < .5:
                return 0
            return scene
        except (AttributeError, KeyError, ValueError, TypeError):
            return 0

    def _controller(self, symbol):
        scene = self._scene()
        if scene and symbol in SCENE_PARAMS:
            symbol = "scene_%s_%s" % ("abcd"[scene], symbol)
        return self.processor.controllers_dict.get(symbol)

    def _arm_store(self, armed):
        self.store_armed = bool(armed)
        if self.store_armed and self._ready():
            self.store_source = self._scene()
            self.store_snapshot = {s: self._value(s) for s in SCENE_PARAMS}
        else:
            self.store_source = None
            self.store_snapshot = None
        if "store_scene" in self.virtual:
            self.virtual["store_scene"].set_value(int(self.store_armed), send=False)

    def select_scene(self, scene):
        if not 0 <= scene < 4:
            return
        if self.store_armed:
            # Snapshot source before writing any destination controls. The source
            # bank stays active until the inactive destination is fully populated.
            source_scene = self.store_source if self.store_source is not None else self._scene()
            values = self.store_snapshot if self.store_snapshot is not None else {s: self._value(s) for s in SCENE_PARAMS}
            if scene != source_scene:
                destination = "scene_%s_" % "abcd"[scene] if scene else ""
                if scene:
                    self._set(destination + "ready", 0)
                for symbol, value in values.items():
                    self.processor.controllers_dict[destination + symbol].set_value(value)
                if scene:
                    self._set(destination + "ready", 1)
            self._arm_store(False)
        elif scene and self._value("scene_%s_ready" % "abcd"[scene]) < .5:
            self.scene_message = "Empty: Store, then choose this scene"
            self.refresh_gui()
            return
        self._set("scene", scene)
        self.scene_message = ""
        self._bind_pages(self._current_page())
        self._flush_pages()
        self.refresh_gui()

    def prev_scene(self):
        self._step_scene(-1)

    def next_scene(self):
        self._step_scene(1)

    def _step_scene(self, step):
        current = self._scene()
        if self.store_armed:
            target = (current + step) % 4
            self.select_scene(target)
            return
        ready_scenes = [i for i in range(4) if i == 0 or self._value("scene_%s_ready" % "abcd"[i]) >= 0.5]
        if not ready_scenes:
            return
        if len(ready_scenes) == 1:
            self.scene_message = "Only Scene A ready (Store to B-D first)"
            self.refresh_gui()
            return
        if current not in ready_scenes:
            current = ready_scenes[0]
        index = ready_scenes.index(current)
        target = ready_scenes[(index + step) % len(ready_scenes)]
        self.select_scene(target)

    def _value(self, symbol, default=0):
        try:
            value = float(self._controller(symbol).value)
            return value if math.isfinite(value) else default
        except (AttributeError, KeyError, TypeError, ValueError):
            return default

    def _set(self, symbol, value):
        controller = self._controller(symbol)
        if controller is None:
            return False
        controller.set_value(value)
        return True

    def _monitor(self, symbol, default=0):
        try:
            value = float((self.monitors or {}).get(symbol, default))
            return value if math.isfinite(value) else default
        except (TypeError, ValueError):
            return default

    def _selected(self):
        return max(0, min(3, round(self._value("selected_voice", 1)) - 1))

    def _chord(self):
        return max(0, min(len(CHORDS) - 1, round(self._value("chord", 0))))

    def _scale_mode(self):
        return self._value("mode") >= 2.5

    def _tracking_mode(self):
        return self._value("mode") >= 1.5

    def _midi_mode(self):
        return 1.5 <= self._value("mode") < 2.5

    @staticmethod
    def _note_name(note):
        if note < 0:
            return "--"
        n = round(note)
        return ("C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B")[n % 12] + str(n // 12 - 1)

    def _base_intervals(self):
        if self._tracking_mode():
            return [self._monitor("interval%d" % (v + 1)) for v in range(4)]
        if self._value("mode") < .5:
            return [self._value(symbol, default)
                    for symbol, default in zip(PITCH, (7, 4, 10, 12))]
        notes = list(CHORDS[self._chord()])
        voicing = round(self._value("voicing"))
        for index in range(len(notes)):
            if voicing == 1 and index % 2 == 0:
                notes[index] -= 12
            if voicing == 2:
                notes[index] -= 12
            if voicing == 3 and index == len(notes) - 2:
                notes[index] -= 12
        return notes

    def _available(self, voice):
        return voice < len(self._base_intervals())

    def _enabled(self, voice):
        value = self._value("enable%d" % (voice + 1), -1)
        enabled = (self._midi_mode() or voice < round(self._value("voices", 1))) if value < -.5 else value >= .5
        return enabled and self._available(voice)

    def _interval(self, voice):
        notes = self._base_intervals()
        if voice >= len(notes):
            return 0
        pitch = notes[voice]
        if .5 <= self._value("mode") < 1.5:
            shifted = pitch + 12 * round(self._value("octave%d" % (voice + 1)))
            if -12 <= shifted <= 21:
                pitch = shifted
        fine = self._monitor("fine_actual%d" % (voice + 1))
        if abs(fine) < .001:
            fine = 0
        return self._monitor("interval%d" % (voice + 1), pitch) + fine / 100

    def _ready(self):
        return self.processor is not None and all(
            symbol in self.processor.controllers_dict
            for symbol in ("selected_voice", "enable1", "solo", "master", "delay_ms1", "sync1", "scene", "scene_d_division4", "scene_d_fine4", "scene_d_tone_on4", "scene_d_degree4", "scene_d_lpf_res_on4"))

    # These editor-only controllers forward actual edits to canonical LV2 ports.
    # They never replace canonical values in controllers_dict or snapshot state.
    def send_controller_value(self, controller):
        if controller.symbol == "prev_scene":
            if controller.value >= 0.5:
                self.prev_scene()
                controller.set_value(0, send=False)
            return
        if controller.symbol == "next_scene":
            if controller.value >= 0.5:
                self.next_scene()
                controller.set_value(0, send=False)
            return
        if controller.symbol == "store_scene":
            self._arm_store(controller.value >= 0.5)
            self.scene_message = ""
            self.refresh_gui()
            return
        self._set(controller.symbol, controller.value)

    def _sync_virtual(self):
        notes = self._base_intervals()
        for voice in range(4):
            on = self.virtual[("on", voice)]
            on.readonly = False
            on.set_value(int(self._enabled(voice)), send=False)
            on.readonly = voice >= len(notes)
            octave = self.virtual[("octave", voice)]
            octave.readonly = False
            base = notes[voice] if voice < len(notes) else 0
            leading = self._midi_mode() or (self._value("mode") >= .5 and round(self._value("voicing")) == 4)
            low = -1 if leading or base >= 0 else 0
            high = 1 if leading or base <= 9 else 0
            if octave.value_min != low or octave.value_max != high:
                octave.set_options({"value_min": low, "value_max": high})
            value = max(low, min(high, round(self._value("octave%d" % (voice + 1)))))
            octave.set_value(value, send=False)
            octave.readonly = voice >= len(notes)
        if "store_scene" in self.virtual:
            self.virtual["store_scene"].set_value(int(self.store_armed), send=False)

    def _make_virtual(self):
        self.virtual = {}
        for voice in range(4):
            for kind, symbol, name, low, high in (
                    ("on", "enable%d" % (voice + 1), "On", 0, 1),
                    ("octave", "octave%d" % (voice + 1), "Octave", -1, 1)):
                options = {
                    "name": name, "short_name": name, "group_symbol": "g_voice",
                    "group_name": "Voice", "processor": self.processor,
                    "value_min": low, "value_max": high, "value": 0,
                    "is_integer": True, "midi_autolearn": False,
                }
                if kind == "on":
                    options.update({"is_toggle": True, "labels": ["Off", "On"], "ticks": [0, 1]})
                self.virtual[(kind, voice)] = zynthian_controller(self, symbol, options)
        self.virtual["prev_scene"] = zynthian_controller(self, "prev_scene", {
            "name": "Prev Scene", "short_name": "Prev", "group_symbol": "g_scenes",
            "group_name": "Scenes", "processor": self.processor,
            "value_min": 0, "value_max": 1, "value": 0,
            "is_toggle": True, "midi_autolearn": True,
            "labels": ["< Prev", "< Prev"], "ticks": [0, 1],
        })
        self.virtual["next_scene"] = zynthian_controller(self, "next_scene", {
            "name": "Next Scene", "short_name": "Next", "group_symbol": "g_scenes",
            "group_name": "Scenes", "processor": self.processor,
            "value_min": 0, "value_max": 1, "value": 0,
            "is_toggle": True, "midi_autolearn": True,
            "labels": ["Next >", "Next >"], "ticks": [0, 1],
        })
        self.virtual["store_scene"] = zynthian_controller(self, "store_scene", {
            "name": "Store Scene", "short_name": "Store", "group_symbol": "g_scenes",
            "group_name": "Scenes", "processor": self.processor,
            "value_min": 0, "value_max": 1, "value": int(self.store_armed),
            "is_toggle": True, "midi_autolearn": False,
            "labels": ["Ready", "Arm Store"], "ticks": [0, 1],
        })

    def _current_page(self):
        info = getattr(self.zyngui_control, "screen_info", None)
        if info and len(info) >= 5 and info[3] is self.processor:
            return info[2]
        return None

    def _page_rows(self):
        return [(index, row[2]) for index, row in
                enumerate(getattr(self.zyngui_control, "list_data", []))
                if len(row) >= 5 and row[0] and row[3] is self.processor]

    def _bind_pages(self, requested=None):
        if not self._ready():
            return
        raw = self.processor.controllers_dict
        controllers = {s: self._controller(s) for s in raw}
        source = (id(self.processor), id(raw["pitch"]), id(raw["enable1"]))
        if source != self.controller_source:
            self.controller_source = source
            self._make_virtual()
        self._sync_virtual()
        voice = self._selected()
        pitch = (self.virtual[("octave", voice)] if self._value("mode") >= .5
                 else controllers[PITCH[voice]])
        if self._scale_mode() and "degree%d" % (voice + 1) in controllers:
            pitch = controllers["degree%d" % (voice + 1)]
        delay_symbol = ("division%d" if self._value("sync%d" % (voice + 1)) >= .5
                        else "delay_ms%d") % (voice + 1)
        pages = {
            "Harmony": [controllers[s] for s in
                        (("mode", "scale_tonic", "scale_mode", "dry") if self._scale_mode() and "scale_tonic" in controllers
                         else ("mode", "midi_input_channel", "tracking_profile", "dry")
                         if self._midi_mode() and "midi_input_channel" in controllers
                         else ("mode", "chord", "voicing", "dry"))],
            "Voice": [pitch, controllers[LEVEL[voice]], controllers["pan%d" % (voice + 1)],
                      self.virtual[("on", voice)]],
            "Time": [controllers[delay_symbol], controllers["feedback%d" % (voice + 1)],
                     controllers["sync%d" % (voice + 1)], controllers["scene"]],
            "Character": [controllers["%s%d" % (kind, voice + 1)]
                          for kind in ("fine", "humanize", "human_speed")] + [controllers["tracking_profile" if self._scale_mode() else "scene"]],
            "Tone": [controllers["%s%d" % (kind, voice + 1)]
                     for kind in ("hpf", "lpf", "tone_res", "tone_on")],
            "Output": [controllers[s] for s in ("master", "dry_timing", "selected_voice", "solo")],
            "Scenes": [controllers["scene"], self.virtual["prev_scene"], self.virtual["next_scene"], self.virtual["store_scene"]],
        }
        changed = self.processor.ctrl_screens_dict != pages
        if changed:
            previous = self._current_page()
            self.processor.ctrl_screens_dict = pages
            self.pending_page = requested or (previous if previous in pages else "Harmony")
            self.processor.current_screen_index = list(pages).index(self.pending_page)
        elif requested:
            self.pending_page = requested
        missing = not any(title == "Voice" for _, title in self._page_rows())
        if (changed or requested or missing) and not self.pages_queued:
            self.pages_queued = True
            self.page_job = self.after_idle(self._apply_pages)

    def _apply_pages(self):
        self.page_job = None
        self.pages_queued = False
        if not self._ready():
            return
        parent = self.zyngui_control
        if parent.get_current_processor() is not self.processor:
            return
        wanted = self.pending_page or self._current_page() or "Harmony"
        self.pending_page = None
        parent.fill_list()
        for index, title in self._page_rows():
            if title == wanted:
                parent.select(index)
                return

    def _flush_pages(self):
        # Finish a touch selection before another encoder event can edit the old slot.
        if self.page_job is not None:
            self.after_cancel(self.page_job)
            self.page_job = None
        self._apply_pages()

    def select_voice(self, voice):
        if self._set("selected_voice", voice + 1):
            page = self._current_page()
            self._bind_pages(page if page in ("Voice", "Time", "Character", "Tone") else "Voice")
            self._flush_pages()
            self.refresh_gui()

    def toggle_on(self):
        voice = self._selected()
        if self._available(voice):
            self._set("enable%d" % (voice + 1), int(not self._enabled(voice)))
            self.refresh_gui()

    def toggle_solo(self):
        voice = self._selected() + 1
        if self._available(voice - 1):
            self._set("solo", 0 if round(self._value("solo")) == voice else voice)
            self.refresh_gui()

    def toggle_tone_flag(self, prefix):
        symbol = "%s%d" % (prefix, self._selected() + 1)
        self._set(symbol, int(self._value(symbol) < .5))
        self.refresh_gui()

    def _page(self, title):
        self._bind_pages(title)
        self._flush_pages()

    # Fixed canvas objects are reused between frames; meters use actual DSP peaks.
    def _layout(self):
        canvas = self.widget_canvas
        canvas.delete("all")
        self.items = {}
        self.boxes = {}
        self.tone_drawn = None
        width, height = max(260, self.width), max(240, self.height)
        self.drawn = (self.width, self.height, self.view, self.time_span)
        self.font = (zynthian_gui_config.font_family, 11 if width < 430 else 12)
        self.small = (zynthian_gui_config.font_family, 10 if width < 430 else 11)
        self.fg = zynthian_gui_config.color_tx
        self.dim = getattr(zynthian_gui_config, "color_tx_off", "#888888")
        bg = zynthian_gui_config.color_bg
        canvas.create_rectangle(0, 0, width, height, fill=bg, outline="")
        def text(key, x, y, **options):
            self.items[key] = canvas.create_text(x, y, font=self.font, fill=self.fg, **options)
        def button(key, box, label):
            self.boxes[key] = box
            self.items[key] = canvas.create_rectangle(*box, fill=bg, outline=DIM)
            text(key + "_label", (box[0] + box[2]) / 2, (box[1] + box[3]) / 2, text=label)
        # The host header already identifies the plugin and preset.
        nav_start = 2
        nav_width = (width - nav_start - 4) / len(PAGES)
        for i, title in enumerate(PAGES):
            box = (nav_start + i * nav_width, 2, nav_start + (i + 1) * nav_width - 2, 29)
            button(title, box, {"Harmony": "Main", "Character": "Char", "Output": "Out",
                                "Scenes": "Scn" if width < 360 else "Scene"}.get(title, title))
        text("summary", 5, 43, text="", anchor="w")
        text("clip", width - 5, 43, text="", anchor="e")
        top, bottom = 60, height - 82
        card_font = tkinter.font.Font(font=self.small)
        card_width = max(74, card_font.measure("-60 dB TAIL") + 22,
                         card_font.measure("4 -12.5 st") + 22)
        card_height = (bottom - top - 18) / 4
        for voice in range(4):
            y = top + voice * (card_height + 6)
            key = "card%d" % voice
            button(key, (2, y, card_width, y + card_height), "")
            canvas.coords(self.items[key + "_label"], 10, y + card_height / 2)
            canvas.itemconfigure(self.items[key + "_label"], anchor="w", font=self.small)
            self.items["meter%d" % voice] = canvas.create_rectangle(
                card_width - 8, y + card_height - 4, card_width - 4, y + card_height - 4,
                fill=COLOURS[voice], outline="")
        plot_top = top
        if self.view == "tone":
            btn_h = 18 if height < 320 else 21
            btn_gap = 3
            frame_x0 = card_width + 12
            frame_x1 = width - 6
            btn_w = min(74, max(54, int((frame_x1 - frame_x0) * 0.25)))
            for key, box, label in (
                ("hpf_slope", (frame_x0, top, frame_x0 + btn_w, top + btn_h), "HP 12"),
                ("hpf_res", (frame_x0, top + btn_h + btn_gap, frame_x0 + btn_w, top + 2 * btn_h + btn_gap), "Res"),
                ("lpf_slope", (frame_x1 - btn_w, top, frame_x1, top + btn_h), "LP 12"),
                ("lpf_res", (frame_x1 - btn_w, top + btn_h + btn_gap, frame_x1, top + 2 * btn_h + btn_gap), "Res"),
            ):
                button(key, box, label)
                canvas.itemconfigure(self.items[key + "_label"], font=self.small)
            plot_top = top + 2 * btn_h + btn_gap + 4
        self.plot = (card_width + 30, plot_top + 20, width - 24, bottom - 20)
        x0, y0, x1, y1 = self.plot
        canvas.create_rectangle(x0 - 18, y0 - 18, x1 + 18, y1 + 18, outline=DIM)
        if self.view == "stereo":
            for pitch in (-12, 0, 12, 24):
                y = self._y(pitch)
                canvas.create_line(x0, y, x1, y, fill=GRID, dash=() if pitch == 0 else (2, 4))
                canvas.create_text(x0 - 7, y, text="%+d" % pitch, anchor="e", font=self.small, fill=self.dim)
            centre = (x0 + x1) / 2
            canvas.create_line(centre, y0, centre, y1, fill=GRID, dash=(2, 4))
            for x, label in ((x0, "L"), (centre, "Pan"), (x1, "R")):
                canvas.create_text(x, bottom - 5, text=label, font=self.small, fill=self.dim)
            y = self._y(0)
            canvas.create_oval(centre - 3, y - 3, centre + 3, y + 3, fill=self.dim, outline="")
            canvas.create_text(centre + 7, y + 10, text="Dry", anchor="w", font=self.small, fill=self.dim)
        elif self.view == "time":
            for voice in range(4):
                y = self._time_y(voice)
                canvas.create_line(x0, y, x1, y, fill=GRID, dash=(2, 4))
            for tick in range(5):
                x = x0 + (x1 - x0) * tick / 4
                canvas.create_line(x, y0, x, y1, fill=GRID, dash=(2, 4))
                # Sparse ticks keep the narrow widget readable.
                if width >= 430 or tick % 2 == 0:
                    canvas.create_text(x, bottom - 5, text=self._time_label(self.time_span * tick / 4),
                                       font=self.small, fill=self.dim)
        else:
            for hz in (20, 100, 1000, 10000, 20000):
                x = self._frequency_x(hz)
                canvas.create_line(x, y0, x, y1, fill=GRID, dash=(2, 4))
                if width >= 430 or hz in (20, 1000, 20000):
                    canvas.create_text(x, bottom - 5, text=self._hz_label(hz),
                                       font=self.small, fill=self.dim)
            for db in (12, 0, -12, -24, -36):
                y = self._response_y(db)
                canvas.create_line(x0, y, x1, y, fill=GRID, dash=() if db == 0 else (2, 4))
                if height >= 320 or db in (12, 0, -36):
                    canvas.create_text(x0 - 6, y, text="+12" if db > 0 else str(db), anchor="e",
                                       font=self.small, fill=self.dim)
        for voice in range(4):
            self.items["curve%d" % voice] = canvas.create_line(
                0, 0, 1, 1, fill=COLOURS[voice], state="hidden", tags="tone%d" % voice)
        self.ghosts = []
        for voice in range(4):
            self.ghosts.append([canvas.create_oval(0, 0, 0, 0, outline=self.dim,
                                                  state="hidden") for _ in range(3)])
        self.markers = []
        for voice in range(4):
            tag = "voice%d" % voice
            self.markers.append((
                canvas.create_oval(0, 0, 0, 0, fill=bg, outline=COLOURS[voice], tags=tag),
                canvas.create_text(0, 0, text=str(voice + 1), fill=self.fg, font=self.font, tags=tag),
                canvas.create_text(0, 0, text="", fill=self.fg, font=self.small, tags=tag)))
        scene_width = (width - 4) / 5
        for scene, letter in enumerate("ABCD"):
            button("scene%d" % scene,
                   (2 + scene * scene_width, height - 77, (scene + 1) * scene_width - 2, height - 45), letter)
        button("store", (2 + 4 * scene_width, height - 77, width - 2, height - 45), "Store")
        button("on", (2, height - 40, 82, height - 2), "On")
        button("solo", (88, height - 40, 168, height - 2), "Solo")
        text("footer", width - 4, height - 21, text="", anchor="e")

    def _y(self, pitch):
        _, y0, _, y1 = self.plot
        return y1 - (max(-12, min(24, pitch)) + 12) / 36 * (y1 - y0)

    @staticmethod
    def _hz_label(hz):
        if hz >= 1000:
            return "%.2gk" % (hz / 1000) if hz < 10000 else "%.0fk" % (hz / 1000)
        return "%d" % round(hz)

    def _frequency_x(self, hz):
        x0, _, x1, _ = self.plot
        return x0 + math.log(max(20, hz) / 20) / math.log(1000) * (x1 - x0)

    def _response_y(self, db):
        _, y0, _, y1 = self.plot
        return y0 + max(0, min(48, 12 - db)) / 48 * (y1 - y0)

    @staticmethod
    def _tone_damping(slope_24, res_norm):
        if slope_24:
            return (0.7653668647301795 * (0.30 / 0.7653668647301795) ** res_norm,
                    1.8477590650225735 * (0.95 / 1.8477590650225735) ** res_norm)
        return (1.4142135623730951 * (0.30 / 1.4142135623730951) ** res_norm, None)

    def _tone_values(self, voice):
        i = voice + 1
        limit = (self._monitor("sample_rate", 48000) or 48000) * .45
        high = min(limit, max(20, min(4000, self._value("hpf%d" % i, 20))))
        low = min(limit, max(200, min(20000, self._value("lpf%d" % i, 20000))))
        on = self._value("tone_on%d" % i) >= .5
        res = max(0.0, min(100.0, self._value("tone_res%d" % i, 0)))
        hp24 = self._value("hpf_slope%d" % i) >= .5
        lp24 = self._value("lpf_slope%d" % i) >= .5
        hp_res = self._value("hpf_res_on%d" % i) >= .5
        lp_res = self._value("lpf_res_on%d" % i, 1) >= .5
        return high, low, on, res, hp24, lp24, hp_res, lp_res

    def _draw_tone(self):
        values = tuple(self._tone_values(v) for v in range(4))
        enabled = tuple(self._enabled(v) for v in range(4))
        available = tuple(self._available(v) for v in range(4))
        rate = self._monitor("sample_rate", 48000) or 48000
        key = (self.view, self._selected(), values, enabled, available, rate)
        if key == self.tone_drawn:
            return
        self.tone_drawn = key
        for voice, (high, low, on, res, hp24, lp24, hp_res, lp_res) in enumerate(values):
            item = self.items["curve%d" % voice]
            shown = self.view == "tone" and available[voice]
            self.widget_canvas.itemconfigure(item, state="normal" if shown else "hidden")
            if not shown:
                continue
            coordinates = []
            gh = math.tan(math.pi * high / rate)
            gl = math.tan(math.pi * low / rate)
            hk1, hk2 = self._tone_damping(hp24, (res / 100.0) if hp_res else 0.0)
            lk1, lk2 = self._tone_damping(lp24, (res / 100.0) if lp_res else 0.0)
            for point in range(73):
                hz = 20 * 1000 ** (point / 72)
                db = 0
                if on:
                    if hz >= rate / 2:
                        db = -36
                    else:
                        warp = math.tan(math.pi * hz / rate)
                        rh, rl = warp / gh, warp / gl
                        rh2, rl2 = rh * rh, rl * rl
                        hp_pow = (rh2 * rh2) / ((1 - rh2) ** 2 + (hk1 * rh) ** 2)
                        if hk2 is not None:
                            hp_pow *= (rh2 * rh2) / ((1 - rh2) ** 2 + (hk2 * rh) ** 2)
                        lp_pow = 1.0 / ((1 - rl2) ** 2 + (lk1 * rl) ** 2)
                        if lk2 is not None:
                            lp_pow *= 1.0 / ((1 - rl2) ** 2 + (lk2 * rl) ** 2)
                        db = 10 * math.log10(max(1e-12, hp_pow * lp_pow))
                coordinates.extend((self._frequency_x(hz), self._response_y(db)))
            focused = voice == self._selected()
            colour = COLOURS[voice] if enabled[voice] else DIM
            self.widget_canvas.coords(item, *coordinates)
            self.widget_canvas.itemconfigure(item, fill=colour if focused else self._echo_colour(colour, .45),
                                             width=2 if focused else 1, dash=() if on else (3, 3))
        self.widget_canvas.tag_raise("tone%d" % self._selected())

    def _delay_ms(self, voice):
        index = voice + 1
        fallback = self._value("delay_ms%d" % index)
        if self._value("sync%d" % index) >= .5:
            division = max(0, min(19, round(self._value("division%d" % index, 12))))
            unit = self._monitor("beat_unit", 4) or 4
            bar = self._monitor("beats_per_bar", 4) or 4
            beats = QUARTERS[division] * unit / 4 if division < 17 else bar * 2 ** (division - 17)
            fallback = 60000 / max(20, self._monitor("bpm", 120)) * beats
        return max(0, min(8000, self._monitor("delay_actual%d" % index, fallback)))

    def _time_horizon(self):
        extent = 500
        for voice in range(4):
            if self._available(voice) or self._monitor("peak%d" % (voice + 1)) > 1e-4:
                repeats = 3 if self._value("feedback%d" % (voice + 1)) > 0 else 1
                extent = max(extent, self._delay_ms(voice) * repeats)
        for span in (500, 1000, 2000, 4000, 8000, 16000, 24000):
            if span >= extent:
                return span
        return 24000

    def _time_x(self, milliseconds):
        x0, _, x1, _ = self.plot
        return x0 + max(0, min(1, milliseconds / self.time_span)) * (x1 - x0)

    def _time_y(self, voice):
        _, y0, _, y1 = self.plot
        return y0 + (voice + .5) / 4 * (y1 - y0)

    @staticmethod
    def _time_label(milliseconds):
        return ("%gs" % (milliseconds / 1000) if milliseconds >= 1000
                else "%gms" % milliseconds)

    def _echo_colour(self, colour, weight):
        foreground = self.widget_canvas.winfo_rgb(colour)
        background = self.widget_canvas.winfo_rgb(zynthian_gui_config.color_bg)
        rgb = [round((b + (a - b) * weight) / 257) for a, b in zip(foreground, background)]
        return "#%02x%02x%02x" % tuple(rgb)

    @staticmethod
    def _meter(peak):
        return max(0, min(1, (20 * math.log10(max(1e-6, peak)) + 60) / 60))

    def refresh_gui(self):
        if self.processor is None:
            return
        if self.store_armed and self._ready() and self.store_source is not None:
            raw_scene = max(0, min(3, round(float(self.processor.controllers_dict["scene"].value))))
            if raw_scene != self.store_source:
                self.select_scene(raw_scene)
                return
        self._bind_pages()
        if self.pages_queued and self._current_page() in ("Voice", "Time", "Character", "Tone", "Scenes") and self.shown:
            self._flush_pages()
        page = self._current_page()
        if page == "Tone":
            self.view = "tone"
        elif page == "Time":
            self.view = "time"
        elif page in ("Voice", "Character", "Scenes"):
            self.view = "stereo"
        self.time_span = self._time_horizon() if self.view == "time" else 500
        if self.drawn != (self.width, self.height, self.view, self.time_span):
            self._layout()
        canvas = self.widget_canvas
        if not self._ready():
            canvas.itemconfigure(self.items["summary"], text="Reload the effect to update its controls")
            return
        selected = self._selected()
        solo = round(self._value("solo"))
        page = self._current_page()
        for title in PAGES:
            active = title == page
            canvas.itemconfigure(self.items[title], outline=ACCENT if active else DIM,
                                 fill=ACCENT if active else zynthian_gui_config.color_bg,
                                 width=1)
            canvas.itemconfigure(self.items[title + "_label"],
                                 fill="#000000" if active else self.fg)
        mode = self._value("mode")
        if mode >= 2.5:
            key = max(0, min(11, round(self._value("scale_tonic"))))
            scale = max(0, min(len(SCALE_NAMES)-1, round(self._value("scale_mode"))))
            summary = "%s %s | In %s" % (KEY_NAMES[key], SCALE_NAMES[scale], self._note_name(self._monitor("input_note", -1)))
        elif mode >= 1.5:
            source_note = self._monitor("input_note", -1)
            summary = "MIDI | In %s | %d keys" % (self._note_name(source_note), self._monitor("held_notes"))
        elif mode >= .5:
            voicing = max(0, min(len(VOICINGS)-1, round(self._value("voicing"))))
            summary = "%s / %s" % (CHORD_NAMES[self._chord()], VOICINGS[voicing])
        else:
            summary = "Intervals"
        if self.view == "time":
            division = max(0, min(19, round(self._value("division%d" % (selected + 1), 12))))
            duration = self._time_label(self._delay_ms(selected))
            if self._value("sync%d" % (selected + 1)) >= .5:
                duration = DIV_NAMES[division] + " = " + duration
            if self._delay_ms(selected) >= 8000:
                duration += " MAX"
            summary = "Voice %d  |  %s" % (selected + 1, duration)
        elif page == "Tone":
            high, low, on, res, _, _, _, _ = self._tone_values(selected)
            summary = ("V%d | HP %s | LP %s | R %g%%" % (selected + 1, self._hz_label(high), self._hz_label(low), round(res))
                       if on else "V%d | Filters Off" % (selected + 1))
        elif page == "Character":
            fine = self._monitor("fine_actual%d" % (selected + 1), self._value("fine%d" % (selected + 1)))
            micro = self._monitor("human_time_actual%d" % (selected + 1))
            summary = "V%d | %+.1fc | H %g%%" % (selected + 1, fine,
                         self._value("humanize%d" % (selected + 1)))
            if self.width >= 430:
                summary += " | %.1fms" % micro
        elif page == "Scenes":
            active_letter = "ABCD"[self._scene()]
            summary = "Scene %s active  |  Prev / Next to switch" % active_letter if self.width >= 400 else "Scene %s  |  Prev/Next" % active_letter
        else:
            summary = "%s    |    Voice %d" % (summary, selected + 1)
        if self.store_armed:
            summary = "Store: choose destination A-D"
        elif self.scene_message:
            summary = self.scene_message
        canvas.itemconfigure(self.items["summary"], text=summary,
                             fill=COLOURS[selected] if not self.store_armed else COLOURS[1])
        scene = self._scene()
        for index, letter in enumerate("ABCD"):
            ready = index == 0 or self._value("scene_%s_ready" % letter.lower()) >= .5
            key = "scene%d" % index
            canvas.itemconfigure(self.items[key], outline=self.fg if index == scene else DIM,
                                 fill=self.fg if index == scene else zynthian_gui_config.color_bg,
                                 width=1)
            canvas.itemconfigure(self.items[key + "_label"], text=letter if ready else letter + " -",
                                 fill="#000000" if index == scene else self.fg if ready else DIM)
        canvas.itemconfigure(self.items["store_label"], text="Cancel" if self.store_armed else "Store")
        canvas.itemconfigure(self.items["store"], outline=COLOURS[1] if self.store_armed else DIM,
                             fill=COLOURS[1] if self.store_armed else zynthian_gui_config.color_bg)
        canvas.itemconfigure(self.items["store_label"], fill="#000000" if self.store_armed else self.fg)
        clip = self._monitor("output_peak") >= 1
        canvas.itemconfigure(self.items["clip"], text="CLIP" if clip else "", fill="#ff6b6b")
        x0, _, x1, _ = self.plot
        for voice in range(4):
            available = self._available(voice)
            enabled = self._enabled(voice)
            peak = self._monitor("peak%d" % (voice + 1))
            tail = not enabled and peak > 1e-4 and not solo
            audible = ((solo == voice + 1 if solo else enabled) and available) or tail
            colour = COLOURS[voice] if audible else DIM
            interval = self._interval(voice)
            level = self._value(LEVEL[voice], -6 if voice == 0 else -12)
            status = "TAIL" if tail else (("SOLO" if solo == voice + 1 else "ON" if enabled else "OFF") if available else "--")
            interval_label = ("%+.1f" % interval).rstrip("0").rstrip(".") if available else "--"
            if self._tracking_mode():
                target = self._monitor("midi_note%d" % (voice + 1), -1)
                target_name = self._note_name(target + 12 * self._value("octave%d" % (voice + 1)) if target >= 0 else -1)
                if self._scale_mode():
                    target = self._monitor("target_note%d" % (voice + 1), -1)
                    target_name = self._note_name(target)
                valid = round(self._monitor("available_mask")) & (1 << voice)
                if enabled and not tail:
                    status = ("WAIT" if self._scale_mode() and self._monitor("input_note", -1) < 0
                              else "ARM" if target < 0 else "WAIT" if self._monitor("input_note", -1) < 0
                              else "RANGE" if not valid else "SOLO" if solo == voice + 1 else "ON")
                audible = (enabled and bool(valid) and (not solo or solo == voice + 1)) or tail
                colour = COLOURS[voice] if audible else DIM
                interval_label = target_name
                label = "%d  %s\n%g dB  %s" % (voice + 1, target_name, level, status)
            else:
                label = "%d  %s st\n%g dB  %s" % (voice + 1, interval_label, level, status)
            card = "card%d" % voice
            focused = voice == selected
            canvas.itemconfigure(self.items[card], outline=colour,
                                 fill=colour if focused else zynthian_gui_config.color_bg,
                                 width=2 if focused else 1)
            canvas.itemconfigure(self.items[card + "_label"], text=label,
                                 fill="#000000" if focused else colour if audible else self.dim)
            canvas.itemconfigure(self.items["meter%d" % voice],
                                 fill="#000000" if focused else COLOURS[voice])
            box = self.boxes[card]
            meter = self._meter(peak)
            canvas.coords(self.items["meter%d" % voice], box[2] - 8,
                          box[3] - 4 - (box[3] - box[1] - 8) * meter, box[2] - 4, box[3] - 4)
            pan = max(-1, min(1, self._value("pan%d" % (voice + 1))))
            x = self._time_x(self._delay_ms(voice)) if self.view == "time" else x0 + (pan + 1) / 2 * (x1 - x0)
            y = self._time_y(voice) if self.view == "time" else self._y(interval)
            visible = self.view != "tone" and (available or (tail and self.view == "time"))
            radius = 11 + 4 * meter
            circle, number, caption = self.markers[voice]
            canvas.coords(circle, x - radius, y - radius, x + radius, y + radius)
            canvas.itemconfigure(circle, outline=colour,
                                 fill=colour if focused and audible else zynthian_gui_config.color_bg,
                                 dash=() if audible else (3, 3),
                                 width=3 if voice == selected else 1,
                                 state="normal" if visible else "hidden")
            canvas.coords(number, x, y)
            canvas.itemconfigure(number, fill="#000000" if focused and audible else colour, state="normal" if visible else "hidden")
            canvas.coords(caption, x, y - radius - 10)
            caption_text = self._time_label(self._delay_ms(voice)) if self.view == "time" else interval_label + ("" if self._tracking_mode() else " st")
            canvas.itemconfigure(caption, text=caption_text,
                                 state="normal" if voice == selected and visible else "hidden")
            feedback = self._value("feedback%d" % (voice + 1)) / 100
            for repeat, ghost in enumerate(self.ghosts[voice], 2):
                echo_ms = self._delay_ms(voice) * repeat
                shown = self.view == "time" and visible and feedback > 0 and 0 < echo_ms <= self.time_span
                if shown:
                    echo_x = self._time_x(echo_ms)
                    canvas.coords(ghost, echo_x - 4, y - 4, echo_x + 4, y + 4)
                    canvas.itemconfigure(ghost, outline=self._echo_colour(colour, feedback ** (repeat - 1)),
                                         state="normal")
                else:
                    canvas.itemconfigure(ghost, state="hidden")
        self._draw_tone()
        if self.view == "tone":
            _, _, _, _, hp24, lp24, hp_res, lp_res = self._tone_values(selected)
            vc = COLOURS[selected]
            bg = zynthian_gui_config.color_bg
            for key, active, text_label in (
                ("hpf_slope", hp24, "HP 24" if hp24 else "HP 12"),
                ("hpf_res", hp_res, "Res"),
                ("lpf_slope", lp24, "LP 24" if lp24 else "LP 12"),
                ("lpf_res", lp_res, "Res"),
            ):
                canvas.itemconfigure(self.items[key], outline=vc if active else DIM,
                                     fill=vc if active else bg)
                canvas.itemconfigure(self.items[key + "_label"], text=text_label,
                                     fill="#000000" if active else (self.fg if key.endswith("slope") else self.dim))
        canvas.tag_raise("voice%d" % selected)
        canvas.itemconfigure(self.items["on_label"], text="On" if self._enabled(selected) else "Off")
        on = self._enabled(selected)
        canvas.itemconfigure(self.items["on"], outline=COLOURS[selected] if on else DIM,
                             fill=COLOURS[selected] if on else zynthian_gui_config.color_bg)
        canvas.itemconfigure(self.items["on_label"], fill="#000000" if on else self.fg)
        canvas.itemconfigure(self.items["solo_label"], text="Solo %d" % solo if solo else "Solo")
        canvas.itemconfigure(self.items["solo"], outline=COLOURS[1] if solo else DIM,
                             fill=COLOURS[1] if solo else zynthian_gui_config.color_bg, width=1)
        canvas.itemconfigure(self.items["solo_label"], fill="#000000" if solo else self.fg)
        timing = "Live" if self._value("dry_timing") < .5 else "Aligned"
        if self.view == "time":
            clock = "%g BPM" % self._monitor("bpm", 120)
            if self._monitor("tempo_host") < .5:
                clock += " local"
            footer = clock if self.width < 400 else "%s | Master %g dB" % (clock, self._value("master"))
        else:
            template = ("%s | %g dB" if self.width < 400 else "Dry %s   |   Master %g dB")
            footer = template % (timing, self._value("master"))
        canvas.itemconfigure(self.items["footer"], text=footer)

    def _hit(self, x, y):
        for key, (x0, y0, x1, y1) in self.boxes.items():
            if x0 <= x <= x1 and y0 <= y <= y1:
                return key
        # Stable side buttons remain available when marks overlap.
        nearest = None
        distance = 22 * 22
        for voice, (_, number, _) in enumerate(self.markers):
            if not self._available(voice):
                continue
            px, py = self.widget_canvas.coords(number)
            squared = (px - x) ** 2 + (py - y) ** 2
            if squared < distance:
                distance, nearest = squared, "card%d" % voice
        return nearest

    def _on_press(self, event):
        self.press = self._hit(event.x, event.y)

    def _on_release(self, event):
        key, self.press = self.press, None
        if key is None or key != self._hit(event.x, event.y) or not self._ready():
            return
        if key.startswith("card"):
            self.select_voice(int(key[4:]))
        elif key.startswith("scene"):
            self.select_scene(int(key[5:]))
        elif key == "store":
            self._arm_store(not self.store_armed)
            self.scene_message = ""
            self.refresh_gui()
        elif key == "on":
            self.toggle_on()
        elif key == "solo":
            self.toggle_solo()
        elif key == "hpf_slope":
            self.toggle_tone_flag("hpf_slope")
        elif key == "lpf_slope":
            self.toggle_tone_flag("lpf_slope")
        elif key == "hpf_res":
            self.toggle_tone_flag("hpf_res_on")
        elif key == "lpf_res":
            self.toggle_tone_flag("lpf_res_on")
        elif key in PAGES:
            self._page(key)


# The host derives both filename and class lookup from the displayed plugin name.
# A Python attribute may contain a hyphen even though a class declaration cannot.
# Keep the old class/file for existing engine definitions as well.
globals()["zynthian_widget_eka_weave-4"] = zynthian_widget_eka_weave4
