# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 FrameDepth contributors
# Synthetic test scenes with known depth, for judging FrameDepth's depth
# post-processing without wearing the headset. A small ray caster draws a
# textured ground in perspective, sky, fog and objects with shadows (the cues
# a single-image depth model reads), adds a little sensor noise per frame, and
# records the true depth and which object each pixel shows. Deterministic
# (fixed seeds): the same files every run.
#   python -I make_scenes.py OUT_DIR [SCENE ...]
# For each scene: NAME.webm (VP9 at a streaming-like quality: the pipeline
# input, and what Chrome plays in the live tests), NAME.npz (true depth in metres and object ids at the
# depth model's 512x288, float16/uint8; -1 depth outside the picture) and
# NAME.json (what the scene holds: cuts, picture area, stereo format,
# subtitle boxes). Needs numpy, Pillow and ffmpeg.
import json, math, os, subprocess, sys
import numpy as np
from PIL import Image, ImageDraw, ImageFont

W, H, FPS = 960, 540, 30
MW, MH = 512, 288          # the depth model's input: ground truth is kept at this size
EYE = 1.6                  # camera height, metres
FOV = math.radians(60)
SKY = 1000.0               # depth given to the sky


def value_noise(size, seed, octaves=((8, 1.0), (32, 0.5), (128, 0.25))):
    """Tileable smooth noise in 0..1, size x size."""
    r = np.random.default_rng(seed)
    out = np.zeros((size, size))
    total = 0.0
    for cells, amp in octaves:
        g = r.random((cells, cells))
        x = np.arange(size) * cells / size
        i0 = np.floor(x).astype(int); f = x - i0; i1 = (i0 + 1) % cells
        f = f * f * (3 - 2 * f)
        rows = g[i0][:, i0] * (1 - f)[None, :] + g[i0][:, i1] * f[None, :]
        rows1 = g[i1][:, i0] * (1 - f)[None, :] + g[i1][:, i1] * f[None, :]
        out += amp * (rows * (1 - f)[:, None] + rows1 * f[:, None])
        total += amp
    return out / total


TEX = {}
def tex(name, seed):
    if name not in TEX:
        TEX[name] = value_noise(256, seed)
    return TEX[name]


def sample(t, u, v):
    """Nearest sample of a 256x256 tileable texture at world units u, v (1 unit = 256 texels / 4)."""
    iu = (np.floor(u * 64).astype(np.int64)) % 256
    iv = (np.floor(v * 64).astype(np.int64)) % 256
    return t[iv, iu]


class Obj:
    def __init__(self, x, z, w, h, color, shape='box', seed=1, oid=1):
        self.x, self.z, self.w, self.h = x, z, w, h
        self.color = np.array(color, float)
        self.shape, self.seed, self.oid = shape, seed, oid


_last = [None, None]
def render(w, h, cam_x, cam_z, objects, palette='day', wall=None):
    """One view: rgb float (h, w, 3) 0..1, depth (h, w) metres, object id (h, w).
    The last result is reused while nothing in the view changed."""
    key = (w, h, cam_x, cam_z, palette, wall, tuple((o.x, o.z, o.w, o.h, o.shape, o.seed, tuple(o.color)) for o in objects))
    if _last[0] == key:
        return _last[1]
    _last[0], _last[1] = key, _render(w, h, cam_x, cam_z, objects, palette, wall)
    return _last[1]


