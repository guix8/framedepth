// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 FrameDepth contributors
// FrameDepth service worker: the only part that talks to framedepth-companion
// (ws://127.0.0.1:47321), with small JSON messages (view on/off, picture
// rectangle, settings, stats). Content scripts reach it through a runtime
// port, so page CSP and local-network prompts never apply.
const COMPANION = 'ws://127.0.0.1:47321';
const DEFAULTS = { enabled: false, strength: 0.9, popout: 50, smooth: 3, sharpen: 0, detail: 0, subtitles: 1, scrollflat: 1 };

const log = (...a) => {
  console.log('[FrameDepth sw]', ...a);
  const line = new Date().toISOString().slice(11, 19) + ' sw ' + a.join(' ');
  chrome.storage.local.get({ diag: [] }).then(({ diag }) => chrome.storage.local.set({ diag: [...diag.slice(-39), line] })).catch(() => {});
};

let ws = null;
let ready = false;
let active = null;         // the port whose tab has the view
let statsTimer = null;
let popupRect = null;      // where the FrameDepth popup is while it is open (see below)
let busy = false;          // the companion serves another Chrome profile: ask again less often
let leaving = false;       // the socket is closed on purpose (no tab needs it): not an error
let unreached = false;     // the last try did not reach the companion: say so until one does
let helloFailed = false;   // the companion answered with an error (no depth model): connected and asked again every 10 s
let retryTimer = 0;
const ports = new Set();
const status = (companion, extra = {}) => chrome.storage.session.set({ companion, ...extra });
// true when it went out
const send = (data) => {
  if (ws?.readyState !== WebSocket.OPEN) return false;
  ws.send(JSON.stringify(data));
  return true;
};
const sendHoles = () => send({ type: 'holes', rects: popupRect ? [popupRect] : [] });

function connect() {
  if (ws) return;
  // while the companion is not there, the popup says so; "connecting" between
  // the tries every 2 s made it flicker between the two (review 2026-10-31)
  if (!busy && !unreached && !helloFailed) status('connecting');
  log('connecting to', COMPANION);
  // busy holds only while the companion keeps turning this profile away: a
  // socket that closes without saying so (the companion gone) ends it, or the
  // popup said "another profile" for good (review 2026-10-31)
  let refused = false;
  try { ws = new WebSocket(COMPANION); } catch (e) { log('WebSocket failed', String(e)); status('error', { error: String(e) }); return; }
  // the depth detail comes with the hello, so the companion loads that model
  // first (settings follow only after ready)
  ws.onopen = () => {
    log('connected');
    chrome.storage.local.get({ detail: DEFAULTS.detail }).then((s) => send({ type: 'hello', detail: Number(s.detail) || 0 }));
  };
  ws.onerror = (e) => log('WebSocket error', e.type);
  ws.onmessage = (ev) => {
    let m;
    try { m = JSON.parse(ev.data); } catch (e) { log('bad message from companion', String(e)); return; }
    if (m.type === 'ready') {
      // also after the companion restarted: it has forgotten the view, the
      // popup's place and everything else, so the tabs ask again
      ready = true; busy = false; unreached = false; helloFailed = false;
      // refused: the depth detail whose own model did not start (another runs)
      status('connected', { model: m.model, companionVersion: m.version ?? '', refused: m.refused ?? -1, viewError: '' });
      sendHoles();
      for (const p of ports) p.postMessage({ t: 'ready' });
    } else if (m.type === 'model') {
      // another depth model now runs; refused: the depth detail that could
      // not start (the model before it runs again)
      status('connected', { model: m.model, refused: m.refused ?? -1 });
    } else if (m.type === 'error' && m.busy) {
      busy = refused = true;
      status('busy', { companionVersion: m.version ?? '' });
    } else if (m.type === 'error') {
      status('error', { error: m.error, companionVersion: m.version ?? '' });
      // no depth model runs (a hello's did not start, or a switch and the
      // way back both failed): the socket is closed and a hello sent again
      // in 10 s, after every such error (once only, it stayed after the
      // second; review 2026-11-01)
      helloFailed = true;
      if (!retryTimer) {
        const failed = ws;
        // closed at 8 s, connected again 2 s later (onclose): 10 s in all
        retryTimer = setTimeout(() => { retryTimer = 0; if (ws === failed) failed.close(); }, 8000);
      }
      for (const p of ports) p.postMessage({ t: 'error', error: m.error });
    } else if (m.type === 'view') {
      // To the tab that asked, found by the marker the reply names
      for (const p of ports) if (p.marker === m.marker) p.postMessage({ t: 'view', ok: m.ok, retry: m.retry, error: m.error, marker: m.marker });
      // why the 3D does not start, for the popup (the tab retries meanwhile)
      chrome.storage.session.set({ viewError: m.ok ? '' : String(m.error || '') });
    } else if (m.type === 'stats') {
      chrome.storage.session.set({ stats: { ...m, at: Date.now() } });
    }
  };
  ws.onclose = (e) => {
    log('closed', e.code, e.reason);
    ws = null; ready = false;
    busy = refused;
    // a retry armed for this socket is done with it (left armed, the next
    // socket's error got none, and stayed shown for good)
    clearTimeout(retryTimer); retryTimer = 0;
    // closed on purpose: an error from before says nothing about the next try
    if (leaving) helloFailed = false;
    // no tab needs it (any more): not tried again, which is no error; "in use
    // by another profile, trying again" stayed after 3D was turned off here
    // (review 2026-11-01)
    if (leaving || !ports.size) status('idle', { viewError: '' });
    else if (helloFailed) { /* the error stays shown */ }
    else if (!busy) { unreached = true; status('disconnected', { viewError: '' }); }
    leaving = false;
    for (const p of ports) p.postMessage({ t: 'error', error: 'companion disconnected' });
    // another profile holds the companion until it lets go: every 10 s
    if (ports.size) setTimeout(() => { if (ports.size) connect(); }, busy ? 10000 : 2000);
  };
}

