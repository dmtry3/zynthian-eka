# Changelog

## 0.14.0 (2026-11-19)

First public version.

- Four-voice real-time LV2 pitch harmonizer (mono in, stereo out) powered by Bungee Basic (standard granularity 0) with zero heap allocations in the audio callback.
- Four harmony modes: **Interval** (`-12..+12 st`), **Chord** (14 chord types, 5 voicings including **Lead** smooth voice leading, per-voice octave offset), **MIDI** (absolute held note targets with embedded aubio YIN-fast input tracker), and **Scale** (12 keys, 9 seven-note scales, signed per-voice degrees).
- Per-voice **Delay** (`0..8 s`, `0..90%` feedback, host tempo sync `1/64..4 bars`, tail preservation on mute), **Character** (fine detune `±50 cents`, organic micro-pitch/timing humanize), and **Tone** (12 dB/oct HPF/LPF with logarithmic response curves).
- **Scenes A–D** stored in canonical LV2 control ports (retained by Zynthian snapshots and LV2 presets), plus dedicated **Scenes** page with `Prev` / `Next` (MIDI Learn friendly for footpedals) and `Store`.
- Low-latency **Live** dry timing (default) and **Fast** native-latency per-voice timing (default), with optional **Aligned** modes.
- Nine factory presets (*Weave-4 Factory*) and Zynthian touch widget (`Main`, `Voice`, `Time`, `Char`, `Tone`, `Out`, `Scene`).
