// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 FrameDepth contributors
const DEFAULTS = { enabled: false, strength: 0.9, popout: 50, smooth: 3, sharpen: 0, detail: 0, subtitles: 1, scrollflat: 1, preset: 'custom', presetEdits: {}, presetChanged: false };
const $ = (id) => document.getElementById(id);
const esc = (v) => String(v ?? '').replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' })[c]);
// Text in the browser's language (_locales: en, ko, ja; English otherwise)
const t = (key, ...subs) => chrome.i18n.getMessage(key, subs.map(String));
for (const el of document.querySelectorAll('[data-i18n]')) el.textContent = t(el.dataset.i18n);

function render(s) {
  $('toggle').className = 'toggle ' + (s.enabled ? 'on' : 'off');
  $('toggle').textContent = t(s.enabled ? 'toggleOn' : 'toggleOff');
  $('strength').value = s.strength; $('strengthVal').textContent = Number(s.strength).toFixed(2) + '%';
  $('popout').value = s.popout; $('popoutVal').textContent = s.popout;
  $('smooth').value = s.smooth; $('smoothVal').textContent = s.smooth;
  $('sharpen').value = s.sharpen; $('sharpenVal').textContent = s.sharpen > 0 ? Number(s.sharpen).toFixed(1) : t('valueOff');
  $('detail').value = String(s.detail);
  $('preset').value = s.preset;
  // a scene type: save the values as it, or bring back its own
  const typed = s.preset in PRESETS, saved = typed && !!s.presetEdits[s.preset];
  $('presetTools').hidden = !typed;
  // what the chosen type is for (Custom: nothing)
  $('presetHelp').hidden = !typed;
  $('presetHelp').textContent = typed ? t(PRESET_HELP[s.preset]) : '';
  $('presetSave').disabled = !s.presetChanged;
  $('presetRestore').disabled = !(s.presetChanged || saved);
  $('presetNote').hidden = !typed || !(s.presetChanged || saved);
  $('presetNote').textContent = s.presetChanged ? t('presetChanged') : saved ? t('presetSaved') : '';
  $('subtitles').checked = !!s.subtitles;
  $('scrollflat').checked = !!s.scrollflat;
}

// The companion's problem codes (stats) and view errors, in the user's language
const PROBLEMS = { 'model-stopped': 'problemModelStopped', 'model-fails': 'problemModelFails', 'no-model': 'problemNoModel', 'gpu-failed': 'problemGpu' };
function viewErrorText(e) {
  if (/title marker|overlay vanished|gone from SteamVR|cannot open the X display/.test(e)) return t('viewNotInVr');
  if (/^OpenVR/.test(e)) return t('viewNoSteamVr');
  if (/SteamVR restarted|X display lost/.test(e)) return t('viewRestarting');
  // the GPU or SteamVR would not take the 3D: restarting them is what helps
  if (/^GPU|cannot create the overlay|MIT-SHM/.test(e)) return t('viewRestartVr', esc(e));
  return t('viewFailed', esc(e));
}

async function showStatus() {
  const s = await chrome.storage.session.get(['companion', 'model', 'error', 'stats', 'companionVersion', 'viewError', 'refused']);
  const set = await chrome.storage.local.get({ enabled: DEFAULTS.enabled, detail: DEFAULTS.detail });
  const parts = [];
  // Installing updates the companion at once, but Chrome keeps running the
  // unpacked extension it loaded until it is reloaded; a companion from
  // before versions were reported sends none ('')
  const mine = chrome.runtime.getManifest().version;
  const mismatch = (s.companion === 'connected' || s.companion === 'error') && s.companionVersion !== mine;
  $('warn').hidden = !mismatch;
  $('warn').innerHTML = mismatch ? t('versionMismatch', esc(mine), esc(s.companionVersion || '?')) : '';
  // the messages carry a little markup; every value put into them is escaped
  // 3D off: no connection is wanted, and none is not an error (nor a
  // connection kept a little longer, or another profile's)
  if (!set.enabled) parts.push(t('statusOff'));
  else if (s.companion === 'connected') parts.push(t('companionConnected', esc(s.model)));
  else if (s.companion === 'error') parts.push(t('companionError', esc(s.error)));
  else if (s.companion === 'busy') parts.push(t('companionBusy'));
  else if (s.companion === 'connecting') parts.push(t('companionConnecting'));
  else if (s.companion === 'disconnected') parts.push(t('companionDisconnected'));
  else parts.push(t('companionIdle'));   // 3D on, nothing to show yet (or closed with no tab needing it: 'idle')
  if (set.enabled && s.companion === 'connected') {
    // why the 3D does not start (the tab tries again meanwhile)
    if (s.viewError) parts.push(viewErrorText(s.viewError));
    // the chosen depth detail could not start: the one before runs
    if (s.refused >= 0 && s.refused === Number(set.detail))
      parts.push(t('detailRefused', esc($('detail').querySelector(`option[value="${s.refused}"]`)?.textContent ?? s.refused)));
  }
  // what keeps the 3D from working as it should (a depth model that fails, the GPU)
  if (s.stats && Date.now() - s.stats.at < 5000 && s.stats.problem) parts.push(PROBLEMS[s.stats.problem] ? t(PROBLEMS[s.stats.problem]) : t('viewProblem', esc(s.stats.problem)));
  if (s.stats && Date.now() - s.stats.at < 5000 && s.stats.stereo)
    parts.push(t(s.stats.stereo === 1 ? 'stereoSideBySide' : 'stereoTopBottom'));
  // A playing video: how many of its frames the 3D drew (fewer means frames
  // were skipped); otherwise how often the 3D was drawn
  if (s.stats && Date.now() - s.stats.at < 5000 && s.stats.videoFps > 0)
    parts.push(t('statsVideo', esc(s.stats.videoFps), esc(s.stats.frameFps), esc(s.stats.grabMs), esc(s.stats.warpMs), esc(s.stats.mapsPerSecond), esc(s.stats.modelMs), esc(s.stats.ageMs)));
  else if (s.stats && Date.now() - s.stats.at < 5000)
    parts.push(t('stats', esc(s.stats.fps), esc(s.stats.grabMs), esc(s.stats.warpMs), esc(s.stats.mapsPerSecond), esc(s.stats.modelMs), esc(s.stats.ageMs)));
  // written only when it changed: rewritten every second, a selection in it
  // (to copy it) was lost at once (review 2026-11-01)
  const html = parts.join('<br>'), version = t('version', mine, s.companionVersion || '?');
  if (html !== shownStatus) { shownStatus = html; $('status').innerHTML = html; }
  // always shown: a bug report asks for both (the companion's once it answered)
  if ($('version').textContent !== version) $('version').textContent = version;
}
let shownStatus = null;

