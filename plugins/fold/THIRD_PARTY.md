# Third-party code in Eka Fold

Eka Fold is GPL-3.0-or-later. It contains code and data under the MIT license, which allows this.

## pichenettes/eurorack (MIT)

Source: https://github.com/pichenettes/eurorack (the `plaits` folder), commit f4cfa76 (2026-10-12).

Used in `src/plaits/`:

| File | What it takes | Changes |
|---|---|---|
| `src/plaits/tables.cc` | Two tables of `plaits/resources.cc`: the FM ratio quantizer and the integrated waves of the wavetable engine | None to the values; new names, C linkage |
| `src/plaits/engines.cc` | The algorithms of `plaits/dsp/engine/virtual_analog_engine.cc` (variant 0), `fm_engine.cc`, `wavetable_engine.cc`, `additive_engine.cc`, `plaits/dsp/engine2/phase_distortion_engine.cc`, `plaits/dsp/oscillator/variable_shape_oscillator.h`, `wavetable_oscillator.h`, `harmonic_oscillator.h` | One sample at a time with a signed phase increment (through-zero FM) at any rate, external hard sync; VA band-limited by stateless polyBLEP / polyBLAMP; FM2 without 4x oversampling and the sub output; Wave from the slope of the integral between two phases; Harm (additive): the 24 integer harmonics only, as one Chebyshev sum of the sine of the phase, cut at 24 kHz, amplitudes gliding over 20 ms; PD: the synced triangle computed from the master phase, band-limited by polyBLAMP, the amount held under 18 kHz, no aux output |
| `src/plaits/engines.h` | The C interface of the above | New |

The build writes the two tables again into `build/eka-fold.lv2/engine_tables.bin` for the widget, which draws
the waves from them.

```
Copyright 2016 Emilie Gillet.

Author: Emilie Gillet (emilie.o.gillet@gmail.com)

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.

See http://creativecommons.org/licenses/MIT/ for more information.
```
