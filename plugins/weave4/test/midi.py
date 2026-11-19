"""Actual LV2 MIDI note lifecycle, absolute targets and pitch-tracking tests."""
import ctypes as c
import math
import struct
from check import Host, set_extra
from delays import Mapper, atom, pad
MIDI = "http://lv2plug.in/ns/ext/midi#MidiEvent"
def send(host, mapper, events=()):
    body = struct.pack("<II", 0, 0)
    for frame, data in events:
        payload = bytes(data)
        body += struct.pack("<qII", frame, len(payload), mapper.id(MIDI)) + pad(payload)
    host.midi_buffer = c.create_string_buffer(atom(mapper, "Sequence", body))
    host.d.connect(host.handle, 61, host.midi_buffer)
def notes(host, mapper, *messages):
    send(host, mapper, [(0, message) for message in messages])
    host.d.run(host.handle, 0)
    send(host, mapper)
def targets(h):
    return [round(h.extra[308+v].value) for v in range(4)]
def midi_host(mapper, **kw):
    settings = {7:2, 10:1, 21:0, 304:0, 305:0, 306:0, 307:0, 312:0, 313:0,
                33:0, 34:0, 35:0, 36:0, 41:0, 44:0}
    settings.update({308+v:0 for v in range(4)})
    settings.update(kw.pop("extra", {}))
    return Host(features=mapper.features, extra=settings, **kw)
def source(freq, n=48000, start=0):
    return [.2 * math.sin(2*math.pi*freq*(i+start)/48000) for i in range(n)]
