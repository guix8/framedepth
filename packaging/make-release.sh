#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 FrameDepth contributors
# Pack a prebuilt release for SteamOS on the headset (aarch64), after
# scripts/build.sh:
#   bash packaging/make-release.sh
# Writes dist/framedepth-VERSION-steamos-arm64.tar.gz and its .sha256. Users
# unpack it and run install.sh: no build tools, no build-deps/.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
version=$(sed -n 's/^ *"version": *"\([^"]*\)".*/\1/p' extension/manifest.json)
[[ -n $version ]] || { echo 'No version in extension/manifest.json' >&2; exit 1; }
[[ -x build/framedepth-companion ]] || { echo 'Build first: bash scripts/build.sh' >&2; exit 1; }
built=$(build/framedepth-companion --version)
[[ $built == "framedepth-companion $version" ]] || { echo "build/ has '$built', the manifest $version: build again" >&2; exit 1; }
# Everything the companion loads at run time must come with SteamOS or
# SteamVR, not from this build machine
if ldd build/framedepth-companion | grep -E "not found|$HOME|$root"; then
  echo 'The companion needs libraries a fresh headset does not have (above).' >&2; exit 1
fi

name="framedepth-$version-steamos-arm64"
rm -rf -- "dist/$name" "dist/$name.tar.gz" "dist/$name.tar.gz.sha256"
mkdir -p dist
bash packaging/install.sh --stage "dist/$name"
cat > "dist/$name/README.txt" <<EOF
FrameDepth $version for the Steam Frame (SteamOS, arm64), prebuilt.

Install, in a terminal on the headset (no sudo, home folder only):
  bash install.sh
Then in Chrome: chrome://extensions -> Developer mode -> Load unpacked ->
Downloads/framedepth-extension, click the FrameDepth icon and turn 3D on.
After an update, reload FrameDepth in chrome://extensions.
Remove: bash ~/.local/share/framedepth/uninstall.sh

License: GPL-3.0-only (app/licenses/LICENSE). Third-party components and
their licenses: app/licenses/. Source: tag v$version of
https://github.com/guix8/framedepth, and the third-party sources this build
uses in framedepth-$version-deps-src.tar.gz on the same release page.
EOF
# fixed owner and order, so the archive depends only on the files
tar --sort=name --owner=0 --group=0 --numeric-owner -C dist -czf "dist/$name.tar.gz" "$name"
(cd dist && sha256sum "$name.tar.gz" > "$name.tar.gz.sha256")
rm -rf -- "dist/$name"
echo "Wrote dist/$name.tar.gz ($(du -h "dist/$name.tar.gz" | cut -f1))"
cat "dist/$name.tar.gz.sha256"
