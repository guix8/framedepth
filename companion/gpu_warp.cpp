// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 FrameDepth contributors
// Portions (the depth upsample, warp and sharpening shaders below) are adapted
// from Moonlight XR by Sean Gilleece and Moonlight XR for Frame by
// simplysanatori, both GPL-3.0, and modified for FrameDepth; each shader's
// comment names its source (THIRD_PARTY.md).
#include "gpu_warp.hpp"
#define GL_GLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/glcorearb.h>
#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <cstdio>

namespace {
const char* kVertex = R"(#version 330 core
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
})";

// Joint bilateral upsample of the depth map guided by the colour, from
// Moonlight XR shaders/upsample.frag (GPLv3): each output texel weights the
// 5x5 low resolution neighbourhood by how closely each neighbour's colour
// (the frame the depth came from, in the map's rgb) matches the colour here,
// so the depth edge snaps to the colour edge instead of straddling it.
// Rows: y = 0 is the top row of the image here (v = y / height); the warp
// samples this texture with the same convention.
// Motion guard (ours, not in Moonlight): the map trails the picture (about
// 30 ms on the GPU model at 20 maps/s, 70-200 ms on a busy CPU). Where the
// picture has changed since (fast motion), no neighbour's colour matches the colour here, and the bilateral weights keep
// a sharp depth edge where the object used to be; the warp then tears and
// stretches the moving object. conf, the colour weight's share of the spatial
// weight, falls there, and the depth is eased toward a wide average (mip 3,
// about 8x8 map texels): wrong by a little but smooth, so nothing tears.
const char* kUpsample = R"(#version 330 core
uniform sampler2D uSrc;      // video rectangle, BGRA uploaded as GL_BGRA, or Chrome's window
uniform vec4 uSrcRect;       // the picture in uSrc: x, y, w, h (texture coordinates, top first)
// The picture at tc (0..1 over the picture): uSrc is either the picture
// uploaded alone or Chrome's whole window (Reader's zero-copy reads), kept
// half a texel inside the picture so nothing beside it bleeds in
vec3 src(vec2 tc) {
    vec2 ts = vec2(textureSize(uSrc, 0));
    vec2 lo = uSrcRect.xy + 0.5 / ts, hi = uSrcRect.xy + uSrcRect.zw - 0.5 / ts;
    return texture(uSrc, clamp(uSrcRect.xy + tc * uSrcRect.zw, lo, hi)).rgb;
}
uniform sampler2D uDepth;    // RGBA8: guide rgb, depth a (1 = near), rows top first
uniform vec2 uUp;            // this pass's size
uniform float uConv;         // depth that stays on the screen plane
uniform vec4 uFlat[8];       // rectangles kept flat: x0, y0, x1, y1 (top first)
uniform float uFlatW[8];     // ... and how flat (easing in and out)
uniform int uFlatN;
uniform vec4 uOnly[16];      // when given, the only rectangles with depth (a gallery's pictures)
uniform int uOnlyN;
uniform vec4 uMapRect;       // where the depth map lies in the picture: x0, y0, x1, y1 (top first)
uniform vec4 uArea;          // the picture inside black bars; the bars stay on the screen plane
out vec4 color;
const float SIGMA_S = 1.5;
const float SIGMA_R = 0.25;
void main() {
    vec2 uv = vec2(gl_FragCoord.x / uUp.x, gl_FragCoord.y / uUp.y);
    vec3 hi = src(uv);
    vec2 dsz = vec2(textureSize(uDepth, 0));
    vec2 muv = (uv - uMapRect.xy) / (uMapRect.zw - uMapRect.xy);
    vec2 lp = muv * dsz - 0.5;
    // the texel nearest lp in the middle: with floor() the 5x5 window sat
    // up to a texel off to one side, a depth shift of 0.7 px on average on
    // a 512 map shown 1920 wide (review 2026-11-01)
    ivec2 base = ivec2(floor(lp + 0.5));
    float num = 0.0, den = 0.0, denS = 0.0;
    for (int dy = -2; dy <= 2; dy++) {
        for (int dx = -2; dx <= 2; dx++) {
            ivec2 q = clamp(base + ivec2(dx, dy), ivec2(0), ivec2(dsz) - 1);
            vec4 s = texelFetch(uDepth, q, 0);
            vec2 off = vec2(q) - lp;
            vec3 cd = hi - s.rgb;
            float ws = exp(-dot(off, off) / (2.0 * SIGMA_S * SIGMA_S));
            float w = ws * exp(-dot(cd, cd) / (2.0 * SIGMA_R * SIGMA_R));
            num += w * s.a;
            den += w;
            denS += ws;
        }
    }
    float d = num / max(den, 1e-6);
    float conf = den / max(denS, 1e-6);
    d = mix(textureLod(uDepth, muv, 3.0).a, d, smoothstep(0.08, 0.25, conf));
    if (uv.x < uArea.x || uv.y < uArea.y || uv.x > uArea.z || uv.y > uArea.w) d = uConv;
    // A gallery: depth only inside its pictures, the page between them on
    // the screen plane. Inside each picture the depth eases in from its edge
    // over 8% of its size: a ramp of 0.4% of the region (a few pixels) put
    // the whole shift there, and each picture's border cracked (headset,
    // 2026-10-28); now a picture sits in its frame on the screen plane
    if (uOnlyN > 0) {
        float inside = 0.0;
        for (int i = 0; i < uOnlyN; i++) {
            vec4 f = uOnly[i];
            float fx = 0.08 * (f.z - f.x), fy = 0.08 * (f.w - f.y);
            float wx = smoothstep(f.x, f.x + fx, uv.x) * (1.0 - smoothstep(f.z - fx, f.z, uv.x));
            float wy = smoothstep(f.y, f.y + fy, uv.y) * (1.0 - smoothstep(f.w - fy, f.w, uv.y));
            inside = max(inside, wx * wy);
        }
        d = mix(uConv, d, inside);
    }
    // Player controls and captions sit on the screen plane (no shift either
    // eye). Around them the depth eases back over a ramp outside the
    // rectangle: the picture there shifts sideways by up to the full
    // disparity, and a ramp of 1% of the height (7 px) put that whole shift
    // into a few rows, a seam that cut vertical lines across a control
    // bar's top edge, opposite in each eye (headset, 2026-10-27). Over 6% of
    // the height they lean instead; sideways 3% of the width.
    for (int i = 0; i < uFlatN; i++) {
        vec4 f = uFlat[i];
        const float fx = 0.03, fy = 0.06;
        float wx = smoothstep(f.x - fx, f.x, uv.x) * (1.0 - smoothstep(f.z, f.z + fx, uv.x));
        float wy = smoothstep(f.y - fy, f.y, uv.y) * (1.0 - smoothstep(f.w, f.w + fy, uv.y));
        d = mix(d, uConv, wx * wy * uFlatW[i]);
    }
    color = vec4(d);
})";

