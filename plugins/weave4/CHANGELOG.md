# Changelog

## 0.16.0 (2026-11-19)

- **Tone filter upgrades:** independent `12 / 24 dB/oct` slopes for HPF and LPF, resonance control (`Res`, `0..100%`) with per-filter resonance toggles above the Tone plot, `+12..-36 dB` response curve display, and Tone filtering inside the delay feedback loop.
- **MIDI & Voice tracking:** smoothed pitch-shifter input gate eliminating attack clicks on peak-phase MIDI Note-On/retrigger; added `Voice` tracking profile (`75–1100 Hz`, vocal-friendly confidence/hold) and widened `-12 / +21` range-edge margin for natural vocal vibrato.
- **Glide, Width & Master Limiter:** added per-scene `Glide` (`0..500 ms`) on `Char`, `Voice timing` + `Width` (`0..100%` stereo spread) on `Out`, zero-latency stereo-linked brickwall master limiter with `LIM` indicator, and soft delay feedback saturation.
- **UX & Delay:** reduced max delay to `3 s` (`3000 ms`), removed redundant `Scene` knobs from `Time`/`Char`/`Tone`, showed selected voice `Pitch`/`Level` on `Main` in `Interval` mode, auto-enabled all chord voices in `Chord` mode, kept current page when selecting voices, added tap-selected-card `On/Off` toggle and `In: Auto/Gtr/Flt/Voc` quick button in `MIDI`/`Scale` modes.

## 0.14.0 (2026-11-19)

First public version.

- Four-voice real-time LV2 pitch harmonizer (mono in, stereo out) powered by Bungee Basic (standard granularity 0) with zero heap allocations in the audio callback.
- Four harmony modes: **Interval** (`-12..+12 st`), **Chord** (14 chord types, 5 voicings including **Lead** smooth voice leading, per-voice octave offset), **MIDI** (absolute held note targets with embedded aubio YIN-fast input tracker), and **Scale** (12 keys, 9 seven-note scales, signed per-voice degrees).
- Per-voice **Delay** (`0..8 s`, `0..90%` feedback, host tempo sync `1/64..4 bars`, tail preservation on mute), **Character** (fine detune `±50 cents`, organic micro-pitch/timing humanize), and **Tone** (12 dB/oct HPF/LPF with logarithmic response curves).
- **Scenes A–D** stored in canonical LV2 control ports (retained by Zynthian snapshots and LV2 presets), plus dedicated **Scenes** page with `Prev` / `Next` (MIDI Learn friendly for footpedals) and `Store`.
- Low-latency **Live** dry timing (default) and **Fast** native-latency per-voice timing (default), with optional **Aligned** modes.
- Nine factory presets (*Weave-4 Factory*) and Zynthian touch widget (`Main`, `Voice`, `Time`, `Char`, `Tone`, `Out`, `Scene`).
