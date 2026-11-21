#!/usr/bin/env python3
"""Generate a clean, punchy 16-sample factory drum kit (48 kHz, 16-bit mono WAV) for Eka Pads."""
import math
import os
import random
import struct
import wave

SR = 48000


def write_wav(path, samples):
    peak = max((abs(x) for x in samples), default=1.0)
    scale = 0.92 / peak if peak > 1e-6 else 1.0
    data = bytearray()
    for x in samples:
        v = max(-1.0, min(1.0, x * scale))
        data.extend(struct.pack("<h", int(round(v * 32767.0))))
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(bytes(data))


def synth_kick(dur=0.45, f0=170.0, f1=48.0, pdec=0.045, adec=0.18, click=0.35):
    n = int(SR * dur)
    out = []
    ph = 0.0
    rng = random.Random(101)
    for i in range(n):
        t = i / SR
        f = f1 + (f0 - f1) * math.exp(-t / pdec)
        ph += 2.0 * math.pi * f / SR
        body = math.sin(ph) * math.exp(-t / adec)
        tr = (rng.uniform(-1.0, 1.0) * click) * math.exp(-t / 0.004)
        out.append(math.tanh((body + tr) * 1.4))
    return out


def synth_snare(dur=0.28, tone=195.0, ndec=0.11):
    n = int(SR * dur)
    out = []
    ph = 0.0
    rng = random.Random(202)
    lp = 0.0
    for i in range(n):
        t = i / SR
        f = tone * (1.0 + 0.5 * math.exp(-t / 0.015))
        ph += 2.0 * math.pi * f / SR
        body = math.sin(ph) * math.exp(-t / 0.055) * 0.65
        nz = rng.uniform(-1.0, 1.0)
        lp += 0.35 * (nz - lp)
        hp = nz - lp
        noise = hp * math.exp(-t / ndec) * 0.85
        out.append(math.tanh((body + noise) * 1.3))
    return out


def synth_clap(dur=0.25):
    n = int(SR * dur)
    out = []
    rng = random.Random(303)
    lp = 0.0
    hp_prev = 0.0
    bursts = (0.0, 0.011, 0.022, 0.034)
    for i in range(n):
        t = i / SR
        nz = rng.uniform(-1.0, 1.0)
        lp += 0.28 * (nz - lp)
        bp = lp - hp_prev
        hp_prev += 0.08 * (lp - hp_prev)
        env = 0.0
        for b in bursts:
            if t >= b:
                dt = t - b
                env += math.exp(-dt / (0.012 if b < 0.03 else 0.075))
        out.append(math.tanh(bp * env * 2.2))
    return out


def synth_rim(dur=0.08):
    n = int(SR * dur)
    out = []
    for i in range(n):
        t = i / SR
        s = (math.sin(2.0 * math.pi * 490.0 * t) * 0.7 +
             math.sin(2.0 * math.pi * 1680.0 * t) * 0.5) * math.exp(-t / 0.014)
        out.append(math.tanh(s * 1.8))
    return out


def synth_hat(dur=0.09, dec=0.028, seed=404):
    n = int(SR * dur)
    out = []
    rng = random.Random(seed)
    prev = 0.0
    freqs = (205.3, 304.4, 369.6, 522.7, 540.0, 800.0)
    for i in range(n):
        t = i / SR
        met = sum(1.0 if math.sin(2.0 * math.pi * f * t) >= 0 else -1.0 for f in freqs) / len(freqs)
        nz = rng.uniform(-1.0, 1.0)
        x = 0.45 * met + 0.55 * nz
        hp = x - prev
        prev = 0.78 * prev + 0.22 * x
        out.append(hp * math.exp(-t / dec))
    return out


def synth_tom(dur=0.35, f0=180.0, f1=90.0, adec=0.14):
    n = int(SR * dur)
    out = []
    ph = 0.0
    for i in range(n):
        t = i / SR
        f = f1 + (f0 - f1) * math.exp(-t / 0.04)
        ph += 2.0 * math.pi * f / SR
        out.append(math.sin(ph) * math.exp(-t / adec))
    return out


def synth_shaker(dur=0.14):
    n = int(SR * dur)
    out = []
    rng = random.Random(505)
    prev = 0.0
    for i in range(n):
        t = i / SR
        nz = rng.uniform(-1.0, 1.0)
        hp = nz - prev
        prev = 0.6 * prev + 0.4 * nz
        env = min(t / 0.015, 1.0) * math.exp(-max(0.0, t - 0.015) / 0.032)
        out.append(hp * env)
    return out


def synth_cowbell(dur=0.22):
    n = int(SR * dur)
    out = []
    for i in range(n):
        t = i / SR
        a = 1.0 if math.sin(2.0 * math.pi * 545.0 * t) >= 0 else -1.0
        b = 1.0 if math.sin(2.0 * math.pi * 815.0 * t) >= 0 else -1.0
        out.append((a + b) * 0.5 * math.exp(-t / 0.065))
    return out


def synth_zap(dur=0.18):
    n = int(SR * dur)
    out = []
    ph = 0.0
    for i in range(n):
        t = i / SR
        f = 90.0 + 1800.0 * math.exp(-t / 0.025)
        ph += 2.0 * math.pi * f / SR
        out.append(math.sin(ph) * math.exp(-t / 0.06))
    return out


def synth_stab(dur=0.35):
    n = int(SR * dur)
    out = []
    freqs = (261.63, 311.13, 392.00, 466.16)  # Cm7
    for i in range(n):
        t = i / SR
        s = sum(math.sin(2.0 * math.pi * f * t) + 0.35 * math.sin(4.0 * math.pi * f * t) for f in freqs)
        env = min(t / 0.004, 1.0) * math.exp(-t / 0.11)
        out.append(math.tanh(s * 0.35) * env)
    return out


def main():
    out_dir = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "samples")
    os.makedirs(out_dir, exist_ok=True)
    kit = [
        ("01_kick.wav",       synth_kick(0.45, 180.0, 50.0, 0.04, 0.17, 0.35)),
        ("02_sub_kick.wav",   synth_kick(0.60, 120.0, 40.0, 0.06, 0.30, 0.15)),
        ("03_snare.wav",      synth_snare(0.28, 195.0, 0.10)),
        ("04_clap.wav",       synth_clap(0.26)),
        ("05_rim.wav",        synth_rim(0.08)),
        ("06_chh.wav",        synth_hat(0.08, 0.022, 404)),
        ("07_ohh.wav",        synth_hat(0.42, 0.16, 405)),
        ("08_pedal_hat.wav",  synth_hat(0.12, 0.045, 406)),
        ("09_tom_lo.wav",     synth_tom(0.38, 140.0, 72.0, 0.15)),
        ("10_tom_mid.wav",    synth_tom(0.34, 200.0, 110.0, 0.13)),
        ("11_tom_hi.wav",     synth_tom(0.30, 290.0, 165.0, 0.11)),
        ("12_crash.wav",      synth_hat(0.90, 0.38, 407)),
        ("13_shaker.wav",     synth_shaker(0.14)),
        ("14_cowbell.wav",    synth_cowbell(0.22)),
        ("15_zap.wav",        synth_zap(0.18)),
        ("16_chord_stab.wav", synth_stab(0.35)),
    ]
    for name, samples in kit:
        write_wav(os.path.join(out_dir, name), samples)
    print(f"Generated {len(kit)} samples in {out_dir}")


if __name__ == "__main__":
    main()
