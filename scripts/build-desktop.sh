#!/usr/bin/env bash
# Desktop tools: kp_replay (recordings), kp_live (adapter plugged into this PC), kp_sim, kp_gen. Run inside nix-shell.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cmake -S "$ROOT/native" -B "$ROOT/build/desktop" -G Ninja -DKEPYUR_RADIO=ON >/dev/null
ninja -C "$ROOT/build/desktop" kp_replay kp_live kp_sim kp_gen
# the app's renderer off screen: only where EGL + OpenGL ES 3 development files are installed
if ninja -C "$ROOT/build/desktop" -t targets all | grep -q '^kp_render:'; then ninja -C "$ROOT/build/desktop" kp_render; fi
