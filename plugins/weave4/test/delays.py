"""Delay/feedback DSP and timestamped LV2 host-tempo regression checks."""
import ctypes as c
import math
import struct
from check import Host, LIB, set_extra

FloatPtr = c.POINTER(c.c_float)
LIB.eka_delay_create.argtypes = [c.c_double]
LIB.eka_delay_create.restype = c.c_void_p
LIB.eka_delay_destroy.argtypes = [c.c_void_p]
LIB.eka_delay_reset.argtypes = [c.c_void_p]
LIB.eka_delay_set.argtypes = [c.c_void_p, c.c_double, c.c_float]
LIB.eka_delay_process.argtypes = [c.c_void_p, c.c_uint32, FloatPtr]
LIB.eka_delay_milliseconds.argtypes = [c.c_void_p]
LIB.eka_delay_milliseconds.restype = c.c_double

MapCallback = c.CFUNCTYPE(c.c_uint32, c.c_void_p, c.c_char_p)
class URIDMap(c.Structure):
    _fields_ = [("handle", c.c_void_p), ("map", MapCallback)]
class Feature(c.Structure):
    _fields_ = [("uri", c.c_char_p), ("data", c.c_void_p)]

ATOM = "http://lv2plug.in/ns/ext/atom#"
TIME = "http://lv2plug.in/ns/ext/time#"
class Mapper:
    def __init__(self):
        self.ids = {}
        self.callback = MapCallback(lambda handle, uri: self.id(uri.decode()))
        self.mapping = URIDMap(None, self.callback)
        self.feature = Feature(b"http://lv2plug.in/ns/ext/urid#map", c.cast(c.pointer(self.mapping), c.c_void_p))
        self.array = (c.POINTER(Feature) * 2)(c.pointer(self.feature), None)
        self.features = c.cast(self.array, c.c_void_p)
    def id(self, uri):
        if uri not in self.ids:
            self.ids[uri] = len(self.ids) + 1
        return self.ids[uri]

def pad(data):
    return data + b"\0" * (-len(data) % 8)

def atom(mapper, kind, body):
    return struct.pack("<II", len(body), mapper.id(ATOM + kind)) + body

def position_buffer(mapper, bpm=None, beats_per_bar=None, beat_unit=None, frame=0, kind="Float"):
    properties = b""
    for key, value in (("beatsPerMinute", bpm), ("beatsPerBar", beats_per_bar), ("beatUnit", beat_unit)):
        if value is None:
            continue
        format_code = {"Float": "<f", "Double": "<d", "Int": "<i"}[kind]
        value_atom = atom(mapper, kind, struct.pack(format_code, value))
        properties += struct.pack("<II", mapper.id(TIME + key), 0) + pad(value_atom)
    body = struct.pack("<II", 0, mapper.id(TIME + "Position")) + properties
    event = struct.pack("<q", frame) + pad(atom(mapper, "Object", body))
    sequence = atom(mapper, "Sequence", struct.pack("<II", 0, 0) + event)
    return c.create_string_buffer(sequence)

def send_position(host, mapper, **values):
    host.time_buffer = position_buffer(mapper, **values)
    host.d.connect(host.handle, 61, host.time_buffer)

def raw_process(handle, samples):
    data = (c.c_float * len(samples))(*samples)
    original = list(data)
    LIB.eka_delay_process(handle, len(samples), data)
    return original, list(data)

