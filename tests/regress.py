# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 FrameDepth contributors
# Everything that can be checked without wearing the headset, in one run:
#   pc        the extension in a throwaway Chrome against mock_companion.py,
#             on each test page; what it reports is compared with the last
#             recorded run (--record to record)
#   headset   on the headset (FD_HEADSET=user@host, built tree at FD_REMOTE,
#             default ~/src/framedepth): protocol test on a second companion,
#             --gputest (see-through share), --depthcheck (CPU against GPU model,
#             on a frame of the synthetic static scene: run synth once first)
#   synth     synthetic scenes through --pipeline (synth/run.py) as LABEL,
#             compared with BASELINE: worse by more than the tolerance fails
#   live      (shows on the headset) the whole live path with tests/live/run_live.sh,
#             then --rerender of each dump must match it exactly
#   python -I regress.py [pc] [headset] [synth=SCENES_DIR:LABEL:BASELINE] [live=SCENES_DIR] [--record]
# CHROME: path of chrome.exe/chrome on this computer (default: the usual Windows place).
# FD_EXTENSION: another extension folder to test (default extension/), e.g. a
# previous version to record the expectations with.
# FD_SHOW=1: print what drive_chrome.py printed for each page.
# FD_ONLY: run only these pages (comma separated; nothing is compared for the others).
# FD_DEBUG_PORT: Chrome's debugging port (default 9334; Windows can reserve a
# range around it while WSL or Hyper-V runs, then pick one outside it, e.g. 9700).
import json, os, re, shutil, socket, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DEBUG_PORT = int(os.environ.get('FD_DEBUG_PORT', '9334'))
PAGES = ['image-test.html', 'iframe-test.html', 'grid-test.html', 'ui-detect.html', 'flap-test.html', 'shadow-video.html',
         'nested-iframe-test.html', 'shadow-iframe-test.html', 'offscreen-iframe-test.html']
# What each page is there to prove, compared with the recorded run (flap-test
# scrolls all the time: only that the view started once and never restarted)
# ('starts', from the extension's diagnostic log, is printed but not compared:
# the content script and the service worker write that log at once and a
# line can be lost)
COMPARE = {'image-test.html': ['viewsOn', 'lastRect'], 'iframe-test.html': ['viewsOn', 'lastRect'],
           'grid-test.html': ['viewsOn', 'onlyCount'], 'ui-detect.html': ['viewsOn', 'lastRect', 'uiCount'],
           'flap-test.html': ['viewsOn'], 'shadow-video.html': ['viewsOn', 'lastRect', 'uiCount'],
           'nested-iframe-test.html': ['viewsOn', 'lastRect'], 'shadow-iframe-test.html': ['viewsOn', 'lastRect'],
           'offscreen-iframe-test.html': ['viewsOn', 'lastRect']}
# pages scrolled while they run (drive_chrome.py FD_SCROLL), and run longer
SCROLLED = {'nested-iframe-test.html': '7'}
# Where each page says its picture is (top-page CSS px, after any scroll):
# the last rect must be there in window pixels, worked out from the top
# page's own sizes (drive_chrome.py prints them), within a pixel. Comparing
# with a recorded run only would pass a wrong recording for ever (review
# 2026-11-01).
EXPECT_CSS = {'image-test.html': (50, 60, 800, 450), 'iframe-test.html': (120, 180, 640, 360),
              'nested-iframe-test.html': (170, 0, 640, 280), 'shadow-iframe-test.html': (120, 180, 640, 360),
              'border-iframe-test.html': (132, 192, 640, 360), 'offscreen-iframe-test.html': (50, 60, 800, 450)}
# Runs checked only against EXPECT_CSS: (page, Chrome's display scale, page zoom)
VARIANTS = [('border-iframe-test.html', 1, 1), ('iframe-test.html', 1.5, 1), ('border-iframe-test.html', 1.5, 1),
            ('iframe-test.html', 1, 1.25), ('image-test.html', 1, 1.25)]


def expected_window(css, top, zoom):
    # as content.js's topInfo: Chrome's toolbar above the page, borders at
    # the sides, from the window's outer size (DIP) and the page's inner (CSS)
    dpr = top['dpr']; scale = dpr / zoom
    extra = top['ow'] * scale - top['iw'] * dpr
    cl = 0 if extra > 32 else max(0, extra / 2)
    ct = max(0, top['oh'] * scale - top['ih'] * dpr)
    x, y, w, h = css
    return [round(cl + x * dpr), round(ct + y * dpr), round(w * dpr), round(h * dpr)]
