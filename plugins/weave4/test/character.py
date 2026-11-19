#!/usr/bin/env python3
"""Fine tuning and bounded character through the actual LV2 entry point."""
import math
from check import Host, set_extra

def character_checks():
    # Enough cycles to measure a half-semitone accurately without FFT dependencies.
    signal=[.1*math.sin(2*math.pi*440*i/48000) for i in range(48000*4)]
    for fine in (-50,50):
        h=Host(pitch=0,extra={199:fine,247:0,251:0})
        out=h.process(signal)[48000:]
        crossings=sum(a<=0<b for a,b in zip(out,out[1:])) / 3
        expected=440*2**(fine/1200)
        assert abs(crossings-expected)<.7,(fine,crossings,expected)
        assert abs(h.extra[247].value-fine)<.001
        assert h.extra[251].value==0
        h.close()
    controls={10:4,199:12,200:100,201:1,202:-8,203:100,204:.7,
              205:3,206:100,207:.3,208:-3,209:100,210:.1}
    a=Host(extra=controls,stereo=True)
    b=Host(extra=controls,stereo=True)
    x=a.process(signal)
    y=b.process(signal,[1,17,64,255,256])
    difference=max(abs(u-v) for u,v in zip(x,y))
    assert difference<1e-6,('partition',difference)
    a.close();b.close()
    h=Host(pitch=0,extra=controls | {**{247+v:0 for v in range(4)},**{251+v:0 for v in range(4)}})
    readings=[[] for _ in range(4)]
    for i in range(150):
        h.process(signal[:4096])
        for v in range(4):
            cents=h.extra[247+v].value
            timing=h.extra[251+v].value
            fine=controls[199+v*3]
            assert fine-8.01<=cents<=fine+8.01,(v,cents)
            assert 0<=timing<=12.01,timing
            readings[v].append((round(cents,3),round(timing,3)))
    assert all(len(set(row))>30 for row in readings)
    assert all(readings[0]!=row for row in readings[1:])
    # Supplementary B-D fields append after the original 41-field scene banks.
    for scene in (1,2,3):
        set_extra(h,70+scene,1);set_extra(h,70,scene)
        set_extra(h,211+(scene-1)*12,25)
        set_extra(h,211+(scene-1)*12+1,0)
        h.process(signal[:48000])
        assert abs(h.extra[247].value-25)<.02
        assert h.extra[251].value<.001
    h.close()
    for upper in (False, True):
        controls = ({7:1,8:9,10:4,208:50,209:100,250:0} if upper
                    else {199:-50,200:100,247:0})
        h=Host(pitch=-12,extra=controls)
        h.process(signal[:48000])
        actual=h.extra[250 if upper else 247].value
        assert abs(actual)<.001,('backend range clamp',actual)
        h.close()
    # Humanize may advance silently before the first note. The current target
    # remains bounded and is applied when the sleeping shifter wakes.
    h=Host(pitch=7,extra={10:0,199:7,200:100,201:.5,247:0})
    assert max(map(abs,h.process([0.0]*96000)))==0
    assert -1.01<=h.extra[247].value<=15.01
    set_extra(h,10,1)
    out=h.process(signal)
    steady=out[-48000:]
    crossings=sum(a<=0<b for a,b in zip(steady,steady[1:]))
    expected=440*2**((7+.07)/12)
    assert abs(crossings-expected)<4,(crossings,expected)
    assert max(map(abs,steady))>.01
    h.close()
    print('character: cents frequency, bounded independent drift, partition equivalence and scene fields passed')

if __name__=='__main__':character_checks()