// Output rows: y = 0 is the bottom row in GL, which is how OpenVR reads an
// OpenGL overlay texture, so v (top first) = 1 - y / height.
// Each eye: the source x whose shifted position lands here,
//     x_out = x_src + disparity * (depth(x_src) - convergence),
// found as Moonlight XR does it (shaders/offset.frag + warp.frag, GPLv3):
// every zero crossing of e(t) = t + disp * (d(x + t) - c) along the depth
// row is a surface that lands on this pixel; the nearest one wins (that is
// occlusion), then two Newton steps on the bilinear depth settle it.
// Optionally (uFloat, off by default), when the picture comes out of the
// screen (convergence below one half), a floating window masks the outer edge
// of each eye by the largest crossed disparity, so the frame sits in front of
// what pops out instead of cutting through it.
const char* kWarp = R"(#version 330 core
uniform sampler2D uSrc;      // BGRA uploaded as GL_BGRA: sampled as rgb; or Chrome's window
uniform vec4 uSrcRect;       // the picture in uSrc: x, y, w, h (texture coordinates, top first)
// The picture at tc (0..1 over the picture): uSrc is either the picture
// uploaded alone or Chrome's whole window (Reader's zero-copy reads), kept
// half a texel inside the picture so nothing beside it bleeds in
vec3 src(vec2 tc) {
    vec2 ts = vec2(textureSize(uSrc, 0));
    vec2 lo = uSrcRect.xy + 0.5 / ts, hi = uSrcRect.xy + uSrcRect.zw - 0.5 / ts;
    return texture(uSrc, clamp(uSrcRect.xy + tc * uSrcRect.zw, lo, hi)).rgb;
}
// The same with a Catmull-Rom filter across (rows stay linear): each eye is
// resampled sideways at every pixel's own fraction, and a linear filter
// there was the main softening left, the eyes' sideways detail 81-89% of the
// source's (review 2026-11-02; Catmull-Rom 89-97%). Four taps at texel
// centres across, each linear only up and down; the result kept within its
// two middle taps, so no edge rings (and the sharpening after has no halo
// to grow).
vec3 srcAcross(vec2 tc) {
    float tw = float(textureSize(uSrc, 0).x);
    float x = (uSrcRect.x + tc.x * uSrcRect.z) * tw - 0.5;
    float i = floor(x), f = x - i;
    float w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    float w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    float w3 = f * f * (-0.5 + 0.5 * f);
    float at = (i + 0.5) / tw;   // texel i's centre, texture coordinates
    float step = 1.0 / tw;
    vec3 a = src(vec2((at - step - uSrcRect.x) / uSrcRect.z, tc.y));
    vec3 b = src(vec2((at - uSrcRect.x) / uSrcRect.z, tc.y));
    vec3 c = src(vec2((at + step - uSrcRect.x) / uSrcRect.z, tc.y));
    vec3 d = src(vec2((at + 2.0 * step - uSrcRect.x) / uSrcRect.z, tc.y));
    return clamp(w0 * a + w1 * b + w2 * c + w3 * d, min(b, c), max(b, c));
}
uniform sampler2D uDepth;    // upsampled depth (r), 1 = near, rows top first
uniform vec2 uOut;           // output size, both eyes
uniform float uSep;          // per-eye disparity, fraction of the width
uniform float uConv;
uniform vec2 uCursor;        // pointer ring position (u, v top first), negative = none
uniform float uFloat;        // 1 = floating window on (off by default: the black edge made small misalignments stand out)
uniform float uSharpen;      // 0 = off; 0..1 CAS, above that up to 10 more detail gain
uniform float uZoom;         // overscan, at least 1 + the largest shift either way
uniform int uStereo;         // the picture is already 3D: 1 side by side, 2 top and bottom (left eye first)
uniform float uEyeFill;      // share of the eye its half fills (1 half-width formats, 0.5 full-width)
uniform vec4 uHole[8];       // see-through rectangles, picture coordinates x0, y0, x1, y1 (top first)
uniform int uHoleN;
out vec4 color;
// Contrast adaptive sharpening, as Moonlight XR for Frame's shaders/warp.frag
// (sharpened(), GPLv3); the gain above 1 as Frame3D Browser 0.1.15 (GPLv3).
// The four source texels around the sample; the weight
// shrinks where the neighbourhood is already near black or white, so it
// cannot ring past either. Brings back some of the detail the bilinear
// resampling of the warp and the overscan zoom soften.
vec3 sharpened(vec2 tc, vec3 c) {
    vec2 t = 1.0 / (vec2(textureSize(uSrc, 0)) * uSrcRect.zw);   // a source texel, in picture coordinates
    vec3 n = src(tc - vec2(0.0, t.y));
    vec3 s = src(tc + vec2(0.0, t.y));
    vec3 w = src(tc - vec2(t.x, 0.0));
    vec3 e = src(tc + vec2(t.x, 0.0));
    vec3 mn = min(c, min(min(n, s), min(w, e)));
    vec3 mx = max(c, max(max(n, s), max(w, e)));
    vec3 amp = sqrt(clamp(min(mn, 1.0 - mx) / max(mx, 1e-5), 0.0, 1.0));
    vec3 wt = -amp * mix(0.125, 0.2, clamp(uSharpen, 0.0, 1.0));
    vec3 base = clamp((c + wt * (n + s + w + e)) / (1.0 + 4.0 * wt), 0.0, 1.0);
    float gain = clamp(uSharpen, 1.0, 10.0);
    return clamp(c + (base - c) * gain, 0.0, 1.0);
}
void main() {
    float hw = uOut.x * 0.5;
    bool left = gl_FragCoord.x < hw;
    float u = (gl_FragCoord.x - (left ? 0.0 : hw)) / hw;
    float v = 1.0 - gl_FragCoord.y / uOut.y;
    // Holes: transparent in both eyes at the same place, so what Chrome draws
    // just behind our overlay (a popup over the video) shows untouched
    for (int i = 0; i < uHoleN; i++) {
        vec4 h = uHole[i];
        if (u >= h.x && u < h.z && v >= h.y && v < h.w) { color = vec4(0.0); return; }
    }
    // Overscan: shifting each eye sideways leaves a strip at the frame's side
    // with nothing to show. Instead of stretching the edge pixels into it
    // (which read as the picture shrinking sideways), widen the picture by
    // the largest shift and let the shift pull real pixels in from just
    // outside. Costs that much of the frame's sides. Only sideways: enlarged
    // both ways (as before, to keep the aspect) every row was resampled
    // between two source rows, softening the picture in bands, which the
    // 1.5-5% wider aspect does not cost visibly (review 2026-11-01; Frame3D
    // Browser does not enlarge at all).
    float zoom = uZoom;
    if (uCursor.x >= 0.0) {
        // same place in both eyes: the ring sits on the screen plane
        vec2 at = vec2(0.5 + (uCursor.x - 0.5) * zoom, uCursor.y);
        float r = length((vec2(u, v) - at) * vec2(hw, uOut.y));
        float scale = max(1.0, uOut.y / 540.0);
        if (r < 11.0 * scale) {
            if (r > 9.0 * scale || r < 2.0 * scale) { color = vec4(0.0, 0.0, 0.0, 1.0); return; }
            if (r > 6.0 * scale) { color = vec4(1.0); return; }
        }
    }
    if (uStereo > 0) {
        // each eye its own half of the picture, nothing shifted
        vec2 e = vec2(u, v);
        float f = (uStereo == 1 ? e.x : e.y) - 0.5;
        if (abs(f) > 0.5 * uEyeFill) { color = vec4(0.0, 0.0, 0.0, 1.0); return; }
        f = f / uEyeFill + 0.5;
        if (uStereo == 1) e.x = f; else e.y = f;
        vec2 tc = uStereo == 1 ? vec2((left ? 0.0 : 0.5) + e.x * 0.5, e.y) : vec2(e.x, (left ? 0.0 : 0.5) + e.y * 0.5);
        color = vec4(srcAcross(tc), 1.0);
        return;
    }
    float window = uFloat * 2.0 * uSep * max(0.0, 1.0 - 2.0 * uConv);
    if ((left && u < window) || (!left && u > 1.0 - window)) { color = vec4(0.0, 0.0, 0.0, 1.0); return; }
    u = 0.5 + (u - 0.5) / zoom;
    float disp = left ? uSep : -uSep;
    ivec2 dsz = textureSize(uDepth, 0);
    int row = clamp(int(v * float(dsz.y)), 0, dsz.y - 1);
    int xi = clamp(int(u * float(dsz.x)), 0, dsz.x - 1);
    float dispT = disp * float(dsz.x);                 // in depth texels
    int reach = int(ceil(abs(dispT) * max(uConv, 1.0 - uConv))) + 2;
    float here = texelFetch(uDepth, ivec2(xi, row), 0).r;
    float bestD = -1.0;
    float bestOff = -dispT * (here - uConv);
    float pd = texelFetch(uDepth, ivec2(clamp(xi - reach, 0, dsz.x - 1), row), 0).r;
    float pe = float(-reach) + dispT * (pd - uConv);
    for (int t = -reach + 1; t <= reach; t++) {
        float cd = texelFetch(uDepth, ivec2(clamp(xi + t, 0, dsz.x - 1), row), 0).r;
        float ce = float(t) + dispT * (cd - uConv);
        float span = ce - pe;
        if (pe * ce <= 0.0 && abs(span) > 1e-6) {
            float f = clamp(-pe / span, 0.0, 1.0);
            float rd = pd + f * (cd - pd);
            if (rd > bestD) { bestD = rd; bestOff = float(t - 1) + f; }
        }
        pd = cd; pe = ce;
    }
    float s = u + bestOff / float(dsz.x);
    float h = 1.0 / float(dsz.x);
    for (int i = 0; i < 2; i++) {
        float d0 = texture(uDepth, vec2(s, v)).r;
        float dm = texture(uDepth, vec2(s - h, v)).r;
        float dp = texture(uDepth, vec2(s + h, v)).r;
        float e = (s - u) + disp * (d0 - uConv);
        float slope = 1.0 + disp * (dp - dm) / (2.0 * h);
        if (abs(slope) < 0.25) slope = 0.25;
        s -= clamp(e / slope, -4.0 * h, 4.0 * h);
    }
    vec2 tc = vec2(clamp(s, 0.0, 1.0), v);
    vec3 rgb = srcAcross(tc);
    if (uSharpen > 0.0) rgb = sharpened(tc, rgb);
    color = vec4(rgb, 1.0);
})";

