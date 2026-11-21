# Eka Press

Single-band live compressor LV2 plugin for [Zynthian](https://zynthian.org) (Raspberry Pi 5, V5 touch screen).

- **LV2 URI:** `http://dmtry3.github.io/zynthian-eka/press`
- **4 modes:** `Clean` (VCA), `Opto` (optical program-dependent release), `Punch` (fast FET bite), `Glue` (bus VCA)
- **Sidechain:** 2nd-order Butterworth `SC HPF` (`20..500 Hz`), `Stereo Link` (`0..100%`), `Auto Release`, `SC Listen`
- **Color & Output:** analog `Drive`, `Makeup` + `Auto Makeup`, parallel `Mix` (`0..100%`), `-0.3 dBFS` `Soft Clip` ceiling, `Output` trim
- **Touch widget:** interactive Transfer Curve (drag for Threshold & Ratio), scrolling Input/Output/GR timeline, IN/GR/OUT peak meters, mode & toggle buttons
- **10 factory presets**

## Build & Install

```bash
make                # build eka-press.lv2
make test           # run DSP, LV2 host and widget tests
sudo make install   # install on Zynthian
```
