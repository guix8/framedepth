// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 FrameDepth contributors
// FrameDepth content script. It runs in every frame of a page. With 3D on, each
// frame picks its largest visible video, or failing that the image that is
// clearly the picture of the page, and
// the service worker grants the view to the largest of all frames of the tab.
// The granted frame keeps telling framedepth-companion where that picture is,
// in Chrome window pixels. The companion reads those pixels from Chrome's
// window, makes the stereo pair and shows it in its own SteamVR overlay over
// the same spot. This script draws nothing; text and page UI stay as Chrome
// renders them.
//
// Frames inside iframes learn where they sit in the top page from their
// parent (postMessage, see "Frame offsets"); only the top page's title is
// the window title, so the title marker goes through the service worker to
// the top frame.
//
// One copy per frame. Reloading or updating the extension leaves the copy
// already running in an open tab cut off from it (every chrome.* call throws
// "Extension context invalidated"); that copy retires (see retire) and the
// copy the service worker injects then waits for it here and takes over.
(function frameDepth(tries) {
  if (window.__framedepth) {
    if (tries < 20) setTimeout(() => frameDepth(tries + 1), 250);
    return;
  }
  const token = {};
  window.__framedepth = token;
  const isTop = window === top;

  // Diagnostics also go to chrome.storage.local ('diag', last 40 lines) so they
  // can be read from the profile on the headset without DevTools.
  const alive = () => { try { return !!chrome.runtime?.id; } catch (e) { return false; } };
  const log = (...a) => {
    console.log('[FrameDepth]', ...a);
    if (!alive()) return;
    // no site name: the log stays in the profile and should not be a list of visited sites
    const line = new Date().toISOString().slice(11, 19) + ' cs' + (isTop ? '' : ' (frame)') + ' ' + a.join(' ');
    chrome.storage.local.get({ diag: [] }).then(({ diag }) => chrome.storage.local.set({ diag: [...diag.slice(-39), line] })).catch(() => {});
  };
  // popout: 0 = everything behind the screen, 100 = everything in front.
  // The companion takes the depth that stays on the screen plane:
  // convergence = 100 - popout.
  const settings = { enabled: false, strength: 0.9, popout: 50, smooth: 3, sharpen: 0, detail: 0, subtitles: 1, scrollflat: 1 };
  const MIN_W = 200, MIN_H = 112;          // video
  const GALLERY_W = 120, GALLERY_H = 68;   // images shown together (thumbnails), not icons
  let session = null;
  let failedStarts = 0;   // view requests the companion refused in a row

  chrome.storage.local.get(settings).then((s) => { Object.assign(settings, s); if (isTop) log('loaded, enabled=' + settings.enabled); check(); });
  chrome.storage.onChanged.addListener((ch, area) => {
    if (area !== 'local') return;
    let changed = false;
    for (const k of Object.keys(ch)) if (k in settings) { settings[k] = ch[k].newValue; changed = true; }
    if (!changed) return;
    session?.sendSettings();
    check();
  });
  document.addEventListener('fullscreenchange', check, true);
  // Another tab brought to the front: the view ends at once, not at the next
  // check up to half a second later, while the new tab's page showed in 3D
  // in the old video's place (review 2026-10-31)
  document.addEventListener('visibilitychange', check);
  let pointerMovedAt = -1e9;
  document.addEventListener('pointermove', () => { pointerMovedAt = performance.now(); }, { capture: true, passive: true });
  // Scrolling input (wheel, touch, scroll keys) arrives before the page has
  // moved: telling the companion then lets it hide the 3D before Chrome draws
  // the scrolled page, instead of a frame or two after (the old picture slid
  // with the page while it caught up, headset 2026-10-28). At most every 50 ms.
  // Any frame of the tab sends it (the page around an iframe's video scrolls
  // too); the service worker passes it on only for the tab holding the view.
  // The listeners run after the page's own (bubble phase), so input the page
  // took for itself (a player's Space, a zoom's Ctrl+wheel) and typing in a
  // text field do not count.
  let movingSentAt = -1e9;
  const scrolling = (e) => {
    const now = performance.now();
    if (now - movingSentAt < 50 || retired || !settings.enabled || !settings.scrollflat) return;
    if (e.defaultPrevented || e.ctrlKey || e.altKey || e.metaKey || document.fullscreenElement) return;
    movingSentAt = now;
    try { chrome.runtime.sendMessage({ fdMoving: 1 }).catch(() => {}); } catch (err) { /* extension reloaded */ }
  };
  window.addEventListener('wheel', scrolling, { passive: true });
  window.addEventListener('touchmove', scrolling, { passive: true });
  window.addEventListener('keydown', (e) => {
    const t = e.composedPath()[0];
    if (t && (t.isContentEditable || /^(INPUT|TEXTAREA|SELECT)$/.test(t.tagName))) return;
    if (/^(ArrowUp|ArrowDown|PageUp|PageDown|Home|End| )$/.test(e.key)) scrolling(e);
  }, { passive: true });
  const checkTimer = setInterval(check, 500);

  // The extension was reloaded or updated: stop everything this copy does,
  // take its marker off the title and make way for the new copy
  let retired = false;
  function retire() {
    if (retired) return;
    retired = true;
    clearInterval(checkTimer);
    document.removeEventListener('fullscreenchange', check, true);
    document.removeEventListener('visibilitychange', check);
    if (session) { session.stopped = true; session = null; }
    if (titleWatch) { titleWatch.disconnect(); titleWatch = null; }
    clearTimeout(titleFixTimer);
    if (titleMarker && document.title.startsWith(titleMarker)) document.title = document.title.slice(titleMarker.length + 1);
    titleMarker = null;
    if (window.__framedepth === token) window.__framedepth = null;
    console.log('[FrameDepth] the extension was reloaded: this copy stopped');
  }

  // ---- Title marker (top frame only) ----
  // The companion finds Chrome's X window by a marker in its title, which is
  // the top page's title; the service worker forwards a granted frame's
  // marker here.
  // Clearing names the marker it clears, so a late clear from a frame that
  // lost the view cannot remove the marker the new holder just set. The
  // service worker also passes the tab's page zoom here (see topInfo).
  let titleMarker = null, pageZoom = 1, titleWatch = null, titleFixAt = -1000, titleFixTimer = 0;
  // A page that writes its own title (an unread count, a player's time) took
  // the marker out before the companion found the window, and the view kept
  // failing to start (review 2026-10-31): it is put back, at most every 200 ms
  const keepMarker = () => {
    if (!titleMarker || document.title.startsWith(titleMarker)) return;
    const wait = 200 - (performance.now() - titleFixAt);
    if (wait > 0) { titleFixTimer ||= setTimeout(() => { titleFixTimer = 0; keepMarker(); }, wait); return; }
    titleFixAt = performance.now();
    document.title = titleMarker + ' ' + document.title;
  };
  if (isTop) {
    chrome.runtime.onMessage.addListener((m, sender, reply) => {
      if (!m) return;
      // the popup's place needs the display's scale (devicePixelRatio
      // without the page zoom, which the popup reads itself)
      if (m.fdDpr) { reply(devicePixelRatio); return; }
      if (typeof m.fdZoom === 'number' && m.fdZoom > 0) pageZoom = m.fdZoom;
      if (!('fdTitle' in m)) return;
      if (m.fdTitle === null && m.clear !== titleMarker) return;
      if (titleMarker && document.title.startsWith(titleMarker)) document.title = document.title.slice(titleMarker.length + 1);
      titleMarker = m.fdTitle;
      if (titleMarker) document.title = titleMarker + ' ' + document.title;
      if (titleMarker && !titleWatch && document.head) {
        titleWatch = new MutationObserver(keepMarker);
        titleWatch.observe(document.head, { subtree: true, childList: true, characterData: true });
      } else if (!titleMarker && titleWatch) { titleWatch.disconnect(); titleWatch = null; }
    });
  }

  // ---- Frame offsets ----
  // Where this frame's viewport is in the top page, in top CSS px, with the
  // part of it the top page shows (clip: x0, y0, x1, y1) and the browser's
  // toolbar size in window pixels (ct, cl) and the pixel ratio (dpr). The top
  // frame knows it; a child gets it from its parent, which sends it to each
  // child frame that asked ('want', repeated every second while needed) on
  // every animation frame it changes.
  // outerWidth/Height are in DIP, unaffected by page zoom, while innerWidth/
  // Height and devicePixelRatio follow it: the screen's own scale is
  // devicePixelRatio / zoom. Extra width beyond the window's borders is a side
  // panel (Chrome puts it on the right), so the page then starts at the left.
  const topInfo = () => {
    const dpr = devicePixelRatio || 1, scale = dpr / pageZoom;
    const extraW = outerWidth * scale - innerWidth * dpr;
    return { x: 0, y: 0, clip: [0, 0, innerWidth, innerHeight], dpr,
             ct: Math.max(0, outerHeight * scale - innerHeight * dpr), cl: extraW > 32 ? 0 : Math.max(0, extraW / 2) };
  };
  let parentOffset = null;   // a child's offset, from its parent
  const myOffset = () => (isTop ? topInfo() : parentOffset);
  let wantedAt = -1e9;
  const wantOffset = () => {
    if (isTop || performance.now() - wantedAt < 1000) return;
    wantedAt = performance.now();
    try { parent.postMessage({ __framedepth: 'want' }, '*'); } catch (e) { /* detached */ }
  };
  const children = new Map();   // child window -> { at: last want, sent: last offset sent (JSON) }
  let offsetLoop = false;
  window.addEventListener('message', (e) => {
    if (retired) return;
    const d = e.data;
    if (!d || typeof d !== 'object' || !d.__framedepth) return;
    if (d.__framedepth === 'want' && e.source) {
      // sent again on every ask: a frame that loaded another page keeps its
      // window, and the new page got nothing while the offset was unchanged
      // (review 2026-11-01)
      const c = children.get(e.source) || {};
      c.at = performance.now(); c.sent = '';
      children.set(e.source, c);
      if (!offsetLoop) { offsetLoop = true; requestAnimationFrame(sendOffsets); }
    } else if (d.__framedepth === 'offset' && e.source === parent && !isTop) {
      // the parent page's own scripts can post this too: take only a
      // well-formed offset with sane numbers (pixels within +-100000)
      const o = d.o, num = (v) => typeof v === 'number' && Number.isFinite(v) && Math.abs(v) < 1e5;
      if (o && typeof o === 'object' && [o.x, o.y, o.dpr, o.ct, o.cl].every(num) && o.dpr > 0 && o.dpr <= 8
          && o.ct >= 0 && o.cl >= 0 && Array.isArray(o.clip) && o.clip.length === 4 && o.clip.every(num)) parentOffset = o;
    }
  });
  function sendOffsets() {
    const now = performance.now();
    for (const [w, c] of children) if (now - c.at > 3000) children.delete(w);
    if (!children.size || retired) { offsetLoop = false; return; }
    // A middle frame asks its own parent for as long as a child asks it:
    // asking only until the first answer, it was dropped by its parent 3 s
    // later and passed on a stale place from then on, so a video two frames
    // deep stayed where it was when the page scrolled (review 2026-10-31)
    if (!isTop) wantOffset();
    const me = myOffset();
    if (me) {
      // frames in open shadow roots too (web components)
      for (const f of roots().flatMap((root) => [...root.querySelectorAll('iframe, frame')])) {
        const c = children.get(f.contentWindow);
        if (!c) continue;
        const b = f.getBoundingClientRect(), cs = getComputedStyle(f);
        const x = b.left + f.clientLeft + parseFloat(cs.paddingLeft), y = b.top + f.clientTop + parseFloat(cs.paddingTop);
        const X = me.x + x, Y = me.y + y;
        const clip = [Math.max(me.clip[0], X), Math.max(me.clip[1], Y),
                      Math.min(me.clip[2], X + f.clientWidth - parseFloat(cs.paddingLeft) - parseFloat(cs.paddingRight)),
                      Math.min(me.clip[3], Y + f.clientHeight - parseFloat(cs.paddingTop) - parseFloat(cs.paddingBottom))];
        const o = { x: X, y: Y, clip, dpr: me.dpr, ct: me.ct, cl: me.cl };
        const key = JSON.stringify(o);
        if (key !== c.sent) { c.sent = key; try { f.contentWindow.postMessage({ __framedepth: 'offset', o }, '*'); } catch (e) { /* gone */ } }
      }
    }
    requestAnimationFrame(sendOffsets);
  }

  // ---- Choosing the picture ----
  // The picture inside a <video> or <img> element (letterboxing excluded),
  // in CSS px of this frame. contain (the default for video) and scale-down
  // letterbox the picture; fill, cover and none fill the box or spill out of
  // it (what is shown is then the box).
  const mediaSize = (el) => el.tagName === 'VIDEO' ? [el.videoWidth, el.videoHeight] : [el.naturalWidth, el.naturalHeight];
  function pictureRect(el) {
    const b = el.getBoundingClientRect();
    let { left: x, top: y, width: w, height: h } = b;
    const [mw, mh] = mediaSize(el);
    if (/^(contain|scale-down)$/.test(getComputedStyle(el).objectFit) && mw && mh && w && h) {
      const va = mw / mh;
      if (w / h > va) { const nw = h * va; x += (w - nw) / 2; w = nw; } else { const nh = w / va; y += (h - nh) / 2; h = nh; }
    }
    return { x, y, w, h };
  }
  function visiblePart(r) {
    const x0 = Math.max(0, r.x), y0 = Math.max(0, r.y);
    const x1 = Math.min(innerWidth, r.x + r.w), y1 = Math.min(innerHeight, r.y + r.h);
    return x1 - x0 >= 1 && y1 - y0 >= 1 ? { x: x0, y: y0, w: x1 - x0, h: y1 - y0 } : null;
  }
  // The part of r the user sees: in a frame, also clipped to what the pages
  // around it show (an ad's video in a frame below the fold scored by its
  // own viewport alone, won the view and showed nothing; review 2026-11-01).
  // A frame that has not learnt its place yet counts as not shown and asks
  // for it (wantsPlace).
  let wantsPlace = false;
  function shownPart(r) {
    const p = visiblePart(r);
    if (!p || isTop) return p;
    wantsPlace = true;
    const o = parentOffset;
    if (!o) return null;
    const x0 = Math.max(o.clip[0], o.x + p.x), y0 = Math.max(o.clip[1], o.y + p.y);
    const x1 = Math.min(o.clip[2], o.x + p.x + p.w), y1 = Math.min(o.clip[3], o.y + p.y + p.h);
    return x1 - x0 >= 1 && y1 - y0 >= 1 ? { x: x0 - o.x, y: y0 - o.y, w: x1 - x0, h: y1 - y0 } : null;
  }
  // The page's videos, those in open shadow roots too (web components keep
  // their insides there, and querySelectorAll does not look in: a player built
  // that way was never found; review 2026-10-31). The roots are gathered
  // every 2 s, as that touches every element of the page.
  let shadowRoots = [], rootsAt = -1e9;
  // the document and its open shadow roots, those every 2 s
  function roots() {
    const now = performance.now();
    if (now - rootsAt > 2000) {
      rootsAt = now; shadowRoots = [];
      const gather = (root) => { for (const el of root.querySelectorAll('*')) if (el.shadowRoot) { shadowRoots.push(el.shadowRoot); gather(el.shadowRoot); } };
      gather(document);
    }
    return [document, ...shadowRoots];
  }
  const videos = () => roots().flatMap((root) => [...root.querySelectorAll('video')]);
  // el is the video or holds it, through shadow roots (Node.contains stops at them)
  const holds = (el, video) => { for (let n = video; n; n = n.parentNode || n.host) if (n === el) return true; return false; };

  // A playing or paused video counts double against an image of the same
  // size, so a page's video wins over its pictures.
  function pickMedia() {
    wantsPlace = false;
    if (!settings.enabled || document.visibilityState !== 'visible') return null;
    let best = null, bestScore = 0;
    for (const v of videos()) {
      if (!v.videoWidth || v.readyState < 2) continue;
      // big enough first: shownPart makes a frame ask for its place, and
      // every frame with an icon or a pixel image asked (review 2026-11-01)
      const r = pictureRect(v);
      if (r.w < MIN_W || r.h < MIN_H) continue;
      const p = shownPart(r);
      if (!p) continue;
      const score = 2 * p.w * p.h;
      if (score > bestScore) { best = v; bestScore = score; }
    }
    // Images: one that is clearly the picture of the page (a fifth of the
    // view or more, no other image half its size) is shown alone; several
    // (a grid of thumbnails, a gallery) are shown together as one region
    // whose 3D is kept inside the pictures (the gallery object below).
    if (best) return { el: best, score: bestScore };
    const imgs = galleryImages();
    if (!imgs.length) return null;
    const lead = imgs[0];
    if (lead.score >= 0.2 * innerWidth * innerHeight && !(imgs[1] && imgs[1].score >= 0.5 * lead.score))
      return { el: lead.img, score: lead.score };
    if (imgs.length < 2) return null;
    return { el: gallery, score: imgs.reduce((s, i) => s + i.score, 0) };
  }
  // Visible pictures big enough to bother with (thumbnails count), largest first
  function galleryImages() {
    const imgs = [];
    for (const img of document.images) {
      if (!img.complete || !img.naturalWidth || !img.naturalHeight) continue;
      const r = pictureRect(img);
      if (r.w < GALLERY_W || r.h < GALLERY_H) continue;
      const p = shownPart(r);
      if (!p) continue;
      if (getComputedStyle(img).visibility === 'hidden' || !img.checkVisibility({ opacityProperty: true })) continue;
      imgs.push({ img, score: p.w * p.h, part: p });
    }
    return imgs.sort((a, b) => b.score - a.score);
  }
  // Stands in for a media element when several images are shown: the region
  // around them is read, and only the pictures inside it get depth
  const gallery = { tagName: 'GALLERY', gallery: true };
  // The choice is kept steady: switching the picture or losing it for a
  // moment (a thumbnail's hover preview starting, fewer images in view while
  // scrolling) restarted the 3D view, so the page flickered between 3D and
  // flat (87 restarts in 10 minutes on YouTube's home page, 2026-10-26).
  // Now the view stays up and only the picture inside it changes, a new
  // pick must hold for STEADY_MS first (unless the current one is gone),
  // and the view ends only after nothing qualified for STEADY_MS.
  const STEADY_MS = 1500;
  let pendingEl = null, pendingSince = 0, lostSince = 0;
  const describe = (el) => el.gallery ? 'gallery' : el.tagName.toLowerCase() + ' ' + mediaSize(el).join('x');
  function stillShown(el) {
    if (el.gallery) return galleryImages().length >= 2;
    if (!el.isConnected || !shownPart(pictureRect(el))) return false;
    return el.tagName === 'VIDEO' ? el.readyState >= 2 && el.videoWidth > 0 : el.complete && el.naturalWidth > 0;
  }
  function check() {
    if (retired) return;
    if (!alive()) { retire(); return; }
    const now = performance.now();
    const pick = pickMedia();
    const el = pick?.el || null;
    if (!session) {
      if (el) { log('view start', describe(el)); session = new Session(el); }
    } else if (!el) {
      if (!lostSince) lostSince = now;
      if (now - lostSince > STEADY_MS || !settings.enabled || document.visibilityState !== 'visible') {
        log('view stop'); session.stop(); session = null; lostSince = 0;
      }
      return;
    } else if (el !== session.video) {
      lostSince = 0;
      if (pendingEl !== el) { pendingEl = el; pendingSince = now; }
      if (!stillShown(session.video) || now - pendingSince > STEADY_MS) {
        log('picture now', describe(el)); session.setMedia(el); pendingEl = null;
      }
    } else { lostSince = 0; pendingEl = null; }
    if (session) session.offer(pick.score);
    if (!isTop && (session || wantsPlace)) wantOffset();
  }

  // What the page draws over the video's picture, in CSS px, at most 8
  // rectangles. Candidates are the elements of the player box (the outermost
  // ancestor about the video's size) that come after the video; this also
  // finds overlays with pointer-events: none, such as captions. Where the
  // video has no such box, the elements stacked over a coarse grid of points
  // are used (elementsFromPoint costs about 0.3 ms a point on YouTube, so the
  // grid is the fallback, not the rule). A candidate counts when it is shown
  // (opacity and visibility of it and its ancestors), paints something (text,
  // an image, a background, a border) and covers less than 40% of the picture;
  // it is then widened to its largest ancestor that is still that small and
  // does not hold the video, which gives whole control bars rather than
  // single buttons. Chrome's own controls (<video controls>) live
  // in a closed shadow root, so for them the bar's usual place is used.
  // Page dialogs: things outside the player drawn over the picture (a modal
  // and its backdrop, a cookie banner, a chat bubble, a sticky header) are
  // found at a 3x3 grid of points (elementsFromPoint, the elements above the
  // video there). Small ones join the flat rectangles; one that is shown,
  // paints and covers 40% of the picture or more sets covered, and the 3D
  // pauses (Chrome's flat picture only) while it is open.
  // Returns { rects, covered }.
  function findUi(video) {
    const pic = pictureRect(video);
    const area = pic.w * pic.h;
    if (!area) return { rects: [], covered: false };
    const small = (r) => r.width * r.height < area * 0.4;
    const over = (r) => r.right > pic.x && r.left < pic.x + pic.w && r.bottom > pic.y && r.top < pic.y + pic.h;
    const shown = (el) => el.checkVisibility({ opacityProperty: true, visibilityProperty: true });
    const paints = (el) => {
      if (/^(IMG|SVG|CANVAS|BUTTON|INPUT|PICTURE|svg)$/.test(el.tagName)) return true;
      const cs = getComputedStyle(el);
      if (cs.backgroundImage !== 'none' || !/rgba\(.*, 0\)|transparent/.test(cs.backgroundColor)) return true;
      if (parseFloat(cs.borderTopWidth) + parseFloat(cs.borderBottomWidth) > 0 && !/rgba\(.*, 0\)|transparent/.test(cs.borderTopColor)) return true;
      for (const n of el.childNodes) if (n.nodeType === 3 && n.textContent.trim()) return true;
      return false;
    };
    const candidates = new Set();
    // a video in a shadow root: the points are asked there (the document's
    // answer stops at the shadow host, which could pass for a dialog over it)
    const root = video.getRootNode().elementsFromPoint ? video.getRootNode() : document;
    let box = video;
    const vb = video.getBoundingClientRect();
    for (let el = video.parentElement, n = 0; el && n < 8; el = el.parentElement, n++) {
      const r = el.getBoundingClientRect();
      if (r.width * r.height > vb.width * vb.height * 1.6) break;
      box = el;
    }
    if (box !== video) {
      const all = box.getElementsByTagName('*');
      for (let i = 0; i < all.length && i < 3000; i++) {
        const el = all[i];
        if (el !== video && !el.contains(video) && video.compareDocumentPosition(el) & Node.DOCUMENT_POSITION_FOLLOWING) candidates.add(el);
      }
    } else {
      const nx = 6, ny = 4;
      for (let i = 0; i < nx; i++) for (let j = 0; j < ny; j++) {
        const x = pic.x + pic.w * (i + 0.5) / nx, y = pic.y + pic.h * (j + 0.5) / ny;
        if (x < 0 || y < 0 || x >= innerWidth || y >= innerHeight) continue;
        for (const el of root.elementsFromPoint(x, y)) {
          if (holds(el, video)) break;   // the rest is under the picture
          candidates.add(el);
        }
      }
    }
    let covered = false;
    {
      for (let i = 0; i < 3; i++) for (let j = 0; j < 3; j++) {
        const x = pic.x + pic.w * (i + 0.5) / 3, y = pic.y + pic.h * (j + 0.5) / 3;
        if (x < 0 || y < 0 || x >= innerWidth || y >= innerHeight) continue;
        for (const el of root.elementsFromPoint(x, y)) {
          if (holds(el, video)) break;
          if (box !== video && box.contains(el)) continue;   // the player's own, handled above
          const r = el.getBoundingClientRect();
          if (!r.width || !r.height || !shown(el) || !paints(el)) continue;
          if (small(r)) candidates.add(el);
          else covered = true;
        }
      }
    }
    const found = new Set();
    for (const el of candidates) {
      const r = el.getBoundingClientRect();
      if (!r.width || !r.height || !over(r) || !small(r) || !shown(el) || !paints(el)) continue;
      let top = el;
      for (let p = el.parentElement; p && p !== box && !holds(p, video); p = p.parentElement) {
        if (!small(p.getBoundingClientRect())) break;
        top = p;
      }
      found.add(top);
    }
    let rects = [...found].map((el) => el.getBoundingClientRect())
      .map((r) => ({ x0: Math.max(r.left, pic.x), y0: Math.max(r.top, pic.y), x1: Math.min(r.right, pic.x + pic.w), y1: Math.min(r.bottom, pic.y + pic.h) }));
    if (video.controls && (video.paused || performance.now() - pointerMovedAt < 3000)) {
      rects.push({ x0: vb.left, y0: vb.bottom - Math.min(56, vb.height * 0.3), x1: vb.right, y1: vb.bottom });
      if (video.paused) {
        const s = Math.min(vb.width, vb.height) * 0.12, cx = vb.left + vb.width / 2, cy = vb.top + vb.height / 2;
        rects.push({ x0: cx - s, y0: cy - s, x1: cx + s, y1: cy + s });
      }
    }
    // Overlapping or touching rectangles become one; above 8 the closest
    // pair is joined until 8 remain.
    const join = (a, b) => ({ x0: Math.min(a.x0, b.x0), y0: Math.min(a.y0, b.y0), x1: Math.max(a.x1, b.x1), y1: Math.max(a.y1, b.y1) });
    const gap = (a, b) => Math.max(0, a.x0 - b.x1, b.x0 - a.x1) + Math.max(0, a.y0 - b.y1, b.y0 - a.y1);
    for (let merged = true; merged;) {
      merged = false;
      for (let i = 0; i < rects.length && !merged; i++) for (let j = i + 1; j < rects.length; j++) {
        if (gap(rects[i], rects[j]) <= 4) { rects[i] = join(rects[i], rects[j]); rects.splice(j, 1); merged = true; break; }
      }
    }
    while (rects.length > 8) {
      let bi = 0, bj = 1, bg = Infinity;
      for (let i = 0; i < rects.length; i++) for (let j = i + 1; j < rects.length; j++) { const g = gap(rects[i], rects[j]); if (g < bg) { bg = g; bi = i; bj = j; } }
      rects[bi] = join(rects[bi], rects[bj]); rects.splice(bj, 1);
    }
    return { rects: rects.filter((r) => r.x1 > r.x0 && r.y1 > r.y0).map((r) => ({ x: r.x0, y: r.y0, w: r.x1 - r.x0, h: r.y1 - r.y0 })), covered };
  }

  class Session {
    constructor(video) {
      this.video = video; this.stopped = false; this.last = ''; this.ui = { rects: [], covered: false }; this.uiAt = -1e9; this.wasCovered = false;
      this.ready = false; this.granted = false; this.viewing = false; this.offered = 0;
      this.port = chrome.runtime.connect({ name: 'fd' });
      this.port.onMessage.addListener((m) => this.onMessage(m));
      // The service worker can be stopped (Chrome ends idle MV3 workers, e.g.
      // while the companion is down); start over with a new port, which wakes it
      this.port.onDisconnect.addListener(() => {
        if (this.stopped) return;
        if (!alive()) { retire(); return; }
        log('extension port closed; reconnecting');
        this.stopped = true; this.restoreTitle();
        if (session === this) { session = null; setTimeout(check, 1000); }
      });
      this.tick = this.tick.bind(this);
      this.ticking = false;
    }
    // Another picture in the same view (no restart of the companion's view)
    setMedia(el) {
      this.video = el;
      this.ui = { rects: [], covered: false }; this.uiAt = -1e9; this.wasCovered = false;
      this.pics = null; this.vfFrames = null;
    }
    send(data) { try { this.port.postMessage({ t: 'send', data }); } catch (e) { /* port gone */ } }
    sendSettings() { this.send({ type: 'settings', strength: settings.strength, convergence: 100 - settings.popout, smooth: settings.smooth, sharpen: settings.sharpen, detail: settings.detail,
                                        subtitles: settings.subtitles, scrollflat: settings.scrollflat }); }
    // This frame's candidate size; the service worker grants the view to the
    // largest candidate of the tab (resent when it changes by a tenth)
    offer(score) {
      if (Math.abs(score - this.offered) <= this.offered * 0.1) return;
      this.offered = score;
      try { this.port.postMessage({ t: 'candidate', score }); } catch (e) { /* port gone */ }
    }
    onMessage(m) {
      if (this.stopped) return;
      if (m.t === 'ready') {
        // also after the companion restarted or the connection came back:
        // the companion forgot the view then, so ask for it again
        this.ready = true;
        this.sendSettings();
        this.viewing = false;
        this.startView();
      } else if (m.t === 'free') {
        // the tab's view ended: a frame without one may take it now
        if (!this.viewing) this.startView();
      } else if (m.t === 'lost') {
        // another tab took the view; a 'ready' brings it back when that ends
        this.viewing = false;
        this.restoreTitle();
      } else if (m.t === 'grant') {
        this.granted = m.on;
        if (m.on) this.startView(); else this.stopView();
      } else if (m.t === 'view') {
        // answers name the request's marker, kept for the whole view (a retry
        // after SteamVR restarted comes long after the title marker is gone)
        if (m.marker !== this.viewMarker) return;   // an answer to an earlier request
        clearTimeout(this.answerTimer);
        this.restoreTitle();
        log('view', m.ok ? 'ok' : 'failed ' + (m.error || ''));
        if (m.ok) failedStarts = 0;
        else if (m.retry) {
          // e.g. SteamVR restarted: a fresh session (new marker) in a few
          // seconds; after five in a row every 30 s (it gave up there before,
          // and the page stayed flat until reloaded)
          setTimeout(() => { if (session === this) { session.stop(); session = null; check(); } }, ++failedStarts <= 5 ? 5000 : 30000);
        }
      } else if (m.t === 'error') {
        log('companion', m.error);
      }
    }
    startView() {
      if (!this.ready || !this.granted || this.viewing) return;
      this.viewing = true;
      this.restoreTitle();
      this.marker = this.viewMarker = '[FD:' + Math.random().toString(36).slice(2, 8) + ']';
      // the window title (top page) carries the marker the companion looks for
      try { this.port.postMessage({ t: 'title', marker: this.marker }); } catch (e) { /* port gone */ }
      // view first: the service worker passes rect and ui only from the
      // port that holds the view
      this.send({ type: 'view', on: true, marker: this.marker });
      // No answer in 30 s (the companion stuck in SteamVR or the X server):
      // asked again with a fresh session, as after a refusal; it waited for
      // good before (review 2026-10-31)
      clearTimeout(this.answerTimer);
      const asked = this.viewMarker;
      this.answerTimer = setTimeout(() => {
        if (session !== this || this.viewMarker !== asked || !this.viewing) return;
        log('no answer to the view request');
        this.onMessage({ t: 'view', ok: false, retry: true, error: 'no answer', marker: asked });
      }, 30000);
      this.last = ''; this.lastUi = ''; this.lastOnly = ''; this.vfFrames = null; this.vfSent = null;
      if (!this.ticking) { this.ticking = true; this.tick(); }
    }
    stopView() {
      clearTimeout(this.answerTimer);
      if (!this.viewing) return;
      this.viewing = false;
      this.restoreTitle();
      this.send({ type: 'view', on: false });
    }
    restoreTitle() {
      if (this.marker) { try { this.port.postMessage({ t: 'title', marker: null, clear: this.marker }); } catch (e) { /* port gone */ } }
      this.marker = null;
    }
    // Every animation frame: where the picture is, in window pixels. Only
    // changes are sent (scrolling, resizing, fullscreen, layout shifts).
    tick() {
      if (this.stopped || !this.viewing) { this.ticking = false; return; }
      // While a page dialog covers the picture it counts as not visible: the
      // companion hides the 3D and Chrome's flat picture shows (findUi below
      // runs a few times a second and sets this.ui.covered).
      const now = performance.now();
      if (!(now - this.uiAt < 200) && !this.video.gallery) {
        this.uiAt = now; this.ui = findUi(this.video);
        if (this.ui.covered !== this.wasCovered) { this.wasCovered = this.ui.covered; log(this.ui.covered ? 'page dialog over the video: 3D paused' : 'page dialog gone'); }
      }
      // To top-page CSS px (a child frame adds its offset and is clipped to
      // what the pages around it show), then to window pixels below Chrome's
      // tab strip and toolbar (none in fullscreen)
      const o = myOffset();
      const toTop = (r) => {
        if (!o || !r) return null;
        const x0 = Math.max(o.clip[0], o.x + r.x), y0 = Math.max(o.clip[1], o.y + r.y);
        const x1 = Math.min(o.clip[2], o.x + r.x + r.w), y1 = Math.min(o.clip[3], o.y + r.y + r.h);
        return x1 - x0 >= 1 && y1 - y0 >= 1 ? { x: x0, y: y0, w: x1 - x0, h: y1 - y0 } : null;
      };
      const toWindow = (r) => [Math.round(o.cl + r.x * o.dpr), Math.round(o.ct + r.y * o.dpr), Math.round(r.w * o.dpr), Math.round(r.h * o.dpr)];
      // A gallery: the region is the box around the visible pictures; the
      // pictures themselves (up to 16, largest first) are the only places
      // that get depth ("only"), the page between them stays flat
      let only = [];
      let part;
      if (this.video.gallery) {
        // which pictures: looked up a few times a second (it touches every
        // image of the page); where they are: every frame (cheap for 16)
        if (!this.pics || now - this.picsAt > 200) { this.picsAt = now; this.pics = galleryImages().slice(0, 16).map((i) => i.img); }
        const pics = this.pics.map((img) => toTop(visiblePart(pictureRect(img)))).filter(Boolean);

        if (pics.length) {
          const x0 = Math.min(...pics.map((p) => p.x)), y0 = Math.min(...pics.map((p) => p.y));
          const x1 = Math.max(...pics.map((p) => p.x + p.w)), y1 = Math.max(...pics.map((p) => p.y + p.h));
          part = { x: x0, y: y0, w: x1 - x0, h: y1 - y0 };
          only = pics.map(toWindow);
        } else part = null;
      } else {
        part = this.ui.covered ? null : toTop(visiblePart(pictureRect(this.video)));
      }
      const onlyKey = JSON.stringify(only);
      if (onlyKey !== this.lastOnly) { this.lastOnly = onlyKey; this.send({ type: 'only', rects: only }); }
      const rect = part ? (([x, y, w, h]) => ({ x, y, w, h, visible: true }))(toWindow(part))
                        : { x: 0, y: 0, w: 0, h: 0, visible: false };
      const key = JSON.stringify(rect);
      if (key !== this.last) { this.last = key; this.send({ type: 'rect', ...rect }); }
      // Player controls and captions are drawn over the picture, so the depth
      // model gives them the scene's depth and each eye shifts them apart
      // (the play button showing over the speaker button). Their rectangles
      // go to the companion, which keeps them on the screen plane. They are
      // found from the page itself (findUi, a few times a second), so no
      // per-site rules are needed.
      const uiRects = part ? this.ui.rects.map(toTop).filter(Boolean).map(toWindow) : [];
      const uiKey = JSON.stringify(uiRects);
      if (uiKey !== this.lastUi) { this.lastUi = uiKey; this.send({ type: 'ui', rects: uiRects }); }
      // The video's own frame rate (frames shown less frames dropped, each
      // second; 0 while paused or for a picture), which the popup sets
      // against how many of them the 3D drew
      if (!(now - this.vfAt < 1000)) {
        const v = this.video, q = v.tagName === 'VIDEO' && !v.paused ? v.getVideoPlaybackQuality() : null;
        const n = q ? q.totalVideoFrames - q.droppedVideoFrames : null;
        const fps = n === null ? 0 : this.vfFrames === null || n < this.vfFrames ? null : Math.round((n - this.vfFrames) * 1000 / (now - this.vfAt));
        this.vfFrames = n; this.vfAt = now;
        if (fps !== null && fps !== this.vfSent) { this.vfSent = fps; this.send({ type: 'videofps', fps }); }
      }
      requestAnimationFrame(this.tick);
    }
    stop() {
      this.stopView();
      clearTimeout(this.answerTimer);
      this.stopped = true;
      try { this.port.disconnect(); } catch (e) { /* already gone */ }
    }
  }
})(0);
