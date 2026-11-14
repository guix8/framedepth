#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 FrameDepth contributors
# Install framedepth-companion for the current user, start it as a systemd
# user service and copy the Chrome extension where Flatpak Chrome can load it.
# No sudo; home folder only:
#   ~/.local/share/framedepth                  the companion and its models,
#                                              steam-chrome/ for Chrome from Steam's library
#   ~/.config/systemd/user/framedepth-companion.service
#   ~/Downloads/framedepth-extension           the unpacked extension
# Runs from a source tree after scripts/build.sh, or from an unpacked release
# (packaging/make-release.sh), which holds the same files ready to copy:
#   bash packaging/install.sh                  (source tree)
#   bash install.sh                            (release)
#   bash packaging/install.sh --stage DIR      (source tree: only lay out a release in DIR)
set -euo pipefail
src=$(cd "$(dirname "$0")" && pwd)
dest="$HOME/.local/share/framedepth"
unit_dir="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
ext="$HOME/Downloads/framedepth-extension"
marker='framedepth-install-v1'

# A release from the source tree: app/ is what goes to $dest, extension/ the
# extension, and this script beside them
stage() { # source tree, target folder
  local tree=$1 out=$2
  [[ -x $tree/build/framedepth-companion ]] || { echo 'Build first: bash scripts/build.sh' >&2; exit 1; }
  local app="$out/app" ml="$tree/build-deps/moonlight-xr/assets/depth" deps="$tree/build-deps"
  mkdir -p "$app/assets/depth" "$app/licenses" "$out/extension"
  install -m 755 "$tree/build/framedepth-companion" "$app/framedepth-companion"
  # ZipDepth on the CPU (LiteRT, from Moonlight XR's assets) and on the GPU
  # (ncnn, models/: one weights file for both input sizes)
  install -m 644 "$ml/zipdepth_512x288_fp16.tflite" "$ml/zipdepth_LICENSE.txt" "$app/assets/depth/"
  # depth detail High on the CPU (models/, scripts/convert-zipdepth-litert.sh)
  install -m 644 "$tree/models/zipdepth_672x384_fp16.tflite" "$app/assets/depth/"
  for size in 512x288 768x448; do
    install -m 644 "$tree/models/zipdepth_npu_$size.ncnn.param" "$app/assets/depth/"
    install -m 644 "$tree/models/zipdepth_npu.ncnn.bin" "$app/assets/depth/zipdepth_npu_$size.ncnn.bin"
  done
  install -m 755 "$tree/packaging/uninstall.sh" "$app/uninstall.sh"
  # For Chrome started from Steam's library (README: Chrome from Steam's library)
  mkdir -p "$app/steam-chrome"
  install -m 755 "$tree/steam-chrome/chrome-with-scroll.sh" "$tree/steam-chrome/frame-chrome-scroll.py" "$app/steam-chrome/"
  # The notices of what the companion contains (see THIRD_PARTY.md)
  install -m 644 "$tree/LICENSE" "$tree/THIRD_PARTY.md" "$app/licenses/"
  install -m 644 "$tree/models/ZipDepth-LICENSE.txt" "$app/licenses/ZipDepth-LICENSE.txt"
  install -m 644 "$ml/litert_LICENSE.txt" "$app/licenses/LiteRT-LICENSE.txt"
  install -m 644 "$ml/third_party_LICENSES.txt" "$app/licenses/LiteRT-third-party-LICENSES.txt"
  install -m 644 "$deps/moonlight-xr/LICENSE.txt" "$app/licenses/MoonlightXR-LICENSE.txt"
  install -m 644 "$deps/ncnn/LICENSE.txt" "$app/licenses/ncnn-LICENSE.txt"
  install -m 644 "$deps/ncnn/glslang/LICENSE.txt" "$app/licenses/glslang-LICENSE.txt"
  install -m 644 "$deps/downloads/openvr-2.12.14-LICENSE" "$app/licenses/OpenVR-LICENSE.txt"
  cp -r "$tree/extension/." "$out/extension/"
  install -m 755 "$tree/packaging/install.sh" "$out/install.sh"
}

