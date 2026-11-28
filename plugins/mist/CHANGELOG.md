# Changelog

## 0.14.0 (2026-11-28)

- **Audio & engine:** enabled Hermite interpolation by default, added smooth-knee output soft-clipper (`> 0.9`) to prevent harsh DAC clipping on resonant chords, fixed `X-Mod` LFO/Env stepping when the main `Mod` grid is empty, and fixed long reverse grains to start from `mark_end` instead of reading cut-away audio before `mark_start`.
- **Pitch & expression:** added `Octaves` and `Fifths` scales for shimmer-style quantization, marker-aware pitch detection (`Auto` root) and marker-aware peak normalization, and MIDI Channel Pressure (Aftertouch) routed into `Wheel`.
- **Touch widget:** added grain length tails (`Size`) on the `Wave` strip, live modulated cutoff marker (`filter_fc`) on `Filter`, live newest grain pitch ring (`last_pitch`) on `Pitch`, LFO polarity zero-line on `Mod`, and two-axis drag (horizontal or vertical) for `Env 1`, `Env 2`, and `Depth`.

## 0.13.0 (2026-11-13)

Initial release of **Eka Mist** (`http://dmtry3.github.io/zynthian-eka/mist`):

- 8-voice polyphonic granular sampler (Poly or Mono with Glide) backed by a shared 128-grain pool for bounded CPU usage.
- Sample playback controls: `Position`, `Scan` (forward/reverse/freeze), `Spray`, `Window`, waveform start/end markers, root note / Auto, and sample gain with peak normalization.
- Grain shaping (`Size`, `Density`, `Shape`, `Chaos`) and pitch controls (`Pitch`, `Pitch Spray`, `Reverse`, scale `Quantize` across 12 scales, ±24 st pitch bend).
- Per-voice filter (`LP`, `HP`, `BP` 12/24 dB/oct, `Notch`, `Comb`, `Vowel` with Lows/Highs, `Mix`, `Key Track`).
- Per-voice modulation: 2 tempo-syncable LFOs, 2 envelopes, 8 × 9 `Mod` grid and 8 × 6 `X-Mod` cross-modulation grid.
- Background sample loading (`libsndfile` via LV2 Worker), 8 factory samples ([samples/CREDITS.md](samples/CREDITS.md)), and 18 factory presets.
- 6-tab Zynthian touch widget (`Wave`, `Grain`, `Pitch`, `Filter`, `Mod`, `X-Mod`) synchronized with the 6 encoder pages.