// The picture shrunk to the depth model's size for the zero-copy reads:
// each texel the mean of the pixels it covers, the same boxes as
// toModelSize on the CPU (what the offline tests measure). Linear taps
// spread over the box, as before, weighted its middle more and gave the
// model a slightly softer input than the tests had (review 2026-11-02).
// Boxes over 16 pixels a side (a 10000 px wide picture) take every few.
// Rows top first in memory (glReadPixels' first row is gl_FragCoord.y 0.5).
const char* kShrink = R"(#version 330 core
uniform sampler2D uSrc;
uniform ivec4 uRect;      // the part shrunk: x, y, w, h in texels, top first
uniform ivec2 uOutSize;
out vec4 color;
void main() {
    ivec2 o = ivec2(gl_FragCoord.xy);
    ivec2 a = o * uRect.zw / uOutSize;
    ivec2 b = min(uRect.zw, max((o + 1) * uRect.zw / uOutSize, a + 1));
    ivec2 by = max(ivec2(1), (b - a + 15) / 16);
    vec3 sum = vec3(0.0);
    float n = 0.0;
    for (int y = a.y; y < b.y; y += by.y)
        for (int x = a.x; x < b.x; x += by.x) { sum += texelFetch(uSrc, uRect.xy + ivec2(x, y), 0).rgb; n += 1.0; }
    color = vec4(sum / n, 1.0);
})";