// Where this popup is in Chrome's window, in window pixels, with a few
// pixels around it: the companion keeps that place see-through. screenX/Y
// and outerWidth/Height are the popup window's, whose border and shadow
// (7 px a side in Chrome 155 on Windows: outer 363x614 against inner
// 349x600) surround the page; taking them as the page's put the hole that
// much plus the margin too far left and up (headset, 2026-10-27), so the
// page is placed in the middle of them. While Chrome is still opening the
// popup its window is first 0 wide, then a small one in another place, and
// only then sized and placed (about half a second, Chrome 155): those
// reports are skipped for the first 1.5 s (after that outer can be smaller
// than inner for good, at a popup zoom below 100%, and the place is still
// reported). Measured from the browser window as Chrome itself places it
// (chrome.windows): on the headset Chrome's idea of where its window is on
// the screen differs from the X server's (0,0), so screen coordinates alone
// put the hole ~300 px too far right (2026-10-25).
// Units: screenX/Y, outerWidth/Height and the browser window's place are in
// DIP, innerWidth/Height in the popup's CSS px; devicePixelRatio is the
// display's scale times the popup's own zoom. Both used to be multiplied by
// it, which put the hole off at a popup zoom other than 100% (135 px at 110%
// and a 1.5 scale; review 2026-11-01): the DIP ones now take the display's
// scale, from the page's top frame (its devicePixelRatio over its zoom).
const popupPort = chrome.runtime.connect({ name: 'popup' });
let browserWindow = null, displayScale = 0;
chrome.windows.getCurrent().then((w) => { browserWindow = w; reportPlace(); }).catch(() => {});
chrome.tabs.query({ active: true, currentWindow: true }).then(async ([tab]) => {
  const zoom = await chrome.tabs.getZoom(tab.id), dpr = await chrome.tabs.sendMessage(tab.id, { fdDpr: 1 }, { frameId: 0 });
  if (dpr > 0 && zoom > 0) { displayScale = dpr / zoom; reportPlace(); }
}).catch(() => {});   // no FrameDepth in that tab (a chrome:// page): as at 100%
const reportPlace = () => {
  if (!browserWindow || !outerWidth || (performance.now() < 1500 && (outerWidth < innerWidth || outerHeight < innerHeight))) return;
  const d = devicePixelRatio || 1, s = displayScale || d, zoom = d / s, m = 3;
  const x = screenX + Math.max(0, outerWidth - innerWidth * zoom) / 2 - browserWindow.left;
  const y = screenY + Math.max(0, outerHeight - innerHeight * zoom) / 2 - browserWindow.top;
  popupPort.postMessage({ rect: [Math.round(x * s - m * d), Math.round(y * s - m * d),
                                 Math.round((innerWidth + 2 * m) * d), Math.round((innerHeight + 2 * m) * d)],
                          raw: [screenX, screenY, outerWidth, outerHeight, innerWidth, innerHeight, d,
                                browserWindow.left, browserWindow.top, browserWindow.width, browserWindow.height] });
};

new ResizeObserver(reportPlace).observe(document.documentElement);
setInterval(reportPlace, 100);   // the popup is placed after it opens (only changes go on)