def midi_checks():
    m=Mapper(); h=midi_host(m)
    notes(h,m,(0x90,60,100),(0x90,64,100),(0x90,67,100),(0x90,71,100))
    assert targets(h)==[60,64,67,71], targets(h)
    h.process(source(220,24000))
    assert abs(h.extra[306].value-57)<.03, h.extra[306].value
    assert h.extra[44].value==15 and h.extra[41].value==15
    for v,target in enumerate(targets(h)):
        assert abs(h.extra[33+v].value-(target-57))<.03
    # Four lowest distinct notes; common targets keep physical voice parameters.
    notes(h,m,(0x90,48,100)); assert targets(h)==[60,64,67,48], targets(h)
    assert h.extra[312].value==5
    notes(h,m,(0x80,48,0)); assert targets(h)==[60,64,67,71]
    notes(h,m,(0xb0,64,127),(0x80,60,0))
    assert targets(h)==[60,64,67,71]
    notes(h,m,(0xb0,64,0)); assert targets(h)==[-1,64,67,71]
    notes(h,m,(0x90,60,90),(0x90,60,90),(0x90,60,0))
    assert targets(h)==[60,64,67,71]
    notes(h,m,(0x80,60,0)); assert targets(h)==[-1,64,67,71]
    notes(h,m,(0xb0,123,0)); assert targets(h)==[-1]*4
    # Channel filtering never leaves notes stranded when the filter changes.
    notes(h,m,(0x91,62,100)); assert targets(h)[0]==62
    set_extra(h,304,1); h.d.run(h.handle,0); assert targets(h)==[-1]*4
    set_extra(h,304,2); h.d.run(h.handle,0); assert targets(h)[0]==62
    notes(h,m,(0xb1,120,0)); assert targets(h)==[-1]*4
    # Unreachable target mutes, confidence loss expires, reactivation clears keys.
    set_extra(h,304,0); notes(h,m,(0x90,120,100))
    assert h.extra[44].value==0
    h.process(source(220,12000)); assert h.extra[44].value==0
    notes(h,m,(0xff,)); assert targets(h)==[-1]*4
    notes(h,m,(0x90,60,100)); h.process(source(220,12000))
    assert h.extra[44].value==1
    h.process([0.]*16000); assert h.extra[44].value==0
    h.d.activate(h.handle); h.d.run(h.handle,0); assert targets(h)==[-1]*4
    h.close()
    # Absolute A4 stays at 440Hz for both 220Hz and 330Hz input.
    for freq in (220,330):
        h=midi_host(m); notes(h,m,(0x90,69,100))
        audio=h.process(source(freq,96000)); steady=audio[48000:]
        crossings=sum(a<=0<b for a,b in zip(steady,steady[1:]))
        assert abs(crossings-440)<3, (freq,crossings)
        h.close()
    # Host partitions must not alter tracker decisions or shifted samples.
    a=midi_host(m,block=1024); b=midi_host(m,block=1024)
    for h in (a,b): notes(h,m,(0x90,60,100),(0x90,64,100))
    signal=source(220,24000)+source(330,24000)
    x=a.process(signal); y=b.process(signal,[1,17,63,129,511,1024])
    error=max(abs(i-j) for i,j in zip(x,y)); assert error<1e-6,error
    a.close(); b.close()
    # Timestamped events give the same audio as explicitly splitting at the event.
    a=midi_host(m); b=midi_host(m)
    for h in (a,b):
        notes(h,m,(0x90,60,100)); h.process(source(220,24000))
    wave=source(220,256,start=24000)
    send(a,m,[(73,(0x80,60,0)),(129,(0x90,64,100))])
    first=a.process(wave); send(a,m)
    second=b.process(wave[:73]); notes(b,m,(0x80,60,0))
    second+=b.process(wave[73:129]); notes(b,m,(0x90,64,100))
    second+=b.process(wave[129:])
    assert first==second
    a.close(); b.close()
    # Mode MIDI persists in native scene banks without changing bank order.
    h=midi_host(m,extra={70:1,71:1,77:2})
    notes(h,m,(0x90,60,100));h.process(source(220,12000))
    assert targets(h)[0]==60 and h.extra[44].value==1
    # Overflow and malformed sequence cannot strand held keys.
    send(h,m,[(0,(0x90,64,100))]*257);h.d.run(h.handle,0)
    assert targets(h)==[-1]*4
    notes(h,m,(0x90,60,100))
    h.midi_buffer=c.create_string_buffer(struct.pack("<IIII",1,m.id("http://lv2plug.in/ns/ext/atom#Sequence"),0,0))
    h.d.connect(h.handle,61,h.midi_buffer);h.d.run(h.handle,0)
    assert targets(h)==[-1]*4
    h.close()
    # Click-free attack when MIDI Note-On arrives at a waveform peak or quick retrigger.
    h=midi_host(m,dry=-60,wet=0)
    cos_wave=[.2*math.cos(2*math.pi*220*i/48000) for i in range(14400)]
    h.process(cos_wave)
    notes(h,m,(0x90,64,100))
    out=h.process(cos_wave)
    assert max(abs(b-a) for a,b in zip(out,out[1:]))<.02
    notes(h,m,(0x80,64,0))
    out=h.process(cos_wave[:240])
    notes(h,m,(0x90,67,100))
    out+=h.process(cos_wave)
    assert max(abs(b-a) for a,b in zip(out,out[1:]))<.02
    h.close()
    # Voice profile (3): 75-1100 Hz band and stable -12 interval with +30c sharp vocal pitch.
    h=midi_host(m,dry=-60,wet=0,extra={305:3})
    notes(h,m,(0x90,45,100)) # A2 = 110 Hz (-12 from A3=220 Hz)
    h.process(source(220 * 2**(0.30/12), 12000)) # A3 + 30 cents sharp
    assert h.extra[44].value == 1, h.extra[44].value
    notes(h,m,(0x80,45,0),(0x90,44,100)) # G#2 (-13.3 st) remains out of range
    h.process(source(220 * 2**(0.30/12), 6000))
    assert h.extra[44].value == 0, h.extra[44].value
    notes(h,m,(0x80,44,0),(0x90,60,100))
    h.process(source(130.81, 12000)) # C3 male voice -> tracked
    assert h.extra[44].value == 1 and abs(h.extra[306].value - 48) < .1
    h.process(source(1800, 12000)) # above Voice 1100 Hz ceiling -> rejected
    assert h.extra[44].value == 0
    h.close()
    print("MIDI: absolute notes, tracking, four voices, stable slots, sustain, repeated keys, channels, panic, range, silence, reset and partitions passed")
if __name__ == "__main__": midi_checks()
