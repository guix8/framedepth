#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 FrameDepth contributors
# The whole live path on the headset with nobody wearing it: a throwaway
# Flatpak Chrome (its own profile, nothing of the user's) loads a copy of the
# extension that talks to a second companion on port 47399, opens PAGE from
# WWW and plays it for SECONDS while that companion writes what it hands
# SteamVR every DUMP_EVERY seconds (--dump) into OUT. SteamVR shows a desktop
# window only when it is brought up in its dashboard: the test window is
# brought up there, and the window shown before is brought back at the end.
# The Chrome window and the 3D overlay do show in the headset. Run from the
# built source tree:
#   bash tests/live/run_live.sh WWW PAGE SECONDS OUT [DUMP_EVERY]
# PAGE is a path under WWW with its query (video-loop.html?src=static.webm).
# FD_DETAIL sets the depth detail the test extension starts with (0, 1, 2).
# FD_CHROME_ARGS replaces the window size (--window-size=1600,1000), e.g.
# "--window-size=1280,720 --force-device-scale-factor=1.5" as Steam's shortcut.
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
www=$1 page=$2 seconds=$3 out=$4 every=${5:-2}
# Never while someone wears the headset: the test takes over the dashboard
if "$root/build/framedepth-companion" --worn >/dev/null 2>&1 && [[ ${FD_ALLOW_WORN:-} != 1 ]]; then
  echo 'The headset is being worn: not running the live test (FD_ALLOW_WORN=1 to run anyway).' >&2; exit 3
fi
ext="$HOME/Downloads/fd-test-ext"           # Flatpak Chrome may read Downloads
profile="$HOME/.var/app/com.google.Chrome/fd-test-profile"
assets="$HOME/.local/share/framedepth/assets"
vrcmd=/opt/steamvr/bin/linuxarm64/vrcmd
export DISPLAY=${DISPLAY:-:0}
shown_window() { "$vrcmd" --overlays 2>/dev/null | grep -E "^'gamescope\.[^']*\.window\.[0-9]+' .* visible VROverlayType_Dashboard_Main" | grep "${1:-}" | head -n 1 | cut -d"'" -f2; }
before=$(shown_window || true)
# OUT is replaced only when this script made it (its mark inside)
if [[ -e $out && ! -f $out/.fd-live-out ]]; then echo "$out exists and is not a run_live.sh output; give a new folder" >&2; exit 1; fi
rm -rf -- "$ext" "$profile" "$out"
mkdir -p "$out"
touch "$out/.fd-live-out"
cp -r "$root/extension" "$ext"
sed -i 's/127\.0\.0\.1:47321/127.0.0.1:47399/' "$ext/sw.js" "$ext/manifest.json"
[[ -z ${FD_DETAIL:-} ]] || sed -i "s/detail: 0,/detail: $FD_DETAIL,/" "$ext/content.js"
pids=()
cleanup() {
  for p in "${pids[@]}"; do kill "$p" 2>/dev/null || true; done
  wait 2>/dev/null || true
  # Right after the test window closes SteamVR may ignore the request: wait,
  # check, and ask again (opening the dashboard first)
  for _ in 1 2 3 4; do
    [[ -z $before || -n $(shown_window "'$before'") ]] && break
    sleep 2
    "$vrcmd" --showdashboard >/dev/null 2>&1 || true
    "$vrcmd" --showdashboard "$before" >/dev/null 2>&1 || true
  done
  [[ -z $before || -n $(shown_window "'$before'") ]] || echo "could not show $before in VR again" >&2
  rm -rf -- "$ext" "$profile"
}
trap cleanup EXIT
trap 'exit 1' INT TERM HUP PIPE   # through cleanup as well
# another program on our ports would take the test's place unnoticed
for port in 8731 9334 47399; do
  if (exec 3<>"/dev/tcp/127.0.0.1/$port") 2>/dev/null; then echo "port $port is in use: stop what holds it first" >&2; exit 1; fi
done
python3 -I -m http.server 8731 --bind 127.0.0.1 -d "$www" >/dev/null 2>&1 & pids+=($!)
"$root/build/framedepth-companion" --port 47399 --assets "$assets" --dump "$out" "$every" > "$out/companion.log" 2>&1 & pids+=($!)
flatpak run com.google.Chrome --user-data-dir="$profile" --remote-debugging-port=9334 --enable-unsafe-extension-debugging \
  --no-first-run --no-default-browser-check ${FD_CHROME_ARGS:---window-size=1600,1000} about:blank > "$out/chrome.log" 2>&1 & pids+=($!)
for _ in $(seq 50); do curl -s http://127.0.0.1:9334/json/version >/dev/null && break; sleep 0.2; done
# bring the test page's window up once its title is there, and again whenever
# it goes: with nobody wearing the headset SteamVR's standby closes the
# dashboard after a few seconds (2026-10-28)
( set +e   # a failed vrcmd must not end this loop (set -e reaches into the subshell)
  key=
  for _ in $(seq 40); do
    key=$("$vrcmd" --overlays 2>/dev/null | grep -E "^'gamescope\.[^']*\.window\.[0-9]+' -- '.*FrameDepth [^']*test" | head -n 1 | cut -d"'" -f2) || true   # not there yet (set -e would end the loop)
    [[ -n $key ]] && { "$vrcmd" --showdashboard "$key" >/dev/null 2>&1; echo "shown in VR: $key"; break; }
    sleep 0.5
  done
  while [[ -n $key ]]; do
    sleep 1
    [[ -n $(shown_window "'$key'") ]] || { "$vrcmd" --showdashboard >/dev/null 2>&1; "$vrcmd" --showdashboard "$key" >/dev/null 2>&1; echo "shown in VR again: $key"; }
  done ) & pids+=($!)
FD_CLOSE=1 python3 -I "$root/tests/extension/drive_chrome.py" "$ext" "http://127.0.0.1:8731/$page" "$seconds" | tee "$out/drive.log"
grep -E "listening|view started|view failed|view [0-9]|dump" "$out/companion.log" | tail -n 20
