#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 FrameDepth contributors
"""Self-test for framedepth-companion without Chrome (SSH is enough).

  python3 -I tests/companion_client.py [EXTENSION_ID] [PORT]

Checks that another Origin is refused, connects as the extension, asks for a
view with a marker no window has and expects the refusal to name that marker
and ask for a retry, reads the stats, and checks that a second extension
connection is told the companion is busy while the first is served. The
companion serves one client: on the usual port the extension's own
connection would hold it, so test a second instance instead:
    framedepth-companion --port 47399 &  python3 -I tests/companion_client.py '' 47399
"""
import base64
import json
import os
import socket
import struct
import sys
import time

EXT = sys.argv[1] if len(sys.argv) > 1 and sys.argv[1] else 'bmhflpdnkcbkfooeloiabjnpinajmnfl'
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 47321


def connect(origin):
    s = socket.create_connection(('127.0.0.1', PORT), timeout=30)
    key = base64.b64encode(os.urandom(16)).decode()
    s.sendall((f'GET / HTTP/1.1\r\nHost: 127.0.0.1:{PORT}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n'
               f'Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\nOrigin: {origin}\r\n\r\n').encode())
    head = b''
    while b'\r\n\r\n' not in head:
        chunk = s.recv(1)   # no further: a message right after the handshake stays for recv()
        if not chunk:
            break
        head += chunk
    return s, head.split(b'\r\n', 1)[0].decode()


def send(s, obj):
    data = json.dumps(obj).encode()
    mask = os.urandom(4)
    n = len(data)
    h = bytes([0x81])
    h += bytes([0x80 | n]) if n < 126 else bytes([0x80 | 126]) + struct.pack('>H', n)
    s.sendall(h + mask + bytes(b ^ mask[i & 3] for i, b in enumerate(data)))


def recv(s):
    def exact(n):
        b = b''
        while len(b) < n:
            c = s.recv(n - len(b))
            if not c:
                raise EOFError
            b += c
        return b
    b0, b1 = exact(2)
    n = b1 & 0x7f
    if n == 126:
        n = struct.unpack('>H', exact(2))[0]
    elif n == 127:
        n = struct.unpack('>Q', exact(8))[0]
    return json.loads(exact(n))


def expect(s, kind, seconds):
    s.settimeout(seconds)
    end = time.time() + seconds
    while time.time() < end:
        m = recv(s)
        if m.get('type') == kind:
            return m
    raise TimeoutError(kind)


def main():
    ok = True
    s, status = connect('https://evil.example')
    print('foreign origin:', status)
    ok &= '403' in status
    s.close()
    s, status = connect('chrome-extension://' + EXT)
    print('extension origin:', status)
    ok &= '101' in status
    send(s, {'type': 'hello'})
    m = expect(s, 'ready', 40)
    print('hello ->', m)
    ok &= bool(m.get('version'))   # the popup compares it with the extension's
    send(s, {'type': 'settings', 'strength': 1.5, 'convergence': 50, 'smooth': 3, 'sharpen': 0, 'detail': 0})
    marker = '[FD:selftest]'   # no window has it
    send(s, {'type': 'view', 'on': True, 'marker': marker})
    m = expect(s, 'view', 15)
    print('view ->', m)
    ok &= m.get('ok') is False and m.get('retry') is True and m.get('marker') == marker
    send(s, {'type': 'view', 'on': False})
    send(s, {'type': 'stats'})
    m = expect(s, 'stats', 5)
    print('stats ->', m)
    ok &= m.get('problem') == ''
    # A second extension connection (another Chrome profile) while this one talks:
    # told the companion is busy and closed, this one still served
    s2, status = connect('chrome-extension://' + EXT)
    m = expect(s2, 'error', 5)
    print('second client ->', status, m)
    ok &= m.get('busy') is True
    s2.close()
    send(s, {'type': 'stats'})
    ok &= expect(s, 'stats', 5).get('type') == 'stats'
    # The same profile reconnecting (its service worker restarted): the old socket
    # closes as the new one comes, and the new one is served, not told busy
    s.close()
    s3, status = connect('chrome-extension://' + EXT)
    send(s3, {'type': 'hello'})
    m = expect(s3, 'ready', 10)
    print('reconnect at once ->', status, m)
    ok &= m.get('type') == 'ready'
    s3.close()
    print('PASS' if ok else 'FAIL')
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
