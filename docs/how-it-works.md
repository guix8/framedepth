# How FrameDepth works

## Overview

```
Chrome (Flathub, X11 window on gamescope)            SteamVR
 ├─ page ── extension content script (every frame)      ▲
 │            │ picture rectangle, player UI, …         │ overlay: left|right
 │            ▼                                         │ images, anchored
 │          extension service worker ── ws://127.0.0.1:47321 ──► framedepth-companion
 │                                                      │  reads Chrome's window (X11)
 └─ Chrome's own window, shown by SteamVR ◄─ our overlay 1 mm in front, same place
```

The headset shows every X11 window as a SteamVR dashboard overlay (gamescope's
OpenVR backend). FrameDepth leaves Chrome's overlay alone and puts its own
side-by-side stereo overlay just in front of it, exactly over the picture,
while the page around it stays Chrome's.

## One frame

1. **Choosing the picture (extension).** Every frame of the page (iframes
   included) proposes its best candidate: the largest visible playing video, or
   else an image that is clearly the page's picture (a fifth of the view, no
   other image half its size), or else a *gallery* of all visible pictures of
   120×68 CSS px or more. The service worker grants the view to the largest
   candidate of the tab. Videos inside open shadow roots (players built as
   web components) count too. Choices are kept steady: a new candidate must win for
   1.5 s before it replaces the current one, a vanished one is waited for 1.5 s,
   and changing the picture does not restart the view.
2. **Where it is.** The granted frame computes the picture's rectangle in
   Chrome window pixels. A frame inside an iframe gets its offset in the top
   page, the part the pages around it show, the toolbar size and the pixel
   ratio from its parent (postMessage, every animation frame while needed).
3. **Finding the window.** The top page's title briefly carries a marker; the
   companion finds the X11 window and the SteamVR overlay with that title, on
   any of gamescope's X displays: Chrome opened from the dashboard is a
   dashboard window on `:0`, Chrome started from Steam's library (as a
   non-Steam game) a desktop game overlay on `:1`. gamescope shows a window
   smaller than that overlay scaled up to fill it, and the companion places
   the 3D by the same scale. `steam-chrome/` (see the README) starts Chrome
   from Steam at 1920×1080 and 150%, so nothing is scaled up and the 3D gets
   2.25 times the pixels, and sends the thumbstick's scrolling on to Chrome
   with XSendEvent (Chrome drops most of it, as the laser and the scrolling
   arrive from two input devices).
