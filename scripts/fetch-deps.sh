#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 FrameDepth contributors
# Fetch the pinned sources the companion builds from, into build-deps/.
# Runs on the headset (or any Linux host with git, curl, sha256sum).
#   build-deps/moonlight-xr   Moonlight XR for Frame at a fixed commit (depth
#                             engine and ZipDepth/LiteRT assets; unmodified)
#   build-deps/downloads      TensorFlow (LiteRT) source archive, OpenVR header and license
#   build-deps/ncnn           ncnn at a fixed tag, with glslang
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

MOONLIGHT_URL=https://github.com/simplysanatori/moonlight-xr-steam-frame.git
MOONLIGHT_REV=f00500686968dfce0173f0e2a7cf0c75da0bb0ff
TF_URL=https://github.com/tensorflow/tensorflow/archive/refs/tags/v2.21.0.tar.gz
# GitHub tag archive, first observed 2026-10-24 (upstream's script pins no hash)
TF_SHA256=ef3568bb4865d6c1b2564fb5689c19b6b9a5311572cd1f2ff9198636a8520921
OPENVR_URL=https://raw.githubusercontent.com/ValveSoftware/openvr/v2.12.14/headers/openvr.h
OPENVR_SHA256=0c217387163b6e2b7712a9bb650085f4dc8546041270bb3e2d726901a025dc93
OPENVR_LICENSE_URL=https://raw.githubusercontent.com/ValveSoftware/openvr/v2.12.14/LICENSE
OPENVR_LICENSE_SHA256=f56ff606104d4ef18e617921a75c73ad73b5a1a1d70c69590c29de16919e04ad
NCNN_URL=https://github.com/Tencent/ncnn.git
NCNN_TAG=20260526
NCNN_REV=e54f7b1f88434e1d844ea0551b880a1cfb079ce1

fetch() { # url file sha256
  local dest="build-deps/downloads/$2"
  if [[ ! -f $dest ]]; then
    curl --fail --location --silent --show-error --output "$dest.part" "$1" || { rm -f -- "$dest.part"; exit 1; }
    mv "$dest.part" "$dest"
  fi
  # a wrong file is removed, so the next run downloads it again
  printf '%s  %s\n' "$3" "$dest" | sha256sum --check --quiet || { echo "$dest: wrong checksum, removed" >&2; rm -f -- "$dest"; exit 1; }
}
mkdir -p build-deps/downloads build-deps/openvr
fetch "$TF_URL" tensorflow-2.21.0.tar.gz "$TF_SHA256"
fetch "$OPENVR_URL" openvr-2.12.14.h "$OPENVR_SHA256"
fetch "$OPENVR_LICENSE_URL" openvr-2.12.14-LICENSE "$OPENVR_LICENSE_SHA256"
cp build-deps/downloads/openvr-2.12.14.h build-deps/openvr/openvr.h

# A clone that fails halfway is removed, so the next run starts it again
ml=build-deps/moonlight-xr
if [[ ! -d $ml/.git ]]; then
  { git -c core.hooksPath=/dev/null init -q "$ml" &&
    git -C "$ml" -c core.hooksPath=/dev/null fetch -q --depth 1 "$MOONLIGHT_URL" "$MOONLIGHT_REV" &&
    git -C "$ml" -c core.hooksPath=/dev/null checkout -q --detach FETCH_HEAD; } || { rm -rf -- "$ml"; exit 1; }
fi
[[ $(git -C "$ml" rev-parse HEAD) == "$MOONLIGHT_REV" ]] || { echo "moonlight-xr: not at $MOONLIGHT_REV" >&2; exit 1; }

nc=build-deps/ncnn
if [[ ! -d $nc/.git ]]; then
  { git -c core.hooksPath=/dev/null clone -q --depth 1 --branch "$NCNN_TAG" "$NCNN_URL" "$nc" &&
    git -C "$nc" -c core.hooksPath=/dev/null submodule update -q --init --depth 1 glslang; } || { rm -rf -- "$nc"; exit 1; }
fi
[[ $(git -C "$nc" rev-parse HEAD) == "$NCNN_REV" ]] || { echo "ncnn: not at $NCNN_REV" >&2; exit 1; }
# glslang at the commit ncnn's tag records (a leading space: checked out, matching)
[[ $(git -C "$nc" submodule status glslang) == " "* ]] || { echo "ncnn: glslang is missing or not at ncnn's commit" >&2; exit 1; }
echo "Fetched: moonlight-xr $MOONLIGHT_REV, ncnn $NCNN_TAG, TensorFlow 2.21.0, OpenVR 2.12.14 header"
