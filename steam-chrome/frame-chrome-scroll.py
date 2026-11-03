#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 FrameDepth contributors
"""Steam Frame: sends the laser's thumbstick scrolling on to Chrome again, as
wheel input Chrome accepts.

The problem:
  In Chrome started from Steam (Xwayland :1) the thumbstick's scrolling does
  reach Chrome's window, but the laser's movement (xwayland-pointer) and the
  scrolling (xwayland-relative-pointer) come from two different devices, so
  the pointer keeps switching between them (SlaveSwitch). Each switch makes
  Chrome start its scroll position over, and it drops nearly all of them.

What this does:
  It watches the "emulated" wheel buttons the X server makes from the scroll
  values (RawButtonPress, buttons 4 to 7) and, when the window under the
  pointer is one of the apps, sends that window a core ButtonPress/Release
  with XSendEvent.

  Not XTEST: Xwayland 24's XTEST goes through libei to gamescope and comes
  back on the same scroll device, an endless loop. XSendEvent does not pass
  the compositor and makes no XI2 raw events, so nothing comes back. The
  number sent per second is capped all the same.
"""

import argparse
import ctypes
import ctypes.util
import re
import sys
import time
from ctypes import (POINTER, Structure, Union, byref, c_char_p, c_double, c_int,
                    c_long, c_ubyte, c_uint, c_ulong, c_void_p)

GenericEvent = 35
ButtonPress, ButtonRelease = 4, 5
ButtonPressMask, ButtonReleaseMask = 1 << 2, 1 << 3
XI_RawButtonPress = 15
XIAllMasterDevices = 1
XIPointerEmulated = 1 << 16
WHEEL_BUTTONS = (4, 5, 6, 7)

DEFAULT_APPS = r"chrome|chromium|brave|microsoft-edge|vivaldi"


class XIEventMask(Structure):
    _fields_ = [("deviceid", c_int), ("mask_len", c_int), ("mask", POINTER(c_ubyte))]


class XGenericEventCookie(Structure):
    _fields_ = [("type", c_int), ("serial", c_ulong), ("send_event", c_int),
                ("display", c_void_p), ("extension", c_int), ("evtype", c_int),
                ("cookie", c_uint), ("data", c_void_p)]


class XButtonEvent(Structure):
    _fields_ = [("type", c_int), ("serial", c_ulong), ("send_event", c_int),
                ("display", c_void_p), ("window", c_ulong), ("root", c_ulong),
                ("subwindow", c_ulong), ("time", c_ulong), ("x", c_int), ("y", c_int),
                ("x_root", c_int), ("y_root", c_int), ("state", c_uint),
                ("button", c_uint), ("same_screen", c_int)]


class XEvent(Union):
    _fields_ = [("type", c_int), ("xcookie", XGenericEventCookie),
                ("xbutton", XButtonEvent), ("pad", c_long * 24)]


class XIValuatorState(Structure):
    _fields_ = [("mask_len", c_int), ("mask", POINTER(c_ubyte)), ("values", POINTER(c_double))]


class XIRawEvent(Structure):
    _fields_ = [("type", c_int), ("serial", c_ulong), ("send_event", c_int),
                ("display", c_void_p), ("extension", c_int), ("evtype", c_int),
                ("time", c_ulong), ("deviceid", c_int), ("sourceid", c_int),
                ("detail", c_int), ("flags", c_int), ("valuators", XIValuatorState),
                ("raw_values", POINTER(c_double))]


class XClassHint(Structure):
    # Not c_char_p: that hands back a Python bytes copy, and the pointer XFree needs is lost
    _fields_ = [("res_name", c_void_p), ("res_class", c_void_p)]


def load(name):
    return ctypes.CDLL(ctypes.util.find_library(name) or f"lib{name}.so.6")


