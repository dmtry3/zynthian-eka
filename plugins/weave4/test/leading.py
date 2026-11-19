"""Voice-leading spec/oracle and actual LV2 transitions, without audio exports."""
import ctypes as c
import itertools
import math
from check import LIB, Host, set_extra
from four_voices import power

class Leading(c.Structure):
    _fields_=[('previous',c.c_float*4),('input',c.c_float*4),('count',c.c_uint),
              ('input_count',c.c_uint),('valid',c.c_int),('cached',c.c_int)]
F4=c.c_float*4
LIB.eka_leading_reset.argtypes=[c.POINTER(Leading)]
LIB.eka_leading_seed.argtypes=[c.POINTER(Leading),c.POINTER(c.c_float),c.c_uint]
LIB.eka_leading_apply.argtypes=[c.POINTER(Leading),c.POINTER(c.c_float),c.c_uint,c.POINTER(c.c_float)]
CHORDS=((4,7),(3,7),(4,7,10),(4,7,11),(3,7,10),(4,7,11,14),
        (3,7,10,14),(4,7,9,14),(4,7,10,14),(4,10,14,21),(4,7,10,15),(2,7),(5,7),(3,6,9))

def apply(state,notes):
    out=F4();LIB.eka_leading_apply(c.byref(state),F4(*(list(notes)+[0]*(4-len(notes)))),len(notes),out)
    return tuple(round(x) for x in out[:len(notes)])

def score(output,permutation,previous):
    motion=[abs(x-y) for x,y in zip(output,previous)]
    crossings=sum((previous[a]-previous[b])*(output[a]-output[b])<0
                  for a in range(len(output)) for b in range(a+1,len(output)))
    return (sum(motion),max(motion,default=0),crossings,sum(abs(x-y) for x,y in zip(output,permutation)))

def oracle(previous,target):
    refs=previous[:len(target)]+target[len(previous):]
    best=None
    for permutation in itertools.permutations(target):
        choices=[tuple(n for n in range(-12,22) if (n-tone)%12==0) for tone in permutation]
        for result in itertools.product(*choices):
            cost=score(result,permutation,refs)
            if best is None or cost<best:best=cost
    return best

def core_checks():
    for previous in CHORDS:
        for target in CHORDS:
            state=Leading();LIB.eka_leading_seed(c.byref(state),F4(*(list(previous)+[0]*(4-len(previous)))),len(previous))
            result=apply(state,target)
            assert sorted(n%12 for n in result)==sorted(n%12 for n in target)
            assert all(-12<=n<=21 for n in result)
            refs=previous[:len(target)]+target[len(previous):]
            possible=[score(result,p,refs) for p in itertools.permutations(target)
                      if all((a-b)%12==0 for a,b in zip(result,p))]
            assert min(possible)==oracle(previous,target),(previous,target,result)
            assert apply(state,target)==result,'unchanged target must not drift'
    a,b=Leading(),Leading()
    for chord in CHORDS*20:
        assert apply(a,chord)==apply(b,chord),'deterministic independent instances'
    LIB.eka_leading_reset(c.byref(a))
    assert apply(a,CHORDS[9])==CHORDS[9]
    print('Lead core: all 196 chord pairs globally optimal, pitch classes/range, repeated-target stability and reset passed')

def intervals(h):return [round(h.extra[i].value) for i in range(33,37)]

def leading_checks():
    core_checks()
    monitors={i:0 for i in range(33,37)}
    settings={7:1,8:5,9:4,10:4,13:-18,16:-18,19:-18,21:0}|monitors
    source=[.1*math.sin(2*math.pi*220*i/48000) for i in range(96000)]
    h=Host(wet=-18,extra=settings)
    h.process(source[:256]);assert intervals(h)==[4,7,11,14]
    set_extra(h,8,9)
    audio=h.process(source)[48000:]
    assert intervals(h)==[4,9,10,14],intervals(h)
    assert sum(abs(x-y) for x,y in zip(intervals(h),[4,7,11,14]))==3
    for note in [4,9,10,14]:assert power(audio,220*2**(note/12))>1e-5,note
    for port,val in [(23,0),(26,3),(32,4),(31,-6)]:set_extra(h,port,val)
    h.process(source[:256]);assert intervals(h)==[4,9,10,14]
    set_extra(h,27,-1);set_extra(h,28,1)
    h.process(source[:256]);assert intervals(h)==[-8,21,10,14]
    # Close and old octave semantics remain unchanged; re-enter Lead anchors to it.
    set_extra(h,9,0);h.process(source[:256]);assert intervals(h)==[-8,10,14,21]
    set_extra(h,9,4);h.process(source[:256]);assert intervals(h)==[-8,10,14,21]
    set_extra(h,7,0);h.process(source[:256]);set_extra(h,7,1)
    set_extra(h,27,0);set_extra(h,28,0)
    h.process(source[:256]);assert intervals(h)==[4,10,14,21]
    set_extra(h,8,5);h.process(source[:256])
    h.d.activate(h.handle);h.process(source[:256]);assert intervals(h)==[4,7,11,14]
    h.close()
    # Scene A Close -> B Lead; inactive/active canonical banks stay untouched.
    h=Host(wet=-18,extra=(settings|{9:0,70:0,71:1,77:1,78:9,79:4,80:4}))
    h.process(source[:256]);set_extra(h,70,1);h.process(source[:256])
    assert intervals(h)==[4,9,10,14]
    assert h.extra[9].value==0 and h.extra[79].value==4
    set_extra(h,70,0);h.process(source[:256]);assert intervals(h)==[4,7,11,14]
    h.close()
    # Identical chord changes at identical sample positions, different host partitions.
    a=Host(block=512,wet=-18,extra=settings);b=Host(block=512,wet=-18,extra=settings)
    for chord in [5,9,6,0,13,5]:
        set_extra(a,8,chord);set_extra(b,8,chord)
        x=a.process(source[:12000]);y=b.process(source[:12000],[1,63,127,511,17])
        assert max(abs(p-q) for p,q in zip(x,y))<1e-6
        assert intervals(a)==intervals(b)
    a.close();b.close()
    print('Lead LV2: audible notes, common-tone example, manual octaves, independent controls, scenes, reset and partition invariance passed')

if __name__=='__main__':leading_checks()
