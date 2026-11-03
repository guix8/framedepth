#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 FrameDepth contributors
# Chrome from Steam's library (a non-Steam game) runs this as its target, with
# the Chrome command (flatpak run ... com.google.Chrome @@u @@) as the launch
# options. It starts the scroll helper, starts Chrome with that command, and
# stops the helper when Chrome ends.

DIR="$(dirname "$(readlink -f "$0")")"

# Added to Chrome's arguments. gamescope opens an app from Steam at 1280x720:
# this makes the window the output's 1920x1080, with a scale of 1.5 so text
# is as large as at 720p but drawn with 1.5 times the pixels.
# --window-size is the size before the scale, so 1280x720 gives a 1920x1080
# window.
CHROME_EXTRA_ARGS=(
    --window-size=1280,720
    --window-position=0,0
    --force-device-scale-factor=1.5
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
