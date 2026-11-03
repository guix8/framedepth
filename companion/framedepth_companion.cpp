// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 FrameDepth contributors
// framedepth-companion: the headset side of the FrameDepth Chrome extension.
//   - WebSocket on 127.0.0.1 that accepts only the extension's Origin
//   - reads the video's rectangle from Chrome's X11 window, makes its depth
//     with Moonlight XR's depth engine (ZipDepth on the GPU through ncnn, or
//     LiteRT on the CPU; normalisation, temporal smoothing) and shows it as
//     side-by-side stereo in our own overlay anchored over that rectangle
// Protocol: docs/how-it-works.md. One client at a time (the extension's service
// worker); a new connection replaces the old one.
#include <openvr.h>
extern "C" {
#include "depth_engine.h"
#include "litert_backend.h"
#include "xr_depthmap.h"
}
#include <sched.h>
#include "gpu_warp.hpp"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <regex>
#include <string>
#include <vector>
#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <dirent.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/eventfd.h>
#include <pthread.h>
#include <dlfcn.h>
#include <link.h>
#include <strings.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xfixes.h>
#include <X11/Xlib-xcb.h>
#include <xcb/dri3.h>
#include <sys/stat.h>

#include <X11/extensions/XShm.h>

static std::atomic<bool> g_stop{false};
static void onSignal(int) { g_stop = true; }
// --dump DIR SECONDS: the live view writes what it hands SteamVR, with its
// inputs, every SECONDS (for checking the whole path without wearing the headset)
static std::string g_dumpDir;
static double g_dumpEvery = 0;
#define LOG(...) do { std::fprintf(stderr, "framedepth-companion: " __VA_ARGS__); std::fputc('\n', stderr); } while (0)
#ifndef FD_VERSION
#define FD_VERSION "dev"   // set by CMakeLists.txt from extension/manifest.json
#endif
// The edge clean-up: near objects widened by this many depth map
// texels (dilateDepth). On the test scenes at strength 3% without depth blur
// it smoothed jagged outlines of near objects; a blur of stretched bands was
// tried too and changed 0.2% of pixels at most (the depth blur already keeps
// stretches short), so it was left out (2026-10-27).
static constexpr int kEdgeDilate = 2;
// With Steady on, the engine's depth range average (ms; its default is 150):
// scene cuts are caught up 2-3 times sooner and Steady keeps the flicker down.
// At 8-15 maps a second it takes most of each new map's range; 150 and 250
// were tried against the depth feeling different over time (2026-11-01):
// scene cuts caught up worse (error 0.76 to 1.18 and 1.78 on the cut scene)
// and no less flicker on the real scenes.
static constexpr int kSteadyRangeTau = 75;

// ---- SHA-1 and base64, only for the WebSocket handshake ----
static std::string sha1(const std::string& msg) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    std::string m = msg;
    uint64_t bits = (uint64_t)msg.size() * 8;
    m += (char)0x80;
    while (m.size() % 64 != 56) m += (char)0;
    for (int i = 7; i >= 0; --i) m += (char)((bits >> (i * 8)) & 0xff);
    auto rol = [](uint32_t v, int s) { return (v << s) | (v >> (32 - s)); };
    for (size_t off = 0; off < m.size(); off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t)(uint8_t)m[off + 4 * i] << 24 | (uint32_t)(uint8_t)m[off + 4 * i + 1] << 16 |
                   (uint32_t)(uint8_t)m[off + 4 * i + 2] << 8 | (uint32_t)(uint8_t)m[off + 4 * i + 3];
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    std::string out;
    for (uint32_t v : h) for (int i = 3; i >= 0; --i) out += (char)((v >> (i * 8)) & 0xff);
    return out;
}
static std::string base64(const std::string& in) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        uint32_t v = (uint8_t)in[i] << 16 | (uint8_t)in[i + 1] << 8 | (uint8_t)in[i + 2];
        out += t[v >> 18]; out += t[(v >> 12) & 63]; out += t[(v >> 6) & 63]; out += t[v & 63];
    }
    if (i + 1 == in.size()) {
        uint32_t v = (uint8_t)in[i] << 16;
        out += t[v >> 18]; out += t[(v >> 12) & 63]; out += "==";
    } else if (i + 2 == in.size()) {
        uint32_t v = (uint8_t)in[i] << 16 | (uint8_t)in[i + 1] << 8;
        out += t[v >> 18]; out += t[(v >> 12) & 63]; out += t[(v >> 6) & 63]; out += '=';
    }
    return out;
}

// ---- WebSocket connection (server side) ----
struct Client {
    int fd = -1;
    std::string in;
    std::string partial;     // a fragmented message being reassembled
    int partialOpcode = 0;
    bool open() const { return fd >= 0; }
    void close() { if (fd >= 0) ::close(fd); fd = -1; in.clear(); partial.clear(); partialOpcode = 0; }
    bool sendAll(const void* p, size_t n) {
        const char* c = static_cast<const char*>(p);
        while (n) {
            ssize_t w = ::send(fd, c, n, MSG_NOSIGNAL);
            if (w < 0 && errno == EINTR) continue;
            if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                pollfd p{fd, POLLOUT, 0};
                if (poll(&p, 1, 1000) <= 0) return false;
                continue;
            }
            if (w <= 0) return false;
            c += w; n -= (size_t)w;
        }
        return true;
    }
    bool sendFrame(int opcode, const void* data, size_t n) {
        std::string h;
        h += (char)(0x80 | opcode);
        if (n < 126) h += (char)n;
        else if (n < 65536) { h += (char)126; h += (char)(n >> 8); h += (char)(n & 0xff); }
        else { h += (char)127; for (int i = 7; i >= 0; --i) h += (char)((uint64_t)n >> (i * 8) & 0xff); }
        return sendAll(h.data(), h.size()) && sendAll(data, n);
    }
    bool sendText(const std::string& s) { return sendFrame(1, s.data(), s.size()); }
};

// A request header's value. Scanned by hand: std::regex matches by recursion,
// one level a character, and a 16 KB header line from a local program could
// run the stack out before the Origin check (review 2026-11-01).
static std::string header(const std::string& req, const char* name) {
    const size_t n = std::strlen(name);
    for (size_t at = req.find("\r\n"); at != std::string::npos; at = req.find("\r\n", at + 2)) {
        const size_t k = at + 2;
        if (req.size() < k + n + 1 || req[k + n] != ':' || strncasecmp(req.c_str() + k, name, n) != 0) continue;
        size_t v = k + n + 1;
        while (v < req.size() && (req[v] == ' ' || req[v] == '\t')) ++v;
        return req.substr(v, req.find_first_of("\r\n", v) - v);
    }
    return {};
}

// A string as a JSON string literal's contents (error texts can hold quotes,
// backslashes and line breaks, e.g. a shader log)
static std::string jsonEscape(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o;
}

// Reads the HTTP upgrade and answers it; false refuses the connection.
static bool handshake(int fd, const std::string& origin) {
    std::string req;
    char buf[2048];
    // The whole request within 3 s: the main loop waits here, so a local
    // process trickling bytes must not hold it longer
    const int64_t deadline = mlxr_depth_now_ns() + 3000000000LL;
    while (req.find("\r\n\r\n") == std::string::npos) {
        pollfd p{fd, POLLIN, 0};
        const int left = (int)((deadline - mlxr_depth_now_ns()) / 1000000);
        if (left <= 0 || poll(&p, 1, left) <= 0 || req.size() > 16384) { LOG("handshake: no complete request"); return false; }
        ssize_t r = ::recv(fd, buf, sizeof(buf), 0);
        if (r <= 0) { LOG("handshake: connection closed"); return false; }
        req.append(buf, (size_t)r);
    }
    const std::string gotOrigin = header(req, "Origin"), key = header(req, "Sec-WebSocket-Key");
    if (gotOrigin != origin || key.empty()) {
        const char* deny = "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        (void)!::send(fd, deny, std::strlen(deny), MSG_NOSIGNAL);
        LOG("refused a connection from origin '%s'", gotOrigin.c_str());
        return false;
    }
    const std::string accept = base64(sha1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));
    const std::string resp = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                             "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
    return ::send(fd, resp.data(), resp.size(), MSG_NOSIGNAL) == (ssize_t)resp.size();
}

// One complete message out of c.in, or false when more bytes are needed.
// Control frames are answered here. Chrome splits large binary messages into
// a first frame and continuation frames (opcode 0); they are joined here.
static bool nextMessage(Client& c, int& opcode, std::string& payload, bool& closed) {
    for (;;) {
        if (c.in.size() < 2) return false;
        const uint8_t b0 = c.in[0], b1 = c.in[1];
        uint64_t n = b1 & 0x7f;
        size_t pos = 2;
        if (n == 126) { if (c.in.size() < 4) return false; n = (uint8_t)c.in[2] << 8 | (uint8_t)c.in[3]; pos = 4; }
        else if (n == 127) {
            if (c.in.size() < 10) return false;
            n = 0; for (int i = 0; i < 8; ++i) n = n << 8 | (uint8_t)c.in[2 + i];
            pos = 10;
        }
        if (n > (64u << 20)) { closed = true; return false; }
        const bool masked = b1 & 0x80;
        if (c.in.size() < pos + (masked ? 4 : 0) + n) return false;
        uint8_t mask[4] = {0, 0, 0, 0};
        if (masked) { std::memcpy(mask, c.in.data() + pos, 4); pos += 4; }
        payload.assign(c.in, pos, (size_t)n);
        if (masked) for (size_t i = 0; i < payload.size(); ++i) payload[i] ^= mask[i & 3];
        c.in.erase(0, pos + (size_t)n);
        const bool fin = b0 & 0x80;
        opcode = b0 & 0x0f;
        if (opcode == 8) { closed = true; return false; }
        if (opcode == 9) { c.sendFrame(10, payload.data(), payload.size()); continue; }
        if (opcode == 10) continue;
        if (opcode == 0) {                       // continuation
            if (!c.partialOpcode) { closed = true; return false; }
            c.partial += payload;
            if (c.partial.size() > (64u << 20)) { closed = true; return false; }
            if (!fin) continue;
            opcode = c.partialOpcode; payload.swap(c.partial);
            c.partial.clear(); c.partialOpcode = 0;
            return true;
        }
        if (!fin) { c.partialOpcode = opcode; c.partial = payload; continue; }
        return true;
    }
}

// ---- our own stereo overlay over the video, anchored to Chrome's window ----
// The extension reports where the video is (window pixels). This thread reads
// that rectangle from Chrome's X11 window (XShm), feeds the depth engine, warps
// a left and a right eye image (as Moonlight XR's warp: source x = x -
// disparity * (depth - convergence)), and shows them side by side in an overlay
// placed 1 cm in front of the same pixels of Chrome's SteamVR window overlay.

struct Rect { int x = 0, y = 0, w = 0, h = 0; bool visible = false; };

struct Shared {
    std::mutex m;
    bool want = false;            // extension asked for a view
    std::string marker;           // window title marker for finding Chrome
    uint64_t generation = 0;      // bumps on every view request
    Rect rect;
    float strength = 0.9f, convergence = 50.0f;
    int smooth = 3;                     // depth blur radius, depth map texels
    float sharpen = 0;                  // contrast adaptive sharpening, 0 = off
    int detail = 0;                     // depth model: 0 ZipDepth 512x288 CPU, 1 768x448 GPU, 2 672x384 CPU
    int subtitles = 1;                  // keep burned-in subtitles flat (SubtitleFinder): 1 on, 0 off
    int scrollflat = 1;                 // flat while the picture moves in the window (scrolling): 1 on, 0 off
    std::atomic<int> stereoMode{0};     // what the view shows now: 0 made 3D, 1/2 a 3D video as it is
    std::vector<std::array<int, 4>> ui; // player controls and captions, window pixels (x, y, w, h)
    std::vector<std::array<int, 4>> holes; // the FrameDepth popup, window pixels (x, y, w, h)
    std::vector<std::array<int, 4>> only;  // a gallery's pictures: the only places with depth, window pixels
    std::vector<std::string> outbox;   // replies for the WebSocket thread
    std::atomic<float> fps{0}, warpMs{0}, grabMs{0}, mapsPerSecond{0}, modelMs{0}, depthAgeMs{0};
    std::atomic<float> frameFps{0};     // drawings that carried a new picture from Chrome, per second
    std::atomic<float> videoFps{0};     // the video's own frames per second (the extension's count), 0: none or paused
    std::atomic<int64_t> movingAt{0};   // the extension saw scrolling input (wheel, touch, keys) then
    std::atomic<bool> viewBusy{false};  // the view thread starts or shows a view (and uses the engine)
    std::string problem;                // what keeps the view from working (stats), "" when nothing
    void reply(const std::string& s) { std::lock_guard<std::mutex> l(m); outbox.push_back(s); }
};

// SteamVR's list of overlays. With a time limit: a vrcmd that hung (SteamVR
// starting or stuck) held the view thread for good, and the 3D never started
// (review 2026-10-31). It takes 32-85 ms (headset, 2026-11-01); 2 s is far
// above that, and a run that hit it ends the search (*hung), as 10 tries of
// it made a view start longer than the extension waits for an answer.
static const char* const kListOverlays = "timeout -k 1 2 /opt/steamvr/bin/linuxarm64/vrcmd --overlays 2>/dev/null";
static std::string findOverlayKeyByTitle(const std::string& marker, bool* hung = nullptr) {
    FILE* p = popen(kListOverlays, "r");
    if (!p) return {};
    std::string out; char buf[4096]; size_t r;
    while ((r = std::fread(buf, 1, sizeof(buf), p)) > 0) out.append(buf, r);
    const int status = pclose(p);
    // timeout's own exit codes: 124 timed out, 137 killed after it
    if (hung && WIFEXITED(status) && (WEXITSTATUS(status) == 124 || WEXITSTATUS(status) == 137)) *hung = true;
    // a window of the dashboard (gamescope.*.window.N) or a program started
    // from Steam's library (valve.steam.desktopgame.APPID, 2026-10-29)
    std::regex line("^'(gamescope\\.[^']*\\.window\\.[0-9]+|valve\\.steam\\.desktopgame\\.[0-9]+)' -- '([^\\n]*)', ", std::regex::multiline);
    for (std::sregex_iterator it(out.begin(), out.end(), line), end; it != end; ++it)
        if ((*it)[2].str().find(marker) != std::string::npos) return (*it)[1].str();
    return {};
}

static thread_local int g_xerror = 0;   // per thread: the view loop and the reader each have a connection
static int onXError(Display*, XErrorEvent* e) { g_xerror = e->error_code; return 0; }
// A lost X connection (gamescope's Xwayland restarting): Xlib's default
// handler ended the whole companion with exit(1), its threads still running
// (review 2026-10-31). With an exit handler that returns, every later call
// on that display fails and closing it is safe (libX11 1.8, tried on the
// headset); the view loop then starts the view again.
static int onXIoError(Display*) { return 0; }
static void onXLost(Display*, void* lost) { *static_cast<bool*>(lost) = true; }

// XOpenDisplay with a time limit: a display whose server does not answer
// (an Xwayland that hangs) held the view thread for good (review
// 2026-10-31). An open that takes too long is left to finish on its own
// thread, which closes what it got (with an exit handler that returns: an
// IO error there took Xlib's default exit(1)). While it has not finished,
// that display is skipped rather than opened again, which piled up one stuck
// thread a try (review 2026-11-01). Opening takes under 2 ms (headset,
// 2026-11-01). (libX11 1.8 is thread-safe by default.)
static void onXLostLate(Display*, void*) {}
static Display* openDisplay(const std::string& name, int ms) {
    static std::mutex pendingM;
    static std::vector<std::string> pending;   // displays whose late open still runs
    {
        std::lock_guard<std::mutex> l(pendingM);
        if (std::find(pending.begin(), pending.end(), name) != pending.end()) return nullptr;
    }
    struct Job { std::mutex m; std::condition_variable cv; Display* d = nullptr; bool done = false, late = false; };
    auto job = std::make_shared<Job>();
    std::thread([job, name] {
        Display* d = XOpenDisplay(name.c_str());
        std::lock_guard<std::mutex> l(job->m);
        if (job->late) {
            if (d) { XSetIOErrorExitHandler(d, onXLostLate, nullptr); XCloseDisplay(d); }
            std::lock_guard<std::mutex> p(pendingM);
            pending.erase(std::remove(pending.begin(), pending.end(), name), pending.end());
            return;
        }
        job->d = d; job->done = true;
        job->cv.notify_one();
    }).detach();
    std::unique_lock<std::mutex> l(job->m);
    if (job->cv.wait_for(l, std::chrono::milliseconds(ms), [&] { return job->done; })) return job->d;
    job->late = true;
    { std::lock_guard<std::mutex> p(pendingM); pending.push_back(name); }
    LOG("X display %s did not answer in %d ms", name.c_str(), ms);
    return nullptr;
}

static bool titleHas(Display* d, Window w, const std::string& marker) {
    // Atoms belong to the X server: asked each time, as the server (and so
    // the numbers) change when the SteamVR session restarts
    const Atom netName = XInternAtom(d, "_NET_WM_NAME", False), utf8 = XInternAtom(d, "UTF8_STRING", False);
    Atom type; int fmt; unsigned long n = 0, after = 0; unsigned char* data = nullptr;
    bool hit = false;
    if (XGetWindowProperty(d, w, netName, 0, 4096, False, utf8, &type, &fmt, &n, &after, &data) == Success && data) {
        hit = std::string(reinterpret_cast<char*>(data), n).find(marker) != std::string::npos;
        XFree(data);
    }
    if (!hit) {
        char* name = nullptr;
        if (XFetchName(d, w, &name) && name) { hit = std::string(name).find(marker) != std::string::npos; XFree(name); }
    }
    return hit;
}
static Window findWindow(Display* d, Window w, const std::string& marker, int depth = 0) {
    if (titleHas(d, w, marker)) return w;
    if (depth > 4) return 0;
    Window root, parent, *kids = nullptr; unsigned n = 0;
    if (!XQueryTree(d, w, &root, &parent, &kids, &n)) return 0;
    Window found = 0;
    for (unsigned i = 0; i < n && !found; ++i) found = findWindow(d, kids[i], marker, depth + 1);
    if (kids) XFree(kids);
    return found;
}

// Where the companion's threads run. SteamOS keeps every user service on
// CPUs 0-4 (/etc/systemd/user.conf CPUAffinity), two of them the little
// cores, shared with Chrome's renderers (its video decoding), Xwayland and
// the compositor, and the service ran at nice 5: while a video played the
// helper's depth post-processing took 8-13 ms against 2 ms alone, and the
// view loop's own steps stretched likewise (headset, 2026-11-01). The view
// loop and the helper are light and on every picture's way, so they go to
// the big cores ahead of others; the depth model, which takes cores whole
// for most of each map, gets its own cores and yields to Chrome.
// Measured with a 60 fps 1080p video, the service's start (cores 0-4, nice
// 5) imitated: the helper's work halved (8-9 to 5 ms); the depth model alone
// on 5-7 with the loop on 2-4 ran 5-8 ms slower, a nice 0 model no faster
// (headset, 2026-11-01).
static constexpr unsigned kViewCpus = 0xfc;   // 2-7
static constexpr int kViewNice = -5;
static constexpr unsigned kDepthCpus = 0xfc;
static constexpr int kDepthThreads = 3;
static constexpr int kDepthNice = 5;
static void placeThread(const char* what, unsigned cpus, int nice) {
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int i = 0; i < 32; ++i) if (cpus & (1u << i)) CPU_SET(i, &set);
    if (int e = pthread_setaffinity_np(pthread_self(), sizeof set, &set)) LOG("%s: CPUs 0x%x refused: %s", what, cpus, std::strerror(e));
    if (setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), nice)) LOG("%s: nice %d refused: %s", what, nice, std::strerror(errno));
}

struct Capture {
    Display* d = nullptr;
    Window win = 0;
    XImage* img = nullptr;
    XShmSegmentInfo shm{};
    Visual* visual = nullptr;
    int depth = 0, w = 0, h = 0, winW = 0, winH = 0;
    // Chrome's window reports each change (XDamage), so a frame is read only
    // when there is a new one: half the work for 30 fps video, none while
    // paused. If no reports come although the pixels change (checked by a
    // forced read every 250 ms), damageWorks goes false and every loop reads.
    Damage damage = 0;
    int damageEvent = 0;
    bool damageWorks = true;
    bool lost = false;   // the X connection is gone (onXLost)

    std::string open(const std::string& marker) {
        // Chrome can be on any of gamescope's X displays: the dashboard's
        // (:0), or the one a program started from Steam's library gets (:1;
        // FrameDepth found no window there, 2026-10-29). All are searched.
        // The socket files in /tmp/.X11-unix can be gone while the servers
        // still answer on their abstract sockets (another program's X server
        // removed them, 2026-10-30), so :0 to :3 are always tried as well.
        std::vector<std::string> names;
        const char* env = getenv("DISPLAY");
        names.push_back(env && *env ? env : ":0");
        auto add = [&](const std::string& n) { if (std::find(names.begin(), names.end(), n) == names.end()) names.push_back(n); };
        if (DIR* dir = opendir("/tmp/.X11-unix")) {
            while (const dirent* e = readdir(dir)) {
                if (e->d_name[0] != 'X' || !std::isdigit((unsigned char)e->d_name[1])) continue;
                add(std::string(":") + (e->d_name + 1));
            }
            closedir(dir);
        }
        for (int i = 0; i < 4; ++i) add(":" + std::to_string(i));
        std::vector<Display*> shown;
        lost = false;
        XSetIOErrorHandler(onXIoError);
        for (const auto& n : names) if (Display* x = openDisplay(n, 500)) { XSetIOErrorExitHandler(x, onXLost, &lost); shown.push_back(x); }
        if (shown.empty()) return "cannot open the X display";
        XSetErrorHandler(onXError);
        for (int i = 0; i < 15 && !win && !g_stop; ++i) {
            for (Display* x : shown) if ((win = findWindow(x, DefaultRootWindow(x), marker))) { d = x; break; }
            if (!win) usleep(150 * 1000);
        }
        for (Display* x : shown) if (x != d) XCloseDisplay(x);
        if (!win) return "no X window has the title marker";
        if (!XShmQueryExtension(d)) return "X server has no MIT-SHM";
        XWindowAttributes a{};
        XGetWindowAttributes(d, win, &a);
        visual = a.visual; depth = a.depth; winW = a.width; winH = a.height;
        LOG("X window 0x%lx %dx%d depth %d on %s", win, winW, winH, depth, DisplayString(d));
        int damageError = 0;
        damageWorks = XDamageQueryExtension(d, &damageEvent, &damageError);
        if (damageWorks) damage = XDamageCreate(d, win, XDamageReportNonEmpty);
        else LOG("no DAMAGE extension: reading every frame");
        return {};
    }
    void freeImage() {
        if (img) { XShmDetach(d, &shm); XDestroyImage(img); shmdt(shm.shmaddr); img = nullptr; }
        w = h = 0;
    }
    void close() {
        if (d) { if (damage) XDamageDestroy(d, damage); freeImage(); XCloseDisplay(d); }
        d = nullptr; win = 0; damage = 0;
    }
    // Whether the window changed since the last call
    bool takeDamage() {
        bool got = false;
        while (XPending(d)) {
            XEvent e;
            XNextEvent(d, &e);
            if (damage && e.type == damageEvent + XDamageNotify) got = true;
        }
        if (got) XDamageSubtract(d, damage, None, None);
        return got;
    }

    // Where the mouse pointer is in Chrome's window. SteamVR's laser moves it
    // (that is where clicks go), but its dot is drawn on the window surface,
    // behind our overlay, so we draw our own ring there.
    bool pointer(int& x, int& y) {
        Window root, child; int rx, ry; unsigned mask;
        return XQueryPointer(d, win, &root, &child, &rx, &ry, &x, &y, &mask);
    }
    // Whether one of Chrome's own popups (menus, tooltips, the extension
    // popup, select lists, permission bubbles) is open over the rectangle.
    // They are separate top-level X windows of the same X client as the main
    // window, so their ids share its client base (Xorg's default resource
    // mask 0x1FFFFF; the first popup found is logged). Our opaque overlay sits
    // in front of Chrome's, so while one is open only Chrome's flat picture
    // may show. Only popup-like windows count: override-redirect, transient
    // for the main window, or a menu/tooltip/notification/dialog window type.
    // Chrome's other normal windows (a second window, DevTools, picture in
    // picture) also lie at 0,0 in gamescope and must not pause the 3D.
    bool popupLike(Window w, const XWindowAttributes& a) {
        if (a.override_redirect) return true;
        Window owner = 0;
        if (XGetTransientForHint(d, w, &owner) && owner == win) return true;
        static const char* const kTypes[] = {"_NET_WM_WINDOW_TYPE_MENU", "_NET_WM_WINDOW_TYPE_POPUP_MENU",
            "_NET_WM_WINDOW_TYPE_DROPDOWN_MENU", "_NET_WM_WINDOW_TYPE_TOOLTIP", "_NET_WM_WINDOW_TYPE_NOTIFICATION",
            "_NET_WM_WINDOW_TYPE_COMBO", "_NET_WM_WINDOW_TYPE_DIALOG", "_NET_WM_WINDOW_TYPE_UTILITY"};
        Atom type; int format; unsigned long count = 0, after = 0; unsigned char* data = nullptr;
        bool popup = false;
        if (XGetWindowProperty(d, w, XInternAtom(d, "_NET_WM_WINDOW_TYPE", False), 0, 8, False, XA_ATOM,
                               &type, &format, &count, &after, &data) == Success && data && format == 32) {
            const Atom* atoms = reinterpret_cast<const Atom*>(data);
            for (unsigned long i = 0; i < count && !popup; ++i)
                for (const char* name : kTypes) if (atoms[i] == XInternAtom(d, name, False)) { popup = true; break; }
        }
        if (data) XFree(data);
        return popup;
    }
    // Chrome's popup windows over the rectangle, in window pixels (x, y, w, h)
    std::vector<std::array<int, 4>> popupRects(const Rect& r) {
        std::vector<std::array<int, 4>> out;
        Window root, parent, *kids = nullptr; unsigned n = 0;
        if (!XQueryTree(d, DefaultRootWindow(d), &root, &parent, &kids, &n)) return out;
        const unsigned long base = win & ~0x1FFFFFUL;
        for (unsigned i = 0; i < n; ++i) {
            if (kids[i] == win || (kids[i] & ~0x1FFFFFUL) != base) continue;
            XWindowAttributes a{};
            g_xerror = 0;
            if (!XGetWindowAttributes(d, kids[i], &a) || g_xerror || a.map_state != IsViewable || a.width < 20 || a.height < 20) continue;
            if (!popupLike(kids[i], a) || g_xerror) continue;
            int x = 0, y = 0; Window child;
            if (!XTranslateCoordinates(d, kids[i], win, 0, 0, &x, &y, &child)) continue;
            if (!(x < r.x + r.w && x + a.width > r.x && y < r.y + r.h && y + a.height > r.y)) continue;
            out.push_back({x, y, (int)a.width, (int)a.height});
            if (kids[i] != loggedPopup) { loggedPopup = kids[i]; LOG("popup 0x%lx %dx%d at %d,%d over the video: see-through there", kids[i], a.width, a.height, x, y); }
        }
        if (kids) XFree(kids);
        return out;
    }
    Window loggedPopup = 0;
    // BGRA rows of the rectangle (clamped to the window); nullptr on failure
    const uint8_t* grab(Rect& r, int& stride) {
        XWindowAttributes a{};
        if (!XGetWindowAttributes(d, win, &a)) return nullptr;
        winW = a.width; winH = a.height;
        // a part left or above the window is cut off, not moved in: x -100
        // w 800 read 800 from 0 (review 2026-11-01)
        if (r.x < 0) { r.w += r.x; r.x = 0; }
        if (r.y < 0) { r.h += r.y; r.y = 0; }
        r.x = std::min(r.x, winW - 1); r.y = std::min(r.y, winH - 1);
        r.w = std::min(r.w, winW - r.x); r.h = std::min(r.h, winH - r.y);
        if (r.w < 16 || r.h < 16) return nullptr;
        if (r.w != w || r.h != h) {
            freeImage();
            img = XShmCreateImage(d, visual, depth, ZPixmap, nullptr, &shm, r.w, r.h);
            if (!img) return nullptr;
            shm.shmid = shmget(IPC_PRIVATE, (size_t)img->bytes_per_line * r.h, IPC_CREAT | 0600);
            void* at = shm.shmid < 0 ? (void*)-1 : shmat(shm.shmid, nullptr, 0);
            if (at == (void*)-1) {
                // out of shared memory: not kept, so the next grab tries again
                if (shm.shmid >= 0) shmctl(shm.shmid, IPC_RMID, nullptr);
                XDestroyImage(img); img = nullptr;
                return nullptr;
            }
            shm.shmaddr = img->data = static_cast<char*>(at);
            shm.readOnly = False;
            XShmAttach(d, &shm); XSync(d, False);
            shmctl(shm.shmid, IPC_RMID, nullptr);
            w = r.w; h = r.h;
        }
        g_xerror = 0;
        if (!XShmGetImage(d, win, img, r.x, r.y, AllPlanes) || g_xerror) return nullptr;
        stride = img->bytes_per_line;
        return reinterpret_cast<const uint8_t*>(img->data);
    }
};

