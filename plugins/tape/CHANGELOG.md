# Changelog

## 0.1.1 (2026-11-21)

- **Non-destructive loop Trim:** long-press a recorded track on the `Loop` screen to open the Trim window and adjust loop start/end markers with `FREE`, `BEAT`, or `BAR` snap; trim boundaries are saved in sessions.
- Widget usability improvements and fixed master level indicator on the `Loop` screen.

## 0.1.0 (2026-11-13)

Initial release of **Eka Tape** (`http://dmtry3.github.io/zynthian-eka/tape`):

- Multi-part live stereo audio looper: 4 × 4 (or 2 × 2 / 6 × 6) parts and tracks, unlimited overdubs with Undo/Redo, RPO/ROP record order, Free and Quantize modes, count-in, and signal-threshold recording start.
- **Lock** tracks into the next empty part seamlessly; quantized part switches and mutes.
- Tape-style transport tempo following, per-track speed (0.5×–2×) and pitch shift (±12 semitones via Signalsmith Stretch).
- Mixer with per-track level, stereo/split pan, mute, peak meters, and master soft limiter.
- Background 24-bit WAV session save/load via Zynthian presets without interrupting playback.
- Momentary footswitch-ready MIDI controllers and touch widgets (`Loop`, `Mixer`, `Speed`, `Settings`).
