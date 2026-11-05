#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 FrameDepth contributors
# Chrome from Steam's library (a non-Steam game) runs this as its target, with
# the Chrome command (flatpak run ... com.google.Chrome @@u @@) as the launch
# options. It starts the scroll helper, starts Chrome with that command, and
# stops the helper when Chrome ends.

DIR="$(dirname "$(readlink -f "$0")")"

# Added to Chrome's arguments: a window as large as the screen gamescope
# gives the app, with text as large as at 720p. Chrome opens at 1280x720,
# scale 1. On a 1920x1080 screen (SteamOS 0.4) that makes a 1920x1080 window
# at scale 1.5: the same text drawn with 1.5 times the pixels. The window
# must not be larger than the screen: the pointer stays inside the screen,
# and on the 1280x720 screen of the SteamOS 0.5 beta a 1920x1080 window left
# a third of it out of the laser's reach (2026-11-05); there it stays at
# scale 1. --window-size is the size before the scale.
scale=1.5
height=$(xdpyinfo -display "${DISPLAY:-:1}" 2>/dev/null | sed -n 's/^ *dimensions: *[0-9]*x\([0-9]*\) pixels.*/\1/p')
if [ -n "$height" ]; then
    scale=$(awk -v h="$height" 'BEGIN { s = h / 720; if (s < 1) s = 1; printf "%.4g", s }')
fi
CHROME_EXTRA_ARGS=(
    --window-size=1280,720
    --window-position=0,0
    --force-device-scale-factor="$scale"
)

# Right after the Flatpak app ID: the @@u @@ that follow are Flatpak's file
# forwarding marks and keep their place.
cmd=()
for arg in "$@"; do
    cmd+=("$arg")
    [ "$arg" = "com.google.Chrome" ] && cmd+=("${CHROME_EXTRA_ARGS[@]}")
done

python3 -I "$DIR/frame-chrome-scroll.py" --display "${DISPLAY:-:1}" &
helper=$!

"${cmd[@]}" &
chrome=$!

# Stop in Steam sends this script TERM: pass it on to Chrome.
trap 'kill "$chrome" 2>/dev/null' TERM INT HUP
trap 'kill "$helper" 2>/dev/null' EXIT

# When a trap wakes wait, wait again until Chrome has really ended.
while kill -0 "$chrome" 2>/dev/null; do
    wait "$chrome"
done
