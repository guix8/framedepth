# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 FrameDepth contributors
# A stand-in for framedepth-companion on a PC (127.0.0.1:47321), for testing
# the extension without a headset: answers hello -> ready and view on -> ok
# (with the marker) and logs every JSON message it gets to LOG. VERSION is
# the version it reports in ready (none, like a companion before 0.4.1, if left out).
#   python -I mock_companion.py LOG [VERSION]
import base64, hashlib, json, socket, struct, sys, time

LOG = sys.argv[1]
VERSION = sys.argv[2] if len(sys.argv) > 2 else None
ORIGIN = 'chrome-extension://bmhflpdnkcbkfooeloiabjnpinajmnfl'
ls = socket.socket(); ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
ls.bind(('127.0.0.1', 47321)); ls.listen(4)
log = open(LOG, 'a', encoding='utf-8')

def frame(s, text):
    d = text.encode(); n = len(d)
    s.sendall(bytes([0x81]) + (bytes([n]) if n < 126 else bytes([126]) + struct.pack('>H', n)) + d)

def serve(c):
    buf = b''
    while b'\r\n\r\n' not in buf:
        r = c.recv(4096)
        if not r: return   # closed before a whole request (a port probe)
        buf += r
    head, rest = buf.split(b'\r\n\r\n', 1)
    hdr = {l.split(':', 1)[0].lower(): l.split(':', 1)[1].strip() for l in head.decode().split('\r\n')[1:] if ':' in l}
    if hdr.get('origin') != ORIGIN:
        c.sendall(b'HTTP/1.1 403 Forbidden\r\n\r\n'); return
    acc = base64.b64encode(hashlib.sha1((hdr['sec-websocket-key'] + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest()).decode()
    c.sendall(f'HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: {acc}\r\n\r\n'.encode())
    data = rest
    def need(n):
        nonlocal data
        while len(data) < n:
            r = c.recv(65536)
            if not r: raise EOFError
            data += r
        out, data = data[:n], data[n:]
        return out
    video_fps = 0   # the last videofps, given back in stats
    while True:
        b0, b1 = need(2); n = b1 & 0x7f
        if n == 126: n = struct.unpack('>H', need(2))[0]
        elif n == 127: n = struct.unpack('>Q', need(8))[0]
        mask = need(4) if b1 & 0x80 else b'\0\0\0\0'
        p = bytes(x ^ mask[i % 4] for i, x in enumerate(need(n)))
        if b0 & 0x0f == 8: return
        if b0 & 0x0f != 1: continue
        m = json.loads(p)
        log.write(f'{time.time():.3f} {json.dumps(m)}\n'); log.flush()
        t = m.get('type')
        if t == 'hello': frame(c, json.dumps({'type': 'ready', 'model': 'mock', **({'version': VERSION} if VERSION else {})}))
        elif t == 'view' and m.get('on'): time.sleep(0.3); frame(c, json.dumps({'type': 'view', 'ok': True, 'marker': m.get('marker')}))
        elif t == 'videofps': video_fps = m.get('fps', 0)
        elif t == 'stats': frame(c, json.dumps({'type': 'stats', 'fps': 0, 'frameFps': 0, 'videoFps': video_fps, 'grabMs': 0, 'warpMs': 0, 'mapsPerSecond': 0, 'modelMs': 0, 'ageMs': 0}))

while True:
    c, _ = ls.accept()
    try: serve(c)
    except Exception as e: log.write(f'client gone: {e}\n'); log.flush()
    finally: c.close()