4. **Reading.** A reader thread with its own X connection takes Chrome's
   window as soon as it changed (it waits for XDamage reports, then 2 ms for
   the rest of the frame's damage; a forced look every 250 ms) and wakes the
   view loop, which so never waits for it. Zero-copy where the X server
   allows (gamescope's Xwayland does): XComposite names the window's pixmap,
   DRI3 gives its dma-buf, and the GPU imports it (EGL) and draws from the
   picture's rectangle in it; the picture at the depth model's size is
   averaged down on the GPU (the same pixel boxes as on the CPU) and read
   back (1 MB). Xwayland draws the window into a few buffers in turn, so the
   pixmap is named anew each frame; damage inside the picture says which
   frames are new. With no new picture for 250 ms a small copy of it is
   compared with the last such check, so a picture that changes without
   damage reports is still caught (three times in a row: XShm from then
   on). A read that fails (the window resized) falls back to XShm and tries
   zero-copy again 2 s later; nothing is read while the 3D is not shown.
   Otherwise XShm copies the
   rectangle out (3-10 ms at 1080p) into one of three images, and the loop
   uploads it. The loop and the
   reader run on the big cores (2-7) at nice -5, ahead of others: SteamOS
   keeps user services on cores 0-4, two of them the little ones, shared with
   Chrome's video decoding, Xwayland and the compositor.
5. **Depth.** New frames go to Moonlight XR's depth engine (its own thread,
   per-texel temporal averaging, scene cut detection, range normalisation)
   running ZipDepth: 512×288 on the CPU (LiteRT + XNNPACK, 3 threads on the big
   cores at nice 5, so Chrome comes first) for *Normal*, 672×384 on the CPU for *High*, 768×448 on the GPU
   (ncnn on Vulkan) for *Highest*. The picture is shrunk to the model's size
   by averaging the pixels each texel covers (one pixel picked per texel
   aliased fine texture differently from frame to frame, so the input
   flickered, and sat half a texel off the shader's guide). A picture goes in
   only once the last map is done (the finished map is taken first, so the
   engine never idles a loop), so the newest picture of that moment is the one used: handed over
   while the model still ran, it waited for that run and was most of a run old
   when its own began. While a video plays a picture goes in as it arrives
   (one kept from an earlier loop was up to a frame old), stamped with the
   time it was read.
6. **Stereo (GPU, OpenGL through zink).** Guided upsample of the depth map
   (joint bilateral, Moonlight XR), then for each eye an occlusion-aware
   warp: along each row, every surface that lands on this pixel is found and
   the nearest wins, refined by two Newton steps. The picture is sampled
   there with a Catmull-Rom filter across (kept within its two middle taps,
   so edges do not ring) and linearly up and down: the eyes are resampled
   sideways only, and a linear filter there softened their detail to 90% of
   the source's (95% with it). Output: two full eyes side by
   side, handed to SteamVR as an OpenGL texture, in three buffers with fences
   so the next frame is read while the GPU draws and up to two drawn frames
   wait for their turn. While a video plays, a new depth map or setting is
   drawn with the next video frame rather than on its own (one drawing per
   video frame); over a still picture, the pointer moving or a setting
   changing is drawn once a refresh at most, and with nothing on its way the
   loop wakes once a refresh rather than every 4 ms. Each new depth map is steadied and smoothed, and the checks
   for 3D video and burned-in subtitles run, on a helper thread, so the loop
   that reads, draws and hands frames over is never held up by them.
7. **Pacing.** A frame with a new picture is handed to SteamVR at a fixed
   point of the headset's refresh (just after its vsync, from
   `GetTimeSinceLastVsync`), and which refresh follows the video's own
   rhythm (the read gaps averaged) rather than when the frame happened to be
   ready: every picture of a 30 fps video gets the same three refreshes of a
   90 Hz display, a 24 fps one 4-4-4-3, a 60 fps one 1-2. Handing frames over
   as soon as they were ready gave a 30 fps video's pictures 2, 3 or 4
   refreshes, seen as judder. The refresh rate is read from SteamVR every
   second, so 72, 90, 120 or 144 Hz and a change while running are followed
   (tried on the headset, 2026-10-31). At 144 Hz SteamVR's vsync count at
   times moves on by two (its compositor apparently running at half rate
   then); refreshes per picture are counted by time, not by that count.
   They say when pictures were handed over, not when the headset showed
   them: if the compositor does run at half rate then, a 30 fps video is
   shown 4 and 6 refreshes a picture although handed over 5 and 5.
   The popup shows the video's frame rate against how many of its frames
   were drawn in 3D; the companion's log has the time of each step, how
   many refreshes each picture got, how much older the picture the depth
   map in use came from is (the model's time, the steps after it and, on
   average, half the time between maps), each step of a map's way from its
   picture to the drawing, the GPU time of a drawing (timer queries), and how
   the depth looks over time.
8. **Placing.** The overlay is anchored to the picture on Chrome's window,
   which may be flat or curved and tilted (below).

## Keeping the window's shape

The companion casts rays from the headset at Chrome's overlay
(`ComputeOverlayIntersection`): a row of points across the middle and a column
down it give the window's up axis, whether it is flat or a cylinder (centre,
radius, angle per unit width) and the picture's place on it. Our overlay gets
the same curvature (`SetOverlayCurvature`) and sits 1 mm in front. A cheap
check every frame (two mapped corners) triggers an immediate re-measure when
the window moves, turns or is resized; a full re-measure runs every 15 frames
anyway (curved/flat switches do not show in the cheap check). The 3D stays on
while the window moves.