const char* kPattern = R"(#version 330 core
uniform vec2 uOut;
out vec4 color;
void main() {
    float hw = uOut.x * 0.5;
    bool left = gl_FragCoord.x < hw;
    vec2 p = vec2((gl_FragCoord.x - (left ? 0.0 : hw)) / hw, 1.0 - gl_FragCoord.y / uOut.y);
    vec3 eye = left ? vec3(1.0, 0.2, 0.15) : vec3(0.15, 0.35, 1.0);
    bool frame = p.x < 0.02 || p.x > 0.98 || p.y < 0.03 || p.y > 0.97 || abs(p.x - 0.5) < 0.008 || abs(p.y - 0.5) < 0.012;
    bool top = p.y < 0.12 && p.x > 0.3 && p.x < 0.7;
    // A ring drawn in pixel units: it looks round only if the overlay's
    // aspect is right.
    vec2 d = (p - 0.5) * vec2(hw, uOut.y) / uOut.y;
    bool ring = abs(length(d) - 0.35) < 0.015;
    color = vec4(top ? vec3(1.0) : (frame || ring) ? eye : vec3(0.08), 1.0);
})";

unsigned compile(const char* fragment, std::string& err) {
    auto shader = [&](GLenum type, const char* src) {
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) { char log[1024]; glGetShaderInfoLog(s, sizeof(log), nullptr, log); err += log; }
        return s;
    };
    GLuint p = glCreateProgram();
    GLuint vs = shader(GL_VERTEX_SHADER, kVertex), fs = shader(GL_FRAGMENT_SHADER, fragment);
    glAttachShader(p, vs); glAttachShader(p, fs);
    glLinkProgram(p);
    GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { char log[1024]; glGetProgramInfoLog(p, sizeof(log), nullptr, log); err += log; }
    glDeleteShader(vs); glDeleteShader(fs);
    return ok ? p : 0;
}

