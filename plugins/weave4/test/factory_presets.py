#!/usr/bin/env python3
"""Exercise generated factory values through DSP and optionally native Jalv presets."""
import ctypes as c
import math
import re
from pathlib import Path
from check import Host
ROOT=Path(__file__).resolve().parents[1]
URI="http://dmtry3.github.io/zynthian-eka/weave4"
def load():
    ttl=(ROOT/'lv2/eka_weave4.ttl').read_text();indices={}
    for p in re.split(r'(?=    \[ a lv2:(?:Input|Output)Port)',ttl):
        if not p.startswith('    [ a lv2:InputPort, lv2:ControlPort'):continue
        symbol=re.search(r'lv2:symbol "([^"]+)"',p).group(1)
        index=int(re.search(r'lv2:index (\d+)',p).group(1));indices[symbol]=index
    text=(ROOT/'build/eka-weave4.lv2/factory-presets.ttl').read_text()
    presets=[]
    for part in text.split(' a pset:Preset ;')[1:]:
        name=re.search(r'rdfs:label "([^"]+)"',part).group(1)
        values={s:float(v) for s,v in re.findall(r'lv2:symbol "([^"]+)" ; pset:value (-?[0-9.eE+]+)',part)}
        assert set(values)==set(indices),(name,set(indices)-set(values))
        assert values['dry_timing']==0 and values['scene']==0 and values['solo']==0
        assert all(values['scene_'+s+'_ready']==0 for s in 'bcd')
        presets.append((name,values))
    assert len(presets)==9
    return indices,presets

def checks():
    from delays import Mapper
    from midi import notes
    indices,presets=load();peaks={}
    wave=[.20*math.sin(2*math.pi*293.66477*i/48000)+.06*math.sin(2*math.pi*587.32954*i/48000) for i in range(72000)]
    for name,values in presets:
        mapper=Mapper()
        extra={index:values[symbol] for symbol,index in indices.items() if index>=7}
        h=Host(pitch=values['pitch'],dry=values['dry'],wet=values['wet'],extra=extra,stereo=True,features=mapper.features)
        if values['mode']==2:notes(h,mapper,(0x90,62,100),(0x90,65,100),(0x90,69,100),(0x90,72,100))
        audio=h.process(wave)
        peak=max(map(abs,audio+h.right));assert 0.01<peak<.95,(name,peak)
        assert h.latency.value==0
        peaks[name]=round(peak,4)
        h.close()
    print('Factory presets: nine complete banks, finite stereo audio, Live dry, MIDI held chords and test-signal headroom passed',peaks)

def jalv_checks():
    from jalv_scenes import session
    indices,presets=load()
    subjects=re.findall(r'<([^>]+)> a pset:Preset',(ROOT/'build/eka-weave4.lv2/factory-presets.ttl').read_text())
    assert len(subjects)==9
    for number,((name,values),subject) in enumerate(zip(presets,subjects)):
        _,_,actual=session(['solo=3','scene_b_ready=1','scene=1','preset '+subject],'FactoryPreset%d'%number)
        for symbol in ('mode','pitch','wet','dry','master','voices','enable1','enable2','solo','scene','scene_b_ready','tracking_profile','scale_tonic','scale_mode','degree1','pitch2','delay_ms1','feedback1','sync1','division1','fine1','humanize1','tone_on1'):
            assert abs(actual.get(symbol,999)-values[symbol])<1e-5,(name,symbol,actual.get(symbol),values[symbol])
    print('Jalv: nine factory preset URIs loaded, previous Solo/scene cleared, key/voices/time/character/tone restored')
if __name__=='__main__':
    import sys
    checks()
    if '--jalv' in sys.argv:jalv_checks()
