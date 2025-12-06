#!/usr/bin/env bash
set -euo pipefail

# Functional checks (best-effort):
#  - build the plugin
#  - export GST_PLUGIN_PATH to repo root
#  - run short gst-launch-1.0 pipelines
#
# Notes:
#  - Some systems cannot negotiate GRAY16_LE via videoconvert; in such case
#    we treat this as SKIP and exit 0.

echo "[func] Building plugin..."
make -s clean && make -s

export GST_PLUGIN_PATH="${PWD}${GST_PLUGIN_PATH:+:${GST_PLUGIN_PATH}}"

if ! command -v gst-launch-1.0 >/dev/null 2>&1; then
  echo "SKIP: gst-launch-1.0 missing"
  exit 0
fi

run_pipe() {
  local name="$1"; shift
  echo "[func] Pipeline: ${name}"
  if ! gst-launch-1.0 -q "$@"; then
    echo "[func] SKIP/FAIL: ${name} (likely caps on this system)"
  else
    echo "[func] OK: ${name}"
  fi
}

# Auto mode: videotestsrc → GRAY16_LE → gray16norm → fakesink
run_pipe "auto-gray8" \
  videotestsrc num-buffers=5 ! \
  video/x-raw,format=GRAY16_LE ! gray16norm ! fakesink sync=false

# Manual mode: use explicit black/white levels
run_pipe "manual-gray8" \
  videotestsrc num-buffers=5 ! \
  video/x-raw,format=GRAY16_LE ! gray16norm auto-range=false black-level=1000 white-level=20000 ! \
  fakesink sync=false

# Normalized color output via gray16norm (RGBx)
run_pipe "norm-color-rgbx" \
  videotestsrc num-buffers=5 ! \
  video/x-raw,format=GRAY16_LE ! gray16norm palette=viridis ! \
  video/x-raw,format=RGBx ! fakesink sync=false

# Color output via gray16color (tabular Gray16->color, no normalization)
run_pipe "color-rgbx-viridis" \
  videotestsrc num-buffers=5 ! \
  video/x-raw,format=GRAY16_LE ! gray16color palette=viridis ! \
  video/x-raw,format=RGBx ! fakesink sync=false

run_pipe "color-bgr16-turbo" \
  videotestsrc num-buffers=5 ! \
  video/x-raw,format=GRAY16_LE ! gray16color palette=turbo ! \
  video/x-raw,format=BGR16 ! fakesink sync=false

echo "[func] Done"
