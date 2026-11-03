# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 FrameDepth contributors
# Burns subtitles into a letterboxed real scene (make_real.py), inside its
# bottom bar as letterboxed films often have them: on for two seconds, off
# for one. Writes NAME_subs.webm and NAME_subs.json (the subtitle boxes).
#   python -I add_subs.py SCENES_DIR NAME
# Needs numpy, Pillow and ffmpeg (FD_FONT: a .ttf, default arial.ttf).
import json, os, subprocess, sys
import numpy as np
from PIL import Image, ImageDraw, ImageFont

LINES = ['Where were you last night?', 'I told you, I was at home.', 'Then why is the car still warm?']


def main():
    scenes, name = sys.argv[1], sys.argv[2]
    meta = json.load(open(os.path.join(scenes, name + '.json')))
    w, h = meta['size']
    fps = int(meta['fps'])
    x, y, cw, ch = meta['content']
    if y + ch >= h:
        sys.exit(f'{name} has no bottom bar')
    mid = (h + y + ch) // 2
    font = ImageFont.truetype(os.environ.get('FD_FONT', 'arial.ttf'), round(h * 0.06))
    src = subprocess.Popen(['ffmpeg', '-loglevel', 'error', '-i', os.path.join(scenes, name + '.webm'),
                            '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-'], stdout=subprocess.PIPE)
    out = name + '_subs'
    dst = subprocess.Popen(['ffmpeg', '-loglevel', 'error', '-y', '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-s', f'{w}x{h}',
                            '-r', str(fps), '-i', '-', '-c:v', 'libvpx-vp9', '-crf', '30', '-b:v', '0', '-deadline', 'good',
                            '-cpu-used', '4', '-row-mt', '1', '-pix_fmt', 'yuv420p', os.path.join(scenes, out + '.webm')],
                           stdin=subprocess.PIPE)
    subs, i = [], 0
    while True:
        raw = src.stdout.read(w * h * 3)
        if len(raw) < w * h * 3:
            break
        img = Image.fromarray(np.frombuffer(raw, np.uint8).reshape(h, w, 3))
        if (i // fps) % 3 != 2:
            text = LINES[(i // (3 * fps)) % len(LINES)]
            d = ImageDraw.Draw(img)
            box = d.textbbox((0, 0), text, font=font, stroke_width=2)
            tw, th = box[2] - box[0], box[3] - box[1]
            tx, ty = (w - tw) // 2, mid - th // 2
            d.text((tx - box[0], ty - box[1]), text, font=font, fill=(255, 255, 255), stroke_width=2, stroke_fill=(0, 0, 0))
            subs.append([i, tx, ty, tw, th])
        dst.stdin.write(img.tobytes())
        i += 1
    src.wait(); dst.stdin.close(); dst.wait()
    meta.update({'name': out, 'frames': i, 'subs': subs})
    json.dump(meta, open(os.path.join(scenes, out + '.json'), 'w'), indent=1)
    print('wrote', out, i, 'frames')


if __name__ == '__main__':
    main()
