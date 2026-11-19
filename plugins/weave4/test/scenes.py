#!/usr/bin/env python3
"""Exercise scene banks through the real LV2 entry point."""
import math
import json
from check import Host, set_extra

PORTS = [0,1,2] + list(range(7,21)) + list(range(22,26)) + list(range(27,31)) + list(range(45,61))
DEFAULTS = {0:7,1:0,2:-6,7:0,8:0,9:0,10:1,11:0,12:4,13:-12,14:0,
            15:10,16:-12,17:0,18:12,19:-12,20:0}
for v in range(4):
    DEFAULTS[22+v] = -1
    DEFAULTS[27+v] = 0
    for o,x in enumerate((0,0,0,12)): DEFAULTS[45+v*4+o] = x

def bank(values, scene):
    return {74+(scene-1)*41+i: values[p] for i,p in enumerate(PORTS)}

def scene_checks():
    signal = [.1*math.sin(2*math.pi*220*i/48000) for i in range(24000)]
    values = dict(DEFAULTS)
    values.update({0:-5,1:-60,2:-9,7:1,8:5,10:4,27:-1,45:125,46:70})
    state = bank(values,1) | {70:1,71:1,197:0,198:0,33:0,62:0,32:3,31:-3}
    b = Host(extra=state,stereo=True)
    a = Host(pitch=values[0],dry=values[1],wet=values[2],extra={p:v for p,v in values.items() if p>6} | {32:3,31:-3},stereo=True)
    assert b.process(signal) == a.process(signal), 'bank B differs from canonical A'
    assert b.right == a.right
    assert b.extra[197].value == 1 and b.extra[198].value == 3
    assert b.extra[33].value == -8 and b.extra[62].value == 125
    assert b.extra[32].value == 3 and b.extra[31].value == -3
    # Native recall is independent of the custom widget; missing banks fall back to A.
    set_extra(b,70,3); b.process([0]*256)
    assert b.extra[197].value == 0
    set_extra(b,70,1); set_extra(b,71,0); b.process([0]*256)
    assert b.extra[197].value == 0
    a.close(); b.close()
    # Returning between identical banks must not reset any delay/pitch history.
    common = {p:v for p,v in values.items() if p>6} | bank(values,1) | {71:1,70:0}
    x=Host(pitch=values[0],dry=values[1],wet=values[2],extra=common,stereo=True)
    y=Host(pitch=values[0],dry=values[1],wet=values[2],extra=common,stereo=True)
    assert x.process(signal) == y.process(signal)
    set_extra(x,70,1)
    silence=[0]*24000
    assert x.process(silence) == y.process(silence), 'scene recall reset tails'
    # All bank controls survive serialization, including inactive scenes.
    restored={int(p):v for p,v in json.loads(json.dumps({p:float(v.value) for p,v in x.extra.items()})).items()}
    z=Host(pitch=values[0],dry=values[1],wet=values[2],extra=restored,stereo=True)
    fresh=Host(pitch=values[0],dry=values[1],wet=values[2],extra={p:v for p,v in values.items() if p>6},stereo=True)
    assert z.process(signal,[17,128,256]) == fresh.process(signal,[17,128,256])
    for h in (x,y,z,fresh): h.close()
    # B/C/D address ranges stay isolated, with invalid control values bounded.
    for scene in (1,2,3):
        state=bank(values,scene) | {70:scene,70+scene:1,197:0,33:0}
        h=Host(extra=state);h.process([0]*256)
        assert h.extra[197].value==scene and h.extra[33].value==-8
        h.close()
    print('scenes: native banks, empty fallback, canonical equivalence, tails, state controls and isolation passed')

if __name__ == '__main__': scene_checks()