**Flat while scrolling** (popup switch, on by default): our overlay trails
Chrome's own drawing by a frame or more, and the depth map in use was made
from the picture as it was cut before, so while the page scrolls (or the
video moves or changes size) the 3D would tear and flicker against Chrome's
picture. While the picture's rectangle, or any picture of a gallery (whose
region can stay put while a page full of thumbnails scrolls through it),
moves by more than 2 px, or the extension sees scrolling input (wheel, touch,
scroll keys: it comes before the page moves, so the 3D is gone before Chrome
draws the scrolled page instead of sliding along for a frame or two), the
overlay is hidden and Chrome's flat picture shows; once it has been still for
300 ms (shorter showed the 3D for a moment at each turn of a quick
up-and-down scroll) the companion reads the picture again, and the first frame drawn with
a depth map of that read (or after 1 s at the latest) shows the 3D again,
about 0.4 s after the scroll stops. While hidden it draws nothing and runs the
depth model only for that settled read, leaving the GPU to SteamVR and Chrome. A picture cut differently
than before the move restarts the steadying average, and the comfort limits
take their values for the new picture at once rather than gliding there from
the old one's (which made the picture swell or sink after it appeared).

## Keeping things flat or see-through

- **Player controls and captions** drawn over a video are found from the page
  itself, on any site: elements of the player box after the video that are
  shown and paint something and cover under 40% of the picture, widened to
  their largest such ancestor (Chrome's own `<video controls>` bar is assumed
  at the bottom while paused or the pointer moves). Their depth is set to the
  screen plane, so both eyes show them in the same place; around them it eases
  back over 6% of the picture's height (3% of its width), so lines crossing a
  control bar's edge lean instead of breaking. When controls come up they
  go flat within about 0.1 s, and when they go the depth comes back over
  about 0.3 s, instead of between two frames.
- **Page dialogs** over the picture (found at a 3×3 grid of points) that cover
  40% or more pause the 3D; smaller ones are kept flat.
- **Chrome's menus and tooltips** (separate X11 windows) and **the extension's
  own popup** (drawn inside Chrome's window, its place reported by the popup:
  its page, which sits in the middle of the popup window's border and shadow,
  plus 3 px) are cut out of our overlay (alpha 0), so Chrome's drawing shows
  there.
- **A gallery** gets depth only inside its pictures; the page between them is
  on the screen plane, and inside each picture the depth eases in over 8% of
  its size, so its border sits on the screen plane like a frame instead of
  cracking.
- **Black bars burned into a video** (a wide film in a 16:9 video, a 4:3 show)
  are found on each picture sent to the depth model: a row or column counts as
  picture once something in it is lit and stays so for 10 s, so a dark scene
  keeps its place; bars must match on both sides. Lines of subtitle-like text
  in the outer rows are left out of this, so subtitles drawn inside a bar (as
  letterboxed films often have them) do not light it. Only the picture inside them
  goes to the depth model (its depth range is the film's, not the bars'), and
  the bars stay on the screen plane.

## Picture quality

- **Overscan**: shifting each eye leaves an empty strip at the side; the picture
  is widened by the largest shift instead of stretching edge pixels (the sides
  lose that much). Only sideways: enlarging both ways kept the aspect but
  resampled every row between two source rows, softening the picture.
- **Soft edges**: a blur of the depth map (box, two passes) shortens the stretch
  where a near object uncovers the background.
- **Motion guard**: the depth map trails the picture by a few tens of ms; where
  the colour no longer matches the map's guide colour, depth eases to a wide
  average so moving objects bend slightly instead of tearing.
- **Sharpen**: contrast adaptive sharpening on the source taps, off by default.
- **Depth limit**: the separation is capped so
  that no point of the picture sits more than about 1° from the screen plane
  between the eyes, from the overlay's measured width and the eye's distance
  to it (a window made larger or brought closer no longer deepens the 3D),
  counting the overscan zoom the shift is made under. It changes smoothly;
  the overscan is fixed for the settings, so the picture does not change
  size: an eye's picture moves by s = separation x the largest distance from
  the convergence, and the zoom 1 / (1 - 2s) keeps that much picture beyond
  each side. (An edge guard, which pushed the scene back while something in
  front of the screen touched a side edge, was taken out in 0.6.26: on real
  scenes it was at work most of the time and took away the pop-out set.)
