#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 FrameDepth contributors
# Build the companion on the headset (SteamOS, aarch64):
#   bash scripts/fetch-deps.sh   (once)
#   bash scripts/build.sh
# LiteRT (TensorFlow Lite C API with XNNPACK) and ncnn (Vulkan) are built
# once into build-deps/; the companion into build/. Nothing is installed.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
[[ $(uname -s) == Linux && $(uname -m) == aarch64 ]] || { echo 'Build on the ARM64 headset.' >&2; exit 1; }
[[ -d build-deps/moonlight-xr/depth ]] || { echo 'Run scripts/fetch-deps.sh first.' >&2; exit 1; }
JOBS=${JOBS:-6}

# LiteRT: Moonlight's script downloads TensorFlow without a hash, so the
# checked archive is unpacked where it looks first
export MLXR_LITERT_ROOT="$root/build-deps/litert"
if [[ ! -f $MLXR_LITERT_ROOT/dist/lib/libmlxr_litert.a ]]; then
  if [[ ! -d $MLXR_LITERT_ROOT/src/tensorflow-2.21.0 ]]; then
    mkdir -p "$MLXR_LITERT_ROOT/src"
    tar -xzf build-deps/downloads/tensorflow-2.21.0.tar.gz -C "$MLXR_LITERT_ROOT/src"
  fi
  bash build-deps/moonlight-xr/depth/tools/build-litert.sh
fi

if [[ ! -f build-deps/ncnn/dist/lib/libncnn.a ]]; then
  cmake -S build-deps/ncnn -B build-deps/ncnn/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DNCNN_VULKAN=ON -DNCNN_SHARED_LIB=OFF \
    -DNCNN_BUILD_EXAMPLES=OFF -DNCNN_BUILD_TOOLS=OFF -DNCNN_BUILD_BENCHMARK=OFF -DNCNN_BUILD_TESTS=OFF \
    -DCMAKE_INSTALL_PREFIX="$root/build-deps/ncnn/dist"
  nice -n 10 ninja -C build-deps/ncnn/build -j "$JOBS" install
fi

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release "$@"
cmake --build build --parallel "$JOBS"
echo "Built build/framedepth-companion"
