#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 FrameDepth contributors
"""Depth detail checks for framedepth-companion without Chrome (SSH is enough).

  python3 -I tests/companion_detail.py PORT DETAIL MODEL [REFUSED]

Connects as the extension with a hello carrying DETAIL and expects the ready
answer to name MODEL (and, given, refused = REFUSED: that detail's own model
did not start). While the model loads, a stats request must be answered
first: the load runs on its own thread. Run it against a second instance with
an assets folder that has the models the case needs:
    framedepth-companion --port 47399 --assets DIR &  python3 -I tests/companion_detail.py 47399 2 'ZipDepth 672x384 CPU'
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import companion_client as c   # noqa: E402  (its own test runs only as a script)

c.PORT = int(sys.argv[1])
c.EXT = 'bmhflpdnkcbkfooeloiabjnpinajmnfl'   # it read our arguments as its own
detail, model = int(sys.argv[2]), sys.argv[3]
refused = int(sys.argv[4]) if len(sys.argv) > 4 else None

s, status = c.connect('chrome-extension://' + c.EXT)
assert '101' in status, status
t0 = time.monotonic()
c.send(s, {'type': 'hello', 'detail': detail})
c.send(s, {'type': 'stats'})
first = c.recv(s)
order = [first['type']]
ready = first
while ready['type'] != 'ready':
    ready = c.recv(s)
    order.append(ready['type'])
took = time.monotonic() - t0
print('answers in order:', order, f'ready after {took:.1f} s ->', ready)
ok = order[0] == 'stats' and ready.get('model') == model and ready.get('refused') == refused
print('PASS' if ok else 'FAIL')
sys.exit(0 if ok else 1)