// Reads Chrome's window on a thread of its own, with its own X connection.
// Zero-copy where it can (the usual case): instead of copying the pixels
// out (XShm: Xwayland copies the window from the GPU, 3-10 ms at 1080p, and
// the loop uploads them back, 1 ms), it hands over the window's own buffer
// (XComposite names the window's pixmap, DRI3 gives its dma-buf) for the GPU
// to import (GpuWarp::useWindowBuffer). Xwayland draws the window into a few
// buffers in turn, so the pixmap is named anew for each frame: one buffer
// named once showed the frame drawn into it, an older one a third of the
// time (headset, 2026-11-01). Which frames are new comes from the damage
// region (only damage inside the picture counts) instead of a checksum.
// XShm stays for when the X server cannot (no Composite or DRI3 1.2).
// In the view loop a 1080p read took 9-10 ms, during which nothing was
// drawn or handed over: 60 fps video got 44-54 pictures drawn a second and
// 27-56 in 5 s missed their refresh (headset, 2026-11-01). The reader waits
// for Chrome's damage reports (its connection's socket, so a new frame is
// read at once rather than at the loop's next 4 ms tick), reads into one of
// three images (one the loop holds, one the newest, one being written) and
// wakes the loop. The loop keeps its own connection for the pointer and
// Chrome's popups. Reads as before: on damage, when the rectangle changes,
// when asked (force), and every 250 ms regardless, which also checks that
// damage reports come at all.
struct Reader {
    struct Frame {
        const uint8_t* px = nullptr;   // XShm reads
        int stride = 0;
        // zero-copy reads: the window's dma-buf (the taker closes fd), its
        // size, layout and identity
        int fd = -1, bw = 0, bh = 0;
        uint32_t bstride = 0, boffset = 0;
        uint64_t modifier = 0, key = 0;
        Rect asked, r;          // the rectangle asked for, and as read (clamped to the window)
        int winW = 0, winH = 0;
        int64_t at = 0;         // when the read finished
        double readMs = 0;
        uint64_t hash = 0, seq = 0;
        bool failed = false;
    };
    std::atomic<bool> lost{false}, damageWorks{true}, zeroCopy{false};
    // No more zero-copy reads for this view (the GPU could not import the
    // window's buffer, or its damage reports missed the picture)
    void refuseZeroCopy(const char* why) {
        if (zeroCopyRefused.exchange(true)) return;
        zeroCopy = false;
        LOG("%s: reading Chrome's window with XShm", why);
    }

    std::string start(const std::string& display, Window window) {
        stop();
        d = openDisplay(display, 500);
        if (!d) return "cannot open a second X connection";
        lostFlag = false;
        XSetIOErrorExitHandler(d, onXLost, &lostFlag);
        win = window;
        if (!XShmQueryExtension(d)) { close(); return "X server has no MIT-SHM"; }
        XWindowAttributes a{};
        if (!XGetWindowAttributes(d, win, &a)) { close(); return "Chrome's window is gone"; }
        visual = a.visual; depth = a.depth;
        int damageError = 0;
        damageWorks = XDamageQueryExtension(d, &damageEvent, &damageError);
        damage = damageWorks ? XDamageCreate(d, win, XDamageReportNonEmpty) : 0;
        XSync(d, False);
        zeroCopy = false; zeroCopyRefused = false; canZeroCopy = false; retryAt = 0;
        int cEvent, cError, fEvent, fError;
        xc = XGetXCBConnection(d);
        if (damage && XCompositeQueryExtension(d, &cEvent, &cError) && XFixesQueryExtension(d, &fEvent, &fError)) {
            xcb_dri3_query_version_reply_t* v = xcb_dri3_query_version_reply(xc, xcb_dri3_query_version(xc, 1, 2), nullptr);
            canZeroCopy = v && (v->major_version > 1 || v->minor_version >= 2);
            free(v);
        }
        if (canZeroCopy) {
            region = XFixesCreateRegion(d, nullptr, 0);
            Frame probe;
            if (windowBuffer(probe)) { ::close(probe.fd); zeroCopy = true; }
            else retryAt = mlxr_depth_now_ns() + kRetryNs;
        }
        LOG("reading Chrome's window %s", zeroCopy ? "zero-copy (its own GPU buffers)" : "with XShm");
        wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        quit = false; seq = 0; newest = held = -1; force = true; misses = 0; lastHash = 0; lastAt = 0;
        lost = false;
        t = std::thread([this] { run(); });
        return {};
    }
    void stop() {
        if (t.joinable()) {
            { std::lock_guard<std::mutex> l(m); quit = true; }
            poke();
            cv.notify_all();
            t.join();
        }
        close();
    }
    ~Reader() { stop(); }
    // The rectangle to read (window pixels); force: read it at once
    void want(const Rect& r, bool now) {
        bool changed;
        {
            std::lock_guard<std::mutex> l(m);
            changed = r.x != asked.x || r.y != asked.y || r.w != asked.w || r.h != asked.h;
            asked = r;
            if (now) force = true;
        }
        if (changed || now) poke();
    }
    // The newest frame if newer than haveSeq; the loop holds it (its pixels
    // stay put) until it takes another
    bool take(Frame& f, uint64_t haveSeq) {
        std::lock_guard<std::mutex> l(m);
        if (newest < 0 || frames[newest].seq <= haveSeq) return false;
        held = newest;
        f = frames[newest];
        frames[newest].fd = -1;   // the loop's now
        return true;
    }
    // Waits up to ns for a frame newer than haveSeq, or a nudge
    void waitNew(uint64_t haveSeq, int64_t ns) {
        std::unique_lock<std::mutex> l(m);
        cv.wait_for(l, std::chrono::nanoseconds(ns), [&] { return quit || nudged || (newest >= 0 && frames[newest].seq > haveSeq); });
        nudged = false;
    }
    // Ends the loop's wait early (the helper finished a depth map)
    void nudge() {
        { std::lock_guard<std::mutex> l(m); nudged = true; }
        cv.notify_all();
    }

private:
    Display* d = nullptr;
    xcb_connection_t* xc = nullptr;
    // Zero-copy: whether the X server can, whether this view may (see
    // refuseZeroCopy), and when to try again after a read that could not
    // (the window resized or going away): those used to leave XShm for the
    // rest of the view (review 2026-11-02)
    bool canZeroCopy = false, nudged = false;
    std::atomic<bool> zeroCopyRefused{false};
    int64_t retryAt = 0;
    static constexpr int64_t kRetryNs = 2000000000LL;
    XserverRegion region = 0;
    uint64_t content = 0;   // zero-copy: counts damage inside the picture (the frame's hash)
    Window win = 0;
    Visual* visual = nullptr;
    int depth = 0, damageEvent = 0;
    Damage damage = 0;
    bool lostFlag = false;
    int wake = -1;
    std::thread t;
    std::mutex m;
    std::condition_variable cv;
    bool quit = false, force = true;
    Rect asked, lastAsked;
    struct Image { XImage* img = nullptr; XShmSegmentInfo shm{}; int w = 0, h = 0; } images[3];
    Frame frames[3];
    int newest = -1, held = -1;
    uint64_t seq = 0, lastHash = 0;
    int misses = 0;
    int64_t lastAt = 0;

    void poke() { if (wake >= 0) { const uint64_t one = 1; if (write(wake, &one, sizeof one) < 0) {} } }
    void freeImage(Image& im) {
        if (im.img) { XShmDetach(d, &im.shm); XDestroyImage(im.img); shmdt(im.shm.shmaddr); im.img = nullptr; }
        im.w = im.h = 0;
    }
    void close() {
        if (d) {
            for (Image& im : images) freeImage(im);
            if (damage) XDamageDestroy(d, damage);
            if (region) XFixesDestroyRegion(d, region);
            XCloseDisplay(d);
        }
        for (Frame& f : frames) if (f.fd >= 0) { ::close(f.fd); f.fd = -1; }
        d = nullptr; damage = 0; region = 0; xc = nullptr;
        if (wake >= 0) { ::close(wake); wake = -1; }
    }
    bool sized(Image& im, int w, int h) {
        if (im.img && im.w == w && im.h == h) return true;
        freeImage(im);
        im.img = XShmCreateImage(d, visual, depth, ZPixmap, nullptr, &im.shm, w, h);
        if (!im.img) return false;
        im.shm.shmid = shmget(IPC_PRIVATE, (size_t)im.img->bytes_per_line * h, IPC_CREAT | 0600);
        void* at = im.shm.shmid < 0 ? (void*)-1 : shmat(im.shm.shmid, nullptr, 0);
        if (at == (void*)-1) {
            // out of shared memory: not kept, so the next read tries again
            if (im.shm.shmid >= 0) shmctl(im.shm.shmid, IPC_RMID, nullptr);
            XDestroyImage(im.img); im.img = nullptr;
            return false;
        }
        im.shm.shmaddr = im.img->data = static_cast<char*>(at);
        im.shm.readOnly = False;
        XShmAttach(d, &im.shm); XSync(d, False);
        shmctl(im.shm.shmid, IPC_RMID, nullptr);
        im.w = w; im.h = h;
        return true;
    }
    // Whether the window changed since the last call; inside: whether any of
    // it was inside rectangle r (XShm reads: whether it changed at all)
    bool takeDamage(const Rect& r, bool* inside = nullptr) {
        bool got = false;
        while (XPending(d)) {
            XEvent e;
            XNextEvent(d, &e);
            if (damage && e.type == damageEvent + XDamageNotify) got = true;
        }
        if (inside) *inside = got;
        if (!got) return false;
        if (zeroCopy && region) {
            XDamageSubtract(d, damage, None, region);
            int n = 0;
            XRectangle* parts = XFixesFetchRegion(d, region, &n);
            if (inside) {
                *inside = false;
                for (int i = 0; i < n && !*inside; ++i)
                    *inside = parts[i].x < r.x + r.w && parts[i].x + parts[i].width > r.x && parts[i].y < r.y + r.h && parts[i].y + parts[i].height > r.y;
            }
            if (parts) XFree(parts);
        } else XDamageSubtract(d, damage, None, None);
        return got;
    }
    // The window's current buffer (its pixmap named now): fd, size, layout
    bool windowBuffer(Frame& f) {
        g_xerror = 0;
        const Pixmap pix = XCompositeNameWindowPixmap(d, win);
        XSync(d, False);
        if (g_xerror) return false;
        xcb_dri3_buffers_from_pixmap_reply_t* b = xcb_dri3_buffers_from_pixmap_reply(xc, xcb_dri3_buffers_from_pixmap(xc, pix), nullptr);
        XFreePixmap(d, pix);
        if (!b) return false;
        int* fds = xcb_dri3_buffers_from_pixmap_reply_fds(xc, b);
        // one plane of 8 bits a channel (a 30 bit window would be read as
        // wrong colours by the ARGB8888 import)
        const bool ok = b->nfd == 1 && b->bpp == 32 && (b->depth == 24 || b->depth == 32);
        if (ok) {
            struct stat st{};
            fstat(fds[0], &st);
            f.fd = fds[0]; f.bw = b->width; f.bh = b->height; f.modifier = b->modifier; f.key = st.st_ino;
            f.bstride = xcb_dri3_buffers_from_pixmap_strides(b)[0]; f.boffset = xcb_dri3_buffers_from_pixmap_offsets(b)[0];
        }
        for (int i = ok ? 1 : 0; i < b->nfd; ++i) ::close(fds[i]);
        free(b);
        return ok;
    }
    void publish(const Frame& f, int b) {
        {
            std::lock_guard<std::mutex> l(m);
            if (frames[b].fd >= 0) ::close(frames[b].fd);   // never taken
            frames[b] = f;
            frames[b].seq = ++seq;
            newest = b;
        }
        cv.notify_all();
    }
    void run() {
        placeThread("reader", kViewCpus, kViewNice);
        bool carried = false;
        for (;;) {
            Rect want; bool now;
            {
                std::lock_guard<std::mutex> l(m);
                if (quit) return;
                want = asked; now = force; force = false;
            }
            if (lostFlag) { lost = true; std::unique_lock<std::mutex> l(m); cv.wait_for(l, std::chrono::milliseconds(50), [&] { return quit; }); continue; }
            int64_t t0 = mlxr_depth_now_ns();
            bool inside = false;
            bool damaged = takeDamage(want, &inside) || carried;
            inside = inside || carried;
            carried = false;
            if (damaged) {
                // A frame's damage comes in parts: read at the first, the
                // picture was often caught before the rest and read again a
                // frame later; reads more than 1.5 frames apart went from
                // 14-35 to 42-87 in 5 s, and pictures shown for 3 refreshes
                // (60 fps on 90 Hz: 1 and 2) from 0-2 to 3-19. Waiting 2 ms
                // for the rest: 0-2 and 0-1 (headset, 2026-11-01).
                usleep(2000);
                bool more = false;
                takeDamage(want, &more);
                inside = inside || more;
                t0 = mlxr_depth_now_ns();
            }
            const bool moved = want.x != lastAsked.x || want.y != lastAsked.y || want.w != lastAsked.w || want.h != lastAsked.h;
            const bool due = t0 - lastAt > 250000000LL;
            if (!zeroCopy && canZeroCopy && !zeroCopyRefused && retryAt && t0 >= retryAt && want.w > 0) {
                Frame probe;
                if (windowBuffer(probe)) { ::close(probe.fd); zeroCopy = true; retryAt = 0; LOG("reading Chrome's window zero-copy again"); }
                else retryAt = t0 + kRetryNs;
            }
            if (zeroCopy && want.w > 0 && want.h > 0 && (damaged || moved || now || due)) {
                int b = 0;
                {
                    std::lock_guard<std::mutex> l(m);
                    while (b == held || b == newest) ++b;
                }
                Frame f;
                f.asked = want;
                const bool ok = windowBuffer(f);
                const int64_t t1 = mlxr_depth_now_ns();
                lastAt = t1; lastAsked = want;
                f.at = t1; f.readMs = (t1 - t0) / 1e6;
                if (!ok) {
                    // not now (the window going away or resized): XShm until
                    // it can again
                    zeroCopy = false; retryAt = t1 + kRetryNs;
                    LOG("zero-copy read failed: reading with XShm, trying again in %.0f s", kRetryNs / 1e9);
                    continue;
                }
                f.winW = f.bw; f.winH = f.bh;
                Rect r = want;
                if (r.x < 0) { r.w += r.x; r.x = 0; }
                if (r.y < 0) { r.h += r.y; r.y = 0; }
                r.x = std::min(r.x, f.winW - 1); r.y = std::min(r.y, f.winH - 1);
                r.w = std::min(r.w, f.winW - r.x); r.h = std::min(r.h, f.winH - r.y);
                if (r.w < 16 || r.h < 16) { ::close(f.fd); f.fd = -1; f.failed = true; publish(f, b); continue; }
                f.r = r;
                // a new picture when something inside it changed (or the rectangle did)
                if (inside || moved || !content) ++content;
                f.hash = content;
                publish(f, b);
            } else if (!zeroCopy && want.w > 0 && want.h > 0 && (damaged || moved || now || due || !damageWorks)) {
                // into the image neither the loop holds nor the newest is in
                int b = 0;
                {
                    std::lock_guard<std::mutex> l(m);
                    while (b == held || b == newest) ++b;
                }
                Frame f;
                f.asked = want;
                Rect r = want;
                XWindowAttributes a{};
                g_xerror = 0;
                bool ok = XGetWindowAttributes(d, win, &a) && !g_xerror;
                if (ok) {
                    f.winW = a.width; f.winH = a.height;
                    // a part left or above the window is cut off, not moved
                    // in: x -100 w 800 read 800 from 0 (review 2026-11-01)
                    if (r.x < 0) { r.w += r.x; r.x = 0; }
                    if (r.y < 0) { r.h += r.y; r.y = 0; }
                    r.x = std::min(r.x, f.winW - 1); r.y = std::min(r.y, f.winH - 1);
                    r.w = std::min(r.w, f.winW - r.x); r.h = std::min(r.h, f.winH - r.y);
                    ok = r.w >= 16 && r.h >= 16 && sized(images[b], r.w, r.h);
                }
                if (ok) {
                    g_xerror = 0;
                    ok = XShmGetImage(d, win, images[b].img, r.x, r.y, AllPlanes) && !g_xerror;
                }
                const int64_t t1 = mlxr_depth_now_ns();
                lastAt = t1; lastAsked = want;
                f.r = r; f.at = t1; f.readMs = (t1 - t0) / 1e6;
                if (!ok) { f.failed = true; publish(f, b); }
                else {
                    f.px = reinterpret_cast<const uint8_t*>(images[b].img->data);
                    f.stride = images[b].img->bytes_per_line;
                    // a sparse checksum of the picture: did it really change?
                    // 96 x 64 points (every 20 px across 1920): 32 x 24
                    // (every 60) could miss a small animation in a gallery,
                    // which then kept its old picture (review 2026-10-31)
                    uint64_t h = 1469598103934665603ULL;
                    for (int y = 0; y < r.h; y += std::max(1, r.h / 64))
                        for (int x = 0; x < r.w; x += std::max(1, r.w / 96)) {
                            uint32_t px; std::memcpy(&px, f.px + (size_t)y * f.stride + (size_t)x * 4, 4);
                            h = (h ^ px) * 1099511628211ULL;
                        }
                    f.hash = h;
                    if (!damaged && !moved && lastHash && h != lastHash && damageWorks) {
                        // The report for a frame drawn just after the check
                        // above comes in by now: not a miss (the next round
                        // reads again). Damage is given up only after three
                        // real misses in a row.
                        if (takeDamage(want)) carried = true;
                        else if (++misses >= 3) {
                            damageWorks = false;
                            LOG("Chrome's window changed without a damage report: reading every frame");
                        }
                    } else if (damaged) misses = 0;
                    lastHash = h;
                    publish(f, b);
                }
                if (carried) continue;
            }
            // wait for a damage report, a wish from the loop, or the 250 ms
            // read; without damage reports every 4 ms
            if (XPending(d)) continue;
            const int64_t left = damageWorks ? std::max<int64_t>(1000000, 250000000LL - (mlxr_depth_now_ns() - lastAt)) : 4000000;
            pollfd fds[2] = {{ConnectionNumber(d), POLLIN, 0}, {wake, POLLIN, 0}};
            poll(fds, 2, (int)std::min<int64_t>(left / 1000000, 250));
            if (fds[1].revents & POLLIN) { uint64_t n; if (read(wake, &n, sizeof n) < 0) {} }
        }
    }
};

// The shaders take 8 flat and 8 see-through rectangles: beyond that the two
// closest (x0, y0, x1, y1 each) become the box around both. The subtitle box
// with 8 player rectangles lost the last of these (review 2026-10-31).
// w: each rectangle's weight (flat ones), the larger kept for a joined pair
static void joinToEight(std::vector<float>& v, std::vector<float>* w = nullptr) {
    while (v.size() > 32) {
        size_t bi = 0, bj = 4; float bg = INFINITY;
        for (size_t i = 0; i < v.size(); i += 4)
            for (size_t j = i + 4; j < v.size(); j += 4) {
                const float g = std::max({0.0f, v[i] - v[j + 2], v[j] - v[i + 2]}) + std::max({0.0f, v[i + 1] - v[j + 3], v[j + 1] - v[i + 3]});
                if (g < bg) { bg = g; bi = i; bj = j; }
            }
        v[bi] = std::min(v[bi], v[bj]); v[bi + 1] = std::min(v[bi + 1], v[bj + 1]);
        v[bi + 2] = std::max(v[bi + 2], v[bj + 2]); v[bi + 3] = std::max(v[bi + 3], v[bj + 3]);
        v.erase(v.begin() + bj, v.begin() + bj + 4);
        if (w) { (*w)[bi / 4] = std::max((*w)[bi / 4], (*w)[bj / 4]); w->erase(w->begin() + bj / 4); }
    }
}

// Our overlay, kept for the life of the process. Stopping a view only hides
// it and clears its texture: tearing the OpenVR connection down while the
// compositor still used our image crashed vrcompositor once (SetOverlayRaw,
// 2026-10-25), so the overlay is destroyed and OpenVR shut down only at exit,
// after a pause.
struct VrView {
    vr::IVROverlay* o = nullptr;
    vr::VROverlayHandle_t view = vr::k_ulOverlayHandleInvalid;
    bool shown = false;
    std::string init() {
        if (o) return {};
        vr::EVRInitError ie = vr::VRInitError_None;
        vr::VR_Init(&ie, vr::VRApplication_Overlay);
        if (ie != vr::VRInitError_None) return std::string("OpenVR: ") + vr::VR_GetVRInitErrorAsEnglishDescription(ie);
        o = vr::VROverlay();
        const std::string key = "framedepth.view." + std::to_string(getpid());
        if (o->CreateOverlay(key.c_str(), "FrameDepth", &view) != vr::VROverlayError_None) {
            // no overlay of ours yet, so closing is safe: left open, the next
            // init saw o set and reported success with no overlay, and the
            // 3D never showed (review 2026-10-31)
            vr::VR_Shutdown();
            o = nullptr; view = vr::k_ulOverlayHandleInvalid;
            return "cannot create the overlay";
        }
        o->SetOverlayFlag(view, vr::VROverlayFlags_SideBySide_Parallel, true);
        // Each eye's half is a whole, unsqueezed picture (full SBS): square
        // texels. Checked on the headset 2026-10-25 (--calib: 1.0 matched the
        // video and kept a circle round; 2.0 did not).
        o->SetOverlayTexelAspect(view, 1.0f);
        o->SetOverlaySortOrder(view, 100);
        o->SetOverlayInputMethod(view, vr::VROverlayInputMethod_None);
        return {};
    }
    void hide() {
        if (!o || view == vr::k_ulOverlayHandleInvalid) return;
        o->HideOverlay(view);
        if (shown) o->ClearOverlayTexture(view);
        shown = false;
    }
    vr::EVROverlayError submit(unsigned glTexture) {
        vr::Texture_t tex{reinterpret_cast<void*>(static_cast<uintptr_t>(glTexture)), vr::TextureType_OpenGL, vr::ColorSpace_Auto};
        return o->SetOverlayTexture(view, &tex);
    }
    // Where Chrome's window really is. GetTransformForOverlayCoordinates
    // describes a flat plane, but SteamVR draws dashboard windows either
    // curved around the user (headset, 2026-10-25: radius 1.09 m) or flat,
    // switchable by the user, and slightly tilted. The intersection test is
    // the truth: it returns the uv and the 3D point a ray really hits. So:
    // aim rays from the HMD through the flat map at the window's left,
    // middle and right (middle row) and its bottom and top (middle column):
    //   up      = the window's own vertical (bottom -> top hit), not world up
    //   curved: a cylinder around `up`: centre, radius, angle(u) = ta + tb*u
    //   flat:   d(point)/du, made perpendicular to up
    struct Shape {
        bool valid = false, flat = false;
        float up[3] = {0, 1, 0}, e1[3] = {1, 0, 0}, e2[3] = {0, 0, 1};
        float centre[3] = {0, 0, 0}, radius = 0, ta = 0, tb = 0;
        float ref[3] = {0, 0, 0}, uRef = 0.5f, vRef = 0.5f, perV = 0;   // middle hit, metres per v
        float ux[3] = {0, 0, 0};                                         // flat: metres per u
    } shape;
    int shapeAge = 1 << 30;
    static float dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
    static void cross3(const float* a, const float* b, float* o) {
        o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0];
    }
    static float norm3(float* a) { const float l = std::sqrt(dot3(a, a)); if (l > 1e-9f) for (int k = 0; k < 3; ++k) a[k] /= l; return l; }
    bool hit(vr::VROverlayHandle_t h, const float eye[3], const float aim[3], vr::VROverlayIntersectionResults_t& res) {
        float d[3] = {aim[0] - eye[0], aim[1] - eye[1], aim[2] - eye[2]};
        if (norm3(d) < 1e-4f) return false;
        vr::VROverlayIntersectionParams_t ip{};
        ip.vSource = {{eye[0], eye[1], eye[2]}};
        ip.vDirection = {{d[0], d[1], d[2]}};
        ip.eOrigin = vr::TrackingUniverseStanding;
        return o->ComputeOverlayIntersection(h, &ip, &res);
    }
    void eyePosition(float eye[3]) {
        vr::TrackedDevicePose_t pose[1];
        vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, pose, 1);
        for (int k = 0; k < 3; ++k) eye[k] = pose[0].mDeviceToAbsoluteTracking.m[k][3];
    }
    // The user can grab a window by its handles and move, tilt, roll or
    // resize it, and switch it between curved and flat, at any time. Every
    // frame a cheap check (two calls: where the flat map puts two opposite
    // corners) catches moves, rotations and resizes and re-measures at
    // once; a full re-measure also runs every 15 frames for changes the
    // flat map does not show (curved <-> flat).
    float lastKey[7] = {0, 0, 0, 0, 0, 0, 0};
    int64_t movedAt = 0;   // last time the window moved, turned or resized
    bool measure(vr::VROverlayHandle_t parent) {
        vr::HmdVector2_t scale{};
        o->GetOverlayMouseScale(parent, &scale);
        float key[7];
        for (int i = 0; i < 2; ++i) {
            vr::HmdMatrix34_t m{}; vr::HmdVector2_t c{{i * scale.v[0], i * scale.v[1]}};
            o->GetTransformForOverlayCoordinates(parent, vr::TrackingUniverseStanding, c, &m);
            for (int k = 0; k < 3; ++k) key[i * 3 + k] = m.m[k][3];
        }
        key[6] = scale.v[0] * 10000.0f + scale.v[1];
        bool moved = false;
        for (int k = 0; k < 6; ++k) moved |= std::fabs(key[k] - lastKey[k]) > 0.001f;
        moved |= key[6] != lastKey[6];
        if (moved && shape.valid) movedAt = mlxr_depth_now_ns();
        if (!moved && ++shapeAge < 15 && shape.valid) return true;
        shapeAge = 0;
        std::copy(key, key + 7, lastKey);
        float eye[3];
        eyePosition(eye);
        // Aim at the window's edges first; right after a curved <-> flat
        // switch the flat map can lie off the drawn window and those rays
        // miss, so try again closer to the middle.
        float uv[5][2], p[5][3], du = 0, dv = 0;
        bool ok = false;
        for (float spread : {1.0f, 0.5f, 0.2f}) {
            const float lo = 0.5f - spread / 2, hi = 0.5f + spread / 2;
            const float aims[5][2] = {{lo, 0.5f}, {0.5f, 0.5f}, {hi, 0.5f}, {0.5f, lo}, {0.5f, hi}};
            ok = true;
            for (int i = 0; i < 5 && ok; ++i) {
                vr::HmdMatrix34_t m{}; vr::HmdVector2_t c{{aims[i][0] * scale.v[0], aims[i][1] * scale.v[1]}};
                o->GetTransformForOverlayCoordinates(parent, vr::TrackingUniverseStanding, c, &m);
                const float aim[3] = {m.m[0][3], m.m[1][3], m.m[2][3]};
                vr::VROverlayIntersectionResults_t res{};
                if (!hit(parent, eye, aim, res)) { ok = false; break; }
                uv[i][0] = res.vUVs.v[0]; uv[i][1] = res.vUVs.v[1];
                for (int k = 0; k < 3; ++k) p[i][k] = res.vPoint.v[k];
            }
            if (!ok) continue;
            du = uv[2][0] - uv[0][0]; dv = uv[4][1] - uv[3][1];
            if (std::fabs(du) >= 0.05f * spread && std::fabs(dv) >= 0.05f * spread) break;
            ok = false;
        }
        // Moved and could not measure: better hidden than shown in the old place
        if (!ok) {
            if (moved) { shape.valid = false; LOG("window shape: could not measure after a change; view hidden until it can"); }
            return shape.valid;
        }
        Shape s;
        for (int k = 0; k < 3; ++k) { s.up[k] = p[4][k] - p[3][k]; s.ref[k] = p[1][k]; }
        s.perV = norm3(s.up) / dv;
        s.uRef = uv[1][0]; s.vRef = uv[1][1];
        // the middle row, flattened onto the plane across `up` through the middle hit
        float q[3][3];
        for (int i = 0; i < 3; ++i) {
            float rel[3] = {p[i][0] - p[1][0], p[i][1] - p[1][1], p[i][2] - p[1][2]};
            const float along = dot3(rel, s.up);
            for (int k = 0; k < 3; ++k) q[i][k] = rel[k] - along * s.up[k];
        }
        for (int k = 0; k < 3; ++k) s.e1[k] = q[2][k] - q[0][k];
        const float chord = norm3(s.e1);
        cross3(s.e1, s.up, s.e2);   // right x up = towards the user
        norm3(s.e2);
        const float x0 = dot3(q[0], s.e1), y0 = dot3(q[0], s.e2), x2 = dot3(q[2], s.e1), y2 = dot3(q[2], s.e2);
        // circle through (x0, y0), (0, 0), (x2, y2)
        const float det = 2 * (x0 * y2 - x2 * y0);
        if (std::fabs(det) < 1e-4f * chord * chord) {
            s.flat = true;
        } else {
            const float a2 = x0 * x0 + y0 * y0, c2 = x2 * x2 + y2 * y2;
            const float cxl = (a2 * y2 - c2 * y0) / det, cyl = (c2 * x0 - a2 * x2) / det;
            s.radius = std::hypot(cxl, cyl);
            s.flat = s.radius > 30.0f;
            if (!s.flat) {
                for (int k = 0; k < 3; ++k) s.centre[k] = s.ref[k] + cxl * s.e1[k] + cyl * s.e2[k];
                float t0 = std::atan2(y0 - cyl, x0 - cxl), t2 = std::atan2(y2 - cyl, x2 - cxl);
                if (t2 - t0 > (float)M_PI) t2 -= 2 * (float)M_PI;
                if (t0 - t2 > (float)M_PI) t2 += 2 * (float)M_PI;
                s.tb = (t2 - t0) / du;
                s.ta = t0 - s.tb * uv[0][0];
            }
        }
        if (s.flat) {
            s.radius = 0;
            for (int k = 0; k < 3; ++k) s.ux[k] = (q[2][k] - q[0][k]) / du;
        }
        s.valid = true;
        if (!shape.valid || s.flat != shape.flat || (!s.flat && std::fabs(s.radius - shape.radius) > 0.02f))
            LOG("window shape: %s%s, height %.3f m, up (%.2f %.2f %.2f)", s.flat ? "flat" : "curved, radius ",
                s.flat ? "" : std::to_string(s.radius).c_str(), (double)s.perV, (double)s.up[0], (double)s.up[1], (double)s.up[2]);
        shape = s;
        return true;
    }
    // Point on the window surface at (u, v), optionally moved `inset` metres
    // towards the user; tangent (direction of increasing u) on request.
    void surface(float u, float v, float inset, float* pt, float* tangent) const {
        const Shape& s = shape;
        if (s.flat) {
            float n[3];
            cross3(s.ux, s.up, n); norm3(n);
            for (int k = 0; k < 3; ++k) pt[k] = s.ref[k] + s.ux[k] * (u - s.uRef) + s.up[k] * s.perV * (v - s.vRef) + n[k] * inset;
            if (tangent) { for (int k = 0; k < 3; ++k) tangent[k] = s.ux[k]; norm3(tangent); }
            return;
        }
        const float th = s.ta + s.tb * u, rad = s.radius - inset;
        const float c = std::cos(th), sn = std::sin(th);
        for (int k = 0; k < 3; ++k) pt[k] = s.centre[k] + rad * (c * s.e1[k] + sn * s.e2[k]) + s.up[k] * s.perV * (v - s.vRef);
        if (tangent) {
            const float sgn = s.tb > 0 ? 1.0f : -1.0f;
            for (int k = 0; k < 3; ++k) tangent[k] = sgn * (-sn * s.e1[k] + c * s.e2[k]);
        }
    }
    // Place the overlay over rectangle r (window pixels, top-left origin)
    // on the window's own surface, curved like it. It sits only 1 mm towards
    // the user: 1 cm showed as a visible sideways shift when the window is
    // turned and seen at an angle (headset, 2026-10-25). Our sort order (100
    // against the window's 0) keeps it drawn on top.
    static constexpr float kInset = 0.001f;
    float placedCurvature = -1;
    float placedWidth = 0, placedPos[3] = {0, 0, 0};   // the last placement (metres), for --dump
    int winW = 0, winH = 0;   // Chrome's X window, pixels (set by the loop)
    // Window pixels to the parent overlay's coordinates (0..1, top first). A
    // dashboard window's overlay is the window itself; a program started
    // from Steam's library is shown scaled to fill its display (aspect
    // kept), so its 1280x720 window filled a 1920x1080 overlay and the 3D sat
    // at two thirds of its size in the top left (2026-10-29)
    void toOverlay(const vr::HmdVector2_t& scale, float x, float y, float& u, float& v) const {
        float s = 1, ox = 0, oy = 0;
        if (winW > 0 && winH > 0 && (winW < scale.v[0] - 1 || winH < scale.v[1] - 1)) {
            s = std::min(scale.v[0] / winW, scale.v[1] / winH);
            ox = (scale.v[0] - winW * s) / 2; oy = (scale.v[1] - winH * s) / 2;
        }
        u = (ox + x * s) / scale.v[0]; v = (oy + y * s) / scale.v[1];
    }
    bool anchor(vr::VROverlayHandle_t parent, const Rect& r) {
        if (!measure(parent) || !shape.valid) return false;
        vr::HmdVector2_t scale{};
        o->GetOverlayMouseScale(parent, &scale);
        float u0, u1, top, bottom;
        toOverlay(scale, r.x, r.y, u0, top);
        toOverlay(scale, r.x + r.w, r.y + r.h, u1, bottom);
        const float uc = (u0 + u1) * 0.5f, vc = 1.0f - (top + bottom) * 0.5f;
        float pos[3], ax[3], az[3], width, curvature;
        surface(uc, vc, kInset, pos, ax);
        if (shape.flat) {
            float l[3] = {shape.ux[0], shape.ux[1], shape.ux[2]};
            width = norm3(l) * (u1 - u0);
            curvature = 0;
        } else {
            width = (shape.radius - kInset) * std::fabs(shape.tb) * (u1 - u0);
            curvature = std::fabs(shape.tb) * (u1 - u0) / (2.0f * (float)M_PI);
        }
        cross3(ax, shape.up, az);
        vr::HmdMatrix34_t t{};
        for (int k = 0; k < 3; ++k) { t.m[k][0] = ax[k]; t.m[k][1] = shape.up[k]; t.m[k][2] = az[k]; t.m[k][3] = pos[k]; }
        placedWidth = width;
        for (int k = 0; k < 3; ++k) placedPos[k] = pos[k];
        o->SetOverlayWidthInMeters(view, width);
        if (std::fabs(curvature - placedCurvature) > 1e-4f) { o->SetOverlayCurvature(view, curvature); placedCurvature = curvature; }
        o->SetOverlayTransformAbsolute(view, vr::TrackingUniverseStanding, &t);
        return true;
    }
    // Self check, logged: rays from the HMD through the window's surface at
    // the rectangle's corners and centre should hit our overlay at those
    // uv (one eye's half of the side-by-side image; v up).
    void verify(vr::VROverlayHandle_t parent, const Rect& r) {
        if (!shape.valid || !shown) return;
        vr::HmdVector2_t scale{};
        o->GetOverlayMouseScale(parent, &scale);
        float eye[3];
        eyePosition(eye);
        std::string line;
        for (auto f : {std::array<float, 2>{0.03f, 0.03f}, {0.97f, 0.03f}, {0.5f, 0.5f}, {0.03f, 0.97f}, {0.97f, 0.97f}}) {
            float u, v;
            toOverlay(scale, r.x + r.w * f[0], r.y + r.h * (1.0f - f[1]), u, v);
            v = 1.0f - v;
            float pt[3];
            surface(u, v, 0.0f, pt, nullptr);
            vr::VROverlayIntersectionResults_t res{};
            char buf[96];
            if (hit(view, eye, pt, res)) std::snprintf(buf, sizeof(buf), " (%.2f,%.2f)->(%.3f,%.3f)", f[0], f[1], res.vUVs.v[0], res.vUVs.v[1]);
            else std::snprintf(buf, sizeof(buf), " (%.2f,%.2f)->miss", f[0], f[1]);
            line += buf;
        }
        LOG("anchor check (wanted uv = the left number pair):%s", line.c_str());
    }
    void show() { if (!shown) { o->ShowOverlay(view); shown = true; } }
    // True when SteamVR is quitting (restarted, or crashed and coming back):
    // every handle is then dead and the connection has to be made again
    bool quitting() {
        if (!o || !vr::VRSystem()) return false;
        vr::VREvent_t ev;
        while (vr::VRSystem()->PollNextEvent(&ev, sizeof(ev)))
            if (ev.eventType == vr::VREvent_Quit) { vr::VRSystem()->AcknowledgeQuit_Exiting(); return true; }
        return false;
    }
    void shutdown() {
        if (!o) return;
        hide();
        usleep(300 * 1000);
        o->DestroyOverlay(view);
        usleep(300 * 1000);
        vr::VR_Shutdown();
        o = nullptr; view = vr::k_ulOverlayHandleInvalid;
    }
};

