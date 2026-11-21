# Eka Pads

**English** · [Русский](README.ru.md)

16-pad Bitwig-style drum sampler and 8-scene step sequencer LV2 instrument for [Zynthian](https://zynthian.org), designed for live finger-drumming with pad controllers like **M-VAVE SMC-PAD Pocket** over Bluetooth MIDI.

## Features
- **16 independent pads (4×4 grid)** with async background sample loading (`libsndfile`: WAV, FLAC, AIFF, OGG, MP3) via LV2 Worker and a built-in 16-sample factory drum kit (`samples/`).
- **Per-pad DSP engine**:
  - `Pitch` (±24 st), `Decay` (10 ms – 10 s / Full), bipolar DJ `Filter` (LP / Open / HP), `Level`
  - `Start` / `End` trim, 4 playback modes (`One-Shot`, `Gate`, `Rev Shot`, `Rev Gate`), `Punch` (fast transient pitch envelope), `Drive` (soft saturation + bitcrusher)
  - `Pan`, `Vel Sens`, 4 `Choke Groups` (smooth 2 ms click-free fade), `Space` send to built-in stereo Room Reverb, `Mute` & `Solo`
- **Auto-Select for knobless pad controllers (M-VAVE SMC-PAD Pocket)**:
  - Hitting any pad on your controller automatically focuses that pad in the widget and syncs Zynthian's 4 physical encoders to its parameters without value jumps.
  - **Pad Learn**: bind any MIDI note to any pad in one tap.
- **8-Scene 16-Step Sequencer (`A..H`)**:
  - Per-step velocity (`0..127`) and ratchet rolls (`1x..4x`), `Swing` (`50..75%`), quantized scene switching at bar boundaries (via touchscreen or MIDI notes `52..59`).
  - **Live Overdub (`Rec`)** directly from MIDI pads, synced to Zynthian / `Eka Roll` transport (`time:Position`) or internal BPM (`40..240`).
  - Full kit (16 sample paths, 16 pad configs, 8 scenes, BPM) saved in Zynthian snapshots (`LV2 State`).

## Build & Install

```bash
make                # build build/eka-pads.lv2
make test           # run unit + LV2 host + widget layout tests
sudo make install   # build & link into Zynthian (/usr/local/lib/lv2 & widgets_user)
```

## License

GPL-3.0-or-later.
