#!/usr/bin/env bash
# Apply patches/ to the wifi-sdr and devourer submodules, for building wifi-sdr outside kepyur's CMake (which
# does this itself: native/cmake/patches.cmake). Idempotent: a patch already in is left alone.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd -P)

apply() {  # submodule dir, patch, a file in it and a piece of text the patch adds to that file
  local dir patch=$2 marker_file=$3 marker=$4
  dir=$(cd "$1" && pwd -P)  # as it really is (a link resolved), like patches.cmake: the ceiling below is its parent
  # git must look for a repository in $dir and no further up: where $dir is a plain directory inside some other
  # repository (vendored, or unpacked from an archive in a checkout), git apply takes the patch's paths as
  # relative to that repository's top, skips every one and reports success, the reverse check included.
  local git=(env GIT_CEILING_DIRECTORIES="$(dirname "$dir")" git -C "$dir") into=()
  # A .git in $dir that leads to no repository (a copy of a checkout made without the top .git directory): git
  # refuses to work there. From the directory above, with no repository in sight from it, git apply is the same
  # plain patch tool and never looks at that file.
  if [ -e "$dir/.git" ] && ! "${git[@]}" rev-parse --git-dir >/dev/null 2>&1; then
    local above; above=$(dirname "$dir")
    git=(env GIT_CEILING_DIRECTORIES="$(dirname "$above")" git -C "$above")
    into=(--directory="$(basename "$dir")")
  fi
  if "${git[@]}" apply "${into[@]}" --reverse --check "$patch" 2>/dev/null; then
    echo "already applied: $(basename "$patch")"
  elif "${git[@]}" apply "${into[@]}" --check "$patch" 2>/dev/null; then
    "${git[@]}" apply "${into[@]}" "$patch"
    echo "applied $(basename "$patch") to $dir"
  else
    echo "error: $(basename "$patch") neither applies to nor is already in $dir (at commit" \
      "$("${git[@]}" rev-parse --short HEAD 2>/dev/null || echo '?')): the submodule pin and the patch are" \
      "out of step (patches/README.md)" >&2
    exit 1
  fi
  # not on git's word alone: the patched text has to be there
  if ! grep -qF -- "$marker" "$dir/$marker_file" 2>/dev/null; then
    echo "error: $(basename "$patch") is reported as applied in $dir, but $marker_file there does not have" \
      "\"$marker\", which the patch adds: apply it by hand (patches/README.md)" >&2
    exit 1
  fi
}

apply "$ROOT/third_party/wifi-sdr" "$ROOT/patches/wifi-sdr-0001-playrx-stream.patch" \
  src/receiver.h "double centre = 0;"
apply "$ROOT/third_party/wifi-sdr/external/devourer" "$ROOT/patches/devourer-0001-playrx-usb.patch" \
  src/UsbTransport.h "kCtrlDeadAfter"