def delay_checks():
    delay = LIB.eka_delay_create(48000)
    assert delay
    LIB.eka_delay_set(delay, 100, .5)
    _, output = raw_process(delay, [1.] + [0.] * 19999)
    for index, level in ((4800, 1), (9600, .5), (14400, .25), (19200, .125)):
        assert abs(output[index] - level) < 1e-6, (index, output[index])
    assert sum(abs(x) for i,x in enumerate(output) if i not in (4800, 9600, 14400, 19200)) == 0
    LIB.eka_delay_reset(delay)
    _, output = raw_process(delay, [0.] * 20000)
    assert max(map(abs, output)) == 0

    # A true zero-delay bypass does not add gain or feedback.
    LIB.eka_delay_reset(delay)
    LIB.eka_delay_set(delay, 0, .9)
    source = [.2 * math.sin(i / 13) for i in range(1000)]
    original, output = raw_process(delay, source)
    assert output == original
    LIB.eka_delay_reset(delay)
    LIB.eka_delay_set(delay, .5 / 48, 0)
    _, output = raw_process(delay, [1.] + [0.] * 10)
    assert output[:2] == [.5, .5] and max(map(abs, output[2:])) == 0

    # Rapid retiming uses bounded fixed-tap crossfades.
    LIB.eka_delay_reset(delay)
    LIB.eka_delay_set(delay, 100, 0)
    raw_process(delay, [.2] * 60000)
    for milliseconds in (500, 0, 800, 17.3, 300, 10, 0):
        LIB.eka_delay_set(delay, milliseconds, 0)
        _, output = raw_process(delay, [.2] * 128)
        assert max(abs(x - .2) for x in output) < 1e-6
    LIB.eka_delay_destroy(delay)

    # Maximum time clamps safely and supports the oldest circular-buffer tap.
    delay = LIB.eka_delay_create(8000)
    LIB.eka_delay_set(delay, 10000, 0)
    assert LIB.eka_delay_milliseconds(delay) == 8000
    _, output = raw_process(delay, [1.] + [0.] * 64000)
    assert output[-1] == 1 and max(map(abs, output[:-1])) == 0
    LIB.eka_delay_destroy(delay)

    # Tail survives stopping the voice input, but the voice's fader can still mute it.
    host = Host(pitch=0, wet=0, extra={22: 1, 45: 200, 46: 50})
    before = host.process([1.] + [0.] * 4799)
    pitch_latency = host.latency.value
    set_extra(host, 22, 0)
    after = host.process([0.] * 40000)
    full = before + after
    peak = max(range(len(full)), key=lambda i: abs(full[i]))
    assert abs(peak - (pitch_latency + 9600)) <= 2, peak
    assert abs(full[peak + 9600] / full[peak] - .5) < .002
    host.controls[2].value = -60
    muted = host.process([0.] * 24000)
    assert max(map(abs, muted[12000:])) == 0
    host.close()

    # Host tempo, meter, subdivisions and bounded time.
    mapper = Mapper()
    host = Host(pitch=0, extra={47: 1, 48: 12, 62: 0, 66: 0, 67: 0}, features=mapper.features)
    host.d.run(host.handle, 0)
    assert host.extra[62].value == 500 and host.extra[67].value == 0
    send_position(host, mapper, bpm=90, kind="Double")
    host.d.run(host.handle, 0)
    assert abs(host.extra[62].value - 60000/90) < .001
    assert host.extra[66].value == 90 and host.extra[67].value == 1
    send_position(host, mapper, bpm=120, beats_per_bar=6, beat_unit=8)
    set_extra(host, 48, 17)
    host.d.run(host.handle, 0)
    assert host.extra[62].value == 3000
    set_extra(host, 48, 19)
    host.d.run(host.handle, 0)
    assert host.extra[62].value == 8000
    set_extra(host, 48, 7)
    host.d.run(host.handle, 0)
    assert abs(host.extra[62].value - 1000/3) < .001
    set_extra(host, 48, 11)
    host.d.run(host.handle, 0)
    assert host.extra[62].value == 750
    for invalid in (float("nan"), float("inf"), 0, -50, 1000):
        send_position(host, mapper, bpm=invalid)
        host.d.run(host.handle, 0)
        assert host.extra[66].value == 120
    host.close()

    # A mid-block position event must not change audio before its timestamp.
    a = Host(pitch=0, extra={47: 1, 48: 12}, features=mapper.features)
    b = Host(pitch=0, extra={47: 1, 48: 12}, features=mapper.features)
    source = [.2 * math.sin(2*math.pi*233.3*i/48000) for i in range(48000)]
    assert a.process(source) == b.process(source)
    send_position(a, mapper, bpm=60, frame=128)
    following = [.2 * math.sin(2*math.pi*233.3*(48000+i)/48000) for i in range(256)]
    first, second = a.process(following), b.process(following)
    assert first[:128] == second[:128]
    assert max(abs(x-y) for x,y in zip(first[160:],second[160:])) > 1e-4
    a.close()
    b.close()

    # Feedback tails and time transitions stay independent of host block partitioning.
    settings = {45: 50, 46: 70}
    a, b = Host(block=1024, extra=settings), Host(block=1024, extra=settings)
    source = [.1 * math.sin(i/17) for i in range(40000)]
    first = a.process(source)
    second = b.process(source, [1, 17, 127, 511, 1024])
    assert first == second
    a.close()
    b.close()
    print("delay: impulses, feedback, tails, fader mute, retiming, tempo/meter and event timestamps passed")

if __name__ == "__main__":
    delay_checks()
