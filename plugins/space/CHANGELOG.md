# Changelog

## 0.1.0 (2026-11-26)

- Initial release of **Eka Space** (`http://dmtry3.github.io/zynthian-eka/space`):
  - 2D Soundstage positioning: binaural constant-power `Pan` (`-100..+100%`) with far-ear head-shadow damping, Mid/Side `Width` (`0..300%`), front-to-back acoustic `Depth` (`0..100%`), and opposite L/R spectral `Tilt` (`±6 dB` around `900 Hz`).
  - Five stereo expansion engines (`Mode`): `M/S`, mono-safe complementary `Comb`, psychoacoustic `Haas` (`0.5..40 ms` `Spread`), quadrature BBD `Dimension`, and 4-stage all-pass `Vortex`, plus 4-tap decorrelated early reflections (`Room`).
  - Tempo-synced spatial modulation (`Mod Shape`: `Pan`, `Breathe`, `Orbit`, `Figure-8`, `Drift`) with LV2 `time:Position` BPM sync (`4 bars .. 1/64`).
  - Frequency & dynamic stage tools: `Bass Mono` low-end anchor (`20..500 Hz`), `Side HPF` (`20..1000 Hz`), `Side Air` shelf (`-12..+12 dB`), and bipolar input-envelope `Env Width` (`-100..+100%`).
  - Dedicated live **FX** X-Y performance screen (`Pan FX`, `Width FX`, `Spin FX`, `HOLD / SNAP 0` spring return, MIDI-mappable `FX Zero`), `Audition` monitor (`Stereo / Mono / Mid / Side`), automatic `SafePhase` anti-cancellation guard, and click-free `Bypass`.
  - Interactive Zynthian touch widget (`zynthian_widget_eka_space.py`), 10 factory presets, and full DSP/LV2/widget test suite.