// Gaussian blur (two box passes each way) of the depth in the map's alpha,
// radius in map texels: softer depth edges make the stretch where a near
// object uncovers the background shorter and less visible (Frame3D Browser
// 0.1.15's "depth blur" against broken 3D edges). The guide colour stays sharp.
// Each box is a running sum (one texel in, one out), the edge texel repeated
// beyond the edge: summing the whole box at every texel took 13 ms a 672x384
// map and 50-60 ms while a video played (the helper's CPU shared with the
// model and Chrome), most of a map's way to the drawing (headset, 2026-11-01).
static void blurDepth(std::vector<uint8_t>& rgba, int w, int h, int radius) {
    if (radius <= 0 || w < 2 || h < 2) return;
    std::vector<float> a((size_t)w * h), b((size_t)w * h), col(w);
    for (size_t i = 0; i < a.size(); ++i) a[i] = rgba[i * 4 + 3];
    const float n = 2.0f * radius + 1;
    for (int pass = 0; pass < 2; ++pass) {
        for (int y = 0; y < h; ++y) {
            const float* in = &a[(size_t)y * w];
            float* out = &b[(size_t)y * w];
            float s = 0;
            for (int k = -radius; k <= radius; ++k) s += in[std::clamp(k, 0, w - 1)];
            for (int x = 0; x < w; ++x) {
                out[x] = s / n;
                s += in[std::min(x + radius + 1, w - 1)] - in[std::max(x - radius, 0)];
            }
        }
        std::fill(col.begin(), col.end(), 0.0f);
        for (int k = -radius; k <= radius; ++k) {
            const float* in = &b[(size_t)std::clamp(k, 0, h - 1) * w];
            for (int x = 0; x < w; ++x) col[x] += in[x];
        }
        for (int y = 0; y < h; ++y) {
            float* out = &a[(size_t)y * w];
            const float* add = &b[(size_t)std::min(y + radius + 1, h - 1) * w];
            const float* drop = &b[(size_t)std::max(y - radius, 0) * w];
            for (int x = 0; x < w; ++x) { out[x] = col[x] / n; col[x] += add[x] - drop[x]; }
        }
    }
    for (size_t i = 0; i < a.size(); ++i) rgba[i * 4 + 3] = (uint8_t)std::lround(a[i]);
}

// FNV-1a over the bytes
static uint64_t fnv(const std::vector<uint8_t>& v) {
    uint64_t h = 1469598103934665603ULL;
    for (uint8_t b : v) h = (h ^ b) * 1099511628211ULL;
    return h;
}

// The live view and the offline tools (--pipeline, --rerender) share these,
// so what the tools measure is what the headset shows.

// The video rectangle (BGRA rows, stride bytes apart) at the depth model's
// size, as the RGBA the depth engine takes: each texel the mean of the
// pixels it covers (a picture smaller than the model's size repeats its
// pixels, the nearest one each). One pixel taken per texel (as before)
// aliased fine texture differently from frame to frame, so the model's input
// and the map's guide colour flickered on still pictures; and that pixel was
// the top left one of the texel's area, so depth edges sat half a texel up
// and left of where the shader puts them (review 2026-11-01; Frame3D
// Browser averages too). On the test scenes the depth flickered 2-5% less
// (one real scene 4% more). 1920x1080 to 672x384: 3.8 ms on a big core
// against 0.6 ms for one pixel a texel (headset, 2026-11-01).
static void toModelSize(const uint8_t* bgra, int stride, int w, int h, int mw, int mh, std::vector<uint8_t>& out) {
    out.resize((size_t)mw * mh * 4);
    thread_local std::vector<int> cx, cy;
    thread_local std::vector<uint32_t> acc;
    cx.resize(mw + 1); cy.resize(mh + 1); acc.resize((size_t)mw * 3);
    for (int x = 0; x <= mw; ++x) cx[x] = (int)((int64_t)x * w / mw);
    for (int y = 0; y <= mh; ++y) cy[y] = (int)((int64_t)y * h / mh);
    for (int y = 0; y < mh; ++y) {
        const int y0 = cy[y], y1 = std::min(h, std::max(cy[y + 1], y0 + 1));
        std::fill(acc.begin(), acc.end(), 0u);
        for (int sy = y0; sy < y1; ++sy) {
            const uint8_t* row = bgra + (size_t)sy * stride;
            uint32_t* a = acc.data();
            for (int x = 0; x < mw; ++x, a += 3) {
                const int x0 = cx[x], x1 = std::min(w, std::max(cx[x + 1], x0 + 1));
                uint32_t b = 0, g = 0, r = 0;
                for (const uint8_t *p = row + (size_t)x0 * 4, *e = row + (size_t)x1 * 4; p < e; p += 4) { b += p[0]; g += p[1]; r += p[2]; }
                a[0] += r; a[1] += g; a[2] += b;
            }
        }
        uint8_t* q = &out[(size_t)y * mw * 4];
        const uint32_t* a = acc.data();
        for (int x = 0; x < mw; ++x, a += 3, q += 4) {
            const uint32_t n = (uint32_t)((std::min(w, std::max(cx[x + 1], cx[x] + 1)) - cx[x]) * (y1 - y0));
            q[0] = (uint8_t)((a[0] + n / 2) / n); q[1] = (uint8_t)((a[1] + n / 2) / n); q[2] = (uint8_t)((a[2] + n / 2) / n); q[3] = 255;
        }
    }
}

// Near objects widened sideways by r map texels (the larger depth of the
// neighbourhood, along the rows the eyes are shifted along): where a near
// object uncovers the background, the stretch then falls on the background
// beside it instead of tearing the object's own edge (iw3's edge dilation;
// its author warns that large values widen unrelated edges, so it is small)
static void dilateDepth(std::vector<uint8_t>& rgba, int w, int h, int r) {
    if (r <= 0) return;
    std::vector<uint8_t> row(w);
    for (int y = 0; y < h; ++y) {
        uint8_t* p = &rgba[(size_t)y * w * 4];
        for (int x = 0; x < w; ++x) {
            uint8_t m = 0;
            for (int k = std::max(0, x - r); k <= std::min(w - 1, x + r); ++k) m = std::max(m, p[k * 4 + 3]);
            row[x] = m;
        }
        for (int x = 0; x < w; ++x) p[x * 4 + 3] = row[x];
    }
}

// What is done to each new depth map before it is drawn
struct PostSettings {
    int smooth = 3;   // depth blur radius, map texels
    int dilate = 0;   // near objects widened by this many texels (dilateDepth)
};
static void postDepth(std::vector<uint8_t>& rgba, int w, int h, const PostSettings& s) {
    dilateDepth(rgba, w, h, s.dilate);
    blurDepth(rgba, w, h, s.smooth);
}

// A map's depth (alpha, 0..255) at its 5th, 50th and 95th percentile, as 0..1
static void depthLook(const std::vector<uint8_t>& rgba, float out[3]) {
    int hist[256] = {0}, n = 0;
    for (size_t i = 3; i < rgba.size(); i += 4) { ++hist[rgba[i]]; ++n; }
    const float qs[3] = {0.05f, 0.5f, 0.95f};
    for (int q = 0; q < 3; ++q) {
        int want = (int)(qs[q] * n), sum = 0, v = 0;
        while (v < 255 && sum + hist[v] <= want) sum += hist[v++];
        out[q] = v / 255.0f;
    }
}

// Lines of subtitle-like text in rows y0..y1 of a w x h picture (RGBA), top
// first: x0, y0, x1, y1 in pixels (ends exclusive). Text-like pixels are
// bright (kBright), white or yellow as subtitles are (no other hue: bright
// sky between dark leaves looked like outlined letters), with a dark outline
// or shadow (below kDark) within two pixels; rows with enough of them in at
// least two separate runs form a band, and a band of plausible size (height
// kMinH-kMaxH of the picture, at least kMinW wide, its centre within kCentre
// of the middle) is a line.
struct TextLines {
    static constexpr float kMinH = 0.025f, kMaxH = 0.14f, kMinW = 0.10f, kCentre = 0.15f;
    static constexpr int kBright = 190, kDark = 70;
    static std::vector<std::array<int, 4>> find(const uint8_t* rgba, int w, int h, int y0, int y1) {
        std::vector<std::array<int, 4>> lines;
        auto L = [&](int x, int y) { const uint8_t* p = rgba + ((size_t)y * w + x) * 4; return (2 * p[0] + 5 * p[1] + p[2]) / 8; };
        // white (all channels close) or yellow (red and green high, blue low)
        auto letter = [&](int x, int y) {
            const uint8_t* p = rgba + ((size_t)y * w + x) * 4;
            const int lo = std::min({p[0], p[1], p[2]}), hi = std::max({p[0], p[1], p[2]});
            return (hi - lo <= 40 && lo >= 160) || (p[0] >= 170 && p[1] >= 150 && p[2] <= 120);
        };
        int bandTop = -1, bandBot = -1, minX = w, maxX = -1, gap = 0;
        auto close = [&] {
            const float bh = (float)(bandBot - bandTop + 1) / h, bw = (float)(maxX - minX + 1) / w, cx = (minX + maxX) / 2.0f / w;
            if (bh >= kMinH && bh <= kMaxH && bw >= kMinW && std::fabs(cx - 0.5f) < kCentre) lines.push_back({minX, bandTop, maxX + 1, bandBot + 1});
            bandTop = -1; minX = w; maxX = -1; gap = 0;
        };
        for (int y = y0; y <= y1; ++y) {
            int count = 0, runs = 0, rowMin = w, rowMax = -1;
            bool inRun = false;
            for (int x = 2; x < w - 2; ++x) {
                bool text = false;
                if (L(x, y) > kBright && letter(x, y))
                    for (int k = -2; k <= 2 && !text; ++k)
                        text = L(x + k, y) < kDark || (y + k >= 0 && y + k < h && L(x, y + k) < kDark);
                if (text) { ++count; rowMin = std::min(rowMin, x); rowMax = std::max(rowMax, x); if (!inRun) ++runs; }
                inRun = text || (inRun && L(x, y) > kBright);
            }
            if (count >= 3 && runs >= 2) {
                if (bandTop < 0) bandTop = y;
                bandBot = y; gap = 0;
                minX = std::min(minX, rowMin); maxX = std::max(maxX, rowMax);
            } else if (bandTop >= 0 && ++gap > 2) close();
        }
        if (bandTop >= 0) close();
        return lines;
    }
};

// Black bars burned into the picture (a film's letterbox in a 16:9 video, a
// 4:3 show's pillarbox), found on each picture made for the depth model, at
// its size. A row or column counts as picture once something in it is lit
// (more than 1% of it brighter than a dark grey) and stays picture for
// kMemoryNs after: a dark scene in the middle of a film keeps its place;
// only rows never lit for that long are bars. The two must match (within
// 3%) and be 2-35% each: a dark night scene's sky and ground looked like two
// uneven bars once that was relaxed (2026-10-27). Subtitles drawn inside a
// bar (letterboxed films often have them there) lit its rows: the bars went
// off while they showed and for kMemoryNs after, the model's input switching
// between the picture and the whole frame (review 2026-11-02). So lines of
// text (TextLines) in the outer rows, with a little room around them (their
// outline and shadow, the letters' tops and tails), are left out: in a bar
// nothing else is lit beside them, in the picture the scene is. Nothing lit
// yet (a dark opening): no bars. The picture alone goes to the depth model
// and the bars stay on the screen plane.
struct BarFinder {
    static constexpr int64_t kMemoryNs = 10000000000LL;
    static constexpr float kOuter = 0.36f;   // where bars can be (35% a side)
    std::vector<int64_t> rowLit, colLit;
    float area[4] = {0, 0, 1, 1};   // the picture inside the bars: x0, y0, x1, y1 (fractions, top first)
    void reset() { rowLit.clear(); colLit.clear(); area[0] = area[1] = 0; area[2] = area[3] = 1; }
    void update(const uint8_t* rgba, int w, int h, int64_t now) {
        if ((int)rowLit.size() != h || (int)colLit.size() != w) {
            rowLit.assign(h, INT64_MIN / 2); colLit.assign(w, INT64_MIN / 2);
        }
        std::vector<int> rows(h, 0), cols(w, 0);
        std::vector<std::pair<int, int>> skip(h, {0, 0});   // each row's x0, x1 left out
        const int outer = (int)(h * kOuter);
        auto lines = TextLines::find(rgba, w, h, 0, outer);
        const auto low = TextLines::find(rgba, w, h, h - 1 - outer, h - 1);
        lines.insert(lines.end(), low.begin(), low.end());
        const int px = std::max(2, w / 50), py = std::max(2, h / 72);
        for (const auto& l : lines)
            for (int y = std::max(0, l[1] - py); y < std::min(h, l[3] + py); ++y) skip[y] = {std::max(0, l[0] - px), std::min(w, l[2] + px)};
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                if (x >= skip[y].first && x < skip[y].second) continue;
                const uint8_t* p = rgba + ((size_t)y * w + x) * 4;
                if (2 * p[0] + 5 * p[1] + p[2] > 28 * 8) { ++rows[y]; ++cols[x]; }
            }
        for (int y = 0; y < h; ++y) if (rows[y] > std::max(2, w / 100)) rowLit[y] = now;
        for (int x = 0; x < w; ++x) if (cols[x] > std::max(2, h / 100)) colLit[x] = now;
        auto side = [&](const std::vector<int64_t>& lit, int n, float& a0, float& a1) {
            a0 = 0; a1 = 1;
            int first = -1, last = -1;
            for (int i = 0; i < n; ++i) if (now - lit[i] < kMemoryNs) { if (first < 0) first = i; last = i; }
            if (first < 0) return;
            const int t = first, b = n - 1 - last;
            if (std::min(t, b) < n * 0.02 || std::max(t, b) > n * 0.35 || std::abs(t - b) > std::max(2.0, n * 0.03)) return;
            a0 = (float)std::min(t, b) / n; a1 = 1.0f - a0;
        };
        side(rowLit, h, area[1], area[3]);
        side(colLit, w, area[0], area[2]);
    }
    bool full() const { return area[0] == 0 && area[1] == 0 && area[2] == 1 && area[3] == 1; }
};

// The picture made for the depth model: the area inside the bars (if any)
// of the w x h rectangle at the model's size. *map gets where it lies.
// shrink(x, y, w, h, out): that part of the rectangle at the model's size
// (false when it could not be made); whole: the whole rectangle at the
// model's size when already made. False when there is no input to give.
using Shrink = std::function<bool(int, int, int, int, std::vector<uint8_t>&)>;
static bool modelInput(const Shrink& shrink, int w, int h, int mw, int mh, BarFinder* bars, bool crop,
                       int64_t now, std::vector<uint8_t>& out, float map[4], const std::vector<uint8_t>* whole = nullptr) {
    if (whole) out = *whole;
    else if (!shrink(0, 0, w, h, out)) return false;
    if (out.size() != (size_t)mw * mh * 4) return false;
    map[0] = map[1] = 0; map[2] = map[3] = 1;
    if (!bars) return true;
    bars->update(out.data(), mw, mh, now);
    if (!crop || bars->full()) return true;
    const int x0 = (int)std::lround(bars->area[0] * w), y0 = (int)std::lround(bars->area[1] * h);
    const int x1 = (int)std::lround(bars->area[2] * w), y1 = (int)std::lround(bars->area[3] * h);
    if (x1 - x0 < 16 || y1 - y0 < 16) return true;
    if (!shrink(x0, y0, x1 - x0, y1 - y0, out)) return false;
    map[0] = (float)x0 / w; map[1] = (float)y0 / h; map[2] = (float)x1 / w; map[3] = (float)y1 / h;
    return true;
}

// Comfort limits. The eyes see the screen plane where the window is; depth
// shifts each eye's picture by separation * (depth - convergence) of its
// width, so a point sits 2 * separation * |depth - convergence| * width
// metres apart between the eyes. Seen from distance metres that is an angle,
// and keeping it within about a degree of the screen plane is the usual
// comfort rule (Shibata et al. 2011, J. Vision 11(8):11; SuperDepth3D's "max
// depth"): the separation is capped so the largest shift stays within
// kMaxDeg, whatever the window's size and distance. A window made twice as
// large, or brought twice as close, keeps the same angle instead of doubling.
// The window's size and distance and the pop-out setting change smoothly
// (time constants below) so nothing pumps. The overscan zoom is for the
// separation asked for, not the one the limit leaves: that follows the
// eye's distance, and the picture would grow and shrink as the head moves.
// An "edge guard" (the scene pushed back behind the frame while something
// in front of it was cut by a side edge) was taken out: on real scenes it
// was at work most of the time, taking away the pop-out the user set, and
// cut more of the edges for its room (2026-11-01).
struct Comfort {
    static constexpr float kMaxDeg = 1.0f;
    static constexpr float kConvTau = 0.6f, kGeomTau = 1.0f;   // seconds
    float conv = -1, dist = 0, width = 0;
    int64_t at = 0;
    bool snapNext = false;   // the next step takes its targets at once (snap)
    // Each frame: the convergence and separation to draw with. widthM and
    // distM are the picture's width and the eye's distance to it (0 when not
    // known yet: no cap).
    void step(int64_t now, float userConv, float userSep, float widthM, float distM, float& convOut, float& sepOut) {
        const float dt = at ? (now - at) / 1e9f : 1e9f;
        at = now;
        const float target = userConv;
        float aC = 1.0f - std::exp(-dt / kConvTau), aG = 1.0f - std::exp(-dt / kGeomTau);
        if (snapNext) { aC = 1.0f; if (widthM > 0 && distM > 0) { aG = 1.0f; snapNext = false; } }
        conv = conv < 0 ? target : conv + (target - conv) * aC;
        if (widthM > 0 && distM > 0) {
            dist = dist > 0 ? dist + (distM - dist) * aG : distM;
            width = width > 0 ? width + (widthM - width) * aG : widthM;
        }
        convOut = conv;
        sepOut = userSep;
        const float dev = std::max(conv, 1.0f - conv);
        if (width > 0 && dist > 0 && dev > 0) {
            // the shift is made on the zoomed picture, so it shows `zoom`
            // times larger: the 1 degree held 1.02-1.075 (review 2026-11-01)
            const float cap = std::tan(kMaxDeg * (float)M_PI / 180.0f) * dist / (2.0f * dev * width * zoom(userSep, userConv));
            sepOut = std::min(userSep, cap);
        }
    }
    void reset() { conv = -1; dist = width = 0; at = 0; snapNext = false; }
    // The 3D is shown again after being hidden (scrolling): gliding there
    // from the values of the picture as it was before made the new one swell
    // or sink once or twice after it appeared (headset, 2026-10-28), so the
    // next step (with the window measured) takes its targets at once
    void snap() { snapNext = true; }
    // The fixed overscan for these settings: an eye's picture moves by s =
    // sep x dev of the source's width at most, and zooming by z leaves
    // (1 - 1/z) / 2 of it beyond each side, so z = 1 / (1 - 2s). 1 + 2s was a
    // little short: 5 px of stretched edge on a 1920 px eye at the largest
    // strength (review 2026-11-01).
    static float zoom(float userSep, float userConv) {
        return 1.0f / (1.0f - 2.0f * userSep * std::max(userConv, 1.0f - userConv));
    }
};

// The depth range each map is stretched over. The model's output (larger
// nearer) is relative, so each map is read between its own 2nd and 98th
// percentile (robustRange), as the engine does; that range is averaged over
// time so the scene does not breathe with every map. The engine averages
// both ends alike (kSteadyRangeTau) and starts over only at a scene cut it
// has confirmed, which comes a map late: the first map of a new shot was
// stretched over the old shot's range and the new one's together, flat or
// pushed aside for about 0.1 s (review 2026-11-02: 10 of 19 cuts of the Big
// Buck Bunny trailer, 5 of 6 in Sintel). Here a map the engine found to jump
// (either reset) and the one after take their own range at once: a new
// shot's first maps filled 0.94 of its settled spread in Sintel (0.81
// before), 0.99 and 1.02 in the other real scenes (0.91, 0.92), the worst
// cut 0.71-0.99 (0.61-0.88). Between, each end eases at widenMs while the
// range grows and narrowMs while it shrinks; a slow range (widening 150-300
// ms, narrowing 1-2 s) was tried against the depth slowly moving where the
// picture is still and hardly changed that (it comes from the model) while
// lag and error grew 10-30% (2026-11-02), so both stay kSteadyRangeTau.
// From the model's raw output (the engine's keepRaw), into the map's depth
// (alpha); the guide colour stays the engine's.
struct RangeNorm {
    float widenMs = kSteadyRangeTau, narrowMs = kSteadyRangeTau;
    float lo = 0, hi = 0;
    bool valid = false, snapNext = false;
    int64_t at = 0;
    void reset() { valid = snapNext = false; at = 0; }
    void apply(const float* raw, int count, int resets, int64_t now, uint8_t* rgba) {
        float l, h;
        robustRange(raw, count, &l, &h);
        const bool jump = (resets & (DEPTH_RESET_TEXEL | DEPTH_RESET_RANGE)) != 0;
        if (!valid || jump || snapNext) { lo = l; hi = h; valid = true; snapNext = jump; }
        else {
            const float dt = (now - at) / 1e9f;
            lo += (l - lo) * depthTauAlpha(dt, (l < lo ? widenMs : narrowMs) / 1000.0f);
            hi += (h - hi) * depthTauAlpha(dt, (h > hi ? widenMs : narrowMs) / 1000.0f);
        }
        at = now;
        const float scale = 1.0f / std::max(hi - lo, depthSpanFloor(lo));
        for (int i = 0; i < count; ++i) {
            const float v = std::clamp((depthRead(raw[i], lo) - lo) * scale, 0.0f, 1.0f);
            rgba[(size_t)i * 4 + 3] = (uint8_t)(v * 255.0f + 0.5f);
        }
    }
};

