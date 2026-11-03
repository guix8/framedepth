# Third-party components

FrameDepth itself is GPL-3.0-only. This repository contains only FrameDepth's
own sources, the converted ZipDepth models in `models/` and test pages; the
components marked *fetched* are downloaded by `scripts/fetch-deps.sh` at the
pinned versions below and built on the headset.

| Component | Used for | How it reaches the build | License |
|---|---|---|---|
| [Moonlight XR for Frame](https://github.com/simplysanatori/moonlight-xr-steam-frame) at `f00500686968dfce0173f0e2a7cf0c75da0bb0ff` | depth engine (`depth/depth_engine.c`, `depth/litert_backend.c`, `xr/core/xr_depthmap.c`, `xr/core/xr_rate.c`) compiled unchanged into the companion; the CPU depth model and its license from `assets/depth/` | fetched | GPL-3.0 |
| [Moonlight XR](https://github.com/Gilleece/moonlight-android-xr) (Sean Gilleece) and its Steam Frame port (Moonlight XR for Frame, above) | what the companion's shaders follow: guided depth upsample, occlusion-aware warp (offset search and Newton refinement), contrast adaptive sharpening (`sharpened()`); ported, see comments in `companion/gpu_warp.cpp` | adapted in this repository | GPL-3.0 |
| Frame3D Browser 0.1.15 | sharpening gain above 1, the depth blur idea | adapted in this repository | GPL-3.0 |
| [ZipDepth](https://github.com/fabiotosi92/ZipDepth) (Fabio Tosi) at `91f3fd2` | depth model: `models/zipdepth_npu*.ncnn.*` converted from `checkpoints/zipdepth_base_npu.pth` by `scripts/convert-zipdepth-ncnn.sh`; `models/zipdepth_672x384_fp16.tflite` from `checkpoints/zipdepth_base.pth` by `scripts/convert-zipdepth-litert.sh` (Moonlight XR's converter); the 512x288 LiteRT copy comes from Moonlight XR's assets | in this repository (`models/`, license in `models/ZipDepth-LICENSE.txt`) and fetched | MIT (see the note below) |
| [ncnn](https://github.com/Tencent/ncnn) `20260526` with glslang | GPU (Vulkan) inference; linked statically | fetched | BSD-3-Clause (glslang: BSD-3-Clause and others, see its repository) |
| [TensorFlow Lite / LiteRT](https://github.com/tensorflow/tensorflow) 2.21.0 with XNNPACK, abseil, ruy, Eigen and others it fetches | CPU inference (C API); linked statically | fetched | Apache-2.0 (dependencies: BSD, Apache-2.0, MIT, MPL-2.0 for Eigen; full list in Moonlight XR's `assets/depth/third_party_LICENSES.txt`) |
| [OpenVR](https://github.com/ValveSoftware/openvr) 2.12.14 header | SteamVR overlay API; the client library is SteamVR's own (`/opt/steamvr/bin/linuxarm64/libopenvr_api.so`) | fetched (header only) | BSD-3-Clause |
| pnnx (part of ncnn) | converting ZipDepth to ncnn, offline only | not part of the build | BSD-3-Clause |
| GCC runtime (libstdc++, libgcc) | linked statically into the companion | the build's toolchain | GPL-3.0 with the GCC Runtime Library Exception |

The extension icons and the test pages in `tests/pages/` were made for this
project (GPL-3.0-only).

The pictures `docs/images/depth-example.jpg` and `docs/images/stereo-wiggle.gif`
show a frame of *Tears of Steel* (c) Blender Foundation, mango.blender.org,
under CC-BY-3.0 (`LICENSES/CC-BY-3.0.txt`), with FrameDepth's depth map and
left and right eyes of it. The popup pictures in `docs/images/` are
screenshots of the extension (GPL-3.0-only).

Note on ZipDepth: its authors trained it by distillation from pseudo-labels of
Depth Anything V2 Large (CC-BY-NC-4.0) and released the weights under MIT
(ZipDepth README). FrameDepth relies on that MIT grant; whether the teacher's
non-commercial terms reach a distilled model is not settled.

## Distributing built binaries

Sharing this source needs nothing beyond the GPL-3.0. If you distribute a
built `framedepth-companion`, it contains statically linked LiteRT/TensorFlow
Lite (Apache-2.0) and ncnn/glslang (BSD-3-Clause and others): ship their
license texts and notices with it, together with the corresponding source as
the GPL-3.0 requires (this repository at that commit, Moonlight XR for Frame at
the pinned commit, TensorFlow 2.21.0 with the dependencies its build fetched,
ncnn with glslang). The ZipDepth weights need their MIT notice.
`packaging/install.sh` installs these license texts next to the companion
(`licenses/`), so an installed copy carries its notices.
