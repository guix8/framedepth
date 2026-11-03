# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 FrameDepth contributors
# Real footage as test scenes, in the form make_scenes.py writes (NAME.webm,
# NAME.json; no ground truth, so no NAME.npz). Black bars burned into the
# picture and scene cuts are found from the clip itself.
#   python -I make_real.py SRC OUT_DIR NAME START SECONDS [fit=scale|pad|crop] [layout=2d|tb2sbs|left] [format=2d|tb_half|sbs_half] [source=TEXT]
# SRC is a file or URL ffmpeg reads (an http source is read only around the
# clip). The clip is made 1280x720:
#   fit    scale (default) stretches, pad keeps the aspect with black bars,
#          crop fills the frame cutting the sides
#   layout for a top-bottom 3D source: tb2sbs rearranges it side by side,
#          left keeps only the top (left eye) picture as plain 2D
#   format what the clip is (recorded for the stereo detector's tests)
import json, os, subprocess, sys
import numpy as np

W, H, MW, MH = 1280, 720, 512, 288


def gray_frames(path, w, h):
    raw = subprocess.run(['ffmpeg', '-loglevel', 'error', '-i', path, '-vf', f'scale={w}:{h}', '-f', 'rawvideo', '-pix_fmt', 'gray', '-'],
                         capture_output=True, check=True).stdout
    return np.frombuffer(raw, np.uint8).reshape(-1, h, w).astype(np.float32) / 255.0


def main():
    src, out, name, start, secs = sys.argv[1:6]
    opt = dict(a.split('=', 1) for a in sys.argv[6:])
    fit, layout = opt.get('fit', 'scale'), opt.get('layout', '2d')
    vf = {'scale': f'scale={W}:{H}',
          'pad': f'scale={W}:{H}:force_original_aspect_ratio=decrease,pad={W}:{H}:(ow-iw)/2:(oh-ih)/2',
          'crop': f'scale={W}:{H}:force_original_aspect_ratio=increase,crop={W}:{H}'}[fit]
    if layout == 'tb2sbs':
        vf = f'stereo3d=abl:sbsl,{vf}'
    elif layout == 'left':
        vf = f'crop=iw:ih/2:0:0,{vf}'
    os.makedirs(out, exist_ok=True)
    webm = os.path.join(out, name + '.webm')
    # square pixels: the stereo filter marks its output 2:1, which a browser
    # then shows twice as wide
    subprocess.run(['ffmpeg', '-loglevel', 'error', '-y', '-ss', start, '-i', src, '-t', secs, '-vf', vf + ',setsar=1,fps=30', '-an',
                    '-c:v', 'libvpx-vp9', '-crf', '30', '-b:v', '0', '-deadline', 'good', '-cpu-used', '4', '-row-mt', '1',
                    '-pix_fmt', 'yuv420p', webm], check=True)
    g = gray_frames(webm, MW, MH)
    # bars: rows and columns that stay near black in every frame
    dark = g.max(0) < 0.07
    rows, cols = np.where(~dark.all(1))[0], np.where(~dark.all(0))[0]
    y0, y1 = (rows[0], rows[-1] + 1) if len(rows) else (0, MH)
    x0, x1 = (cols[0], cols[-1] + 1) if len(cols) else (0, MW)
    content = [int(x0 * W / MW), int(y0 * H / MH), int((x1 - x0) * W / MW), int((y1 - y0) * H / MH)]
    # cuts: a jump in the picture well above the frame-to-frame changes around it
    d = np.abs(g[1:] - g[:-1]).mean((1, 2))
    # (a burst of jumps, as in an animated title or a flash, counts once)
    cuts = []
    for i in range(len(d)):
        if d[i] > 0.08 and d[i] > 4 * np.median(d[max(0, i - 15):i + 15]) and not (cuts and i + 1 - cuts[-1] < 10):
            cuts.append(int(i + 1))
    meta = {'name': name, 'kind': 'real', 'format': opt.get('format', '2d'), 'size': [W, H], 'fps': 30, 'frames': int(len(g)),
            'content': content, 'cuts': cuts, 'subs': [], 'source': opt.get('source', src), 'clip': [float(start), float(secs)]}
    json.dump(meta, open(os.path.join(out, name + '.json'), 'w'), indent=1)
    print(name, len(g), 'frames, content', content, 'cuts', cuts)


if __name__ == '__main__':
    main()
