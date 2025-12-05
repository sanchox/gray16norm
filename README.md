### GstGray16Norm — GStreamer 1.0 plugin for 16‑bit grayscale normalization and coloring

This repository provides a GStreamer plugin with elements to normalize 16‑bit grayscale video to GRAY8 and to map 16‑bit grayscale to RGB using palette LUTs.

## Quick start

Build and run a simple preview pipeline:

```bash
make
export GST_PLUGIN_PATH="$PWD${GST_PLUGIN_PATH:+:$GST_PLUGIN_PATH}"
gst-launch-1.0 -v videotestsrc num-buffers=30 ! videoconvert ! \
  video/x-raw,format=GRAY16_LE ! gray16norm ! videoconvert ! autovideosink
```

Manual levels example (normalized GRAY8 via fixed range):

```bash
gst-launch-1.0 -v videotestsrc ! videoconvert ! \
  video/x-raw,format=GRAY16_LE ! gray16norm auto-range=false \
  black-level=1000 white-level=20000 ! videoconvert ! autovideosink
```

Play from a raw 16‑bit file:

```bash
gst-launch-1.0 -v filesrc location=your_input.raw ! \
  videoparse width=<W> height=<H> format=gray16-le framerate=30/1 ! \
  gray16norm ! videoconvert ! autovideosink
```

## Elements

### gray16norm (normalize GRAY16_LE to GRAY8 or color using a palette)

- Sink caps: `video/x-raw,format=GRAY16_LE`
- Src caps:
  - `video/x-raw,format=GRAY8`
  - `video/x-raw,format={ RGBx, BGRx, RGBA, BGRA, RGB16, BGR16, RGB15 }`

Key properties:
- `auto-range` (bool, default: true) — per‑frame min/max auto scaling.
- `black-level`/`white-level` (uint) — manual scaling window when `auto-range=false`.
- `palette` (string, default: `turbo`) — one of: `turbo`, `viridis`, `magma`, `jet`, `prism` (applies to RGB output modes).
- `alpha` (uint, 0..255, default: 255) — used for `RGBA/BGRA`.

Colorized output example (per‑frame auto range, RGBx):

```bash
gst-launch-1.0 -v videotestsrc ! videoconvert ! \
  video/x-raw,format=GRAY16_LE ! gray16norm palette=viridis ! \
  video/x-raw,format=RGBx ! fakesink sync=false
```

Manual levels, BGR output:

```bash
gst-launch-1.0 -v videotestsrc ! videoconvert ! \
  video/x-raw,format=GRAY16_LE ! gray16norm auto-range=false black-level=1000 white-level=20000 palette=turbo ! \
  video/x-raw,format=BGR ! fakesink sync=false
```

### gray16color (direct Gray16 → RGB via LUT, without normalization)

Direct table mapping `dst = LUT[src16]` using a 65,536‑entry palette LUT, packed into the selected output format.

- Sink caps: `video/x-raw,format=GRAY16_LE`
- Src caps: `video/x-raw,format={ RGBx, BGRx, RGBA, BGRA, RGB16, BGR16, RGB15 }`

Properties:
- `palette` (string, default: `turbo`) — `turbo`, `viridis`, `magma`, `jet`, `prism`.
- `alpha` (uint, 0..255, default: 255) — for `RGBA/BGRA`.

Examples:
- RGBx (fast path on many platforms):
```bash
gst-launch-1.0 -v videotestsrc num-buffers=30 ! videoconvert ! \
  video/x-raw,format=GRAY16_LE ! gray16color palette=viridis ! \
  video/x-raw,format=RGBx ! fakesink sync=false
```
- BGR16 (RGB565):
```bash
gst-launch-1.0 -v videotestsrc num-buffers=30 ! videoconvert ! \
  video/x-raw,format=GRAY16_LE ! gray16color palette=turbo ! \
  video/x-raw,format=BGR16 ! fakesink sync=false
```

Note on H.264/H.265 encoders: many HW encoders expect YUV (`NV12/NV21/I420`). Use `gray16color` → `videoconvert` to convert RGBx/BGRx to the required YUV format.

### gray16window (bit‑window to GRAY8)

`gray16window` extracts an 8‑bit window from 16‑bit grayscale by detecting the most significant changing bit across the frame and right‑shifting all pixels so that up to 8 informative bits remain. Useful when the signal occupies a narrow bit range and you want a quick GRAY8 view without choosing manual levels.

- Sink caps: `video/x-raw,format=GRAY16_LE`
- Src caps: `video/x-raw,format=GRAY8`

Example:

```bash
gst-launch-1.0 -v videotestsrc num-buffers=30 ! videoconvert ! \
  video/x-raw,format=GRAY16_LE ! gray16window ! \
  videoconvert ! autovideosink
```

Enable element‑specific logs if needed:

```bash
GST_DEBUG=gray16window:4 gst-inspect-1.0 gray16window
```

## LUT generation