// Steadier depth where the picture is still. The depth engine averages every
// texel over the same short time (30 ms), so a still scene flickers with the
// model's own noise while a moving one already trails. Here, with the
// engine's average off (tauMs 0), each texel is averaged by whether the
// picture there moved: the colour the map was made from (its guide rgb)
// against the previous map's, over a 3x3 neighbourhood so one noisy texel is
// not motion. Still (change below lo): a long average (tauMs); moving (above
// hi): the new depth at once, so nothing trails more than before; between, a
// blend. A scene cut starts over: one the engine found (either reset),
// or the colour changing across the picture at once (mean change above
// kCut). After a start the average is short at first and lengthens over
// kWarmNs: the engine's depth range settles on the new shot over a few maps
// (and confirms the cut a map later), and holding the first map of a still
// shot kept its unsettled depth for ten frames (Big Buck Bunny, 2026-10-27).
struct Steady {
    float tauMs = 500, lo = 0.005f, hi = 0.03f;   // tuned on the test scenes (2026-10-27)
    static constexpr float kCut = 0.06f;
    static constexpr int64_t kWarmNs = 400000000LL;
    int64_t startedAt = 0;
    std::vector<float> depth, change, rowSum;
    std::vector<uint8_t> guide;
    int w = 0, h = 0;
    int64_t at = 0;
    void reset() { depth.clear(); }
    // rgba: a new map (guide rgb, depth a), its depth replaced by the average
    void apply(std::vector<uint8_t>& rgba, int mw, int mh, int64_t now, bool cut) {
        const size_t n = (size_t)mw * mh;
        if (cut || depth.size() != n || mw != w) {
            depth.resize(n); guide.resize(n * 3); change.resize(n);
            for (size_t i = 0; i < n; ++i) {
                depth[i] = rgba[i * 4 + 3];
                for (int c = 0; c < 3; ++c) guide[i * 3 + c] = rgba[i * 4 + c];
            }
            w = mw; h = mh; at = now; startedAt = now;
            return;
        }
        const float dt = std::max(0.0f, (now - at) / 1e6f);
        at = now;
        const float warm = std::clamp((now - startedAt) / (float)kWarmNs, 0.0f, 1.0f);
        const float still = 1.0f - std::exp(-dt / std::max(1.0f, tauMs * warm * warm));
        double total = 0;
        for (size_t i = 0; i < n; ++i) {
            int d = 0;
            for (int c = 0; c < 3; ++c) d += std::abs((int)rgba[i * 4 + c] - (int)guide[i * 3 + c]);
            change[i] = d / (3.0f * 255.0f);
            total += change[i];
        }
        if (total / n > kCut) { depth.clear(); apply(rgba, mw, mh, now, true); return; }
        // 3x3 mean, rows then columns (edges: the texels there are)
        rowSum.resize(n);
        for (int y = 0; y < mh; ++y) {
            const float* c = &change[(size_t)y * mw];
            float* o = &rowSum[(size_t)y * mw];
            for (int x = 0; x < mw; ++x) o[x] = c[x] + (x > 0 ? c[x - 1] : 0.0f) + (x + 1 < mw ? c[x + 1] : 0.0f);
        }
        for (int y = 0; y < mh; ++y)
            for (int x = 0; x < mw; ++x) {
                const size_t i0 = (size_t)y * mw + x;
                const float m = (rowSum[i0] + (y > 0 ? rowSum[i0 - mw] : 0.0f) + (y + 1 < mh ? rowSum[i0 + mw] : 0.0f))
                                / ((1 + (x > 0) + (x + 1 < mw)) * (1 + (y > 0) + (y + 1 < mh)));
                const float t = std::clamp((m - lo) / (hi - lo), 0.0f, 1.0f);
                const float a = still + (1.0f - still) * t * t * (3.0f - 2.0f * t);
                const size_t i = (size_t)y * mw + x;
                depth[i] += (rgba[i * 4 + 3] - depth[i]) * a;
            }
        for (size_t i = 0; i < n; ++i) {
            rgba[i * 4 + 3] = (uint8_t)std::lround(depth[i]);
            for (int c = 0; c < 3; ++c) guide[i * 3 + c] = rgba[i * 4 + c];
        }
    }
};

// Video that is already 3D (side by side or top and bottom, as 3D films and
// VR180 previews come): turning it 3D again ruins it, so each eye is given
// its own half instead. Found a few times a second on the picture at the
// depth model's size: the two halves (left/right, top/bottom) must match,
// allowing a small sideways shift (the 3D's own parallax), both as a whole
// (normalised correlation at the best shift above kWhole) and block by block
// (kBlocks of the textured 8x8 blocks above 0.7 at that shift), so a
// repeating pattern that only looks alike as a whole does not count. Each
// row (side by side) or column (top and bottom) has its mean taken off first:
// a flat picture's halves share its horizontal layering (sky, horizon,
// ground, black bars) and matched as a whole (0.85-0.98 on misty and desert
// scenes); without it, what is left is the detail, which only a real pair
// shares. On the test clips 3D then scored 0.93-0.99 whole and 31-85% of
// blocks (one shift does not fit every depth), flat video at most 0.13 and
// 40%; the gates sit between. A dark or flat picture (contrast below kMinStd) tells nothing and
// leaves the answer as it was (a fade-in matched as top and bottom). It takes
// kHoldNs of the same answer to switch either way. Full-width formats are
// told from half-width ones by the picture's shape.
struct StereoFinder {
    static constexpr float kWhole = 0.8f, kBlocks = 0.25f, kMinStd = 10.0f;
    static constexpr int64_t kHoldNs = 1000000000LL, kEveryNs = 200000000LL;
    int mode = 0, seen = 0;          // 0 flat video, 1 side by side, 2 top and bottom; seen: the last answer
    int64_t seenSince = 0, checkedAt = INT64_MIN / 2;
    void reset() { mode = seen = 0; seenSince = 0; checkedAt = INT64_MIN / 2; }
    // luminance of the halves a and b (w x h each, rows stride apart),
    // whether they match at some shift within +-maxShift, each row's
    // (rows) or column's (!rows) mean taken off first
    static bool match(const std::vector<float>& L0, int stride, int ax, int ay, int bx, int by, int w, int h, int maxShift, bool rows) {
        std::vector<float> L((size_t)w * h * 2);
        for (int half = 0; half < 2; ++half) {
            const int ox = half ? bx : ax, oy = half ? by : ay;
            float* D = &L[(size_t)half * w * h];
            for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) D[(size_t)y * w + x] = L0[(size_t)(oy + y) * stride + ox + x];
            if (rows) {
                for (int y = 0; y < h; ++y) { float m = 0; for (int x = 0; x < w; ++x) m += D[(size_t)y * w + x]; m /= w; for (int x = 0; x < w; ++x) D[(size_t)y * w + x] -= m; }
            } else {
                for (int x = 0; x < w; ++x) { float m = 0; for (int y = 0; y < h; ++y) m += D[(size_t)y * w + x]; m /= h; for (int y = 0; y < h; ++y) D[(size_t)y * w + x] -= m; }
            }
        }
        stride = w; ax = 0; ay = 0; bx = 0; by = h;
        float best = -1; int bestS = 0;
        for (int s = -maxShift; s <= maxShift; ++s) {
            double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0; int n = 0;
            for (int y = 0; y < h; y += 2)
                for (int x = std::max(0, -s) + maxShift; x < w - maxShift - std::max(0, s); x += 2) {
                    const float a = L[(size_t)(ay + y) * stride + ax + x], b = L[(size_t)(by + y) * stride + bx + x + s];
                    sa += a; sb += b; saa += a * a; sbb += b * b; sab += a * b; ++n;
                }
            if (n < 64) continue;
            const double va = saa - sa * sa / n, vb = sbb - sb * sb / n;
            if (va <= 1e-6 || vb <= 1e-6) continue;
            const float r = (float)((sab - sa * sb / n) / std::sqrt(va * vb));
            if (r > best) { best = r; bestS = s; }
        }
        if (best < kWhole) return false;
        int textured = 0, good = 0;
        for (int y0 = 0; y0 + 8 <= h; y0 += 8)
            for (int x0 = maxShift; x0 + 8 <= w - maxShift; x0 += 8) {
                double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
                for (int y = y0; y < y0 + 8; ++y)
                    for (int x = x0; x < x0 + 8; ++x) {
                        const float a = L[(size_t)(ay + y) * stride + ax + x], b = L[(size_t)(by + y) * stride + bx + x + bestS];
                        sa += a; sb += b; saa += a * a; sbb += b * b; sab += a * b;
                    }
                const double va = saa - sa * sa / 64, vb = sbb - sb * sb / 64;
                if (va < 64 * 16 || vb < 64 * 16) continue;   // flat block (std below 4 grey levels)
                ++textured;
                if ((sab - sa * sb / 64) / std::sqrt(va * vb) > 0.7) ++good;
            }
        return textured >= 8 && good >= kBlocks * textured;
    }
    void update(const uint8_t* rgba, int w, int h, int64_t now) {
        if (now - checkedAt < kEveryNs) return;
        checkedAt = now;
        std::vector<float> L((size_t)w * h);
        double sum = 0, sum2 = 0;
        for (size_t i = 0; i < L.size(); ++i) {
            L[i] = (2.0f * rgba[i * 4] + 5.0f * rgba[i * 4 + 1] + rgba[i * 4 + 2]) / 8.0f;
            sum += L[i]; sum2 += (double)L[i] * L[i];
        }
        const double mean = sum / L.size();
        if (std::sqrt(std::max(0.0, sum2 / L.size() - mean * mean)) < kMinStd) return;
        const int answer = match(L, w, 0, 0, w / 2, 0, w / 2, h, 8, true) ? 1 : match(L, w, 0, 0, 0, h / 2, w, h / 2, 8, false) ? 2 : 0;
        if (answer != seen) { seen = answer; seenSince = now; }
        if (seen != mode && now - seenSince >= kHoldNs) mode = seen;
    }
    // the share of each eye its half fills, for a w x h picture
    float eyeFill(int w, int h) const { return eyeFillFor(mode, w, h); }
    static float eyeFillFor(int mode, int w, int h) {
        // Side by side, full (each half a whole picture: VR180's square
        // eyes, 2:1; two 4:3 or 16:9 pictures, 2.67 or 3.56) or half (a usual
        // frame, films up to 2.39:1, each half squeezed). The cut at 2.4 took
        // VR180 for half (each eye stretched twice as wide) and a 2.39 film
        // a pixel wider for full (review 2026-11-01). A 2:1 film shown half
        // side by side would now be taken for full; it is rarer than VR180.
        if (mode == 1) { const float a = (float)w / h; return a > 2.5f || std::fabs(a - 2.0f) < 0.1f ? 0.5f : 1.0f; }
        if (mode == 2) return (float)w / h < 1.2f ? 0.5f : 1.0f;
        return 1.0f;
    }
};

// Subtitles burned into the video: the depth model gives the letters the
// depth of the scene behind them, so each eye shifts them differently and
// they blur and double. Found on the picture at the model's size, in its
// lower part (from kTop down): the lowest line of text there (TextLines) is
// a subtitle. kOnChecks checks in a row turn it on, kOffNs without turn it
// off; the box is kept on the screen plane like player controls.
struct SubtitleFinder {
    static constexpr float kTop = 0.62f;
    static constexpr int kOnChecks = 3;
    static constexpr int64_t kOffNs = 400000000LL;
    float box[4] = {0, 0, 0, 0};     // x0, y0, x1, y1 (fractions); empty when x1 <= x0
    int streak = 0;
    int64_t seenAt = INT64_MIN / 2;
    float last[4] = {0, 0, 0, 0};
    void reset() { box[0] = box[1] = box[2] = box[3] = 0; streak = 0; seenAt = INT64_MIN / 2; }
    bool on() const { return box[2] > box[0]; }
    void update(const uint8_t* rgba, int w, int h, int64_t now) {
        const auto lines = TextLines::find(rgba, w, h, (int)(h * kTop), h - 1);
        float found[4] = {0, 0, 0, 0};
        if (!lines.empty()) {
            const auto& b = lines.back();
            found[0] = (float)b[0] / w; found[1] = (float)b[1] / h; found[2] = (float)b[2] / w; found[3] = (float)b[3] / h;
        }
        if (found[2] > found[0]) {
            ++streak; seenAt = now;
            // a little room around the letters (their outline, the blur of the map)
            const float px = 0.01f, py = 0.01f;
            last[0] = std::max(0.0f, found[0] - px); last[1] = std::max(0.0f, found[1] - py);
            last[2] = std::min(1.0f, found[2] + px); last[3] = std::min(1.0f, found[3] + py);
            if (streak >= kOnChecks) std::copy(last, last + 4, box);
        } else {
            streak = 0;
            if (now - seenAt > kOffNs) box[0] = box[1] = box[2] = box[3] = 0;
        }
    }
};

// The subtitle box as drawn. The finder's box changes at once: a new line of
// another width, a subtitle coming or going. The depth around it then went
// from the scene's to flat (or back) between two frames, a jolt under each
// new line. Drawn, each edge eases to the finder's (kGrowMs when it makes
// the box bigger, so the letters soon go flat; kShrinkMs when smaller); a
// subtitle opens from its middle and closes to it.
struct SubtitleEase {
    static constexpr float kGrowMs = 60, kShrinkMs = 150;
    float box[4] = {0, 0, 0, 0};
    int64_t at = 0;
    void reset() { box[0] = box[1] = box[2] = box[3] = 0; at = 0; }
    // the box to draw at `now` (nullptr: none) for the finder's `target`
    const float* step(const float* target, int64_t now) {
        const float dt = at ? std::min(200.0f, (now - at) / 1e6f) : 0.0f;
        at = now;
        const bool on = target[2] > target[0], shown = box[2] > box[0];
        if (!on && !shown) return nullptr;
        float to[4];
        if (on) std::copy(target, target + 4, to);
        else { const float mid = (box[0] + box[2]) / 2; to[0] = to[2] = mid; to[1] = box[1]; to[3] = box[3]; }
        if (on && !shown) { const float mid = (to[0] + to[2]) / 2; box[0] = box[2] = mid; box[1] = to[1]; box[3] = to[3]; }
        for (int k = 0; k < 4; ++k) {
            const bool grows = k < 2 ? to[k] < box[k] : to[k] > box[k];
            box[k] += (to[k] - box[k]) * (1 - std::exp(-dt / (grows ? kGrowMs : kShrinkMs)));
            if (std::fabs(to[k] - box[k]) < 0.001f) box[k] = to[k];
        }
        if (!on && box[2] - box[0] < 0.04f) { reset(); return nullptr; }
        return box[2] > box[0] ? box : nullptr;
    }
};

// Player controls as drawn. The extension's rectangles come and go at once,
// and the depth around a control bar went from the scene's to flat (and
// back) between two frames whenever the pointer brought the controls up or
// they faded (review 2026-11-02). Each rectangle is kept flat by a weight
// that eases in (kInMs: Chrome draws the controls at once, so soon) and out
// (kOutMs); one that moves or changes size (overlapping most of the last)
// is the same rectangle, at its new place at once.
struct UiEase {
    static constexpr float kInMs = 30, kOutMs = 100;
    struct Kept { float r[4]; float w; bool seen; };
    std::vector<Kept> kept;
    int64_t at = 0;
    void reset() { kept.clear(); at = 0; }
    // rects: x0, y0, x1, y1 each; out, weights: what to draw at `now`
    void step(const std::vector<float>& rects, int64_t now, std::vector<float>& out, std::vector<float>& weights) {
        const float dt = at ? std::min(200.0f, (now - at) / 1e6f) : 0.0f;
        at = now;
        auto area = [](const float* a) { return std::max(0.0f, a[2] - a[0]) * std::max(0.0f, a[3] - a[1]); };
        for (Kept& k : kept) k.seen = false;
        for (size_t i = 0; i + 3 < rects.size(); i += 4) {
            const float* t = &rects[i];
            Kept* m = nullptr;
            for (Kept& k : kept) {
                if (k.seen) continue;
                const float in[4] = {std::max(k.r[0], t[0]), std::max(k.r[1], t[1]), std::min(k.r[2], t[2]), std::min(k.r[3], t[3])};
                if (area(in) > 0.5f * std::min(area(k.r), area(t))) { m = &k; break; }
            }
            if (!m) { kept.push_back({{0, 0, 0, 0}, 0.0f, false}); m = &kept.back(); }
            std::copy(t, t + 4, m->r); m->seen = true;
        }
        for (Kept& k : kept) {
            const float to = k.seen ? 1.0f : 0.0f;
            k.w += (to - k.w) * (1 - std::exp(-dt / (k.seen ? kInMs : kOutMs)));
            // in 32nds, so a settled one asks for no more drawings
            k.w = std::fabs(to - k.w) < 1.0f / 32 ? to : std::round(k.w * 32) / 32;
        }
        kept.erase(std::remove_if(kept.begin(), kept.end(), [](const Kept& k) { return !k.seen && k.w <= 0; }), kept.end());
        for (const Kept& k : kept) {
            if (k.w <= 0) continue;
            out.insert(out.end(), k.r, k.r + 4);
            weights.push_back(k.w);
        }
    }
};

// Each eye's size for a w x h picture: at most 1920 wide
static void eyeSize(int w, int h, int& ow, int& oh) {
    ow = std::min(w, 1920);
    oh = std::max(1, h * ow / w);
}

// Netpbm files for the tools: P6 from 4-byte pixels (r, g, b at those
// offsets), P5 from one byte of each, P7 RGB_ALPHA as is
static bool writePpm(const std::string& path, int w, int h, const uint8_t* px, size_t stride, int r, int g, int b) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    std::vector<uint8_t> row((size_t)w * 3);
    for (int y = 0; y < h; ++y) {
        const uint8_t* s = px + (size_t)y * stride;
        for (int x = 0; x < w; ++x) { row[x * 3] = s[x * 4 + r]; row[x * 3 + 1] = s[x * 4 + g]; row[x * 3 + 2] = s[x * 4 + b]; }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    return std::fclose(f) == 0;
}
static bool writePgm(const std::string& path, int w, int h, const uint8_t* px, int offset) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fprintf(f, "P5\n%d %d\n255\n", w, h);
    std::vector<uint8_t> v((size_t)w * h);
    for (size_t i = 0; i < v.size(); ++i) v[i] = px[i * 4 + offset];
    std::fwrite(v.data(), 1, v.size(), f);
    return std::fclose(f) == 0;
}
static bool writePam(const std::string& path, int w, int h, const uint8_t* rgba) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fprintf(f, "P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", w, h);
    std::fwrite(rgba, 1, (size_t)w * h * 4, f);
    return std::fclose(f) == 0;
}
// P6 or P7 (RGB_ALPHA) into RGBA, alpha 255 for P6
static bool readPnm(const std::string& path, int& w, int& h, std::vector<uint8_t>& rgba) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char magic[3] = {0};
    bool ok = std::fscanf(f, "%2s", magic) == 1;
    int depth = 3, maxv = 255;
    if (ok && !std::strcmp(magic, "P6")) {
        ok = std::fscanf(f, "%d %d %d", &w, &h, &maxv) == 3;
        std::fgetc(f);
    } else if (ok && !std::strcmp(magic, "P7")) {
        char key[32];
        while ((ok = std::fscanf(f, "%31s", key) == 1) && std::strcmp(key, "ENDHDR")) {
            if (!std::strcmp(key, "WIDTH")) ok = std::fscanf(f, "%d", &w) == 1;
            else if (!std::strcmp(key, "HEIGHT")) ok = std::fscanf(f, "%d", &h) == 1;
            else if (!std::strcmp(key, "DEPTH")) ok = std::fscanf(f, "%d", &depth) == 1;
            else if (!std::strcmp(key, "MAXVAL")) ok = std::fscanf(f, "%d", &maxv) == 1;
            else ok = std::fscanf(f, "%*s") == 0;
            if (!ok) break;
        }
        std::fgetc(f);
    } else ok = false;
    ok = ok && w > 0 && h > 0 && maxv == 255 && (depth == 3 || depth == 4);
    if (ok) {
        std::vector<uint8_t> raw((size_t)w * h * depth);
        ok = std::fread(raw.data(), 1, raw.size(), f) == raw.size();
        rgba.resize((size_t)w * h * 4);
        for (size_t i = 0; ok && i < (size_t)w * h; ++i)
            for (int c = 0; c < 4; ++c) rgba[i * 4 + c] = c < depth ? raw[i * depth + c] : 255;
    }
    std::fclose(f);
    return ok;
}

// The depth engine. The main thread starts it on the extension's hello and
// stops it a minute after the extension leaves; the view thread, its only
// user, switches it to another model when the detail setting changes.
struct DepthOwner {
    std::string assets;
    std::atomic<MlxrDepthEngine*> engine{nullptr};
    std::atomic<int> detail{-1};   // the setting the running engine was started for
    std::atomic<unsigned> starts{0};   // engines started so far: tells a new engine from an old one at the same address

    // its own lock: m is held for seconds while a model loads
    std::string modelName() { std::lock_guard<std::mutex> l(nameM); return name; }
    // Normal detail runs ZipDepth 512x288 on the CPU (LiteRT, 3 threads on
    // the big cores cpu2-7, 0xfc: 29 ms a map, --depthbench 2026-10-25). The
    // GPU (ncnn/Vulkan) is faster only while the headset is idle (15 ms):
    // worn, SteamVR's compositor, Xwayland's readbacks and our warp share it,
    // and with the depth model on it too the video fell to 19-48 frames a
    // second (reads 8-25 ms) against 60 (reads 2-4 ms) with CPU depth
    // (measured on a Steam Frame, 2026-10-25). Detail 1 (768x448) runs on the
    // GPU, for still pictures; detail 2 is 672x384 on the CPU (Frame3D
    // Browser's Wide size: 45 ms a map, thin outlines clearly sharper than
    // 512x288; 2026-10-31), at 15 maps a second, as 20 took the three big
    // cores nearly whole. Each choice falls back to the next.
    // (Re)starts the engine for that setting. Only the view thread may replace
    // a running engine, as it is the one using it; others call ensure().
    std::string start(int want) {
        std::lock_guard<std::mutex> l(m);
        if (engine.load() && detail.load() == want) return {};
        stopLocked();
        // perSecond: the GPU 768 model at 15 maps a second leaves the
        // shared GPU more room than 20
        struct Choice { const char* file; const char* name; int perSecond; };
        static const Choice kDetail0[] = {{"zipdepth_512x288_fp16.tflite", "ZipDepth 512x288 CPU", 20},
                                          {"zipdepth_npu_512x288.ncnn.param", "ZipDepth 512x288 GPU", 20}};
        static const Choice kDetail1[] = {{"zipdepth_npu_768x448.ncnn.param", "ZipDepth 768x448 GPU", 15},
                                          {"zipdepth_npu_512x288.ncnn.param", "ZipDepth 512x288 GPU", 20},
                                          {"zipdepth_512x288_fp16.tflite", "ZipDepth 512x288 CPU", 20}};
        static const Choice kDetail2[] = {{"zipdepth_672x384_fp16.tflite", "ZipDepth 672x384 CPU", 15},
                                          {"zipdepth_512x288_fp16.tflite", "ZipDepth 512x288 CPU", 20}};
        std::vector<Choice> choices = want == 1 ? std::vector<Choice>(std::begin(kDetail1), std::end(kDetail1))
                                    : want == 2 ? std::vector<Choice>(std::begin(kDetail2), std::end(kDetail2))
                                                : std::vector<Choice>(std::begin(kDetail0), std::end(kDetail0));
        std::string why;
        fellBack = -1;
        for (const Choice& c : choices) {
            // A model whose load hung is not tried again until the companion
            // restarts, nor, after a GPU one hung, any GPU model: switching
            // back to its setting froze the view 20 s a model each time and
            // left another engine stuck (review 2026-11-01)
            const bool gpuModel = std::strstr(c.file, ".ncnn.") != nullptr;
            if ((gpuModel && gpuHung) || std::find(hung.begin(), hung.end(), c.file) != hung.end()) {
                why += std::string(c.file) + ": its load hung before; ";
                continue;
            }
            const std::string model = assets + "/depth/" + c.file;
            MlxrDepthConfig cfg{};
            cfg.modelPath = model.c_str(); cfg.threads = kDepthThreads; cfg.cpuMask = kDepthCpus; cfg.flags = MLXR_DEPTH_FP16; cfg.perSecond = c.perSecond;
            cfg.keepRaw = 1;   // the depth range is ours (RangeNorm)
            mlxr_depth_params_default(&cfg.params);
            // the engine's threads take the nice of the thread starting them
            const id_t self = (id_t)syscall(SYS_gettid);
            errno = 0;
            const int niceWas = getpriority(PRIO_PROCESS, self);
            if (errno == 0) setpriority(PRIO_PROCESS, self, kDepthNice);
            MlxrDepthEngine* e = mlxr_depth_start(&cfg);
            if (errno == 0) setpriority(PRIO_PROCESS, self, niceWas);
            char err[256] = "";
            const MlxrDepthState st = e ? mlxr_depth_wait_ready(e, 20000, err, sizeof(err)) : MLXR_DEPTH_FAILED;
            if (st != MLXR_DEPTH_READY) {
                // Stopping waits for the engine's thread, so an engine still
                // loading after 20 s (a GPU driver that hangs) is stopped on a
                // thread of its own: stopped here, the time limit held nothing
                // up and the companion hung with it (review 2026-10-31)
                if (e && st == MLXR_DEPTH_LOADING) {
                    std::snprintf(err, sizeof(err), "still loading after 20 s");
                    std::thread([e] { mlxr_depth_stop(e); }).detach();
                    hung.push_back(c.file);
                    if (gpuModel) gpuHung = true;
                }
                else if (e) mlxr_depth_stop(e);
                LOG("depth model %s failed: %s", c.file, err);
                why += std::string(c.file) + ": " + err + "; ";
                continue;
            }
            engine = e; detail = want; ++starts;
            // the setting's own model did not start: the popup says so
            // (an earlier choice ran in its place without a word)
            if (&c != &choices.front()) fellBack = want;
            { std::lock_guard<std::mutex> n(nameM); name = c.name; }
            LOG("depth engine ready: %s", model.c_str());
            return {};
        }
        return "depth model failed: " + why;
    }
    // Starts one when none runs; a running engine is left as it is. While the
    // view thread is switching models it returns at once (not waiting out the
    // load); the view thread then reports the new model itself.
    std::string ensure(int want) {
        {
            std::unique_lock<std::mutex> l(m, std::try_to_lock);
            if (!l.owns_lock() || engine.load()) return {};
        }
        return start(want);
    }
    void stop() { std::lock_guard<std::mutex> l(m); stopLocked(); }
    // the setting whose own model did not start (a fallback runs), -1: none
    std::atomic<int> fellBack{-1};
    // ,"refused":N for the ready and model replies when one did not start
    std::string refusedField() { const int f = fellBack.load(); return f >= 0 ? ",\"refused\":" + std::to_string(f) : std::string(); }

private:
    std::mutex m, nameM;
    std::string name;
    std::vector<std::string> hung;   // model files whose load hung (under m)
    bool gpuHung = false;            // a GPU model's load hung (under m)
    void stopLocked() {
        if (MlxrDepthEngine* e = engine.exchange(nullptr)) { mlxr_depth_stop(e); LOG("depth engine stopped"); }
        detail = -1;
    }
};

// Work taken off the view loop, so that it only reads, draws and hands
// frames over in time for the headset's refreshes: steadying and smoothing
// each new depth map, and the checks for 3D video and burned-in subtitles.
// In the loop they took 5 ms a map and up to 20 at 1080p, and the frames
// they delayed missed their refresh (2026-10-31). Tasks run in order.
struct Helper {
    Helper() : t([this] { run(); }) {}
    ~Helper() {
        { std::lock_guard<std::mutex> l(m); quit = true; }
        cv.notify_one();
        t.join();
    }
    void post(std::function<void()> f) {
        { std::lock_guard<std::mutex> l(m); q.push_back(std::move(f)); }
        cv.notify_one();
    }
private:
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::function<void()>> q;
    bool quit = false;
    std::thread t;
    void run() {
        placeThread("helper", kViewCpus, kViewNice);
        for (;;) {
            std::function<void()> f;
            {
                std::unique_lock<std::mutex> l(m);
                cv.wait(l, [&] { return quit || !q.empty(); });
                if (q.empty()) return;
                f = std::move(q.front());
                q.pop_front();
            }
            f();
        }
    }
};

// Keeps every library loaded so far (the GPU driver among them) for good.
// eglTerminate let the Vulkan loader unload zink's driver (Turnip,
// libvulkan_freedreno), whose per-thread data destructor then ran at the
// view thread's exit in unmapped code: the companion crashed while stopping
// (SIGSEGV in __nptl_deallocate_tsd) after any run that had set the GPU up
// without SteamVR's libraries holding the driver too (headset, 2026-11-01).
static void pinLoadedLibraries() {
    dl_iterate_phdr([](dl_phdr_info* info, size_t, void*) {
        if (info->dlpi_name && *info->dlpi_name) dlopen(info->dlpi_name, RTLD_NOW | RTLD_NOLOAD | RTLD_NODELETE);
        return 0;
    }, nullptr);
}

