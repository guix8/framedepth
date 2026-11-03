// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 FrameDepth contributors
// GPU side of the companion: a surfaceless EGL desktop-OpenGL context that
// turns the captured video rectangle plus a depth map into a side-by-side
// stereo texture, handed to SteamVR with SetOverlayTexture (vrclient copies
// it inside our process; no shared CPU buffer for the compositor to read).
// Passes per frame, as Moonlight XR: guided depth upsample, then the
// occlusion-aware warp for both eyes.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

class GpuWarp {
public:
    ~GpuWarp() { shutdown(); }
    // Creates the context on the calling thread; every other call must come
    // from the same thread. Empty string on success.
    std::string init();
    void shutdown();
    // Source: BGRA rows top first; nullptr keeps the last one (a redraw for a
    // new depth map or pointer position). Depth map: the depth engine's RGBA8
    // (guide colour rgb, depth a, 1 = near), rows top first; nullptr keeps
    // the last one. Output: ow x oh per eye, side by side, into the next of
    // kBuffers (current() names it: two finished frames can wait to be
    // handed over while a third is drawn). In async mode the call returns once the GPU has the work
    // (a fence marks its end); done() says when the frame may be handed to
    // SteamVR, so the caller captures the next frame while the GPU draws.
    // cursorU/V: a pointer ring at that point of the picture (0..1, top
    // first), drawn the same in both eyes; negative for none.
    bool render(const uint8_t* src, int sw, int sh, int stride, const uint8_t* depthRgba, int dw, int dh,
                float separation, float convergence, int ow, int oh, float cursorU = -1, float cursorV = -1,
                const float* flat = nullptr, int flatCount = 0, float sharpen = 0,
                const float* holes = nullptr, int holeCount = 0, const float* only = nullptr, int onlyCount = 0,
                const float* mapRect = nullptr, const float* area = nullptr, float zoom = 0,
                int stereo = 0, float eyeFill = 1, const float* flatWeight = nullptr);
    // flat: flatCount rectangles (x0, y0, x1, y1 in picture coordinates, top
    // first) kept on the screen plane: player controls and captions;
    // flatWeight: how flat each is (0..1, easing in and out; null: all 1).
    // sharpen: contrast adaptive sharpening, 0 = off (see kWarp).
    // holes: holeCount rectangles (picture coordinates, top first) left
    // transparent in both eyes, where Chrome's own drawing must show.
    // only: when given, onlyCount rectangles (picture coordinates) that are
    // the only places with depth; everything else on the screen plane.
    // mapRect: where the depth map lies in the picture (x0, y0, x1, y1, top
    // first; the whole picture when null), as when the depth model was given
    // only the picture inside black bars. area: the picture inside the bars;
    // outside it on the screen plane (null: everything is picture).
    // zoom: the overscan (see kWarp); 0 works it out from separation and
    // convergence. The caller passes a fixed one while those move (comfort
    // limits), so the picture does not change size.
    // stereo: the picture is already 3D (1 side by side, 2 top and bottom,
    // left eye first): each eye shows its own half as it is, no depth.
    // eyeFill: the share of each eye's width (side by side) or height (top
    // and bottom) its half fills, centred: 1 for half-width formats (stretched
    // back, as meant), 0.5 for full-width ones (their own aspect).
    // Test pattern: left eye red frame, right eye blue, a white bar at the top
    // of each eye, and a ring that is round only at the right aspect.
    bool pattern(int ow, int oh);
    // The output as RGBA rows, top first (for the offline test)
    bool readback(std::vector<uint8_t>& rgba);
    void setAsync(bool on) { async_ = on; }
    // Whether the last rendered frame (or buffer b's) is complete, waiting up to timeoutNs
    bool done(int64_t timeoutNs) { return done(cur_, timeoutNs); }
    bool done(int b, int64_t timeoutNs);
    unsigned texture() const { return outTex_[cur_]; }
    unsigned texture(int b) const { return outTex_[b]; }
    int current() const { return cur_; }
    static constexpr int kBuffers = 3;
    // Zero-copy source (Reader): one of the dma-bufs of Chrome's window
    // (DRI3), imported once per buffer (key: its inode; Xwayland turns over
    // a few) and used until a CPU picture is given to render() again. fd is
    // always closed. x, y, w, h: the picture in the window.
    bool useWindowBuffer(int fd, int bw, int bh, uint32_t stride, uint32_t offset, uint64_t modifier, uint64_t key,
                         int x, int y, int w, int h);
    // Lets go of the window's buffers (a view ends, reads are XShm again)
    void dropWindowBuffers();
    // The zero-copy source's x, y, w, h shrunk to mw x mh, as RGBA rows top
    // first, each texel the mean of what it covers (as toModelSize)
    bool shrink(int x, int y, int w, int h, int mw, int mh, std::vector<uint8_t>& rgba);
    // CPU time the last render() spent handing the picture and map to the GL (ms)
    double uploadMs() const { return uploadMs_; }
    // GPU time of the drawings finished since the last call (ms, summed, and
    // how many): measured by timer queries, read once the GPU has them
    void takeGpuTime(double& ms, int& n) { ms = gpuMsSum_; n = gpuN_; gpuMsSum_ = 0; gpuN_ = 0; }
    int width() const { return outW_; }
    int height() const { return outH_; }

private:
    bool ensureOutput(int w, int h);
    bool ensureUpsample(int w, int h);
    int nextTarget();
    void finish(int target);
    bool async_ = false;
    int cur_ = 0;
    double uploadMs_ = 0;
    void* fence_[kBuffers] = {nullptr, nullptr, nullptr};
    unsigned query_[kBuffers] = {0, 0, 0};
    bool queried_[kBuffers] = {false, false, false};
    double gpuMsSum_ = 0;
    int gpuN_ = 0;
    void readQueries();
    void* display_ = nullptr;
    void* context_ = nullptr;
    unsigned program_ = 0, pattern_ = 0, upsample_ = 0, vao_ = 0, fbo_[kBuffers] = {0, 0, 0}, fboUp_ = 0;
    unsigned srcTex_ = 0, depthTex_ = 0, upTex_ = 0, outTex_[kBuffers] = {0, 0, 0};
    // zero-copy: the window's buffers as textures, the one in use, the picture in it
    struct WindowBuffer { uint64_t key; int w, h; unsigned tex; void* image; };
    std::vector<WindowBuffer> winBufs_;
    unsigned winTex_ = 0, shrink_ = 0, shrinkTex_ = 0, shrinkFbo_ = 0;
    int winW_ = 0, winH_ = 0, winX_ = 0, winY_ = 0, winRW_ = 0, winRH_ = 0, shrinkW_ = 0, shrinkH_ = 0;
    bool fromWindow_ = false;

    int srcW_ = 0, srcH_ = 0, depthW_ = 0, depthH_ = 0, upW_ = 0, upH_ = 0, outW_ = 0, outH_ = 0;
};
