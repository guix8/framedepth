# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 FrameDepth contributors
# Numbers for one pipeline run on one synthetic scene (make_scenes.py), from
# the depth each frame was drawn with (shown_*.pgm), the depth of that very
# frame (oracle_*.pgm) and the scene's true depth and object ids. Depth values
# are the map's 0..1 (1 = near), reported x100 (percent of the depth range).
#   flicker     frame-to-frame change of the shown depth where nothing moved
#               (lower is steadier)
#   lag         shown against oracle depth where something moved in the last
#               frames: how far the shown depth trails the picture
#   error       shown against oracle over the whole picture
#   cutFrames   frames after each scene cut until the shown depth caught up
#   drift1s, drift3s  how far the shown depth moved over 1 s and 3 s where
#               nothing moved all that while (no cut between; x100), and
#               *Shift: the part of it that moved the whole picture at once
#               (the mean signed change): flicker sees only one frame's
#               change, so a scene slowly pushed back or forward by the
#               range's normalisation did not show (review 2026-11-02)
#   cutFill     the depth's spread (95th less 5th percentile) in the first
#               two maps of a shot against its spread once settled (15-25
#               frames on), mean over cuts and the worst (cutFillWorst): below
#               1 the new shot came in flat and widened after
#   modelVsTrue correlation of the oracle depth with the true inverse depth:
#               whether the model reads the scene at all (a sanity check)
#   depthRange  spread of the oracle depth inside the picture (5th to 95th
#               percentile, mean over frames): flicker, lag and error are also
#               given as a share of it (*Rel, %), comparable when a change
#               widens or narrows the depth range itself
#   barEdgePx   how far the black bars found (frames.csv area) are from the
#               scene's picture area, worst edge in frame pixels, mean over
#               frames after the first second
#   parallaxDeg the largest angle between where the two eyes see a point of
#               the picture, from the depth each frame was drawn with and its
#               separation and convergence (frames.csv), for the picture's
#               width and distance in the run's settings (widthm, distm); 95th
#               percentile over frames
#   edgeOut     how far in front of the screen plane the picture comes at its
#               left and right edges (5% bands, upper three quarters as the
#               companion's check, 95th percentile depth minus convergence,
#               x100, mean over frames; 0 when nothing does)
#   convStep    the largest change of convergence from one frame to the next
#               (x100): pumping
#   stereoWrong share of frames after the first two seconds where the video
#               was taken for another format than it is (flat, side by side,
#               top and bottom: frames.csv stereo against the scene's format)
#   streaks     share of each eye's pixels that look like a stretch streak
#               (flat sideways, sharp up and down: |dx| < 2, |dy| > 12 grey
#               levels) beyond the source frame's own share, x100, from the
#               side-by-side frames (sbs_*.ppm) of flat scenes
#   sharpness   mean gradient of the eyes against the source's (x100): below
#               100 means the 3D softened the picture
#   sharpX, sharpY  the same across and up and down alone: the eyes are
#               resampled sideways only, so softening shows in sharpX
#   subRecall   share of frames showing a subtitle (from 10 frames after it
#               appears) where the box kept flat covers half of it or more
#   subFalse    share of frames with no subtitle (from 15 frames after one
#               went) where a box was kept flat anyway
#   barWrong    share of those frames where an edge is off by more than 2% of
#               the frame (bars where there are none count, and missed bars)
# Real footage (make_real.py) has no true depth: what moved comes from the
# picture itself (its frame-to-frame change), the picture area from the
# bars found in it, and modelVsTrue is left out.
#   python -I metrics.py SCENES_DIR RUN_DIR NAME     prints the JSON
import json, os, subprocess, sys
import numpy as np

MW, MH = 512, 288


def read_pgm(path):
    with open(path, 'rb') as f:
        data = f.read()
    parts, pos = [], 0
    while len(parts) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        end = pos
        while not data[end:end + 1].isspace():
            end += 1
        parts.append(data[pos:end]); pos = end
    w, h = int(parts[1]), int(parts[2])
    return np.frombuffer(data, np.uint8, w * h, pos + 1).reshape(h, w).astype(np.float32) / 255.0


def dilate(m, r):
    out = m.copy()
    for dy in range(-r, r + 1):
        for dx in range(-r, r + 1):
            out |= np.roll(np.roll(m, dy, 0), dx, 1)
    return out


def load_run(run_dir, name, n):
    shown = np.stack([read_pgm(os.path.join(run_dir, f'shown_{i:05d}.pgm')) for i in range(n)])
    op = os.path.join(run_dir, 'oracle_00000.pgm')
    oracle = np.stack([read_pgm(os.path.join(run_dir, f'oracle_{i:05d}.pgm')) for i in range(n)]) if os.path.exists(op) else None
    return shown, oracle