static void viewThread(Shared* sh, DepthOwner* owner) {
    placeThread("view", kViewCpus, kViewNice);   // before the GPU's driver threads start: they take it over
    Capture cap;
    Reader reader;
    uint64_t frameSeq = 0;   // the reader's frame last taken
    VrView vrv;
    GpuWarp gpu;
    bool gpuUp = false;
    vr::VROverlayHandle_t parent = vr::k_ulOverlayHandleInvalid;
    std::string parentKey;           // Chrome's overlay key, for checking it is still there
    int64_t parentCheckedAt = 0;
    bool active = false, reportedSubmit = false, verified = false;
    uint64_t gen = 0, haveSeq = 0, frames = 0, frameId = 0;
    int dw = 0, dh = 0;
    std::vector<uint8_t> depthMap;   // the engine's RGBA8: guide rgb, depth a
    std::vector<uint8_t> small;
    BarFinder bars;
    int barsW = 0, barsH = 0;        // the picture size the bars were found for
    Comfort comfortState;
    float drawSep = 0, drawConv = 0;   // what the last frame was drawn with (for --dump)
    Steady steady;
    RangeNorm rangeNorm;
    StereoFinder stereoFinder;
    SubtitleFinder subtitleFinder;
    // Steady, the finders and the depth post-processing run on the helper
    // (only it touches them once the loop runs; resets go to it as tasks, so
    // they keep their order). What it hands back: the newest finished depth
    // map, the subtitle box (both under doneM) and the video's 3D format.
    // A map or answer made for an older view or depth model (mapGen) is dropped.
    struct DoneMap {
        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
        int64_t captureNs = 0, helperNs = 0, acquiredNs = 0;
        std::array<float, 4> rect = {0, 0, 1, 1};
        float look[3] = {0, 0, 0};   // its depth's 5th, 50th and 95th percentile (0..1, 1 near)
        bool cut = false;
        unsigned gen = 0;
        bool ready = false;
    } doneMap;
    std::mutex doneM;
    float subBox[4] = {0, 0, 0, 0};
    SubtitleEase subtitleEase;   // the loop's own (subBox as drawn)
    UiEase uiEase;               // player controls as drawn
    std::atomic<int> stereoMode{0};
    std::atomic<unsigned> mapGen{0};
    int64_t stereoCheckedAt = INT64_MIN / 2;
    int subtitlesWere = -1;   // the setting last seen
    int detailSetting = -1, refusedDetail = -1;   // the detail setting, and one whose models would not start
    Helper helper;
    unsigned tunedStart = 0;                 // the engine (DepthOwner::starts) whose average was last set for Steady
    // where each capture's map lies in the picture (by frame id; the engine
    // answers later), and the map in use
    std::map<uint64_t, std::array<float, 4>> mapRects;
    std::array<float, 4> mapRect = {0, 0, 1, 1};
    int lastPointerX = -1, lastPointerY = -1;
    std::vector<std::array<int, 4>> xPopups;   // Chrome popup windows over the video, window pixels
    unsigned loops = 0;
    double ageSum = 0; int ageN = 0;
    // The capture the depth engine is working on (its frame id, 0 when none),
    // when it was handed over and to which engine (see the loop)
    uint64_t busyId = 0;
    int64_t busyAt = 0;
    const MlxrDepthEngine* busyEngine = nullptr;
    static constexpr int64_t kBusyMaxNs = 1000000000LL;   // a run that failed gives no map: free again by then
    // pipelined capture/draw (see the loop)
    bool depthWanted = false;
    int64_t lastMovedAt = 0;
    const uint8_t* src = nullptr;   // XShm reads: the picture's pixels
    bool havePic = false;           // a picture to draw (XShm or the GPU's zero-copy source)
    bool picZeroCopy = false;       // ... and it is Chrome's window buffer on the GPU (zero-copy)
    int64_t heldAt = 0;             // when the picture held was last read (it is current as of then)
    // zero-copy pictures changing without damage reports (see the loop)
    static constexpr int64_t kMissCheckNs = 250000000LL;
    int64_t missCheckAt = 0;
    uint64_t missHash = 0;
    int missed3 = 0;
    std::vector<uint8_t> missPixels;
    int stride = 0;
    Rect grabbedRect, srcRect;
    uint64_t lastHash = 0, shown = 0, grabs = 0;
    std::vector<float> lastKey;
    int64_t pointerMovedAt = 0;
    int64_t statsAt = mlxr_depth_now_ns();
    double warpSum = 0, grabSum = 0;
    // Where the loop's time goes, logged with the stats: each part's mean and
    // worst over the period. And the gaps between frames with a new picture
    // handed to SteamVR: an even video shows one video frame's time each, a
    // dropped or late picture a longer one.
    struct Part { double sum = 0, worst = 0; int n = 0; void add(double ms) { sum += ms; worst = std::max(worst, ms); ++n; } };
    enum { kWindow, kRead, kDepthIn, kDepthOut, kPrep, kDraw, kUpload, kSubmit, kLoop, kParts };
    static const char* const kPartNames[kParts] = {"window", "read", "depth in", "depth out", "prep", "draw", "upload", "submit", "loop"};
    Part parts[kParts];
    auto ms = [](int64_t a, int64_t b) { return (b - a) / 1e6; };
    // A depth map's way from its picture to the drawing, for the stats: the
    // engine free until a picture went in, the picture's age then, the
    // engine's convert, wait and run, until the loop took the map, the
    // helper, until the loop took it from the helper
    enum { kFree, kOld, kConvert, kWait, kRun, kPickup, kHelper, kQueue, kSteady, kPost, kLook, kTaken, kPathParts };
    static const char* const kPathNames[kPathParts] = {"free", "picture age", "convert", "wait", "run", "pickup", "helper", "(queue", "steady", "post", "look)", "taken"};
    std::mutex pathM;   // the helper's parts (kQueue..kLook) are added there
    Part path[kPathParts];
    int64_t freeAt = 0;
    std::vector<float> behind;   // each drawn picture's mapBehind, for its median
    // How the depth looks over time, for finding what changes the feel of it
    // while a video plays (user, 2026-11-01: "the 3D shows less, or the
    // depth feels different", now and then): each map's middle depth and
    // spread (95th less 5th percentile) and their largest step from one map
    // to the next, scene cuts, and what the frames were drawn with
    struct Looks {
        int maps = 0, cuts = 0, frames = 0, capped = 0;
        float midLo = 1, midHi = 0, spreadLo = 1, spreadHi = 0, midStep = 0, spreadStep = 0, lastMid = -1, lastSpread = -1;
        float convLo = 1, convHi = 0, sepLo = 1, sepHi = 0;
        void map(const float* l, bool cut) {
            const float mid = l[1], spread = l[2] - l[0];
            ++maps; cuts += cut;
            midLo = std::min(midLo, mid); midHi = std::max(midHi, mid);
            spreadLo = std::min(spreadLo, spread); spreadHi = std::max(spreadHi, spread);
            if (lastMid >= 0 && !cut) { midStep = std::max(midStep, std::fabs(mid - lastMid)); spreadStep = std::max(spreadStep, std::fabs(spread - lastSpread)); }
            lastMid = mid; lastSpread = spread;
        }
        void frame(float conv, float sep, float userSep) {
            ++frames; capped += sep < userSep * 0.99f;
            convLo = std::min(convLo, conv); convHi = std::max(convHi, conv);
            sepLo = std::min(sepLo, sep); sepHi = std::max(sepHi, sep);
        }
        void clear() { const float m = lastMid, sp = lastSpread; *this = Looks{}; lastMid = m; lastSpread = sp; }
    } looks;
    // Also the gaps between new pictures read from Chrome (what we were
    // given), and how many of the headset's refreshes each picture handed
    // over got (by SteamVR's vsync count at the hand-over; a 30 fps video on
    // a 90 Hz display should get 3 each)
    std::vector<float> gaps, readGaps;
    int64_t pictureShownAt = 0, pictureReadAt = 0, lastReadAt = 0;
    uint64_t lastVsync = 0;
    int refreshes[7] = {0};   // pictures that got 0..5 refreshes, and 6 or more
    double latencySum = 0; int latencyN = 0, newPictures = 0;
    // how much older the picture a drawn depth map was made from is than the
    // picture drawn with it (the stats' age stops when the map is taken, before
    // it waits to be drawn)
    int64_t mapCapturedAt = 0;
    double mapBehindSum = 0; int mapBehindN = 0;
    int missed = 0;   // pictures handed over after the vsync their slot was for
    uint64_t failedRuns = 0;   // the depth engine's failed runs, as last seen
    unsigned failedEngine = 0;  // ... of this engine (DepthOwner::starts): each counts from 0
    bool drawFailed = false;   // a drawing failed since the last stats
    // What keeps the view from working as it should, for the popup ("" when
    // nothing): a code the popup puts in the user's language (viewProblem*)
    auto setProblem = [&](const char* p) {
        std::lock_guard<std::mutex> l(sh->m);
        if (sh->problem == p) return;
        if (*p) LOG("problem: %s", p);
        sh->problem = p;
    };
    // Pacing. A frame with a new picture is handed over at a fixed point of
    // the headset's refresh (kSlotNs after its vsync), the refresh chosen by
    // the picture's place in the video's own time, counted in refreshes from
    // an anchor picture: the video's step (refreshes a picture: the mean of
    // the last 16 read gaps, those far from their median left out) is added
    // for each picture, and the count follows the
    // reads only slowly (the video's clock against the display's) or, off by
    // over a refresh, at once. So a read a few ms early or late (Chrome's own
    // jitter, up to 6 ms) does not move its picture, and a 30 fps video on a
    // 90 Hz display gets 3 refreshes a picture, 24 fps 4-4-3-4, 60 fps 1-2.
    // Handed over as soon as each was done, the pictures of a 30 fps video
    // got 2, 3 or 4 (only about half of them 3) although Chrome gave them
    // evenly (2026-10-31). Any refresh rate (72, 90, 120, 144 Hz) is read
    // from SteamVR again every second, as it can change while running. A
    // picture too late for its slot anchors anew, kMarginNs after it is
    // done, so the next pictures have that much room for their own delays;
    // when the 16 pictures since had over a refresh more room than that, the
    // slots move a refresh earlier (the anchor's own delay is not kept).
    static constexpr int64_t kSlotNs = 1000000, kMarginNs = 4000000;
    float refreshHz = 0;
    int64_t refreshAt = INT64_MIN / 2, anchorRead = 0, anchorSlot = 0, lastSlot = 0, prevRead = 0, roomMin = INT64_MAX;
    int roomN = 0;
    double phase = 0, step = 0;   // refreshes: the newest picture's place after the anchor, and between pictures
    std::deque<double> stepGaps;  // the last read gaps, refreshes
    int anchors = 0;              // anchored anew (for the stats)
    // the first slot at or after t; 0 when SteamVR gives no vsync timing
    auto slotAfter = [&](int64_t t) -> int64_t {
        const int64_t now = mlxr_depth_now_ns();
        if (now - refreshAt > 1000000000LL && vr::VRSystem()) {
            refreshAt = now;
            const float hz = vr::VRSystem()->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_DisplayFrequency_Float);
            if (std::fabs(hz - refreshHz) > 0.5f) {
                if (hz > 0) LOG("headset refresh %.1f Hz", (double)hz);
                refreshHz = hz; anchorSlot = 0; step = 0; stepGaps.clear();
            }
        }
        float since = 0; uint64_t count = 0;
        if (refreshHz < 20 || !vr::VRSystem() || !vr::VRSystem()->GetTimeSinceLastVsync(&since, &count)) return 0;
        const int64_t period = (int64_t)(1e9 / refreshHz);
        int64_t slot = now - (int64_t)(since * 1e9) + kSlotNs;
        if (slot < t) slot += (t - slot + period - 1) / period * period;
        else slot -= (slot - t) / period * period;
        return slot;
    };
    // when the frame with the picture read at `read` is to be handed over
    auto pictureSlot = [&](int64_t read) -> int64_t {
        const int64_t now = mlxr_depth_now_ns();
        const int64_t next = slotAfter(now + kMarginNs);
        if (!next) return 0;
        const int64_t period = (int64_t)(1e9 / refreshHz);
        const double gap = prevRead && read > prevRead ? (double)(read - prevRead) / period : 0;
        prevRead = read;
        // a picture's gap counts up to 125 ms (videos of 8 fps and more): 8
        // refreshes, as before, was 55 ms at 144 Hz, and a video under 18 fps
        // anchored anew at every picture (review 2026-10-31)
        const double most = std::max(8.0, refreshHz * 0.125);
        if (gap > 0 && gap < most) {
            stepGaps.push_back(gap);
            if (stepGaps.size() > 16) stepGaps.pop_front();
            std::vector<double> sorted(stepGaps.begin(), stepGaps.end());
            std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
            const double median = sorted[sorted.size() / 2];
            double sum = 0; int n = 0;
            for (double g : stepGaps) if (g > 0.6 * median && g < 1.6 * median) { sum += g; ++n; }
            step = n ? sum / n : median;
        }
        if (anchorSlot && gap > 0 && gap < most && step > 0) {
            const double measured = (double)(read - anchorRead) / period;
            phase += step;
            const double off = measured - phase;
            phase = std::fabs(off) > 1.0 ? measured : phase + 0.1 * off;
            int64_t want = slotAfter(anchorSlot + std::llround(phase) * period - period / 2);
            if (want && want <= lastSlot) want = slotAfter(lastSlot + period / 2);   // one picture a refresh
            if (want >= now) {
                roomMin = std::min(roomMin, want - now);
                if (++roomN >= 16) {
                    if (roomMin > kMarginNs + period) { anchorSlot -= period; want = std::max(want - period, slotAfter(lastSlot + period / 2)); }
                    roomMin = INT64_MAX; roomN = 0;
                }
                return lastSlot = want;
            }
        }
        ++anchors;
        anchorRead = read; phase = 0; roomMin = INT64_MAX; roomN = 0;
        anchorSlot = next > lastSlot ? next : slotAfter(lastSlot + period / 2);
        return lastSlot = anchorSlot;
    };
    int64_t dumpedAt = 0;
    int dumps = 0;
    // Flat while the picture moves (scrollflat, see the loop): the rectangle
    // last seen, when it last moved, the read the settled picture's depth
    // map must come from, and the first drawn frame that may show again
    Rect seenRect;
    std::vector<std::array<int, 4>> seenOnly;   // a gallery's pictures as last seen
    int64_t rectMovedAt = 0, settledReadAt = 0, seenMovingAt = 0;
    int moveW = 0, moveH = 0;           // the picture's size when the move began
    bool moving = false, settledMap = false;
    uint64_t renders = 0, showFrom = 0;
    // Frames drawn and waiting to be handed over, oldest first: at most two
    // (GpuWarp draws the next into its third buffer), so a 60 fps video's
    // next picture is drawn while the last one waits for its refresh
    struct Waiting { int buffer; int64_t at, readAt; bool picture; uint64_t render; Rect rect; };
    std::deque<Waiting> waiting;
    int movePauses = 0;
    // Drawing (see the loop): a picture or depth map not yet drawn, when
    // Chrome last gave a new picture, drawings with a new picture
    bool frameDirty = false, depthDirty = false;
    int64_t lastFrameAt = 0, lastDrawAt = 0;
    int frameDraws = 0;
    static constexpr int64_t kPlayingNs = 100000000LL;   // a new picture this recently: the video plays
    // still for this long: the move is over. 150 ms showed the 3D for a
    // moment at each turn while scrolling quickly up and down (2026-10-28)
    static constexpr int64_t kSettleNs = 300000000LL;
    static constexpr int64_t kSettleMaxNs = 1000000000LL;   // shown by then even without a new map

    auto stopView = [&] {
        vrv.hide();
        src = nullptr; havePic = false; picZeroCopy = false;   // the reader's images go with it
        reader.stop();
        // and the window's buffers the GPU imported (1080p: 8 MB each, up
        // to 6), which also keep Xwayland's buffers alive
        if (gpuUp) gpu.dropWindowBuffers();
        cap.close();
        frameSeq = 0;
        parent = vr::k_ulOverlayHandleInvalid;
        if (active) LOG("view stopped");
        active = false;
    };
    auto startView = [&](const std::string& marker) -> std::string {
        if (!gpuUp) {
            std::string err = gpu.init();
            if (!err.empty()) { gpu.shutdown(); return "GPU: " + err; }   // a clean start next time
            gpuUp = true;
            pinLoadedLibraries();
        }
        std::string err = cap.open(marker);
        if (!err.empty()) return err;
        // the reader takes the damage reports from here on (unread on this
        // connection they would pile up)
        if (cap.damage) { XDamageDestroy(cap.d, cap.damage); cap.damage = 0; }
        err = reader.start(DisplayString(cap.d), cap.win);
        if (!err.empty()) return err;
        std::string key;
        bool hung = false;
        for (int i = 0; i < 10 && key.empty() && !hung && !g_stop; ++i) { key = findOverlayKeyByTitle(marker, &hung); if (key.empty()) usleep(150 * 1000); }
        if (hung) return "OpenVR: SteamVR does not answer (vrcmd timed out)";
        if (key.empty()) return "no SteamVR overlay has the window title marker";
        err = vrv.init();
        if (!err.empty()) return err;
        if (vrv.o->FindOverlay(key.c_str(), &parent) != vr::VROverlayError_None) {
            // SteamVR (vrcmd) has the overlay but our connection does not see
            // it: SteamVR went away without a Quit event (a crash) and came
            // back, and the old connection is dead (review 2026-10-31)
            LOG("Chrome's overlay %s is not found through our OpenVR connection: connecting again", key.c_str());
            vrv.shutdown();
            err = vrv.init();
            if (!err.empty()) return err;
            if (vrv.o->FindOverlay(key.c_str(), &parent) != vr::VROverlayError_None) return "Chrome overlay vanished";
        }
        parentKey = key; parentCheckedAt = mlxr_depth_now_ns();
        LOG("view started on %s", key.c_str());
        return {};
    };

    // A new view or depth model: the helper's state starts over, and what it
    // still hands back for the old one is dropped
    auto startOver = [&] {
        const unsigned g = ++mapGen;
        stereoMode = 0; subtitleEase.reset(); uiEase.reset();
        helper.post([&, g] {
            steady.reset(); rangeNorm.reset(); stereoFinder.reset(); subtitleFinder.reset(); stereoMode = 0;
            std::lock_guard<std::mutex> l(doneM);
            std::fill(subBox, subBox + 4, 0.0f);
            if (doneMap.gen != g) doneMap.ready = false;
        });
    };

    while (!g_stop) {
        Rect r; bool want; std::string marker; uint64_t g; float strength, conv, sharpen; int smooth, detail, subtitles, scrollflat;
        std::vector<std::array<int, 4>> ui, popupHoles, onlyRects;
        {
            std::lock_guard<std::mutex> l(sh->m);
            r = sh->rect; want = sh->want; marker = sh->marker; g = sh->generation;
            strength = sh->strength; conv = sh->convergence; smooth = sh->smooth; sharpen = sh->sharpen; ui = sh->ui;
            detail = sh->detail; popupHoles = sh->holes; onlyRects = sh->only; subtitles = sh->subtitles; scrollflat = sh->scrollflat;
        }
        if (active && (!want || g != gen)) stopView();
        if (!active && want && g != gen) {
            gen = g;
            sh->viewBusy = true;
            std::string err = startView(marker);
            // Replies name the marker they answer, so a late one cannot reach
            // a newer view request; retry asks the extension to try again
            // with a fresh marker (the overlay keys change after a SteamVR restart)
            if (err.empty()) {
                active = true; haveSeq = 0; depthMap.clear(); mapCapturedAt = 0; verified = false;
                bars.reset(); mapRects.clear(); mapRect = {0, 0, 1, 1}; comfortState.reset(); startOver();
                stereoCheckedAt = INT64_MIN / 2; anchorSlot = 0;
                vrv.placedWidth = 0;   // the comfort limits wait for this view's own placement
                src = nullptr; havePic = false; picZeroCopy = false; waiting.clear(); lastKey.clear(); grabbedRect = Rect{};
                missCheckAt = 0; missHash = 0; missed3 = 0;
                seenRect = Rect{}; seenOnly.clear(); moving = false; showFrom = 0; frameDirty = depthDirty = false; lastFrameAt = 0;
                gpu.setAsync(true);
                setProblem(""); drawFailed = false;
                sh->reply("{\"type\":\"view\",\"ok\":true,\"marker\":\"" + jsonEscape(marker) + "\"}");
            } else {
                LOG("view failed: %s", err.c_str()); stopView();
                sh->reply("{\"type\":\"view\",\"ok\":false,\"retry\":true,\"marker\":\"" + jsonEscape(marker) +
                          "\",\"error\":\"" + jsonEscape(err) + "\"}");
            }
        }
        if (vrv.quitting()) {
            LOG("SteamVR is quitting: overlay connection closed");
            const bool was = active;
            stopView();
            vrv.shutdown();
            if (was) sh->reply("{\"type\":\"view\",\"ok\":false,\"retry\":true,\"marker\":\"" + jsonEscape(marker) +
                               "\",\"error\":\"SteamVR restarted\"}");
        }
        if (active && (cap.lost || reader.lost)) {
            LOG("the X connection to Chrome's window was lost: starting the view again");
            stopView();
            sh->reply("{\"type\":\"view\",\"ok\":false,\"retry\":true,\"marker\":\"" + jsonEscape(marker) +
                      "\",\"error\":\"X display lost\"}");
        }
        if (detail != detailSetting) { detailSetting = detail; refusedDetail = -1; }
        if (active && detail != owner->detail.load() && detail != refusedDetail && owner->engine.load()) {
            // Another depth model: a second or two of model loading, Chrome's
            // flat picture meanwhile. When none of the setting's models
            // starts, the one that ran comes back (start() had stopped it, and
            // the 3D stayed flat until the extension reconnected; review
            // 2026-10-31), and that setting is not tried again until it changes.
            vrv.hide();
            const int before = owner->detail.load();
            const std::string e = owner->start(detail);
            if (!e.empty()) {
                // the popup says the setting could not start (refused); an
                // error reply before the model one was overwritten by it at once
                LOG("%s", e.c_str()); refusedDetail = detail;
                const std::string back = owner->start(before);
                sh->reply(back.empty() ? "{\"type\":\"model\",\"model\":\"" + jsonEscape(owner->modelName()) + "\",\"refused\":" + std::to_string(detail) +
                                             ",\"error\":\"" + jsonEscape(e) + "\"}"
                                       : "{\"type\":\"error\",\"error\":\"" + jsonEscape(e + "; " + back) + "\"}");
            } else sh->reply("{\"type\":\"model\",\"model\":\"" + jsonEscape(owner->modelName()) + "\"" + owner->refusedField() + "}");
            haveSeq = 0; depthMap.clear(); mapCapturedAt = 0; ++mapGen;
            // the new engine needs a picture: a still one (an image, a
            // paused video) gives no new frame, and stayed flat (review
            // 2026-10-29); in a move, the settle read went to the old engine
            depthWanted = true;
            if (moving) { settledReadAt = 0; rectMovedAt = mlxr_depth_now_ns(); }
        }
        sh->viewBusy = active;
        if (!active) { usleep(20 * 1000); continue; }
        const int64_t t0 = mlxr_depth_now_ns();
        auto idle = [&](const char* why) {
            vrv.hide();
            // nothing is drawn now: the popup's numbers stayed at their last
            // values while the 3D was paused (review 2026-11-01)
            sh->fps = 0; sh->frameFps = 0;
            lastKey.clear();   // draw (and so show) again once things are back
            // frames drawn before are not handed over when it shows again:
            // they flashed the picture from before the pause (review 2026-10-31)
            waiting.clear();
            // and the reader rests: it went on reading every frame of a video
            // while nothing was shown (review 2026-11-02); asked for the
            // rectangle again, it reads at once
            reader.want(Rect{}, false);
            if (t0 - statsAt > 5000000000LL) {
                LOG("view waiting: %s (rect %d,%d %dx%d visible %d)", why, r.x, r.y, r.w, r.h, r.visible ? 1 : 0);
                statsAt = t0; frames = 0; grabSum = warpSum = 0; frameDraws = 0; movePauses = 0;
                for (Part& p : parts) p = Part{};
                { std::lock_guard<std::mutex> pl(pathM); for (Part& p : path) p = Part{}; }
                behind.clear(); freeAt = 0; looks.clear();
                gaps.clear(); readGaps.clear(); std::fill(std::begin(refreshes), std::end(refreshes), 0);
                latencySum = 0; latencyN = 0; newPictures = 0; missed = 0; mapBehindSum = 0; mapBehindN = 0;
                ageSum = 0; ageN = 0;
            }
            usleep(30 * 1000);
        };
        if (!r.visible) { idle("no visible video"); continue; }
        if (!vrv.o->IsOverlayVisible(parent)) {
            // Hidden, or gone: Chrome's window closed, or SteamVR died without
            // a Quit event and our handles are dead. Checked every 3 s; the
            // extension is asked to start again, which connects anew if needed.
            vr::VROverlayHandle_t now = vr::k_ulOverlayHandleInvalid;
            if (t0 - parentCheckedAt > 3000000000LL) {
                parentCheckedAt = t0;
                if (vrv.o->FindOverlay(parentKey.c_str(), &now) != vr::VROverlayError_None || now != parent) {
                    LOG("Chrome's overlay %s is gone: starting the view again", parentKey.c_str());
                    stopView();
                    sh->reply("{\"type\":\"view\",\"ok\":false,\"retry\":true,\"marker\":\"" + jsonEscape(marker) +
                              "\",\"error\":\"Chrome's window is gone from SteamVR\"}");
                    continue;
                }
            }
            idle("Chrome window not shown in VR"); continue;
        }
        // The oldest frame drawn is handed to SteamVR at its slot (pacing,
        // above; at once when it has none), once the GPU has finished it.
        // Meanwhile the loop reads Chrome's next frame, so reading and
        // drawing overlap.
        auto present = [&] {
            if (waiting.empty() || mlxr_depth_now_ns() < waiting.front().at || !gpu.done(waiting.front().buffer, 2000000)) return;
            const Waiting w = waiting.front();
            waiting.pop_front();
            const int64_t s0 = mlxr_depth_now_ns();
            const vr::EVROverlayError se = vrv.submit(gpu.texture(w.buffer));
            if (se != vr::VROverlayError_None) {
                if (!reportedSubmit) { LOG("SetOverlayTexture failed: %s", vrv.o->GetOverlayErrorNameFromEnum(se)); reportedSubmit = true; }
                return;
            }
            ++shown;
            // placed where the picture was when drawn: the video may have
            // moved since, and the frame shows the old place (review 2026-10-31)
            if (vrv.anchor(parent, w.rect) && !moving && w.render >= showFrom) vrv.show();
            if (!verified && shown >= 120) { vrv.verify(parent, w.rect); verified = true; }   // once a view, ~2 s in
            const int64_t s1 = mlxr_depth_now_ns();
            parts[kSubmit].add(ms(s0, s1));
            if (w.picture) {
                if (pictureShownAt && vrv.shown) gaps.push_back((float)ms(pictureShownAt, s1));
                pictureShownAt = vrv.shown ? s1 : 0;
                latencySum += ms(w.readAt, s1); ++latencyN;
                if (w.at && refreshHz > 20 && s0 - w.at > (int64_t)(1e9 / refreshHz) - kSlotNs) ++missed;
                // The refresh it was handed over in, by time: at 144 Hz
                // SteamVR's compositor at times moves its count on only every
                // other refresh (by 2, its vsync then up to two refreshes
                // back), which read as 4 and 6 refreshes for pictures handed
                // over 5 and 5 apart (2026-10-31)
                float sinceVsync = 0; uint64_t vsync = 0;
                if (vr::VRSystem() && vr::VRSystem()->GetTimeSinceLastVsync(&sinceVsync, &vsync) && refreshHz > 20) {
                    vsync += (uint64_t)(sinceVsync * refreshHz);
                    if (lastVsync && vrv.shown && vsync >= lastVsync) ++refreshes[std::min<uint64_t>(vsync - lastVsync, 6)];
                    lastVsync = vrv.shown ? vsync : 0;
                }
            }
        };
        present();
        const int64_t tWindow = mlxr_depth_now_ns();
        // The 3D stays on while the window is moved (it trails Chrome's
        // picture by a frame or so then); the window's place is re-measured
        // on every change. Scrolling: see scrollflat below.
        vrv.measure(parent);
        // The window moved, turned or was resized: draw again so the frame is
        // handed over and re-anchored even when the picture itself is still
        // (an image or a paused video makes no new frames to carry it along)
        if (vrv.movedAt != lastMovedAt) { lastMovedAt = vrv.movedAt; lastKey.clear(); }
        // Scrolling (or a resize) moves the picture in Chrome's window. Our
        // overlay trails Chrome's own drawing by a frame or more and the depth
        // map in use is of the picture as it was cut before, so the 3D tore
        // and flickered while the page scrolled (headset, 2026-10-27). With
        // scrollflat the overlay is hidden then (Chrome's flat picture shows)
        // and comes back once the picture has been still for kSettleNs and a
        // depth map of a read made after that is drawn. Moves of a pixel or
        // two (layout jitter) do not count. A gallery's region (the box
        // around the pictures in view) can keep its size and place while
        // the pictures scroll through it, so it took only the moments a gap
        // between rows reached the window's edge as scrolling and showed the
        // 3D in between (headset, 2026-10-28): its pictures moving count too.
        {
            const bool first = seenRect.w == 0;
            bool jumped = std::abs(r.x - seenRect.x) > 2 || std::abs(r.y - seenRect.y) > 2
                          || std::abs(r.w - seenRect.w) > 2 || std::abs(r.h - seenRect.h) > 2;
            if (onlyRects.size() != seenOnly.size()) jumped = true;
            for (size_t i = 0; !jumped && i < onlyRects.size(); ++i)
                for (int k = 0; k < 4; ++k) jumped |= std::abs(onlyRects[i][k] - seenOnly[i][k]) > 2;
            seenOnly = onlyRects;
            // scrolling input the extension saw: the page is about to move
            const int64_t inputAt = sh->movingAt.load();
            if (inputAt != seenMovingAt) { seenMovingAt = inputAt; jumped = true; }
            if (first || jumped) {
                if (!first && scrollflat) {
                    if (!moving) { moveW = srcRect.w; moveH = srcRect.h; ++movePauses; vrv.hide(); }
                    moving = true;
                    settledReadAt = 0; settledMap = false;
                }
                seenRect = r; rectMovedAt = t0;
            }
            if (!scrollflat && moving) { moving = false; lastKey.clear(); }
        }
        // Chrome's popup windows (menus, tooltips) over the video and the
        // FrameDepth popup (drawn inside Chrome's window; its place comes from
        // the extension in window pixels): our overlay is made see-through
        // there, so Chrome's own flat drawing shows and the rest stays 3D.
        // X popups are looked for every 4th loop (a few X calls).
        if ((++loops & 3) == 0) xPopups = cap.popupRects(r);
        std::vector<std::array<int, 4>> holeRects = xPopups;
        holeRects.insert(holeRects.end(), popupHoles.begin(), popupHoles.end());
        parts[kWindow].add(ms(tWindow, mlxr_depth_now_ns()));
        present();
        const int64_t tRead = mlxr_depth_now_ns();

        // The newest picture the reader has (Reader). The move is over: the
        // settled picture is asked for now, whatever damage says.
        const bool settleRead = moving && !settledReadAt && t0 - rectMovedAt >= kSettleNs;
        reader.want(r, settleRead);
        bool newFrame = false, settledFrame = false;
        int64_t t1 = t0;
        Reader::Frame fr;
        if (reader.take(fr, frameSeq)) {
            frameSeq = fr.seq;
            if (fr.failed) { src = nullptr; havePic = false; idle("window capture failed"); continue; }
            // The GPU's state follows the kind of picture: what it imported
            // goes when it cannot import one, or reads are XShm again. Left
            // as it was, the model's input was shrunk from the last window
            // buffer (an old page, an old size) or, with none, came out
            // empty and was read past its end (review 2026-11-02).
            if (fr.fd >= 0 && !gpu.useWindowBuffer(fr.fd, fr.bw, fr.bh, fr.bstride, fr.boffset, fr.modifier, fr.key, fr.r.x, fr.r.y, fr.r.w, fr.r.h)) {
                // the GPU cannot take the window's buffer: XShm from here on
                reader.refuseZeroCopy("the GPU could not import Chrome's window buffer");
                gpu.dropWindowBuffers();
                src = nullptr; havePic = picZeroCopy = false;
                continue;
            }
            if (fr.fd < 0 && picZeroCopy) gpu.dropWindowBuffers();
            const bool otherKind = havePic && picZeroCopy != (fr.fd >= 0);
            picZeroCopy = fr.fd >= 0;
            t1 = heldAt = fr.at;
            ++grabs; grabSum += fr.readMs;
            const bool moved = fr.asked.x != grabbedRect.x || fr.asked.y != grabbedRect.y || fr.asked.w != grabbedRect.w || fr.asked.h != grabbedRect.h;
            newFrame = !havePic || moved || otherKind || fr.hash != lastHash;
            // Zero-copy reads know a new picture only from damage reports
            // inside its rectangle, so a picture changing without them would
            // stay as it was. With no new picture for kMissCheckNs, its
            // shrunk copy is compared with the one from the last such check
            // (none when a new picture came between): changed, it is new
            // after all, and three times in a row reads go to XShm, which
            // checks the pixels themselves (review 2026-11-02).
            bool missed = false;
            if (havePic && !newFrame && picZeroCopy && t0 - missCheckAt >= kMissCheckNs) {
                missCheckAt = t0;
                const uint64_t h = gpu.shrink(fr.r.x, fr.r.y, fr.r.w, fr.r.h, 64, 36, missPixels) ? fnv(missPixels) : 0;
                if (h && missHash && h != missHash) {
                    newFrame = missed = true;
                    if (++missed3 >= 3) reader.refuseZeroCopy("Chrome's picture changed without a damage report");
                } else if (h && h == missHash) missed3 = 0;
                missHash = h;
            }
            if (newFrame && !missed) missHash = 0;
            havePic = true;
            src = fr.px; stride = fr.stride; lastHash = fr.hash; grabbedRect = fr.asked; srcRect = fr.r;
            cap.winW = fr.winW; cap.winH = fr.winH;
            // read after the move settled: this one's depth map brings the 3D back
            settledFrame = settleRead && fr.at >= rectMovedAt + kSettleNs;
            if (newFrame) {
                frameDirty = true; lastFrameAt = t1; pictureReadAt = t1; ++newPictures;
                if (lastReadAt && t1 - lastReadAt < 1000000000LL) readGaps.push_back((float)ms(lastReadAt, t1));
                lastReadAt = t1;
            }
        }
        if (!havePic) {
            // no picture yet (the reader's first one is on its way)
            reader.waitNew(frameSeq, 20000000);
            continue;
        }
        r = srcRect;   // the rectangle as read (clamped to the window)
        const int64_t tDepthIn = mlxr_depth_now_ns();
        parts[kRead].add(ms(tRead, tDepthIn));
        vrv.winW = cap.winW; vrv.winH = cap.winH;
        if (settledFrame) {
            // a depth map of this read (or a later one) brings the 3D back;
            // a picture cut differently now (scrolled partly out of view)
            // starts the steadying average over
            settledReadAt = t1; depthWanted = true;   // (t1: the read's own time)
            if (r.w != moveW || r.h != moveH) helper.post([&] { steady.reset(); });
        }

        bool newDepth = false;
        MlxrDepthEngine* depth = owner->engine.load();
        if (depth && owner->starts.load() != tunedStart) {
            // Steady replaces the engine's per texel average (another engine
            // after a model switch starts with its defaults again)
            MlxrDepthParams p;
            mlxr_depth_params_default(&p);
            p.tauMs = 0; p.rangeTauMs = kSteadyRangeTau;
            mlxr_depth_set_params(depth, &p);
            tunedStart = owner->starts.load();
            helper.post([&] { steady.reset(); rangeNorm.reset(); });
        }
        if (depth) {
            int mw = 0, mh = 0;
            mlxr_depth_size(depth, &mw, &mh);
            // The finished map is taken first, so this loop's picture can go
            // to the engine it freed: taken after the hand-over, the next
            // picture waited a loop (a 10 ms read at 1080p), the engine idle
            // meanwhile (review 2026-11-01)
            const int64_t tDepthOut = tDepthIn;
            const MlxrDepthResult* res = mlxr_depth_acquire(depth, haveSeq);
            if (res && res->frameId >= busyId) { if (busyId) freeAt = mlxr_depth_now_ns(); busyId = 0; }
            // A map of a capture this view did not submit (the engine's newest
            // can be the last page's, as a new view starts) is skipped: drawn,
            // it showed the old picture's depth for a map's time (review 2026-10-31)
            const auto at = res ? mapRects.find(res->frameId) : mapRects.end();
            if (res && at == mapRects.end()) { haveSeq = res->seq; mlxr_depth_release(depth, res); res = nullptr; }
            if (res) {
                haveSeq = res->seq;
                const std::array<float, 4> rect = at->second;
                ageSum += (mlxr_depth_now_ns() - res->captureNs) / 1e6; ++ageN;
                const int64_t acquired = mlxr_depth_now_ns();
                path[kConvert].add(ms(res->captureNs, res->submitNs)); path[kWait].add(ms(res->submitNs, res->startNs));
                path[kRun].add(ms(res->startNs, res->doneNs)); path[kPickup].add(ms(res->doneNs, acquired));
                auto map = std::make_shared<std::vector<uint8_t>>(res->rgba, res->rgba + (size_t)res->width * res->height * 4);
                auto raw = res->raw ? std::make_shared<std::vector<float>>(res->raw, res->raw + (size_t)res->width * res->height) : nullptr;
                const int resets = res->resets;
                const int64_t made = res->doneNs;
                const int w = res->width, h = res->height;
                const int64_t captured = res->captureNs;
                // Steady starts over at a scene cut the engine confirmed
                // (DEPTH_RESET_RANGE). Every unconfirmed jump (any large
                // change between two maps, DEPTH_RESET_TEXEL) started it
                // over too, and at 8-15 maps a second fast pans counted as
                // jumps: the steadying left for 0.4 s each time (review
                // 2026-11-01; no difference on the test scenes). Its own
                // check of the colour still catches cuts.
                const bool cut = (res->resets & DEPTH_RESET_RANGE) != 0;
                mlxr_depth_release(depth, res);
                PostSettings post; post.smooth = smooth; post.dilate = kEdgeDilate;
                const unsigned g = mapGen;
                helper.post([&, map, raw, resets, made, w, h, captured, cut, post, rect, g, acquired] {
                    if (g != mapGen) return;
                    const int64_t h0 = mlxr_depth_now_ns();
                    if (raw) rangeNorm.apply(raw->data(), w * h, resets, made, map->data());
                    steady.apply(*map, w, h, captured, cut);   // (Steady's own cuts are not counted)
                    const int64_t h1 = mlxr_depth_now_ns();
                    postDepth(*map, w, h, post);
                    const int64_t h2 = mlxr_depth_now_ns();
                    float look[3];
                    depthLook(*map, look);
                    {
                        std::lock_guard<std::mutex> l(pathM);
                        path[kQueue].add(ms(acquired, h0)); path[kSteady].add(ms(h0, h1)); path[kPost].add(ms(h1, h2)); path[kLook].add(ms(h2, mlxr_depth_now_ns()));
                    }
                    std::lock_guard<std::mutex> l(doneM);
                    doneMap.rgba.swap(*map);
                    doneMap.w = w; doneMap.h = h; doneMap.captureNs = captured; doneMap.rect = rect;
                    doneMap.gen = g; doneMap.ready = true; doneMap.acquiredNs = acquired; doneMap.helperNs = mlxr_depth_now_ns();
                    std::copy(look, look + 3, doneMap.look); doneMap.cut = cut;
                    reader.nudge();
                });
                parts[kDepthOut].add(ms(tDepthOut, mlxr_depth_now_ns()));
            }
            const int64_t tSubmit = mlxr_depth_now_ns();
            // a new picture waits for the depth engine to be due (a still
            // image or paused video gives only one)
            if (newFrame) depthWanted = true;
            // the picture at the model's size, made once a loop for all that
            // want it (the 3D video and subtitle checks, the depth model):
            // each made its own before (review 2026-11-01)
            // a part of the picture at the model's size: from the CPU pixels
            // (XShm), or shrunk on the GPU from Chrome's window (zero-copy)
            const Shrink shrinkPic = [&](int x, int y, int w, int h, std::vector<uint8_t>& o) {
                if (picZeroCopy) {
                    if (gpu.shrink(srcRect.x + x, srcRect.y + y, w, h, mw, mh, o)) return true;
                    o.clear();
                    return false;
                }
                toModelSize(src + (size_t)y * stride + (size_t)x * 4, stride, w, h, mw, mh, o);
                return true;
            };
            // (empty when it could not be made)
            std::shared_ptr<std::vector<uint8_t>> thumb;
            auto modelThumb = [&] {
                if (!thumb) { thumb = std::make_shared<std::vector<uint8_t>>(); shrinkPic(0, 0, r.w, r.h, *thumb); }
                return thumb;
            };
            // Already 3D? (a few times a second; while it is, the depth model rests)
            if (mw && havePic && newFrame) {
                if (t1 - stereoCheckedAt >= StereoFinder::kEveryNs && !modelThumb()->empty()) {
                    stereoCheckedAt = t1;
                    auto thumb = modelThumb();
                    const unsigned g = mapGen;
                    helper.post([&, thumb, mw, mh, t1, g] {
                        if (g != mapGen) return;
                        const int before = stereoFinder.mode;
                        stereoFinder.update(thumb->data(), mw, mh, t1);
                        if (stereoFinder.mode != before) LOG("the video is %s", stereoFinder.mode == 1 ? "3D side by side: shown as it is"
                                                                     : stereoFinder.mode == 2 ? "3D top and bottom: shown as it is" : "flat: made 3D");
                        stereoMode = stereoFinder.mode;
                    });
                }
                sh->stereoMode = stereoMode.load();
            }
            // A capture is handed over only once the engine has finished the
            // last one. The engine takes one while running and makes it wait
            // for the run; it is then most of a run old when its own begins,
            // and the newer pictures of that time go unused: a drawn 60 fps
            // picture's depth came from one 220 ms older, so depth trailed
            // moving things, and pausing brought two or three maps of moving
            // pictures in turn, the depth shifting each time (headset,
            // 2026-11-01). Waiting for the run to end, the next new picture
            // goes in fresh.
            if (depth != busyEngine || (busyId && t0 - busyAt >= kBusyMaxNs)) busyId = 0;
            // While a video plays, a picture goes in the loop it came in (and
            // the engine's rate gate, made to be asked once a video frame, is
            // asked then): one waiting since an earlier loop went in up to a
            // frame old, while a newer one was a few ms away, and was stamped
            // with the loop's time, so the logged ages left that out (about
            // 9 ms; review 2026-11-02). A still picture (or a paused video)
            // goes in whenever it is wanted. A capture's time is that of the
            // read it is current as of.
            const bool freshEnough = newFrame || t0 - lastFrameAt >= kPlayingNs;
            // while hidden for a move only the settle read is worth a map
            if (mw && depthWanted && havePic && freshEnough && !stereoMode && (!moving || settledReadAt) && !busyId && !modelThumb()->empty()
                && mlxr_depth_due(depth, t0)) {
                depthWanted = false;
                // another video (another size): its bars are its own
                if (r.w != barsW || r.h != barsH) { bars.reset(); barsW = r.w; barsH = r.h; }
                std::array<float, 4> at;
                if (subtitles) {
                    auto thumb = modelThumb();
                    const unsigned g = mapGen;
                    helper.post([&, thumb, mw, mh, t1, g] {
                        if (g != mapGen) return;
                        subtitleFinder.update(thumb->data(), mw, mh, t1);
                        std::lock_guard<std::mutex> l(doneM);
                        std::copy(subtitleFinder.box, subtitleFinder.box + 4, subBox);
                    });
                } else if (subtitlesWere != 0) {
                    helper.post([&] { subtitleFinder.reset(); std::lock_guard<std::mutex> l(doneM); std::fill(subBox, subBox + 4, 0.0f); });
                }
                subtitlesWere = subtitles;
                if (modelInput(shrinkPic, r.w, r.h, mw, mh, &bars, true, t1, small, at.data(), modelThumb().get())) {
                    mapRects[++frameId] = at;
                    while (mapRects.size() > 16) mapRects.erase(mapRects.begin());
                    if (mlxr_depth_submit(depth, small.data(), (size_t)mw * 4, mw, mh, 0, frameId, heldAt) >= 0) {
                        busyId = frameId; busyAt = t0; busyEngine = depth;
                        if (freeAt) path[kFree].add(ms(freeAt, t0));
                        path[kOld].add(ms(pictureReadAt, t0));
                        freeAt = 0;
                    }
                } else depthWanted = true;   // (the GPU could not shrink it: the next loop tries again)
            }
            parts[kDepthIn].add(ms(tSubmit, mlxr_depth_now_ns()));
        }
        // the helper's finished map (the newest), and the subtitle box
        float subtitleBox[4];
        {
            std::lock_guard<std::mutex> l(doneM);
            if (doneMap.ready) {
                doneMap.ready = false;
                if (doneMap.gen == mapGen) {
                    depthMap.swap(doneMap.rgba);
                    dw = doneMap.w; dh = doneMap.h; mapRect = doneMap.rect; mapCapturedAt = doneMap.captureNs;
                    path[kHelper].add(ms(doneMap.acquiredNs, doneMap.helperNs)); path[kTaken].add(ms(doneMap.helperNs, mlxr_depth_now_ns()));
                    looks.map(doneMap.look, doneMap.cut);
                    if (moving && settledReadAt && doneMap.captureNs >= settledReadAt) settledMap = true;
                    newDepth = true;
                }
            }
            std::copy(subBox, subBox + 4, subtitleBox);
        }
        const int stereoNow = stereoMode.load();
        const int64_t tPrep = mlxr_depth_now_ns();
        if (depthMap.empty()) { const uint8_t c = (uint8_t)(conv * 2.55f); depthMap.assign({c, c, c, c}); dw = dh = 1; newDepth = true; }
        if (newDepth) depthDirty = true;
        // The move is over once the settled read's depth map is in (a 3D
        // video or a missing depth engine needs none), or kSettleMaxNs after
        // the last move at the latest: the next frame drawn shows again
        if (moving && settledReadAt && (settledMap || stereoNow || !depth || t1 - rectMovedAt >= kSettleMaxNs)) {
            moving = false; showFrom = renders + 1; lastKey.clear(); comfortState.snap();
            if (!g_dumpDir.empty()) LOG("3D back %.0f ms after the last move", (t1 - rectMovedAt) / 1e6);   // test runs only
        }

        int ow, oh;
        eyeSize(r.w, r.h, ow, oh);
        // The pointer ring: shown over the picture while the pointer moves, for 3 s after
        float cursorU = -1, cursorV = -1;
        {
            int px = 0, py = 0;
            if (cap.pointer(px, py)) {
                if (px != lastPointerX || py != lastPointerY) { lastPointerX = px; lastPointerY = py; pointerMovedAt = t1; }
                if (t1 - pointerMovedAt < 3000000000LL && px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h) {
                    cursorU = (px - r.x + 0.5f) / r.w; cursorV = (py - r.y + 0.5f) / r.h;
                }
            }
        }
        // Player controls and captions in picture coordinates (0..1, top first)
        std::vector<float> controls;
        for (const auto& u : ui) {
            const float x0 = (float)(u[0] - r.x) / r.w, y0 = (float)(u[1] - r.y) / r.h;
            const float x1 = (float)(u[0] + u[2] - r.x) / r.w, y1 = (float)(u[1] + u[3] - r.y) / r.h;
            if (x1 <= 0 || y1 <= 0 || x0 >= 1 || y0 >= 1) continue;
            controls.insert(controls.end(), {x0, y0, x1, y1});
        }
        // flat rectangles and their weights (UiEase); the subtitle box
        // first: the shader takes 8 flat rectangles
        std::vector<float> flat, flatW;
        if (const float* sb = subtitleEase.step(subtitleBox, t1)) { flat.insert(flat.end(), sb, sb + 4); flatW.push_back(1); }
        if (stereoNow) flat.insert(flat.end(), controls.begin(), controls.end());
        else uiEase.step(controls, t1, flat, flatW);
        // See-through rectangles in picture coordinates
        std::vector<float> holes;
        for (const auto& u : holeRects) {
            const float x0 = (float)(u[0] - r.x) / r.w, y0 = (float)(u[1] - r.y) / r.h;
            const float x1 = (float)(u[0] + u[2] - r.x) / r.w, y1 = (float)(u[1] + u[3] - r.y) / r.h;
            if (x1 <= 0 || y1 <= 0 || x0 >= 1 || y0 >= 1 || holes.size() >= 32) continue;
            holes.insert(holes.end(), {x0, y0, x1, y1});
        }
        // A 3D video shows each eye its own half, so player controls drawn
        // over it would be split between the eyes: they are left see-through
        // instead, where Chrome's own flat drawing shows
        if (stereoNow) { holes.insert(holes.end(), flat.begin(), flat.end()); flat.clear(); flatW.clear(); }
        joinToEight(flat, &flatW); joinToEight(holes);
        // A gallery's pictures, the only places with depth
        std::vector<float> only;
        for (const auto& u : onlyRects) {
            const float x0 = (float)(u[0] - r.x) / r.w, y0 = (float)(u[1] - r.y) / r.h;
            const float x1 = (float)(u[0] + u[2] - r.x) / r.w, y1 = (float)(u[1] + u[3] - r.y) / r.h;
            if (x1 <= 0 || y1 <= 0 || x0 >= 1 || y0 >= 1) continue;
            only.insert(only.end(), {x0, y0, x1, y1});
        }
        // Draw only when something it shows changed: a new frame or depth
        // map, the pointer, the flat, see-through or depth rectangles or a setting
        // Comfort limits (separation and convergence to draw with), from where
        // the picture was last placed; rounded, so a value that has settled
        // does not ask for a new frame every loop. A convergence step of
        // 0.005 moves the picture by sep x 0.005 of its width: 0.14 px per
        // eye at the default strength on a 1920 px eye, 0.5 px at the largest: rounding the
        // convergence to 0.001 redrew a still gallery about 27 times a second
        // while the edge guard eased after each new map (2026-10-28)
        float sepDraw = strength / 100.0f, convDraw = conv / 100.0f;
        {
            float eye[3];
            vrv.eyePosition(eye);
            float d[3] = {vrv.placedPos[0] - eye[0], vrv.placedPos[1] - eye[1], vrv.placedPos[2] - eye[2]};
            comfortState.step(t1, conv / 100.0f, strength / 100.0f, vrv.placedWidth, vrv.placedWidth > 0 ? VrView::norm3(d) : 0.0f, convDraw, sepDraw);
            sepDraw = std::round(sepDraw * 2e4f) / 2e4f; convDraw = std::round(convDraw * 200.0f) / 200.0f;
        }
        const float zoom = Comfort::zoom(strength / 100.0f, conv / 100.0f);
        drawSep = sepDraw; drawConv = convDraw;
        std::vector<float> key = {sepDraw, convDraw, zoom, sharpen, (float)ow, (float)oh, cursorU, cursorV, (float)stereoNow};
        key.insert(key.end(), flat.begin(), flat.end());
        key.insert(key.end(), flatW.begin(), flatW.end());
        key.push_back(-1);
        key.insert(key.end(), holes.begin(), holes.end());
        key.push_back(-2);
        key.insert(key.end(), only.begin(), only.end());
        // When to draw. While a video plays, a new depth map or a setting
        // waits for the next picture (at most a frame) and goes with it:
        // drawing for each of them as well drew 40-55 times a second for a
        // 30 fps video, and while the headset is worn SteamVR's compositor
        // shares the GPU, so a drawing took 10-25 ms (headset, 2026-10-27)
        // and the loop could not keep up with a 60 fps video. And the GPU
        // is not waited for: while the last drawing is still on it, this
        // loop goes on reading, and the newest picture is drawn once it is
        // free (a picture in between is skipped, not queued).
        const bool playing = t1 - lastFrameAt < kPlayingNs;
        const bool wanted = frameDirty || depthDirty || key != lastKey;
        // A change of the pointer, a rectangle or a setting alone is drawn
        // once a refresh at most: the pointer moving over a still picture
        // drew up to 94 times a second (review 2026-11-02; 190-210 on the
        // headset); what changed meanwhile goes with the next one
        const bool paced = !frameDirty && !depthDirty && refreshHz >= 20 && t0 - lastDrawAt < (int64_t)(1e9 / refreshHz);
        parts[kPrep].add(ms(tPrep, mlxr_depth_now_ns()));
        // Nothing is drawn while hidden for a move (scrolling): the GPU is
        // SteamVR's and Chrome's then; what changed is drawn when it shows
        // Frames reach SteamVR in the order drawn, none drawn over before it is shown
        present();
        if (wanted && !paced && !moving && !(playing && !frameDirty) && waiting.size() < GpuWarp::kBuffers - 1) {
            const bool withFrame = frameDirty;
            lastDrawAt = t0;
            // Another output size makes all of GpuWarp's buffers anew, those
            // of frames still waiting too: they would go out empty
            if (ow * 2 != gpu.width() || oh != gpu.height()) waiting.clear();
            const int64_t tDraw = mlxr_depth_now_ns();
            // a dump of this drawing (test runs): the zero-copy picture is
            // copied before it is drawn, as Chrome may draw the next frame
            // into the same buffer meanwhile
            const bool dumpNow = !g_dumpDir.empty() && tDraw - dumpedAt > (int64_t)(g_dumpEvery * 1e9) && havePic;
            std::vector<uint8_t> srcCopy;
            if (dumpNow && picZeroCopy) gpu.shrink(srcRect.x, srcRect.y, r.w, r.h, r.w, r.h, srcCopy);
            const bool rendered = gpu.render(frameDirty ? src : nullptr, r.w, r.h, stride, depthDirty ? depthMap.data() : nullptr, dw, dh,
                                             sepDraw, convDraw, ow, oh, cursorU, cursorV,
                                             flat.empty() ? nullptr : flat.data(), (int)flat.size() / 4, sharpen,
                                             holes.empty() ? nullptr : holes.data(), (int)holes.size() / 4,
                                             only.empty() ? nullptr : only.data(), (int)only.size() / 4,
                                             mapRect.data(), mapRect.data(), zoom, stereoNow, StereoFinder::eyeFillFor(stereoNow, r.w, r.h),
                                             flatW.empty() ? nullptr : flatW.data());

            const int64_t t2 = mlxr_depth_now_ns();
            if (!rendered) { drawFailed = true; setProblem("gpu-failed"); idle("GPU render failed"); continue; }
            parts[kDraw].add(ms(tDraw, t2)); parts[kUpload].add(gpu.uploadMs());
            waiting.push_back({gpu.current(), withFrame && playing ? pictureSlot(pictureReadAt) : 0, pictureReadAt, withFrame, ++renders, r});
            lastKey = key;
            if (withFrame && mapCapturedAt && !stereoNow) { mapBehindSum += ms(mapCapturedAt, pictureReadAt); ++mapBehindN; behind.push_back((float)ms(mapCapturedAt, pictureReadAt)); }
            if (withFrame && !stereoNow) looks.frame(drawConv, drawSep, strength / 100.0f);
            frameDirty = depthDirty = false; frameDraws += withFrame;
            ++frames; warpSum += (t2 - tDraw) / 1e6;   // the drawing itself
            if (dumpNow) {
                dumpedAt = t2;
                char prefix[64];
                std::snprintf(prefix, sizeof(prefix), "/dump_%04d", dumps++);
                const std::string p = g_dumpDir + prefix;
                std::vector<uint8_t> out;
                float eye[3];
                vrv.eyePosition(eye);
                float d[3] = {vrv.placedPos[0] - eye[0], vrv.placedPos[1] - eye[1], vrv.placedPos[2] - eye[2]};
                auto list = [](const std::vector<float>& v) {
                    std::string s = "[";
                    for (size_t i = 0; i < v.size(); ++i) { char b[24]; std::snprintf(b, sizeof(b), "%s%.6f", i ? "," : "", (double)v[i]); s += b; }
                    return s + "]";
                };
                char info[768];
                std::snprintf(info, sizeof(info),
                    "{\"strength\":%.4f,\"convergence\":%.4f,\"sharpen\":%.4f,\"smooth\":%d,\"ow\":%d,\"oh\":%d,"
                    "\"cursorU\":%.6f,\"cursorV\":%.6f,\"rect\":[%d,%d,%d,%d],\"mapW\":%d,\"mapH\":%d,"
                    "\"widthM\":%.4f,\"distanceM\":%.4f,\"model\":\"%s\",\"grabMs\":%.2f,\"warpMs\":%.2f,\"version\":\"" FD_VERSION "\",",
                    (double)strength, (double)conv, (double)sharpen, smooth, ow, oh, (double)cursorU, (double)cursorV,
                    r.x, r.y, r.w, r.h, dw, dh, (double)vrv.placedWidth, (double)VrView::norm3(d), jsonEscape(owner->modelName()).c_str(),
                    (t1 - t0) / 1e6, (t2 - t1) / 1e6);
                const std::string json = std::string(info) + "\"flat\":" + list(flat) + ",\"flatW\":" + list(flatW) + ",\"holes\":" + list(holes) + ",\"only\":" + list(only)
                    + ",\"mapRect\":" + list(std::vector<float>(mapRect.begin(), mapRect.end()))
                    + ",\"draw\":" + list({drawSep, drawConv, Comfort::zoom(strength / 100.0f, conv / 100.0f)})
                    + ",\"stereo\":" + list({(float)stereoNow, StereoFinder::eyeFillFor(stereoNow, r.w, r.h)}) + "}\n";
                const bool ok = gpu.readback(out) && writePpm(p + "_sbs.ppm", gpu.width(), gpu.height(), out.data(), (size_t)gpu.width() * 4, 0, 1, 2)
                    && (picZeroCopy ? !srcCopy.empty() && writePpm(p + "_src.ppm", r.w, r.h, srcCopy.data(), (size_t)r.w * 4, 0, 1, 2)
                                         : writePpm(p + "_src.ppm", r.w, r.h, src, stride, 2, 1, 0))
                    && writePam(p + "_map.pam", dw, dh, depthMap.data());
                FILE* f = ok ? std::fopen((p + "_info.json").c_str(), "w") : nullptr;
                if (f) { std::fputs(json.c_str(), f); std::fclose(f); }
                LOG("dump %s: %s", p.c_str(), f ? "written" : "failed");
            }
        }
        const int64_t t2 = mlxr_depth_now_ns();
        parts[kLoop].add(ms(t0, t2));
        if (t2 - statsAt > 5000000000LL) {
            const double secs = (t2 - statsAt) / 1e9;
            // fps: frames drawn (= new pictures, depth maps, pointer moves);
            // grab: one read of Chrome's window; gpu: handing a frame to the GPU
            sh->fps = (float)(frames / secs);
            sh->frameFps = (float)(frameDraws / secs);
            sh->grabMs = grabs ? (float)(grabSum / grabs) : 0.0f;
            sh->warpMs = frames ? (float)(warpSum / frames) : 0.0f;
            // maps this view took in the period (the engine's own rate is over
            // its last 32 maps, and kept its last value when maps stopped coming)
            sh->mapsPerSecond = (float)(ageN / secs);
            // A depth model whose runs fail, or none at all, leaves the 3D
            // flat or its depth stuck: said in the popup (stats), not only here
            const char* trouble = "";
            if (depth) {
                MlxrDepthStats s{}; mlxr_depth_stats(depth, &s); sh->modelMs = s.modelMsAvg;
                if (owner->starts.load() != failedEngine) { failedEngine = owner->starts.load(); failedRuns = 0; }
                if (s.state == MLXR_DEPTH_FAILED) trouble = "model-stopped";
                else if (s.failed > failedRuns) trouble = "model-fails";
                failedRuns = s.failed;
            } else if (!stereoNow) trouble = "no-model";
            if (drawFailed) trouble = "gpu-failed";
            drawFailed = false;
            setProblem(trouble);
            if (stereoNow) { sh->mapsPerSecond = 0; sh->modelMs = 0; }
            // Age: how much older the picture a drawn depth map was made from
            // is than the picture drawn with it, what the popup calls the
            // depth trailing the video. The map's own age when taken (made
            // to taken) left out the wait to be drawn, and read about 36 ms
            // where the depth trailed by 65-77 (512, 30 fps, 2026-10-31); it is
            // used only when no picture was drawn with a map.
            sh->depthAgeMs = mapBehindN ? (float)(mapBehindSum / mapBehindN) : ageN ? (float)(ageSum / ageN) : 0.0f;
            // (SteamVR's cumulative frame stats read 0 for an overlay
            // application, so the headset's own drops are not logged here)
            LOG("view %dx%d -> %dx%d per eye: %.1f fps drawn (%.1f with a new picture, video %.0f fps), %.1f shown, %.1f reads/s (%s), grab %.1f ms, gpu %.1f ms, "
                "depth %.1f/s %.0f ms, age %.0f ms, %d flat while moving",
                r.w, r.h, ow, oh, (double)sh->fps, (double)sh->frameFps, (double)sh->videoFps, shown / secs, grabs / secs, reader.damageWorks ? "on damage" : "every loop",
                (double)sh->grabMs, (double)sh->warpMs, (double)sh->mapsPerSecond, (double)sh->modelMs, (double)sh->depthAgeMs, movePauses);
            if (frameDraws) {
                // ms, mean/worst per part (per loop, or per time it ran);
                // gaps between new pictures shown: median, 95th, worst, and
                // how many were over 1.5 video frames
                std::string line;
                for (int i = 0; i < kParts; ++i) {
                    char b[64];
                    std::snprintf(b, sizeof(b), "%s%s %.1f/%.1f", i ? ", " : "", kPartNames[i], parts[i].n ? parts[i].sum / parts[i].n : 0.0, parts[i].worst);
                    line += b;
                }
                const double frameMs = sh->videoFps > 1 ? 1000.0 / sh->videoFps : 0;
                auto spread = [&](std::vector<float>& v) {
                    std::sort(v.begin(), v.end());
                    auto at = [&](double q) { return v.empty() ? 0.0 : (double)v[std::min(v.size() - 1, (size_t)(q * v.size()))]; };
                    int late = 0;
                    for (float gp : v) late += frameMs > 0 && gp > 1.5 * frameMs;
                    char b[96];
                    std::snprintf(b, sizeof(b), "%.1f/%.1f/%.1f, %d over 1.5 frames", at(0.5), at(0.95), v.empty() ? 0.0 : (double)v.back(), late);
                    return std::string(b);
                };
                std::string counts;
                for (int i = 0; i < 7; ++i) counts += (i ? " " : "") + std::to_string(refreshes[i]);
                double gpuMs = 0; int gpuN = 0;
                gpu.takeGpuTime(gpuMs, gpuN);
                LOG("timing ms (mean/worst): %s; %d new pictures, gaps read %s, shown %s; refreshes per picture 0..6+: %s (%.0f Hz, %d anchors, %d past their refresh); read to shown %.1f ms; depth map from a picture %.0f ms older; GPU %.2f ms a drawing",
                    line.c_str(), newPictures, spread(readGaps).c_str(), spread(gaps).c_str(), counts.c_str(), (double)refreshHz, anchors, missed,
                    latencyN ? latencySum / latencyN : 0.0, mapBehindN ? mapBehindSum / mapBehindN : 0.0, gpuN ? gpuMs / gpuN : 0.0);
                std::string way;
                std::lock_guard<std::mutex> pl(pathM);
                for (int i = 0; i < kPathParts; ++i) {
                    char b[64];
                    std::snprintf(b, sizeof(b), "%s%s %.1f/%.1f", i ? ", " : "", kPathNames[i], path[i].n ? path[i].sum / path[i].n : 0.0, path[i].worst);
                    way += b;
                }
                std::sort(behind.begin(), behind.end());
                LOG("depth map path ms (mean/worst): %s; picture older than its map: median %.0f, 90th %.0f",
                    way.c_str(), behind.empty() ? 0.0 : (double)behind[behind.size() / 2], behind.empty() ? 0.0 : (double)behind[behind.size() * 9 / 10]);
                if (looks.maps || looks.frames)
                    LOG("depth look: %d maps, middle %.2f-%.2f (largest step %.2f), spread %.2f-%.2f (largest step %.2f), %d cuts; "
                        "%d pictures drawn, pop-out plane %.3f-%.3f, separation %.4f-%.4f (held by the angle limit in %d)",
                        looks.maps, (double)looks.midLo, (double)looks.midHi, (double)looks.midStep, (double)looks.spreadLo, (double)looks.spreadHi, (double)looks.spreadStep, looks.cuts,
                        looks.frames, (double)looks.convLo, (double)looks.convHi, (double)looks.sepLo, (double)looks.sepHi, looks.capped);
            }
            { std::lock_guard<std::mutex> pl(pathM); for (Part& p : path) p = Part{}; }
            behind.clear(); looks.clear();
            for (Part& p : parts) p = Part{};
            gaps.clear(); readGaps.clear(); std::fill(std::begin(refreshes), std::end(refreshes), 0);
            latencySum = 0; latencyN = 0; newPictures = 0; anchors = 0; missed = 0; mapBehindSum = 0; mapBehindN = 0;
            frames = 0; shown = 0; grabs = 0; grabSum = warpSum = 0; statsAt = t2; ageSum = 0; ageN = 0; movePauses = 0; frameDraws = 0;
        }
        // Loop every 4 ms at most while something is on its way that the
        // loop looks for (the depth engine's run, frames to hand over, a
        // map wanted, a move), once a refresh otherwise: a still picture
        // looped 250 times a second, about 5% of a core and
        // 1,500 SteamVR and 1,000 X calls a second (review 2026-11-02). A new
        // picture from the reader wakes it at once, and so do a frame's slot
        // coming first and a map the helper finished. A drawing held back
        // to once a refresh (paced) is due a refresh after the last one:
        // left to the next loop it came 12-15 ms after, the pointer moving
        // at 64 drawings a second on a 90 Hz display (headset, 2026-11-02).
        const int64_t now = mlxr_depth_now_ns();
        const bool polling = busyId || !waiting.empty() || depthWanted || moving;
        int64_t sleepNs = (polling || refreshHz < 20 ? 4000000 : (int64_t)(1e9 / refreshHz)) - (now - t0);
        if (!waiting.empty() && waiting.front().at) sleepNs = std::min(sleepNs, waiting.front().at - now);
        if (wanted && paced) sleepNs = std::min(sleepNs, lastDrawAt + (int64_t)(1e9 / refreshHz) - now);
        if (sleepNs > 0) reader.waitNew(frameSeq, sleepNs);
    }
    stopView();
    vrv.shutdown();
    if (gpuUp) gpu.shutdown();
}

