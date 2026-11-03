# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 FrameDepth contributors
# Drives a throwaway Chrome (own profile, started with
# --remote-debugging-port=9334, or FD_DEBUG_PORT, --enable-unsafe-extension-debugging) with the
# extension and a test page, then prints the titles seen, the extension's
# diagnostic lines and the popup's status text. Use with mock_companion.py and a local web server for
# tests/pages (python -m http.server 8731 from the repository root):
#   python -I drive_chrome.py EXT_DIR URL SECONDS
# With FD_CLOSE=1 in the environment it closes that Chrome at the end; with
# FD_SCROLL=T1,T2,... it turns the mouse wheel (300 px down) that many seconds in;
# with FD_SWITCH=T it opens another tab in front T seconds in and prints when
# ('switched at' a time.time() to set against mock_companion.py's log);
# with FD_OFF=1 it leaves the extension off; with FD_ZOOM=Z it sets Chrome's
# page zoom (as Ctrl+plus does) to Z for the test page first.
import base64, json, os, socket, struct, sys, time, urllib.request
EXT_DIR, URL, SECONDS = sys.argv[1], sys.argv[2], float(sys.argv[3])
class WS:
    def __init__(self, url):
        host_port, path = url[len('ws://'):].split('/', 1)
        host, port = host_port.split(':')
        self.s = socket.create_connection((host, int(port)), timeout=30)
        key = base64.b64encode(os.urandom(16)).decode()
        self.s.sendall((f'GET /{path} HTTP/1.1\r\nHost: {host_port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n'
                        f'Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n').encode())
        buf = b''
        while b'\r\n\r\n' not in buf:
            buf += self.s.recv(4096)
        self.rest = buf.split(b'\r\n\r\n', 1)[1]

    def _read(self, n):
        while len(self.rest) < n:
            c = self.s.recv(1 << 16)
            if not c:
                raise EOFError
            self.rest += c
        out, self.rest = self.rest[:n], self.rest[n:]
        return out

    def send(self, text):
        data = text.encode()
        mask = os.urandom(4)
        n = len(data)
        head = bytes([0x81]) + (bytes([0x80 | n]) if n < 126 else bytes([0x80 | 126]) + struct.pack('>H', n) if n < 65536
                                else bytes([0x80 | 127]) + struct.pack('>Q', n))
        self.s.sendall(head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))

    def recv(self):
        msg = b''
        while True:
            b0, b1 = self._read(2)
            n = b1 & 0x7f
            if n == 126:
                n = struct.unpack('>H', self._read(2))[0]
            elif n == 127:
                n = struct.unpack('>Q', self._read(8))[0]
            msg += self._read(n)
            if b0 & 0x80:
                return json.loads(msg)



version = json.load(urllib.request.urlopen(f"http://127.0.0.1:{os.environ.get('FD_DEBUG_PORT', '9334')}/json/version"))
ws = WS(version['webSocketDebuggerUrl'])
nid = 0
def call(method, params=None, session=None):
    global nid
    nid += 1
    m = {'id': nid, 'method': method, 'params': params or {}}
    if session: m['sessionId'] = session
    ws.send(json.dumps(m))
    while True:
        r = ws.recv()
        if r.get('id') == nid:
            if 'error' in r: raise RuntimeError(method + ': ' + str(r['error']))
            return r.get('result', {})
ext = call('Extensions.loadUnpacked', {'path': EXT_DIR})['id']
call('Target.setDiscoverTargets', {'discover': True}); time.sleep(1.5)
sw = next(t for t in call('Target.getTargets')['targetInfos'] if t['type'] == 'service_worker' and ext in t['url'])
sws = call('Target.attachToTarget', {'targetId': sw['targetId'], 'flatten': True})['sessionId']
# FD_OFF=1: the extension stays off (a run to compare with, nothing read)
call('Runtime.evaluate', {'expression': f"chrome.storage.local.set({{enabled: {'false' if os.environ.get('FD_OFF') == '1' else 'true'}}})", 'awaitPromise': True}, session=sws)
page = call('Target.createTarget', {'url': URL})['targetId']
ps = call('Target.attachToTarget', {'targetId': page, 'flatten': True})['sessionId']
if os.environ.get('FD_ZOOM'):
    # once the page is there: zoom is kept per site, and set while the tab
    # was still blank it went to no site and the page stayed at 100%
    for _ in range(50):
        r = call('Runtime.evaluate', {'expression': 'location.href.startsWith("http") && document.readyState !== "loading"', 'returnByValue': True}, session=ps)
        if r['result'].get('value'): break
        time.sleep(0.1)
    # every tab: the extension sees no tab URLs (no tabs permission)
    call('Runtime.evaluate', {'expression': f"chrome.tabs.query({{}}).then(ts => Promise.all(ts.map(t => chrome.tabs.setZoom(t.id, {float(os.environ['FD_ZOOM'])}).catch(() => {{}}))))",
                              'awaitPromise': True}, session=sws)
titles = set(); start = time.time(); end = start + SECONDS
scrolls = sorted(float(t) for t in os.environ.get('FD_SCROLL', '').split(',') if t)
switch = float(os.environ['FD_SWITCH']) if os.environ.get('FD_SWITCH') else None
while time.time() < end:
    if switch is not None and time.time() - start >= switch:
        switch = None
        print(f'switched at {time.time():.3f}')
        call('Target.createTarget', {'url': 'about:blank'})   # opens in front of the test page
    if scrolls and time.time() - start >= scrolls[0]:
        scrolls.pop(0)   # a real wheel turn in the middle of the page, as a mouse or the VR laser gives
        call('Input.dispatchMouseEvent', {'type': 'mouseWheel', 'x': 400, 'y': 300, 'deltaX': 0, 'deltaY': 300}, session=ps)
    r = call('Runtime.evaluate', {'expression': 'document.title', 'returnByValue': True}, session=ps)
    titles.add(r['result']['value']); time.sleep(0.1)
r = call('Runtime.evaluate', {'expression': 'JSON.stringify({dpr: devicePixelRatio, ct: outerHeight - innerHeight, cl: (outerWidth - innerWidth) / 2, '
                              'ow: outerWidth, oh: outerHeight, iw: innerWidth, ih: innerHeight, title: document.title, '
                              'video: (q => q && {decoded: q.totalVideoFrames, dropped: q.droppedVideoFrames})(document.querySelector("video")?.getVideoPlaybackQuality()), '
                              'presented: window.fdPresented?.()})', 'returnByValue': True}, session=ps)
print('top page:', r['result']['value'])
print('titles seen:', sorted(titles))
r = call('Runtime.evaluate', {'expression': 'chrome.storage.local.get("diag")', 'awaitPromise': True, 'returnByValue': True}, session=sws)
print('diag:'); [print('  ', l) for l in r['result']['value'].get('diag', [])]
popup = call('Target.createTarget', {'url': f'chrome-extension://{ext}/popup.html'})['targetId']
pps = call('Target.attachToTarget', {'targetId': popup, 'flatten': True})['sessionId']
time.sleep(1.5)
r = call('Runtime.evaluate', {'expression': 'document.getElementById("status").innerText', 'returnByValue': True}, session=pps)
print('popup status:'); [print('  ', l) for l in r['result']['value'].splitlines()]
if os.environ.get('FD_CLOSE') == '1':
    try: call('Browser.close')
    except Exception: pass   # Chrome may close the connection before it answers
