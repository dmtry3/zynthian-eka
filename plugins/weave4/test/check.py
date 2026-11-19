#!/usr/bin/env python3
"""Exercise the actual LV2 entry point; optional WAV rendering uses the same path."""
import ctypes as c
import os
import json
import math
import pathlib
import struct
import sys
import wave

ROOT = pathlib.Path(__file__).resolve().parents[1]
URI = b"http://dmtry3.github.io/zynthian-eka/weave4"
FloatPtr = c.POINTER(c.c_float)
class Descriptor(c.Structure):
    pass
Instantiate = c.CFUNCTYPE(c.c_void_p, c.POINTER(Descriptor), c.c_double, c.c_char_p, c.c_void_p)
Connect = c.CFUNCTYPE(None, c.c_void_p, c.c_uint32, c.c_void_p)
Activate = c.CFUNCTYPE(None, c.c_void_p)
Run = c.CFUNCTYPE(None, c.c_void_p, c.c_uint32)
Cleanup = c.CFUNCTYPE(None, c.c_void_p)
Descriptor._fields_ = [("uri", c.c_char_p), ("instantiate", Instantiate),
    ("connect", Connect), ("activate", Activate), ("run", Run),
    ("deactivate", c.c_void_p), ("cleanup", Cleanup), ("extension", c.c_void_p)]
LIB = c.CDLL(os.environ.get("WEAVE_TEST_BINARY", str(ROOT / "build/eka-weave4.lv2/eka_weave4.so")))
LIB.lv2_descriptor.argtypes = [c.c_uint32]
LIB.lv2_descriptor.restype = c.POINTER(Descriptor)

class Host:
    def __init__(self, rate=48000, block=256, pitch=7, dry=-60, wet=0, extra=None, stereo=False, features=None):
        self.block = block
        self.stereo = stereo
        self.right = []
        self.extra = {}
        self.desc_ptr = LIB.lv2_descriptor(0)
        self.d = self.desc_ptr.contents
        self.features = features
        self.handle = self.d.instantiate(self.desc_ptr, rate, b"", features)
        if not self.handle:
            raise RuntimeError("instantiate failed")
        self.controls = [c.c_float(x) for x in (pitch, dry, wet)]
        self.buffers = [(c.c_float * block)() for _ in range(3)]
        self.latency = c.c_float()
        for port, value in enumerate(self.controls):
            self.d.connect(self.handle, port, c.byref(value))
        for port, value in enumerate(self.buffers, start=3):
            self.d.connect(self.handle, port, value)
        self.d.connect(self.handle, 6, c.byref(self.latency))
        for port, value in (extra or {}).items():
            self.extra[port] = c.c_float(value)
            self.d.connect(self.handle, port, c.byref(self.extra[port]))
        self.d.activate(self.handle)
    def process(self, samples, sizes=None):
        result = []
        pos = 0
        cycle = 0
        while pos < len(samples):
            n = min(self.block if sizes is None else sizes[cycle % len(sizes)],
                    len(samples) - pos)
            for i in range(n):
                self.buffers[0][i] = samples[pos + i]
            self.d.run(self.handle, n)
            for i in range(n):
                left, right = self.buffers[1][i], self.buffers[2][i]
                assert math.isfinite(left) and math.isfinite(right)
                if not self.stereo:
                    assert left == right
                self.right.append(right)
                result.append(left)
            pos += n
            cycle += 1
        return result
    def close(self):
        self.d.cleanup(self.handle)

def set_extra(host, port, value):
    if port not in host.extra:
        host.extra[port] = c.c_float(value)
        host.d.connect(host.handle, port, c.byref(host.extra[port]))
    else:
        host.extra[port].value = value

def save_wav(path, samples, rate=48000):
    with wave.open(str(path), "wb") as out:
        out.setparams((1, 2, rate, 0, "NONE", "not compressed"))
        out.writeframes(b"".join(struct.pack("<h", round(max(-1, min(1, x)) * 32767)) for x in samples))