- **Steadier depth**: the engine's per texel
  average (30 ms for every texel) is turned off and each texel is averaged by
  whether the picture there moved, judged from the map's guide colour against
  the previous map's (3x3): still, a 500 ms average; moving, the new depth at
  once; a scene cut the engine confirmed starts over. The depth range each
  map is stretched over (its 2nd to 98th percentile) is averaged over 75 ms,
  done by the companion from the model's raw output: a map the engine found
  to jump, and the one after, take their own range at once (the engine's
  own average started over only at a cut it had confirmed, a map late, so a
  new shot's first map came in flat or pushed aside). A slower range was
  tried against the depth slowly moving where the picture is still: that
  comes from the model, and a slower range only made the depth trail. On the test scenes: shimmer down 52-65%, lag
  0-6% more, scene cuts caught up 2-3 times sooner.
- **Recognising 3D video**: a few times a second
  the picture's halves (left/right, top/bottom) are compared after taking off
  each row's (column's) mean, allowing a small sideways shift; a match as a
  whole and in a share of 8x8 blocks, held for a second, makes the view show
  each eye its own half as it is (no depth model meanwhile). Full-width
  formats keep their own aspect: side by side is taken as full width from a
  2.5:1 frame up, and at 2:1 (VR180's square eyes), as half width (each half
  squeezed) for usual frames up to 2.39:1 films.
- **Cleaner edges**: near objects are widened by
  two depth map texels sideways before the depth blur, so their outlines stay
  clean where they uncover the background.
- **Flat burned-in subtitles** (a switch in the popup, on by default): white or yellow
  text with a dark outline in the lower part of the picture, centred, held
  for a few checks, is kept on the screen plane like player controls. The
  flat box eases to each new line's size and opens from (and closes to) its
  middle, so the depth around it does not jump between two frames.
- **Scene types** (popup): Film & live action, Animation, Game video and Photo set
  several sliders at once:

  | Type | Strength | Pop-out | Soft edges | Sharpen | Depth detail |
  |---|---|---|---|---|---|
  | Film & live action | 0.9 | 35 | 3 | 0.3 | Normal |
  | Animation | 0.8 | 40 | 4 | 0 | Normal |
  | Game video | 1.1 | 40 | 3 | 0.5 | Normal |
  | Photo | 1.5 | 50 | 2 | 0.4 | Highest |

  Moving pictures sit mostly behind the screen: something cut by a frame edge
  that comes forward fights that edge, and stereo films and 3D TV keep about a
  third of their depth in front (Avatar about -0.5 to +1 % of the width, the
  BSkyB budget 3 % with 1 % in front), hence pop-out 35 for live action.
  Those budgets are % of a cinema or TV screen, seen about 30-40 degrees wide;
  the headset's window is seen wider (a 0.98 m window at 1.48 m, about 38
  degrees, larger when brought closer), so the angle is what is matched:
  between nearest and farthest a stereo film gives about 0.6-0.7 degrees and
  3D TV up to about 0.9; strength 0.9 gives live action about 0.7 (0.24 in
  front, 0.44 behind), Animation 0.6, Game video 0.8, Photo 1.1. A
  conversion's depth errors (torn edges, wobble, depth trailing motion) grow
  with strength, and discomfort builds over time with changing depth even
  within 1 degree (Lambooij et al. 2009), so the defaults stay near film;
  stronger is a slider away (0-3 %, 0.05 a step, 0.1 a shortcut press).
  Custom starts at 0.9 and pop-out 50. Animation's flat
  colours give the model less to read and its clean lines tear easily (less
  strength, softer edges, no sharpening, which rings on flat colour); game
  videos (streams, cloud gaming; games drawn on a canvas are not found) draw
  strong perspective but often have a weapon or cockpit at the bottom
  edge; a still picture has no motion to trail, so it gets the most depth and
  the GPU model. The 1 degree cap holds for all of them. Moving a slider marks the type changed: "Save to this type" keeps
  those values as the type's own (in the extension's storage), "Default"
  brings back the built-in ones; unsaved, the type gives its values again
  next time it is chosen.

## Depth backends