if [[ ${1:-} == --stage ]]; then
  [[ -n ${2:-} && ! -e $2 ]] || { echo 'Usage: install.sh --stage NEW_FOLDER' >&2; exit 1; }
  stage "$(cd "$src/.." && pwd)" "$2"
  exit 0
fi

(( EUID != 0 )) || { echo 'Install as the normal user, not root.' >&2; exit 1; }
for d in "$dest" "$ext"; do
  if [[ -e $d && ! -f $d/.framedepth-install ]]; then
    echo "$d exists and was not installed by this script; refusing to replace it." >&2
    echo "Move it elsewhere (or delete it if you do not need it), then run install.sh again." >&2; exit 1
  fi
done
tmp=
if [[ ! -d $src/app ]]; then
  # in a source tree's packaging/: lay out the release first
  tmp=$(mktemp -d)
  trap 'rm -rf -- "$tmp"' EXIT
  stage "$(cd "$src/.." && pwd)" "$tmp/release"
  src="$tmp/release"
fi
[[ -x $src/app/framedepth-companion ]] || { echo "$src/app/framedepth-companion is missing" >&2; exit 1; }
# systemctl --user needs the login session's bus. A shell reached through su or
# without a login session lacks these two, so fill them in from the session
# folder when it exists.
runtime=/run/user/$(id -u)
: "${XDG_RUNTIME_DIR:=$runtime}"
[[ -n ${DBUS_SESSION_BUS_ADDRESS:-} || ! -S $XDG_RUNTIME_DIR/bus ]] || DBUS_SESSION_BUS_ADDRESS="unix:path=$XDG_RUNTIME_DIR/bus"
export XDG_RUNTIME_DIR
[[ -z ${DBUS_SESSION_BUS_ADDRESS:-} ]] || export DBUS_SESSION_BUS_ADDRESS
if ! systemctl --user show-environment >/dev/null 2>&1; then
  echo 'Cannot reach your systemd user session (the "user scope bus").' >&2
  echo 'Run this in a terminal of the account that is logged in on the headset' >&2
  echo '(for SteamOS usually steamos, or deck), without su or sudo, then try again.' >&2
  exit 1
fi
systemctl --user stop framedepth-companion.service 2>/dev/null || true
rm -rf -- "$dest" "$ext"
mkdir -p "$dest" "$ext" "$unit_dir"
# marked at once, so a run that fails halfway can be repeated
printf '%s\n' "$marker" > "$dest/.framedepth-install"
printf '%s\n' "$marker" > "$ext/.framedepth-install"
cp -r "$src/app/." "$dest/"
cp -r "$src/extension/." "$ext/"
cat > "$unit_dir/framedepth-companion.service" <<EOF
[Unit]
Description=FrameDepth companion (depth and SteamVR stereo for the Chrome extension)

[Service]
ExecStart="$dest/framedepth-companion"
Restart=on-failure
RestartSec=3

[Install]
WantedBy=default.target
EOF
systemctl --user daemon-reload
systemctl --user enable --now framedepth-companion.service
# enabled, or it is off again after the next reboot (issue 1)
if ! systemctl --user is-enabled --quiet framedepth-companion.service; then
  echo 'The companion service is not enabled, so it would not start after a reboot.' >&2
  echo 'Run: systemctl --user enable --now framedepth-companion' >&2; exit 1
fi
sleep 1
if ! systemctl --user is-active --quiet framedepth-companion.service; then
  echo 'The companion service did not start. Its log:' >&2
  journalctl --user -u framedepth-companion -n 20 --no-pager >&2 || true
  exit 1
fi
echo "Installed $("$dest/framedepth-companion" --version) in $dest and the extension in $ext"
echo 'Load the extension once: chrome://extensions -> Developer mode -> Load unpacked -> Downloads/framedepth-extension'
echo '(keep that folder: Chrome runs FrameDepth from it)'
echo 'After an update: reload FrameDepth in chrome://extensions.'
