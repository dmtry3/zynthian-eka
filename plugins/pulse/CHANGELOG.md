# Changelog — Eka Pulse

## 1.0.0 — 2026-11-20

- **M0:** Plugin scaffold (`eka-pulse.lv2`, `eka_pulse.so`, `zynthian_widget_eka_pulse.py`), polyrhythmic 1..16-step shaper with 6 step shapes and 24 built-in patterns, `4: GATE & SPACE` slot, stereo-linked brickwall limiter, deterministic transport sync (halts when stopped, starts on beat 1).
- **M1:** `2: DRIVE & CRUSH` (Tube, ADAA Fold, Hard, Crush, Combo), `3: FILTER` (TPT SVF LP24/LP12/BP/HP/Morph/Notch + 4-stage stereo Phaser), Linkwitz-Riley 24 dB/oct `Low Split` crossover with `Low Duck`, `Flow` widget tab, dynamic per-slot encoder labels, and `Help` tab.
- **M2:** `1: TIME` slot on stereo ring buffer with 4-point Hermite interpolation and de-click crossfade (`Curve`, `Chop`, `Stutter`, `TapeStop` with tunable brake time), `Audio` / `MIDI` triggers, `Env → Depth` modulation, colored `TIME: Chop` action badges.
- **M3:** `Perform` tab with 3×3 momentary live pads, interactive XY Pad (`Cutoff` / `Depth`), 4 scenes (`A–D`) with `Store`, pedal scene switching (`Prev Scene` / `Next Scene`), and full LV2 State persistence.
- **M4:** 24 factory presets (`lv2/presets.ttl`), full preset load & audio verification in `test_plugin`, `README.md` / `README.ru.md`, monorepo `deploy-zynthian.sh` integration.
