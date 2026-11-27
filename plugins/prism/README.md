# Eka Prism for Zynthian

**English** · [Русский](README.ru.md)

**Dual-Chain Morphing & Modulated Multi-Model Filter** for [Zynthian](https://zynthian.org): stereo **LV2** audio effect with host BPM sync (`time:Position`), MIDI note tracking (`midi:MidiEvent`), and an **interactive touch widget** featuring a live dual frequency-response graph (showing **Filter A**, **Filter B**, and their combined Serial / Parallel / Stereo curve dancing in real time with LFO and Envelope modulation), draggable `Cutoff ×Resonance` nodes, `Morph A / Morph B` bars, `IN / ENV / OUT` meters, and a dedicated **XY / FX** performance pad screen.

> Tested on: Zynthian **Vangelis** (stable 2610), Raspberry Pi 5, V5 kit, 800×480 touch screen.
> Other versions/hardware are untested.

## Features

- **Two independent stereo filter chains (`Filter A` & `Filter B`), each with 6 filter models (`Model A` / `Model B`):**
  1. `SVF Morph` — zero-delay-feedback Cytomic state-variable filter with continuous 4-zone morphing (`LP ↔ BP ↔ HP ↔ Notch`) and internal saturation at high resonance.
  2. `Ladder 24` — 4-pole (`24 dB/oct`) transistor ladder with low-end bass compensation and continuous pole morphing (`24 dB LP ↔ 12 dB LP ↔ 4-pole BP ↔ HP`).
  3. `MS-20 Scream` — aggressive Sallen-Key filter with asymmetric diode feedback clipping and continuous `LP ↔ BP ↔ HP` input topology morphing.
  4. `Comb Res` — tuned Karplus-Strong / Hermite fractional-delay comb resonator (`Morph` sweeps negative hollow clarinet comb ↔ positive string/flange comb with variable damping).
  5. `Formant` — 3-band parallel vocal tract resonator morphing smoothly across 5 vowels (`A ↔ E ↔ I ↔ O ↔ U`) while `Cutoff` shifts vocal tract pitch.
  6. `Phaser 6` — 6-stage bilinear all-pass cascade where `Morph` adjusts pole spread and negative/positive feedback polarity.
- **3 Routing topologies & Cutoff Link modes (`Routing` page):**
  - `Serial A>B` — Filter A feeds into Filter B (crossfaded by `Balance`).
  - `Parallel A+B` — Filter A and Filter B summed in parallel, with **`Phase B` (`Norm / Invert`)** for deep phase-cancellation notches and spectral valleys.
  - `Stereo L/R` — Left channel through Filter A, Right channel through Filter B for ultra-wide analog stereo motion.
  - `Link B` (`Free / Offset / Mirror`): `Cutoff B` either moves independently (`Free`), tracks `Cutoff A` at a constant interval (`Offset`), or moves in the opposite direction around `1 kHz` (`Mirror`).
- **Deep Modulation Core (`LFO` & `Mod Depth` pages):**
  - **7-shape LFO:** `Sine`, `Tri`, `Saw`, `Square`, `S&H` (slewed random), `Chaos` (coupled nonlinear Buchla/Lorenz drift), and `Step 8` (8-step rhythmic groove gate).
  - Free `Rate` (`0.05..40 Hz`, reaching **audio-rate FM** growl) or Zynthian tempo `Sync` (`4 bars .. 1/64`).
  - **`B Phase` (`0..180°`):** phase offset between LFO A and LFO B so a single LFO drives both chains in quadrature (`90°`) or counter-motion (`180°`).
  - Bipolar `LFO > Cut`, `Mod > Morph` (modulates filter response/vowel/comb polarity), and `Env > Cut` (input **Envelope Follower** with `Env Speed` from slow groove to snappy transient auto-wah).
  - **MIDI Note Tracking:** receiving MIDI notes shifts both filter cutoffs relative to `C4` (`60`) for melodic self-oscillation and tuned comb plucking.
- **Analog Pre-Drive & Master (`Master` page):**
  - Compensated asymmetric pre-filter `Drive` (`0..100%`), dry/wet `Mix`, `Output` (`-18..+12 dB`), and click-free `Bypass`.
- **12 factory presets** (bank *Factory*).

## Installation

On Zynthian (SSH as root):

```bash
git clone https://github.com/dmtry3/zynthian-eka.git
cd zynthian-eka/plugins/prism
sudo make install
systemctl restart zynthian
```

One-line update:

```bash
cd /root/zynthian-eka && git pull && sudo make install-prism && systemctl restart zynthian
```

## Controls

Six pages of 4 encoders:

| Page | Knob 1 | Knob 2 | Knob 3 | Knob 4 |
|---|---|---|---|---|
| **1. Filter A** | `Model A` (`SVF / Ladder / MS-20 / Comb / Formant / Phaser`) | `Cutoff A` (`20..18000 Hz`) | `Res A` (`0..100%`) | `Morph A` (`0..100%`) |
| **2. Filter B** | `Model B` (`SVF / Ladder / MS-20 / Comb / Formant / Phaser`) | `Cutoff B` (`20..18000 Hz`) | `Res B` (`0..100%`) | `Morph B` (`0..100%`) |
| **3. Routing** | `Routing` (`Serial A>B / Parallel A+B / Stereo L/R`) | `Link B` (`Free / Offset / Mirror`) | `Balance` (`-100..+100%`) | `Phase B` (`Norm / Invert`) |
| **4. LFO** | `Shape` (`Sine / Tri / Saw / Square / S&H / Chaos / Step 8`) | `Rate` (`0.05..40 Hz` / Sync) | `Sync` (`Free / Sync`) | `B Phase` (`0..180°`) |
| **5. Mod Depth** | `LFO > Cut` (`-100..+100%`) | `Mod > Morph` (`-100..+100%`) | `Env > Cut` (`-100..+100%`) | `Env Speed` (`0..100%`) |
| **6. Master** | `Drive` (`0..100%`) | `Mix` (`0..100%`) | `Output` (`-18..+12 dB`) | `Bypass` (`Off / On`) |

## Build & Tests on Linux / WSL

```bash
make -C plugins/prism                 # build LV2 bundle
make -C plugins/prism test            # DSP, LV2 host, and widget tests
SAN=1 make -C plugins/prism test      # with AddressSanitizer + UBSan
```

## License

GPL-3.0-or-later.
