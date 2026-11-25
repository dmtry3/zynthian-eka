# Changelog

## 0.13.0 (2026-11-13)

Initial release of **Eka Mist** (`http://dmtry3.github.io/zynthian-eka/mist`):

- 8-voice polyphonic granular sampler (Poly or Mono with Glide) backed by a shared 128-grain pool for bounded CPU usage.
- Sample playback controls: `Position`, `Scan` (forward/reverse/freeze), `Spray`, `Window`, waveform start/end markers, root note / Auto, and sample gain with peak normalization.
- Grain shaping (`Size`, `Density`, `Shape`, `Chaos`) and pitch controls (`Pitch`, `Pitch Spray`, `Reverse`, scale `Quantize` across 12 scales, ±24 st pitch bend).
- Per-voice filter (`LP`, `HP`, `BP` 12/24 dB/oct, `Notch`, `Comb`, `Vowel` with Lows/Highs, `Mix`, `Key Track`).
- Per-voice modulation: 2 tempo-syncable LFOs, 2 envelopes, 8 × 9 `Mod` grid and 8 × 6 `X-Mod` cross-modulation grid.
- Background sample loading (`libsndfile` via LV2 Worker), 8 factory samples ([samples/CREDITS.md](samples/CREDITS.md)), and 18 factory presets.
- 6-tab Zynthian touch widget (`Wave`, `Grain`, `Pitch`, `Filter`, `Mod`, `X-Mod`) synchronized with the 6 encoder pages.