def _render(w, h, cam_x, cam_z, objects, palette, wall):
    f = (w / 2) / math.tan(FOV / 2)
    u = (np.arange(w) + 0.5 - w / 2) / f
    v = -(np.arange(h) + 0.5 - h / 2) / f
    pitch = math.radians(-6)
    cp, sp = math.cos(pitch), math.sin(pitch)
    dx = np.broadcast_to(u[None, :], (h, w))
    dy_c = np.broadcast_to(v[:, None], (h, w))
    dy = dy_c * cp + sp            # camera space (x, y, 1) pitched about x
    dz = -dy_c * sp + cp
    depth = np.full((h, w), SKY)
    oid = np.zeros((h, w), np.uint8)
    rgb = np.zeros((h, w, 3))
    # sky
    sky_top = np.array([0.25, 0.45, 0.85]) if palette == 'day' else np.array([0.05, 0.07, 0.15])
    sky_hor = np.array([0.75, 0.85, 0.95]) if palette == 'day' else np.array([0.15, 0.15, 0.22])
    s = np.clip(dy / np.maximum(dz, 1e-3) * 2.5, 0, 1)[..., None]
    rgb[:] = sky_hor * (1 - s) + sky_top * s
    clouds = sample(tex('cloud', 7), dx * 3 + cam_x * 0.01, dy * 6)
    rgb += (np.clip(clouds - 0.55, 0, 1) * 1.5 * (dy > 0))[..., None] * 0.6
    # ground: y = 0
    t = np.where(dy < -1e-4, -EYE / np.minimum(dy, -1e-4), np.inf)
    hit = t < 400
    gx = cam_x + dx * t; gz = cam_z + dz * t
    grass = sample(tex('grass', 3), gx * 0.7, gz * 0.7)
    fine = sample(tex('fine', 4), gx * 4, gz * 4)
    base = np.array([0.30, 0.45, 0.18]) * (0.6 + 0.6 * grass[..., None]) + 0.12 * (fine[..., None] - 0.5)
    path = (np.abs(gx - 0.0) < 1.2)[..., None]
    base = np.where(path, np.array([0.55, 0.48, 0.38]) * (0.7 + 0.5 * fine[..., None]), base)
    tiles = ((np.floor(np.where(hit, gz, 0) / 2) % 2) == 0)[..., None] & path
    base = np.where(tiles, base * 0.85, base)
    # shadows
    for o in objects:
        sh = ((gx - o.x - 0.3) / (o.w * 0.6)) ** 2 + ((gz - o.z - 0.4) / (max(o.w, 0.6) * 0.5)) ** 2 < 1
        base = np.where(sh[..., None], base * 0.55, base)
    rgb = np.where(hit[..., None], base, rgb)
    depth = np.where(hit, t, depth)
    # a wall facing the camera (a repeating pattern: the stereo detector's hard case)
    if wall is not None:
        wz, period = wall
        tw = (wz - cam_z) / dz
        wx = cam_x + dx * tw; wy = EYE + dy * tw
        on = (tw > 0) & (wy > 0) & (wy < 6) & (tw < depth)
        brick = ((np.floor(wx / period) + np.floor(wy / (period * 0.5))) % 2)
        wc = np.array([0.6, 0.3, 0.25]) * (0.75 + 0.25 * brick[..., None]) + 0.1 * (sample(tex('fine', 4), wx * 3, wy * 3)[..., None] - 0.5)
        rgb = np.where(on[..., None], wc, rgb); depth = np.where(on, tw, depth); oid = np.where(on, 250, oid)
    # objects: billboards facing the camera, far to near
    for o in sorted(objects, key=lambda o: -o.z):
        to = (o.z - cam_z) / dz
        ox = cam_x + dx * to - o.x
        oy = EYE + dy * to
        if o.shape == 'ball':
            r = o.w / 2
            q = (ox / r) ** 2 + ((oy - r) / r) ** 2
            inside = q < 1
            shade = np.sqrt(np.clip(1 - q, 0, 1))[..., None] * 0.7 + 0.3
            light = np.clip(1 - ((ox + r * 0.4) ** 2 + (oy - r * 1.4) ** 2) / (r * r * 0.25), 0, 1)[..., None] * 0.35
        elif o.shape == 'tree':
            trunk = (np.abs(ox) < o.w * 0.08) & (oy > 0) & (oy < o.h * 0.45)
            crown = (ox / (o.w / 2)) ** 2 + ((oy - o.h * 0.65) / (o.h * 0.35)) ** 2 < 1
            inside = trunk | crown
            shade = np.where(crown[..., None], 0.8, 0.5) * np.ones((h, w, 1))
            light = 0
        else:
            inside = (np.abs(ox) < o.w / 2) & (oy > 0) & (oy < o.h)
            shade = (0.55 + 0.45 * np.clip(oy / o.h, 0, 1))[..., None]
            edge = (np.abs(np.abs(ox) - o.w / 2) < 0.04) | (np.abs(oy - o.h) < 0.04)
            shade = np.where(edge[..., None], shade * 0.6, shade)
            light = 0
        inside &= (to > 0.05) & (to < depth)
        n = sample(tex('obj%d' % o.seed, 20 + o.seed), ox * 2 + 10 * o.seed, oy * 2)
        col = o.color * shade * (0.75 + 0.5 * n[..., None]) + light
        if o.shape == 'tree':
            col = np.where(((np.abs(ox) < o.w * 0.08) & (oy < o.h * 0.45))[..., None], np.array([0.35, 0.22, 0.1]) * (0.7 + 0.5 * n[..., None]), col)
        rgb = np.where(inside[..., None], col, rgb)
        depth = np.where(inside, to, depth)
        oid = np.where(inside, o.oid, oid)
    # fog: far things fade to the horizon colour
    fog = 1 - np.exp(-np.minimum(depth, 300) / 90.0)
    rgb = rgb * (1 - fog[..., None] * 0.7) + sky_hor * fog[..., None] * 0.7
    if palette == 'night':
        rgb *= 0.18
    return np.clip(rgb, 0, 1), depth, oid


