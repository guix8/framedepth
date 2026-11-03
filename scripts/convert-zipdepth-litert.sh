#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 FrameDepth contributors
# Remakes models/zipdepth_672x384_fp16.tflite: the CPU (LiteRT) copy of
# ZipDepth that the companion runs for depth detail High. Moonlight XR's own
# converter (tools/xr/convert_zipdepth.py, its 512x288 model is the one Normal
# runs) at another input size; sides must be multiples of 32.
# Runs on a PC or Linux host with git, uv and Python 3.12 (needs no headset):
#   bash scripts/convert-zipdepth-litert.sh
# Sources, pinned: Moonlight XR for Frame (GPL-3.0) at the commit
# scripts/fetch-deps.sh uses, which clones ZipDepth (MIT, Fabio Tosi) at
# 91f3fd2 and exports checkpoints/zipdepth_base.pth to ONNX, then onnx2tf to
# an fp16 tflite; it checks the result against the torch model itself.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
work=$root/build-deps/convert-litert
mkdir -p "$work"
cd "$work"
eval "$(grep -E '^MOONLIGHT_(URL|REV)=' "$root/scripts/fetch-deps.sh")"
[[ -d moonlight-xr ]] || git clone -q "$MOONLIGHT_URL" moonlight-xr
git -C moonlight-xr checkout -q "$MOONLIGHT_REV"
# the versions models/ was made with (Python 3.12)
if [[ ! -d .venv ]]; then
  uv venv -q --python 3.12 .venv
  VIRTUAL_ENV=$PWD/.venv uv pip install -q --index-url https://download.pytorch.org/whl/cpu torch==2.14.1 torchvision==0.29.1
  VIRTUAL_ENV=$PWD/.venv uv pip install -q tensorflow==2.19.1 tf_keras onnx onnxscript onnxruntime onnxsim \
    onnx_graphsurgeon sng4onnx simple_onnx_processing_tools psutil onnx2tf ai-edge-litert "numpy<2"
fi
py=$PWD/.venv/bin/python; [[ -x $py ]] || py=$PWD/.venv/Scripts/python.exe
cd moonlight-xr
rm -rf tools/build/zipdepth_*   # a run before (the exporter does not overwrite on Windows)
PYTHONIOENCODING=utf-8 "$py" -X utf8 tools/xr/convert_zipdepth.py --width 672 --height 384
cp tools/build/zipdepth_672x384_fp16.tflite "$root/models/"
sha256sum "$root/models/zipdepth_672x384_fp16.tflite"
