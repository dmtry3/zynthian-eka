# Changelog

## 0.3.2 (2026-11-26)

- **MIDI-Mappable `Play` Encoder Pages (`Play FX`, `Play Mutes`, `Play Perf`):** exposed 12 dedicated LV2 ControlPorts across 3 top-priority Zynthian encoder pages shown automatically when the `Play` tab is active (with 1-tap `FX | Mute | Perf` page pills in the top bar):
  - **`Play FX`:** `Mst Filter`, `Mst Reso`, `Stutter` (`Off / 1/4 / 1/8 / 1/16`), `Aux1 Throw`.
  - **`Play Mutes`:** `Mute G1`, `Mute G2`, `Mute G3`, `Mute G4`.
  - **`Play Perf`:** `Tape Stop`, `Break`, `Note Rpt` (`Off..1/32`), `16 Levels` (`Normal / Chrom / 16 Vel / 16 Flt`).
  - Full two-way sync between the touchscreen `Play` UI, Zynthian's physical encoders, and Zynthian MIDI Learn.

## 0.3.1 (2026-11-26)

- **Dedicated `Play` Performance Tab (first tab):** live stage view (`Play | Pads | Mod | Seq | Kit`) with large touch targets:
  - **4 Choke Group Mute buttons (`G1..G4`):** independent click-free live Mute per choke group (short tap) and Solo (long press) without touching individual pad mutes, live group activity meters, plus a one-tap `All On` reset button.
  - **Direct Note Repeat (`Off | 1/8 | 1/16 | 16T | 1/32`) & 16 Levels (`Norm | Chrm | 16Vl | 16Fl`) strips:** instant 1-tap switching without cycling.
  - **Master DJ Filter XY-Pad:** stereo TPT SVF `LP ← Open → HP` (X) + `Reso` (Y) with `Mom` (smooth auto-return on release) and `Hold` (double-tap to reset) modes.
  - **Momentary Punch-in FX (hold):** tempo-synced master `Stut 1/4`, `Stut 1/8`, `Stut 1/16`, `Tape Stop` vinyl brake, `Break` (smoothly mutes Main Out while Aux 1..4 reverb/delay tails ring out), and `Aux1 Throw` (momentary 100% Aux 1 dub throw).
  - **Full-width Scene Launcher (`A..E`) + 4-Beat Bar Progress strip:** tap for quantized scene switch, long-press for 1-bar Fill.
- **Snapshot Restore Fix:** restoring a Zynthian snapshot or LV2 preset no longer auto-starts the sequencer from a saved `seq_mode` when host transport is stopped; explicit UI `Play`/`Dub` and host transport start still engage immediately.

## 0.3.0 (2026-11-26)

- **Quick Sample Step (`<` / `>`) & Load Folder as Kit:** step through audio files (`WAV/FLAC/AIFF/OGG/MP3`) in the pad's current folder directly from the `Pads` tab with instant audition, and load any sample folder into Pads 1..16 via `Load Folder Kit` on the `Kit` tab.
- **16 Levels / Chromatic Mode (`16L`) & Note Repeat (`Rpt`):** two unobtrusive performance pills in the top bar. `16L` cycles `Off → Chromatic → 16Vel → 16Flt` across both touch pads and physical MIDI controllers (plus external MIDI keyboards in Chromatic mode), recording directly into Overdub with p-locks; `Rpt` provides tempo-synced MPC-style retriggers (`1/8`, `1/16`, `1/16T`, `1/32`) while holding pads.
- **Auto-Slice, Pad Link (Layer / Round-Robin) & Copy/Swap Pad:** organized under a clean `Tools` sub-view inside the Sample Settings popup (long-press waveform) — slice any loop into 4/8/16 grid pads or by transient detection with live preview markers and optional Choke Grp 1, link pads for simultaneous layering or round-robin cycling, and copy or swap pads in one tap.
- **Attack, Filter Resonance (`Reso`), Loop Mode & Master Bus Glue:** added exponential per-pad `Attack` (`0..500 ms`), SVF `Reso` (`0..100%`), 5th playback mode `Loop`, and stereo-linked Master `Bus Glue` compressor (Page 7 4th knob + `Kit` tab `Glue` slider).
- **Step Parameter Locks, Euclidean Generator, Pattern Shift, Metronome/Count-in, Undo Dub, Scene Chain & 1-Bar Fill:** expanded the Step Popup with `Pitch`, `Filter`, and `Microtiming` p-locks; added a compact `Seq Tools` popup (`Euclid` Hits/Rot, `< Shift / Shift >`, `Metro Off/Rec/On`, `Count-in 1B`, `Undo Dub`, and 8-bar `Scene Chain`); long-press any Scene button (`A..E`) while playing for a quantized 1-bar Fill with automatic return. Full backward-compatible LV2 state migration from `0.2.x`.

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