# another tab brought to the front: the view must end within this (seconds)
SWITCH_LIMIT = 0.1
# synth: a metric may get worse by this much (relative, or absolute below `floor`) before it fails
TOLERANCE = {'flicker': (0.15, 0.05), 'lag': (0.15, 0.1), 'error': (0.15, 0.1), 'warpMs': (0.5, 1.0), 'postMs': (0.5, 0.5)}
failures = []


def check(ok, what):
    print(('PASS ' if ok else 'FAIL ') + what)
    if not ok:
        failures.append(what)


def wait_port(port, timeout=15):
    end = time.time() + timeout
    while time.time() < end:
        try:
            socket.create_connection(('127.0.0.1', port), 0.5).close(); return True
        except OSError:
            time.sleep(0.2)
    return False


def pc(record):
    chrome = os.environ.get('CHROME', r'C:\Program Files\Google\Chrome\Application\chrome.exe')
    version = json.load(open(os.path.join(os.environ.get('FD_EXTENSION', os.path.join(ROOT, 'extension')), 'manifest.json')))['version']
    tmp = tempfile.mkdtemp(prefix='fd-regress-')
    log = os.path.join(tmp, 'mock.log')
    procs = [subprocess.Popen([sys.executable, '-I', '-m', 'http.server', '8731', '--bind', '127.0.0.1'], cwd=ROOT,
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL),
             subprocess.Popen([sys.executable, '-I', os.path.join(HERE, 'extension', 'mock_companion.py'), log, version])]
    got, geometry = {}, []
    switched = False
    try:
        wait_port(8731); time.sleep(1.5)   # the mock serves one connection at a time: no probing it
        runs = [(p, 1, 1) for p in PAGES] + [('switch', 1, 1)] + VARIANTS
        if os.environ.get('FD_ONLY'):   # a few pages only (comma separated), e.g. to try an older extension on them
            runs = [r for r in runs if r[0] in os.environ['FD_ONLY'].split(',')]
        for i, (page, dsf, zoom) in enumerate(runs):
            start = os.path.getsize(log) if os.path.exists(log) else 0
            prof = os.path.join(tmp, f'profile-{i}-' + page)
            c = subprocess.Popen([chrome, f'--user-data-dir={prof}', f'--remote-debugging-port={DEBUG_PORT}', '--enable-unsafe-extension-debugging',
                                  '--no-first-run', '--no-default-browser-check', '--lang=en-US', '--window-size=1600,1000',
                                  f'--force-device-scale-factor={dsf}', 'about:blank'])
            wait_port(DEBUG_PORT)
            time.sleep(1.5)
            # 'switch': the video page, then another tab in front 5 s in
            r = subprocess.run([sys.executable, '-I', os.path.join(HERE, 'extension', 'drive_chrome.py'), os.environ.get('FD_EXTENSION', os.path.join(ROOT, 'extension')),
                                f'http://127.0.0.1:8731/tests/pages/{"ui-detect.html" if page == "switch" else page}', '10' if page in SCROLLED else '8'],
                               capture_output=True, text=True,
                               env={**os.environ, 'FD_CLOSE': '1', **({'FD_SWITCH': '5'} if page == 'switch' else {}),
                                    **({'FD_SCROLL': SCROLLED[page]} if page in SCROLLED else {}),
                                    **({'FD_ZOOM': str(zoom)} if zoom != 1 else {})})
            try:
                c.wait(timeout=20)
            except subprocess.TimeoutExpired:
                # a Chrome that did not close kept the debugging port, and the
                # next page was driven in it: wrong results for every later page
                print('chrome did not close: killed it', page)
                c.kill(); c.wait(timeout=10); time.sleep(1)
            if os.environ.get('FD_SHOW'):   # the driver's own output (titles, the extension's log, popup)
                print(r.stdout)
            lines = [l for l in open(log, 'rb').read()[start:].decode('utf-8').splitlines() if l[:1].isdigit()]
            if page == 'switch':
                at = float(next(l for l in r.stdout.splitlines() if l.startswith('switched at')).split()[2])
                off = next((float(l.split(' ', 1)[0]) for l in lines
                            if float(l.split(' ', 1)[0]) >= at and json.loads(l.split(' ', 1)[1]) == {'type': 'view', 'on': False}), None)
                print('switch: view off', 'never' if off is None else f'{off - at:.3f} s after the other tab came')
                switched = off is not None and off - at < SWITCH_LIMIT
                continue
            msgs = [json.loads(l.split(' ', 1)[1]) for l in lines]
            last = lambda t: next((m for m in reversed(msgs) if m.get('type') == t), None)
            if page in EXPECT_CSS:
                top = next((json.loads(l.split(': ', 1)[1]) for l in r.stdout.splitlines() if l.startswith('top page: ')), None)
                rect = last('rect')
                gotr = [rect[k] for k in ('x', 'y', 'w', 'h')] if rect and rect.get('visible') else None
                want = expected_window(EXPECT_CSS[page], top, zoom) if top else None
                geometry.append((f'{page} scale {dsf} zoom {zoom}', gotr, want))
                print('geometry', page, dsf, zoom, gotr, 'expected', want)
            if (page, dsf, zoom) in VARIANTS:
                continue
            starts = sorted({l.split(' cs ', 1)[1] for l in r.stdout.splitlines() if ' cs ' in l and 'view start' in l})
            got[page] = {'viewsOn': sum(1 for m in msgs if m.get('type') == 'view' and m.get('on')), 'starts': starts,
                         'lastRect': last('rect'), 'onlyCount': len((last('only') or {}).get('rects', [])),
                         'uiCount': len((last('ui') or {}).get('rects', [])), 'warning': 'Version mismatch' in r.stdout}
            print(page, json.dumps(got[page]))
    finally:
        for p in procs:
            p.terminate()
        shutil.rmtree(tmp, ignore_errors=True)
    expect_file = os.path.join(ROOT, 'build', 'regress', 'expect_pages.json')
    ran = {r[0] for r in runs}
    if record:
        # merged into what was recorded: an FD_ONLY run keeps the other pages'
        os.makedirs(os.path.dirname(expect_file), exist_ok=True)
        old = json.load(open(expect_file)) if os.path.exists(expect_file) else {}
        json.dump({**old, **got}, open(expect_file, 'w'), indent=1)
        print('recorded', expect_file)
        return
    if 'switch' in ran:
        check(switched, f'pc view ends within {SWITCH_LIMIT} s of another tab coming to the front')
    for what, g, w in geometry:
        check(bool(g and w) and all(abs(a - b) <= 1 for a, b in zip(g, w)), f'pc {what} picture at {g} (worked out {w})')
    want = json.load(open(expect_file))
    for page in [p for p in PAGES if p in ran]:
        g, w = got.get(page, {}), want.get(page, {})
        for k in COMPARE[page]:
            check(g.get(k) == w.get(k), f'pc {page} {k}: {g.get(k)} (recorded {w.get(k)})')
        check(not g.get('warning'), f'pc {page} no version warning')


