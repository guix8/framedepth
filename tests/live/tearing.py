# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 FrameDepth contributors
# Is a frame read from Chrome's window ever torn (its top from one video
# frame, its bottom from another)? A clip whose every frame is one solid
# colour, never the same colour twice in a row, makes that plain: a read
# picture showing two of the colours, one above the other, is torn.
#   python -I tearing.py make OUT_DIR             flash30.webm and flash60.webm (1280x720, 10 s)
#   python -I tearing.py check DUMP_DIR [...]     the *_src.ppm of run_live.sh dumps
# check prints, for each dump folder, how many reads were torn and where.
import glob, json, os, subprocess, sys
import numpy as np

# red, green, blue, yellow, cyan, magenta: frame N has colour N % 6
PALETTE = np.array([[255, 0, 0], [0, 255, 0], [0, 0, 255], [255, 255, 0], [0, 255, 255], [255, 0, 255]], np.float32)


def make(out):
    os.makedirs(out, exist_ok=True)
    m = 'mod(N,6)'
    r = f'255*(eq({m},0)+eq({m},3)+eq({m},5))'
    g = f'255*(eq({m},1)+eq({m},3)+eq({m},4))'
    b = f'255*(eq({m},2)+eq({m},4)+eq({m},5))'
    for fps in (30, 60):
        path = os.path.join(out, f'flash{fps}.webm')
        subprocess.run(['ffmpeg', '-loglevel', 'error', '-y', '-f', 'lavfi', '-i', f'color=c=black:s=1280x720:r={fps}:d=10',
                        '-vf', f"format=rgb24,geq=r='{r}':g='{g}':b='{b}',format=yuv420p",
                        '-c:v', 'libvpx-vp9', '-b:v', '1M', '-row-mt', '1', '-deadline', 'good', '-cpu-used', '4', path], check=True)
        print('wrote', path)


def read_ppm(path):
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
    return np.frombuffer(data, np.uint8, w * h * 3, pos + 1).reshape(h, w, 3).astype(np.float32)


# Each row's colour (the middle 80% of it), as the nearest palette colour or
# -1 when none is near (a row blending two, or not the clip at all)
def row_colours(img):
    h, w, _ = img.shape
    rows = img[:, w // 10: w - w // 10].mean(axis=1)
    d = np.linalg.norm(rows[:, None, :] - PALETTE[None], axis=2)
    near = d.argmin(axis=1)
    near[d.min(axis=1) > 80] = -1
    return near


def check(folder):
    torn, total, unclear, where = 0, 0, 0, []
    for path in sorted(glob.glob(os.path.join(folder, '*_src.ppm'))):
        c = row_colours(read_ppm(path))
        total += 1
        known = c[c >= 0]
        if len(known) < len(c) * 0.5:
            unclear += 1
            continue
        counts = np.bincount(known, minlength=6)
        shown = [i for i in range(6) if counts[i] >= len(c) * 0.02]
        if len(shown) > 1:
            torn += 1
            # the rows where the colour changes, as a share of the height
            edges = [round(i / len(c), 3) for i in range(1, len(c)) if c[i] >= 0 and c[i - 1] >= 0 and c[i] != c[i - 1]]
            where.append({'dump': os.path.basename(path), 'colours': shown, 'at': edges[:6]})
    return {'folder': folder, 'reads': total, 'torn': torn, 'unclear': unclear, 'examples': where[:10]}


if __name__ == '__main__':
    if sys.argv[1] == 'make':
        make(sys.argv[2])
    else:
        for f in sys.argv[2:]:
            print(json.dumps(check(f)))
