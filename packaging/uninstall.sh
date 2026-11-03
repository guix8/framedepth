#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 FrameDepth contributors
# Remove framedepth-companion and the extension files. Remove the extension
# from Chrome yourself in chrome://extensions (its settings live in Chrome).
set -euo pipefail
dest="$HOME/.local/share/framedepth"
unit="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user/framedepth-companion.service"
ext="$HOME/Downloads/framedepth-extension"
systemctl --user disable --now framedepth-companion.service 2>/dev/null || true
rm -f -- "$unit"
systemctl --user daemon-reload 2>/dev/null || true
for d in "$dest" "$ext"; do
  if [[ -e $d ]]; then
    [[ -f $d/.framedepth-install ]] || { echo "$d was not installed by install.sh; leaving it." >&2; continue; }
    rm -rf -- "$d"
  fi
done
echo 'Removed FrameDepth. Remove "FrameDepth" in chrome://extensions as well.'
# A Steam shortcut that starts Chrome through the launcher removed above no
# longer starts at all
if grep -qsF "$dest/steam-chrome/" "$HOME"/.local/share/Steam/userdata/*/config/shortcuts.vdf; then
  echo 'Chrome in your Steam library still starts through the FrameDepth launcher, which is gone now:'
  echo 'in its Properties -> Shortcut, set the target back to "/usr/bin/flatpak" and take'
  echo '"/usr/bin/flatpak" out of the start of the launch options (README: Chrome from Steam'"'"'s library).'
fi
