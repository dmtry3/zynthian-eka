# Eka Pulse

**English** · [Русский](README.ru.md)

Rhythmic multi-effect shaper LV2 plugin with a touch widget for [Zynthian](https://zynthian.org) (Raspberry Pi 5, V5 kit, 800×480 touchscreen), part of the `zynthian-eka` bundle.

## Features

- **4 independent polyrhythmic shaper slots** in series, each with its own `1..16` step loop length, step shapes (`_`, `\`, `/`, `/\`, `2x`, `3x`), `Warp`, `Smooth / Punch`, and 24 built-in musical patterns:
  1. **1: TIME** — stereo ring buffer with 4-point Hermite interpolation and de-click crossfade: `Curve` (TimeShaper), `Chop` (Step-Chopper: `THRU`, `1/8`, `1/16`, `1/2x`, `REV`, `STOP`), `Stutter`, `TapeStop` (tunable brake time via `Range`).
  2. **2: DRIVE & CRUSH** — `Tube`, `Fold` (ADAA wavefolder), `Hard`, `Crush` (sample-rate + bit crush), `Combo`.
  3. **3: FILTER** — TPT SVF (`LP 24`, `LP 12`, `BP`, `HP`, `Morph`, `Notch`) and stereo 4-stage allpass `Phaser`.
  4. **4: GATE & SPACE** — `Gate` (with `Floor` and `Punch`), `Pan`, `Width` (M/S), `Gate+Pan`.
- **Low Split Crossover (Linkwitz-Riley 24 dB/oct, `Off / 60..500 Hz`)** — keep sub-bass clean while shaping highs, or enable `Low Duck` to apply the Gate envelope to the low band.
- **Triggers & Envelope Modulation** — `Sync` (locked to Zynthian transport: stops when halted, starts on beat 1), `Free`, `Audio` (restarts on input transient), `MIDI` (restarts on Note On), plus `Env → Depth` (`-100..+100%`).
- **Live Performance (`Perform` tab)** — 3×3 momentary performance pads (`TapeStop`, `1/16 Rpt`, `1/8 Rpt`, `Reverse`, `Half 1/2`, `GateChop`, `FiltDrop`, `BitCrush`, `Bypass`), interactive **XY Pad** (`X: Cutoff`, `Y: Depth`), and **4 Scenes (`A–D`)** with `Store`, pedal switching (`Prev Scene` / `Next Scene`), and full LV2 State persistence.
- **24 Factory Presets** + built-in **Help** tab and dynamic per-slot encoder labels.
- **Zero-latency stereo-linked brickwall limiter** (0 dBFS ceiling, `LIM` indicator), 0 RT allocations, flush-to-zero enabled.

## Encoder Pages (5 × 4)

| Page | Knob 1 | Knob 2 | Knob 3 | Knob 4 |
|---|---|---|---|---|
| **1. Main** | Slot (`1:Time..4:Gate`) | Mix % | Low Split (`Off, 60..500 Hz`) | Low Duck (`Off / On`) |
| **2. FX** *(slot)* | Mode *(dynamic name)* | Param 1 *(Range / Drive / Cutoff / Floor)* | Param 2 *(Window / Tone-Crush / Res / Width)* | Depth (`-100..+100%`) |
| **3. Curve** *(slot)* | Pattern (`Custom` + 24) | Step Rate (`1/32..1 bar`, `Free`) | Steps (`1..16`) | Warp (`-100..+100%`) |
| **4. Trigger** *(slot)* | Trig (`Sync/Free/Audio/MIDI`) | Threshold dB | Env→Depth (`-100..+100%`) | Smooth / Punch |
| **5. Perform** | Live Pad (`Off` + 8 FX) | Prev Scene *(pedal)* | Next Scene *(pedal)* | Scene (`A–D`) |

## Build & Install

On Zynthian (SSH as root):

```bash
cd /root/zynthian-eka
sudo make install-pulse
systemctl restart zynthian
```

In WSL / Linux:

```bash
make -C plugins/pulse
make -C plugins/pulse test
SAN=1 make -C plugins/pulse test
```

## License

GPL-3.0-or-later. See [LICENSE](LICENSE).
