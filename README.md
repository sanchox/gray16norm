# GstGray16Norm

GstGray16Norm is a GStreamer 1.0 video filter plugin that converts GRAY16_LE frames to GRAY8 with normalization. It supports:

- Auto range normalization: per-frame min/max mapping to 0–255.
- Manual levels: map a given black-level to 0 and white-level to 255.

Element factory name: `gray16norm`

Plugin metadata (from source):
- Version: 1.0
- License: LGPL-2.1-or-later
- Category: Filter/Effect/Video
- Description: "Normalize GRAY16 to GRAY8 with auto or manual range"
- Debug category: `gray16norm`

## Stack

- Language: C
- Framework: GStreamer 1.0 (gst-video, gst-base)
- Build system: Makefile (gcc + pkg-config)
- Package manager(s): system packages via apt (example below)

### SIMD/vectorization

The filter provides multiple accelerated implementations, selected at compile time/runtime by the compiler and target:

- AArch64 NEON: hand-written intrinsics for min/max scanning and normalization.
- x86/x86_64 SSE2: hand-written intrinsics for min/max scanning and normalization.
- GCC/Clang Vector Extensions: a portable `vector_size(16)` path for auto min/max scanning that lets the compiler generate appropriate vector instructions for the target when possible. If hardware/flags do not support vectorization, the compiler may scalarize the code while keeping it correct.
- Scalar fallback: always available.

Priority of paths: NEON → SSE2 → GNU Vector Extensions → scalar.

## Requirements

Build-time:
- GCC (or compatible C compiler)
- pkg-config
- GStreamer 1.0 development headers and libraries: `gstreamer-1.0`, `gstreamer-video-1.0`

Runtime:
- GStreamer 1.0 core and base plugins

Debian/Ubuntu:
```bash
sudo apt update
sudo apt install build-essential pkg-config \
    libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
    gstreamer1.0-plugins-base gstreamer1.0-tools
```

## Build

The project is built into a shared object GStreamer plugin `libgstgray16norm.so`.

```bash
make            # builds libgstgray16norm.so
make clean      # removes objects and the built .so
make debug      # rebuilds with -O0 -g and ASan/UBSan
```

## Install

By default, `make install` installs the plugin to a user-local directory:

- Install dir (from Makefile): `~/.local/lib/gstreamer-1.0`

```bash
make install
```

GStreamer may not scan your user-local install dir by default. If `gst-inspect-1.0 gray16norm` cannot find the element after installing, set `GST_PLUGIN_PATH` to include that directory:

```bash
export GST_PLUGIN_PATH="$HOME/.local/lib/gstreamer-1.0${GST_PLUGIN_PATH:+:$GST_PLUGIN_PATH}"
```

Uninstall:
```bash
make uninstall
```

## Verify installation and quick smoke test

Quick smoke test without installing (uses the build dir):
```bash
make -s clean && make -s
export GST_PLUGIN_PATH="$PWD${GST_PLUGIN_PATH:+:$GST_PLUGIN_PATH}"
gst-inspect-1.0 gray16norm
```

Alternatively, use the Make target:
```bash
make test-smoke
```

Enable debug logs for this plugin:
```bash
GST_DEBUG=gray16norm:4 gst-inspect-1.0 gray16norm
```

## Usage

The element accepts `video/x-raw,format=GRAY16_LE` on sink and outputs `video/x-raw,format=GRAY8` on src.

Properties:
- `auto-range` (boolean, default: true) — compute per-frame min/max
- `black-level` (uint16, default: 0) — used when `auto-range=false`
- `white-level` (uint16, default: 65535) — used when `auto-range=false`

Example pipelines (may require that your GStreamer build supports GRAY16_LE in `videoconvert`):

- Auto normalization (default):
```bash
gst-launch-1.0 -v videotestsrc ! videoconvert ! \
  video/x-raw,format=GRAY16_LE ! gray16norm ! \
  videoconvert ! autovideosink
```

- Manual levels (map [1000, 20000] to [0, 255]):
```bash
gst-launch-1.0 -v videotestsrc ! videoconvert ! \
  video/x-raw,format=GRAY16_LE ! gray16norm auto-range=false \
  black-level=1000 white-level=20000 ! videoconvert ! autovideosink
```

If you need to test with a file that contains 16-bit grayscale frames, adjust the caps accordingly, for example:
```bash
gst-launch-1.0 -v filesrc location=your_input.raw ! \
  videoparse width=<W> height=<H> format=gray16-le framerate=30/1 ! \
  gray16norm ! videoconvert ! autovideosink
```

## Scripts and Make targets

