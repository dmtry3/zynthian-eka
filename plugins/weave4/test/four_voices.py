"""Regression checks through LV2: voicings, stereo, alignment and voice lifecycle."""
import math
import json
from check import Host, set_extra

def power(samples, frequency, rate=48000):
    coefficient = 2 * math.cos(2 * math.pi * frequency / rate)
    first = second = 0
    for sample in samples:
        current = sample + coefficient * first - second
        second, first = first, current
    return max(0, first * first + second * second - coefficient * first * second) / len(samples) ** 2

def four_checks():
    source = [.1 * math.sin(2 * math.pi * 220 * i / 48000) for i in range(96000)]
    pitches = [-12, -5, 7, 12]
    # Isolate each of the four real voices, including staggered adapter phases.
    for voice in range(4):
        extra = {10: 4, 13: -60, 16: -60, 19: -60}
        host = Host(wet=0 if voice == 0 else -60, pitch=pitches[0], extra=extra)
        for index, port in enumerate((0, 12, 15, 18)):
            if index:
                set_extra(host, port, pitches[index])
        if voice:
            set_extra(host, (2, 13, 16, 19)[voice], 0)
        output = host.process(source)
        crossings = sum(a <= 0 < b for a, b in zip(output[48000:], output[48001:]))
        expected = 220 * 2 ** (pitches[voice] / 12)
        assert abs(crossings - expected) < 4, (voice, crossings, expected)
        host.close()

    # Every staggered adapter retains the same total unison impulse delay.
    for voice in range(4):
        extra = {10: 4, 12: 0, 15: 0, 18: 0, 13: -60, 16: -60, 19: -60}
        host = Host(pitch=0, wet=0 if voice == 0 else -60, extra=extra)
        if voice:
            set_extra(host, (2, 13, 16, 19)[voice], 0)
        output = host.process([1.0] + [0.0] * 12000)
        peak = max(range(len(output)), key=lambda i: abs(output[i]))
        assert abs(peak - host.latency.value) <= 2, (voice, peak, host.latency.value)
        host.close()

    chords = [
        [4, 7], [3, 7], [4, 7, 10], [4, 7, 11], [3, 7, 10],
        [4, 7, 11, 14], [3, 7, 10, 14], [4, 7, 9, 14],
        [4, 7, 10, 14], [4, 10, 14, 21], [4, 7, 10, 15],
        [2, 7], [5, 7], [3, 6, 9]
    ]
    # Chord tables and all four voicings reach the audio output.
    for chord, intervals in enumerate(chords):
        host = Host(wet=-12, extra={7: 1, 8: chord, 10: 4, 13: -12, 16: -12, 19: -12})
        for voicing in range(4):
            set_extra(host, 9, voicing)
            output = host.process(source[:36000])[24000:]
            expected = list(intervals)
            if voicing == 1:
                expected = [p - 12 if i % 2 == 0 else p for i, p in enumerate(expected)]
            if voicing == 2:
                expected = [p - 12 for p in expected]
            if voicing == 3:
                expected[-2] -= 12
            for pitch in expected:
                frequency = 220 * 2 ** (pitch / 12)
                assert power(output, frequency) > 2e-5, (chord, voicing, pitch)
        host.close()

    # Pan affects only the harmony. Muted unused voices cannot leak dry audio.
    host = Host(pitch=0, extra={11: -1}, stereo=True)
    left = host.process(source)
    assert max(map(abs, host.right)) == 0
    assert max(map(abs, left[24000:])) > .05
    set_extra(host, 11, 1)
    host.right.clear()
    left = host.process(source[:24000])
    assert max(map(abs, left[12000:])) < 1e-6
    assert max(map(abs, host.right[12000:])) > .05
    host.close()

    # Four-voice output stays independent of external block partitioning.
    settings = {7: 1, 8: 5, 9: 1, 10: 4, 13: -12, 16: -12, 19: -12}
    first = Host(block=1024, wet=-12, extra=settings)
    second = Host(block=1024, wet=-12, extra=settings)
    a = first.process(source[:20000])
    b = second.process(source[:20000], [1, 127, 17, 1024, 255])
    assert max(abs(x-y) for x,y in zip(a,b)) < 1e-6
    first.close()
    second.close()

    # Sleep, change pitch while asleep, wake all voices and return to silence.
    host = Host(extra={10: 4, 13: -12, 16: -12, 19: -12})
    host.process(source[:24000])
    set_extra(host, 10, 0)
    output = host.process(source)
    assert max(map(abs, output[48000:])) < 1e-8
    set_extra(host, 7, 1)
    set_extra(host, 8, 9)
    set_extra(host, 10, 4)
    output = host.process(source)
    assert max(map(abs, output[48000:])) > .05
    output = host.process([0.] * 96000)
    assert max(map(abs, output[48000:])) < 1e-8
    host.close()
    print(json.dumps({"four_voices": "passed", "chords": 14, "voicings": 4,
                      "checks": ["voice isolation", "staggered latency", "chord audio",
                                 "pan", "partition invariance", "sleep/wake"]}))

if __name__ == "__main__":
    four_checks()
