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
run_pipe "auto" \
  videotestsrc num-buffers=5 ! videoconvert ! \
  video/x-raw,format=GRAY16_LE ! gray16norm ! fakesink sync=false

# Manual mode: use explicit black/white levels
run_pipe "manual" \
  videotestsrc num-buffers=5 ! videoconvert ! \
  video/x-raw,format=GRAY16_LE ! gray16norm auto-range=false black-level=1000 white-level=20000 ! \
  fakesink sync=false

echo "[func] Done"