// Every frame of a tab with a picture to show offers it (its visible size);
// the largest gets the view, the others wait (a video in an iframe against
// pictures of the page, say)
function arbitrate(tabId) {
  let best = null, holder = null;
  for (const p of ports) {
    if (p.sender?.tab?.id !== tabId) continue;
    if (p.score > (best?.score ?? 0)) best = p;
    if (p.granted && p.score > 0) holder = p;
  }
  // the frame holding the view keeps it unless another is clearly larger
  // (scores move while the page scrolls; handing the view back and forth
  // would restart it each time)
  if (holder && best && best !== holder && best.score < 1.5 * holder.score) best = holder;
  for (const p of ports) {
    if (p.sender?.tab?.id !== tabId) continue;
    const on = p === best;
    if (p.granted !== on) { p.granted = on; try { p.postMessage({ t: 'grant', on }); } catch (e) { /* gone */ } }
  }
}

// The view ended: a tab that lost it to this one may take it back. Not
// 'ready': that also restarts a frame that has just asked for the view (the
// one a frame of the same tab gave it up to), which asked twice and started
// about 2.5 s late (review 2026-10-31); 'free' starts only those without one.
function released() {
  active = null;
  chrome.storage.session.set({ viewError: '' });
  if (ready) for (const p of ports) p.postMessage({ t: 'free' });
}

// The page zoom, which the top frame needs to find where the page starts in
// Chrome's window
const sendZoom = (tabId) => chrome.tabs.getZoom(tabId)
  .then((z) => chrome.tabs.sendMessage(tabId, { fdZoom: z }, { frameId: 0 })).catch(() => {});
chrome.tabs.onZoomChange.addListener(({ tabId, newZoomFactor }) =>
  chrome.tabs.sendMessage(tabId, { fdZoom: newZoomFactor }, { frameId: 0 }).catch(() => {}));

// Scrolling input seen in any frame (content.js): for the tab that holds
// the view, the companion hides the 3D before the page moves
chrome.runtime.onMessage.addListener((m, sender) => {
  if (m?.fdMoving && active && sender.tab?.id === active.sender?.tab?.id) send({ type: 'moving' });
});

