# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 FrameDepth contributors
# A page for judging 3D on a normal monitor: for each side-by-side frame, a
# "wiggle" GIF (left and right eye in turn: depth shows as motion parallax)
# and a red-cyan anaglyph, with one or more runs next to each other.
#   python -I compare.py OUT_DIR LABEL=RUN_DIR [LABEL=RUN_DIR ...] [--frames sbs_00030,sbs_00060]
# RUN_DIR holds sbs_*.ppm (run.py's results, or --dump's *_sbs.ppm). Open
# OUT_DIR/index.html.
import html, os, sys
from PIL import Image


def eyes(path):
    im = Image.open(path).convert('RGB')
    w = im.width // 2
    return im.crop((0, 0, w, im.height)), im.crop((w, 0, 2 * w, im.height))


def make(sbs, out_base, width=640):
    left, right = eyes(sbs)
    h = left.height * width // left.width
    left, right = left.resize((width, h)), right.resize((width, h))
    left.save(out_base + '_wiggle.gif', save_all=True, append_images=[right], duration=140, loop=0)
    l, _, _ = left.split()
    _, g, b = right.split()
    Image.merge('RGB', (l, g, b)).save(out_base + '_anaglyph.png')


if __name__ == '__main__':
    out = sys.argv[1]
    runs = [a.split('=', 1) for a in sys.argv[2:] if '=' in a and not a.startswith('--')]
    pick = None
    if '--frames' in sys.argv:
        pick = sys.argv[sys.argv.index('--frames') + 1].split(',')
    os.makedirs(out, exist_ok=True)
    names = sorted({f[:-4] for _, d in runs for f in os.listdir(d) if f.endswith('.ppm') and 'sbs' in f})
    if pick:
        names = [n for n in names if n in pick]
    rows = []
    for n in names:
        cells = []
        for label, d in runs:
            p = os.path.join(d, n + '.ppm')
            if not os.path.exists(p):
                cells.append('<td>-</td>'); continue
            base = f'{label}_{n}'
            make(p, os.path.join(out, base))
            cells.append(f'<td><b>{html.escape(label)}</b><br><img src="{base}_wiggle.gif"><br>'
                         f'<img src="{base}_anaglyph.png"></td>')
        rows.append(f'<tr><th>{html.escape(n)}</th>{"".join(cells)}</tr>')
    with open(os.path.join(out, 'index.html'), 'w', encoding='utf-8') as f:
        f.write('<!doctype html><meta charset="utf-8"><title>FrameDepth compare</title>'
                '<style>body{font:14px sans-serif;background:#111;color:#ddd}img{display:block;margin:4px 0}'
                'td,th{vertical-align:top;padding:6px}</style>'
                '<p>Wiggle: left and right eye in turn (depth shows as parallax). Anaglyph: red-cyan glasses.</p>'
                f'<table>{"".join(rows)}</table>')
    print('wrote', os.path.join(out, 'index.html'), len(names), 'frames')
