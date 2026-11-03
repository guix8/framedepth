// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 FrameDepth contributors
// The depth model behind Moonlight XR's litert_backend.h interface, so its
// depth engine (thread, averaging, scene cuts, normalisation) runs unchanged
// on either of two backends:
//   *.param  ncnn on the GPU through Vulkan (Turnip). ZipDepth 512x288 takes
//            about 16 ms there against 29 ms for LiteRT on three big CPU
//            cores, and leaves the CPU to Chrome and SteamVR (measured on a
//            Steam Frame, 2026-10-25).
//   anything else  LiteRT + XNNPACK on the CPU, upstream's litert_backend.c,
//            compiled with its model functions renamed fd_litert_* (see
//            CMakeLists.txt) and called from here.
// An ncnn model's input size comes from its file name (..._512x288.ncnn.param);
// its input is "in0" (RGB 0..1, planar) and its output "out0" (H x W, larger
// is nearer), as pnnx names them. ncnn is BSD-3-Clause.
//
// The GPU run is split into short submissions. The Frame's Adreno has a
// single queue: one 25-40 ms submission (ZipDepth 768x448) made SteamVR's
// compositor, Xwayland's window reads and our warp wait behind it, and the
// video stuttered while the headset was worn (2026-10-25). The model is run
// up to kChunks intermediate blobs in turn, each its own submission of a few
// ms, with a short pause between, so other work slots in. Measured with the
// headset in use (768x448): whole 40.6 ms, 2 chunks 38.9, 4 chunks 48.5,
// 8 chunks 51.5; 6 keeps each submission near 7 ms for ~15% more time.
#include "litert_backend.h"

#include <gpu.h>
#include <net.h>

#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

extern "C" {
typedef struct MlxrLitertModel MlxrLitertModel;
MlxrLitertModel* fd_litert_load(const char* path, const MlxrDepthModelOptions* opt, char* err, size_t errLen);
void fd_litert_free(MlxrLitertModel* m);
int fd_litert_width(const MlxrLitertModel* m);
int fd_litert_height(const MlxrLitertModel* m);
int fd_litert_threads(const MlxrLitertModel* m);
unsigned fd_litert_flags(const MlxrLitertModel* m);
float fd_litert_load_ms(const MlxrLitertModel* m);
float* fd_litert_input(MlxrLitertModel* m);
int fd_litert_run(MlxrLitertModel* m, float* ms);
const float* fd_litert_output(const MlxrLitertModel* m);
}

struct MlxrDepthModel {
    MlxrLitertModel* lite = nullptr;
    ncnn::Net net;
    int w = 0, h = 0;
    float loadMs = 0;
    std::vector<float> in, out;   // NHWC input as the engine writes it; H x W output
    std::vector<std::string> splits;   // blobs run up to, one submission each, before "out0"
};

static constexpr int kChunks = 6;

// Output blob names of evenly spaced layers of the .param (text format:
// magic line, counts line, then "Type name inputs outputs bottoms... tops...")
static std::vector<std::string> splitBlobs(const std::string& param) {
    std::ifstream f(param);
    std::string line;
    std::vector<std::string> tops;
    for (int n = 0; std::getline(f, line); ++n) {
        if (n < 2) continue;
        std::istringstream s(line);
        std::string type, name; int ins = 0, outs = 0;
        if (!(s >> type >> name >> ins >> outs) || outs < 1 || type == "Input") continue;
        std::string blob;
        for (int i = 0; i < ins + outs && s >> blob; ++i) {}
        if (blob != "out0") tops.push_back(blob);   // the layer's last output
    }
    std::vector<std::string> out;
    for (int k = 1; k < kChunks && !tops.empty(); ++k) out.push_back(tops[tops.size() * k / kChunks]);
    return out;
}

static void fail(char* err, size_t len, const std::string& why) {
    if (err && len) std::snprintf(err, len, "%s", why.c_str());
}

static bool endsWith(const std::string& s, const char* tail) {
    const size_t n = std::strlen(tail);
    return s.size() >= n && s.compare(s.size() - n, n, tail) == 0;
}