chrome.storage.local.get(DEFAULTS).then(render);
// changed elsewhere too (the keyboard shortcuts): an open popup showed the
// values from when it opened (review 2026-10-31)
chrome.storage.onChanged.addListener(async (ch, area) => {
  if (area === 'local' && Object.keys(ch).some((k) => k in DEFAULTS)) render(await chrome.storage.local.get(DEFAULTS));
});

$('toggle').onclick = async () => {
  const s = await chrome.storage.local.get(DEFAULTS);
  s.enabled = !s.enabled;
  await chrome.storage.local.set({ enabled: s.enabled });
  render(s);
};
// Scene types: several settings at once. Moving pictures sit mostly behind
// the screen: something cut by the frame's edge that comes forward fights
// the edge (a window violation), and stereo films and 3D TV keep about a
// third of their depth in front (Avatar -0.5..+1 %, BSkyB 1 % of 3 %), so
// live action takes pop-out 35 and gets a little sharpening back. Those
// budgets are % of a cinema or TV screen; the headset's window is seen
// wider (about 38 degrees for a 0.98 m window at 1.48 m), so the same % is
// a larger angle: strength 0.9 gives live action the angle of a stereo film
// (about 0.7 degrees between nearest and farthest, 1.5 gave 1.1), with room
// kept for the conversion's own depth errors, which grow with strength;
// animation's flat colours give the depth model less to read and its clean
// lines tear easily, so less strength and softer edges; game videos (weapons and
// cockpits at the bottom edge), a little more depth and sharpening; a still
// picture, the most depth, half in front, and the detailed GPU depth
// (nothing plays). Moving a slider
// while a type is chosen marks it changed: "Save" makes those values the
// type's own (presetEdits, kept in storage), "Default" brings back the
// values below. Without saving, choosing the type again gives its values.
const PRESETS = {
  movie: { strength: 0.9, popout: 35, smooth: 3, sharpen: 0.3, detail: 0 },
  anime: { strength: 0.8, popout: 40, smooth: 4, sharpen: 0, detail: 0 },
  game: { strength: 1.1, popout: 40, smooth: 3, sharpen: 0.5, detail: 0 },
  photo: { strength: 1.5, popout: 50, smooth: 2, sharpen: 0.4, detail: 1 },
};
const PRESET_KEYS = ['strength', 'popout', 'smooth', 'sharpen', 'detail'];
const PRESET_HELP = { movie: 'presetHelpMovie', anime: 'presetHelpAnime', game: 'presetHelpGame', photo: 'presetHelpPhoto' };
const presetValues = (p, s) => ({ ...PRESETS[p], ...(s.presetEdits[p] || {}) });
// Custom keeps its own values: they are kept when a scene type is chosen and
// come back with Custom (a type's values overwrote them for good; review
// 2026-11-01)
$('preset').onchange = async () => {
  const p = $('preset').value, s = await chrome.storage.local.get(DEFAULTS);
  const edits = s.preset === 'custom' ? { ...s.presetEdits, custom: Object.fromEntries(PRESET_KEYS.map((k) => [k, Number(s[k])])) } : s.presetEdits;
  await chrome.storage.local.set({ preset: p, presetChanged: false, presetEdits: edits,
                                   ...(p in PRESETS ? presetValues(p, { presetEdits: edits }) : edits.custom || {}) });
  render({ ...(await chrome.storage.local.get(DEFAULTS)) });
};
for (const key of PRESET_KEYS) {
  $(key).oninput = async () => {
    const value = Number($(key).value), s = await chrome.storage.local.get(DEFAULTS);
    await chrome.storage.local.set({ [key]: value, presetChanged: s.preset in PRESETS });
    render({ ...(await chrome.storage.local.get(DEFAULTS)) });
  };
}
$('presetSave').onclick = async () => {
  const s = await chrome.storage.local.get(DEFAULTS);
  if (!(s.preset in PRESETS)) return;
  const values = Object.fromEntries(PRESET_KEYS.map((k) => [k, Number(s[k])]));
  await chrome.storage.local.set({ presetEdits: { ...s.presetEdits, [s.preset]: values }, presetChanged: false });
  render({ ...(await chrome.storage.local.get(DEFAULTS)) });
};
$('presetRestore').onclick = async () => {
  const s = await chrome.storage.local.get(DEFAULTS);
  if (!(s.preset in PRESETS)) return;
  const edits = { ...s.presetEdits };
  delete edits[s.preset];
  await chrome.storage.local.set({ presetEdits: edits, presetChanged: false, ...PRESETS[s.preset] });
  render({ ...(await chrome.storage.local.get(DEFAULTS)) });
};
// Switches: on/off, kept as 1/0
for (const key of ['subtitles', 'scrollflat']) {
  $(key).onchange = () => chrome.storage.local.set({ [key]: $(key).checked ? 1 : 0 });
}
showStatus();
setInterval(showStatus, 1000);