void linearClamp() {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

bool attach(GLuint fbo, GLuint tex) {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    const bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return ok;
}
}  // namespace

std::string GpuWarp::init() {
    EGLDisplay dpy = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, nullptr, nullptr)) return "EGL surfaceless display unavailable";
    display_ = dpy;
    if (!eglBindAPI(EGL_OPENGL_API)) return "EGL has no desktop OpenGL";
    const EGLint ctxAttrs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
                               EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
    EGLContext ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctxAttrs);
    if (ctx == EGL_NO_CONTEXT) return "cannot create an OpenGL 3.3 core context";
    context_ = ctx;
    if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) return "cannot make the context current (surfaceless)";
    std::string err;
    program_ = compile(kWarp, err);
    pattern_ = compile(kPattern, err);
    upsample_ = compile(kUpsample, err);
    shrink_ = compile(kShrink, err);
    if (!program_ || !pattern_ || !upsample_ || !shrink_) return "shader: " + err;
    glGenVertexArrays(1, &vao_);
    glGenFramebuffers(kBuffers, fbo_); glGenFramebuffers(1, &fboUp_);
    glGenTextures(1, &srcTex_); glGenTextures(1, &depthTex_); glGenTextures(1, &upTex_); glGenTextures(kBuffers, outTex_);
    glBindTexture(GL_TEXTURE_2D, depthTex_); linearClamp();
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    const uint8_t mid[4] = {128, 128, 128, 128};
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, mid);
    glGenerateMipmap(GL_TEXTURE_2D);
    depthW_ = depthH_ = 1;
    std::fprintf(stderr, "framedepth-companion: GL %s on %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));
    return {};
}

void GpuWarp::shutdown() {
    if (!display_) return;
    if (context_) {
        for (void*& f : fence_) if (f) { glDeleteSync(static_cast<GLsync>(f)); f = nullptr; }
        glDeleteQueries(kBuffers, query_);
        dropWindowBuffers();
        if (shrinkTex_) glDeleteTextures(1, &shrinkTex_);
        if (shrinkFbo_) glDeleteFramebuffers(1, &shrinkFbo_);
        glDeleteProgram(shrink_);
        GLuint tex[] = {srcTex_, depthTex_, upTex_};
        glDeleteTextures(3, tex);
        glDeleteTextures(kBuffers, outTex_);
        glDeleteFramebuffers(kBuffers, fbo_);
        glDeleteFramebuffers(1, &fboUp_);
        glDeleteVertexArrays(1, &vao_);
        glDeleteProgram(program_); glDeleteProgram(pattern_); glDeleteProgram(upsample_);
        eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(display_, context_);
    }
    eglTerminate(display_);
    display_ = context_ = nullptr;
}

bool GpuWarp::ensureOutput(int w, int h) {
    if (w == outW_ && h == outH_) return true;
    for (int i = 0; i < kBuffers; ++i) {
        glBindTexture(GL_TEXTURE_2D, outTex_[i]);
        linearClamp();
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        if (!attach(fbo_[i], outTex_[i])) return false;   // size kept unset: the next frame tries again
    }
    outW_ = w; outH_ = h;
    return true;
}

