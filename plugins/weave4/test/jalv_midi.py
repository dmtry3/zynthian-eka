#!/usr/bin/env python3
"""Native Jalv/JACK MIDI delivery, isolated test client; no hardware connections."""
import os
import re
import subprocess
import time
from pathlib import Path
import jack
import numpy as np
ROOT=Path(__file__).resolve().parents[1]
URI="http://dmtry3.github.io/zynthian-eka/weave4"
def main():
    home=ROOT/"build/midi-host-home"; home.mkdir(exist_ok=True)
    bundles=ROOT/"build/lv2";bundles.mkdir(exist_ok=True)
    link=bundles/"eka-weave4.lv2"
    if not link.exists():link.symlink_to(ROOT/"build/eka-weave4.lv2")
    env=dict(os.environ,HOME=str(home),LV2_PATH=str(bundles))
    log=ROOT/"build/jalv-midi.txt"
    with log.open("w") as out:
        host=subprocess.Popen(["jalv","-n","WeaveMidiNative",URI],stdin=subprocess.PIPE,stdout=out,stderr=subprocess.STDOUT,env=env,text=True,cwd=home)
        client=None
        try:
            time.sleep(1);assert host.poll() is None,log.read_text()
            client=jack.Client("WeaveMidiSource",no_start_server=True)
            audio=client.outports.register("source")
            midi=client.midi_outports.register("keys")
            phase=0; pending=None
            @client.set_process_callback
            def process(n):
                nonlocal phase,pending
                audio.get_array()[:]=.2*np.sin(2*np.pi*220*(phase+np.arange(n))/client.samplerate)
                phase+=n; midi.clear_buffer()
                if pending:
                    for message in pending:midi.write_midi_event(0,bytes(message))
                    pending=None
            host.stdin.write("mode=2\ndry=-60\nwet=0\nmidi_input_channel=0\ntracking_profile=1\n");host.stdin.flush()
            with client:
                dst_audio=client.get_ports("^WeaveMidiNative:",is_audio=True,is_input=True)
                dst_midi=client.get_ports("^WeaveMidiNative:",is_midi=True,is_input=True)
                assert len(dst_audio)==1 and len(dst_midi)==1,(dst_audio,dst_midi)
                client.connect(audio,dst_audio[0]);client.connect(midi,dst_midi[0])
                pending=[(0x90,60,100),(0x90,64,100),(0x90,67,100),(0x90,71,100)]
                time.sleep(1)
                host.stdin.write("monitors\n");host.stdin.flush();time.sleep(.2)
                text=log.read_text()
                values={s:float(v) for s,v in re.findall(r"([A-Za-z_][A-Za-z_0-9]*)\s*=\s*([-+0-9.eE]+)",text)}
                assert values.get("held_notes")==4 and values.get("available_mask")==15,values
                assert abs(values.get("input_note",999)-57)<.03,values
                assert [values.get("midi_note%d"%v) for v in range(1,5)]==[60,64,67,71],values
                pending=[(0xb0,123,0)];time.sleep(.2)
                host.stdin.write("monitors\n");host.stdin.flush();time.sleep(.2)
                values={s:float(v) for s,v in re.findall(r"([A-Za-z_][A-Za-z_0-9]*)\s*=\s*([-+0-9.eE]+)",log.read_text())}
                assert values.get("held_notes")==0,values
            print("Jalv/JACK: real MIDI port, four absolute targets, shared input detection and all-notes-off passed")
        finally:
            if client:client.close()
            if host.poll() is None:
                host.terminate()
                try:host.wait(timeout=3)
                except subprocess.TimeoutExpired:host.kill();host.wait()
if __name__=="__main__":main()
