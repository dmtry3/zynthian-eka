# Changelog — Eka Press

## 0.1.2 — 2026-11-26

- Fixed UI lag when turning knobs: replaced full `canvas.delete("all")` redraws in `zynthian_widget_eka_press.py` with persistent retained canvas items, fine-grained dirty-checking, knob-active monitor IPC backoff, and throttled touch drag.
- Minor DSP speedups in `src/press_core.c` (`expf` instead of `powf` for dB-to-linear conversion, removed duplicate `tanhf` in drive).

## 0.1.1 — 2026-11-21

- Long-press (0.5 s) on the mode button toggles clickless true **Bypass** (`bypass` LV2 port); the entire widget turns greyscale while bypassed.

## 0.1.0 — 2026-11-21

- Initial release of **Eka Press** (`eka-press.lv2`, `eka_press.so`, `zynthian_widget_eka_press.py`).
- Four compressor characters (`Clean`, `Opto`, `Punch`, `Glue`), quadratic soft knee, `Auto Release`, `SC HPF` (`20..500 Hz`), `Stereo Link` (`0..100%`), `SC Listen`, analog `Drive`, `Auto Makeup`, parallel `Mix`, and `-0.3 dBFS` `Soft Clip`.
- Touch widget with interactive Transfer Curve (`Threshold` / `Ratio`), scrolling Input/Output/GR timeline, vertical `IN / GR / OUT` peak meters, compact status strip, and 5-button touch bar.
- 10 factory presets (`lv2/presets.ttl`), offline DSP/LV2/widget tests, and `README.md` / `README.ru.md`.
