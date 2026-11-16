# Changelog

## 0.1 — 2026-11-16

First public version of **Eka Grain Flow**, included as `zynthian-eka/plugins/grainflow`.
LV2 metadata version: 0.1 (`minorVersion 0`, `microVersion 1`).

- Live stereo granular effect with three independent streams, an 8-second source buffer and a shared pool of 48 grains.
- Four encoder pages: Main, Grain, Flow and Out. Per-stream Scan, Fine, Intervals and Spread are on Flow.
- Continuous Freeze retention plus a separate full-capture button; horizontal Amount adjustment on the Freeze button.
- Forward/reverse source scanning, random grain placement and timing, soft-limited feedback and grain tempo sync.
- Continuous grain Shape with its own envelope thumbnail; wide stereo grain placement and channel read offsets.
- Random interval sets plus Q Oct, Q Fifth, Q Major, Q Minor and Q Penta, with pitch held per grain in Q modes.
- Two LFOs with Sine, Tri, Drift and S&H; independent Free/Sync, bipolar/positive polarity, Smooth and cross-rate modulation.
- Input Env with Attack, Release and Sensitivity. A 3 × 6 modulation matrix targets Pitch, Offset, Length, Level, Filter and Scan.
- 25 factory presets, including Init with only the middle stream enabled and stream pitches +12 / 0 / −12.
- English and Russian documentation, screenshots and local Linux/WSL deployment support.
- Offline audio, LV2 State/preset and widget checks; AddressSanitizer and UBSan checks.

The plugin was called Eka Grain early in private development. Current identity stays fixed:
URI `http://dmtry3.github.io/zynthian-eka/grainflow`, bundle `eka-grainflow.lv2`,
binary `eka_grainflow.so`, widget `zynthian_widget_eka_grain_flow.py`.
