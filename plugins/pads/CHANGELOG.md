# Changelog

## 0.2.3 (2026-11-25)

- **Optional USB SMC-PAD Patch (`make patch-smcpad` / `scripts/patch-smcpad.sh`):** disables autoloading of Zynthian's `zynthian_ctrldev_sinco_smk25.py` driver (`autoload_flag = False`) so plugging an M-VAVE SMC-PAD over USB (`SINCO IN 2`) no longer overwrites its pad layout via SMK25 SysEx. Includes `--status` and `--revert` flags.

## 0.2.2 (2026-11-25)

- **Pad MIDI Notes Window (`Kit` tab):** replaced the single `MIDI Learn` button with a dedicated 16-pad MIDI note mapping window. Select any row (`R1..R4`) or pad to control all 4 pads in that row via Zynthian's 4 physical encoders (`Pad Notes` page: `Note 1..4`), drag vertically on any pad, arm `Learn` to reassign the selected pad from a MIDI controller, or tap `Def` twice to restore `36..51`.

## 0.2.1 (2026-11-24)

- **Compact Step Popup & Velocity Drag (`Seq` tab):** long-press any step tile to open a compact popup with `Velocity` (`0..127`), `Chance` (`10..100%`), and `Roll` (`1x..4x`) sliders, while preserving vertical `Velocity` drag directly on step tiles.
- **Sample Settings Popup (`Pads` tab):** long-press the waveform box to edit all per-pad sound parameters (`Pitch`, `Level`, `Decay`, `Filter`, `Start`, `End`, `Punch`, `Drive`, `Pan`, `Width`, `Mode`, `Choke`) on screen.
- **`Reset All` (`Kit` tab):** two-tap confirmation button that cleanly unloads all samples and resets all pad settings, modulation, and scenes to defaults.
- **Reliability Fixes:** fixed repeated `Play` triggering via `seq_cmd` nonce (with one-shot reset to `0` for clean snapshots), fixed preset per-pad settings persistence, and prevented dropped sample loads from LV2 worker ringbuffer overflow.

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
