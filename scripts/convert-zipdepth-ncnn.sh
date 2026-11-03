#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 FrameDepth contributors
# Remakes models/: the GPU (ncnn) copies of ZipDepth that the companion runs
# for depth detail Normal (512x288) and High (768x448). Both inputs share one
# weights file; sides must be multiples of 32.
# Runs on a PC or Linux host with git, uv and Python 3.12 (needs no headset):
#   bash scripts/convert-zipdepth-ncnn.sh
# Sources, pinned: ZipDepth (MIT, Fabio Tosi) github.com/fabiotosi92/ZipDepth
# at 91f3fd2, checkpoint checkpoints/zipdepth_base_npu.pth (the unfold-free
# head), its own scripts/export.py to ONNX, then pnnx (PyPI) to ncnn.
# Check a result against the LiteRT model on the headset:
#   build/framedepth-companion --depthcheck build-deps/moonlight-xr/assets/depth/zipdepth_512x288_fp16.tflite \
#       models/zipdepth_npu_512x288.ncnn.param PICTURE.ppm
# (2026-10-25: correlation 0.9963; the files in models/ came from this script.)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
work=$root/build-deps/convert
mkdir -p "$work/out"
cd "$work"
rm -f out/zipdepth_npu_*   # nothing left from an earlier run gets copied
[[ -d ZipDepth ]] || git clone -q https://github.com/fabiotosi92/ZipDepth.git
git -C ZipDepth checkout -q 91f3fd21e131641f51e8d35736d1958350180e3a
# the versions models/ was made with (Python 3.12.8)
if [[ ! -d .venv ]]; then
  uv venv -q --python 3.12 .venv
  VIRTUAL_ENV=$PWD/.venv uv pip install -q --index-url https://download.pytorch.org/whl/cpu torch==2.14.1 torchvision==0.29.1
  VIRTUAL_ENV=$PWD/.venv uv pip install -q numpy==2.5.2 onnx==1.23.2 onnxsim==0.8.1 onnxscript==0.7.2 pnnx==20261020
fi
py=.venv/bin/python; [[ -x $py ]] || py=.venv/Scripts/python.exe
pnnx=.venv/bin/pnnx; [[ -x $pnnx ]] || pnnx=.venv/Scripts/pnnx.exe
for size in 512x288 768x448; do
  w=${size%x*}; h=${size#*x}
  PYTHONIOENCODING=utf-8 "$py" -X utf8 ZipDepth/scripts/export.py --ckpt ZipDepth/checkpoints/zipdepth_base_npu.pth --npu \
    --format onnx --width "$w" --height "$h" --output "out/zipdepth_npu_$size.onnx"
  (cd out && "../$pnnx" "zipdepth_npu_$size.onnx" "inputshape=[1,3,$h,$w]")
  cp "out/zipdepth_npu_$size.ncnn.param" "$root/models/"
done
cmp out/zipdepth_npu_512x288.ncnn.bin out/zipdepth_npu_768x448.ncnn.bin   # the same weights
cp out/zipdepth_npu_512x288.ncnn.bin "$root/models/zipdepth_npu.ncnn.bin"
sha256sum "$root"/models/zipdepth_npu*