x11, xi = load("X11"), load("Xi")
x11.XOpenDisplay.restype = c_void_p
x11.XOpenDisplay.argtypes = [c_char_p]
x11.XDefaultRootWindow.restype = c_ulong
x11.XDefaultRootWindow.argtypes = [c_void_p]
x11.XQueryExtension.argtypes = [c_void_p, c_char_p, POINTER(c_int), POINTER(c_int), POINTER(c_int)]
x11.XNextEvent.argtypes = [c_void_p, POINTER(XEvent)]
x11.XGetEventData.argtypes = [c_void_p, POINTER(XGenericEventCookie)]
x11.XFreeEventData.argtypes = [c_void_p, POINTER(XGenericEventCookie)]
x11.XFlush.argtypes = [c_void_p]
x11.XQueryPointer.argtypes = [c_void_p, c_ulong, POINTER(c_ulong), POINTER(c_ulong),
                              POINTER(c_int), POINTER(c_int), POINTER(c_int), POINTER(c_int),
                              POINTER(c_uint)]
x11.XGetClassHint.argtypes = [c_void_p, c_ulong, POINTER(XClassHint)]
x11.XFree.argtypes = [c_void_p]
x11.XSendEvent.argtypes = [c_void_p, c_ulong, c_int, c_long, POINTER(XEvent)]
x11.XSetErrorHandler.argtypes = [c_void_p]
xi.XIQueryVersion.argtypes = [c_void_p, POINTER(c_int), POINTER(c_int)]
xi.XISelectEvents.argtypes = [c_void_p, c_ulong, POINTER(XIEventMask), c_int]

# X errors are ignored, so a BadWindow from a window closing just then does not end the process
_XErrorHandler = ctypes.CFUNCTYPE(c_int, c_void_p, c_void_p)
_ignore_x_errors = _XErrorHandler(lambda dpy, err: 0)
x11.XSetErrorHandler(ctypes.cast(_ignore_x_errors, c_void_p))


def log(msg):
    print(f"frame-chrome-scroll: {msg}", flush=True)


class RateLimit:
    """More than max_per_sec in a second stops the sending for cooldown seconds."""

    def __init__(self, max_per_sec, cooldown=3.0):
        self.max = max_per_sec
        self.cooldown = cooldown
        self.window_start = 0.0
        self.count = 0
        self.paused_until = 0.0

    def allow(self, n):
        now = time.monotonic()
        if now < self.paused_until:
            return False
        if now - self.window_start >= 1.0:
            self.window_start, self.count = now, 0
        self.count += n
        if self.count > self.max:
            self.paused_until = now + self.cooldown
            log(f"{self.count} in a second, more than {self.max}: pausing for {self.cooldown:.0f} s")
            return False
        return True