def checks():
    assert not LIB.lv2_descriptor(1)
    rate = 48000
    signal = [0.3 * math.sin(2 * math.pi * 220 * i / rate) for i in range(rate * 2)]
    results = []
    for pitch in (-12, -5, 0, 7, 12):
        host = Host(pitch=pitch)
        output = host.process(signal, [1, 17, 64, 255, 256])
        steady = output[rate:]
        crossings = sum(a <= 0 < b for a, b in zip(steady, steady[1:]))
        expected = 220 * 2 ** (pitch / 12)
        assert abs(crossings - expected) < 4, (pitch, crossings, expected)
        assert max(map(abs, output)) < 0.6
        results.append({"semitones": pitch, "expected_hz": round(expected, 3),
                        "crossings_hz": crossings})
        host.close()
    host = Host(pitch=0, dry=0, wet=-60)
    impulse = [1.0] + [0.0] * int(rate * .3)
    out = host.process(impulse)
    peak = max(range(len(out)), key=lambda i: abs(out[i]))
    assert peak == int(host.latency.value)  > 0
    host.close()
    host = Host()
    out = host.process([0.0] * 48000)
    assert max(map(abs, out)) == 0
    # Hostile controls must not poison subsequent processing.
    for value in host.controls:
        value.value = float("nan")
    host.process([float("nan")] * 512)
    host.controls[0].value = 12
    host.process(signal[:2000])
    host.d.activate(host.handle)
    assert max(map(abs, host.process([0.0] * 1000))) == 0
    host.close()
    for rate in (44100, 96000):
        host = Host(rate=rate, block=1)
        host.process([0.0] * 500)
        assert 256 < int(host.latency.value) < rate // 2
        host.close()
    # Arbitrary host block partitioning must not change the stream.
    first = Host(pitch=7, block=512)
    second = Host(pitch=7, block=512)
    a = first.process(signal[:12000])
    b = second.process(signal[:12000], [1, 63, 256, 511, 17])
    assert max(abs(x-y) for x,y in zip(a,b)) < 1e-6
    first.close()
    second.close()
    # The wet unison impulse must match the reported algorithm + FIFO latency.
    host = Host(pitch=0, dry=-60, wet=0)
    wet_impulse = host.process([1.0] + [0.0] * 12000)
    wet_peak = max(range(len(wet_impulse)), key=lambda i: abs(wet_impulse[i]))
    assert abs(wet_peak - host.latency.value) <= 2, (wet_peak, host.latency.value)
    host.close()
    print(json.dumps({"checks": "passed", "pitch": results,
                      "dry_latency_samples": peak, "rate": 48000}, indent=2))

def render():
    dest = ROOT / "build/listening"
    dest.mkdir(parents=True, exist_ok=True)
    # Synthetic fixtures deliberately labelled; these do not replace real recordings.
    notes = [110, 146.832, 164.814, 220]
    source = []
    for frequency in notes:
        for i in range(24000):
            t = i / 48000
            env = min(1, t / .005) * math.exp(-5 * t)
            x = sum(math.sin(2 * math.pi * frequency * h * t) / h for h in range(1, 9))
            source.append(.15 * env * x)
    save_wav(dest / "synthetic-pluck-dry.wav", source)
    for pitch in (-12, 7, 12):
        host = Host(pitch=pitch)
        out = host.process(source + [0.0] * 4800)
        save_wav(dest / ("synthetic-pluck-%+d.wav" % pitch), out)
        host.close()
    # Slowly varying vowel-like harmonic signal with vibrato and an amplitude envelope.
    source = []
    phase = 0
    for i in range(96000):
        t = i / 48000
        frequency = 180 * 2 ** ((2 * math.sin(2 * math.pi * .4 * t)
                               + .12 * math.sin(2 * math.pi * 5 * t)) / 12)
        phase += 2 * math.pi * frequency / 48000
        env = min(1, t / .05, (2 - t) / .1)
        source.append(env * .08 * sum(math.sin(h * phase) / h for h in range(1, 16)))
    save_wav(dest / "synthetic-vowel-dry.wav", source)
    host = Host(pitch=7)
    save_wav(dest / "synthetic-vowel-plus7.wav", host.process(source + [0.0] * 4800))
    host.close()
    print(dest)

if __name__ == "__main__":
    if "--render" in sys.argv:
        render()
    else:
        checks()
        from scale import scale_checks
        scale_checks()
        from midi import midi_checks
        midi_checks()
        from four_voices import four_checks
        four_checks()
        from leading import leading_checks
        leading_checks()
        from dry_timing import dry_timing_checks
        dry_timing_checks()
        from voice_timing import voice_timing_checks
        voice_timing_checks()
        from widget_dsp import widget_dsp_checks
        widget_dsp_checks()
        from tone import tone_checks
        tone_checks()
        from character import character_checks
        character_checks()
        from scenes import scene_checks
        scene_checks()
        from delays import delay_checks
        delay_checks()
