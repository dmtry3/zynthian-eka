# Changelog

## 0.1.0 (2026-11-21)

- Initial release of **Eka Q-4** (`http://dmtry3.github.io/zynthian-eka/q4`):
  - 4-band 64-bit parametric EQ (`Shelf`, `Bell`, `Pultec`, `Notch`, `Air`, `Off`).
  - Butterworth `HPF` / `LPF` (`12 / 24 dB/oct`) and `1 kHz` `Tilt`.
  - Global `Scale`, per-band `Solo` audition, perceptual `AutoGain`, click-free `Bypass`, and `Output` trim.
  - Zero-latency 24-band log-spaced spectrum analyzer.
  - Interactive Zynthian touch widget (`zynthian_widget_eka_q4.py` / `zynthian_widget_eka_q-4.py`) with encoder page sync and grayscale Bypass via long-press on `AUTO`.
  - Registered under Zynthian's `Filter & EQ` category (`lv2:FilterPlugin`), 10 factory presets, and full DSP/LV2/widget test suite.
