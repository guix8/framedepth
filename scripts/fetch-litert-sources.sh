#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 FrameDepth contributors
# The libraries LiteRT links (XNNPACK, abseil, Eigen, ruy, protobuf and the
# rest) come from GitHub and elsewhere while LiteRT's CMake configures, at
# versions TensorFlow 2.21.0 pins, and the build removes them afterwards.
# This runs that configure step alone, with the options scripts/build.sh
# uses (Moonlight XR's depth/tools/build-litert.sh), into
# build-deps/litert-sources/, so packaging/make-source-bundle.sh can include
# them. Nothing is built; it needs the network. Run after scripts/fetch-deps.sh:
#   bash scripts/fetch-litert-sources.sh
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
tf=build-deps/downloads/tensorflow-2.21.0.tar.gz
[[ -f $tf ]] || { echo "missing $tf: run scripts/fetch-deps.sh first" >&2; exit 1; }
w="$root/build-deps/litert-sources"
if [[ -f $w/build/CMakeCache.txt ]]; then echo "already there: $w"; exit 0; fi
rm -rf -- "$w"
mkdir -p "$w/src"
tar -xzf "$tf" -C "$w/src"
src="$w/src/tensorflow-2.21.0"
nice -n 10 cmake -G Ninja -S "$src/tensorflow/lite/c" -B "$w/build" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DTENSORFLOW_SOURCE_DIR="$src" -DTFLITE_C_BUILD_SHARED_LIBS=OFF \
  -DTFLITE_ENABLE_XNNPACK=ON -DTFLITE_ENABLE_GPU=OFF -DTFLITE_ENABLE_NNAPI=OFF \
  -DTFLITE_ENABLE_EXTERNAL_DELEGATE=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DABSL_PROPAGATE_CXX_STD=ON > "$w/configure.log" 2>&1 \
  || { echo "configure failed: see $w/configure.log" >&2; exit 1; }
echo "LiteRT library sources in $w/build"