// The next output buffer becomes the current one; its old fence (a frame
// that was never handed on) is dropped
// The timer queries the GPU has answered. The log's "gpu" was the CPU time
// of render(); what the GPU spends was not known (review 2026-11-01).
void GpuWarp::readQueries() {
    for (int i = 0; i < kBuffers; ++i) {
        if (!queried_[i]) continue;
        GLint ready = 0;
        glGetQueryObjectiv(query_[i], GL_QUERY_RESULT_AVAILABLE, &ready);
        if (!ready) continue;
        GLuint64 ns = 0;
        glGetQueryObjectui64v(query_[i], GL_QUERY_RESULT, &ns);
        gpuMsSum_ += ns / 1e6; ++gpuN_;
        queried_[i] = false;
    }
}

int GpuWarp::nextTarget() {
    const int t = (cur_ + 1) % kBuffers;
    if (fence_[t]) { glDeleteSync(static_cast<GLsync>(fence_[t])); fence_[t] = nullptr; }
    return t;
}

void GpuWarp::finish(int target) {
    if (async_) {
        fence_[target] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        glFlush();
    } else {
        glFinish();
    }
    cur_ = target;
}

bool GpuWarp::done(int b, int64_t timeoutNs) {
    void*& f = fence_[b];
    if (!f) return true;
    const GLenum r = glClientWaitSync(static_cast<GLsync>(f), GL_SYNC_FLUSH_COMMANDS_BIT, (GLuint64)std::max<int64_t>(0, timeoutNs));
    if (r != GL_ALREADY_SIGNALED && r != GL_CONDITION_SATISFIED) return false;
    glDeleteSync(static_cast<GLsync>(f));
    f = nullptr;
    return true;
}

bool GpuWarp::ensureUpsample(int w, int h) {
    if (w == upW_ && h == upH_) return true;
    glBindTexture(GL_TEXTURE_2D, upTex_);
    linearClamp();
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R16F, w, h, 0, GL_RED, GL_FLOAT, nullptr);
    if (!attach(fboUp_, upTex_)) return false;
    upW_ = w; upH_ = h;
    return true;
}