LUT headers (65,536 RGB triplets per palette) are generated automatically at build time. This ensures all palettes are available for RGB output.

Generated files and symbols:
- `gray16_to_rgb_lut.h` → `gray16_to_rgb` (Turbo)
- `gray16_to_rgb_lut_viridis.h` → `gray16_to_rgb_viridis` (Viridis)
- `gray16_to_rgb_lut_magma.h` → `gray16_to_rgb_magma` (Magma)
- `gray16_to_rgb_lut_jet.h` → `gray16_to_rgb_jet` (Jet)
- `gray16_to_rgb_lut_prism.h` → `gray16_to_rgb_prism` (Prism; high‑contrast bands)

Manual regeneration (optional):

```bash
make generate-luts            # all palettes
python3 lut_gen.py --palette viridis
python3 lut_gen.py --palette magma
python3 lut_gen.py --palette jet
python3 lut_gen.py --palette prism
```

Notes:
- The build requires Python 3, NumPy, and Matplotlib. Install system‑wide or in a venv (e.g., `pip install numpy matplotlib`).
- Set `GST_DEBUG=gray16norm:4` to see transform logs.

## Build and install

Dependencies: GStreamer 1.0 and gstreamer‑video 1.0 development packages.

```bash
make                  # builds libgstgray16norm.so in the repo root
make install          # installs to $HOME/.local/lib/gstreamer-1.0 (default)
make uninstall        # removes the installed .so
make clean            # removes objects and outputs
make debug            # rebuild with ASan/UBSan and debug info
```

If GStreamer does not discover the plugin, set:

```bash
export GST_PLUGIN_PATH="$PWD${GST_PLUGIN_PATH:+:$GST_PLUGIN_PATH}"
# For installed plugin:
export GST_PLUGIN_PATH="$HOME/.local/lib/gstreamer-1.0${GST_PLUGIN_PATH:+:$GST_PLUGIN_PATH}"
```

## Tests

Lightweight shell tests live under `tests/` and avoid system‑wide install by exporting `GST_PLUGIN_PATH` to the repository root.

- `make test` / `make test-smoke` — build and run `gst-inspect-1.0 gray16norm`
- `make test-functional` — short `gst-launch-1.0` pipelines (best‑effort; may SKIP if caps negotiation for `GRAY16_LE` is unavailable)
- `make test-all` — run both

## SIMD backends and performance

Two portable acceleration paths are used:
- GNU Vector Extensions (GCC/Clang `vector_size` types) — default on x86 and non‑NEON targets.
- ARM NEON (aarch64/ARMv8).

Implementation notes:
- Two‑pass, allocation‑free algorithm in auto mode (min/max then normalize).
- Manual mode full‑range [0, 65535] uses a fast path `v >> 8`.
- Fixed‑point scaling on hot paths: scalar Q32.32; NEON path uses Q8 scaling with rounding (within ±1 LSB; saturates to [0..255]).

## NEON build notes

- aarch64: NEON is enabled by default; no extra flags required.
- armv7 (32‑bit): enable NEON if available, for example:

```bash
make CFLAGS+=" -mfpu=neon -mfloat-abi=hard "
```

If NEON is unavailable, the plugin falls back to scalar paths automatically.

## Environment variables

- `GST_PLUGIN_PATH` — include the build dir or install dir so GStreamer finds the plugin.
- `GST_DEBUG` — set to `gray16norm:LEVEL` (e.g., `gray16norm:4`) for plugin logs.
- Optional: `GST_DEBUG_FILE=debug.log` to save logs to a file.

## Project structure

```
.
├── Makefile              # build/install targets
├── README.md             # this document
├── gstgray16norm.c       # GRAY8 and RGB output implementation
├── gstgray16window.c     # GRAY16 → GRAY8 bit‑window extractor
├── gstgray16color.c      # direct GRAY16 → RGB via LUT
└── tests/                # shell tests
    ├── smoke.sh
    └── functional.sh
```

## Code style and linters

We follow common GStreamer C conventions. Formatting is approximated via clang‑format (GNU‑like profile, 2‑space indentation).

- Formatting:
  - `.clang-format`, `.editorconfig`
  - `make format`, `make check-format`
- Linting:
  - `.clang-tidy`
  - `make lint`
- Combined: `make style`

English‑only requirement: All documentation, comments, and commit messages must be in English.

## License

SPDX: LGPL-2.1-or-later. See source headers, `GST_PLUGIN_DEFINE`, and the `LICENSE` file.

## Troubleshooting

- If the plugin isn’t discovered, dump the registry/search path:

```bash
GST_DEBUG=GST_REGISTRY:6 gst-inspect-1.0 gray16norm |& sed -n '1,120p'
```

- Remove registry cache when changing install paths:

```bash
rm -f ~/.cache/gstreamer-1.0/registry.*
```

- Increase plugin logs:

```bash
GST_DEBUG=gray16norm:6
export GST_DEBUG_FILE=debug.log   # optional
```
