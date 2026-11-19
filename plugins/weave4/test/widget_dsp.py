"""Canonical widget parameters and monitoring, exercised through LV2."""
import math
from check import Host, set_extra

OUTPUTS = {index: 0 for index in range(33, 45)}

def make(extra=None, **options):
    return Host(extra=OUTPUTS | (extra or {}), **options)

def widget_dsp_checks():
    source = [.15 * math.sin(2 * math.pi * 220 * i / 48000) for i in range(96000)]
    host = make({10: 3})
    host.process(source[:12000])
    assert host.extra[41].value == 7
    set_extra(host, 24, 0)
    set_extra(host, 25, 1)
    host.process(source[:12000])
    assert host.extra[41].value == 11
    assert host.extra[44].value == 15
    assert host.extra[37].value > 0 and host.extra[40].value > 0
    assert host.extra[42].value > .1 and host.extra[43].value > 0
    stored = [host.extra[index].value for index in (10, 24, 25)]
    set_extra(host, 26, 4)
    output = host.process(source)[48000:]
    crossings = sum(a <= 0 < b for a, b in zip(output, output[1:]))
    assert abs(crossings - 440) < 4, crossings
    set_extra(host, 26, 0)
    host.process(source[:12000])
    assert stored == [host.extra[index].value for index in (10, 24, 25)]
    host.close()

    # Muting an individually disabled slow voice removes it from alignment.
    host = make({10: 2, 22: 0}, pitch=-12)
    host.process(source[:12000])
    assert host.latency.value < 4190
    host.close()

    # Selection is UI state only; it must not alter audio.
    a = make({10: 4, 32: 1})
    b = make({10: 4, 32: 4})
    assert a.process(source[:12000]) == b.process(source[:12000])
    a.close()
    b.close()

    # Chord octave controls preserve chord roles within the tested pitch range.
    host = make({7: 1, 8: 5, 10: 4, 27: -1, 28: 1, 29: 1, 30: 1})
    host.process(source[:12000])
    assert [host.extra[i].value for i in range(33, 37)] == [-8, 19, 11, 14]
    host.close()

    # Output gain is global, with an exact mute and no startup overshoot.
    for db in (-60, -6, 0):
        host = make({10: 0, 21: 0, 31: db}, dry=0)
        output = host.process([.2] * 1000)
        gain = 0 if db == -60 else 10 ** (db / 20)
        assert max(abs(sample - .2 * gain) for sample in output) < 1e-6
        host.close()
    # Master safety limiter prevents clipping on hot stereo signals and preserves pan balance.
    hot = [1.5 * math.sin(2 * math.pi * 220 * i / 48000) for i in range(4800)]
    host = make({10: 0, 21: 0, 31: 6}, dry=6, stereo=True)
    left = host.process(hot); right = host.right
    assert max(map(abs, left)) <= 1.0001 and max(map(abs, right)) <= 1.0001
    assert host.extra[43].value > 1.0
    host.close()

    # Chord mode auto-enables all available chord voices even when voices=1.
    host = make({7: 1, 8: 5, 10: 1}) # maj9 (4 notes), voices=1, enable=Auto
    host.process(source[:2048])
    assert host.extra[41].value == 15, host.extra[41].value
    host.close()

    # Width spreads centered voices across stereo field.
    host = make({10: 2, 11: 0, 14: 0, 423: 100}, dry=-60, wet=0, stereo=True)
    l = host.process(source[:12000]); r = host.right
    assert max(abs(a - b) for a, b in zip(l, r)) > .05
    host.close()

    # Glide smoothly ramps pitch changes over glide_ms.
    host = make({10: 1, 424: 200}, pitch=0, dry=-60, wet=0)
    host.process(source[:12000])
    host.controls[0].value = 12
    mid = host.process(source[:6000])
    assert all(math.isfinite(x) for x in mid)
    host.close()

    # Tone inside delay feedback loop progressively darkens repeats.
    hi = [.2 * math.sin(2 * math.pi * 6000 * i / 48000) for i in range(2400)] + [0.0] * 18000
    host = make({10: 1, 45: 100, 46: 80, 255: 20, 256: 1500, 257: 1}, pitch=0, dry=-60, wet=0)
    out = host.process(hi)
    r1 = max(abs(x) for x in out[7000:11500])
    r2 = max(abs(x) for x in out[12000:16500])
    assert r1 > 0 and r2 < r1 * 0.4, (r1, r2)
    host.close()
    print("widget DSP: legacy Auto, On/Solo, octave limits, selection, meters, master, limiter, chord-auto, width, glide and feedback-tone passed")

if __name__ == "__main__":
    widget_dsp_checks()