| Detail | Model file | Runs on | Time per map (measured on a Steam Frame) |
|---|---|---|---|
| Normal | `zipdepth_512x288_fp16.tflite` (Moonlight XR assets) | CPU, LiteRT + XNNPACK, 3 threads, cores 2-7 | 29-34 ms, 20 maps/s |
| High | `zipdepth_672x384_fp16.tflite` (`models/`, made by `scripts/convert-zipdepth-litert.sh`) | CPU, as Normal | 45 ms alone, 57 ms with a video playing, 15 maps/s |
| Highest | `zipdepth_npu_768x448.ncnn.*` (`models/`) | GPU, ncnn on Vulkan | 23-40 ms, 15 maps/s |
| fallback | `zipdepth_npu_512x288.ncnn.*` | GPU | ~15 ms |

The Frame's Adreno 750 has a single Vulkan queue, so a 25-40 ms GPU job makes
SteamVR's compositor and Chrome's window reads wait behind it; the GPU model is
therefore run in six submissions of a few ms each. With the headset worn, CPU
depth keeps video at about 60 fps, which is why Normal runs on the CPU.

High (672x384, Frame3D Browser's size) draws thin outlines (railings, lamp
posts, ears) clearly sharper than Normal. Measured with a 1080p 30 fps video
(2026-10-31, not worn): pictures stay as even as with Normal, but the
companion takes about 2.6 big cores instead of 1.9 and the depth map in use
comes from a picture 66 ms back at the moment it is taken instead of 36 ms.
Worn, on YouTube 1080p60 (2026-11-02/03), High took 64-80 ms a map instead
of Normal's 38-47 and Chrome's own video fell to 45-50 fps at times (Normal:
56-63), so Normal stays the default and the popup recommends it for video.
Highest suits still pictures (the Photo scene type uses it).

A picture of another shape than the model's (a portrait 9:16 video) is
stretched to it. Putting it in the middle with black at the sides instead
agreed less with the model's map of the whole frame on cut-out portraits of
real scenes (edges 0.30 against 0.61): the model took the black sides for
near and the picture's depth range shrank.

## Messages (extension → companion unless noted)

| Type | Fields | Meaning |
|---|---|---|
| `hello` | `detail` | start the depth engine for that depth detail (it loads on its own thread; `ready` follows when it runs, or `error` when no model of its list starts: the extension then connects and asks again every 10 s) |
| `ready` (←) | `version`, `model`, `refused` | the companion's version (the popup warns when it differs from the extension's) and the model in use; also after the companion restarted (the extension then asks for the view again). With `refused`, that `detail`'s own model did not start and the next one in its list runs |
| `model` (←) | `model`, `refused`, `error` | another model now runs (after a `detail` change); with `refused`, that `detail`'s own model could not start: another of its list runs, or (with `error`) the model before it runs again (the popup says so) |
| `settings` | `strength` (0-5 %; the popup goes to 3), `convergence` (0-100), `smooth` (0-8), `sharpen` (0-3; the popup goes to 2), `detail` (0 Normal, 1 Highest, 2 High), `subtitles` (0/1: flat burned-in subtitles), `scrollflat` (0/1: flat while scrolling) | clamped again by the companion |
| `view` | `on`, `marker` | start (find the window by marker) / stop |
| `view` (←) | `ok`, `marker`, `retry`, `error` | answer to that marker; `retry` asks for a fresh attempt (after SteamVR or the X display restarted, or Chrome's overlay went away); the popup shows why the 3D does not start. With no answer in 30 s the extension asks again |
| `rect` | `x, y, w, h, visible` | the picture, window pixels |
| `ui` | `rects` | player UI and captions to keep flat, window pixels (up to 8) |
| `only` | `rects` | a gallery's pictures, the only places with depth (up to 16) |
| `holes` | `rects` | the extension popup, window pixels: see-through |
| `ping` | | nothing: sent every 2 s while tabs need the companion and no view runs, so it does not take the extension for gone (silent for 30 s) and give it to another profile |
| `moving` | | scrolling input seen in any frame of the tab holding the view (wheel, touch, scroll keys, not typing in a field nor input the page took itself; at most every 50 ms): hide the 3D now |
| `videofps` | `fps` | the video's own frame rate (shown less dropped frames, each second; 0 when paused) |
| `stats` / `stats` (←) | `fps` (drawings/s), `frameFps` (drawings with a new video frame/s; both 0 while the 3D is paused), `videoFps`, `grabMs` (one read of Chrome's window), `warpMs` (one drawing), `mapsPerSecond, modelMs`, `ageMs` (how much older the picture of the drawn depth map is than the picture drawn), `stereo` (0 made 3D, 1 a side-by-side, 2 a top-and-bottom 3D video shown as it is), `problem` (what keeps the view from working: `model-stopped`, `model-fails`, `no-model`, `gpu-failed`, put in the user's language by the popup; empty when nothing) | shown in the popup |

The companion listens on 127.0.0.1 only and refuses connections whose
`Origin` is not the extension's (the extension ID is fixed by the manifest
`key`). This keeps web pages out; other programs of the same user can still
connect, as they could reach the user's files anyway. It serves one extension
at a time: a second one (another Chrome profile with FrameDepth) is answered
`error` with `busy` and closed while the first talks or holds a view, and
asks again every 10 s. The same profile reconnecting (its service worker
restarted) is not turned away: the old socket's close is read first. Turning
3D off lets the companion go at once. Messages over 8 KB are ignored.