// --selftest SECONDS [OVERLAY_KEY]: a GPU test pattern (left eye red, right
// eye blue, white bar at the top) over the middle of a visible gamescope
// window, submitted as an OpenGL texture every frame, then the safe exit
// sequence. Checks the texture path without Chrome or the extension.
// --calib KEY X Y W H SECONDS: the test pattern over window rectangle
// (X, Y, W, H), first half of the time with texel aspect 1.0 (full SBS: each
// eye's half is a whole picture), second half with 2.0 (half SBS).
static int calibTest(const std::string& key, Rect r, int seconds) {
    GpuWarp gpu;
    std::string err = gpu.init();
    if (!err.empty()) { LOG("calib: GPU: %s", err.c_str()); return 1; }
    VrView vrv;
    err = vrv.init();
    if (!err.empty()) { LOG("calib: %s", err.c_str()); gpu.shutdown(); return 1; }
    vr::VROverlayHandle_t parent = vr::k_ulOverlayHandleInvalid;
    if (vrv.o->FindOverlay(key.c_str(), &parent) != vr::VROverlayError_None) { vrv.shutdown(); gpu.shutdown(); return 1; }
    r.visible = true;
    for (int i = 0; i < seconds * 60 && !g_stop; ++i) {
        const float aspect = i < seconds * 30 ? 1.0f : 2.0f;
        if (i == 0 || i == seconds * 30) LOG("calib: texel aspect %.1f", aspect);
        vrv.o->SetOverlayTexelAspect(vrv.view, aspect);
        if (!gpu.pattern(r.w, r.h) || vrv.submit(gpu.texture()) != vr::VROverlayError_None) { LOG("calib: submit failed"); break; }
        if (vrv.anchor(parent, r)) vrv.show();
        usleep(16 * 1000);
    }
    vrv.shutdown();
    gpu.shutdown();
    LOG("calib: done");
    return 0;
}

