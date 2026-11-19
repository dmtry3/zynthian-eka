#!/usr/bin/env python3
"""Measured filter response, automation, bypass and native scene routing."""
import ctypes as c
import math
from check import LIB, Host, set_extra

class Tone(c.Structure):
    _fields_ = [(name,c.c_float) for name in (
        "high_g","low_g","target_high_g","target_low_g",
        "high_k1","high_k2","target_high_k1","target_high_k2",
        "low_k1","low_k2","target_low_k1","target_low_k2",
        "high_24","target_high_24","low_24","target_low_24",
        "high_s1","high_s2","high2_s1","high2_s2",
        "low_s1","low_s2","low2_s1","low2_s2",
        "mix","target_mix","smoothing")] + [("rate",c.c_double),("started",c.c_int)]
LIB.eka_tone_reset.argtypes=[c.POINTER(Tone),c.c_double]
LIB.eka_tone_set.argtypes=[c.POINTER(Tone),c.c_float,c.c_float,c.c_int]
LIB.eka_tone_shape.argtypes=[c.POINTER(Tone),c.c_float,c.c_int,c.c_int,c.c_int,c.c_int]
LIB.eka_tone_process.argtypes=[c.POINTER(Tone),c.c_uint32,c.POINTER(c.c_float)]

def apply(tone, source):
    data=(c.c_float*len(source))(*source)
    LIB.eka_tone_process(c.byref(tone),len(source),data)
    out=list(data); assert all(math.isfinite(x) for x in out)
    return out

def rms(xs):return math.sqrt(sum(x*x for x in xs)/len(xs))

def damping(slope_24, res_norm):
    if slope_24:
        return (0.7653668647301795 * (0.30 / 0.7653668647301795) ** res_norm,
                1.8477590650225735 * (0.95 / 1.8477590650225735) ** res_norm)
    return (1.4142135623730951 * (0.30 / 1.4142135623730951) ** res_norm, None)

def expected_db(hp,lp,hz,rate,res=0,hp24=0,lp24=0,hp_res=0,lp_res=1):
    warp=math.tan(math.pi*hz/rate)
    rh=warp/math.tan(math.pi*min(hp,rate*.45)/rate)
    rl=warp/math.tan(math.pi*min(lp,rate*.45)/rate)
    hk1,hk2=damping(hp24,(res/100.0) if hp_res else 0.0)
    lk1,lk2=damping(lp24,(res/100.0) if lp_res else 0.0)
    rh2,rl2=rh*rh,rl*rl
    hp_pow=(rh2*rh2)/((1-rh2)**2+(hk1*rh)**2)
    if hk2 is not None: hp_pow*=(rh2*rh2)/((1-rh2)**2+(hk2*rh)**2)
    lp_pow=1.0/((1-rl2)**2+(lk1*rl)**2)
    if lk2 is not None: lp_pow*=1.0/((1-rl2)**2+(lk2*rl)**2)
    return 10*math.log10(hp_pow*lp_pow)

def tone_checks():
    tone=Tone();LIB.eka_tone_reset(c.byref(tone),48000)
    source=[.1*math.sin(2*math.pi*440*i/48000) for i in range(48000)]
    original=list((c.c_float*len(source))(*source))
    LIB.eka_tone_set(c.byref(tone),4000,200,0)
    assert apply(tone,source)==original,'default bypass rounds audio'
    for hp,lp,hz,res,hp24,lp24,hp_res,lp_res in [
        (1000,20000,200,0,0,0,0,1),(1000,20000,1000,0,0,0,0,1),
        (20,1000,5000,0,0,0,0,1),(20,1000,1000,0,0,0,0,1),
        (1000,1000,1000,0,0,0,0,1),(20,20000,20000,0,0,0,0,1),
        (1000,20000,200,0,1,0,0,1),(1000,20000,1000,0,1,0,0,1),
        (20,1000,4000,0,0,1,0,1),(20,1000,1000,0,0,1,0,1),
        (20,1000,1000,100,0,0,0,1),(20,1000,1000,100,0,1,0,1),
        (1000,20000,1000,100,0,0,1,0),(1000,20000,1000,100,1,0,1,0),
        (20,1000,1000,100,0,0,0,0)]:
        LIB.eka_tone_reset(c.byref(tone),48000)
        LIB.eka_tone_set(c.byref(tone),hp,lp,1)
        LIB.eka_tone_shape(c.byref(tone),res,hp24,lp24,hp_res,lp_res)
        signal=[.1*math.sin(2*math.pi*hz*i/48000) for i in range(24000)]
        out=apply(tone,signal)
        db=20*math.log10(rms(out[12000:])/rms(signal[12000:]))
        exp=expected_db(hp,lp,hz,48000,res,hp24,lp24,hp_res,lp_res)
        assert abs(db-exp)<.08,(hp,lp,hz,res,hp24,lp24,hp_res,lp_res,db,exp)
    # Rapid extreme sweeps, bypass fades and all supported rates stay bounded.
    for rate in (8000,48000,192000):
        LIB.eka_tone_reset(c.byref(tone),rate)
        for i in range(200):
            LIB.eka_tone_set(c.byref(tone),20 if i%2 else 4000,200 if i%2 else 20000,i%7!=0)
            out=apply(tone,[.1*math.sin(2*math.pi*min(440,rate/8)*(i*128+j)/rate) for j in range(128)])
            assert max(map(abs,out))<.8
        LIB.eka_tone_set(c.byref(tone),float('nan'),float('inf'),0)
        apply(tone,[0.0]*int(rate))
        assert apply(tone,source[:256])==original[:256]
    # Runtime filter enable is gradual, without an instantaneous DC output jump.
    LIB.eka_tone_reset(c.byref(tone),48000);LIB.eka_tone_set(c.byref(tone),1000,2000,0)
    apply(tone,[.1]*1024);LIB.eka_tone_set(c.byref(tone),1000,2000,1)
    out=apply(tone,[.1]*4096)
    assert abs(out[0]-.1)<.001 and max(abs(a-b) for a,b in zip(out,out[1:]))<.002
    # Actual plugin: dry remains untouched, processing partition is irrelevant.
    a=Host(pitch=0,extra={255:1000,256:2000,257:1},stereo=True)
    b=Host(pitch=0,extra={255:1000,256:2000,257:1},stereo=True)
    assert a.process(source)==b.process(source,[1,17,128,255,256])
    a.close();b.close()
    for scene in (0,1,2,3):
        base=255 if scene==0 else 267+(scene-1)*12
        controls={base:1000,base+1:2000,base+2:1,70:scene,21:0}
        if scene:
            controls[70+scene]=1
            bank=74+(scene-1)*41
            controls.update({bank:0,bank+1:0,bank+2:-60})
        h=Host(pitch=0,dry=0,wet=-60,extra=controls)
        assert h.process(source)==original,'filters changed dry root'
        h.close()
        # Connect scene pitch=0, so filtered and plain host test the same note.
        if scene:controls.update({bank:0,bank+1:-60,bank+2:0})
        filtered=Host(pitch=0,extra=controls)
        plain=Host(pitch=0,extra=controls | {base+2:0})
        x=filtered.process(source);y=plain.process(source)
        assert rms(x[12000:])<rms(y[12000:])*.25,scene
        filtered.close();plain.close()
    print('tone: measured HPF/LPF response, exact bypass, sweeps, rates, dry, partition and scene routing passed')

if __name__=='__main__':tone_checks()
