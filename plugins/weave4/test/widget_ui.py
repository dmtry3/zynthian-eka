#!/usr/bin/env python3
"""Native Pi Tk test using Zynthian's real base/controller classes, without a visible window."""
import importlib.util
import json
import math
import pathlib
import re
import sys
import types
import tkinter

ROOT = pathlib.Path(__file__).resolve().parents[1]
PLATFORM = pathlib.Path("/zynthian/zynthian-ui")
sys.path.insert(0, str(PLATFORM))

# Avoid importing the entire running GUI. Only its actual base/controller modules are needed.
gui = types.ModuleType("zyngui")
gui.__path__ = [str(PLATFORM / "zyngui")]
config = types.ModuleType("zyngui.zynthian_gui_config")
config.color_bg = "#000000"
config.color_tx = "#ffffff"
config.color_tx_off = "#e0e0e0"
config.font_family = "Audiowide"
config.zyngui = types.SimpleNamespace()
gui.zynthian_gui_config = config
sys.modules["zyngui"] = gui
sys.modules["zyngui.zynthian_gui_config"] = config
engine_package = types.ModuleType("zyngine")
engine_package.__path__ = [str(PLATFORM / "zyngine")]
sys.modules["zyngine"] = engine_package
from zyngine.zynthian_controller import zynthian_controller
from check import Host, set_extra