def scene_metrics(scenes_dir, run_dir, name):
    meta = json.load(open(os.path.join(scenes_dir, name + '.json')))
    runs = len([f for f in os.listdir(run_dir) if f.startswith('shown_')])
    npz = os.path.join(scenes_dir, name + '.npz')
    if os.path.exists(npz):
        gt = np.load(npz)
        depth = gt['depth'].astype(np.float32); oid = gt['oid']
        n = min(len(depth), runs)
        depth, oid = depth[:n], oid[:n]
        valid = depth >= 0
        inv = np.where(valid, 1.0 / np.maximum(depth, 0.5), 0)
        # where something moved: another object, or the true depth changed,
        # then widened a little (the map is blurred) and kept for 10 frames
        moved = np.zeros_like(valid)
        moved[1:] = (oid[1:] != oid[:-1]) | (np.abs(inv[1:] - inv[:-1]) > 0.002)
    else:
        MH, MW = read_pgm(os.path.join(run_dir, 'shown_00000.pgm')).shape   # the depth model's size (512x288, 768x448)
        raw = subprocess.run(['ffmpeg', '-loglevel', 'error', '-i', os.path.join(scenes_dir, name + '.webm'), '-vf', f'scale={MW}:{MH}',
                              '-f', 'rawvideo', '-pix_fmt', 'gray', '-'], capture_output=True, check=True).stdout
        g = np.frombuffer(raw, np.uint8).reshape(-1, MH, MW).astype(np.float32) / 255.0
        n = min(len(g), runs)
        g, depth = g[:n], None
        fw, fh = meta['size']
        x, y, w, h = meta['content']
        area = np.zeros((MH, MW), bool)
        area[y * MH // fh:(y + h) * MH // fh, x * MW // fw:(x + w) * MW // fw] = True
        valid = np.broadcast_to(area, (n, MH, MW))
        moved = np.zeros((n, MH, MW), bool)
        moved[1:] = np.abs(g[1:] - g[:-1]) > 0.05
    shown, oracle = load_run(run_dir, name, n)
    if valid.shape[1:] != shown.shape[1:]:
        # a larger depth model (768x448): the truth (512x288) to its size
        ys = np.arange(shown.shape[1]) * valid.shape[1] // shown.shape[1]
        xs = np.arange(shown.shape[2]) * valid.shape[2] // shown.shape[2]
        pick = lambda a: a[:, ys][:, :, xs]
        valid, moved = pick(np.asarray(valid)), pick(moved)
        if depth is not None:
            depth, oid, inv = pick(depth), pick(oid), pick(inv)
    if meta['kind'] == 'pan':
        moved[:] = True
    moved = np.stack([dilate(m, 4) for m in moved])
    recent = moved.copy()
    for k in range(1, 10):
        recent[k:] |= moved[:-k]
    after_cut = np.zeros(n, bool)
    for c in meta.get('cuts', []):
        after_cut[c:c + 15] = True
    still = valid & ~recent & ~after_cut[:, None, None]
    out = {'scene': name, 'frames': int(n)}
    d = np.abs(shown[1:] - shown[:-1])
    sm = still[1:] & still[:-1]
    out['flicker'] = round(float(d[sm].mean()) * 100, 3) if sm.any() else None
    if oracle is not None:
        e = np.abs(shown - oracle)
        mv = recent & valid & ~after_cut[:, None, None]
        out['lag'] = round(float(e[mv].mean()) * 100, 3) if mv.any() else None
        per_frame = np.array([e[i][valid[i]].mean() for i in range(n)])
        ok = ~after_cut
        out['error'] = round(float(per_frame[ok].mean()) * 100, 3)
        typical = np.median(per_frame[ok])
        frames_back = []
        for c in meta.get('cuts', []):
            k = 0
            while c + k < n and per_frame[c + k] > 2 * typical:
                k += 1
            frames_back.append(k)
        out['cutFrames'] = frames_back
        corr = []
        for i in (range(0, n, 5) if depth is not None else ()):
            v = valid[i] & (depth[i] < 900)
            if v.sum() > 100:
                a, b = oracle[i][v], inv[i][v]
                corr.append(np.corrcoef(a, b)[0, 1])
        out['modelVsTrue'] = round(float(np.mean(corr)), 3) if corr else None
        spread = [np.percentile(oracle[i][valid[i]], 95) - np.percentile(oracle[i][valid[i]], 5) for i in range(0, n, 5) if valid[i].any()]
        rng = float(np.mean(spread)) if spread else 0
        out['depthRange'] = round(rng * 100, 2)
        for k in ('flicker', 'lag', 'error'):
            if out.get(k) is not None and rng > 0:
                out[k + 'Rel'] = round(out[k] / rng, 2)
    import csv
    rows = list(csv.DictReader(open(os.path.join(run_dir, 'frames.csv'))))
    fps = int(meta['fps'])
    cuts = [c for c in meta.get('cuts', []) if c < n]
    cut_at = np.zeros(n, bool)
    cut_at[cuts] = True
    for secs, key in ((1, 'drift1s'), (3, 'drift3s')):
        k = secs * fps
        absd, shift = [], []
        for i in range(k, n, 5):
            if cut_at[i - k + 1:i + 1].any():
                continue
            m = valid[i] & ~moved[i - k + 1:i + 1].any(0)
            if m.sum() < 1000:
                continue
            dd = shown[i][m] - shown[i - k][m]
            absd.append(np.abs(dd).mean()); shift.append(abs(dd.mean()))
        out[key] = round(float(np.mean(absd)) * 100, 2) if absd else None
        out[key + 'Shift'] = round(float(np.mean(shift)) * 100, 2) if shift else None
    if rows and 'map_frame' in rows[0]:
        def spread(i):
            v = shown[i][valid[i]]
            return float(np.percentile(v, 95) - np.percentile(v, 5)) if v.size else 0.0
        mapf = np.array([int(r['map_frame']) for r in rows[:n]])
        fills = []
        for j, c in enumerate(cuts):
            end = cuts[j + 1] if j + 1 < len(cuts) else n
            if end < c + 26:
                continue
            settled = np.median([spread(i) for i in range(c + 15, c + 26)])
            first = []   # the frames drawn with the shot's first two maps
            for i in range(c, c + 15):
                if mapf[i] >= c and (not first or mapf[i] != mapf[first[-1]]):
                    first.append(i)
                if len(first) == 2:
                    break
            if settled > 0.05 and first:
                fills.append(min(spread(i) for i in first) / settled)
        if fills:
            out['cutFill'] = round(float(np.mean(fills)), 3)
            out['cutFillWorst'] = round(float(min(fills)), 3)
    if rows and 'area_x0' in rows[0]:
        fw, fh = meta['size']
        x, y, w, h = meta['content']
        truth = np.array([x / fw, y / fh, (x + w) / fw, (y + h) / fh])
        found = np.array([[float(r[k]) for k in ('area_x0', 'area_y0', 'area_x1', 'area_y1')] for r in rows[int(meta['fps']):n]])
        if len(found):
            err = np.abs(found - truth) * np.array([fw, fh, fw, fh])
            worst = err.max(1)
            out['barEdgePx'] = round(float(worst.mean()), 2)
            out['barWrong'] = round(float((np.abs(found - truth).max(1) > 0.02).mean()), 3)
    if rows and 'stereo' in rows[0]:
        want = {'2d': 0, 'sbs_full': 1, 'sbs_half': 1, 'tb_half': 2, 'tb_full': 2}[meta.get('format', '2d')]
        got = np.array([int(r['stereo']) for r in rows[2 * int(meta['fps']):n]])
        if len(got):
            out['stereoWrong'] = round(float((got != want).mean()), 3)
    if rows and 'sub_x0' in rows[0]:
        fw, fh = meta['size']
        truth = {s[0]: (s[1] / fw, s[2] / fh, (s[1] + s[3]) / fw, (s[2] + s[4]) / fh) for s in meta.get('subs', [])}
        on_since, off_since, hit, on_n, fp, off_n = None, -100, 0, 0, 0, 0
        for i in range(n):
            r = rows[i]
            d = [float(r[k]) for k in ('sub_x0', 'sub_y0', 'sub_x1', 'sub_y1')]
            det = d[2] > d[0]
            if i in truth:
                on_since = i if on_since is None else on_since
                if i - on_since >= 10:
                    t = truth[i]
                    ix = max(0, min(d[2], t[2]) - max(d[0], t[0])) * max(0, min(d[3], t[3]) - max(d[1], t[1]))
                    on_n += 1; hit += det and ix >= 0.5 * (t[2] - t[0]) * (t[3] - t[1])
            else:
                if on_since is not None: off_since, on_since = i, None
                if i - off_since >= 15 and i >= int(meta['fps']):
                    off_n += 1; fp += det
        if on_n: out['subRecall'] = round(hit / on_n, 3)
        if off_n: out['subFalse'] = round(fp / off_n, 3)
    if rows and 'sep' in rows[0]:
        s = json.load(open(os.path.join(run_dir, 'summary.json')))['settings']
        sep = np.array([float(r['sep']) for r in rows[:n]]); cv = np.array([float(r['conv']) for r in rows[:n]])
        dev = np.array([max(abs(shown[i][valid[i]].max() - cv[i]), abs(shown[i][valid[i]].min() - cv[i])) if valid[i].any() else 0 for i in range(n)])
        # the shift is made on the picture zoomed by the overscan (Comfort::zoom)
        st, cv0 = float(s.get('strength', 1.5)) / 100, 1 - float(s.get('popout', 50)) / 100
        zoom = 1 / (1 - 2 * st * max(cv0, 1 - cv0))
        ang = np.degrees(np.arctan(2 * sep * dev * zoom * float(s.get('widthm', 0.98)) / float(s.get('distm', 1.48))))
        out['parallaxDeg'] = round(float(np.percentile(ang, 95)), 3)
        band = max(1, int(shown.shape[2] * 0.05))
        edge = []
        for i in range(n):
            v = valid[i].copy()
            rws = np.where(v.any(1))[0]
            if len(rws):
                v[rws[0] + int((rws[-1] + 1 - rws[0]) * 0.75):] = False
            cols = np.where(v.any(0))[0]
            if not len(cols):
                continue
            l, r = shown[i][:, cols[0]:cols[0] + band][v[:, cols[0]:cols[0] + band]], shown[i][:, cols[-1] + 1 - band:cols[-1] + 1][v[:, cols[-1] + 1 - band:cols[-1] + 1]]
            near = max(np.percentile(l, 95) if l.size else 0, np.percentile(r, 95) if r.size else 0)
            edge.append(max(0.0, near - cv[i]))
        out['edgeOut'] = round(float(np.mean(edge)) * 100, 2) if edge else None
        out['convStep'] = round(float(np.abs(np.diff(cv)).max()) * 100, 3) if n > 1 else 0
    sbs = sorted(f for f in os.listdir(run_dir) if f.startswith('sbs_') and f.endswith('.ppm'))
    if sbs and meta.get('format', '2d') == '2d':
        from PIL import Image
        idx = [int(f[4:9]) for f in sbs]
        fw, fh = meta['size']
        sel = '+'.join(f'eq(n\,{i})' for i in idx)
        raw = subprocess.run(['ffmpeg', '-loglevel', 'error', '-i', os.path.join(scenes_dir, name + '.webm'), '-vf', f"select='{sel}'",
                              '-vsync', '0', '-f', 'rawvideo', '-pix_fmt', 'gray', '-'], capture_output=True, check=True).stdout
        srcs = np.frombuffer(raw, np.uint8).reshape(-1, fh, fw).astype(np.float32)
        def streak(img):
            gx = np.abs(np.diff(img, axis=1))[:-1, :]; gy = np.abs(np.diff(img, axis=0))[:, :-1]
            return float(((gx < 2) & (gy > 12)).mean()), float((gx + gy).mean()), float(gx.mean()), float(gy.mean())
        ex, sh, shx, shy = [], [], [], []
        for f, src in zip(sbs, srcs):
            im = np.asarray(Image.open(os.path.join(run_dir, f)).convert('L'), np.float32)
            ew = im.shape[1] // 2
            s0, g0, x0, y0 = streak(np.asarray(Image.fromarray(src.astype(np.uint8)).resize((ew, im.shape[0])), np.float32))
            for eye in (im[:, :ew], im[:, ew:2 * ew]):
                s1, g1, x1, y1 = streak(eye)
                ex.append(s1 - s0); sh.append(g1 / g0 if g0 else 1)
                shx.append(x1 / x0 if x0 else 1); shy.append(y1 / y0 if y0 else 1)
        out['streaks'] = round(float(np.mean(ex)) * 100, 3)
        out['sharpness'] = round(float(np.mean(sh)) * 100, 1)
        out['sharpX'] = round(float(np.mean(shx)) * 100, 1)
        out['sharpY'] = round(float(np.mean(shy)) * 100, 1)
    summ = os.path.join(run_dir, 'summary.json')
    if os.path.exists(summ):
        s = json.load(open(summ))
        out.update({k: s[k] for k in ('modelMs', 'postMs', 'warpMs', 'warpMaxMs')})
    return out


if __name__ == '__main__':
    print(json.dumps(scene_metrics(*sys.argv[1:4]), indent=1))
