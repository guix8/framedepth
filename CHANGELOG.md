# Changelog

## Unreleased

- install.sh: checks at the end that the companion service is enabled and
  running, so it also starts after a reboot (issue #1); README
  Troubleshooting: *Not connected* turns the service on with
  `systemctl --user enable --now`

## 0.6.38

- install.sh: finds the systemd user session itself when the shell has no
  login session (after `su`), and otherwise stops before changing anything
  with a message, instead of failing with "Failed to connect to user scope
  bus"; README Troubleshooting covers it

## 0.6.37

- Steam launcher: the Chrome window follows the screen Steam gives the app.
  1920×1080 at 150% only on a 1080-line screen; on the 1280×720 screen of the
  SteamOS 0.5 beta it stays 1280×720 at 100%, since a larger window left a
  third of it out of the laser's reach

## 0.6.36

- Popup: the paragraph under the version line is gone (what turns 3D and the
  shortcuts are in "What the settings do"); the scene type shows only the
  chosen type's description

## 0.6.35 — first public release

- Turns the largest video in view in Chrome on the Steam Frame into 3D, in
  the page, fullscreen or in an iframe; with no video, the page's main
  pictures
- A Chrome extension finds what to convert and where player controls and
  captions are; a companion on the headset estimates depth with ZipDepth (CPU,
  or GPU for pictures), draws both eyes on the GPU and shows them as a SteamVR
  overlay over the video
- Player controls, captions, page dialogs, black bars and subtitles in the
  picture kept flat; 3D videos (side by side, top and bottom) shown as they
  are; flat while scrolling; the depth kept within about 1 degree
- Scene types (Film & live action, Animation, Game video, Photo) with your own
  saved values; strength, pop-out, soft edges, sharpen and depth detail in the
  popup; shortcuts for 3D on/off and strength
- Zero-copy reads of Chrome's window (XComposite and DRI3), XShm as fallback
- A launcher for Chrome started from Steam's library: a 1920×1080 window at
  150% and thumbstick scrolling
- English, Korean and Japanese
