#!/usr/bin/env python3
"""Real Jalv scene selection and preset round-trip, isolated under build/."""
import os
import re
import subprocess
import time
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
URI = os.environ.get("WEAVE_TEST_URI", "http://dmtry3.github.io/zynthian-eka/weave4")
SUFFIX = URI.removeprefix("http://dmtry3.github.io/zynthian-eka/weave4")
assert SUFFIX == ""
HOME = ROOT / ("build/scenes-host-home" + SUFFIX)

def session(commands, name):
    home = HOME
    home.mkdir(exist_ok=True)
    env = dict(os.environ, HOME=str(home), PWD=str(home), XDG_DATA_HOME=str(home / ".local/share"),
               LV2_PATH=str(ROOT / "build/lv2") + ":" + str(home / ".lv2") + ":" + str(home))
    path=ROOT / ("build/" + name + ".txt")
    with path.open("w") as log:
        p=subprocess.Popen(["jalv","-n",name,URI],stdin=subprocess.PIPE,
                           stdout=log,stderr=subprocess.STDOUT,env=env,text=True,cwd=home)
        try:
            time.sleep(1)
            assert p.poll() is None, path.read_text()
            for command in commands:
                p.stdin.write(command+"\n");p.stdin.flush();time.sleep(.2)
            p.stdin.write("controls\nmonitors\n");p.stdin.flush();time.sleep(.5)
        finally:
            if p.poll() is None:
                p.terminate()
                try:p.wait(timeout=3)
                except subprocess.TimeoutExpired:p.kill();p.wait()
    text=path.read_text()
    assert "doesn't exist" not in text and "bad verb" not in text,text
    values={s:float(v) for s,v in re.findall(r"([A-Za-z_][A-Za-z_0-9]*)\s*=\s*([-+0-9.eE]+)",text)}
    return home,text,values

def main():
    settings={"scene_b_pitch":-5,"scene_b_mode":1,"scene_b_chord":5,
              "scene_b_voicing":4,"scene_d_voicing":4,"scene_b_octave1":-1,"scene_b_delay_ms1":375,"scene_b_feedback1":65,
              "scene_b_fine2":13,"scene_b_humanize2":35,"scene_b_human_speed2":.4,
              "scene_d_fine4":-17,"scene_d_humanize4":65,"scene_b_hpf2":240,"scene_b_lpf2":6200,"scene_b_tone_on2":1,
              "scene_d_hpf4":1400,"scene_d_lpf4":4000,"scene_d_tone_on4":1,"scene_b_ready":1,"scene_d_pitch4":-7,"scene_d_ready":1,
              "scene_d_mode":3,"scene_d_scale_tonic":9,"scene_d_scale_mode":7,
              "scene_d_degree1":-2,"scene_d_degree2":0,"scene_d_degree3":4,"scene_d_degree4":6,"midi_input_channel":2,"tracking_profile":1,
              "selected_voice":3,"master":-4,"scene":1}
    home,text,v=session([f"{s}={x}" for s,x in settings.items()]+["save preset " + (HOME / "WeaveSceneBank.lv2/").as_uri() + "/,Weave-4_Scenes_Test"],"WeaveSceneSave")
    assert v.get("scene_active")==1 and v.get("interval1")==-8,v
    assert v.get("delay_actual1")==375 and v.get("scene_mask")==11,v
    # Select the URI actually returned by this save. Older fixture presets may
    # remain in the isolated test bank and must not be mistaken for the new one.
    saved = re.findall(r"#PRS>\s+(\S+)\s+\(", text)
    assert saved, text
    subject = saved[-1]
    from urllib.parse import unquote, urlparse
    preset = Path(unquote(urlparse(subject).path)).resolve()
    assert home.resolve() in preset.parents, preset
    assert 'scene_d_lpf4' in preset.read_text(), preset
    _,text,v=session(["preset " + subject],"WeaveSceneLoad")
    for s,x in settings.items():assert abs(v.get(s,999)-x)<.00001,(s,v.get(s),text)
    assert v.get("scene_active")==1 and v.get("interval1")==-8,v
    print({"jalv_scene_preset":"passed","controls":len(settings),"active":"B","inactive_D_retained":True})

if __name__ == '__main__':main()