bool GpuWarp::render(const uint8_t* src, int sw, int sh, int stride, const uint8_t* depthRgba, int dw, int dh,
                     float separation, float convergence, int ow, int oh, float cursorU, float cursorV,
                     const float* flat, int flatCount, float sharpen, const float* holes, int holeCount,
                     const float* only, int onlyCount, const float* mapRect, const float* area, float zoom,
                     int stereo, float eyeFill, const float* flatWeight) {
    // an error left from before (a failed texture size) would fail this
    // drawing too (glGetError at the end)
    for (int i = 0; i < 8 && glGetError() != GL_NO_ERROR; ++i) {}
    if (!ensureOutput(ow * 2, oh)) return false;
    // Upsampled depth: half the output per axis, never below the map itself
    // (Moonlight uses a quarter of the video; our rectangles are smaller),
    // nor above the output: a sliver of a tall video (16 px wide) asked a
    // texture past the GPU's size limit and every drawing failed (review
    // 2026-10-31)
    const int uw = std::min({std::max(ow / 2, dw), 960, ow}), uh = std::max(1, uw * oh / ow);
    if (!ensureUpsample(uw, uh)) return false;
    readQueries();
    const int target = nextTarget();
    if (!query_[0]) glGenQueries(kBuffers, query_);
    if (queried_[target]) {   // not answered yet: its result is waited for, so the query can be reused
        GLuint64 ns = 0;
        glGetQueryObjectui64v(query_[target], GL_QUERY_RESULT, &ns);
        gpuMsSum_ += ns / 1e6; ++gpuN_;
    }
    glBeginQuery(GL_TIME_ELAPSED, query_[target]);
    queried_[target] = true;
    const auto uploadStart = std::chrono::steady_clock::now();
    if (src) {
        fromWindow_ = false;
        glBindTexture(GL_TEXTURE_2D, srcTex_);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, stride / 4);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        if (sw != srcW_ || sh != srcH_) {
            linearClamp();
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, sw, sh, 0, GL_BGRA, GL_UNSIGNED_BYTE, src);
            srcW_ = sw; srcH_ = sh;
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, sw, sh, GL_BGRA, GL_UNSIGNED_BYTE, src);
        }
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    } else if (!srcW_ && !fromWindow_) {
        glEndQuery(GL_TIME_ELAPSED);
        queried_[target] = false;
        return false;   // nothing captured yet
    }
    if (depthRgba) {
        glBindTexture(GL_TEXTURE_2D, depthTex_);
        if (dw != depthW_ || dh != depthH_) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, dw, dh, 0, GL_RGBA, GL_UNSIGNED_BYTE, depthRgba);
            depthW_ = dw; depthH_ = dh;
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, dw, dh, GL_RGBA, GL_UNSIGNED_BYTE, depthRgba);
        }
        glGenerateMipmap(GL_TEXTURE_2D);   // the motion guard's wide average
    }
    uploadMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - uploadStart).count();
    glBindVertexArray(vao_);
    // Pass 1: guided upsample
    glBindFramebuffer(GL_FRAMEBUFFER, fboUp_);
    glViewport(0, 0, upW_, upH_);
    glUseProgram(upsample_);
    // the picture: uploaded alone, or its rectangle in Chrome's window
    const unsigned srcTex = fromWindow_ ? winTex_ : srcTex_;
    float srcRect[4] = {0, 0, 1, 1};
    if (fromWindow_) { srcRect[0] = (float)winX_ / winW_; srcRect[1] = (float)winY_ / winH_; srcRect[2] = (float)winRW_ / winW_; srcRect[3] = (float)winRH_ / winH_; }
    glUniform4fv(glGetUniformLocation(upsample_, "uSrcRect"), 1, srcRect);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, srcTex);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, depthTex_);
    glUniform1i(glGetUniformLocation(upsample_, "uSrc"), 0);
    glUniform1i(glGetUniformLocation(upsample_, "uDepth"), 1);
    glUniform2f(glGetUniformLocation(upsample_, "uUp"), (float)upW_, (float)upH_);
    glUniform1f(glGetUniformLocation(upsample_, "uConv"), convergence);
    flatCount = std::min(flatCount, 8);
    glUniform1i(glGetUniformLocation(upsample_, "uFlatN"), flat ? flatCount : 0);
    if (flat && flatCount > 0) {
        glUniform4fv(glGetUniformLocation(upsample_, "uFlat"), flatCount, flat);
        const float ones[8] = {1, 1, 1, 1, 1, 1, 1, 1};
        glUniform1fv(glGetUniformLocation(upsample_, "uFlatW"), flatCount, flatWeight ? flatWeight : ones);
    }
    onlyCount = only ? std::min(onlyCount, 16) : 0;
    glUniform1i(glGetUniformLocation(upsample_, "uOnlyN"), onlyCount);
    if (onlyCount > 0) glUniform4fv(glGetUniformLocation(upsample_, "uOnly"), onlyCount, only);
    static const float kWhole[4] = {0, 0, 1, 1};
    glUniform4fv(glGetUniformLocation(upsample_, "uMapRect"), 1, mapRect ? mapRect : kWhole);
    glUniform4fv(glGetUniformLocation(upsample_, "uArea"), 1, area ? area : kWhole);

    glDrawArrays(GL_TRIANGLES, 0, 3);
    // Pass 2: both eyes
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_[target]);
    glViewport(0, 0, outW_, outH_);
    glUseProgram(program_);
    glUniform4fv(glGetUniformLocation(program_, "uSrcRect"), 1, srcRect);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, srcTex);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, upTex_);
    glUniform1i(glGetUniformLocation(program_, "uSrc"), 0);
    glUniform1i(glGetUniformLocation(program_, "uDepth"), 1);
    glUniform2f(glGetUniformLocation(program_, "uOut"), (float)outW_, (float)outH_);
    glUniform1f(glGetUniformLocation(program_, "uSep"), separation);
    glUniform1f(glGetUniformLocation(program_, "uConv"), convergence);
    glUniform2f(glGetUniformLocation(program_, "uCursor"), cursorU, cursorV);
    glUniform1f(glGetUniformLocation(program_, "uFloat"), 0.0f);
    glUniform1f(glGetUniformLocation(program_, "uSharpen"), sharpen);
    if (zoom <= 0) zoom = 1.0f / (1.0f - 2.0f * separation * std::max(convergence, 1.0f - convergence));   // as Comfort::zoom
    glUniform1f(glGetUniformLocation(program_, "uZoom"), stereo ? 1.0f : zoom);
    glUniform1i(glGetUniformLocation(program_, "uStereo"), stereo);
    glUniform1f(glGetUniformLocation(program_, "uEyeFill"), eyeFill);
    holeCount = holes ? std::min(holeCount, 8) : 0;
    glUniform1i(glGetUniformLocation(program_, "uHoleN"), holeCount);
    if (holeCount > 0) glUniform4fv(glGetUniformLocation(program_, "uHole"), holeCount, holes);

    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glEndQuery(GL_TIME_ELAPSED);
    finish(target);   // the texture must be complete before vrclient copies it: see done()
    return glGetError() == GL_NO_ERROR;
}

void GpuWarp::dropWindowBuffers() {
    static auto destroyImage = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
    for (WindowBuffer& b : winBufs_) {
        glDeleteTextures(1, &b.tex);
        if (destroyImage) destroyImage(display_, b.image);
    }
    winBufs_.clear();
    winTex_ = 0;
    fromWindow_ = false;   // a CPU picture next
}

