# Eka Echo for Zynthian

**English** · [Русский](README.ru.md)

A **stereo & polyrhythmic live delay** for [Zynthian](https://zynthian.org), implemented as an **LV2 audio effect plugin** (stereo in, stereo out, LV2 `time:Position` BPM sync) plus an **interactive touch widget** (draggable L/R stereo tap map, loop HPF–LPF filter bar, IN/DUCK/OUT meters, and live Freeze/Trails/Bypass buttons) for the Zynthian touchscreen.

Made for four encoders: six clean knob pages covering time, mode/routing, loop tone, modulation/ducking, performance controls, and a dedicated live Effects page.

> Tested on: Zynthian **Vangelis** (stable 2610), Raspberry Pi 5, V5 kit, 800×480 touchscreen.
> Other versions/hardware are untested.

## Features

- **Independent L/R Delay (`5..2500 ms`) with BPM Sync:** smooth Hermite-interpolated tape-style repitch; in `Sync` mode locks to Zynthian's transport BPM (`1/64 .. 1 bar`, including triplets and dotted divisions).
- **4 Delay Characters (`Mode`):**
  1. `Clean` — transparent digital stereo delay.
  2. `Tape` — warm asymmetric saturation, DC blocker, organic wow/flutter drift, and critically-damped tape motor inertia for musical pitch glides when sweeping delay time.
  3. `BBD` — darker analog bucket-brigade repeats with smooth roll-off and compression.
  4. `Diff` — 4-stage decorrelated all-pass diffuser in the feedback loop for ambient clouds.
- **3 Stereo Routings:** `Stereo`, `PingPong`, and `Cross` feedback + continuous `Width` (`0..100%`).
- **Feedback Loop Shaping & Motion:** Butterworth `HPF` (`20..1000 Hz`) & `LPF` (`500..20000 Hz`), `Drive`, `Diffuse`, quadrature LFO `Mod Rate`/`Mod Depth`, and input-triggered `Duck` (`0..100%` up to `-24 dB` with adjustable `Duck Rel`).
- **Live Performance Controls:** lossless `Freeze`, dub-throw `Trails` (`Mute In`), `Output` trim, and click-free `Bypass`.
- **10 Factory Presets** (*Factory* bank): `Init Quarter`, `Dotted Eighth Edge`, `Warm Space Tape`, `Analog BBD Slap & Tail`, `Golden Ratio Ping-Pong`, `Ducking Lead Echo`, `Cloudy Diffused Wash`, `Dub Siren Throw`, `Polyrhythm 3 vs 4`, and `Infinite Freeze Pad`.
- **Real-time safe:** zero allocations in `run()`, denormal/NaN-safe, click-free parameter smoothing.

## Installation

On Zynthian (SSH as root):

```bash
git clone https://github.com/dmtry3/zynthian-eka.git
cd zynthian-eka/plugins/echo
sudo make install
systemctl restart zynthian
```

Update in one line:

```bash
cd /root/zynthian-eka && git pull && sudo make install-echo && systemctl restart zynthian
```

| Link or folder | Contents |
|---|---|
| `/usr/local/lib/lv2/eka-echo.lv2` | LV2 bundle (`eka_echo.so`, `manifest.ttl`, `eka_echo.ttl`, `presets.ttl`) |
| `zyngui/widgets_user/zynthian_widget_eka_echo.py` | Touch widget (`ui/`) |
| `zynthian-my-data/presets/lv2/Eka_Echo.presets.lv2` | User presets folder |

`install.sh` refreshes Zynthian's preset cache and sets the category to **Delay**. To remove: `sudo make uninstall`.

If adding for the first time, enable **Eka Echo** in Zynthian webconf (*Software → LV2 Plugins*), then add it to a chain as an *Audio Effect*.

## Controls

Six pages of 4 encoders:

| Page | Knob 1 | Knob 2 | Knob 3 | Knob 4 |
|---|---|---|---|---|
| **1. Time** | `Time L` (`5..2500 ms`) | `Time R` (`5..2500 ms`) | `Feedback` (`0..110%`) | `Mix` (`0..100%`) |
| **2. Effects** | `Time FX` (`-100..+100%`, 0 = original) | `Time Link` (`Off / On`) | `Filter FX` (`-100..+100%`) | `Wash FX` (`0..100%`) + `FX Zero` (MIDI reset) |
| **3. Mode & Sync** | `Mode` (`Clean/Tape/BBD/Diff`) | `Routing` (`Stereo/PingPong/Cross`) | `Sync` (`Free / Sync`) | `Width` (`0..100%`) |
| **4. Tone** | `HPF` (`20..1000 Hz`) | `LPF` (`500..20000 Hz`) | `Drive` (`0..100%`) | `Diffuse` (`0..100%`) |
| **5. Motion** | `Mod Rate` (`0.05..8 Hz`) | `Mod Depth` (`0..100%`) | `Duck` (`0..100%`) | `Duck Rel` (`50..1000 ms`) |
| **6. Perform** | `Freeze` (`Off / On`) | `Trails` (`Send On / Mute In`) | `Output` (`-18..+12 dB`) | `Bypass` (`Off / On`) |

## Widget & Gestures

- **Tap Map (top):** drag `L` (upper half) or `R` (lower half) left/right to set delay time (moves both channels proportionally when `LINK` is on; snaps to note divisions when `Sync` is on) and up/down to set `Feedback`; switches encoders to `Time`.
- **Loop Filter bar (middle):** drag left/right edges to adjust `HPF` / `LPF`; switches encoders to `Tone`.
- **Dedicated `FX` screen:** tap `FX` on the bottom bar to open the live **Effects** touch screen (and switch encoders to `Effects`) with a large **X-Y pad** (`X = TIME FX`, `-100..0..+100%`; `Y = FILTER FX`, down LPF / up HPF), bottom `WASH FX` slider with `0` reset, `HOLD / SNAP 0` toggle (spring-return to zero on release), `LINK / L ONLY`, and `ZERO 0` (reset all without losing original `Time L`/`Time R`).
- **Bottom 6 buttons:** tap `MODE`, `ROUTE`, `SYNC`, `FX`, `FREEZE`, `TRAILS` (Mute In); **long press `TRAILS`** toggles full `Bypass` and grays out the UI.

## Building & Testing on Linux / WSL

```bash
make -C plugins/echo                 # build LV2 bundle
make -C plugins/echo test            # DSP core, LV2 host and widget tests
SAN=1 make -C plugins/echo test      # with AddressSanitizer + UBSan
```

## License

GPL-3.0-or-later. See [LICENSE](LICENSE). Changes: [CHANGELOG.md](CHANGELOG.md).