static int selfTest(int seconds, std::string key) {
    if (key.empty()) {
        FILE* p = popen(kListOverlays, "r");
        std::string out; char buf[4096]; size_t n;
        while (p && (n = std::fread(buf, 1, sizeof(buf), p)) > 0) out.append(buf, n);
        if (p) pclose(p);
        std::smatch m;
        std::regex line("^'(gamescope\\.[^']*\\.window\\.[0-9]+)' -- [^\\n]* visible ", std::regex::multiline);
        if (std::regex_search(out, m, line)) key = m[1].str();
    }
    if (key.empty()) { LOG("selftest: no visible gamescope window"); return 1; }
    GpuWarp gpu;
    std::string err = gpu.init();
    if (!err.empty()) { LOG("selftest: GPU: %s", err.c_str()); return 1; }
    VrView vrv;
    err = vrv.init();
    if (!err.empty()) { LOG("selftest: %s", err.c_str()); gpu.shutdown(); return 1; }
    vr::VROverlayHandle_t parent = vr::k_ulOverlayHandleInvalid;
    if (vrv.o->FindOverlay(key.c_str(), &parent) != vr::VROverlayError_None) { LOG("selftest: no overlay %s", key.c_str()); vrv.shutdown(); gpu.shutdown(); return 1; }
    vr::HmdVector2_t scale{};
    vrv.o->GetOverlayMouseScale(parent, &scale);
    Rect r; r.w = (int)(scale.v[0] / 2); r.h = (int)(scale.v[1] / 2); r.x = r.w / 2; r.y = r.h / 2; r.visible = true;
    LOG("selftest: %s %.0fx%.0f, pattern %dx%d per eye", key.c_str(), scale.v[0], scale.v[1], r.w, r.h);
    int failures = 0;
    for (int i = 0; i < seconds * 60 && !g_stop; ++i) {
        if (!gpu.pattern(r.w, r.h)) { LOG("selftest: GL error"); ++failures; break; }
        const vr::EVROverlayError se = vrv.submit(gpu.texture());
        if (se != vr::VROverlayError_None) { LOG("selftest: SetOverlayTexture %s", vrv.o->GetOverlayErrorNameFromEnum(se)); ++failures; break; }
        if (vrv.anchor(parent, r)) vrv.show();
        if (i == 0) LOG("selftest: first texture submitted");
        usleep(16 * 1000);
    }
    vrv.shutdown();
    gpu.shutdown();
    LOG("selftest: %s", failures ? "FAILED" : "done, overlay removed");
    return failures ? 1 : 0;
}

static std::string jsonString(const std::string& body, const char* name) {
    std::smatch m;
    std::regex re(std::string("\"") + name + "\"\\s*:\\s*\"([^\"]*)\"");
    return std::regex_search(body, m, re) ? m[1].str() : std::string();
}
static bool jsonTrue(const std::string& body, const char* name) {
    return std::regex_search(body, std::regex(std::string("\"") + name + "\"\\s*:\\s*true"));
}
static double jsonNumber(const std::string& body, const char* name, double fallback) {
    std::smatch m;
    std::regex re(std::string("\"") + name + "\"\\s*:\\s*(-?[0-9.]+)");
    return std::regex_search(body, m, re) ? std::atof(m[1].str().c_str()) : fallback;
}
// A window pixel coordinate from a message: the numbers come from the page
// side (an iframe's parent page can feed the extension any offset), so they
// are held to a sane range before they become ints
static int pixelValue(double v) { return (int)std::clamp(std::isfinite(v) ? v : 0.0, -65536.0, 65536.0); }
static std::vector<float> jsonFloats(const std::string& body, const char* name) {
    std::vector<float> v;
    std::smatch m;
    if (!std::regex_search(body, m, std::regex(std::string("\"") + name + "\"\\s*:\\s*\\[([^\\]]*)\\]"))) return v;
    const std::string s = m[1].str();
    const char* p = s.c_str();
    for (char* e; ; p = e) {
        while (*p == ',' || *p == ' ') ++p;
        const float f = std::strtof(p, &e);
        if (e == p) break;
        v.push_back(f);
    }
    return v;
}

// --rerender PREFIX OUT.ppm: draws a --dump again offline from its inputs
// (PREFIX_src.ppm, _map.pam, _info.json) and compares it with what the live
// view handed SteamVR (PREFIX_sbs.ppm): the offline tools draw what the
// headset shows only if the two match.
static int rerenderTest(const std::string& prefix, const std::string& outPath) {
    int sw, sh, dw, dh, lw, lh;
    std::vector<uint8_t> srcRgba, map, live;
    if (!readPnm(prefix + "_src.ppm", sw, sh, srcRgba) || !readPnm(prefix + "_map.pam", dw, dh, map)) { LOG("rerender: cannot read %s_*", prefix.c_str()); return 1; }
    std::string info;
    if (FILE* f = std::fopen((prefix + "_info.json").c_str(), "r")) { char b[4096]; size_t n; while ((n = std::fread(b, 1, sizeof(b), f)) > 0) info.append(b, n); std::fclose(f); }
    if (info.empty()) { LOG("rerender: no %s_info.json", prefix.c_str()); return 1; }
    for (size_t i = 0; i < srcRgba.size(); i += 4) std::swap(srcRgba[i], srcRgba[i + 2]);   // to BGRA, as captured
    const std::vector<float> flat = jsonFloats(info, "flat"), holes = jsonFloats(info, "holes"), only = jsonFloats(info, "only");
    const std::vector<float> flatW = jsonFloats(info, "flatW");   // (older dumps: none, all 1)
    const std::vector<float> mapRect = jsonFloats(info, "mapRect");
    const float* area = mapRect.size() == 4 ? mapRect.data() : nullptr;
    // separation, convergence and zoom as drawn (comfort limits), when recorded
    const std::vector<float> draw = jsonFloats(info, "draw");
    const bool drew = draw.size() == 3;
    const std::vector<float> stereo = jsonFloats(info, "stereo");
    GpuWarp gpu;
    const std::string err = gpu.init();
    if (!err.empty()) { LOG("rerender: %s", err.c_str()); return 1; }
    const int ow = (int)jsonNumber(info, "ow", 0), oh = (int)jsonNumber(info, "oh", 0);
    std::vector<uint8_t> out;
    const bool ok = gpu.render(srcRgba.data(), sw, sh, sw * 4, map.data(), dw, dh,
                               drew ? draw[0] : (float)jsonNumber(info, "strength", 0) / 100.0f,
                               drew ? draw[1] : (float)jsonNumber(info, "convergence", 50) / 100.0f, ow, oh,
                               (float)jsonNumber(info, "cursorU", -1), (float)jsonNumber(info, "cursorV", -1),
                               flat.empty() ? nullptr : flat.data(), (int)flat.size() / 4, (float)jsonNumber(info, "sharpen", 0),
                               holes.empty() ? nullptr : holes.data(), (int)holes.size() / 4,
                               only.empty() ? nullptr : only.data(), (int)only.size() / 4, area, area, drew ? draw[2] : 0.0f,
                               stereo.size() == 2 ? (int)stereo[0] : 0, stereo.size() == 2 ? stereo[1] : 1.0f,
                               flatW.size() * 4 == flat.size() && !flatW.empty() ? flatW.data() : nullptr)
                    && gpu.readback(out) && writePpm(outPath, gpu.width(), gpu.height(), out.data(), (size_t)gpu.width() * 4, 0, 1, 2);
    gpu.shutdown();
    if (!ok) { LOG("rerender: drawing failed"); return 1; }
    if (!readPnm(prefix + "_sbs.ppm", lw, lh, live) || lw * lh * 4 != (int)out.size()) { LOG("rerender: %s_sbs.ppm missing or another size", prefix.c_str()); return 1; }
    int maxDiff = 0; double sum = 0; size_t over = 0;
    for (size_t i = 0; i < out.size(); ++i) {
        if (i % 4 == 3) continue;
        const int d = std::abs((int)out[i] - (int)live[i]);
        maxDiff = std::max(maxDiff, d); sum += d; over += d > 2;
    }
    LOG("rerender %s: %dx%d, differs from the live frame by %.3f on average, at most %d, %zu values over 2",
        prefix.c_str(), lw, lh, sum / (out.size() / 4 * 3), maxDiff, over);
    return over ? 2 : 0;
}

// --pipeline MODEL WxH OUT_DIR [key=value ...]: the live path offline, on raw
// BGRA frames from stdin (ffmpeg -f rawvideo -pix_fmt bgra -), in simulated
// time so a run is repeatable: frames at fps; a capture for the depth model
// when the rate allows and the model is free, as mlxr_depth_due; its map
// (the engine's own normalisation and scene cut detector, then postDepth)
// used from latency ms later; every frame drawn by the GPU warp. Writes
//   shown_NNNNN.pgm   the depth each frame was drawn with (model size)
//   oracle_NNNNN.pgm  with oracle=1: the depth of that very frame, no delay
//                     and no per texel average (what the shown depth chases)
//   sbs_NNNNN.ppm     every Nth frame drawn (every=N, 0 none)
//   frames.csv, maps.csv, summary.json   per frame, per map, the run
// Keys: fps 30, rate 20, latency 35, strength 1.5, popout 50, smooth 3,
// sharpen 0, every 0, oracle 0, tau 30 (engine per texel average, ms), cut 1,
// frames 0 (all), letterbox 1 (find black bars, as live), lbcrop 1 (give the
// depth model only the picture inside them; 0 keeps the whole frame and only
// flattens the bars, for comparison). The depth files are on the whole
// picture's grid either way (bars at the screen plane's depth), and
// frames.csv says where the picture inside the bars was found. The other
// defaults are the live ones too, so a run measures what the headset shows:
// comfort 1 (the angle cap) for a picture widthm 0.98 m wide seen from distm 1.48 m (a window in front of the user,
// as measured on the headset); stable 1 (Steady instead of the engine's
// average: stau 500 ms, slo 0.005, shi 0.03), rangetau -1 (the engine's depth
// range average in ms; -1: kSteadyRangeTau with Steady, the engine's default
// without), rnorm 1 (the depth range as live, RangeNorm; 0: the engine's),
// rwiden and rnarrow (RangeNorm's time constants in ms; -1: the range
// average's), stereo3d 1 (3D video shown as it is, the model resting
// meanwhile), dilate kEdgeDilate (near objects widened by this many map
// texels), subtitles 1 (burned-in subtitles kept flat), oraclerange 150 (the
// reference's range average, ms). frames.csv also has the separation and
// convergence each frame was drawn with, the 3D video format and the
// subtitle box (sub_x0..sub_y1).
static int pipelineTest(int argc, char** argv) {
    const char* modelPath = argv[2];
    int W = 0, H = 0;
    if (std::sscanf(argv[3], "%dx%d", &W, &H) != 2 || W < 16 || H < 16) { LOG("pipeline: size must be WxH"); return 1; }
    const std::string dir = argv[4];
    std::map<std::string, double> opt = {{"fps", 30}, {"rate", 20}, {"latency", 35}, {"strength", 1.5}, {"popout", 50},
                                         {"smooth", 3}, {"sharpen", 0}, {"every", 0}, {"oracle", 0}, {"tau", 30}, {"cut", 1}, {"frames", 0},
                                         {"letterbox", 1}, {"lbcrop", 1}, {"comfort", 1}, {"widthm", 0.98}, {"distm", 1.48},
                                         {"stable", 1}, {"stau", 500}, {"slo", 0.005}, {"shi", 0.03}, {"rangetau", -1}, {"rnorm", 1}, {"rwiden", -1}, {"rnarrow", -1}, {"stereo3d", 1}, {"dilate", kEdgeDilate}, {"subtitles", 1}, {"oraclerange", 150}};
    for (int i = 5; i < argc; ++i) {
        const char* eq = std::strchr(argv[i], '=');
        const std::string key = eq ? std::string(argv[i], eq - argv[i]) : std::string(argv[i]);
        if (!eq || !opt.count(key)) { LOG("pipeline: unknown setting %s", argv[i]); return 1; }
        opt[key] = std::atof(eq + 1);
    }
    // The live engine's cores (cpu2-7) and threads
    cpu_set_t cpus; CPU_ZERO(&cpus);
    for (int c = 2; c < 8; ++c) CPU_SET(c, &cpus);
    sched_setaffinity(0, sizeof(cpus), &cpus);
    MlxrDepthModelOptions mo{}; mo.threads = 3; mo.flags = MLXR_DEPTH_FP16;
    char err[256] = "";
    MlxrDepthModel* model = mlxr_depth_model_load(modelPath, &mo, err, sizeof(err));
    if (!model) { LOG("pipeline: %s: %s", modelPath, err); return 1; }
    const int mw = mlxr_depth_model_width(model), mh = mlxr_depth_model_height(model);
    GpuWarp gpu;
    std::string gerr = gpu.init();
    if (!gerr.empty()) { LOG("pipeline: %s", gerr.c_str()); mlxr_depth_model_free(model); return 1; }

    MlxrDepthParams params; mlxr_depth_params_default(&params);
    const bool stable = opt["stable"] != 0;
    params.tauMs = stable ? 0 : (int)opt["tau"]; params.cutLevel = (int)opt["cut"];
    params.rangeTauMs = opt["rangetau"] >= 0 ? (int)opt["rangetau"] : stable ? kSteadyRangeTau : DEPTH_RANGE_TAU_DEFAULT_MS;
    StereoFinder stereoFinder;
    SubtitleFinder subtitleFinder;
    SubtitleEase subtitleEase;
    std::vector<uint8_t> thumb;
    const bool stereo3d = opt["stereo3d"] != 0, subtitles = opt["subtitles"] != 0;
    Steady steady;
    steady.tauMs = (float)opt["stau"]; steady.lo = (float)opt["slo"]; steady.hi = (float)opt["shi"];
    // the depth range: ours (RangeNorm, as live) or the engine's (rnorm=0)
    RangeNorm rangeNorm;
    const bool ownRange = opt["rnorm"] != 0;
    rangeNorm.widenMs = opt["rwiden"] >= 0 ? (float)opt["rwiden"] : (float)params.rangeTauMs;
    rangeNorm.narrowMs = opt["rnarrow"] >= 0 ? (float)opt["rnarrow"] : (float)params.rangeTauMs;
    // the reference stays the same whatever is being tuned: no per texel
    // average, the range average oraclerange ms (the engine's default; 0 for
    // the depth range of that frame alone, the fair target when the range
    // average itself is being compared)
    MlxrDepthParams oracleParams = params; oracleParams.tauMs = 0; oracleParams.rangeTauMs = (int)opt["oraclerange"];
    MlxrDepthNorm* norm = mlxr_depth_norm_create(mw, mh);
    MlxrDepthNorm* oracleNorm = mlxr_depth_norm_create(mw, mh);
    DepthCut cut, oracleCut;
    depthCutClear(&cut); depthCutClear(&oracleCut);
    PostSettings post; post.smooth = (int)opt["smooth"]; post.dilate = (int)opt["dilate"];
    const float strength = (float)opt["strength"], conv = 100.0f - (float)opt["popout"], sharpen = (float)opt["sharpen"];
    const double fps = opt["fps"], latency = opt["latency"], gap = 1000.0 / std::max(1.0, opt["rate"]);
    const int every = (int)opt["every"], maxFrames = (int)opt["frames"];
    const bool oracle = opt["oracle"] != 0, letterbox = opt["letterbox"] != 0, lbcrop = opt["lbcrop"] != 0;
    BarFinder bars, oracleBars;
    Comfort comfortState;
    const bool comfort = opt["comfort"] != 0;
    const float widthM = (float)opt["widthm"], distM = (float)opt["distm"];
    int ow, oh;
    eyeSize(W, H, ow, oh);

    // One map: the model on this picture, normalised as the engine does at
    // time nowMs; resets from this detector
    std::vector<uint8_t> small;
    std::vector<float> rgb((size_t)mw * mh * 3);
    auto makeMap = [&](const uint8_t* frame, MlxrDepthNorm* n, const MlxrDepthParams& p, DepthCut& c, BarFinder& bf, double nowMs,
                       std::vector<uint8_t>& map, float rect[4], float& modelMs, float& normMs, int& resets, RangeNorm* own) {
        const Shrink shrink = [&](int x, int y, int w, int h, std::vector<uint8_t>& o) { toModelSize(frame + ((size_t)y * W + x) * 4, W * 4, w, h, mw, mh, o); return true; };
        modelInput(shrink, W, H, mw, mh, letterbox ? &bf : nullptr, lbcrop, (int64_t)(nowMs * 1e6), small, rect);
        mlxr_depth_rgba8_to_input(small.data(), (size_t)mw * 4, mw, mh, 0, rgb.data());
        resets = 0;
        if (p.cutLevel) {
            DepthThumb thumb; float diff, hist;
            depthThumbMake(&thumb, rgb.data(), mw, mh);
            resets = depthCutResets(depthCutStep(&c, &thumb, &diff, &hist));
        }
        std::memcpy(mlxr_depth_model_input(model), rgb.data(), rgb.size() * sizeof(float));
        if (mlxr_depth_model_run(model, &modelMs)) return false;
        const int64_t a = mlxr_depth_now_ns();
        map.resize((size_t)mw * mh * 4);
        mlxr_depth_norm_run(n, &p, mlxr_depth_model_output(model), rgb.data(), resets, (int64_t)(nowMs * 1e6), map.data(), nullptr);
        if (own) own->apply(mlxr_depth_model_output(model), mw * mh, resets, (int64_t)(nowMs * 1e6), map.data());
        normMs = (mlxr_depth_now_ns() - a) / 1e6f;
        return true;
    };

    FILE* framesCsv = std::fopen((dir + "/frames.csv").c_str(), "w");
    FILE* mapsCsv = std::fopen((dir + "/maps.csv").c_str(), "w");
    if (!framesCsv || !mapsCsv) { LOG("pipeline: cannot write in %s", dir.c_str()); return 1; }
    std::fprintf(framesCsv, "frame,t_ms,map_seq,map_frame,map_age_ms,warp_ms,area_x0,area_y0,area_x1,area_y1,sep,conv,stereo,sub_x0,sub_y0,sub_x1,sub_y1,subd_x0,subd_y0,subd_x1,subd_y1\n");
    std::fprintf(mapsCsv, "seq,frame,model_ms,norm_ms,post_ms,resets\n");

    std::vector<uint8_t> frame((size_t)W * H * 4), shownMap((size_t)mw * mh * 4, (uint8_t)(conv * 2.55f)), pendingMap, oracleMap, out;
    for (size_t i = 0; i < shownMap.size(); i += 4) shownMap[i] = shownMap[i + 1] = shownMap[i + 2] = 0;
    bool havePending = false, newDepth = true;
    float pendingRect[4] = {0, 0, 1, 1}, shownRect[4] = {0, 0, 1, 1}, shownArea[4] = {0, 0, 1, 1}, pendingArea[4] = {0, 0, 1, 1}, oracleRect[4];
    // A map on the whole picture's grid: the part inside area from the map
    // (which lies at rect), the rest at the screen plane's depth (for the depth files)
    std::vector<uint8_t> whole((size_t)mw * mh * 4);
    auto onWholeGrid = [&](const std::vector<uint8_t>& map, const float rect[4], const float area[4]) -> const uint8_t* {
        const uint8_t flatDepth = (uint8_t)(conv * 2.55f);
        for (int y = 0; y < mh; ++y)
            for (int x = 0; x < mw; ++x) {
                const float u = (x + 0.5f) / mw, v = (y + 0.5f) / mh;
                uint8_t d = flatDepth;
                if (u >= area[0] && u <= area[2] && v >= area[1] && v <= area[3]) {
                    const int mx = std::clamp((int)((u - rect[0]) / (rect[2] - rect[0]) * mw), 0, mw - 1);
                    const int my = std::clamp((int)((v - rect[1]) / (rect[3] - rect[1]) * mh), 0, mh - 1);
                    d = map[((size_t)my * mw + mx) * 4 + 3];
                }
                whole[((size_t)y * mw + x) * 4 + 3] = d;
            }
        return whole.data();
    };
    double pendingAt = 0, nextDue = 0, modelSum = 0, postSum = 0, warpSum = 0, warpMax = 0;
    int pendingFrame = -1, mapFrame = -1, maps = 0, frames = 0;
    double mapMadeAt = 0;
    while ((maxFrames <= 0 || frames < maxFrames) && std::fread(frame.data(), 1, frame.size(), stdin) == frame.size()) {
        const int i = frames++;
        const double t = i * 1000.0 / fps;
        if (havePending && t >= pendingAt) {
            shownMap.swap(pendingMap); havePending = false; newDepth = true;
            std::copy(pendingRect, pendingRect + 4, shownRect); std::copy(pendingArea, pendingArea + 4, shownArea);
            mapFrame = pendingFrame; mapMadeAt = pendingAt - latency; ++maps;
        }
        if (!havePending && t >= nextDue && !stereoFinder.mode) {   // a 3D video rests the model, as live
            float modelMs = 0, normMs = 0; int resets = 0;
            if (subtitles) { toModelSize(frame.data(), W * 4, W, H, mw, mh, thumb); subtitleFinder.update(thumb.data(), mw, mh, (int64_t)(t * 1e6)); }
            if (!makeMap(frame.data(), norm, params, cut, bars, t + latency, pendingMap, pendingRect, modelMs, normMs, resets, ownRange ? &rangeNorm : nullptr)) { LOG("pipeline: model run failed"); return 1; }
            // the bars as found: flat even when the model got the whole frame (lbcrop=0)
            std::copy(bars.area, bars.area + 4, pendingArea);
            if (!letterbox) { pendingArea[0] = pendingArea[1] = 0; pendingArea[2] = pendingArea[3] = 1; }
            const int64_t a = mlxr_depth_now_ns();
            if (stable) steady.apply(pendingMap, mw, mh, (int64_t)(t * 1e6), (resets & DEPTH_RESET_RANGE) != 0);
            postDepth(pendingMap, mw, mh, post);
            const double postMs = (mlxr_depth_now_ns() - a) / 1e6;
            havePending = true; pendingAt = t + latency; pendingFrame = i; nextDue = t + gap;
            modelSum += modelMs; postSum += postMs;
            std::fprintf(mapsCsv, "%d,%d,%.2f,%.2f,%.2f,%d\n", maps + 1, i, (double)modelMs, (double)normMs, postMs, resets);
        }
        if (stereo3d) { toModelSize(frame.data(), W * 4, W, H, mw, mh, thumb); stereoFinder.update(thumb.data(), mw, mh, (int64_t)(t * 1e6)); }
        float sepDraw = strength / 100.0f, convDraw = conv / 100.0f;
        if (comfort) {
            comfortState.step((int64_t)(t * 1e6), conv / 100.0f, strength / 100.0f, widthM, distM, convDraw, sepDraw);
            sepDraw = std::round(sepDraw * 2e4f) / 2e4f; convDraw = std::round(convDraw * 200.0f) / 200.0f;
        }
        const float* subtitleDrawn = subtitleEase.step(subtitleFinder.box, (int64_t)(t * 1e6));
        const int64_t a = mlxr_depth_now_ns();
        if (!gpu.render(frame.data(), W, H, W * 4, newDepth ? shownMap.data() : nullptr, mw, mh, sepDraw, convDraw,
                        ow, oh, -1, -1, subtitleDrawn, subtitleDrawn ? 1 : 0, sharpen,
                        nullptr, 0, nullptr, 0, shownRect, shownArea,
                        Comfort::zoom(strength / 100.0f, conv / 100.0f), stereoFinder.mode, stereoFinder.eyeFill(W, H))) {
            LOG("pipeline: GPU render failed"); return 1;
        }
        const double warpMs = (mlxr_depth_now_ns() - a) / 1e6;
        newDepth = false;
        warpSum += warpMs; warpMax = std::max(warpMax, warpMs);
        std::fprintf(framesCsv, "%d,%.2f,%d,%d,%.1f,%.2f,%.4f,%.4f,%.4f,%.4f,%.5f,%.4f,%d,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f\n", i, t, maps, mapFrame, mapFrame < 0 ? -1.0 : t - mapMadeAt, warpMs,
                     (double)shownArea[0], (double)shownArea[1], (double)shownArea[2], (double)shownArea[3], (double)sepDraw, (double)convDraw,
                     stereoFinder.mode, (double)subtitleFinder.box[0], (double)subtitleFinder.box[1], (double)subtitleFinder.box[2],
                     (double)subtitleFinder.box[3], subtitleDrawn ? (double)subtitleDrawn[0] : 0.0, subtitleDrawn ? (double)subtitleDrawn[1] : 0.0,
                     subtitleDrawn ? (double)subtitleDrawn[2] : 0.0, subtitleDrawn ? (double)subtitleDrawn[3] : 0.0);
        char name[64];
        std::snprintf(name, sizeof(name), "/shown_%05d.pgm", i);
        writePgm(dir + name, mw, mh, onWholeGrid(shownMap, shownRect, shownArea), 3);
        if (oracle) {
            float modelMs, normMs; int resets;
            if (!makeMap(frame.data(), oracleNorm, oracleParams, oracleCut, oracleBars, t, oracleMap, oracleRect, modelMs, normMs, resets, nullptr)) { LOG("pipeline: model run failed"); return 1; }
            postDepth(oracleMap, mw, mh, post);
            float oracleArea[4] = {0, 0, 1, 1};
            if (letterbox) std::copy(oracleBars.area, oracleBars.area + 4, oracleArea);
            std::snprintf(name, sizeof(name), "/oracle_%05d.pgm", i);
            writePgm(dir + name, mw, mh, onWholeGrid(oracleMap, oracleRect, oracleArea), 3);
        }
        if (every > 0 && i % every == 0) {
            std::snprintf(name, sizeof(name), "/sbs_%05d.ppm", i);
            if (!gpu.readback(out) || !writePpm(dir + name, gpu.width(), gpu.height(), out.data(), (size_t)gpu.width() * 4, 0, 1, 2)) { LOG("pipeline: cannot write %s", name); return 1; }
        }
    }
    std::fclose(framesCsv); std::fclose(mapsCsv);
    FILE* f = std::fopen((dir + "/summary.json").c_str(), "w");
    if (f) {
        std::fprintf(f, "{\"version\":\"" FD_VERSION "\",\"model\":\"%s\",\"input\":[%d,%d],\"map\":[%d,%d],\"eye\":[%d,%d],\"frames\":%d,\"maps\":%d,"
                        "\"modelMs\":%.2f,\"postMs\":%.3f,\"warpMs\":%.2f,\"warpMaxMs\":%.2f,\"settings\":{",
                     jsonEscape(modelPath).c_str(), W, H, mw, mh, ow, oh, frames, maps, maps ? modelSum / maps : 0.0,
                     maps ? postSum / maps : 0.0, frames ? warpSum / frames : 0.0, warpMax);
        bool first = true;
        for (const auto& kv : opt) { std::fprintf(f, "%s\"%s\":%g", first ? "" : ",", kv.first.c_str(), kv.second); first = false; }
        std::fprintf(f, "}}\n");
        std::fclose(f);
    }
    LOG("pipeline: %d frames, %d maps, model %.1f ms, post %.2f ms, warp %.2f ms (max %.2f)", frames, maps,
        maps ? modelSum / maps : 0.0, maps ? postSum / maps : 0.0, frames ? warpSum / frames : 0.0, warpMax);
    mlxr_depth_norm_free(norm); mlxr_depth_norm_free(oracleNorm);
    gpu.shutdown();
    mlxr_depth_model_free(model);
    return frames ? 0 : 1;
}

