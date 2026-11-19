"""Verify immediate dry monitoring and unchanged wet timing through real LV2 ports."""
from check import Host, set_extra

def dry_timing_checks():
    impulse = [1.0] + [0.0] * 12000
    for rate in (44100, 48000, 96000):
        live = Host(rate=rate, pitch=0, dry=0, wet=-60, extra={21: 0, 10: 4, 13: -60, 16: -60, 19: -60})
        output = live.process(impulse, [1, 17, 256, 63])
        assert output[0] == 1
        assert max(map(abs, output[1:])) == 0
        assert live.latency.value == 0
        live.close()
    aligned = Host(pitch=0, dry=0, wet=-60, extra={21: 1})
    output = aligned.process(impulse)
    peak = max(range(len(output)), key=lambda i: abs(output[i]))
    assert peak == aligned.latency.value and peak > 0
    aligned.close()

    # Only monitoring changes. The harmony stream must stay sample-identical.
    for settings in ({}, {7: 1, 8: 5, 10: 4}):
        a = Host(wet=-12, extra=settings | {21: 0})
        b = Host(wet=-12, extra=settings | {21: 1})
        first = a.process(impulse)
        second = b.process(impulse)
        assert first == second
        a.close()
        b.close()

    # A stationary dry signal remains bounded and returns to unity after switching.
    host = Host(pitch=0, dry=0, wet=-60, extra={21: 1})
    host.process([1.] * 12000)
    set_extra(host, 21, 0)
    output = host.process([1.] * 12000)
    assert max(abs(x - 1) for x in output) < 1e-6
    assert host.latency.value == 0
    set_extra(host, 21, 1)
    output = host.process([1.] * 12000)
    assert max(abs(x - 1) for x in output) < 1e-6
    assert host.latency.value > 0
    host.close()

    # Host reactivation honours Live from the first sample.
    host = Host(pitch=0, dry=0, wet=-60, extra={21: 0})
    host.process(impulse)
    host.d.activate(host.handle)
    assert host.process(impulse)[0] == 1
    host.close()
    print("dry timing: Live adds 0 dry samples; Aligned and wet stream checks passed")
