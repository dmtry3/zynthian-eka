# Changelog

## 0.1.3 (2026-11-22)

- Redesigned the dedicated **FX** touch screen with a large `Time FX` / `Filter FX` X-Y pad, horizontal `Wash FX` slider with `0` reset, and `HOLD / SNAP 0` spring-return mode.
- Added a MIDI-mappable **FX Zero** LV2 control port with DSP latch and UI auto-reset.
- Displayed musical note divisions everywhere in the widget when `Sync` is enabled.
- Tuned softer `Wash FX`, smoother `Freeze`, PingPong first-bounce timing, and widget FX/Link sync fixes.

## 0.1.2 (2026-11-22)

- Made **Time FX** bipolar (`-100..+100%`, default `0%` with zero deadzone) so original `Time L`/`Time R` are never lost and returning to `0%` restores the exact base times (`-50%` = `0.5x`, `+50%` = `2.0x`).
- Moved **Effects** to Page 2 (right after `Time`) and added a dedicated **LIVE EFFECTS** touch screen in the widget (`FX` button) with bipolar `TIME FX`, `FILTER FX`, `WASH FX` sliders, per-slider `0` reset, `LINK / L ONLY`, and `ZERO 0` buttons.

## 0.1.1 (2026-11-22)

- Added dedicated live **Effects** page (`Time FX`, `Time Link`, `Filter FX`, `Wash FX`):
  - `Time FX` (`25..400%`, logarithmic center at `100%`) smoothly modulates both `Time L` & `Time R` proportionally when `Time Link` is `On`, or `Time L` alone when `Off`.
  - `Filter FX` (`-100..+100%`) provides a live bipolar DJ-style LPF/HPF loop sweep; `Wash FX` (`0..100%`) adds a dub-throw feedback/diffusion/warmth boost.
  - Added `LINK` toggle button to the touch widget (switches encoders to `Effects` and links/unlinks L/R Tap Map dragging).
- Musical **Tape** time physics: critically-damped capstan motor inertia with bounded playback speed (`0.42x..2.0x`) for smooth, natural pitch glides without backward-playback glitches or aliasing.

## 0.1.0 (2026-11-21)

- Initial release of **Eka Echo** (`http://dmtry3.github.io/zynthian-eka/echo`):
  - Independent stereo L/R Hermite-interpolated delay (`5..2500 ms`) with LV2 `time:Position` BPM sync (`1/64 .. 1 bar`).
  - Four characters (`Clean`, `Tape`, `BBD`, `Diff`) and three stereo routings (`Stereo`, `PingPong`, `Cross`).
  - Feedback loop `HPF`/`LPF`, `Drive`, 4-stage stereo all-pass `Diffuse`, LFO modulation, input-triggered `Duck`, `Freeze`, `Trails` (`Mute In`), and click-free `Bypass`.
  - Interactive Zynthian touch widget (`zynthian_widget_eka_echo.py`) with encoder page sync and grayscale Bypass via long-press on `TRAILS`.
  - 10 factory presets and full DSP/LV2/widget test suite.