bool GpuWarp::useWindowBuffer(int fd, int bw, int bh, uint32_t stride, uint32_t offset, uint64_t modifier, uint64_t key,
                              int x, int y, int w, int h) {
    static auto createImage = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    using TargetTexture = void (*)(GLenum, void*);   // glEGLImageTargetTexture2DOES (not in glcorearb.h)
    static auto targetTexture = reinterpret_cast<TargetTexture>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    winX_ = x; winY_ = y; winRW_ = w; winRH_ = h;
    for (const WindowBuffer& b : winBufs_)
        if (b.key == key && b.w == bw && b.h == bh) { winTex_ = b.tex; winW_ = bw; winH_ = bh; fromWindow_ = true; close(fd); return true; }
    // another size: the old buffers are gone with it; a handful at most
    if (!winBufs_.empty() && (winBufs_[0].w != bw || winBufs_[0].h != bh)) dropWindowBuffers();
    if (winBufs_.size() >= 6) {
        static auto destroyImage = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
        glDeleteTextures(1, &winBufs_[0].tex);
        if (destroyImage) destroyImage(display_, winBufs_[0].image);
        winBufs_.erase(winBufs_.begin());
    }
    if (!createImage || !targetTexture) { close(fd); return false; }
    // ARGB8888 (DRM fourcc 'AR24'): the window's 32 bit depth, read as rgb
    const EGLint fourcc = 'A' | ('R' << 8) | ('2' << 16) | ('4' << 24);
    const EGLint attrs[] = {EGL_WIDTH, bw, EGL_HEIGHT, bh, EGL_LINUX_DRM_FOURCC_EXT, fourcc,
                            EGL_DMA_BUF_PLANE0_FD_EXT, fd, EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)offset, EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)stride,
                            EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, (EGLint)(modifier & 0xffffffffu), EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, (EGLint)(modifier >> 32), EGL_NONE};
    EGLImageKHR image = createImage(display_, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attrs);
    close(fd);   // the image holds the buffer
    if (image == EGL_NO_IMAGE_KHR) return false;
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    targetTexture(GL_TEXTURE_2D, image);
    linearClamp();
    if (glGetError() != GL_NO_ERROR) { glDeleteTextures(1, &tex); return false; }
    winBufs_.push_back({key, bw, bh, tex, image});
    winTex_ = tex; winW_ = bw; winH_ = bh; fromWindow_ = true;
    return true;
}

bool GpuWarp::shrink(int x, int y, int w, int h, int mw, int mh, std::vector<uint8_t>& rgba) {
    if (!fromWindow_ || !winTex_ || mw < 1 || mh < 1 || w < 1 || h < 1) return false;
    for (int i = 0; i < 8 && glGetError() != GL_NO_ERROR; ++i) {}
    if (!shrinkTex_) { glGenTextures(1, &shrinkTex_); glGenFramebuffers(1, &shrinkFbo_); }
    if (mw != shrinkW_ || mh != shrinkH_) {
        glBindTexture(GL_TEXTURE_2D, shrinkTex_);
        linearClamp();
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, mw, mh, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        if (!attach(shrinkFbo_, shrinkTex_)) return false;
        shrinkW_ = mw; shrinkH_ = mh;
    }
    glBindVertexArray(vao_);
    glBindFramebuffer(GL_FRAMEBUFFER, shrinkFbo_);
    glViewport(0, 0, mw, mh);
    glUseProgram(shrink_);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, winTex_);
    glUniform1i(glGetUniformLocation(shrink_, "uSrc"), 0);
    glUniform4i(glGetUniformLocation(shrink_, "uRect"), x, y, w, h);
    glUniform2i(glGetUniformLocation(shrink_, "uOutSize"), mw, mh);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    rgba.resize((size_t)mw * mh * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, mw, mh, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return glGetError() == GL_NO_ERROR;
}

bool GpuWarp::readback(std::vector<uint8_t>& rgba) {
    rgba.resize((size_t)outW_ * outH_ * 4);
    done(1000000000LL);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_[cur_]);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, outW_, outH_, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    // GL rows are bottom first
    const size_t row = (size_t)outW_ * 4;
    std::vector<uint8_t> tmp(row);
    for (int y = 0; y < outH_ / 2; ++y) {
        uint8_t* a = &rgba[(size_t)y * row];
        uint8_t* b = &rgba[(size_t)(outH_ - 1 - y) * row];
        std::copy(a, a + row, tmp.begin()); std::copy(b, b + row, a); std::copy(tmp.begin(), tmp.end(), b);
    }
    return glGetError() == GL_NO_ERROR;
}

bool GpuWarp::pattern(int ow, int oh) {
    if (!ensureOutput(ow * 2, oh)) return false;
    const int target = nextTarget();
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_[target]);
    glViewport(0, 0, outW_, outH_);
    glUseProgram(pattern_);
    glUniform2f(glGetUniformLocation(pattern_, "uOut"), (float)outW_, (float)outH_);
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glFinish();
    cur_ = target;
    return glGetError() == GL_NO_ERROR;

}
