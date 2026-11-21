# Changelog

## 0.1.0 (2026-11-21)

- Initial release of **Eka Echo** (`http://dmtry3.github.io/zynthian-eka/echo`):
  - Independent stereo L/R Hermite-interpolated delay (`5..2500 ms`) with LV2 `time:Position` BPM sync (`1/64 .. 1 bar`).
  - Four characters (`Clean`, `Tape`, `BBD`, `Diff`) and three stereo routings (`Stereo`, `PingPong`, `Cross`).
  - Feedback loop `HPF`/`LPF`, `Drive`, 4-stage stereo all-pass `Diffuse`, LFO modulation, input-triggered `Duck`, `Freeze`, `Trails` (`Mute In`), and click-free `Bypass`.
  - Interactive Zynthian touch widget (`zynthian_widget_eka_echo.py`) with encoder page sync and grayscale Bypass via long-press on `TRAILS`.
  - 10 factory presets and full DSP/LV2/widget test suite.