def park(seed=0, n=9, zmin=4, zmax=40, spread=10):
    r = np.random.default_rng(seed)
    objs = []
    colors = [(0.8, 0.2, 0.15), (0.2, 0.4, 0.8), (0.9, 0.75, 0.2), (0.6, 0.3, 0.7), (0.9, 0.9, 0.85), (0.3, 0.7, 0.6)]
    for i in range(n):
        shape = ['box', 'ball', 'tree'][i % 3]
        z = zmin + (zmax - zmin) * r.random()
        x = (r.random() - 0.5) * spread * (z / 10 + 0.5)
        if shape == 'tree':
            w, hh = 2.5, 4.5
        elif shape == 'ball':
            w = hh = 0.6 + r.random() * 0.8
        else:
            w, hh = 0.8 + r.random() * 1.5, 0.8 + r.random() * 1.8
        objs.append(Obj(x, z, w, hh, colors[i % len(colors)], shape, seed=i + 1, oid=i + 1))
    return objs


def to_model(a, nearest=True):
    h, w = a.shape[:2]
    ys = (np.arange(MH) * h // MH); xs = (np.arange(MW) * w // MW)
    return a[ys][:, xs]


class Writer:
    def __init__(self, out, name, w, h):
        self.out, self.name, self.w, self.h = out, name, w, h
        self.p = subprocess.Popen(['ffmpeg', '-loglevel', 'error', '-y', '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-s', f'{w}x{h}',
                                   '-r', str(FPS), '-i', '-', '-c:v', 'libvpx-vp9', '-crf', '30', '-b:v', '0', '-deadline', 'good',
                                   '-cpu-used', '4', '-row-mt', '1', '-pix_fmt', 'yuv420p', os.path.join(out, name + '.webm')],
                                  stdin=subprocess.PIPE)
        self.depth, self.oid, self.n = [], [], 0
        self.rng = np.random.default_rng(1234)

    def frame(self, rgb, depth, oid, noise=1.5):
        img = rgb * 255 + self.rng.normal(0, noise, rgb.shape)
        self.p.stdin.write(np.clip(img, 0, 255).astype(np.uint8).tobytes())
        self.depth.append(to_model(depth).astype(np.float16)); self.oid.append(to_model(oid).astype(np.uint8))
        self.n += 1

    def close(self, meta):
        self.p.stdin.close(); self.p.wait()
        np.savez_compressed(os.path.join(self.out, self.name + '.npz'), depth=np.stack(self.depth), oid=np.stack(self.oid))
        meta.update({'name': self.name, 'size': [self.w, self.h], 'fps': FPS, 'frames': self.n})
        with open(os.path.join(self.out, self.name + '.json'), 'w') as f:
            json.dump(meta, f, indent=1)


def letterboxed(rgb, depth, oid, cw, ch):
    """A cw x ch picture centred in W x H, black around (depth -1 there)."""
    x0, y0 = (W - cw) // 2, (H - ch) // 2
    R = np.zeros((H, W, 3)); D = np.full((H, W), -1.0); O = np.zeros((H, W), np.uint8)
    R[y0:y0 + ch, x0:x0 + cw] = rgb; D[y0:y0 + ch, x0:x0 + cw] = depth; O[y0:y0 + ch, x0:x0 + cw] = oid
    return R, D, O, [x0, y0, cw, ch]


def subtitle(rgb, text, font, mid=None):
    """The text near the bottom, or centred on row mid (inside a bar)."""
    img = Image.fromarray((np.clip(rgb, 0, 1) * 255).astype(np.uint8))
    d = ImageDraw.Draw(img)
    box = d.textbbox((0, 0), text, font=font, stroke_width=3)
    tw, th = box[2] - box[0], box[3] - box[1]
    x, y = (W - tw) // 2, H - th - 40 if mid is None else mid - th // 2
    d.text((x - box[0], y - box[1]), text, font=font, fill=(255, 255, 255), stroke_width=3, stroke_fill=(0, 0, 0))
    return np.asarray(img).astype(float) / 255, [x, y, tw, th]


def scene(out, name):
    T = {'static': 150, 'motion': 150, 'pan': 150, 'cuts': 180, 'letterbox': 180, 'pillarbox': 120, 'edge': 150,
         'sbs_full': 90, 'sbs_half': 90, 'tb_half': 90, 'subs': 180, 'lbsubs': 240, 'dark': 120, 'pattern': 120}[name]
    meta = {'kind': name, 'format': '2d', 'content': [0, 0, W, H], 'cuts': [], 'subs': []}
    base = park(1)
    if name in ('sbs_full', 'sbs_half', 'tb_half'):
        size = {'sbs_full': (2 * W, H), 'sbs_half': (W, H), 'tb_half': (W, H)}[name]
        wr = Writer(out, name, *size)
        meta['format'] = name
        meta['content'] = [0, 0, size[0], size[1]]
        for i in range(T):
            objs = base + [Obj(-2 + 4 * i / T, 6, 1.2, 1.2, (0.9, 0.3, 0.2), 'ball', 30, 30)]
            views = [render(W, H, s * 0.032, 0, objs) for s in (-1, 1)]
            if name == 'sbs_full':
                rgb = np.concatenate([views[0][0], views[1][0]], 1); dep = np.concatenate([views[0][1], views[1][1]], 1)
                oid = np.concatenate([views[0][2], views[1][2]], 1)
            elif name == 'sbs_half':
                rgb = np.concatenate([v[0][:, ::2] for v in views], 1); dep = np.concatenate([v[1][:, ::2] for v in views], 1)
                oid = np.concatenate([v[2][:, ::2] for v in views], 1)
            else:
                rgb = np.concatenate([v[0][::2] for v in views], 0); dep = np.concatenate([v[1][::2] for v in views], 0)
                oid = np.concatenate([v[2][::2] for v in views], 0)
            wr.frame(rgb, dep, oid)
        wr.close(meta)
        return
    wr = Writer(out, name, W, H)
    font = ImageFont.truetype(os.environ.get('FD_FONT', 'arial.ttf'), 34) if name in ('subs', 'lbsubs') else None
    lines = ['Where were you last night?', 'I told you, I was at home.', 'Then why is the car still warm?']
    other = park(5, zmin=3, zmax=25)
    for o in other: o.color = o.color[::-1]
    for i in range(T):
        cam_x, palette, objs = 0.0, 'day', list(base)
        if name in ('motion', 'subs', 'letterbox', 'lbsubs', 'pillarbox'):
            objs += [Obj(-6 + 12 * ((i / T * 1.0) % 1), 7, 1.0, 1.6, (0.9, 0.3, 0.2), 'box', 31, 31),
                     Obj(5 - 10 * ((i / T * 2.0) % 1), 12, 1.0, 1.0, (0.2, 0.3, 0.9), 'ball', 32, 32),
                     Obj(-8 + 16 * ((i / T * 4.0) % 1), 5, 0.8, 0.8, (0.95, 0.95, 0.3), 'ball', 33, 33)]
        if name == 'pan':
            cam_x = 3 * math.sin(i / T * 2 * math.pi)
        if name == 'cuts':
            seg = (i // FPS) % 2
            if i % FPS == 0 and i: meta['cuts'].append(i)
            objs = list(other) if seg else objs
            palette = 'day'
        if name == 'edge':
            # a near box sliding in from the left edge, then one from the right
            k = i / T
            objs += [Obj(-3.2 + 2.0 * min(k * 2, 1), 2.2, 1.2, 2.0, (0.85, 0.5, 0.2), 'box', 34, 34)] if k < 0.5 else \
                    [Obj(3.2 - 2.0 * min((k - 0.5) * 2, 1), 2.2, 1.2, 2.0, (0.2, 0.6, 0.85), 'box', 35, 35)]
        if name == 'dark':
            palette = 'night'
        wall = (14, 0.9) if name == 'pattern' else None
        if name in ('letterbox', 'lbsubs'):
            ch = int(W / 2.39)
            rgb, dep, oid = render(W, ch, cam_x, 0, objs, palette)
            if name == 'letterbox' and i >= T * 2 // 3:   # a very dark last third: the bars must not grow into it
                rgb = rgb * 0.06
            rgb, dep, oid, meta['content'] = letterboxed(rgb, dep, oid, W, ch)
        elif name == 'pillarbox':
            cw = H * 4 // 3
            rgb, dep, oid = render(cw, H, cam_x, 0, objs, palette)
            rgb, dep, oid, meta['content'] = letterboxed(rgb, dep, oid, cw, H)
        else:
            rgb, dep, oid = render(W, H, cam_x, 0, objs, palette, wall)
        if name in ('subs', 'lbsubs'):
            k = (i // (2 * FPS)) % 3
            if (i // FPS) % 2 == 0:   # on for a second, off for a second
                # lbsubs: in the bottom bar, as letterboxed films often have them
                mid = None if name == 'subs' else (H + meta['content'][1] + meta['content'][3]) // 2
                rgb, box = subtitle(rgb, lines[k], font, mid)
                meta['subs'].append([i] + box)
        wr.frame(rgb, dep, oid)
    wr.close(meta)


ALL = ['static', 'motion', 'pan', 'cuts', 'letterbox', 'lbsubs', 'pillarbox', 'edge', 'sbs_full', 'sbs_half', 'tb_half', 'subs', 'dark', 'pattern']

if __name__ == '__main__':
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    for name in sys.argv[2:] or ALL:
        scene(out, name)
        print('wrote', name)