chrome.runtime.onConnect.addListener((port) => {
  // The FrameDepth popup is drawn inside Chrome's window (no X window of its
  // own on the headset), so it would be read with the video and turned 3D.
  // The popup page reports where it is (window pixels); the companion leaves
  // that place of its overlay see-through, so Chrome's own popup shows there
  // and the rest of the video stays 3D. Kept here and sent again when the
  // companion (re)connects.
  if (port.name === 'popup') {
    let last = '';
    port.onMessage.addListener((m) => {
      const key = JSON.stringify(m.rect);
      if (key === last) return;
      popupRect = m.rect;
      if (sendHoles()) { last = key; log('popup hole', key, 'from', JSON.stringify(m.raw)); }
    });
    port.onDisconnect.addListener(() => { popupRect = null; sendHoles(); });
    return;
  }
  if (port.name !== 'fd') return;
  log('content script connected', port.sender?.tab?.id, 'frame', port.sender?.frameId);
  ports.add(port);
  connect();
  if (ready) port.postMessage({ t: 'ready' });
  // One tab holds the view (the last to ask). A tab that lost it (hidden,
  // switched away) may still send a late view off, rect or ui; those would
  // turn off or move the other tab's view, so only the holder's go through.
  // Settings are the same for every tab and always pass.
  port.onMessage.addListener((m) => {
    if (m.t === 'candidate') { port.score = m.score; arbitrate(port.sender?.tab?.id); return; }
    if (m.t === 'title') {
      // the window title is the top page's: its frame (0) sets the marker
      chrome.tabs.sendMessage(port.sender.tab.id, { fdTitle: m.marker, clear: m.clear }, { frameId: 0 }).catch(() => {});
      return;
    }
    if (m.t !== 'send') return;
    const type = m.data.type;
    if (type === 'view' && m.data.on) {
      if (active && active !== port) { try { active.postMessage({ t: 'lost' }); } catch (e) { /* gone */ } }
      active = port; port.marker = m.data.marker;
      sendZoom(port.sender.tab.id);
    } else if (type === 'view') {
      if (port !== active) return;
      send(m.data);
      released();
      return;
    } else if (type !== 'settings' && port !== active) return;
    send(m.data);
  });
  port.onDisconnect.addListener(() => {
    ports.delete(port);
    arbitrate(port.sender?.tab?.id);
    if (port === active) { send({ type: 'view', on: false }); released(); }
    if (!ports.size && ws) setTimeout(() => { if (!ports.size && ws) { leaving = true; ws.close(); } }, 30000);
    // the last tab left while the companion was not reached: no more tries
    else if (!ports.size) status('idle', { viewError: '' });
  });
  // stats while a view runs; otherwise a ping while tabs need the companion:
  // a client silent for 30 s counts as gone, and two profiles took it from
  // each other every 30-40 s while neither showed 3D (review 2026-11-01)
  if (!statsTimer) statsTimer = setInterval(() => {
    if (active) send({ type: 'stats' });
    else if (ports.size) send({ type: 'ping' });
    else { clearInterval(statsTimer); statsTimer = null; }
  }, 2000);
});

// Chrome does not inject declared content scripts into tabs that were already
// open when the extension was installed or reloaded; do it here.
chrome.runtime.onInstalled.addListener(async () => {
  const tabs = await chrome.tabs.query({ url: ['http://*/*', 'https://*/*'] });
  log('installed; injecting into', tabs.length, 'tabs');
  for (const tab of tabs) {
    chrome.scripting.executeScript({ target: { tabId: tab.id, allFrames: true }, files: ['content.js'] })
      .catch((e) => log('inject skipped', tab.id, String(e)));
  }
});

// Keyboard shortcuts (chrome://extensions/shortcuts), the only way to change
// the 3D in fullscreen, where the toolbar and so the popup are out of reach.
// Strength and pop-out move as the popup's sliders would (a scene type then
// counts as changed, see popup.js).
const STEPS = { 'strength-up': ['strength', 0.1, 0, 3], 'strength-down': ['strength', -0.1, 0, 3],
                'popout-up': ['popout', 5, 0, 100], 'popout-down': ['popout', -5, 0, 100] };
chrome.commands.onCommand.addListener(async (cmd) => {
  const s = await chrome.storage.local.get({ ...DEFAULTS, preset: 'custom' });
  if (cmd === 'toggle-3d') { await chrome.storage.local.set({ enabled: !s.enabled }); return; }
  const step = STEPS[cmd];
  // with the 3D off a key press would change it unseen
  if (!step || !s.enabled) return;
  const [key, by, lo, hi] = step;
  const value = Math.round(Math.min(hi, Math.max(lo, Number(s[key]) + by)) * 100) / 100;
  if (value !== s[key]) await chrome.storage.local.set({ [key]: value, presetChanged: s.preset !== 'custom' });
});

chrome.storage.local.get(DEFAULTS).then((s) => chrome.action.setBadgeText({ text: s.enabled ? '3D' : '' }));
chrome.storage.onChanged.addListener((ch, area) => {
  if (area === 'local' && ch.enabled) chrome.action.setBadgeText({ text: ch.enabled.newValue ? '3D' : '' });
  // 3D off: the companion is let go at once, so another profile waiting for
  // it gets it within its 10 s retry (it waited out the 30 s linger first)
  // (a second on, once the tabs' content scripts have let go)
  if (area === 'local' && ch.enabled && !ch.enabled.newValue)
    setTimeout(() => { if (!ports.size && ws) { leaving = true; ws.close(); } }, 1000);
});