spec = importlib.util.spec_from_file_location("weave_widget", ROOT / "ui/zynthian_widget_eka_weave4.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
assert getattr(module, "zynthian_widget_eka_weave-4") is module.zynthian_widget_eka_weave4

def metadata():
    ports = {}
    text = (ROOT / "lv2/eka_weave4.ttl").read_text()
    for part in re.split(r"(?=\[ a lv2:(?:Input|Output)Port)", text):
        index = re.search(r"lv2:index (\d+)", part)
        symbol = re.search(r'lv2:symbol "([^"]+)"', part)
        if not index or not symbol or "lv2:ControlPort" not in part:
            continue
        def number(field, fallback):
            found = re.search(r"lv2:" + field + r" (-?[0-9.]+)", part)
            return float(found.group(1)) if found else fallback
        labels = re.findall(r'rdfs:label "([^"]+)" ; rdf:value (-?[0-9.]+)', part)
        ports[int(index.group(1))] = {
            "symbol": symbol.group(1), "input": part.startswith("[ a lv2:InputPort"),
            "value": number("default", 0), "low": number("minimum", 0),
            "high": number("maximum", 1), "labels": labels,
        }
    return ports

class Engine:
    def __init__(self, ports):
        self.ports = ports
        self.index = {info["symbol"]: index for index, info in ports.items()}
        extras = {index: info["value"] for index, info in ports.items() if index >= 7}
        self.host = Host(pitch=7, dry=0, wet=-6, extra=extras, stereo=True)
        self.writes = []
        self.phase = 0
    def send_controller_value(self, controller):
        self.writes.append((controller.symbol, controller.value))
        index = self.index[controller.symbol]
        if index < 3:
            self.host.controls[index].value = controller.value
        else:
            set_extra(self.host, index, controller.value)
    def get_monitors_dict(self):
        source = [.1 * math.sin(2 * math.pi * 220 * (self.phase+i) / 48000) for i in range(4096)]
        self.phase += len(source)
        self.host.process(source)
        return {info["symbol"]: self.host.extra[index].value
                for index, info in self.ports.items() if not info["input"] and index >= 7}

class Processor:
    def __init__(self):
        ports = metadata()
        self.engine = Engine(ports)
        self.controllers_dict = {}
        self.ctrl_screens_dict = {}
        self.current_screen_index = 0
        for info in ports.values():
            if not info["input"]:
                continue
            options = {"name": info["symbol"], "processor": self, "value": info["value"],
                       "value_min": info["low"], "value_max": info["high"]}
            if info["labels"]:
                options["labels"] = [label for label, _ in info["labels"]]
                options["ticks"] = [float(value) for _, value in info["labels"]]
            self.controllers_dict[info["symbol"]] = zynthian_controller(self.engine, info["symbol"], options)
    def get_ctrl_screens(self):
        return self.ctrl_screens_dict

class Parent:
    def __init__(self, root, processor):
        self.main_frame = tkinter.Frame(root)
        self.wide = False
        self.layout = {"rows": 4}
        self.shown = True
        self.processor = processor
        self.list_data = []
        self.screen_info = None
        self.screen_title = ""
        self.zcontrollers = []
        self.widget = None
        self.selections = 0
    def get_current_processor(self):
        return self.processor
    def fill_list(self):
        self.list_data = [
            (controllers[0].group_symbol, index, title, self.processor, index)
            for index, (title, controllers) in enumerate(self.processor.ctrl_screens_dict.items())]
    def select(self, index):
        self.selections += 1
        assert self.selections < 100, "page rebinding recursed"
        self.screen_info = self.list_data[index]
        self.screen_title = self.screen_info[2]
        self.zcontrollers = self.processor.ctrl_screens_dict[self.screen_title]
        self.widget.set_processor(self.processor)  # Real control screen does this on each selection.

def main():
    root = tkinter.Tk()
    root.withdraw()
    processor = Processor()
    parent = Parent(root, processor)
    widget = module.zynthian_widget_eka_weave4(parent)
    parent.widget = widget
    widget.set_processor(processor)
    root.update_idletasks()
    widget.on_size(types.SimpleNamespace(width=480, height=360))
    assert list(processor.ctrl_screens_dict) == ["Harmony", "Voice", "Time", "Character", "Tone", "Output", "Scenes"]
    assert processor.engine.writes == [], "initial binding wrote audio parameters"

    processor.engine.writes.clear()
    widget.select_voice(2)
    # Encoder destinations must already be correct before deferred Tk events run.
    assert parent.screen_title == "Voice"
    assert parent.zcontrollers[1] is processor.controllers_dict["level3"]
    root.update_idletasks()
    assert processor.engine.writes == [("selected_voice", 3)]
    assert parent.screen_title == "Voice"
    assert parent.zcontrollers[0] is processor.controllers_dict["pitch3"]
    assert parent.zcontrollers[1] is processor.controllers_dict["level3"]
    parent.zcontrollers[1].set_value(-18)
    assert processor.controllers_dict["level3"].value == -18
    assert processor.controllers_dict["wet"].value == -6
    assert processor.controllers_dict["level2"].value == -12

    # A slot outside the old count can be explicitly enabled without changing the count.
    parent.zcontrollers[3].set_value(1)
    assert processor.controllers_dict["enable3"].value == 1
    assert processor.controllers_dict["voices"].value == 1
    widget.toggle_solo()
    assert processor.controllers_dict["solo"].value == 3
    widget.select_voice(0)
    root.update_idletasks()
    assert processor.controllers_dict["solo"].value == 3
    widget._set("solo", 0)

    # MIDI/host changes update the selected editor without writeback.
    processor.controllers_dict["mode"].set_value(1)
    processor.controllers_dict["chord"].set_value(5)
    processor.controllers_dict["selected_voice"].set_value(4)
    processor.engine.writes.clear()
    widget.refresh_gui()
    root.update_idletasks()
    assert parent.zcontrollers[0] is widget.virtual[("octave", 3)]
    assert parent.zcontrollers[0].value_max == 0  # +12 would exceed the supported upper interval.
    assert processor.engine.writes == []
    widget.select_voice(0)
    root.update_idletasks()
    parent.zcontrollers[0].set_value(-1)
    assert processor.controllers_dict["octave1"].value == -1
    assert processor.controllers_dict["pitch"].value == 7

    # MIDI uses the same six pages and canonical controls; all slots stay editable.
    widget._set("mode", 2)
    widget._page("Harmony")
    widget.refresh_gui(); root.update_idletasks()
    assert parent.zcontrollers[1] is processor.controllers_dict["midi_input_channel"]
    assert parent.zcontrollers[2] is processor.controllers_dict["tracking_profile"]
    widget.select_voice(3); root.update_idletasks()
    assert parent.zcontrollers[0] is widget.virtual[("octave", 3)]
    assert parent.zcontrollers[0].value_min == -1 and parent.zcontrollers[0].value_max == 1
    assert not parent.zcontrollers[3].readonly
    widget.monitors = {"input_note":57, "held_notes":1, "midi_note4":60, "available_mask":8}
    processor.engine.writes.clear()
    widget.refresh_gui(); root.update_idletasks()
    label = widget.widget_canvas.itemcget(widget.items["card3_label"], "text")
    assert "C4" in label and "ON" in label, label
    assert processor.engine.writes == []
    widget._set("mode", 1); widget.select_voice(0); root.update_idletasks()

    # Scale binds new scene-aware canonical controls, not MIDI/Chord settings.
    widget._set("mode", 3); widget._page("Harmony")
    widget.refresh_gui(); root.update_idletasks()
    assert parent.zcontrollers[1] is processor.controllers_dict["scale_tonic"]
    assert parent.zcontrollers[2] is processor.controllers_dict["scale_mode"]
    widget.select_voice(2); root.update_idletasks()
    assert parent.zcontrollers[0] is processor.controllers_dict["degree3"]
    parent.zcontrollers[0].set_value(-2)
    assert processor.controllers_dict["degree3"].value == -2
    assert processor.controllers_dict["degree1"].value == 2
    widget._page("Character"); root.update_idletasks()
    assert parent.zcontrollers[3] is processor.controllers_dict["tracking_profile"]
    widget._page("Voice")
    widget.monitors = {"input_note":60, "target_note3":57, "available_mask":4}
    widget._set("enable3", 1)
    processor.engine.writes.clear()
    widget.refresh_gui(); root.update_idletasks()
    label = widget.widget_canvas.itemcget(widget.items["card2_label"], "text")
    assert "A3" in label and "ON" in label, label
    assert processor.engine.writes == []
    # Copy includes the six appended Scale fields, then editors point at destination.
    widget._set("scale_tonic", 5);widget._set("scale_mode", 2)
    widget.store_armed=True;widget.select_scene(3);root.update_idletasks()
    assert processor.controllers_dict["scene_d_scale_tonic"].value == 5
    assert processor.controllers_dict["scene_d_scale_mode"].value == 2
    assert parent.zcontrollers[0] is processor.controllers_dict["scene_d_degree3"]
    assert parent.zcontrollers[0].value == -2
    widget.select_scene(0);widget._set("mode", 1);widget.select_voice(0);root.update_idletasks()

    # Snapshot-like values are reflected on reopen without copying one voice over another.
    saved = {key: controller.value for key, controller in processor.controllers_dict.items()}
    widget.hide()
    processor.controllers_dict["selected_voice"].set_value(2)
    processor.controllers_dict["level2"].set_value(-9)
    processor.engine.writes.clear()
    widget.show()
    widget.refresh_gui()
    root.update_idletasks()
    assert parent.zcontrollers[1] is processor.controllers_dict["level2"]
    assert parent.zcontrollers[1].value == -9
    assert processor.engine.writes == []
    assert processor.controllers_dict["level3"].value == saved["level3"]

    # Time uses the same selected slot and switches only its Delay controller.
    widget._page("Time")
    assert parent.screen_title == "Time"
    assert parent.zcontrollers[0] is processor.controllers_dict["delay_ms2"]
    parent.zcontrollers[1].set_value(50)
    parent.zcontrollers[2].set_value(1)
    processor.engine.writes.clear()
    widget.get_monitors()
    widget.refresh_gui()
    root.update_idletasks()
    assert parent.zcontrollers[0] is processor.controllers_dict["division2"]
    assert processor.engine.writes == [], "sync binding wrote unrelated values"
    parent.zcontrollers[0].set_value(9)  # 1/8 at fallback 120 BPM = 250 ms
    widget.get_monitors()
    widget.refresh_gui()
    assert abs(widget._delay_ms(1) - 250) < .001
    widget.select_voice(0)
    assert parent.screen_title == "Time"
    assert parent.zcontrollers[0] is processor.controllers_dict["delay_ms1"]
    parent.zcontrollers[0].set_value(125)
    parent.zcontrollers[1].set_value(50)
    assert processor.controllers_dict["division2"].value == 9
    widget.select_voice(1)
    assert parent.zcontrollers[0] is processor.controllers_dict["division2"]

    # Character controls always edit only the selected voice and scene.
    widget._page("Character")
    assert parent.zcontrollers[0] is processor.controllers_dict["fine2"]
    parent.zcontrollers[0].set_value(7)
    parent.zcontrollers[1].set_value(40)
    parent.zcontrollers[2].set_value(.3)
    assert processor.controllers_dict["fine1"].value == 0
    widget.select_voice(0)
    assert parent.screen_title == "Character"
    assert parent.zcontrollers[0] is processor.controllers_dict["fine1"]
    widget.select_voice(1)
    widget._page("Time")

    widget._page("Tone")
    assert parent.zcontrollers[0] is processor.controllers_dict["hpf2"]
    assert parent.zcontrollers[1] is processor.controllers_dict["lpf2"]
    assert parent.zcontrollers[2] is processor.controllers_dict["tone_res2"]
    assert parent.zcontrollers[3] is processor.controllers_dict["tone_on2"]
    parent.zcontrollers[0].set_value(200)
    parent.zcontrollers[1].set_value(6000)
    parent.zcontrollers[2].set_value(45)
    parent.zcontrollers[3].set_value(1)
    assert processor.controllers_dict["hpf1"].value == 20
    widget.toggle_tone_flag("hpf_slope")
    widget.toggle_tone_flag("lpf_slope")
    widget.toggle_tone_flag("hpf_res_on")
    assert processor.controllers_dict["hpf_slope2"].value == 1
    assert processor.controllers_dict["lpf_slope2"].value == 1
    assert processor.controllers_dict["hpf_res_on2"].value == 1
    assert processor.controllers_dict["lpf_res_on2"].value == 1
    assert processor.controllers_dict["hpf_slope1"].value == 0
    widget.select_voice(0)
    assert parent.screen_title == "Tone"
    assert parent.zcontrollers[0] is processor.controllers_dict["hpf1"]
    widget.select_voice(1);widget._page("Time")

    # Empty scenes do not alter audio. Store explicitly copies then recalls.
    baseline = {s: widget._value(s) for s in module.SCENE_PARAMS}
    selected = widget._selected()
    widget.select_scene(1)
    assert widget._scene() == 0
    widget.store_armed = True
    widget.select_scene(1)
    assert widget._scene() == 1 and widget._selected() == selected
    assert {s: widget._value(s) for s in module.SCENE_PARAMS} == baseline
    assert parent.zcontrollers[0] is processor.controllers_dict["scene_b_division2"]
    assert parent.zcontrollers[3] is processor.controllers_dict["scene"]
    parent.zcontrollers[1].set_value(65)
    assert processor.controllers_dict["scene_b_feedback2"].value == 65
    assert processor.controllers_dict["feedback2"].value == 50
    widget._page("Character")
    assert parent.zcontrollers[0] is processor.controllers_dict["scene_b_fine2"]
    assert parent.zcontrollers[0].value == 7
    parent.zcontrollers[0].set_value(-11)
    assert processor.controllers_dict["fine2"].value == 7
    assert processor.controllers_dict["scene_b_humanize2"].value == 40
    widget._page("Time")
    widget._page("Tone")
    assert parent.zcontrollers[0] is processor.controllers_dict["scene_b_hpf2"]
    assert parent.zcontrollers[0].value == 200
    parent.zcontrollers[0].set_value(400)
    assert processor.controllers_dict["hpf2"].value == 200
    assert processor.controllers_dict["scene_b_lpf2"].value == 6000
    widget._page("Time")
    # External selector automation updates bindings, without any writeback.
    processor.controllers_dict["scene"].set_value(0)
    processor.engine.writes.clear()
    widget.refresh_gui()
    root.update_idletasks()
    assert parent.zcontrollers[1] is processor.controllers_dict["feedback2"]
    assert processor.engine.writes == []
    widget.select_scene(1)
    # Copy from B back into A must address A directly, not the active alias.
    widget.store_armed = True
    widget.select_scene(0)
    assert processor.controllers_dict["feedback2"].value == 65
    # Reopening must retain banks and bindings without rewriting settings.
    widget.select_scene(1)
    widget.hide(); processor.engine.writes.clear(); widget.show()
    widget.refresh_gui(); root.update_idletasks()
    assert widget._scene() == 1 and processor.engine.writes == []
    widget.select_scene(0)

    # Scenes page binds canonical scene plus Prev, Next, Store virtual controllers.
    widget._page("Scenes")
    widget.refresh_gui()
    assert parent.screen_title == "Scenes"
    prev_ctrl = widget.virtual["prev_scene"]
    next_ctrl = widget.virtual["next_scene"]
    store_ctrl = widget.virtual["store_scene"]
    assert parent.zcontrollers[0] is processor.controllers_dict["scene"]
    assert parent.zcontrollers[1] is prev_ctrl
    assert parent.zcontrollers[2] is next_ctrl
    assert parent.zcontrollers[3] is store_ctrl
    assert "Scene A active" in widget.widget_canvas.itemcget(widget.items["summary"], "text")

    # A, B, D are ready from earlier test steps. Next steps to B, then D (skipping empty C), then wraps to A.
    # Virtual controller identity must be preserved across scene switches (for MIDI Learn), and release (0) ignored.
    next_ctrl.set_value(1)  # Next from A -> B
    assert widget._scene() == 1 and next_ctrl.value == 0
    assert widget.virtual["next_scene"] is next_ctrl and widget.virtual["prev_scene"] is prev_ctrl
    next_ctrl.set_value(0)  # Pedal release must not retrigger
    assert widget._scene() == 1
    next_ctrl.set_value(1)  # Next from B -> D (skips empty C)
    assert widget._scene() == 3
    next_ctrl.set_value(1)  # Next from D -> wraps to A
    assert widget._scene() == 0

    # Prev from A -> D -> B -> A (rising edge 1; release 0 ignored).
    prev_ctrl.set_value(1)  # Prev from A -> D
    assert widget._scene() == 3 and prev_ctrl.value == 0
    prev_ctrl.set_value(0)  # Release ignored
    assert widget._scene() == 3
    prev_ctrl.set_value(1)  # Prev from D -> B
    assert widget._scene() == 1
    prev_ctrl.set_value(1)  # Prev from B -> A
    assert widget._scene() == 0

    # Arm store with store knob, then turn canonical Scene knob to C to clone A -> C.
    store_ctrl.set_value(1)
    assert widget.store_armed is True
    parent.zcontrollers[0].set_value(2)  # Turn canonical Scene knob to C
    widget.refresh_gui()
    assert widget.store_armed is False and store_ctrl.value == 0
    assert widget._scene() == 2
    assert processor.controllers_dict["scene_c_ready"].value == 1

    # Now all A, B, C, D are ready. Next/Prev cycle through all 4.
    next_ctrl.set_value(1)  # Next from C -> D
    assert widget._scene() == 3
    next_ctrl.set_value(1)  # Next from D -> A
    assert widget._scene() == 0
    prev_ctrl.set_value(1)  # Prev from A -> D
    assert widget._scene() == 3
    widget.select_scene(0)

    # No canvas-object growth across repeated monitor refreshes.
    widget.get_monitors()
    widget.refresh_gui()
    count = len(widget.widget_canvas.find_all())
    for _ in range(20):
        widget.get_monitors()
        widget.refresh_gui()
    assert len(widget.widget_canvas.find_all()) == count

    # The time view has bounded items and retains already-recorded tails.
    for _ in range(6):
        widget.get_monitors()
        widget.refresh_gui()
    widget._set("enable1", 0)
    widget.get_monitors()
    widget.refresh_gui()
    assert "TAIL" in widget.widget_canvas.itemcget(widget.items["card0_label"], "text")
    widget._set("enable1", 1)
    widget._page("Time")
    widget.get_monitors()
    widget.refresh_gui()
    widget.widget_canvas.postscript(file=str(ROOT / "build/widget-time-preview.eps"),
                                   colormode="color", width=480, height=360)
    widget._page("Voice")

    # Lead keeps canonical pages and displays actual assigned DSP notes.
    widget._set("mode",1);widget._set("voicing",0);widget._set("chord",5)
    for voice in range(1,5):widget._set("octave%d" % voice,0)
    widget.get_monitors();widget._set("voicing",4);widget.get_monitors()
    widget._set("chord",9);widget.get_monitors();widget.refresh_gui()
    assert [round(widget._monitor("interval%d" % v)) for v in range(1,5)] == [4,9,10,14]
    for voice,note in enumerate([4,9,10,14]):
        assert abs(widget._interval(voice)-widget._monitor("fine_actual%d" % (voice+1))/100-note)<.001
    widget._page("Harmony");widget.refresh_gui()
    assert parent.zcontrollers[2] is processor.controllers_dict["voicing"]
    assert parent.zcontrollers[2].value_max == 4
    assert "Lead" in widget.widget_canvas.itemcget(widget.items["summary"],"text")
    widget.select_voice(1);widget.refresh_gui()
    assert parent.zcontrollers[0] is widget.virtual[("octave",1)]
    parent.zcontrollers[0].set_value(1)
    widget.get_monitors();widget.refresh_gui()
    assert round(widget._monitor("interval2")) == 21
    widget.store_armed=True;widget.select_scene(3)
    assert processor.controllers_dict["scene_d_voicing"].value == 4
    assert widget._scene() == 3
    widget.select_scene(0);widget._set("voicing",0);widget._set("octave2",0)

    # Main screenshot represents real Tk drawing with actual LV2 telemetry.
    widget._set("mode", 1)
    widget._set("chord", 5)
    widget._set("voices", 4)
    for voice, pan in enumerate((-.7, .7, -.25, .25), 1):
        widget._set("pan%d" % voice, pan)
    widget._set("enable3", 1)
    widget.select_voice(1)
    root.update_idletasks()
    for _ in range(4):
        widget.get_monitors()
        widget.refresh_gui()
    output = ROOT / "build/widget-preview.eps"
    widget.widget_canvas.postscript(file=str(output), colormode="color", width=480, height=360)
    widget._page("Time")
    widget.get_monitors()
    widget.refresh_gui()
    widget.widget_canvas.postscript(file=str(ROOT / "build/widget-time-preview.eps"),
                                   colormode="color", width=480, height=360)
    widget._page("Character")
    widget.get_monitors(); widget.refresh_gui()
    widget.widget_canvas.postscript(file=str(ROOT / "build/widget-character-preview.eps"),
                                   colormode="color", width=480, height=360)
    widget._page("Tone")
    widget.get_monitors();widget.refresh_gui()
    assert widget.view == "tone"
    assert len(widget.widget_canvas.coords(widget.items["curve1"])) == 146
    curve_key=widget.tone_drawn
    widget.refresh_gui()
    assert widget.tone_drawn == curve_key
    widget.widget_canvas.postscript(file=str(ROOT / "build/widget-tone-preview.eps"),
                                   colormode="color", width=480, height=360)
    for width, height in ((320, 280), (480, 360), (640, 400)):
        widget.on_size(types.SimpleNamespace(width=width, height=height))
        assert widget.plot[0] < widget.plot[2]
        for voice in range(4):
            box = widget.boxes["card%d" % voice]
            bounds = widget.widget_canvas.bbox(widget.items["card%d_label" % voice])
            assert box[0] <= bounds[0] and bounds[2] <= box[2] - 8, (box, bounds)
        for box in widget.boxes.values():
            assert 0 <= box[0] < box[2] <= width
            assert 0 <= box[1] < box[3] <= height
    print(json.dumps({"widget": "passed", "pages": list(processor.ctrl_screens_dict),
                      "canvas_items": count, "preview": str(output),
                      "checks": ["selection", "shared knobs", "On/Solo", "Chord octave",
                                 "automation/reopen", "Lead telemetry/scenes", "no writeback", "bounded canvas"]}, indent=2))
    widget.destroy()
    processor.engine.host.close()
    root.destroy()

if __name__ == "__main__":
    main()