class Forwarder:
    def __init__(self, display, app_pattern, repeat, max_rate, verbose):
        self.dpy = x11.XOpenDisplay(display.encode())
        if not self.dpy:
            raise RuntimeError(f"cannot connect to display {display}")
        self.display = display
        self.root = x11.XDefaultRootWindow(self.dpy)
        self.app_re = re.compile(app_pattern, re.I) if app_pattern else None
        self.repeat = repeat
        self.limit = RateLimit(max_rate)
        self.verbose = verbose
        self.class_cache = {}

        opcode, ev, err = c_int(), c_int(), c_int()
        if not x11.XQueryExtension(self.dpy, b"XInputExtension", byref(opcode), byref(ev), byref(err)):
            raise RuntimeError("no XInput extension")
        self.xi_opcode = opcode.value
        major, minor = c_int(2), c_int(2)
        xi.XIQueryVersion(self.dpy, byref(major), byref(minor))

        mask = (c_ubyte * 4)()
        mask[XI_RawButtonPress >> 3] |= 1 << (XI_RawButtonPress & 7)
        evmask = XIEventMask(XIAllMasterDevices, len(mask), mask)
        xi.XISelectEvents(self.dpy, self.root, byref(evmask), 1)
        x11.XFlush(self.dpy)

    def pointer_target(self):
        """The top-level window under the pointer, the pointer in that window and on the root."""
        root_ret, child = c_ulong(), c_ulong()
        rx, ry, wx, wy, m = c_int(), c_int(), c_int(), c_int(), c_uint()
        x11.XQueryPointer(self.dpy, self.root, byref(root_ret), byref(child),
                          byref(rx), byref(ry), byref(wx), byref(wy), byref(m))
        wid = child.value
        if not wid:
            return 0, 0, 0, 0, 0
        # The position in the window comes from asking that window (gamescope's are usually at +0+0)
        x11.XQueryPointer(self.dpy, wid, byref(root_ret), byref(child),
                          byref(rx), byref(ry), byref(wx), byref(wy), byref(m))
        return wid, wx.value, wy.value, rx.value, ry.value

    def window_class(self, wid):
        if wid not in self.class_cache:
            hint = XClassHint()
            name = ""
            if x11.XGetClassHint(self.dpy, wid, byref(hint)):
                ptrs = [p for p in (hint.res_name, hint.res_class) if p]
                name = " ".join(ctypes.string_at(p).decode(errors="replace") for p in ptrs)
                for p in ptrs:
                    x11.XFree(p)
            self.class_cache[wid] = name
        return self.class_cache[wid]

    def send_wheel(self, wid, button, x, y, x_root, y_root):
        for typ, evmask in ((ButtonPress, ButtonPressMask), (ButtonRelease, ButtonReleaseMask)):
            e = XEvent()
            b = e.xbutton
            b.type, b.send_event, b.display = typ, 1, self.dpy
            b.window, b.root = wid, self.root
            b.x, b.y, b.x_root, b.y_root = x, y, x_root, y_root
            b.button, b.same_screen = button, 1
            x11.XSendEvent(self.dpy, wid, 1, evmask, byref(e))

    def run(self):
        log(f"watching {self.display} (apps: {self.app_re.pattern if self.app_re else 'all'}, repeat {self.repeat})")
        event = XEvent()
        while True:
            x11.XNextEvent(self.dpy, byref(event))
            cookie = event.xcookie
            if cookie.type != GenericEvent or cookie.extension != self.xi_opcode:
                continue
            if not x11.XGetEventData(self.dpy, byref(cookie)):
                continue
            try:
                if cookie.evtype != XI_RawButtonPress:
                    continue
                raw = ctypes.cast(cookie.data, POINTER(XIRawEvent)).contents
                button, emulated = raw.detail, bool(raw.flags & XIPointerEmulated)
            finally:
                x11.XFreeEventData(self.dpy, byref(cookie))

            if button not in WHEEL_BUTTONS or not emulated:
                continue
            wid, x, y, x_root, y_root = self.pointer_target()
            if not wid:
                continue
            cls = self.window_class(wid)
            if self.app_re and not self.app_re.search(cls):
                continue
            if not self.limit.allow(self.repeat):
                continue
            for _ in range(self.repeat):
                self.send_wheel(wid, button, x, y, x_root, y_root)
            x11.XFlush(self.dpy)
            if self.verbose:
                log(f"wheel {button} x{self.repeat} -> {cls} ({x},{y})")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--display", default=":1", help="the X display to watch (default :1, where apps started from Steam show)")
    ap.add_argument("--apps", default=DEFAULT_APPS,
                    help=f"regular expression for the windows' WM_CLASS (default '{DEFAULT_APPS}', empty for every app)")
    ap.add_argument("--repeat", type=int, default=1, help="wheel clicks sent per thumbstick step (scroll speed, default 1)")
    ap.add_argument("--max-rate", type=int, default=60, help="most wheel clicks per second, more pauses for 3 s (default 60)")
    ap.add_argument("-v", "--verbose", action="store_true", help="log every wheel click sent")
    args = ap.parse_args()

    # Until the display is there, try again. When the connection to it is
    # lost, Xlib ends the process.
    while True:
        try:
            Forwarder(args.display, args.apps, max(1, args.repeat), args.max_rate, args.verbose).run()
        except RuntimeError as e:
            log(f"{e}; trying again in 5 s")
            time.sleep(5)


if __name__ == "__main__":
    sys.exit(main())