Makefile targets:
- `all` (default): build the plugin
- `install`: install the `.so` to `~/.local/lib/gstreamer-1.0`
- `uninstall`: remove the installed `.so` from that directory
- `clean`: remove objects and outputs
- `debug`: rebuild with ASan/UBSan and debug info
- `test`: run the smoke test (see tests/smoke.sh)
- `test-smoke`: run the smoke test explicitly
- `test-functional`: run short gst-launch-1.0 pipelines (best-effort)
- `test-all`: run both smoke and functional tests

## Environment variables

- `GST_PLUGIN_PATH` — ensure it includes the build dir or the install directory if GStreamer does not find the plugin automatically.
- `GST_DEBUG` — set to `gray16norm:LEVEL` (e.g., `gray16norm:4`) to see plugin logs.
- Optional: `GST_DEBUG_FILE=debug.log` to write logs into a file.

## Project structure

```
.
├── Makefile              # build/install targets
├── README.md             # this document
├── gstgray16norm.c       # GStreamer element implementation
└── tests/                # lightweight shell tests
    ├── smoke.sh          # builds and runs gst-inspect-1.0 gray16norm
    └── functional.sh     # short gst-launch-1.0 pipelines (may SKIP on some systems)
```

## Conventions

- All source code comments and commit messages should be written in English.
- Follow the existing code style; see `.editorconfig` and `.clang-format` for formatting rules. Indentation: 2 spaces, no tabs (except Makefile).

## Performance and optimizations

- Two-pass, allocation-free algorithm:
  - Auto mode (default): first pass finds min/max, second pass normalizes.
  - Manual mode: full-range [0, 65535] uses a fast path `v >> 8`.
- Fixed-point instead of float on the hot path:
  - Scalar path uses Q32.32 (accuracy, portability).
  - AArch64 NEON path uses Q8 scaling with rounding (significantly faster; difference vs Q32.32 is within ±1 LSB; saturation ensures [0..255]).
- Hardware-specific optimizations for i.MX8MP (Cortex-A53, aarch64):
  - Auto min/max scan and normalization are vectorized with ARM NEON (enabled automatically on aarch64).
  - For armv7 with NEON, you may enable NEON via compiler flags (see below).

Note: earlier README versions mentioned a LUT optimization and `transform_size`. They are not used in the current code. A LUT for manual mode can be added later as a separate optimization if needed.

## Tests

This repo contains small, hermetic shell tests under `tests/`:

- Smoke test: verifies discovery via gst-inspect-1.0
  - `make test` or `make test-smoke`
  - Does: builds the plugin, sets `GST_PLUGIN_PATH` to the repo root, runs `gst-inspect-1.0 gray16norm`.
- Functional tests: short pipelines with `gst-launch-1.0` (best-effort)
  - `make test-functional`
  - Note: some environments cannot negotiate `GRAY16_LE` with `videoconvert` — such cases are treated as SKIP, not a failure.
- All tests: `make test-all`

Environment isolation:
- Tests do not install the plugin system-wide; they export `GST_PLUGIN_PATH` to point at the repository root.

## NEON build notes

- aarch64 (e.g., i.MX8): NEON is mandatory and enabled by default; no extra flags are required.
- armv7 (32-bit ARM): ensure the compiler enables NEON, for example:
  ```bash
  make CFLAGS+=" -mfpu=neon -mfloat-abi=hard "
  ```
  If NEON is not available, the plugin automatically falls back to the scalar path.

## Code style and linters

This project follows common GStreamer C conventions. We approximate gst-indent formatting via clang-format using a GNU-like profile with 2-space indentation.

- Formatting:
  - Configuration: `.clang-format` (GNU-like, 2 spaces), `.editorconfig` (LF, final newline, 2 spaces; Makefile uses tabs).
  - Commands:
    - `make format` — format sources using clang-format.
    - `make check-format` — verify formatting without modifying files.
- Linting:
  - Configuration: `.clang-tidy` tuned for C (clang-analyzer, bugprone, cert C checks).
  - Command: `make lint` — runs clang-tidy with pkg-config CFLAGS for GStreamer.
- Combined:
  - `make style` — runs format check and lint.

Important: All source code comments, commit messages, and this README must be written in English only.

## License

License: LGPL-2.1-or-later (see source headers, `GST_PLUGIN_DEFINE`, and the `LICENSE` file).

## Notes and Caveats

- If your environment expects another plugin directory, adjust `INSTALL_DIR` in the Makefile or set `GST_PLUGIN_PATH` accordingly.
- The example pipelines assume your GStreamer build can convert to/from `GRAY16_LE` with `videoconvert`.

### Troubleshooting

- If the plugin isn’t discovered, print the search path/registry information:
  ```bash
  GST_DEBUG=GST_REGISTRY:6 gst-inspect-1.0 gray16norm |& sed -n '1,120p'
  ```
- Remove the registry cache if you changed install paths: `rm -f ~/.cache/gstreamer-1.0/registry.*`
- Increase plugin logs: `GST_DEBUG=gray16norm:6` and optionally `GST_DEBUG_FILE=debug.log`.
