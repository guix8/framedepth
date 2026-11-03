#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 FrameDepth contributors
# The sources of the third-party code a release binary is built from, as one
# archive to publish next to the release (GPL-3.0 section 6): Moonlight XR for
# Frame and ncnn with glslang at the commits scripts/fetch-deps.sh pins, the
# TensorFlow 2.21.0 source archive, the libraries LiteRT fetches while it
# configures (scripts/fetch-litert-sources.sh) and the OpenVR header, each
# with its license. FrameDepth's own source is the repository at the
# release's tag. Run after scripts/fetch-deps.sh and
# scripts/fetch-litert-sources.sh (the build need not have run):
#   bash packaging/make-source-bundle.sh   -> dist/framedepth-VERSION-deps-src.tar.gz (+ .sha256)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
version=$(sed -n 's/^ *"version": *"\([^"]*\)".*/\1/p' extension/manifest.json)
[[ -n $version ]] || { echo "no version in extension/manifest.json" >&2; exit 1; }
name="framedepth-$version-deps-src"
for need in build-deps/moonlight-xr/.git build-deps/ncnn/.git build-deps/ncnn/glslang/.git \
            build-deps/downloads/tensorflow-2.21.0.tar.gz build-deps/downloads/openvr-2.12.14.h build-deps/downloads/openvr-2.12.14-LICENSE; do
  [[ -e $need ]] || { echo "missing $need: run scripts/fetch-deps.sh first" >&2; exit 1; }
done
lb=build-deps/litert-sources/build
[[ -f $lb/CMakeCache.txt ]] || { echo "missing $lb: run scripts/fetch-litert-sources.sh first" >&2; exit 1; }
stage=$(mktemp -d)
trap 'rm -rf -- "$stage"' EXIT
out="$stage/$name"
mkdir -p "$out/openvr"
# the checked-out commits only (git archive: tracked files, no .git or build output)
git -C build-deps/moonlight-xr archive --format=tar --prefix=moonlight-xr/ HEAD | tar -x -C "$out"
git -C build-deps/ncnn archive --format=tar --prefix=ncnn/ HEAD | tar -x -C "$out"
git -C build-deps/ncnn/glslang archive --format=tar --prefix=ncnn/glslang/ HEAD | tar -x -C "$out"
cp build-deps/downloads/tensorflow-2.21.0.tar.gz "$out/"
# LiteRT's libraries: git checkouts at their pinned commits (tracked files),
# and the archives XNNPACK and LiteRT download by URL (as unpacked)
mkdir -p "$out/litert-deps"
litert_list=""
for d in "$lb"/*/; do
  d=${d%/}; n=$(basename "$d")
  if [[ -e $d/.git ]]; then
    git -C "$d" archive --format=tar --prefix="litert-deps/$n/" HEAD | tar -x -C "$out"
    litert_list+="    $n at $(git -C "$d" rev-parse HEAD)"$'\n'
  elif [[ $n == *-source || $n == fft2d ]]; then
    tar -C "$lb" --exclude=.git -cf - "$n" | tar -x -C "$out/litert-deps"
    litert_list+="    $n (downloaded archive)"$'\n'
  fi
done
cp build-deps/downloads/openvr-2.12.14.h "$out/openvr/openvr.h"
cp build-deps/downloads/openvr-2.12.14-LICENSE "$out/openvr/LICENSE"
cat > "$out/README.txt" <<EOF
Third-party sources of FrameDepth $version (a Steam Frame build), as pinned
by scripts/fetch-deps.sh of the FrameDepth repository at tag v$version
(https://github.com/guix8/framedepth):

  moonlight-xr/   Moonlight XR for Frame at $(git -C build-deps/moonlight-xr rev-parse HEAD) (GPL-3.0)
  ncnn/           ncnn at $(git -C build-deps/ncnn rev-parse HEAD), glslang at
                  $(git -C build-deps/ncnn/glslang rev-parse HEAD) (BSD-3-Clause and others, see each)
  tensorflow-2.21.0.tar.gz  TensorFlow 2.21.0 (Apache-2.0), from which LiteRT is built
  litert-deps/    the libraries LiteRT's CMake fetches and links, as fetched
                  (each with its own license; Apache-2.0, BSD, MIT, MPL-2.0):
$litert_list  openvr/         OpenVR 2.12.14 header (BSD-3-Clause)

LiteRT is built from tensorflow-2.21.0 by Moonlight XR's
depth/tools/build-litert.sh, as FrameDepth's scripts/build.sh runs it.
EOF
mkdir -p dist
tar --sort=name --owner=0 --group=0 --numeric-owner -C "$stage" -czf "dist/$name.tar.gz" "$name"
(cd dist && sha256sum "$name.tar.gz" > "$name.tar.gz.sha256")
echo "dist/$name.tar.gz ($(du -h "dist/$name.tar.gz" | cut -f1))"