def headset():
    host, remote = os.environ['FD_HEADSET'], os.environ.get('FD_REMOTE', '~/src/framedepth')
    sh = lambda cmd: subprocess.run(['ssh', host, cmd], capture_output=True, text=True)
    # cd first, then only the companion in the background: $! is its PID
    r = sh(f'cd {remote} || exit 1; ./build/framedepth-companion --port 47399 --assets build-deps/moonlight-xr/assets > /tmp/fd-regress.log 2>&1 & '
           f'pid=$!; sleep 1; python3 -I tests/companion_client.py "" 47399; rc=$?; kill $pid; wait $pid 2>/dev/null; exit $rc')
    check(r.returncode == 0 and 'PASS' in r.stdout, 'headset protocol test')
    r = sh(f'cd {remote} && ./build/framedepth-companion --gputest /tmp/fd-gputest.ppm 2>&1; rm -f /tmp/fd-gputest.ppm')
    check(r.returncode == 0, 'headset --gputest ' + r.stdout.strip().splitlines()[-1] if r.stdout.strip() else 'headset --gputest')
    r = sh(f'cd {remote} && ./build/framedepth-companion --gputest /tmp/fd-gputest.ppm narrow 2>&1; rm -f /tmp/fd-gputest.ppm')
    check(r.returncode == 0, 'headset --gputest narrow (a 16x6000 sliver, then a normal drawing)')
    r = sh(f'set -e; d=$(mktemp -d); cd {remote}; cp models/zipdepth_npu_512x288.ncnn.param $d/; cp models/zipdepth_npu.ncnn.bin $d/zipdepth_npu_512x288.ncnn.bin; '
           f'ffmpeg -loglevel error -y -i ~/fd-synth/static.webm -frames:v 1 $d/p.ppm; '
           f'./build/framedepth-companion --depthcheck build-deps/moonlight-xr/assets/depth/zipdepth_512x288_fp16.tflite $d/zipdepth_npu_512x288.ncnn.param $d/p.ppm 2>&1 | grep correlation; rm -rf $d')
    corr = float(r.stdout.split('correlation')[1].split()[0]) if 'correlation' in r.stdout else 0
    check(corr > 0.99, f'headset CPU and GPU models agree (correlation {corr})')


