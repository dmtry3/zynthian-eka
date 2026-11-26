# Changelog

## 1.0.0 — 2026-11-26

- Initial release of **Eka Modls** (`http://dmtry3.github.io/zynthian-eka/modls`), a 6-engine musical-to-extreme "Uber Mod" modulation LV2 effect for Zynthian.
- Six distinct DSP algorithms: `Chorus` (dual-tap BBD), `Tri-Mod` (3-phase ensemble), `Flanger` (true through-zero tape flanger), `Phaser` (4..12-stage bilinear all-pass cascade), `Barber` (infinite Shepard barberpole phaser), and `Uber` (8-tap diffused cloud matrix).
- Tempo-synced modulation engine with 6 shapes (`Sine`, `Tri`, `Exp`, `S&H`, `Chaos`, `Env`), bipolar `Env Mod`, `OverMod` (musical → audio-rate FM/chaos), `Diffuse`, `Drive`, `Low Cut` / `High Cut`, `LFO Freeze`, and click-free `Bypass`.
- Interactive 800×480 Tkinter touch widget (`Main` live topology/LFO visualizer + `Tone & OverMod` strip + dedicated `Live FX` X-Y pad with `HOLD / SNAP 0` and `ZERO 0`).
- 14 factory presets (7 musical classics + 7 extreme/Über sound design).
- Full C DSP test suite, LV2 host smoke test, headless widget check, and ASan/UBSan verification.