int main(int argc, char** argv) {
    std::string extensionId = "bmhflpdnkcbkfooeloiabjnpinajmnfl";
    std::string assets;
    int port = 47321;
    signal(SIGINT, onSignal); signal(SIGTERM, onSignal); signal(SIGPIPE, SIG_IGN);
    // A nice below 0 for the view loop (placeThread): the session lets a
    // process raise its own soft limit to the hard one (28: nice -8)
    if (rlimit rl{}; getrlimit(RLIMIT_NICE, &rl) == 0 && rl.rlim_cur < rl.rlim_max) { rl.rlim_cur = rl.rlim_max; setrlimit(RLIMIT_NICE, &rl); }
    if (argc >= 2 && !std::strcmp(argv[1], "--version")) { std::printf("framedepth-companion %s\n", FD_VERSION); return 0; }
    if (argc >= 3 && !std::strcmp(argv[1], "--selftest")) return selfTest(std::atoi(argv[2]), argc > 3 ? argv[3] : "");
    if (argc >= 8 && !std::strcmp(argv[1], "--calib")) {
        Rect r; r.x = std::atoi(argv[3]); r.y = std::atoi(argv[4]); r.w = std::atoi(argv[5]); r.h = std::atoi(argv[6]);
        return calibTest(argv[2], r, std::atoi(argv[7]));
    }
    if (argc >= 3 && !std::strcmp(argv[1], "--gputest")) {
        // Offline: warp a synthetic picture (checkerboard, disc in the middle
        // marked near) and write the side-by-side result as a PPM. No OpenVR.
        // A third argument "moved" gives the map other colours than the
        // picture, as after fast motion (the motion guard's case); "narrow"
        // first draws a 16 px wide, 6000 px tall sliver (it once asked a
        // texture past the GPU's limit, and its error failed the next drawing).
        GpuWarp gpu;
        std::string err = gpu.init();
        if (!err.empty()) { LOG("gputest: %s", err.c_str()); return 1; }
        const int w = 640, h = 360, dw = 64, dh = 36;
        std::vector<uint8_t> src((size_t)w * h * 4);
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
            uint8_t* p = &src[((size_t)y * w + x) * 4];
            const bool disc = (x - 320) * (x - 320) + (y - 180) * (y - 180) < 80 * 80;
            const bool check = ((x / 40) + (y / 40)) % 2;
            p[0] = disc ? 255 : check ? 60 : 30; p[1] = disc ? 255 : check ? 170 : 140; p[2] = disc ? 255 : check ? 240 : 60; p[3] = 255;  // BGRA
        }
        std::vector<uint8_t> depth((size_t)dw * dh * 4);
        for (int y = 0; y < dh; ++y) for (int x = 0; x < dw; ++x) {
            const uint8_t* s = &src[((size_t)(y * h / dh) * w + x * w / dw) * 4];
            uint8_t* q = &depth[((size_t)y * dw + x) * 4];
            q[0] = s[2]; q[1] = s[1]; q[2] = s[0];
            if (argc > 3 && !std::strcmp(argv[3], "moved")) { q[0] = 255 - q[0]; q[1] = 255 - q[1]; q[2] = 255 - q[2]; }   // the picture changed since the map
            q[3] = (x - 32) * (x - 32) + (y - 18) * (y - 18) < 8 * 8 ? 255 : 51;
        }
        std::vector<uint8_t> out;
        const float flat[4] = {0.0f, 0.85f, 1.0f, 1.0f};   // a bottom bar kept flat
        const float hole[4] = {0.70f, 0.05f, 0.95f, 0.30f};  // a popup's place, see-through
        if (argc > 3 && !std::strcmp(argv[3], "narrow")) {
            const bool sliver = gpu.render(src.data(), w, h, w * 4, depth.data(), dw, dh, 0.03f, 0.5f, 16, 6000, -1, -1, flat, 1, 0.5f, hole, 1);
            LOG("gputest: 16x6000 sliver %s", sliver ? "drawn" : "failed");
            if (!sliver) { gpu.shutdown(); return 1; }
        }
        const auto t0 = mlxr_depth_now_ns();
        bool ok = gpu.render(src.data(), w, h, w * 4, depth.data(), dw, dh, 0.03f, 0.5f, w, h, -1, -1, flat, 1, 0.5f, hole, 1);
        const auto t1 = mlxr_depth_now_ns();
        ok = ok && gpu.readback(out);
        FILE* f = std::fopen(argv[2], "wb");
        if (!f) { LOG("gputest: cannot write %s", argv[2]); gpu.shutdown(); return 1; }
        std::fprintf(f, "P6\n%d %d\n255\n", gpu.width(), gpu.height());
        for (size_t i = 0; i < out.size(); i += 4) std::fwrite(&out[i], 1, 3, f);
        std::fclose(f);
        size_t clear = 0;
        for (size_t i = 3; i < out.size(); i += 4) clear += out[i] == 0;
        LOG("gputest: %s, render %.2f ms, %.1f%% see-through (6.25%% expected: a quarter by a quarter of each eye), wrote %s", ok ? "ok" : "GL error",
            (t1 - t0) / 1e6, out.empty() ? 0.0 : 100.0 * clear / (out.size() / 4), argv[2]);
        gpu.shutdown();
        return ok ? 0 : 1;
    }
    if (argc >= 5 && !std::strcmp(argv[1], "--depthcheck")) {
        // --depthcheck MODEL_A MODEL_B PICTURE.ppm: both models (either
        // backend) on the same picture; their agreement (Pearson correlation
        // of the outputs, same size) and run times. For checking a converted
        // model, and the GPU's fp16, against the one in use.
        FILE* f = std::fopen(argv[4], "rb");
        int pw = 0, ph = 0, maxv = 0;
        if (!f || std::fscanf(f, "P6 %d %d %d", &pw, &ph, &maxv) != 3) { LOG("depthcheck: cannot read %s", argv[4]); return 1; }
        std::fgetc(f);
        std::vector<uint8_t> rgb((size_t)pw * ph * 3);
        if (std::fread(rgb.data(), 1, rgb.size(), f) != rgb.size()) { LOG("depthcheck: short PPM"); return 1; }
        std::fclose(f);
        std::vector<float> outs[2];
        int ow = 0, oh = 0;
        for (int k = 0; k < 2; ++k) {
            MlxrDepthModelOptions opt{}; opt.threads = 3; opt.flags = MLXR_DEPTH_FP16;
            char err[256] = "";
            MlxrDepthModel* m = mlxr_depth_model_load(argv[2 + k], &opt, err, sizeof(err));
            if (!m) { LOG("depthcheck: %s: %s", argv[2 + k], err); return 1; }
            const int w = mlxr_depth_model_width(m), h = mlxr_depth_model_height(m);
            if (k && (w != ow || h != oh)) { LOG("depthcheck: sizes differ"); return 1; }
            ow = w; oh = h;
            float* in = mlxr_depth_model_input(m);
            for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x)
                for (int c = 0; c < 3; ++c) in[((size_t)y * w + x) * 3 + c] = rgb[((size_t)(y * ph / h) * pw + x * pw / w) * 3 + c] / 255.0f;
            float ms = 0, sum = 0;
            for (int i = 0; i < 12; ++i) { if (mlxr_depth_model_run(m, &ms)) { LOG("depthcheck: run failed"); return 1; } if (i >= 2) sum += ms; }
            const float* o = mlxr_depth_model_output(m);
            outs[k].assign(o, o + (size_t)w * h);
            LOG("depthcheck %s: %dx%d, %.1f ms a run (10 after 2 warm-up), threads %d",
                argv[2 + k], w, h, sum / 10, mlxr_depth_model_threads(m));
            mlxr_depth_model_free(m);
        }
        double ma = 0, mb = 0;
        const size_t n = outs[0].size();
        for (size_t i = 0; i < n; ++i) { ma += outs[0][i]; mb += outs[1][i]; }
        ma /= n; mb /= n;
        double ab = 0, aa = 0, bb = 0;
        for (size_t i = 0; i < n; ++i) { const double a = outs[0][i] - ma, b = outs[1][i] - mb; ab += a * b; aa += a * a; bb += b * b; }
        LOG("depthcheck: correlation %.4f (1 = same shape of depth, negative = near and far swapped)", ab / std::sqrt(aa * bb));
        return 0;
    }
    if (argc >= 4 && !std::strcmp(argv[1], "--snapshot")) {
        // --snapshot TITLE_PART OUT.ppm: Chrome's window as the X server has
        // it (what we read), for telling Chrome's own drawing from what
        // SteamVR and our overlay add on top
        Capture cap;
        const std::string err = cap.open(argv[2]);
        if (!err.empty()) { LOG("snapshot: %s", err.c_str()); return 1; }
        Rect r; r.w = cap.winW; r.h = cap.winH;
        int stride = 0;
        const uint8_t* px = cap.grab(r, stride);
        FILE* f = px ? std::fopen(argv[3], "wb") : nullptr;
        if (!f) { LOG("snapshot: capture or file failed"); cap.close(); return 1; }
        std::fprintf(f, "P6\n%d %d\n255\n", r.w, r.h);
        for (int y = 0; y < r.h; ++y)
            for (int x = 0; x < r.w; ++x) { const uint8_t* p = px + (size_t)y * stride + x * 4; const uint8_t rgb[3] = {p[2], p[1], p[0]}; std::fwrite(rgb, 1, 3, f); }
        std::fclose(f);
        LOG("snapshot: %dx%d to %s", r.w, r.h, argv[3]);
        cap.close();
        return 0;
    }
    if (argc >= 6 && !std::strcmp(argv[1], "--depthbench")) {
        // --depthbench MODEL SECONDS THREADS CPUMASK(hex, 0 = any): the depth
        // model as fast as it goes on a changing noise picture; prints the
        // engine's own timings. For choosing threads and cores.
        MlxrDepthConfig cfg{};
        cfg.modelPath = argv[2]; cfg.threads = std::atoi(argv[4]); cfg.flags = MLXR_DEPTH_FP16; cfg.perSecond = 0;
        cfg.cpuMask = std::strtoull(argv[5], nullptr, 16);
        mlxr_depth_params_default(&cfg.params);
        MlxrDepthEngine* e = mlxr_depth_start(&cfg);
        char err[256] = "";
        if (!e || mlxr_depth_wait_ready(e, 30000, err, sizeof(err)) != MLXR_DEPTH_READY) {
            LOG("depthbench: %s", err);
            if (e) mlxr_depth_stop(e);
            return 1;
        }

        int mw = 0, mh = 0;
        mlxr_depth_size(e, &mw, &mh);
        std::vector<uint8_t> img((size_t)mw * mh * 4);
        uint64_t seq = 0, frame = 0, ageN = 0; double ageSum = 0;
        uint32_t rnd = 1;
        const int64_t end = mlxr_depth_now_ns() + (int64_t)std::atoi(argv[3]) * 1000000000LL;
        while (mlxr_depth_now_ns() < end && !g_stop) {
            const int64_t now = mlxr_depth_now_ns();
            if (mlxr_depth_due(e, now)) {
                for (auto& b : img) { rnd = rnd * 1664525u + 1013904223u; b = (uint8_t)(rnd >> 24); }
                mlxr_depth_submit(e, img.data(), (size_t)mw * 4, mw, mh, 0, ++frame, now);
            }
            if (const MlxrDepthResult* res = mlxr_depth_acquire(e, seq)) {
                seq = res->seq; ageSum += (mlxr_depth_now_ns() - res->captureNs) / 1e6; ++ageN;
                mlxr_depth_release(e, res);
            }
            usleep(2000);
        }
        MlxrDepthStats s{};
        mlxr_depth_stats(e, &s);
        LOG("depthbench %dx%d threads %d mask 0x%llx: %.1f maps/s, model %.1f ms (p95 %.1f), capture to map %.0f ms",
            mw, mh, cfg.threads, (unsigned long long)cfg.cpuMask, (double)s.mapsPerSecond, (double)s.modelMsAvg,
            (double)s.modelMsP95, ageN ? ageSum / ageN : 0.0);
        mlxr_depth_stop(e);
        return 0;
    }
    if (argc >= 2 && !std::strcmp(argv[1], "--worn")) {
        // For test scripts: exit 0 and "worn" while someone wears the headset
        // (SteamVR's user interaction level), 1 and "idle" otherwise, 2 without SteamVR
        vr::EVRInitError ie = vr::VRInitError_None;
        vr::IVRSystem* sys = vr::VR_Init(&ie, vr::VRApplication_Background);
        if (!sys) { std::printf("no SteamVR\n"); return 2; }
        const vr::EDeviceActivityLevel a = sys->GetTrackedDeviceActivityLevel(vr::k_unTrackedDeviceIndex_Hmd);
        vr::VR_Shutdown();
        const bool worn = a == vr::k_EDeviceActivityLevel_UserInteraction || a == vr::k_EDeviceActivityLevel_UserInteraction_Timeout;
        std::printf("%s (activity level %d)\n", worn ? "worn" : "idle", (int)a);
        return worn ? 0 : 1;
    }
    if (argc >= 5 && !std::strcmp(argv[1], "--pipeline")) return pipelineTest(argc, argv);
    if (argc >= 4 && !std::strcmp(argv[1], "--rerender")) return rerenderTest(argv[2], argv[3]);
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--extension-id")) extensionId = argv[i + 1];
        else if (!std::strcmp(argv[i], "--assets")) assets = argv[i + 1];
        else if (!std::strcmp(argv[i], "--port")) port = std::atoi(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--dump") && i + 2 < argc) { g_dumpDir = argv[i + 1]; g_dumpEvery = std::atof(argv[i + 2]); ++i; }
    }
    if (assets.empty()) {
        char exe[512]; ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        if (n > 0) { exe[n] = 0; assets = std::string(exe, std::strrchr(exe, '/') - exe) + "/assets"; }
    }
    const std::string origin = "chrome-extension://" + extensionId;

    int ls = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    int one = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(ls, (sockaddr*)&addr, sizeof(addr)) || listen(ls, 4)) { LOG("cannot listen on 127.0.0.1:%d: %s", port, strerror(errno)); return 1; }
    LOG("%s listening on 127.0.0.1:%d for %s", FD_VERSION, port, origin.c_str());

    DepthOwner depth;
    depth.assets = assets;

    Shared shared;
    std::thread view(viewThread, &shared, &depth);

    Client client;
    int64_t idleSince = mlxr_depth_now_ns(), refusedAt = INT64_MIN / 2, heardAt = 0, bigAt = INT64_MIN / 2;
    std::thread loader;                  // starts the depth model for a hello
    std::atomic<bool> loading{false};
    // Also forgets what the client said about the page (a reconnecting
    // extension sends it again)
    auto stopView = [&] {
        std::lock_guard<std::mutex> l(shared.m);
        shared.want = false; shared.ui.clear(); shared.holes.clear(); shared.only.clear(); shared.videoFps = 0;
    };

    // The connected extension has closed: its socket at end of file, or a
    // WebSocket close frame next (it came during the new one's handshake)
    auto clientGone = [&] {
        unsigned char c = 0;
        const ssize_t n = ::recv(client.fd, &c, 1, MSG_PEEK | MSG_DONTWAIT);
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) return true;
        return n == 1 && client.in.empty() && (c & 0x0f) == 8;
    };
    while (!g_stop) {
        pollfd fds[2] = {{ls, POLLIN, 0}, {client.fd, POLLIN, 0}};
        int n = poll(fds, client.open() ? 2 : 1, 10);
        if (n < 0 && errno != EINTR) break;
        bool closed = false;
        if (client.open() && (fds[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            char buf[1 << 16];
            ssize_t r = ::recv(client.fd, buf, sizeof(buf), MSG_DONTWAIT);
            if (r > 0) client.in.append(buf, (size_t)r);
            else if (r == 0 || (errno != EAGAIN && errno != EINTR)) closed = true;
        }
        int opcode; std::string msg;
        while (client.open() && !closed && nextMessage(client, opcode, msg, closed)) {
            heardAt = mlxr_depth_now_ns();
            if (opcode != 1) continue;
            // The extension's messages are under 2 KB. The fields are read
            // with std::regex, which recurses once a character: a long one
            // from another local program could run the stack out (review
            // 2026-11-01), so anything over 8 KB is dropped unread.
            if (msg.size() > 8192) {
                if (mlxr_depth_now_ns() - bigAt > 60LL * 1000000000LL) { bigAt = mlxr_depth_now_ns(); LOG("ignored a %zu byte message", msg.size()); }
                continue;
            }
            const std::string type = jsonString(msg, "type");
            if (type == "hello") {
                // The model loads on a thread of its own (a second or two,
                // 20 s at worst per model): here it held up every message,
                // a second profile's turning away and the popup's place too
                // (review 2026-10-31). The answer goes out when it is ready.
                if (!loading) {
                    // the extension's depth detail comes with the hello: its
                    // settings come only after ready, and Normal's model was
                    // loaded first and then the setting's (review 2026-10-31)
                    int detail;
                    {
                        std::lock_guard<std::mutex> l(shared.m);
                        detail = shared.detail = std::clamp((int)jsonNumber(msg, "detail", shared.detail), 0, 2);
                    }
                    if (loader.joinable()) loader.join();
                    loading = true;
                    loader = std::thread([&, detail] {
                        const std::string err = depth.ensure(detail);
                        shared.reply(err.empty() ? "{\"type\":\"ready\",\"version\":\"" FD_VERSION "\",\"model\":\"" + jsonEscape(depth.modelName()) + "\"" + depth.refusedField() + "}"
                                                 : "{\"type\":\"error\",\"version\":\"" FD_VERSION "\",\"error\":\"" + jsonEscape(err) + "\"}");
                        loading = false;
                    });
                }
            } else if (type == "view") {
                std::lock_guard<std::mutex> l(shared.m);
                shared.want = jsonTrue(msg, "on");
                if (shared.want) { shared.marker = jsonString(msg, "marker"); shared.generation++; }
            } else if (type == "rect") {
                std::lock_guard<std::mutex> l(shared.m);
                shared.rect.x = pixelValue(jsonNumber(msg, "x", 0)); shared.rect.y = pixelValue(jsonNumber(msg, "y", 0));
                shared.rect.w = pixelValue(jsonNumber(msg, "w", 0)); shared.rect.h = pixelValue(jsonNumber(msg, "h", 0));
                shared.rect.visible = jsonTrue(msg, "visible");
            } else if (type == "settings") {
                std::lock_guard<std::mutex> l(shared.m);
                // Clamped here too: a large strength widens the shader's
                // search (reach) without bound
                shared.strength = std::clamp((float)jsonNumber(msg, "strength", shared.strength), 0.0f, 5.0f);
                shared.convergence = std::clamp((float)jsonNumber(msg, "convergence", shared.convergence), 0.0f, 100.0f);
                shared.detail = std::clamp((int)jsonNumber(msg, "detail", shared.detail), 0, 2);
                shared.smooth = std::clamp((int)jsonNumber(msg, "smooth", shared.smooth), 0, 8);
                shared.sharpen = std::clamp((float)jsonNumber(msg, "sharpen", shared.sharpen), 0.0f, 3.0f);
                shared.subtitles = jsonNumber(msg, "subtitles", shared.subtitles) != 0;
                shared.scrollflat = jsonNumber(msg, "scrollflat", shared.scrollflat) != 0;
            } else if (type == "ui" || type == "holes" || type == "only") {
                // "rects":[[x,y,w,h],...]: player UI or the FrameDepth popup,
                // window pixels
                std::vector<std::array<int, 4>> rects;
                const size_t at = msg.find("\"rects\"");
                if (at != std::string::npos) {
                    std::regex num("-?[0-9]+");
                    std::vector<int> v;
                    for (std::sregex_iterator it(msg.begin() + at, msg.end(), num), end; it != end; ++it) v.push_back(pixelValue(std::strtod(it->str().c_str(), nullptr)));
                    const size_t most = type == "only" ? 16 : 8;
                    for (size_t i = 0; i + 3 < v.size() && rects.size() < most; i += 4)
                        if (v[i + 2] > 0 && v[i + 3] > 0) rects.push_back({v[i], v[i + 1], v[i + 2], v[i + 3]});   // the shaders' ramps need a size
                }
                std::lock_guard<std::mutex> l(shared.m);
                (type == "ui" ? shared.ui : type == "holes" ? shared.holes : shared.only) = rects;
            } else if (type == "moving") {
                shared.movingAt = mlxr_depth_now_ns();
            } else if (type == "videofps") {
                shared.videoFps = std::clamp((float)jsonNumber(msg, "fps", 0), 0.0f, 240.0f);
            } else if (type == "stats") {
                char b[320];
                std::snprintf(b, sizeof(b), "{\"type\":\"stats\",\"fps\":%.1f,\"frameFps\":%.1f,\"videoFps\":%.0f,\"grabMs\":%.1f,\"warpMs\":%.1f,\"mapsPerSecond\":%.1f,\"modelMs\":%.0f,\"ageMs\":%.0f,\"stereo\":%d}",
                              (double)shared.fps, (double)shared.frameFps, (double)shared.videoFps, (double)shared.grabMs, (double)shared.warpMs,
                              (double)shared.mapsPerSecond, (double)shared.modelMs, (double)shared.depthAgeMs, shared.stereoMode.load());
                std::string problem;
                { std::lock_guard<std::mutex> l(shared.m); problem = shared.problem; }
                std::string reply(b);
                reply.insert(reply.size() - 1, ",\"problem\":\"" + jsonEscape(problem) + "\"");
                if (!client.sendText(reply)) closed = true;
            }
        }
        {
            std::vector<std::string> outbox;
            { std::lock_guard<std::mutex> l(shared.m); outbox.swap(shared.outbox); }
            for (auto& s : outbox) if (client.open() && !closed && !client.sendText(s)) closed = true;
        }
        if (closed) { stopView(); client.close(); idleSince = mlxr_depth_now_ns(); LOG("extension disconnected"); }
        // After the client's own reading: an extension that reconnects (its
        // service worker restarted, or it was reloaded) closes the old socket
        // as the new one comes, and the old one must be seen closed first or
        // the profile is turned away as a second one (review 2026-10-31)
        if (fds[0].revents & POLLIN) {
            // non-blocking: sendAll's 1 s limit (poll on EAGAIN) applies only
            // then; a blocking send to an extension that stopped reading held
            // the whole loop (review 2026-11-01)
            int fd = accept4(ls, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
            if (fd >= 0) {
                bool viewing;
                { std::lock_guard<std::mutex> l(shared.m); viewing = shared.want; }
                if (!handshake(fd, origin)) ::close(fd);
                else if (client.open() && (viewing || mlxr_depth_now_ns() - heardAt < 30LL * 1000000000LL) && !clientGone()) {
                    // Another one while the extension is connected: a second
                    // Chrome profile with FrameDepth. Taking it in place of the
                    // first made the two put each other out every 2 s (each comes
                    // back that soon; review 2026-10-31): the first keeps the
                    // companion, the other is told why and closed. A client
                    // with no view that has said nothing for 30 s is replaced
                    // (it may be gone without its socket closing).
                    Client other; other.fd = fd;
                    other.sendText("{\"type\":\"error\",\"busy\":true,\"version\":\"" FD_VERSION "\",\"error\":\"another Chrome profile is using FrameDepth\"}");
                    other.close();
                    if (mlxr_depth_now_ns() - refusedAt > 60LL * 1000000000LL) { refusedAt = mlxr_depth_now_ns(); LOG("refused a second client: one is connected"); }
                } else {
                    if (client.open()) { stopView(); client.close(); LOG("replaced the previous client (closed, or silent for 30 s)"); }
                    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                    client.fd = fd; heardAt = mlxr_depth_now_ns(); LOG("extension connected");
                }
            }
        }
        // the view thread may still be starting a view the gone client asked
        // for (a minute at worst before the time limits) and then use the
        // engine: it is stopped only once that thread is done with it
        if (!client.open() && !loading && !shared.viewBusy && depth.engine.load() && mlxr_depth_now_ns() - idleSince > 60LL * 1000000000LL) depth.stop();
    }
    stopView();
    if (loader.joinable()) loader.join();
    view.join();
    client.close();
    depth.stop();

    close(ls);
    LOG("stopped");
    return 0;
}