def synth(spec):
    scenes, label, base = spec.split(':')
    r = subprocess.run([sys.executable, '-I', os.path.join(HERE, 'synth', 'run.py'), scenes, label])
    check(r.returncode == 0, 'synth run ' + label)
    if r.returncode or base == label:
        return
    d = lambda l: os.path.join(scenes, 'runs', l)
    for f in sorted(os.listdir(d(base))):
        if not f.endswith('.json') or f == 'summary.json' or not os.path.exists(os.path.join(d(label), f)):
            continue
        a, b = json.load(open(os.path.join(d(base), f))), json.load(open(os.path.join(d(label), f)))
        for k, (rel, floor) in TOLERANCE.items():
            if a.get(k) is None or b.get(k) is None:
                continue
            limit = max(a[k] * (1 + rel), a[k] + floor)
            check(b[k] <= limit, f'synth {f[:-5]} {k} {b[k]} (baseline {a[k]}, limit {limit:.3f})')


def live(scenes):
    host, remote = os.environ['FD_HEADSET'], os.environ.get('FD_REMOTE', '~/src/framedepth')
    subprocess.run(['ssh', host, 'mkdir -p ~/fd-live/www'], check=True)
    subprocess.run(['scp', '-q', os.path.join(ROOT, 'tests', 'pages', 'video-loop.html'), os.path.join(scenes, 'motion.webm'), f'{host}:fd-live/www/'], check=True)
    # Only this run's dumps count: run_live.sh refuses (and leaves an older
    # run's dumps in place) when the headset is worn or the folder is not its
    # own, so its exit status is checked and each dump must carry the
    # version just built (a refused run once passed on dumps hours old)
    r = subprocess.run(['ssh', host, f'cd {remote} && bash tests/live/run_live.sh ~/fd-live/www "video-loop.html?src=motion.webm" 14 ~/fd-live/regress 3 >/tmp/fd-live-regress.log 2>&1; '
                        f'rc=$?; echo "run_live exit $rc"; [ $rc = 0 ] || tail -n 3 /tmp/fd-live-regress.log; '
                        f'v=$(./build/framedepth-companion --version | cut -d" " -f2); '
                        f'n=0; for p in ~/fd-live/regress/dump_*_info.json; do [ $rc = 0 ] && grep -q "version.:.$v.," $p || continue; p=${{p%_info.json}}; n=$((n+1)); '
                        f'./build/framedepth-companion --rerender $p /tmp/fd-rr.ppm 2>&1 | grep rerender || echo "rerender failed $p"; done; rm -f /tmp/fd-rr.ppm; echo "dumps $n"'],
                       capture_output=True, text=True)
    print(r.stdout)
    n = int(r.stdout.split('dumps')[-1]) if 'dumps' in r.stdout else 0
    check('run_live exit 0' in r.stdout, 'live path ran (run_live.sh exit 0)')
    check(n >= 3, f'live path wrote {n} dumps of this build')
    # Zero-copy reads (0.6.28) draw from Chrome's whole window, the offline
    # drawing from the picture alone: the same texels at coordinates rounded
    # differently, so a few values at sharp edges differ by a few levels.
    # A real difference between the two ways of drawing changes thousands.
    over = [int(m) for m in re.findall(r'(\d+) values over 2', r.stdout)]
    worst = max(over) if over else -1
    check(n > 0 and len(over) == n and worst <= 100, f'live dumps drawn again offline match (values over 2: worst {worst} of about 9 million)')


if __name__ == '__main__':
    args = sys.argv[1:]
    record = '--record' in args
    for a in args:
        if a == 'pc': pc(record)
        elif a == 'headset': headset()
        elif a.startswith('synth='): synth(a[6:])
        elif a.startswith('live='): live(a[5:])
    print(f'{len(failures)} failed' if failures else 'all passed')
    sys.exit(1 if failures else 0)