## Test tools

- `framedepth-companion --gputest OUT.ppm [moved]`: offline warp of a synthetic
  picture; `moved` gives the depth map other colours (motion guard case).
- `--depthcheck MODEL_A MODEL_B PICTURE.ppm`: run two models (either backend)
  on one picture; correlation and timing.
- `--depthbench MODEL SECONDS THREADS CPUMASK`: depth engine at full speed.
- `--snapshot TITLE_PART OUT.ppm`: Chrome's window as the X server has it.
- `--selftest SECONDS`, `--calib KEY X Y W H SECONDS`: overlay placement checks
  (they show a test pattern in the headset).
- `tests/companion_client.py`: protocol test against a second instance
  (`--port 47399`).
- `tests/extension/`: a mock companion and a Chrome DevTools driver to test the
  extension on a PC with the pages in `tests/pages/`.
- `--pipeline MODEL WxH OUT_DIR [key=value ...]`: the live path offline on raw
  frames from stdin (ffmpeg), in simulated time: the depth model at the live
  rate and delay, the engine's normalisation, the same post-processing and GPU
  warp; writes the depth each frame was drawn with, the depth of that very
  frame (`oracle=1`) and side-by-side frames.
- `--dump DIR SECONDS` (with a normal run): the live view writes what it hands
  SteamVR, with its inputs and the overlay's size and distance;
  `--rerender PREFIX OUT.ppm` draws such a dump again offline and compares.
- `tests/synth/`: synthetic scenes with known depth (`make_scenes.py`), the
  pipeline run on the headset with numbers for flicker, lag behind the picture
  and scene cut recovery (`run.py`, `metrics.py`), and a page of wiggle GIFs and
  anaglyphs to judge 3D on a monitor (`compare.py`).
- `tests/live/run_live.sh`: the whole live path on the headset without wearing
  it (a throwaway Chrome profile; shows in the headset).
- `tests/live/tearing.py`: clips whose every frame is one solid colour
  (`make`) and a check of a live run's dumps for reads that mix two frames,
  one above the other (`check`).
- `tests/regress.py`: all of the above that needs no wearing, in one run.

## Limits

- The overlay trails Chrome's own picture by a frame or two while the window
  moves fast.
- Player controls drawn on a canvas and small notices between the dialog
  check's grid points are not detected. Burned-in subtitles are found only
  when they are white or yellow with a dark outline, low in the picture and
  roughly centred; other text drawn inside a black bar (or a subtitle fading
  in) lights it and turns the bar detection off for a while.
- While 3D is on, the page's title briefly carries a marker (how the companion
  finds Chrome's window), and a page can tell the extension is installed (its
  iframes can ask for their offset). The extension's diagnostic log stays in
  its own storage on the device and records no site names.
- One picture region per tab; a gallery shows at most 16 pictures with depth.
- The page is taken to start at the left of Chrome's window below its
  toolbar (a side panel on the right is allowed for): with split view or a
  side panel on the left the 3D lands beside the picture.
- Images inside shadow roots are not looked for (videos are).
- DRM: works where Chrome lets its window be read (it did for one major
  streaming site on the test headset); not guaranteed.
- Depth comes from a single image: relative, not metric; it can be wrong on
  flat graphics, text-like content and reflections.
