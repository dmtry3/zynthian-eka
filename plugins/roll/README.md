# Eka Roll for Zynthian

[Русская версия](README.ru.md)
Demo video https://www.youtube.com/watch?v=ChqsykTJQ88

A MIDI looper for [Zynthian](https://zynthian.org) implemented as an **LV2 MIDI-tool plugin** that sits inside a
chain, plus an optional **on-screen widget** (piano roll, layers, slots) for the Zynthian touch UI.

It records and overdubs notes and controller moves, stays locked to the Zynthian transport (`zynseq`), and can export
a loop to a Standard MIDI File and straight into a `zynseq` pattern.

> Tested on: Zynthian **Vangelis** (stable 2610), Raspberry Pi 5, V5 kit, 800x480 touch screen.
> Other versions/hardware are untested.

## Features

- **Transport-locked looping.** The loop runs only while the Zynthian transport (`zynseq`) is playing and follows
  tempo changes. Recording starts/stops on a beat/bar/loop boundary (*Launch*), so the first beat always matches.
- **Rec / Overdub / Play / Stop** with up to **8 layers**, per-layer mute, delete, undo/redo.
- **Fixed or free length** (Free, 1, 2, 4, 8, 16 bars); fixed length stops recording automatically.
- **Smart timing.** Notes played slightly before the downbeat while armed are recorded at the start of the bar.
  Notes pressed just before the loop end during overdub are placed at the loop start (no micro-notes).
- **Controllers on a shared lane.** CC, pitch bend and aftertouch are stored on a single lane (not per layer).
  Touching a knob mutes that controller's playback and overwrites its old data while you move it; after 0.5 s of
  inactivity playback resumes.
- **Quantize** (1/4 ... 1/32, strength, starts/ends/both) applied to the layers you pick on screen. Undoable.
- **Edit.** Half / Double loop, **per-layer transposition**.
- **16 slots** on disk, auto-load of the saved slot when a Zynthian snapshot is restored.
- **Export** to a `.mid` file and into a `zynseq` pattern (4 steps per beat) with one button.
- **Touch widget.** State, progress, beat/layer indicators; piano roll with bars, notes and a CC lane (Edit page);
  layer selector (Quantize page); slot map (Slots page).
- Live MIDI always passes through; the plugin never blocks your playing.

## Requirements

- Zynthian with `zynseq` running as transport/tempo master.
- `gcc`, `pkg-config`, and LV2 headers (`lv2-dev` or installed from source).
- In the chain's MIDI settings, **"Route CC" must be enabled**, otherwise Zynthian does not pass CC to the chain.

## Installation

```bash
git clone https://github.com/dmtry3/zynthian-eka.git
cd zynthian-eka/plugins/roll
sudo ./scripts/install.sh      # builds the plugin and creates two symlinks (see below)
sudo systemctl restart zynthian
```

The script creates:

| Link | Target |
|---|---|
| `/usr/local/lib/lv2/eka-roll.lv2` | `build/eka-roll.lv2` |
| `/zynthian/zynthian-ui/zyngui/widgets_user/zynthian_widget_eka_roll.py` | `ui/zynthian_widget_eka_roll.py` |

Nothing in Zynthian's own files is modified. To remove: `sudo ./scripts/uninstall.sh`.

Add **Eka Roll** to a chain as a *MIDI tool* placed before the synth.

## Controls

Controls are grouped into pages (4 controllers per page, matching the 4 zynpots).
All buttons are toggle-type: any change counts as a press.

| Page | Control | Description |
|---|---|---|
| **Loop** | Rec/Dub | Empty: arm recording. Recording: arm stop. Playing: overdub on/off |
| | Play/Stop | Mute / unmute the loop (position keeps running) |
| | Undo/Redo | Tap: undo last edit (Apply/Half/Double), otherwise last overdub layer. Long press (touch or encoder push): redo the last undone layer |
| | Clear | Erase everything (press twice) |
| **Layers** | Layer | Selects layer for Mute / Delete |
| | L Mute | Mute/unmute selected layer |
| | L Delete | Delete selected layer (press twice) |
| **Timing** | Length | Free, 1, 2, 4, 8, 16 bars |
| | Launch | Beat / Bar / Loop: when recording and play start |
| | Half / Double | Halve / double the loop (also affects the CC lane) |
| **Quantize** | Grid | Off, 1/4, 1/8, 1/16, 1/32 |
| | Strength | 0-100 % |
| | Target | Starts / Ends / Both |
| | Apply | Quantize the layers selected on the widget |
| **Edit** | Layer | Layer to transpose |
| | Transpose | Semitones to add (-12...+12) |
| | Apply | Adds Transpose to the layer's offset; the knob returns to 0 |
| | Export | `.mid` file + `zynseq` pattern |
| **Slots** | Slot | 1-16 (0 = none) |
| | Save / Load | Save to / load from slot (load happens at loop end) |
| | Del Slot | Delete slot file (press twice) |

A second Undo/Redo within 0.3 s is ignored (touch screens sometimes report one tap twice).
A MIDI button learned to Undo/Redo: tap = Undo (on release), hold 0.5 s = Redo. Use a momentary button
(127 on press, 0 on release); the widget must have been opened once since Zynthian started.

Zynthian's sequencer can stop or restart a scene while the JACK transport keeps running. The widget passes
zynseq's beat to the looper (hidden `zbeat` port), so the loop pauses when zynseq stops and its downbeat follows
zynseq's when a scene starts.

Half, Double, Apply (both) and Export react only to the press (the release of a momentary pedal is ignored), so
they can be mapped to hardware buttons.

### Widget

- All pages: state (EMPTY, ARM REC, REC, OVERDUB, PLAY, STOP, ...), bars/BPM, loop progress, beat blocks, layers
  (each layer has its own colour), messages and confirmations.
- Every page except Slots: piano roll with bars/beats, coloured layers and a CC lane underneath; the beat blocks and
  the layer strip on top (tap a layer to select it).
- **Quantize:** the layer strip chooses which layers are quantized (tap to toggle); the chosen grid is drawn in blue
  over the notes (brighter for a moment after it changes).
- **Edit:** white preview outline for the pending transposition.
- **Slots:** 16-cell map of occupied slots.

## Export

- `.mid`: `/zynthian/zynthian-my-data/capture/eka_roll_YYYYMMDD_HHMMSS.mid` (unmuted layers, transposition
  applied, tempo and time signature included).
- Pattern: if the sequencer cell of the chain (current phrase x chain's MIDI channel) is playing, its pattern is
  overwritten; otherwise the first empty pattern is filled and assigned to that cell if it is free.
  4 steps per beat; sub-step timing is kept with note offsets (`EXPORT_USE_OFFSET` in the widget); CC moves become
  ramps. Edit the constants at the top of `ui/zynthian_widget_eka_roll.py` to change this.

## Limitations

- Needs a running transport. Stopping/relocating the transport or changing tempo during the *first* recording
  cancels that recording.
- Played-back CC also drive Zynthian MIDI-learn bindings (synth and effect parameters, on-screen knobs). This is done by the widget, so it starts once the Eka Roll page has been opened after boot. An echo guard prevents feedback loops.
- Capacity: 8 layers x 4000 events, 16384 controller events.
- Snapshots store control values only; loop data lives in slots (a saved slot is auto-loaded on restore).
- Loop timing is tied to the transport position at the moment of recording.

## Troubleshooting

| Problem | Check |
|---|---|
| Plugin not listed | `lv2ls \| grep zynthian-eka/roll`, restart Zynthian |
| CC are not recorded | Enable *Route CC* in the chain; check with `jack_midi_dump` on the chain's router output |
| No widget | Symlink exists in `widgets_user`; `journalctl -u zynthian \| grep -i widget` |
| Piano roll is empty | `ls /dev/shm \| grep eka_roll` |
| Loop does not play | The transport must be running (press Play in zynseq) |

## How it works

- `src/eka_roll.c`: the whole engine. Time is kept in ticks (960 per beat) and slaved to the JACK transport via
  `time:Position`. Playback is sample-accurate inside each audio block.
- Slot files are written/read by a worker thread (never in the audio thread).
- The widget reads the loop from shared memory (`/dev/shm/eka_roll_<pid>`) and reads state from the plugin's
  output ports (Zynthian "monitors"). It writes only through regular controllers.

## Building manually

```bash
./scripts/build.sh        # output in build/eka-roll.lv2
```

## License

See `LICENSE`.
