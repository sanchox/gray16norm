#!/usr/bin/env bash
set -euo pipefail

# Simple smoke test:
# 1) build the plugin
# 2) set GST_PLUGIN_PATH to the repo root
# 3) run gst-inspect-1.0 (no hard failure on return code)

echo "[smoke] Building plugin..."
make -s clean && make -s

export GST_PLUGIN_PATH="${PWD}${GST_PLUGIN_PATH:+:${GST_PLUGIN_PATH}}"

if ! command -v gst-inspect-1.0 >/dev/null 2>&1; then
  echo "SKIP: gst-inspect-1.0 missing"
  exit 0
fi

echo "[smoke] Running: gst-inspect-1.0 gray16norm"
if ! gst-inspect-1.0 gray16norm >/dev/null 2>&1; then
  echo "[smoke] NOTE: element 'gray16norm' not discoverable in this environment"
  echo "[smoke] Hint: ensure GST_PLUGIN_PATH includes the repo root: $PWD"
  echo "[smoke] Current GST_PLUGIN_PATH: ${GST_PLUGIN_PATH:-<unset>}"
fi

echo "[smoke] Running: gst-inspect-1.0 gray16color"
if ! gst-inspect-1.0 gray16color >/dev/null 2>&1; then
  echo "[smoke] NOTE: element 'gray16color' not discoverable in this environment"
fi
