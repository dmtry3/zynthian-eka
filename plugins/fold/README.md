# Eka Fold for Zynthian

[Русская версия](README.ru.md)

A polyphonic **West Coast synthesizer** for [Zynthian](https://zynthian.org), as an **LV2 instrument plugin**, with an
**on-screen widget** for the Zynthian touch UI that shows what every page of knobs does.

A complex oscillator pair (Principal + Mod OSC) with FM, AM, Sync and Timbre between them, a wavefolder and two
Chebyshev shapers, a lopass gate on a vactrol model, two function generators, a mod matrix, and a waveguide / delay
in every voice. Up to 8 voices. Inspired by the Buchla 259 / 292 / 281 and the Music Easel, and by Madrona Aalto.

> Tested on: Raspberry Pi 5, Zynthian V5 kit, 800x480 touch screen. Other hardware is untested.

## Features

- **Principal oscillator** with six sources: **Sine**, **VA** (two variable-shape oscillators: triangle, saw,
  square, narrow pulse, the second one up to two octaves away), **FM2** (two-operator FM with feedback), **Wave**
  (4 banks of 8 x 8 waves), **Harm** (additive, 24 harmonics), **PD** (phase distortion). Harmonics, Color and
  Morph shape each source; changing the source crossfades.
- **Mod OSC** (triangle, sine, saw, square) in **Free** (0.01 Hz .. 10 kHz: an LFO or an audio oscillator),
  **Keytrack** or **Ratio** mode, into the Principal through **FM** (linear, through zero), **AM**, hard **Sync** and
  **Timbre**, all with one **Mod Index**.
- **Timbre section:** a triangle wavefolder and the Saw / M shapers, crossfaded by **Order**; **Timbre** = depth,
  **Symmetry** = offset (even harmonics), **Drive** = soft saturation. Band-limited: the voices run at 2x with
  ADAA on the folder.
- **Lopass gate** after the Buchla 292: a vactrol that opens fast and closes slowly, slower as it goes dark;
  **VCA**, **LP** or **Combo**, Decay, Resonance.
- **Function generators** FG1 (drives the gate) and FG2, after the Buchla 281: Rise / Fall, **Once**, **Gate** or
  **Cycle** (an LFO).
- **Mod matrix** 8 x 11: FG1, FG2, Mod OSC, Random (a new value every Mod OSC period), Velocity, Key, Mod wheel,
  Aftertouch (channel and polyphonic) into Pitch, Harmonics, Color, Morph, Mod Index, Order, Symmetry, Timbre, LPG,
  Delay Time, Delay Feedback.
- **Waveguide / delay in every voice:** **Tuned** (a string tuned to the note, -24..+24 semitones: Karplus-Strong,
  in tune within a cent from C2 to C7) or **Free** (1 ms .. 2 s echoes), with a peaking EQ and a shaper in the loop;
  or a **resonator** the voice excites: **Modal** (24 modes: string, bar, bell, plate by Structure) or **String**
  (a physical string: a sitar's buzz, plain, or piano / bell-like dispersion). The tail keeps the voice alive after
  the key is let go.
- **Play:** Poly (1..8 voices), **Mono** (last key wins, back to the key still held), **Legato**; Glide; pitch bend
  range.
- **32 factory presets** in the bank *Factory*: Perc, String, Bell, Pad, Drone, Bass, Lead, FX.
- **Touch widget** with a tab for every page: the waveforms, the folder curve, the gate's response, the function
  generators, the loop's EQ and shaper, the voices, the matrix grid; live marks (notes, Mod OSC phase, vactrol,
  FG points, the delay line's level).

## Installation

On the Zynthian (SSH as root):

```bash
git clone https://github.com/dmtry3/zynthian-eka.git
cd zynthian-eka/plugins/fold
sudo make install              # builds the plugin and links it into Zynthian (see below)
systemctl restart zynthian
```

Update later with one line:

```bash
cd /root/zynthian-eka && git pull && cd plugins/fold && sudo make install && systemctl restart zynthian
```

`make install` builds on the device (gcc, g++, `lv2-dev`; all on Zynthian) and creates:

| Link | Target |
|---|---|
| `/usr/local/lib/lv2/eka-fold.lv2` | `build/eka-fold.lv2` |
| `/zynthian/zynthian-ui/zyngui/widgets_user/zynthian_widget_eka_fold.py` | `ui/zynthian_widget_eka_fold.py` |

It also creates the folder for your own presets (`zynthian-my-data/presets/lv2/Eka_Fold.presets.lv2`) and refreshes
Zynthian's preset list. Nothing in Zynthian's own files is changed. To remove: `sudo make uninstall`.

Add **Eka Fold** to a chain as a synth (Instrument).

## Controls

Seven pages of 4 knobs (the 4 zynpots). The widget's tab follows the page, and a tap on a tab selects its page.

| Page | Knobs | |
|---|---|---|
| **Osc** | Source, Harmonics, Color, Morph | Sine / VA / FM2 / Wave / Harm / PD and its three knobs |
| **Mod** | Mod Freq, Mod Index, Mod Shape, Mod Mode | Mod Freq is read by the mode: Hz, semitones or a ratio (1/8 .. 16) |
| **Timbre** | Timbre, Order, Symmetry, Drive | Folder (Order 0) .. Saw (50) .. M (100) |
| **Gate** | Gate Mode, Decay, Resonance, Level | VCA / LP / Combo; Decay 10 ms .. 2 s |
| **Env** | FG1 Rise, FG1 Fall, FG2 Rise, FG2 Fall | 1 ms .. 10 s |
| **Delay** | Delay, Time, Feedback, Mix | Off / Tuned / Free / Modal / String; Time: semitones (Tuned, Modal, String) or 2 s x (Time / 100)^3 (Free); Feedback = Decay for Modal and String |
| **Play** | Play, Voices, Glide, Bend | Poly / Mono / Legato; 1..8 voices; Glide up to 2 s; Bend 0..24 semitones |

More settings live on the widget (they are saved in snapshots and presets):

| Tab | On the screen |
|---|---|
| **Osc** | Source tiles, **Pitch** and **Fine** (a finger up / down, a double tap resets), the waveform, the notes |
| **Mod** | Tap the arrows **FM / AM / Sync / Timbre** to route the Mod OSC; the result drawn for a sine |
| **Fold** | The transfer curve and what it makes of the waveform |
| **Gate** | Buttons VCA / LP / Combo; the response as the vactrol closes; the live vactrol |
| **Env** | Buttons **Once / Gate / Cycle** for FG1 and FG2; FG2 -> Timbre depth |
| **Delay** | Buttons Off / Tuned / Free / Modal / String; sliders **Shaper**, **EQ Freq**, **EQ Gain** (0.1 dB steps), or for the resonators **Structure**, **Brightness** (and **Position** for Modal); the modes or the string drawn; the loop's level |
| **Play** | Buttons Poly / Mono / Legato; 8 voice slots (tap one to set Voices); the glide |
| **Matrix** | The 8 x 11 grid: a finger up / down on a cell, a double tap resets (blue +, orange -) |

## Tips

- **A string:** Delay **Tuned**, Mix 100 %, Feedback 90..99 %, FG1 in **Once** with a short Fall (Env tab). A rich
  strike (VA saw, some Timbre, or a burst of FM2 noise) excites it best. EQ Gain acts on every pass of the loop: a
  few tenths of a dB down damp the string; above 0 dB the loop sings on its own (the shaper holds it).
- **The Buchla "bonk":** Gate **Combo**, FG1 Once, short Fall, Decay 20..40 %.
- **Shaper** colours the delay from the first repeat: try it on echoes of a plain sine.
- The **mod matrix** adds to the knobs: FG2 (Cycle) -> Pitch a little = vibrato; Random -> Pitch with the Mod OSC
  in Free at 2..8 Hz = random steps; Velocity -> Timbre = harder is brighter.

## CPU

Raspberry Pi 5, `./scripts/bench.sh` (% of one core): 4 voices 2.6..9.3 % depending on the patch, delay +1.4 %;
8 voices with VA, the whole matrix and the folder 18 %. In Zynthian, 8 voices played densely: jalv 11 % of one core,
no xruns. Lower **Voices** if the chain needs more room.

## Building and testing (any Linux)

```bash
make            # build/eka-fold.lv2
make test       # the engine offline (pitch, aliasing, clicks, ...), the plugin in a tiny host, every preset,
                # the widget with stub Zynthian modules
```

The presets are generated: edit `scripts/gen_presets.py`, then run it (it writes `lv2/presets.ttl` and
`lv2/manifest.ttl`). `make test` renders every preset to `build_test/presets/*.wav`.

## How it works

- `src/fold_core.c`: the engine, plain C without LV2 (tested as a normal program). Voices at 2x the rate with a
  halfband decimator; the mod matrix and the filter coefficients every 32 samples, empty matrix cells skipped;
  nothing allocated in the audio thread.
- The VA, FM2, Wave, Harm and PD sources: a small C++ unit in `src/` (no exceptions, no RTTI, no STL) behind a C interface
  (`engines.h`).
- `src/eka_fold.c`: the LV2 wrapper. The widget's settings and the matrix are LV2 parameters (`patch:writable`),
  saved in LV2 State; live data for the widget comes from output ports.
- `ui/zynthian_widget_eka_fold.py`: the widget (Tkinter). It draws its curves with the plugin's own formulas and
  redraws only what changed.

## License

GPL-3.0-or-later. The VA, FM2, Wave, Harm and PD sources, the Modal and String resonators and three tables are derived from MIT-licensed code by
Emilie Gillet: see [THIRD_PARTY.md](THIRD_PARTY.md) for the license text and what was changed.
