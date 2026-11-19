# Third-party components

## Bungee Basic (pitch-shifting backend)

- **Source:** https://github.com/bungee-audio-stretch/bungee
- **Revision:** `61f52bf34be577d189d388772ddfa3696dee77d8`
- **License:** MPL-2.0
- **Pinned build dependencies:**
  - Eigen `c29c800126982c561e8d0b9255dc65474cd98de3` (MPL-2.0 / BSD notices retained)
  - PFFFT `02fe7715a5bf8bfd914681c53429600f94e0f536` (UCAR / FFTPACK terms in `pffft.c`)

Source archives and extracted code are downloaded and verified by `scripts/build-bungee.py` (pinned URLs and SHA256 hashes) into ignored `build/bungee/` and linked statically with hidden symbols (`-Wl,--exclude-libs,ALL`). No system package installation is required.

Configuration and modifications applied by `scripts/build-bungee.py`:
- Uses Bungee Basic with standard quality (`log2SynthesisHopAdjust = 0`).
- `Stream.h` is patched to use FFT half-window input lookahead for `autoOut` and adds `resetLive()` to drain pending grains and reuse existing buffers without heap allocation.
- All Bungee, Eigen and PFFFT license notices are collected in `build/bungee/notices/` and installed inside the LV2 bundle.

## aubio (embedded YIN-fast pitch tracker subset)

- **Source:** https://aubio.org/pub/aubio-0.4.9.tar.bz2
- **Version:** `0.4.9` (SHA256 `d48282ae4dab83b3dc94c16cf011bcb63835c1c02b515490e1883049c3d1f3da`)
- **License:** GPL-3.0-or-later (`third_party/aubio/COPYING`, installed as `LICENSE-aubio.txt`)

An unmodified DSP-only subset is vendored under `third_party/aubio/` (YIN-fast, float/complex vectors, math/music/vector helpers, logging, FFT and bundled Ooura FFT8g). `third_party/aubio/SOURCES.json` records the upstream origin and SHA256 hash of every vendored file. Only `config.h` is project-written to select standard C headers and the portable Ooura FFT with no external runtime dependencies (no FFmpeg, BLAS, or FFTW).