extern "C" MlxrDepthModel* mlxr_depth_model_load(const char* path, const MlxrDepthModelOptions* opt,
                                                 char* err, size_t errLen) {
    const std::string p = path ? path : "";
    if (!endsWith(p, ".ncnn.param")) {
        MlxrLitertModel* lite = fd_litert_load(path, opt, err, errLen);
        if (!lite) return nullptr;
        auto* m = new MlxrDepthModel;
        m->lite = lite;
        return m;
    }
    const auto t0 = std::chrono::steady_clock::now();
    std::smatch size;
    if (!std::regex_search(p, size, std::regex("_(\\d+)x(\\d+)\\.ncnn\\.param$"))) {
        fail(err, errLen, "ncnn model name carries no WxH: " + p);
        return nullptr;
    }
    if (ncnn::get_gpu_count() == 0) {
        fail(err, errLen, "no Vulkan GPU for ncnn");
        return nullptr;
    }
    auto* m = new MlxrDepthModel;
    m->w = std::stoi(size[1]); m->h = std::stoi(size[2]);
    m->net.opt.use_vulkan_compute = true;
    m->net.opt.num_threads = 1;   // the few layers ncnn may leave on the CPU
    m->net.opt.lightmode = false;  // keep intermediate blobs between chunks (no recomputing)
    m->net.set_vulkan_device(0);
    const std::string bin = p.substr(0, p.size() - 6) + ".bin";
    if (m->net.load_param(p.c_str()) != 0 || m->net.load_model(bin.c_str()) != 0) {
        fail(err, errLen, "cannot load " + p + " / .bin");
        delete m;
        return nullptr;
    }
    m->splits = splitBlobs(p);
    m->in.assign((size_t)m->w * m->h * 3, 0.0f);
    m->out.assign((size_t)m->w * m->h, 0.0f);
    m->loadMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return m;
}

extern "C" void mlxr_depth_model_free(MlxrDepthModel* m) {
    if (!m) return;
    if (m->lite) fd_litert_free(m->lite);
    delete m;
}

extern "C" int mlxr_depth_model_width(const MlxrDepthModel* m) { return m->lite ? fd_litert_width(m->lite) : m->w; }
extern "C" int mlxr_depth_model_height(const MlxrDepthModel* m) { return m->lite ? fd_litert_height(m->lite) : m->h; }
// The GPU model reports 0 CPU threads
extern "C" int mlxr_depth_model_threads(const MlxrDepthModel* m) { return m->lite ? fd_litert_threads(m->lite) : 0; }
extern "C" unsigned mlxr_depth_model_flags(const MlxrDepthModel* m) { return m->lite ? fd_litert_flags(m->lite) : MLXR_DEPTH_FP16; }
extern "C" float mlxr_depth_model_load_ms(const MlxrDepthModel* m) { return m->lite ? fd_litert_load_ms(m->lite) : m->loadMs; }
extern "C" float* mlxr_depth_model_input(MlxrDepthModel* m) { return m->lite ? fd_litert_input(m->lite) : m->in.data(); }
extern "C" const float* mlxr_depth_model_output(const MlxrDepthModel* m) {
    return m->lite ? fd_litert_output(m->lite) : m->out.data();
}

extern "C" int mlxr_depth_model_run(MlxrDepthModel* m, float* ms) {
    if (m->lite) return fd_litert_run(m->lite, ms);
    const auto t0 = std::chrono::steady_clock::now();
    const int w = m->w, h = m->h;
    ncnn::Mat in(w, h, 3);
    for (int c = 0; c < 3; ++c) {
        float* plane = in.channel(c);
        for (int i = 0; i < w * h; ++i) plane[i] = m->in[(size_t)i * 3 + c];
    }
    // GPU-side extraction needs its allocators set and the input uploaded
    // by hand (ncnn's own CPU-side extract does both inside one submission)
    const ncnn::VulkanDevice* vkdev = m->net.vulkan_device();
    ncnn::VkAllocator* blobAlloc = vkdev->acquire_blob_allocator();
    ncnn::VkAllocator* stagingAlloc = vkdev->acquire_staging_allocator();
    ncnn::Option opt = m->net.opt;
    opt.blob_vkallocator = blobAlloc; opt.workspace_vkallocator = blobAlloc; opt.staging_vkallocator = stagingAlloc;
    ncnn::Mat out;
    int rc = 0;
    {
        ncnn::Extractor ex = m->net.create_extractor();
        ex.set_blob_vkallocator(blobAlloc); ex.set_workspace_vkallocator(blobAlloc); ex.set_staging_vkallocator(stagingAlloc);
        ncnn::VkMat inGpu;
        {
            ncnn::VkCompute cmd(vkdev);
            cmd.record_upload(in, inGpu, opt);
            rc = cmd.submit_and_wait();
        }
        if (rc == 0) rc = ex.input("in0", inGpu);
        for (size_t i = 0; rc == 0 && i <= m->splits.size(); ++i) {
            const bool last = i == m->splits.size();
            ncnn::VkMat part;
            ncnn::VkCompute cmd(vkdev);
            rc = ex.extract(last ? "out0" : m->splits[i].c_str(), part, cmd);
            if (rc == 0 && last) cmd.record_download(part, out, opt);
            if (rc == 0) rc = cmd.submit_and_wait();
            if (!last) usleep(200);   // room for the compositor and the warp between chunks
        }
    }
    vkdev->reclaim_blob_allocator(blobAlloc);
    vkdev->reclaim_staging_allocator(stagingAlloc);
    if (rc != 0) return -1;
    if (out.w != w || out.h != h) return -1;
    const float* src = out.channel(0);
    std::memcpy(m->out.data(), src, sizeof(float) * (size_t)w * h);
    if (ms) *ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return 0;
}
