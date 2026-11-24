# Eka Pads for Zynthian

**English** · [Русский](README.ru.md)

A **16-pad drum sampler, modulation engine, and polyrhythmic scene sequencer** for [Zynthian](https://zynthian.org), implemented as an **LV2 instrument plugin** (MIDI in, main stereo out + 4 stereo Aux sends, LV2 `time:Position` transport sync) plus a **4-tab touch widget** (`Pads`, `Mod`, `Seq`, `Kit`) designed for live finger-drumming with pad controllers like **M-VAVE SMC-PAD Pocket** over Bluetooth MIDI.

Made for four encoders: **Auto-Select** follows the last hit pad and keeps Zynthian's 4 physical encoders synced to that pad across seven clean knob pages without value jumps.

> Tested on: Zynthian **Vangelis** (stable 2610), Raspberry Pi 5, V5 kit, 800×480 touchscreen.
> Other versions/hardware are untested.

## Features

- **16 independent pads (4×4 grid)** with async background sample loading (`libsndfile`: `WAV`, `FLAC`, `AIFF`, `OGG`, `MP3`) via LV2 Worker and a built-in 16-sample factory drum kit (`samples/`).
- **Per-pad DSP engine:**
  - Anti-aliased windowed sinc `Pitch` (`±24 st`), `Decay` (`0..100%`), bipolar DJ `Filter` (`LP` / Open / `HP`), `Level`.
  - `Start` / `End` waveform trim, 4 playback modes (`One-Shot`, `Gate`, `Rev Shot`, `Rev Gate`), transient `Punch` (`-100..+100%`), `Drive` (soft saturation + bitcrush).
  - `Pan`, `Vel Sens`, 4 click-free `Choke` groups (2 ms fade), bipolar `Width` (`-100..+100%`, all-pass + dual-Haas stereo enhancer), `Mute` & `Solo`.
- **4 Stereo Aux Sends (`Aux 1..4`):** per-pad send levels routed directly to Zynthian Mixbuses from the `Kit` tab.
- **Per-pad Modulation Matrix (`Mod` tab):**
  - 4 modulation slots per pad: sources (`Off`, `Vel`, `Rand`, `LFO1`, `LFO2`, `Env`) → destinations (`Pitch`, `Decay`, `Filter`, `Level`, `Start`, `Drive`, `Pan`, `Width`, `Aux1`, `Aux2`) with bipolar amount (`-100..+100%`).
  - 2 independent LFOs per pad (`Sine`, `Tri`, `Saw`, `Sqr`, `S&H`, `Free` `0.05..20 Hz` or tempo `Sync` `4/1..1/16T`) with a waveform preview popup, plus an Envelope Follower.
- **Polyrhythmic Scene Sequencer (`Seq` tab):**
  - Up to **32 steps** per pad with independent `Beg..End` loop range (`1..32`), `Swing` (`50..75%`), smooth `Quantize` (`0..100%` with real microtiming), `Velocity` (`0..127`) and ratchet `Roll` (`1x..4x`).
  - **5 large scenes (`A..E`)** on screen (8 scenes internally, `A..H`), quantized switching at bar boundaries via touchscreen or MIDI notes `52..56`.
  - **Live Overdub (`Dub/Rec`)** from MIDI pads, tightly phase-locked (`<0.25 ms`) to Zynthian / `Eka Roll` Host Transport (`time:Position`) or internal BPM (`40..240`).
  - **One-tap export to ZynSeq (`->ZSeq`)** with automatic bar-count calculation from the LCM of polyrhythmic pad lengths.
- **Controller-ready (`M-VAVE SMC-PAD Pocket`):** Auto-Select, one-tap MIDI Learn, and a hardware trigger filter (`Retrig`, `MinVel`, `XTalk`) to eliminate double/phantom hits.
- **Full Snapshot Persistence:** all 16 sample paths, pad parameters, modulation, sequencer patterns, and Aux routes are saved in Zynthian snapshots (`LV2 State`).

## Installation

On Zynthian (SSH as root):

```bash
git clone https://github.com/dmtry3/zynthian-eka.git
cd zynthian-eka/plugins/pads
sudo make install
systemctl restart zynthian
```

Update in one line:

```bash
cd /root/zynthian-eka && git pull && sudo make install-pads && systemctl restart zynthian
```

| Link or folder | Contents |
|---|---|
| `/usr/local/lib/lv2/eka-pads.lv2` | LV2 bundle (`eka_pads.so`, `manifest.ttl`, `eka_pads.ttl`) |
| `zyngui/widgets_user/zynthian_widget_eka_pads.py` | Touch widget (`ui/`) |
| `zynthian-my-data/files/Samples/eka-pads` | 16-sample factory drum kit (`samples/`) |
| `zynthian-my-data/files/Samples/eka-pads-user` | User samples folder |
| `zynthian-my-data/presets/lv2/Eka_Pads.presets.lv2` | User presets folder |

`install.sh` refreshes Zynthian's preset cache; nothing in Zynthian's own files is modified. To remove: `sudo make uninstall`.

If adding for the first time, enable **Eka Pads** in Zynthian webconf (*Software → LV2 Plugins*), then add it to a chain as a *Synthesizer / Instrument*.

## Controls

Seven pages of 4 encoders (pages 1–5 follow the currently selected pad):

| Page | Knob 1 | Knob 2 | Knob 3 | Knob 4 |
|---|---|---|---|---|
| **1. Pad Sound** | `Pitch` (`-24..+24 st`) | `Decay` (`0..100%`) | `Filter` (`-100..+100%`) | `Level` (`0..100%`) |
| **2. Pad Shape** | `Start` (`0..95%`) | `Mode` (`One-Shot/Gate/Rev Shot/Rev Gate`) | `Punch` (`-100..+100%`) | `Drive` (`0..100%`) |
| **3. Pad Mix** | `Pan` (`-100..+100%`) | `Vel Sens` (`0..100%`) | `Choke` (`Off / Grp 1..4`) | `Width` (`-100..+100%`) |
| **4. Pad Sends** | `Aux 1` (`0..100%`) | `Aux 2` (`0..100%`) | `Aux 3` (`0..100%`) | `Aux 4` (`0..100%`) |
| **5. Pad Mod** | `LFO 1 Hz` (`0.05..20 Hz`) | `LFO 2 Hz` (`0.05..20 Hz`) | `Mod 1 Amt` (`-100..+100%`) | `Mod 2 Amt` (`-100..+100%`) |
| **6. Seq & Master** | `Seq` (`Stop / Play / Rec`) | `Scene` (`Scene A..E`) | `Swing` (`50..75%`) | `Master` (`0..100%`) |
| **7. Steps & Stereo** | `Stereo` (`0..200%`) | `Steps` (`1..32`) | `Beg Step` (`1..32`) | — |

## Widget & Gestures

Four tabs along the top bar (`Pads | Mod | Seq | Kit`):

- **`Pads` tab:**
  - **Left 4×4 pad grid:** tap a pad to trigger and select it; **long press** toggles `Mute`.
  - **Right inspector:** `Load`, `Mute`, `Solo`, `Auto/Lock` (Auto-Select); interactive waveform box (drag left/right edges to trim `Start`/`End`, **long press** opens the Sample Settings popup with `Pitch / Level / Decay / Filter / Start / End / Punch / Drive / Pan / Width / Mode / Choke`, or tap empty slot to load WAV/FLAC); compact `Aux 1..4` horizontal send sliders; large `Sc A..E` scene launch bar at the bottom.
- **`Mod` tab:**
  - **Top `LFO 1` & `LFO 2` buttons:** tap to open the LFO popup with live waveform preview, `Free / Sync` toggle, wave selector (`Sine..S&H`), and `Rate` slider.
  - **4 Matrix rows:** tap source/destination buttons to cycle, drag the bipolar slider (`-100..+100%`) for modulation depth.
  - **Bottom 16-pad strip:** tap to switch pad, long press to `Mute`.
- **`Seq` tab:**
  - **Top bar:** large scene buttons `A..E`, horizontal `Q` quantize slider (`Off / 1..100%`), `Copy` scene, `Clr` pad lane, and `->ZSeq` export.
  - **Second bar:** `Play/Stop`, `Dub/Rec`, draggable `BPM`, and horizontal `Beg..End` steps range slider (`1..32`) for polyrhythms.
  - **4×8 LED step grid:** short tap toggles a step; drag up/down adjusts `Velocity`; **long press** opens the step popup with `Velocity` (`0..127`), `Chance` (`10..100%`), and `Roll` (`1x..4x`) sliders.
  - **Bottom 16-pad strip:** tap to select lane, long press to `Mute / Unmute`.
- **`Kit` tab:**
  - `Load Factory 16-Pad Kit`, `MIDI Learn` for the selected pad, `Clear Scene`, `Unload Pad`, `Reset All` (two-tap confirmation to reset all settings, scenes, and samples to default).
  - Hardware pad filter sliders: `Retrig` (`0..80 ms`), `MinVel` (`0..40`), `XTalk` (`0..60%`).
  - `Aux 1..4 -> Mixbus` routing buttons (tap to cycle available Zynthian Mixbuses).

## Building & Testing on Linux / WSL

```bash
make -C plugins/pads                 # build build/eka-pads.lv2
make -C plugins/pads test            # DSP core, LV2 host and widget tests
```

## License

GPL-3.0-or-later. See [LICENSE](LICENSE).
