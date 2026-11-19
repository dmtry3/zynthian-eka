"""Verify per-voice unaligned (Fast) vs aligned timing through real LV2 ports."""
from check import Host, set_extra

def voice_timing_checks():
    impulse = [1.0] + [0.0] * 12000

    # Fast mode (voice_timing = 0, port 342 = 0)
    # Voice 1 pitch=-12 (slow, ~109 ms), Voice 2 pitch=7 (fast, ~66 ms)
    host_fast = Host(pitch=-12, dry=-60, wet=-60, extra={10: 4, 12: 7, 13: 0, 15: 0, 16: -60, 18: 0, 19: -60, 342: 0})
    out_fast = host_fast.process(impulse)
    peak_fast = max(range(len(out_fast)), key=lambda i: abs(out_fast[i]))
    host_fast.close()

    # Aligned mode (voice_timing = 1, port 342 = 1)
    host_aligned = Host(pitch=-12, dry=-60, wet=-60, extra={10: 4, 12: 7, 13: 0, 15: 0, 16: -60, 18: 0, 19: -60, 342: 1})
    out_aligned = host_aligned.process(impulse)
    peak_aligned = max(range(len(out_aligned)), key=lambda i: abs(out_aligned[i]))
    host_aligned.close()

    assert peak_fast < peak_aligned
    # Fast mode removes the 2048-sample (~42.7 ms) alignment padding of the +7 shift.
    assert abs((peak_aligned - peak_fast) - 2048) < 100

    # Test live switching between Fast and Aligned on continuous stream
    host = Host(pitch=-12, dry=-60, wet=-60, extra={10: 4, 12: 7, 13: 0, 15: 0, 16: -60, 18: 0, 19: -60, 342: 0})
    host.process([1.0] * 6000)
    set_extra(host, 342, 1)
    out = host.process([1.0] * 6000)
    assert all(abs(x) < 2.0 for x in out)
    set_extra(host, 342, 0)
    out2 = host.process([1.0] * 6000)
    assert all(abs(x) < 2.0 for x in out2)
    host.close()

    print("voice timing: Fast mode saves ~42.7 ms padding; dynamic switching passed")
