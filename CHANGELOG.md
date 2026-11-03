# Changelog

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
