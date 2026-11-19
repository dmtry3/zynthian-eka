"""Independent diatonic oracle plus actual LV2 Scale audio/scene/tracking checks."""
import ctypes as c
import math
from check import LIB, Host, set_extra
SCALES=((0,2,4,5,7,9,11),(0,2,3,5,7,8,10),(0,2,3,5,7,9,10),
        (0,1,3,5,7,8,10),(0,2,4,6,7,9,11),(0,2,4,5,7,9,10),
        (0,1,3,5,6,8,10),(0,2,3,5,7,8,11),(0,2,3,5,7,9,11))
class Scale(c.Structure):
    _fields_=[("anchor",c.c_int),("valid",c.c_int),("tonic",c.c_uint),("scale",c.c_uint)]
LIB.eka_scale_reset.argtypes=[c.POINTER(Scale)]
LIB.eka_scale_targets.argtypes=[c.POINTER(Scale),c.c_uint,c.c_uint,c.c_float,c.POINTER(c.c_int),c.POINTER(c.c_float)]
def targets(state,key,mode,note,steps):
    out=(c.c_float*4)()
    valid=LIB.eka_scale_targets(c.byref(state),key,mode,note,(c.c_int*4)(*steps),out)
    return valid,list(out)
def core_checks():
    state=Scale(); cases=0
    for key in range(12):
        for mode,pcs in enumerate(SCALES):
            ladder=sorted(key+12*o+pc for o in range(-3,16) for pc in pcs)
            for source in range(24*4,100*4,3):
                note=source/4
                for steps in ((2,4,6,7),(-7,-4,-2,0),(1,3,5,6)):
                    LIB.eka_scale_reset(c.byref(state))
                    index=min(range(len(ladder)),key=lambda i:(abs(ladder[i]-note),ladder[i]))
                    expected=[ladder[index+d] for d in steps]
                    ok,out=targets(state,key,mode,note,steps)
                    assert ok and out==expected,(key,mode,note,steps,out,expected)
                    cases+=1
    LIB.eka_scale_reset(c.byref(state))
    for note,expected in ((60.8,60),(61.05,60),(61.12,62),(60.99,62),(60.88,60)):
        assert targets(state,0,0,note,(0,0,0,0))[1][0]==expected
    assert targets(state,0,1,61.05,(0,0,0,0))[1][0]==62 # Key/scale change clears hysteresis.
    for note in (float("nan"),float("inf"),-1,200):
        assert targets(state,0,0,note,(0,0,0,0))==(0,[-1]*4)
    print("Scale core:",cases,"oracle cases, nine modes, twelve keys, signed degrees, boundaries and hysteresis passed")
def signal(note,n=48000):
    f=440*2**((note-69)/12)
    return [.2*math.sin(2*math.pi*f*i/48000) for i in range(n)]
def host(**settings):
    extras={7:3,21:0,33:0,34:0,35:0,36:0,44:0,306:0,307:0,
            314:0,315:0,316:2,317:4,318:6,319:7,
            **{338+v:0 for v in range(4)}}
    extras.update(settings.pop("extra",{}))
    return Host(wet=0,extra=extras,**settings)
def scale_checks():
    core_checks()
    # C major thirds change between 4 and 3 semitones as the melody moves.
    for note,target in ((60,64),(62,65),(64,67),(65,69),(67,71),(69,72),(71,74)):
        h=host();out=h.process(signal(note,72000));steady=out[-48000:]
        hz=sum(a<=0<b for a,b in zip(steady,steady[1:]))
        expected=440*2**((target-69)/12)
        assert abs(hz-expected)<3,(note,target,hz,expected)
        assert abs(h.extra[33].value-(target-note))<.03
        assert h.extra[338].value==target
        h.close()
    h=host(extra={10:4})
    h.process(signal(60,24000)); assert [h.extra[338+v].value for v in range(4)]==[64,67,71,72]
    # New scene banks preserve the original scene field order and A controls.
    set_extra(h,70,1);set_extra(h,71,1);set_extra(h,77,3)
    for i,x in enumerate((2,0,2,4,6,7)):set_extra(h,320+i,x)
    h.process(signal(62,24000));assert h.extra[338].value==66
    assert h.extra[314].value==0 and h.extra[316].value==2
    set_extra(h,322,-2);h.d.run(h.handle,0);assert h.extra[338].value==59
    set_extra(h,70,0);h.process(signal(62,24000));assert h.extra[338].value==65
    # Negative degrees, controls at zero frames, reset/uncertain pitch and dry Live.
    set_extra(h,316,-2);h.d.run(h.handle,0);assert h.extra[338].value==59
    h.process([0.]*16000);assert h.extra[44].value==0
    h.d.activate(h.handle);h.d.run(h.handle,0);assert h.extra[338].value==-1
    h.close()
    h=host(dry=0,extra={22:0,23:0,24:0,25:0})
    out=h.process([1.]+[0.]*255);assert out[0]==1 and h.latency.value==0
    h.close()
    a=host(block=1024);b=host(block=1024)
    wave=signal(60,12000)+signal(62,12000)+signal(64,12000)
    x=a.process(wave);y=b.process(wave,[1,17,63,129,511,1024])
    assert max(abs(i-j) for i,j in zip(x,y))<1e-6
    a.close();b.close()
    # Keyboard notes are irrelevant to Scale and cannot retime its pitch decisions.
    from delays import Mapper
    from midi import send
    m=Mapper();a=host(features=m.features);b=host(features=m.features)
    wave=signal(60,24000)
    assert a.process(wave)==b.process(wave)
    send(a,m,[(31,(0x90,60,100)),(91,(0x80,60,0)),(201,(0xb0,64,127))])
    send(b,m)
    assert a.process(signal(61,256))==b.process(signal(61,256))
    a.close();b.close()
    print("Scale LV2: diatonic third audio, four targets, scenes, negative degrees, silence/reset, Live dry and host partitions passed")
if __name__=="__main__":scale_checks()
