# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 FrameDepth contributors
# Runs the companion's offline pipeline (--pipeline) on the headset for
# synthetic scenes (make_scenes.py), pulls the results back and computes the
# numbers (metrics.py). Nothing is shown on the headset's screen.
#   FD_HEADSET=user@host python -I run.py SCENES_DIR LABEL [SCENE ...] [key=value ...]
#   python -I run.py --compare SCENES_DIR LABEL_A LABEL_B
# key=value goes to --pipeline (strength=2 tau=0 ...), plus model=cpu|gpu512|gpu768.
# FD_REMOTE: the source tree on the headset, built (default ~/src/framedepth).
# Before each scene it waits while someone wears the headset (the run takes
# its CPU and GPU), checking every minute.
# Results: SCENES_DIR/runs/LABEL/SCENE/ (maps, SBS frames) and SCENE.json, summary.json.
import json, os, subprocess, sys, tarfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import metrics  # noqa: E402

MODELS = {
    'cpu': 'build-deps/moonlight-xr/assets/depth/zipdepth_512x288_fp16.tflite',
    'gpu512': 'zipdepth_npu_512x288.ncnn.param',
    'gpu768': 'zipdepth_npu_768x448.ncnn.param',
}
COLUMNS = ['flicker', 'lag', 'error', 'depthRange', 'flickerRel', 'lagRel', 'errorRel', 'cutFrames', 'drift1s', 'drift1sShift', 'drift3s', 'drift3sShift', 'cutFill', 'cutFillWorst', 'modelVsTrue',
           'barEdgePx', 'barWrong', 'stereoWrong', 'parallaxDeg', 'edgeOut', 'convStep', 'streaks', 'sharpness', 'sharpX', 'sharpY', 'subRecall', 'subFalse', 'modelMs', 'postMs', 'warpMs', 'warpMaxMs']


def ssh(host, cmd, **kw):
    return subprocess.run(['ssh', host, cmd], check=True, **kw)


def wait_idle(host, remote):
    said = False
    while subprocess.run(['ssh', host, f'{remote}/build/framedepth-companion --worn'], capture_output=True).returncode == 0:
        if not said:
            print('the headset is being worn: waiting', flush=True); said = True
        time.sleep(60)


def run_scene(host, remote, scenes, label, name, settings, model):
    wait_idle(host, remote)
    meta = json.load(open(os.path.join(scenes, name + '.json')))
    w, h = meta['size']
    local = os.path.join(scenes, name + '.webm')
    size = os.path.getsize(local)
    have = subprocess.run(['ssh', host, f'stat -c %s ~/fd-synth/{name}.webm 2>/dev/null || echo 0'],
                          capture_output=True, text=True).stdout.strip()
    if have != str(size):
        ssh(host, 'mkdir -p ~/fd-synth')
        subprocess.run(['scp', '-q', local, f'{host}:fd-synth/{name}.webm'], check=True)
    out = f'~/fd-runs/{label}/{name}'
    if model.startswith('gpu'):
        # ncnn wants NAME.bin beside NAME.param; models/ shares one .bin
        prep = (f'mkdir -p ~/fd-runs/models && cp -f {remote}/models/{MODELS[model]} ~/fd-runs/models/ && '
                f'ln -sf {remote}/models/zipdepth_npu.ncnn.bin ~/fd-runs/models/{MODELS[model][:-6]}.bin && ')
        path = f'~/fd-runs/models/{MODELS[model]}'
    else:
        prep, path = '', f'{remote}/{MODELS[model]}'
    args = ' '.join(f'{k}={v}' for k, v in settings.items())
    ssh(host, f'set -o pipefail; {prep}rm -rf {out} && mkdir -p {out} && cd {remote} && '
              f'ffmpeg -loglevel error -i ~/fd-synth/{name}.webm -f rawvideo -pix_fmt bgra - | '
              f'./build/framedepth-companion --pipeline {path} {w}x{h} {out} oracle=1 every=30 {args} 2>&1 | grep -v "^INFO"')
    dest = os.path.join(scenes, 'runs', label, name)
    os.makedirs(dest, exist_ok=True)
    p = subprocess.Popen(['ssh', host, f'tar -C {out} -czf - . && rm -rf {out}'], stdout=subprocess.PIPE)
    with tarfile.open(fileobj=p.stdout, mode='r|gz') as t:
        t.extractall(dest)
    p.wait()
    m = metrics.scene_metrics(scenes, dest, name)
    json.dump(m, open(os.path.join(scenes, 'runs', label, name + '.json'), 'w'), indent=1)
    return m


def table(rows):
    print('scene'.ljust(11) + ''.join(c.rjust(12) for c in COLUMNS))
    for r in rows:
        print(r['scene'].ljust(11) + ''.join(str(r.get(c, '')).rjust(12) for c in COLUMNS))


def load(scenes, label):
    d = os.path.join(scenes, 'runs', label)
    return {f[:-5]: json.load(open(os.path.join(d, f))) for f in sorted(os.listdir(d))
            if f.endswith('.json') and f != 'summary.json'}


if __name__ == '__main__':
    if sys.argv[1] == '--recompute':
        # the numbers again from a run's saved files (after metrics.py changed)
        scenes, label = sys.argv[2:4]
        d = os.path.join(scenes, 'runs', label)
        rows = []
        for name in sorted(f for f in os.listdir(d) if os.path.isdir(os.path.join(d, f))):
            rows.append(metrics.scene_metrics(scenes, os.path.join(d, name), name))
            json.dump(rows[-1], open(os.path.join(d, name + '.json'), 'w'), indent=1)
        table(rows)
        sys.exit(0)
    if sys.argv[1] == '--compare':
        scenes, a, b = sys.argv[2:5]
        A, B = load(scenes, a), load(scenes, b)
        for name in A:
            if name in B:
                print(name)
                for c in COLUMNS:
                    if c in A[name] or c in B[name]:
                        print(f'  {c:12} {str(A[name].get(c)):>14} {str(B[name].get(c)):>14}')
        sys.exit(0)
    scenes, label = sys.argv[1], sys.argv[2]
    names = [a for a in sys.argv[3:] if '=' not in a]
    settings = dict(a.split('=', 1) for a in sys.argv[3:] if '=' in a)
    model = settings.pop('model', 'cpu')
    host = os.environ['FD_HEADSET']
    remote = os.environ.get('FD_REMOTE', '~/src/framedepth')
    if not names:
        names = sorted(f[:-5] for f in os.listdir(scenes) if f.endswith('.json'))
    rows = []
    for name in names:
        rows.append(run_scene(host, remote, scenes, label, name, settings, model))
        print(json.dumps(rows[-1]))
    json.dump({'label': label, 'model': model, 'settings': settings, 'scenes': rows},
              open(os.path.join(scenes, 'runs', label, 'summary.json'), 'w'), indent=1)
    table(rows)
