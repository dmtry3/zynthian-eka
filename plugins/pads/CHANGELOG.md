# Changelog

## 0.2.0 (2026-11-23)

- **Per-pad Modulation Matrix (`Mod` tab):** 4 modulation slots per pad (`Off`, `Vel`, `Rand`, `LFO1`, `LFO2`, `Env` → `Pitch`, `Decay`, `Filter`, `Level`, `Start`, `Drive`, `Pan`, `Width`, `Aux1`, `Aux2`), 2 independent LFOs with waveform preview popup (`Sine`, `Tri`, `Saw`, `Sqr`, `S&H`, `Free` / `Sync` divisions) and an input Envelope Follower.
- **4 Stereo Aux Sends (`Aux 1..4`) & Bipolar `Width`:** replaced built-in reverb with 4 independent stereo Aux sends per pad routable to Zynthian Mixbuses from the `Kit` tab, plus bipolar `Width` (`-100..+100%`, all-pass + dual-Haas) on `Pad Mix`.
- **Polyrhythmic 32-Step Sequencer & ZynSeq Export:** independent `Beg..End` (`1..32` steps) per pad with horizontal range slider, smooth `Quantize` (`0..100%`) with real microtiming, uniform 4×8 step grid, `Vel` edit mode with top `Velocity` & `Roll` (`1x..4x`) sliders, enlarged `A..E` scene buttons, tight Host Transport phase sync (`<0.25 ms`), and one-tap export of the active scene to **ZynSeq** with automatic bar-count calculation (LCM of polyrhythms).
- **Hardware Pad Filter (`Kit` tab):** `Retrig`, `MinVel`, and `XTalk` sliders to eliminate phantom/double triggers on pad controllers like M-VAVE SMC-PAD Pocket.
- **Windowed Sinc Resampler:** anti-aliased pitch transposition for cleaner pitched drum samples.
- **Reliable Snapshots & Presets:** full `LV2 State` persistence of all 16 samples, per-pad settings, modulation, `seq_data`, and `aux_routes`, plus long-press pad Mute.

## 0.1.0 (2026-11-21)

- Initial release of **Eka Pads** (`http://dmtry3.github.io/zynthian-eka/pads`):
  - 16-pad (4×4) drum sampler with async `libsndfile` loading (`WAV`, `FLAC`, `AIFF`, `OGG`, `MP3`) and 16-sample factory kit.
  - Per-pad DSP (`Pitch`, `Decay`, bipolar DJ `Filter`, `Level`, `Start`/`End`, 4 playback modes, `Punch`, `Drive`, `Pan`, `Vel Sens`, 4 `Choke` groups, `Mute`/`Solo`).
  - Auto-Select syncing Zynthian's 4 physical encoders to the last hit pad, MIDI Learn, and step sequencer with Live Overdub.
  - Touch widget (`zynthian_widget_eka_pads.py`) and offline DSP/LV2/widget test suite.
