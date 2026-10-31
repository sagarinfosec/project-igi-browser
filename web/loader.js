// Project I.G.I. in the browser: copies the player's own game folder into OPFS (once; it never leaves the
// computer), starts the recompiled build (build/recomp.js), replays its frames with WebGPU and feeds it
// input and audio.
//
// URL options: ?mute (no audio) ?fps (frame rate in the console) ?log (host log) ?autoplay
//              ?devgame (download the hosted game folder without asking; testing)
//              ?touch (on-screen controls even without a touch screen) ?touchsens=<x> (extra look factor; the
//              settings sheet on the phone is the normal way)
//              ?exe=<name> (the executable inside the game folder, if not the profile's default)
//              ?pads=N (game controllers to expose, default 1, at most 4)
//              ?queue=1 (frames queued ahead of the display, default 2: 1 cuts 16 ms of input latency online
//              on a machine that simulates a frame well within 16 ms, else it hitches)
//              ?netplay=<url> (the room server for online play, see server/; default: this page's origin)
//              ?join=<room code>
//
// Online play (netplay.js, host/netplay.c): players meet in a room on the server; when the host starts,
// every page boots the game in deterministic lockstep mode with fresh saves, and only pad inputs travel.
// Each player's keyboard and first gamepad merge into one local pad; mouse and raw keys are off.
'use strict';

const $ = (id) => document.getElementById(id);
const params = new URLSearchParams(location.search);
// A/B experiments. Each browser draws a fixed bucket 0-99 once; an experiment is on for the buckets below its
// share, so a change can go to a few percent of players first and its reports (crash.php, field "x") be compared
// with everyone else's before it goes further. A browser whose run in an experiment ended badly (no frame, or
// killed within two minutes) leaves that experiment for good (crashCheck). URL switches force either way.
const EXPERIMENTS = { wide: 100, compact: 0 };   // percent of browsers (wide display modes: 10% 2026-10-30, 50% then 100% 2026-10-31)
const COHORT = (() => {
  let b = 99, off = {};
  try {
    b = parseInt(localStorage.getItem('igi-bucket'), 10);
    if (!(b >= 0 && b < 100)) { b = Math.floor(Math.random() * 100); localStorage.setItem('igi-bucket', String(b)); }
    off = JSON.parse(localStorage.getItem('igi-exp-off') || '{}') || {};
  } catch { b = 99; }
  const on = {};
  for (const [k, pct] of Object.entries(EXPERIMENTS)) on[k] = !off[k] && b < pct;
  // the player's own choice in the settings (controls.js) wins over the bucket: "Full screen (wide)" or "Original 4:3"
  let choice = null;
  try { choice = localStorage.getItem('igi-wide-choice'); } catch { /* storage blocked */ }
  if (choice === '1') on.wide = true;
  if (choice === '0') on.wide = false;
  if (params.has('widemodes')) on.wide = true;
  if (params.has('nowidemodes')) on.wide = false;
  if (params.get('mem') === 'compact') on.compact = true;
  if (params.get('mem') === 'full') on.compact = false;
  // a chosen setting is tagged apart (+pick), so the bucketed comparison stays between like and like
  return { bucket: b, on, tag: (Object.keys(on).filter((k) => on[k]).join('+') || 'base') + (choice !== null ? '+pick' : '') };
})();
// the settings show the screen the game will actually start with
try { RcControls.get().wide = COHORT.on.wide; } catch { /* controls.js not loaded */ }
window.RC_WIDE_RUNNING = COHORT.on.wide;   // the display modes this start runs with (touch.js: a switch to wide waits for the next)
// a run that failed in an experiment leaves it (below): a chosen widescreen is forgotten with it, or it would win again
const leaveExperiments = (keys) => {
  try {
    const off = JSON.parse(localStorage.getItem('igi-exp-off') || '{}') || {};
    for (const k of keys) off[k] = 1;
    localStorage.setItem('igi-exp-off', JSON.stringify(off));
    if (keys.includes('wide') && localStorage.getItem('igi-wide-choice') === '1') localStorage.removeItem('igi-wide-choice');
  } catch { /* storage blocked */ }
};
// one-line telemetry to crash.php (field x = the experiments this browser is in). The per-session events (start,
// play) come from a quarter of the browsers outside experiments and all inside: crash.php is PHP on shared hosting
// and serves ~6 requests/s (load test 2026-10-31), which 2,000 players starting games would approach otherwise.
// Rates per cohort are unaffected (a browser sends all of its events or none).
const SAMPLED = COHORT.tag !== 'base' || COHORT.bucket % 4 === 0;
const beacon = (type, extra) => {
  if ((type === 'start' || type === 'play' || type === 'launch' || type === 'noframe') && !SAMPLED && window.RC_RENDERER !== 'gl') return;   // the WebGL players: all of them, to judge the new renderer
  try { navigator.sendBeacon('crash.php', new Blob([JSON.stringify(Object.assign({ v: 21, type, x: COHORT.tag, r: window.RC_RENDERER || window.RC_GPU_KIND || 'gpu', ua: navigator.userAgent,
    mem: navigator.deviceMemory || 0, screen: `${screen.width}x${screen.height}@${devicePixelRatio}` }, extra || {}))], { type: 'application/json' })); } catch { /* no beacon */ }
};
const OPFS_DIR = 'rcgame';
const MARKER = '.installed.json';   // { version } once every file is copied
const NETPLAY_BASE = new URL(params.get('netplay') || './', location.href).href.replace(/\/?$/, '/');
const status = (t) => { $('status').textContent = t; };
// the launcher's screens (index.html): loading, home, online, lobby
const show = (view) => { for (const v of document.querySelectorAll('#panel .view')) v.classList.toggle('hidden', v.id !== 'view-' + view); };
const fail = (e) => { hideBoot(); status('Error: ' + e.message); $('panel').classList.remove('hidden'); show('loading'); };
const mb = (n) => (n / 1e6).toFixed(0);

// Rotating "intel" tips: a one-time 500 MB download and the first-mission load both take a minute, so the
// wait gets real game tips and a little lore instead of a bare progress bar. Each tip fades to the next.
const TIPS = [
  'You are David Llewellyn Jones — ex-SAS, going in alone.',
  'Open the map computer to see guard positions update in real time.',
  'Silence is a weapon: the knife and silenced pistol keep you unheard.',
  'Cut the alarm before a guard reaches the panel, or the whole base wakes up.',
  'Crouch to steady your aim and shrink your profile.',
  'Scout with the binoculars before you cross open ground.',
  'A headshot drops a guard before he can radio for backup.',
  'Lofted grenades clear a room you can’t safely walk into.',
  'Anya runs mission control from the air — the clock is always ticking.',
  'This is the real 2000 game: about 500 MB, downloaded to your device once.',
  'Nothing leaves your computer — the whole game runs inside your browser.',
  'The first start is the slow one: after this the game opens straight away.',
];
function startTips(box) {
  if (!box) return () => {};
  const textEl = box.querySelector('.tip-text');
  box.classList.remove('hidden');
  const reduce = matchMedia('(prefers-reduced-motion: reduce)').matches;
  let i = Math.floor(Math.random() * TIPS.length);
  const render = () => { textEl.textContent = TIPS[i % TIPS.length]; i++; };
  render();
  const next = () => {
    if (reduce) { render(); return; }
    box.style.opacity = '0';
    setTimeout(() => { render(); box.style.opacity = '1'; }, 350);
  };
  const id = setInterval(next, 4800);
  return () => { clearInterval(id); box.classList.add('hidden'); box.style.opacity = ''; };
}

// Boot overlay (index.html #booting): between the Play click and the first rendered frame the canvas is a
// black void while the game loads a mission into memory. Cover it with a radar, a phase line that escalates
// if it runs long, and tips, then remove it the moment the first frame is presented (run()).
let stopBootTips = null;
function showBoot() {
  const b = $('booting');
  if (!b) return;
  const phase = b.querySelector('.boot-phase');
  phase.textContent = 'Starting the game…';
  b.classList.remove('hidden');
  stopBootTips = startTips(b.querySelector('.tips'));
  // an ad under the loading text (a natural break: the first launch takes from seconds to minutes)
  try { const inner = b.querySelector('.boot-inner'); if (window.RcBreakAd && inner) {
    const phone = RcTouch.wanted(), w = phone ? 320 : 300;
    inner.style.paddingBottom = phone ? '62px' : '262px';
    window.RcBreakAd.show(inner, phone ? 'mobile' : 'box', (inner.clientWidth - w) / 2, inner.scrollHeight - (phone ? 56 : 256)); } } catch { /* no ads */ }
  showBoot.t1 = setTimeout(() => { phase.textContent = 'Loading the game into memory…'; }, 12000);
  showBoot.t2 = setTimeout(() => { phase.textContent = 'The first launch is the slow one — almost there…'; }, 45000);
  showBoot.t3 = setTimeout(() => { phase.textContent = 'Still working — a slow phone can take a couple of minutes…'; }, 90000);
  showBoot.t4 = setTimeout(hideBoot, 300000);   // safety: never trap the player behind the cover
  // A tap or click on the cover must NOT dismiss it (it used to: the canvas behind is still black, so the
  // player saw the loading screen "turn black"). It only answers with a reassuring line.
  b.onpointerdown = () => { phase.textContent = 'Still loading — nothing is stuck, the game is being set up…'; };
}
function hideBoot() {
  try { if (typeof gtag === 'function') gtag('event', 'game_start'); } catch { /* analytics blocked */ }
  const b = $('booting');
  for (const k of ['t1', 't2', 't3', 't4']) clearTimeout(showBoot[k]);
  if (stopBootTips) { stopBootTips(); stopBootTips = null; }
  if (b) { b.classList.add('hidden'); b.onpointerdown = null; const inner = b.querySelector('.boot-inner'); if (inner) inner.style.paddingBottom = ''; }
  try { window.RcBreakAd?.hide(); } catch { /* no ads */ }
}

// Mid-game loading cover: after boot, the game blocks hard (no frames presented) while it loads a selected
// mission or the next level — up to a couple of minutes on a phone, a black screen that looks frozen. The
// frame loop (run()) watches for that stall and shows this, then hides it the moment frames stream again.
let stopLoadTips = null;
function showLoading() {
  const b = $('booting');
  if (!b) return;
  const phase = b.querySelector('.boot-phase');
  phase.textContent = 'Loading mission…';
  b.classList.remove('hidden');
  stopLoadTips = startTips(b.querySelector('.tips'));
  showLoading.t1 = setTimeout(() => { phase.textContent = 'Loading mission… a phone can take a minute or two.'; }, 15000);
  showLoading.t2 = setTimeout(() => { phase.textContent = 'Still loading — hang tight, almost there…'; }, 50000);
  // A load that long is suspicious, but a slow phone loading a big level can take it: the game is only called
  // stuck once it has also opened or read no file for a minute (rc_io_activity, kernel32.c), which no load
  // does. Then, rather than a black screen that never ends, say so and offer the way out.
  let lastIo = -1, lastIoAt = performance.now();
  const t0 = performance.now();
  showLoading.t3 = setInterval(() => {
    const M = window.recomp, io = M && M._rc_io_activity ? M._rc_io_activity() : -1;
    const now = performance.now();
    if (io !== lastIo) { lastIo = io; lastIoAt = now; }
    if (now - t0 < 150000 || now - lastIoAt < 60000) {
      if (now - t0 > 150000) phase.textContent = 'Still loading — a big level on a slow phone. Hang tight…';
      return;
    }
    clearInterval(showLoading.t3);
    phase.innerHTML = 'The game has stopped responding. <button id="stuck-reload" style="font:inherit;padding:6px 16px;' +
      'border-radius:8px;border:0;background:#45cf79;color:#000;font-weight:700;margin-left:8px">Reload the game</button>';
    $('stuck-reload').onclick = () => location.reload();
  }, 10000);
}
function hideLoading() {
  const b = $('booting');
  for (const k of ['t1', 't2']) clearTimeout(showLoading[k]);
  clearInterval(showLoading.t3);
  if (stopLoadTips) { stopLoadTips(); stopLoadTips = null; }
  if (b) b.classList.add('hidden');
}

// The last lines the engine and the page printed, and the page's errors, kept for a start that shows no frame
// (bootlog, run()): older iPhones and iPads (iOS 16-17) pressed Play and the game never appeared, without a word.
const recentLog = [], recentErr = [];
const keepLog = (t) => { recentLog.push(String(t).slice(0, 140)); if (recentLog.length > 14) recentLog.shift(); };
const keepErr = (t) => { if (recentErr.length < 6) recentErr.push(String(t).slice(0, 160)); keepLog(t); };
window.addEventListener('error', (e) => keepErr('ERR ' + e.message + ' @' + String(e.filename || '').split('/').pop() + ':' + e.lineno));
window.addEventListener('unhandledrejection', (e) => keepErr('REJ ' + ((e.reason && (e.reason.message || e.reason)) || '')));
{ const ce = console.error; console.error = (...a) => { keepErr('CE ' + a.map(String).join(' ')); ce.apply(console, a); }; }

// On-screen diagnostics for phones (no devtools): ?debug shows a log overlay capturing errors, warnings,
// unhandled rejections and lost-GPU events, so a remote tester can screenshot exactly what failed (e.g. a
// mission that won't load). Harmless when off.
const DEBUG = params.has('debug');
(function setupDebugLog() {
  if (!DEBUG) return;
  const box = document.createElement('div');
  box.id = 'debuglog';
  box.style.cssText = 'position:fixed;left:0;right:0;bottom:0;max-height:45vh;overflow:auto;z-index:99999;' +
    'background:rgba(0,0,0,.85);color:#9fe6b0;font:11px/1.4 ui-monospace,Menlo,monospace;padding:8px 10px;' +
    'white-space:pre-wrap;word-break:break-word;border-top:2px solid #2ecc71';
  const add = (tag, args) => {
    const line = document.createElement('div');
    line.textContent = tag + ' ' + args.map((a) => (a && a.stack) || (typeof a === 'object' ? JSON.stringify(a) : String(a))).join(' ');
    if (tag === 'ERR') line.style.color = '#ff8b8b';
    box.appendChild(line); box.scrollTop = box.scrollHeight;
    while (box.childElementCount > 300) box.firstChild.remove();
  };
  document.addEventListener('DOMContentLoaded', () => document.body.appendChild(box));
  if (document.body) document.body.appendChild(box);
  const oe = console.error, ow = console.warn, ol = console.log;
  console.error = (...a) => { add('ERR', a); oe.apply(console, a); };
  console.warn = (...a) => { add('WARN', a); ow.apply(console, a); };
  console.log = (...a) => { add('log', a); ol.apply(console, a); };
  window.addEventListener('error', (e) => add('ERR', [e.message, e.filename + ':' + e.lineno]));
  window.addEventListener('unhandledrejection', (e) => add('ERR', ['unhandled rejection:', e.reason]));
  add('log', ['debug log on · ' + navigator.userAgent]);
})();

// Surface WebGPU failures (console.error, so the ?debug overlay shows them): a mission that exceeds a mobile
// GPU's buffer/texture limits, or a lost device (out of memory), otherwise fails silently to a black screen.
if (navigator.gpu && GPUAdapter.prototype.requestDevice) {
  const orig = GPUAdapter.prototype.requestDevice;
  GPUAdapter.prototype.requestDevice = async function (desc) {
    try {
      const dev = await orig.call(this, desc);
      try {
        dev.addEventListener('uncapturederror', (e) => console.error('WebGPU uncaptured error:', e.error?.message || e.error));
        dev.lost.then((info) => console.error('WebGPU device lost:', info.reason, info.message));
      } catch {}
      return dev;
    } catch (e) { console.error('WebGPU requestDevice failed:', e.message); throw e; }
  };
}

// ------------------------------------------------------------------ game files in OPFS

async function gameDir(create) {
  const root = await navigator.storage.getDirectory();
  return root.getDirectoryHandle(OPFS_DIR, { create });
}

async function readMarker() {
  try {
    const dir = await gameDir(false);
    const f = await (await dir.getFileHandle(MARKER)).getFile();
    const m = JSON.parse(await f.text());
    // A browser short of storage can evict the game's files but leave the marker (seen in a test profile:
    // "igi.exe: No such file", and the page waits forever). No executable: treat it as not installed. The
    // host maps names case-insensitively, so look through the folder rather than ask for one spelling.
    if (typeof dir.keys !== 'function') return m;   // no folder listing in this browser: trust the marker
    let exe = false;
    for await (const name of dir.keys()) if (/^igi\.exe$/i.test(name)) { exe = true; break; }
    if (!exe) { console.warn('the game folder has no executable: installing again'); return null; }
    return m;
  } catch { return null; }
}

async function writeMarker(version) {
  const fh = await (await gameDir(true)).getFileHandle(MARKER, { create: true });
  const w = await openWriter(fh, OPFS_DIR + '/' + MARKER);
  await w.write(JSON.stringify({ version, at: new Date().toISOString() }));
  await w.close();
}

async function fileHandle(dir, relPath, create) {
  const parts = relPath.split('/');
  for (const p of parts.slice(0, -1)) dir = await dir.getDirectoryHandle(p, { create });
  return dir.getFileHandle(parts[parts.length - 1], { create });
}

// A writer for one OPFS file, at a path under the OPFS root: { write(Uint8Array | Blob | string), close(), abort() }.
// Safari before 26 (iPhones and iPads on iOS 18 and older, Macs on Safari 18) has OPFS but no createWritable
// ("fh.createWritable is not a function", every download failed at its first file): there the bytes go to a
// worker that writes them through a sync access handle, the only writer those versions have (the engine's
// WASMFS reads and writes the game files the same way). ?nowritable takes that path anywhere (testing).
function opfsWriterWorker() {
  const open = new Map();   // id -> { h, at }
  const run = async (m) => {
    if (m.op === 'open') {
      let dir = await navigator.storage.getDirectory();
      const parts = m.path.split('/');
      for (const p of parts.slice(0, -1)) dir = await dir.getDirectoryHandle(p, { create: true });
      const h = await (await dir.getFileHandle(parts[parts.length - 1], { create: true })).createSyncAccessHandle();
      await h.truncate(0);   // Safari 15-16: these return promises; later ones return at once (await is harmless)
      open.set(m.id, { h, at: 0 });
      return;
    }
    const s = open.get(m.id);
    if (!s) throw new Error('file not open');
    if (m.op === 'write') {
      let d = m.data;
      if (typeof d === 'string') d = new TextEncoder().encode(d);
      if (d instanceof Blob) {   // a picked file: in pieces, not all of it in memory at once
        for (let o = 0; o < d.size; o += 4 << 20) {
          const piece = new Uint8Array(await d.slice(o, o + (4 << 20)).arrayBuffer());
          s.at += await s.h.write(piece, { at: s.at });
        }
      } else s.at += await s.h.write(d, { at: s.at });
      return;
    }
    open.delete(m.id);   // close or abort: release the file either way (a held handle locks it)
    try { if (m.op === 'close') await s.h.flush(); } finally { await s.h.close(); }
  };
  onmessage = (e) => run(e.data).then(() => postMessage({ seq: e.data.seq }),
    (err) => postMessage({ seq: e.data.seq, err: (err && err.name || 'Error') + ': ' + (err && err.message || err) }));
}
let opfsWorker = null, opfsSeq = 0;
const opfsWaiting = new Map();
function opfsCall(m, transfer) {
  if (!opfsWorker) {
    opfsWorker = new Worker(URL.createObjectURL(new Blob(['(' + opfsWriterWorker.toString() + ')()'], { type: 'text/javascript' })));
    opfsWorker.onmessage = (e) => {
      const p = opfsWaiting.get(e.data.seq);
      opfsWaiting.delete(e.data.seq);
      if (p) e.data.err ? p.no(new Error(e.data.err)) : p.ok();
    };
  }
  return new Promise((ok, no) => {
    m.seq = ++opfsSeq;
    opfsWaiting.set(m.seq, { ok, no });
    opfsWorker.postMessage(m, transfer || []);
  });
}
const noWritable = params.has('nowritable');
async function openWriter(fh, path) {
  if (!noWritable && typeof fh.createWritable === 'function') {
    const w = await fh.createWritable();
    return { write: (d) => w.write(d), close: () => w.close(), abort: () => w.abort().catch(() => {}) };
  }
  const id = ++opfsSeq;
  await opfsCall({ op: 'open', id, path });
  return {
    write: (d) => opfsCall({ op: 'write', id, data: d },
      d instanceof Uint8Array && d.byteOffset === 0 && d.byteLength === d.buffer.byteLength ? [d.buffer] : []),
    close: () => opfsCall({ op: 'close', id }),
    abort: () => opfsCall({ op: 'abort', id }).catch(() => {}),
  };
}

// installer leftovers the game never reads: not worth copying into the browser
const SKIP = /^(directx[^/]*\/|.*\.(mp4|ds_store)$)|(^|\/)\.DS_Store$/i;

async function importFromPicker(fileList) {
  // the whole folder, minus its own name: <picked>/a/b.dat -> a/b.dat
  const list = [...fileList].map((f) => ({ f, dest: f.webkitRelativePath.split('/').slice(1).join('/') }))
    .filter((e) => e.dest && !SKIP.test(e.dest));
  if (!list.some((e) => e.dest.toLowerCase() === 'igi.exe'))
    throw new Error('No igi.exe in that folder: pick the Project I.G.I. folder itself.');
  const root = await navigator.storage.getDirectory();
  try { await root.removeEntry(OPFS_DIR, { recursive: true }); } catch {}
  await navigator.storage.persist?.();
  const dir = await gameDir(true);
  const total = list.reduce((s, e) => s + e.f.size, 0);
  let done = 0;
  $('progress').classList.remove('hidden');
  for (const e of list) {
    status(`Copying ${e.dest} (${mb(done)} / ${mb(total)} MB)`);
    const w = await openWriter(await fileHandle(dir, e.dest, true), OPFS_DIR + '/' + e.dest);
    try { await w.write(e.f); await w.close(); } catch (err) { await w.abort(); throw err; }
    done += e.f.size;
    $('progress').value = done / total;
  }
  const sig = list.map((e) => `${e.dest}:${e.f.size}`).sort().join('\n');
  const digest = await crypto.subtle.digest('SHA-256', new TextEncoder().encode(sig));
  await writeMarker([...new Uint8Array(digest)].slice(0, 6).map((b) => b.toString(16).padStart(2, '0')).join(''));
  $('progress').classList.add('hidden');
}

// A hosted copy of the game folder: web/serve.py --game DIR serves it at game/, with game/manifest.json
// listing [{ path, size }]. Each file streams straight into OPFS (a phone couldn't hold 500 MB in memory),
// and files already there at the right size are kept, so an interrupted download resumes where it stopped.
async function hostedManifest() {
  try {
    const r = await fetch('game/manifest.json');
    if (!r.ok) return null;
    const list = (await r.json()).filter((e) => !SKIP.test(e.path));
    return list.some((e) => e.path.toLowerCase() === 'igi.exe') ? list : null;
  } catch { return null; }
}

// The game folder is served through a CDN (Cloudflare) that caches it on an edge near the player, so one
// host is already fast: the files are pulled by a pool of workers over the browser's single multiplexed
// HTTP/2 connection. Each streams straight into OPFS and files already at the right size are skipped, so an
// interrupted download resumes where it stopped.
const DL_CONCURRENCY = 10;
async function importFromServer(list) {
  const stopTips = startTips($('tips'));
  try {
    try { await importFromServerInner(list); }
    catch (err) {
      // A browser short of storage evicts files behind the folder's back: entries still listed can't be opened
      // ("could not be found at the time an operation was processed"), and a download over such a folder
      // fails the same way. Start over from an empty folder once; anything else (network) is reported.
      if (!(err && (err.name === 'NotFoundError' || /could not be found/.test(err.message || '')))) throw err;
      console.warn('the game folder is inconsistent, downloading again from scratch:', err.message);
      const root = await navigator.storage.getDirectory();
      await root.removeEntry(OPFS_DIR, { recursive: true }).catch(() => { /* already gone */ });
      await importFromServerInner(list);
    }
  } finally {
    stopTips();
  }
}
async function importFromServerInner(list) {
  await navigator.storage.persist?.();
  const dir = await gameDir(true);
  const total = list.reduce((s, e) => s + e.size, 0);
  let done = 0, shownAt = 0;
  const startT = performance.now();
  let baseDone = null, baseT = 0;   // a steady-state anchor (set after ~1.5s) so the ETA isn't noisy at the start
  $('progress').classList.remove('hidden');
  const etaText = (secs) => {
    secs = Math.round(secs);
    if (secs >= 110) return `about ${Math.round(secs / 60)} minutes left`;
    if (secs >= 50) return 'about a minute left';
    if (secs >= 18) return `about ${Math.round(secs / 10) * 10} seconds left`;
    return 'a few seconds left';
  };
  const shown = (force) => {
    const now = performance.now();
    if (!force && now - shownAt < 120) return;   // several workers report at once; don't thrash the DOM
    shownAt = now;
    status(`Loading necessary files to start the game… ${Math.floor((done / total) * 100)}%`);
    $('progress').value = done / total;
    if (baseDone === null && now - startT > 1500) { baseDone = done; baseT = now; }
    if (baseDone !== null && done < total) {
      const rate = (done - baseDone) / Math.max(0.5, (now - baseT) / 1000);   // bytes/s, steady-state
      // only a time estimate — not the download size (a big MB figure just worries people)
      if (rate > 60000) { $('eta').textContent = etaText((total - done) / rate); $('eta').classList.remove('hidden'); }
    }
  };
  // done is counted per completed file (not per chunk): several workers run at once, so a per-chunk counter
  // couldn't be reset cleanly when a file is retried. 1955 files make the per-file progress fine-grained.
  const sleep = (ms) => new Promise((res) => setTimeout(res, ms));

  // big files first, so the pool packs well and the slowest file isn't left for last
  const queue = list.slice().sort((a, b) => b.size - a.size);

  const fetchInto = async (e, fh) => {
    // no-store: don't also keep a copy in the browser's HTTP cache (OPFS is the permanent store); the CDN
    // edge cache still serves these, so repeat loads stay fast.
    // A connection the shared host leaves hanging never ends and never errors, and a download then stops at
    // some percentage for good (seen at 42% and 60%): abort a fetch that brings nothing for 30 s and retry it.
    const ac = new AbortController();
    let stallTimer = setTimeout(() => ac.abort(), 30000);
    const alive = () => { clearTimeout(stallTimer); stallTimer = setTimeout(() => ac.abort(), 30000); };
    try {
      const r = await fetch('game/' + e.path.split('/').map(encodeURIComponent).join('/'), { cache: 'no-store', signal: ac.signal });
      if (!r.ok) throw new Error(`HTTP ${r.status}`);
      const w = await openWriter(fh, OPFS_DIR + '/' + e.path);
      const reader = r.body.getReader();
      let wrote = 0;
      try {
        for (;;) {
          const { done: end, value } = await reader.read();
          if (end) break;
          alive();
          wrote += value.length;   // before the write: the worker path hands the bytes over
          await w.write(value);
        }
        await w.close();
      } catch (err) { await w.abort(); throw err; }
      if (wrote !== e.size) throw new Error(`short read ${wrote}/${e.size}`);
    } finally { clearTimeout(stallTimer); }
  };

  const worker = async () => {
    for (;;) {
      const e = queue.shift();
      if (!e) return;
      const fh = await fileHandle(dir, e.path, true);
      if ((await fh.getFile()).size === e.size) { done += e.size; shown(); continue; }
      // Fetch with retries: a dropped or truncated connection (ERR_CONTENT_LENGTH_MISMATCH, a short body) must
      // not leave a wrong-sized file that later breaks a mission — re-fetch until the written size is exact.
      let ok = false, lastErr;
      for (let attempt = 0; attempt < 6 && !ok; attempt++) {
        try { await fetchInto(e, fh); ok = true; }
        catch (err) { lastErr = err; if (attempt < 5) await sleep(400 * (attempt + 1)); }
      }
      if (!ok) throw new Error(`Download failed at ${e.path} (${lastErr?.message}). Reload the page to resume where it stopped.`);
      done += e.size; shown();
    }
  };
  // a pool of workers over the one connection; if any throws it surfaces (and the others settle) so a reload can resume
  const workers = Array.from({ length: Math.min(DL_CONCURRENCY, list.length) }, () => worker());
  await Promise.all(workers);
  shown(true);
  const sig = list.map((e) => `${e.path}:${e.size}`).sort().join('\n');
  const digest = await crypto.subtle.digest('SHA-256', new TextEncoder().encode(sig));
  await writeMarker([...new Uint8Array(digest)].slice(0, 6).map((b) => b.toString(16).padStart(2, '0')).join(''));
  $('progress').classList.add('hidden');
  $('eta').classList.add('hidden');
}

// ------------------------------------------------------------------ input

// KeyboardEvent.code -> USB HID usage (what host/input.c's key table uses)
const HID = {};
for (let i = 0; i < 26; i++) HID['Key' + String.fromCharCode(65 + i)] = 4 + i;
for (let i = 1; i <= 9; i++) HID['Digit' + i] = 29 + i;
HID.Digit0 = 39;
Object.assign(HID, {
  Enter: 40, Escape: 41, Backspace: 42, Tab: 43, Space: 44, Minus: 45, Equal: 46, BracketLeft: 47, BracketRight: 48,
  Backslash: 49, Semicolon: 51, Quote: 52, Backquote: 53, Comma: 54, Period: 55, Slash: 56, CapsLock: 57,
  PrintScreen: 70, ScrollLock: 71, Pause: 72, Insert: 73, Home: 74, PageUp: 75, Delete: 76, End: 77, PageDown: 78,
  ArrowRight: 79, ArrowLeft: 80, ArrowDown: 81, ArrowUp: 82, NumLock: 83, NumpadDivide: 84, NumpadMultiply: 85,
  NumpadSubtract: 86, NumpadAdd: 87, NumpadEnter: 88, Numpad0: 98, NumpadDecimal: 99,
  ControlLeft: 224, ShiftLeft: 225, AltLeft: 226, MetaLeft: 227, ControlRight: 228, ShiftRight: 229, AltRight: 230, MetaRight: 231,
});
for (let i = 1; i <= 12; i++) HID['F' + i] = 57 + i;
for (let i = 1; i <= 9; i++) HID['Numpad' + i] = 88 + i;

// Netplay: the keyboard drives this player's pad (the game itself gets no keys or mouse, which would desync).
// Bit 4 is LB, which the game sees as L1: its modifier for lofted shots (the in-game tutorial: "L1 + shot
// button" to hit a six; on the keyboard that's Shift), so Shift holds it as well as Q. The game pauses only
// on the keyboard's Esc, not the pad's Start, so Esc is bit 20: host/netplay.c applies it to the DirectInput
// keyboard on every peer, with the pad inputs (NP_KEY_ESC). Backspace is B, back in menus.
const NET_KEYS = {   // KeyboardEvent.code -> standard-gamepad bit (20: Esc key)
  Enter: 0, Space: 0, KeyS: 0, Escape: 20, Backspace: 1, KeyD: 1, KeyA: 2, KeyW: 3, KeyQ: 4, KeyE: 5,
  ShiftLeft: 4, ShiftRight: 4,
  Digit1: 6, Digit3: 7, Tab: 8, KeyP: 9, ArrowUp: 12, ArrowDown: 13, ArrowLeft: 14, ArrowRight: 15,
};
let netKeyBits = 0;
let netKeyPressed = 0;   // keys pressed since the last poll, so a tap shorter than a frame still counts

// ---- Single-player key remapping ----------------------------------------------------------------
// The game's own control bindings (its config.qvm) stay at their defaults; here we translate the
// player's chosen physical keys onto the game's default key for each action, entirely in the browser
// and saved per browser (localStorage), so keys can be rebound without touching the game. Netplay is
// unaffected: online, the keyboard only drives the pad (above), never raw keys.
// Two layouts ("presets"): the game's ORIGINAL keys (arrows, Right Shift, Right Ctrl, numpad; what the game's
// own Configuration screen shows, and what players of the 2000 game remember) and a MODERN WASD layout. The
// original one is the default for new players, as the game ships; a few extra keys stand in for the numpad
// and Right Ctrl that laptops don't have. Keys not listed here reach the game unchanged in both layouts.
// id, label, { original, modern }: default physical KeyboardEvent.code per preset (null = unbound), the game's
// default key for that action (its code), group
const CONTROLS = [
  ['forward', 'Move forward',   { original: 'ArrowUp',      modern: 'KeyW' },        'ArrowUp',      'Movement'],
  ['back',    'Move backward',  { original: 'ArrowDown',    modern: 'KeyS' },        'ArrowDown',    'Movement'],
  ['left',    'Strafe left',    { original: 'ArrowLeft',    modern: 'KeyA' },        'ArrowLeft',    'Movement'],
  ['right',   'Strafe right',   { original: 'ArrowRight',   modern: 'KeyD' },        'ArrowRight',   'Movement'],
  ['walkrun', 'Walk / run',     { original: 'KeyW',         modern: 'ShiftLeft' },   'KeyW',         'Movement'],
  ['crouch',  'Crouch',         { original: 'ControlRight', modern: 'ControlLeft' }, 'ControlRight', 'Movement'],
  ['crouch2', 'Crouch (alt)',   { original: 'ControlLeft',  modern: null },          'ControlRight', 'Movement'],
  ['jump',    'Jump',           { original: 'Numpad0',      modern: 'Space' },       'Numpad0',      'Movement'],
  ['jump2',   'Jump (alt)',     { original: 'KeyX',         modern: null },          'Numpad0',      'Movement'],
  ['peek',    'Peek',           { original: 'Numpad1',      modern: 'KeyQ' },        'Numpad1',      'Movement'],
  ['peek2',   'Peek (alt)',     { original: 'KeyQ',         modern: 'KeyE' },        'Numpad1',      'Movement'],
  ['reload',  'Reload',         { original: 'Enter',        modern: 'KeyR' },        'Enter',        'Weapons & action'],
  ['zoomin',  'Zoom in',        { original: 'PageUp',       modern: 'Digit1' },      'PageUp',       'Weapons & action'],
  ['zoomout', 'Zoom out',       { original: 'PageDown',     modern: 'Digit3' },      'PageDown',     'Weapons & action'],
  ['use',     'Use / activate', { original: 'ShiftRight',   modern: 'Tab' },         'ShiftRight',   'Weapons & action'],
  ['bino',    'Binoculars',     { original: 'Space',        modern: 'KeyB' },        'Space',        'Weapons & action'],   // in the modern layout Space is Jump
];
const PRESETS = { original: 'Original (as the game ships)', modern: 'Modern (WASD)' };
const CONTROL_STORE = 'igi-controls';
const PRESET_STORE = 'igi-preset';
let preset = 'original';
// how a KeyboardEvent.code reads on screen
function keyLabel(code) {
  if (!code) return '—';
  const m = {
    ArrowUp: '↑', ArrowDown: '↓', ArrowLeft: '←', ArrowRight: '→', ControlLeft: 'L-Ctrl', ControlRight: 'R-Ctrl',
    ShiftLeft: 'L-Shift', ShiftRight: 'R-Shift', AltLeft: 'L-Alt', AltRight: 'R-Alt', Space: 'Space',
    Enter: 'Enter', NumpadEnter: 'Num ⏎', Tab: 'Tab', Escape: 'Esc', Backspace: '⌫', CapsLock: 'Caps',
    PageUp: 'PgUp', PageDown: 'PgDn', Home: 'Home', End: 'End', Insert: 'Ins', Delete: 'Del',
    Backquote: '`', Minus: '-', Equal: '=', BracketLeft: '[', BracketRight: ']', Backslash: '\\',
    Semicolon: ';', Quote: "'", Comma: ',', Period: '.', Slash: '/',
  };
  if (m[code]) return m[code];
  if (code.startsWith('Key')) return code.slice(3);
  if (code.startsWith('Digit')) return code.slice(5);
  if (code.startsWith('Numpad')) return 'Num ' + code.slice(6);
  return code;
}
let userKeys = {};   // id -> physical KeyboardEvent.code
let keyRemap = {};   // physical code -> HID usage to send to the game
function rebuildRemap() {
  keyRemap = {};
  for (const [id, , , target] of CONTROLS) {
    const phys = userKeys[id], u = HID[target];
    if (phys && u !== undefined) keyRemap[phys] = u;
  }
}
function loadControls() {
  let saved = {};
  try { saved = JSON.parse(localStorage.getItem(CONTROL_STORE) || '{}') || {}; } catch { saved = {}; }
  try { if (PRESETS[localStorage.getItem(PRESET_STORE)]) preset = localStorage.getItem(PRESET_STORE); } catch { /* storage blocked */ }
  userKeys = {};
  for (const [id, , def] of CONTROLS) userKeys[id] = typeof saved[id] === 'string' || saved[id] === null ? saved[id] : def[preset];
  rebuildRemap();
}
function saveControls() {
  try { localStorage.setItem(CONTROL_STORE, JSON.stringify(userKeys)); localStorage.setItem(PRESET_STORE, preset); } catch { /* storage blocked */ }
}
function applyPreset(name) {
  preset = PRESETS[name] ? name : 'original';
  for (const [id, , def] of CONTROLS) userKeys[id] = def[preset];
  saveControls(); rebuildRemap();
}
// Players without a saved choice get the Modern (WASD) layout: earlier players were moved to WASD, new ones got the
// game's own keys (arrows and the numpad, which most laptops lack), so two players saw different "defaults"
// (2026-10-31). A saved choice is never changed.
function migratePreset(installed) {
  let had = false;
  try { had = localStorage.getItem(PRESET_STORE) !== null; } catch { /* storage blocked */ }
  if (had) return;
  let custom = false;
  try { custom = localStorage.getItem(CONTROL_STORE) !== null; } catch { /* storage blocked */ }
  if (custom) { preset = 'modern'; saveControls(); loadControls(); }
  else applyPreset('modern');
  void installed;
  setupControls.render?.();
}
loadControls();

// The keyboard panel (index.html #kbd) doubles as the rebind UI: click a key, then press the new one.
function setupControls() {
  const host = $('kbd-binds');
  if (!host) return;
  let capturing = null;   // id being rebound
  const groups = [];
  for (const c of CONTROLS) {
    let g = groups.find((x) => x.name === c[4]);
    if (!g) groups.push(g = { name: c[4], items: [] });
    g.items.push(c);
  }
  const render = () => {
    host.innerHTML = '<h3>Keyboard layout</h3><div class="seg kbd-preset">' +
      Object.entries(PRESETS).map(([k, label]) => `<button type="button" data-preset="${k}"${preset === k ? ' class="on"' : ''}>${label}</button>`).join('') +
      '</div><p class="kbd-note">' + (preset === 'original'
        ? 'The keys the game ships with. No numpad or right Ctrl on your laptop? X jumps, left Ctrl crouches, Q peeks — or click any key to change it.'
        : 'WASD, Shift walk, Ctrl crouch, Space jump, R reload. Click any key to change it.') + '</p>' +
      groups.map((g) =>
      `<h3>${g.name}</h3>` + g.items.map(([id, label]) =>
        `<div class="row"><span class="label">${label}</span>` +
        `<button class="kbd-rebind${capturing === id ? ' capturing' : ''}" data-id="${id}">` +
        `${capturing === id ? 'press…' : keyLabel(userKeys[id])}</button></div>`).join('')).join('');
    host.querySelectorAll('.kbd-rebind').forEach((b) => {
      b.onclick = () => { capturing = capturing === b.dataset.id ? null : b.dataset.id; render(); };
    });
    host.querySelectorAll('.kbd-preset button').forEach((b) => {
      b.onclick = () => { applyPreset(b.dataset.preset); capturing = null; render(); };
    });
  };
  setupControls.render = render;
  // While rebinding, grab the next key in the capture phase so it reaches neither the page nor the game.
  window.addEventListener('keydown', (e) => {
    if (!capturing) return;
    e.preventDefault(); e.stopPropagation();
    if (e.code === 'Backspace' || e.code === 'Delete') { userKeys[capturing] = null; saveControls(); rebuildRemap(); }   // unbind
    else if (e.code !== 'Escape' && HID[e.code] !== undefined) { userKeys[capturing] = e.code; saveControls(); rebuildRemap(); }
    capturing = null; render();
  }, true);
  // Mouse buttons and sensitivity (controls.js): which action each button does in a mission, and a multiplier
  // on the mouse motion on top of the game's own sensitivity setting.
  const mouseHost = $('kbd-mouse');
  const renderMouse = () => {
    if (!mouseHost) return;
    const C = RcControls, s = C.get();
    const sel = (btn) => `<select data-btn="${btn}">` + C.MOUSE_CHOICES.map((a) =>
      `<option value="${a}"${s.mouse[btn] === a ? ' selected' : ''}>${C.ACTIONS[a].label}</option>`).join('') + '</select>';
    mouseHost.innerHTML = '<h3>Screen</h3><div class="seg kbd-preset kbd-wide">' +
      `<button type="button" data-wide="1"${s.wide ? ' class="on"' : ''}>Full screen (wide)</button>` +
      `<button type="button" data-wide="0"${!s.wide ? ' class="on"' : ''}>Original 4:3</button></div>` +
      '<p class="kbd-note">Wide: the 3D view fills a widescreen display with a wider field of view; menus stay 4:3.' +
      (s.wide && !COHORT.on.wide ? ' <b>Starts with the next start of the game: reload the page and press Play.</b>' : '') + '</p>' +
      '<h3>Mouse</h3>' +
      `<div class="row"><span class="label">Left</span>${sel(0)}</div>` +
      `<div class="row"><span class="label">Right</span>${sel(2)}</div>` +
      `<div class="row"><span class="label">Middle</span>${sel(1)}</div>` +
      '<div class="row"><span class="label">Scroll</span><div class="keys">next / prev weapon</div></div>' +
      `<div class="row"><span class="label">Sensitivity</span><output data-for="mouseSens">${s.mouseSens.toFixed(1)}×</output></div>` +
      `<input class="kbd-range" type="range" min="0.2" max="3" step="0.1" value="${s.mouseSens}" data-k="mouseSens" aria-label="Mouse sensitivity">` +
      `<div class="row"><span class="label">Scoped</span><output data-for="scopeSens">${s.scopeSens.toFixed(1)}×</output></div>` +
      `<input class="kbd-range" type="range" min="0.3" max="3" step="0.1" value="${s.scopeSens}" data-k="scopeSens" aria-label="Scoped sensitivity">` +
      '<p class="kbd-note">Scoped: while zoomed in (scope or binoculars), on top of the game\'s own slow-down by the zoom.</p>';
    mouseHost.querySelectorAll('select').forEach((e) => { e.onchange = () => RcControls.setMouse(Number(e.dataset.btn), e.value); });
    mouseHost.querySelectorAll('.kbd-wide button').forEach((b) => { b.onclick = () => { RcControls.set('wide', b.dataset.wide === '1'); renderMouse(); }; });
    mouseHost.querySelectorAll('input[type=range]').forEach((range) => {
      range.oninput = () => { RcControls.set(range.dataset.k, Number(range.value)); mouseHost.querySelector(`output[data-for="${range.dataset.k}"]`).textContent = Number(range.value).toFixed(1) + '×'; };
    });
  };
  const reset = $('kbd-reset');
  if (reset) reset.onclick = () => {
    applyPreset(preset); capturing = null; render();
    RcControls.reset(); renderMouse();
  };
  render();
  renderMouse();
}

// Larger subtitles on phones: the engine reads the game's speech back from the glyphs it draws (host/subtitle.c:
// rc_subtitle, and rc_subtitle_box / rc_subtitle_color for where and in which colour). The page draws the same text
// in the same place, over the game's own lines, in the game's colour, only larger: on the black band in cutscenes,
// in a panel like the game's in play. Only with the touch controls (on a PC the game's text is readable); the
// setting "Large subtitles" turns it off.
function subtitles(M, touch) {
  const box = $('subcap');
  if (!box || !M._rc_subtitle || !touch) return;
  let last = '', lastSeen = 0, lastBox = '';
  const dec = new TextDecoder('latin1');
  const canvas = $('canvas'), stage = $('stage');
  setInterval(() => {
    let text = '', b = null, col = 0;
    try {
      if (RcControls.get().subs) {
        const p = M._rc_subtitle(), mem = new Uint8Array(M.wasmMemory.buffer, p, 512);
        const end = mem.indexOf(0);
        text = dec.decode(mem.slice(0, end < 0 ? 512 : end));
        if (text && M._rc_subtitle_box) { b = Array.from(new Float32Array(M.wasmMemory.buffer, M._rc_subtitle_box(), 5)); col = M._rc_subtitle_color() >>> 0; }
      }
    } catch { text = ''; }
    const now = performance.now();
    if (text) lastSeen = now;
    if (!text) { if (last && now - lastSeen > 500) { last = ''; box.classList.add('hidden'); } return; }
    // the game's 640x480-style screen is a 4:3 box centred in the canvas (letterboxed, or the HUD box in wide mode)
    const r = canvas.getBoundingClientRect(), S = stage.getBoundingClientRect();
    const h = Math.min(r.height, (r.width * 3) / 4), w = (h * 4) / 3;
    const ox = r.left - S.left + (r.width - w) / 2, oy = r.top - S.top + (r.height - h) / 2;
    const geo = b ? b.map((v) => v.toFixed(3)).join() + '|' + Math.round(w) : '';
    if (text === last && geo === lastBox) return;
    last = text; lastBox = geo;
    const rgb = [(col >> 16) & 255, (col >> 8) & 255, col & 255];
    const green = rgb[1] > 120 && rgb[1] > rgb[0] * 1.4;   // in play the game writes in green in a green panel
    box.className = green ? 'panel' : 'band';
    box.style.color = col ? `rgb(${rgb.join()})` : '';
    const fs = Math.max(14, Math.min(22, h * 0.04));        // about twice the game's size on a phone
    box.style.fontSize = `${fs}px`;
    let x0 = ox + w * 0.05, y0 = oy + h * 0.85, width = w * 0.9, room = h * 0.15;
    if (b && b[2] > b[0]) {
      x0 = Math.max(ox, ox + b[0] * w - 4); y0 = Math.max(oy, oy + b[1] * h - 3);
      width = Math.min(w - (x0 - ox) - 2, Math.max((b[2] - b[0]) * w + 8, w * 0.5));
      // the room the game's own block leaves: to the bottom of the picture on the cutscene band, else twice its height
      room = green ? Math.max((b[3] - b[1]) * h * 2.2, h * 0.16) : oy + h - y0;
    }
    box.style.left = `${x0}px`; box.style.top = `${y0}px`; box.style.width = `${width}px`; box.style.transform = 'none';
    box.textContent = text.replace(/\n/g, ' ');
    // as large as fits that room (about twice the game's size when it fits), never below 14 px (then it grows upwards)
    for (let f = fs; f >= 14; f -= 1) { box.style.fontSize = `${f}px`; if (box.scrollHeight <= room + 1) break; }
    if (box.scrollHeight > room + 1 && !green) box.style.top = `${Math.max(oy, oy + h - box.scrollHeight)}px`;   // still too tall: grow upwards
  }, 120);
}

// An ad while the pause menu the PLAYER opened is up (Esc / MENU pressed in play, then the game paused), never in
// cutscenes or loading (the game reads as paused there too). Placed in the free part of the game's 4:3 picture:
// right of the menu box when there is room for 300x250, else under it (728x90 or 320x50). Gone on resume.
function pauseAds(touch) {
  if (!window.RcBreakAd) return;
  let escAt = 0, wasPlaying = false, on = false;
  const markEsc = () => { if (RcControls.inMission() && !RcControls.paused()) { escAt = performance.now(); wasPlaying = true; } };
  window.addEventListener('keydown', (e) => { if (e.code === 'Escape') markEsc(); }, true);
  pauseAds.menuPressed = markEsc;   // the touch MENU button (touch.js through RcControls.act('menu'))
  const canvas = $('canvas'), stage = $('stage');
  setInterval(() => {
    const paused = RcControls.paused();
    if (on && !paused) { on = false; window.RcBreakAd.hide(); return; }
    if (!on && paused && wasPlaying && performance.now() - escAt < 2000) {
      wasPlaying = false; on = true;
      const r = canvas.getBoundingClientRect(), S = stage.getBoundingClientRect();
      const h = Math.min(r.height, (r.width * 3) / 4), w = (h * 4) / 3;
      const ox = r.left - S.left + (r.width - w) / 2, oy = r.top - S.top + (r.height - h) / 2;
      const right = w * (1 - 483 / 640), below = h * (1 - 397 / 480);   // the game's Esc menu box: x 157-483, y 93-397 of 640x480
      if (!touch && right >= 316) window.RcBreakAd.show(stage, 'box', ox + w * 483 / 640 + (right - 300) / 2, oy + h * 93 / 480);
      else if (!touch && below >= 96 && w >= 740) window.RcBreakAd.show(stage, 'lead', ox + (w - 728) / 2, oy + h * 397 / 480 + (below - 90) / 2);
      else if (below >= 54 && w >= 330) window.RcBreakAd.show(stage, 'mobile', ox + (w - 320) / 2, oy + h * 397 / 480 + (below - 50) / 2);
      else on = false;
    }
  }, 300);
}

function hookInput(M, net) {
  const canvas = $('canvas');
  if (net) {
    const key = (down) => (e) => {
      const bit = NET_KEYS[e.code];
      if (bit === undefined) return;
      e.preventDefault();
      if (down) { netKeyBits |= 1 << bit; netKeyPressed |= 1 << bit; } else netKeyBits &= ~(1 << bit);
      pollLocalPad(M);   // straight to the game: waiting for the next animation frame would add up to 16 ms
    };
    window.addEventListener('keydown', key(true));
    window.addEventListener('keyup', key(false));
    window.addEventListener('blur', () => { netKeyBits = 0; pollLocalPad(M); });
    canvas.addEventListener('contextmenu', (e) => e.preventDefault());
    return;
  }
  const C = RcControls;
  C.attach(M);
  const key = (down) => (e) => {
    const u = keyRemap[e.code] !== undefined ? keyRemap[e.code] : HID[e.code];
    if (u === undefined || e.repeat) return;
    if (!(e.metaKey && !down)) e.preventDefault();
    C.keyUsage(u, down);   // a weapon key drops the scope state (controls.js)
    M._input_key_hid(u, down ? 1 : 0);
  };
  window.addEventListener('keydown', key(true));
  window.addEventListener('keyup', key(false));
  // The game draws its own cursor and reads relative motion. Lock the pointer when the browser allows
  // it (then the system cursor can't leave the game); without a lock, motion over the canvas still works.
  // Each button does the action the player bound to it (controls.js; the Controls panel): in a mission
  // only, the menus always get the plain click. The action a press started is what its release ends, even
  // if the binding or the mission state changed in between.
  const held = {};
  canvas.addEventListener('mousedown', (e) => {
    if (document.pointerLockElement !== canvas) {
      const p = canvas.requestPointerLock?.();
      if (p && p.catch) p.catch(() => {});
    }
    if (held[e.button]) C.act(held[e.button], false);
    held[e.button] = C.mouseAction(e.button);
    C.act(held[e.button], true);
  });
  window.addEventListener('mouseup', (e) => { if (held[e.button]) { C.act(held[e.button], false); held[e.button] = null; } });
  // Without the pointer lock the mouse stops at the window's edge, so the view can only turn so far ("the
  // mouse only works 180 degrees"). Esc (the game's menu) always releases the lock, and not every player
  // clicks again afterwards: take it back on the next key press (a user gesture, which the lock needs) while
  // a mission is running, and say so on screen meanwhile.
  const relock = () => {
    if (!C.inMission() || C.paused() || document.pointerLockElement === canvas) return;
    const p = canvas.requestPointerLock?.();
    if (p && p.catch) p.catch(() => {});
  };
  window.addEventListener('keydown', (e) => { if (!e.repeat && e.code !== 'Escape') relock(); });
  const locktip = $('locktip');
  if (locktip && !RcTouch.wanted()) setInterval(() => {
    locktip.classList.toggle('hidden', !(C.inMission() && !C.paused() && document.pointerLockElement !== canvas && document.hasFocus()));
  }, 500);
  // Relative motion, scaled by the mouse sensitivity setting in a mission (fractions carry over, so 1 = the
  // raw counts as before). The engine (dinput.c) inverts look-Y only during a mission, so the 2D menus keep
  // their normal cursor direction.
  canvas.addEventListener('mousemove', (e) => {
    if (!C.inMission()) { M._input_mouse_move(e.movementX, e.movementY); return; }
    const k = C.get().mouseSens * C.lookScale();   // slower while scoped, in step with the zoom
    C.look(e.movementX * k, e.movementY * k);
  });
  canvas.addEventListener('contextmenu', (e) => e.preventDefault());
  // Mouse wheel cycles weapons: up = next (the game's right-click), down = previous (its O key). A short
  // lock keeps a fast flick from stacking presses; each is held ~100 ms so a game frame samples it.
  let wheelLock = 0;
  canvas.addEventListener('wheel', (e) => {
    e.preventDefault();
    const now = performance.now();
    if (now < wheelLock) return;
    wheelLock = now + 140;
    C.weaponChanged();
    if (e.deltaY < 0) { M._input_mouse_button(1, 1); setTimeout(() => M._input_mouse_button(1, 0), 100); }
    else { M._input_key_hid(18, 1); setTimeout(() => M._input_key_hid(18, 0), 100); }
  }, { passive: false });
}

// Gamepads (Gamepad API, polled every animation frame). Browsers only expose a pad after a button press
// on it. The game sees each pad as a DirectInput "Logitech Dual Action USB" (host/dinput.c); ?pads=N exposes
// N of them (default 1, at most 4). Like the native feeder (host/window.c), slots are DirectInput pad
// numbers handed out in connection order, not the browser's gamepad.index (which can start above 0, e.g.
// after a reconnect), and a pad keeps its slot until it is unplugged.
// With mapping 'standard' buttons and axes are already in the layout input_gamepad() takes. Other pads
// (older DirectInput-era ones such as the Logitech Precision) report buttons in HID order, as Windows
// DirectInput presents them and as the game's controltypes.csv expects; their axes are indexed by HID
// usage (X Y Z Rx Ry Rz ...), so the right stick is Z/Rz and Chrome puts the hat switch at axes[9].
const PAD_SLOTS = 4;
const padLast = Array.from({ length: PAD_SLOTS }, () => '');
const padSlotIndex = Array(PAD_SLOTS).fill(-1);   // slot -> gamepad.index
const padHats = new Set();   // gamepad.index of pads whose axes[9] is a hat switch
// raw button n -> standard-layout bit that host/dinput.c turns back into DirectInput button n
const STD_FOR_RAW = [2, 0, 1, 3, 4, 5, 6, 7, 9, 8, 10, 11];
// Chrome's hat axis: -1 = up, then clockwise in steps of 2/7; anything outside [-1, 1] is centred
const HAT_DIRS = [1 << 12, (1 << 12) | (1 << 15), 1 << 15, (1 << 13) | (1 << 15), 1 << 13, (1 << 13) | (1 << 14), 1 << 14, (1 << 12) | (1 << 14)];
const axis16 = (v) => Math.round(Math.max(-1, Math.min(1, v || 0)) * 32767);

function readPad(g) {
  let buttons = 0;
  if (g.mapping === 'standard') {
    g.buttons.forEach((b, i) => { if (i < 17 && (b.pressed || b.value > 0.3)) buttons |= 1 << i; });
    return { buttons, ax: [0, 1, 2, 3].map((i) => axis16(g.axes[i])) };
  }
  g.buttons.forEach((b, i) => { if (i < STD_FOR_RAW.length && (b.pressed || b.value > 0.3)) buttons |= 1 << STD_FOR_RAW[i]; });
  // axes[9] is only taken as a hat once it has shown the centred value, so a plain axis resting at 0 isn't
  // read as d-pad down
  const hat = g.axes[9];
  if (hat > 1.1) padHats.add(g.index);
  else if (padHats.has(g.index) && hat >= -1.01) buttons |= HAT_DIRS[Math.round((hat + 1) * 3.5) % 8];
  const ry = g.axes.length > 5 ? g.axes[5] : g.axes[3];
  return { buttons, ax: [axis16(g.axes[0]), axis16(g.axes[1]), axis16(g.axes[2]), axis16(ry)] };
}

// Netplay: one local pad, the first connected gamepad merged with the keyboard (host/netplay.c sends it).
// Called from the key handlers and from a 1 ms timer (not just the animation frame), so a change reaches
// the game thread within a few ms wherever it is in its frame.
let netLast = '';
function pollLocalPad(M) {
  const g = (navigator.getGamepads ? [...navigator.getGamepads()] : []).find((p) => p && p.connected);
  const { buttons, ax } = g ? readPad(g) : { buttons: 0, ax: [0, 0, 0, 0] };
  const b = buttons | netKeyBits | netKeyPressed;
  netKeyPressed = 0;
  const key = `${b}:${ax.join(',')}`;
  if (key === netLast) return;
  netLast = key;
  M._input_gamepad(0, 1, b, ax[0], ax[1], ax[2], ax[3]);
}

function pollGamepads(M) {
  const pads = navigator.getGamepads ? [...navigator.getGamepads()].filter((g) => g && g.connected) : [];
  // free the slots of unplugged pads, then give new pads the lowest free slot
  for (let s = 0; s < PAD_SLOTS; s++)
    if (padSlotIndex[s] >= 0 && !pads.some((g) => g.index === padSlotIndex[s])) { padHats.delete(padSlotIndex[s]); padSlotIndex[s] = -1; }
  for (const g of pads) {
    if (padSlotIndex.includes(g.index)) continue;
    const s = padSlotIndex.indexOf(-1);
    if (s < 0) break;
    padSlotIndex[s] = g.index;
    console.log(`gamepad ${g.index} -> pad ${s}: ${g.id} (${g.mapping || 'non-standard'} mapping, ${g.buttons.length} buttons, ${g.axes.length} axes)`);
  }
  for (let slot = 0; slot < PAD_SLOTS; slot++) {
    const g = pads.find((p) => p.index === padSlotIndex[slot]);
    const { buttons, ax } = g ? readPad(g) : { buttons: 0, ax: [0, 0, 0, 0] };
    const connected = g ? 1 : 0;
    const key = `${connected}:${buttons}:${ax.join(',')}`;
    if (key === padLast[slot]) continue;
    padLast[slot] = key;
    M._input_gamepad(slot, connected, buttons, ax[0], ax[1], ax[2], ax[3]);
  }
}

// ------------------------------------------------------------------ audio

// The context is made by run() while the Play click still counts as a user gesture, so it usually starts
// right away. Otherwise (?autoplay, a browser that wants a gesture on the game itself) it waits for the next
// click or key on the page, and a hint says so.
function newAudioContext() {
  if (params.has('mute') || !window.AudioContext) return null;
  try { return new AudioContext({ sampleRate: 44100, latencyHint: 'interactive' }); } catch { return null; }
}

async function startAudio(M, ctx) {
  if (!ctx) return;
  await ctx.audioWorklet.addModule('audio-worklet.js');
  const node = new AudioWorkletNode(ctx, 'rc-ring', { outputChannelCount: [2] });
  node.port.postMessage({ memory: M.wasmMemory, ring: M._rc_audio_ring() });
  node.connect(ctx.destination);
  const tip = $('soundtip');
  const sync = () => tip.classList.toggle('hidden', ctx.state === 'running' || ctx.state === 'closed');
  const resume = () => { if (ctx.state !== 'running') ctx.resume().then(sync, sync); };
  // capture phase, so no other handler can keep the gesture from reaching here
  for (const ev of ['pointerdown', 'mousedown', 'keydown', 'touchend'])
    window.addEventListener(ev, resume, { capture: true });
  ctx.onstatechange = () => { sync(); if (ctx.state === 'interrupted') resume(); };   // Safari: interrupted
  resume();
  sync();
}

// ------------------------------------------------------------------ run

const isWebKit = () => /AppleWebKit\//.test(navigator.userAgent) && !/Chrome\/|Chromium\/|Edg\//.test(navigator.userAgent);
// The compact layout (runtime.c RC_MEM_COMPACT) is now the default everywhere, not only on WebKit: with the
// full layout the engine's memory is grown to ~1.8 GB at start (reserved for the game's 32-bit address space,
// mostly never touched), and on Windows that whole size counts as committed memory, which is what pushes an
// 8 GB machine with other things open into paging (the field reports' minute-long whole-process freezes).
// Compact keeps it at ~0.5 GB and grows it only as the game allocates. ?mem=full restores the old layout.
// (2026-10-31: back to WebKit only. Compact on every browser went live with recomp15 on the morning of
// 2026-10-30, the morning players started reporting black screens on Android; reverted until proven on phones.)
function compactMemory() {
  const want = params.get('mem');
  return want === 'compact' || (want !== 'full' && (isWebKit() || COHORT.on.compact));
}

// The browser dropped the WebGPU device: its GPU process died or restarted (Android Chrome does it when a system
// dialog or another app covers the browser), or the page was pushed out of GPU memory. The renderer rebuilds itself
// on a new device (render_wgpu.c rc_gpu_recover) and the game carries on where it was; this shows a short note
// meanwhile. Only if no new device comes (rcRecoverFailed, or nothing within 20 s while the page is visible) the
// old prompt to reload appears.
let gpuLostAt = 0, gpuFailTimer = 0, gpuReports = 0;   // reports per page: at most 3 of each (a looping GPU flooded crash.php)
const gpuBeacon = (type, extra) => {
  if (++gpuReports > 6) return;
  try {
    navigator.sendBeacon('crash.php', new Blob([JSON.stringify(Object.assign({ v: 21, x: COHORT.tag, type, ua: navigator.userAgent,
      mem: navigator.deviceMemory || 0, mission: RcControls.inMission() ? 1 : 0, bg: document.hidden ? 1 : 0 }, extra))], { type: 'application/json' }));
  } catch { /* no beacon */ }
};
function gpuReloadPrompt(msg) {
  if ($('gpulost')) return;
  $('gpurestore')?.remove();
  const d = document.createElement('div');
  d.id = 'gpulost';
  d.style.cssText = 'position:fixed;inset:0;z-index:50;display:grid;place-items:center;background:rgba(0,0,0,.92);color:#fff;' +
    'font:16px/1.5 ui-sans-serif,system-ui,sans-serif;text-align:center;padding:24px';
  d.innerHTML = '<div><p style="font-size:20px;margin:0 0 8px">The graphics device was lost</p>' +
    '<p style="opacity:.8;margin:0 0 18px">The browser stopped the game\'s renderer' + (msg ? ' (' + String(msg).replace(/[<>&]/g, '') + ')' : '') +
    ' and could not start it again. Your progress since the last save is lost. If this happens again, close the browser completely' +
    ' (on a phone, swipe it away from the recent apps) and open the site again: after repeated graphics crashes the browser turns' +
    ' graphics off for the site until it restarts.</p>' +
    '<button id="gpulost-reload" style="font:inherit;padding:10px 22px;border-radius:8px;border:0;background:#45cf79;color:#000;font-weight:700">Reload the game</button></div>';
  document.body.appendChild(d);
  $('gpulost-reload').onclick = () => location.reload();
}
window.rcDeviceLost = (msg, n) => {
  if (!gpuLostAt) gpuLostAt = performance.now();   // the 20 s limit runs from the first loss of a series
  gpuBeacon('gpulost', { msg: String(msg || '').slice(0, 200), n: n || 1 });
  if (!$('gpurestore')) {
    const n = document.createElement('div');
    n.id = 'gpurestore';
    n.style.cssText = 'position:fixed;left:50%;top:50%;transform:translate(-50%,-50%);z-index:50;padding:12px 20px;border-radius:10px;' +
      'background:rgba(0,0,0,.8);color:#fff;font:600 15px/1.4 ui-sans-serif,system-ui,sans-serif;pointer-events:none';
    n.textContent = 'Restoring graphics…';
    ($('stage') || document.body).appendChild(n);
  }
  clearTimeout(gpuFailTimer);
  const check = () => {   // counts only visible time: in the background Chrome may not give a device until it is back
    if (!gpuLostAt) return;
    if (document.hidden) { gpuFailTimer = setTimeout(check, 1000); return; }
    if (performance.now() - gpuLostAt > 20000) { gpuBeacon('gpufail', {}); gpuReloadPrompt(msg); return; }
    gpuFailTimer = setTimeout(check, 1000);
  };
  gpuFailTimer = setTimeout(check, 1000);
};
// WebGL sessions report once what the renderer saw (render_gl.c gl_diag_frame): GPU, GL errors, back-buffer brightness
window.rcGlRenderer = () => {   // the GPU's real name, where the browser tells it
  try { const gl = $('canvas').getContext('webgl2'); const e = gl && gl.getExtension('WEBGL_debug_renderer_info');
        return e ? String(gl.getParameter(e.UNMASKED_RENDERER_WEBGL)) : ''; } catch { return ''; }
};
window.rcGlDiag = (json) => { try { console.warn('[gl] diag', json); gpuBeacon('gldiag', { msg: String(json).slice(0, 300) }); } catch { /* no beacon */ } };
window.rcDeviceRestored = (n) => {
  const ms = gpuLostAt ? Math.round(performance.now() - gpuLostAt) : 0;
  gpuLostAt = 0; clearTimeout(gpuFailTimer);
  $('gpurestore')?.remove();
  gpuBeacon('gpurestored', { dur: ms, n });
};
window.rcRecoverFailed = (final) => {
  // the engine retries on its own (backing off) and gives up after six losses in a row: then the prompt
  if (final) { clearTimeout(gpuFailTimer); gpuBeacon('gpufail', { final: 1 }); gpuReloadPrompt(''); }
};
document.addEventListener('visibilitychange', () => {
  if (!document.hidden && gpuLostAt) gpuLostAt = performance.now();   // count the 20 s from the page's return; the engine retries itself
});

function sizeCanvas() {
  const canvas = $('canvas');
  const r = canvas.getBoundingClientRect(), dpr = Math.min(window.devicePixelRatio || 1, 2);
  canvas.width = Math.max(640, Math.round(r.width * dpr));
  canvas.height = Math.max(480, Math.round(r.height * dpr));
}

// On a phone the stage is fixed to the viewport (index.html: body.coarse #stage). If the page ends up zoomed
// or scrolled (iOS does that around rotation and fullscreen, Android when the browser's zoom setting is on),
// a fixed box is laid out to the layout viewport and overflows the screen: the picture is cropped and the
// touch controls land off-screen. Size and place the stage on the VISUAL viewport instead whenever the two
// differ; with no zoom and no offset this sets exactly the box the stylesheet gives it.
function followVisualViewport(stage) {
  const vv = window.visualViewport;
  if (!vv) return;
  let applied = false;
  const apply = () => {
    const zoomed = Math.abs(vv.scale - 1) > 0.01 || vv.offsetLeft > 0.5 || vv.offsetTop > 0.5 ||
      Math.abs(vv.width - innerWidth) > 1 || Math.abs(vv.height - innerHeight) > 1;
    if (!zoomed && !applied) return;
    applied = zoomed;
    if (!zoomed) { stage.style.cssText = ''; return; }
    stage.style.cssText = `position:fixed;inset:auto;left:${vv.offsetLeft}px;top:${vv.offsetTop}px;width:${vv.width}px;` +
      `height:${vv.height}px;aspect-ratio:auto;border-radius:0;box-shadow:none`;
  };
  vv.addEventListener('resize', apply);
  vv.addEventListener('scroll', apply);
  window.addEventListener('orientationchange', () => setTimeout(apply, 300));
  apply();
}

// A netplay session: every peer starts from the same empty saves folder, kept apart from this browser's
// normal saves (one per player slot, so two tabs of one browser can play each other).
const netSaves = (session) => `rcnetplay-${session.you}`;
async function clearNetSaves(session) {
  const root = await navigator.storage.getDirectory();
  try { await root.removeEntry(netSaves(session), { recursive: true }); } catch { /* not there yet */ }
}

function showNetStatus(M, session) {
  const el = $('netstatus');
  el.classList.remove('hidden');
  let lastFrame = 0, lastT = performance.now();
  setInterval(() => {
    const frame = M._netplay_status(0), desync = M._netplay_status(2), stalls = M._netplay_status(3);
    const now = performance.now();
    // the game thread always idles until the next frame's inputs arrive; it's only "waiting" when that
    // holds the game below full speed
    const fps = ((frame - lastFrame) * 1000) / Math.max(1, now - lastT);
    lastFrame = frame; lastT = now;
    el.classList.toggle('bad', !!desync);
    // the round trip the inputs ride on, and how many frames the delay failed to cover (a stall is a
    // frame that waited over 10 ms for the others' inputs: the game froze that long)
    const trips = session.client.trips();
    const path = trips.direct ? `direct ${trips.direct} ms` : `relayed ${trips.room} ms`;
    el.textContent = desync ? `Out of sync since frame ${desync}: restart the session`
      : `Online · player ${session.you + 1} of ${session.players} · delay ${session.delay}` +
        ` · ${path}` + (stalls ? ` · ${stalls} stalls` : '') +
        (session.reconnecting ? ' · reconnecting…' : frame > 300 && fps < 50 ? ' · waiting for players' : '') +
        (session.left?.size ? ` · ${session.left.size} left` : '');
  }, 1000);
}

async function run(session) {
  const audio = newAudioContext();   // first, while the click that called run() is still a user gesture
  // Boot report: no frame 8 s after Play sends where the start got to (script / module / frames), the engine's last
  // lines and any errors, once. Set here, not after the engine loads: on iOS 16 the page never got that far.
  const boot = { phase: 'start', at: performance.now(), done: false };
  const bootPhase = (p) => { boot.phase = p; boot.at = performance.now(); keepLog('[boot] ' + p); };
  // also when the page goes away first (players reloaded 4 s after Play on iOS 16)
  const bootReport = (why) => { if (boot.done || boot.sent || session) return; boot.sent = true;
    gpuBeacon('bootlog', { r: window.RC_RENDERER || window.RC_GPU_KIND || 'gpu',
      n: why + ' ' + boot.phase + ' ' + Math.round((performance.now() - boot.at) / 1000) + 's',
      msg: recentErr.join(' | ').slice(0, 300), page: recentLog.join(' | ').slice(-300) }); };
  setTimeout(() => { if (recentErr.length || boot.phase !== 'frames') bootReport('8s'); }, 8000);   // most starts take longer than 8 s
  setTimeout(() => bootReport('30s'), 30000);
  addEventListener('pagehide', () => bootReport('left'));
  const touch = !session && RcTouch.wanted();
  if (touch) RcTouch.goFullscreen($('stage'));   // also needs the gesture
  else if (!session) setupFullscreen.enter?.();   // desktop too: the game fills the screen (Esc kept for its menu; Alt+Enter leaves)
  $('panel').classList.add('hidden');
  document.body.classList.add('playing');   // enables the touch re-fullscreen pill (index.html)
  showBoot();   // cover the black canvas until the first frame renders (see frame loop below)
  beacon('launch', { touch: touch ? 1 : 0 });   // Play pressed: against start, the players who never see a frame
  if (touch) {
    // on a phone the game starts fullscreen; if the player later leaves it, #refull gets them back in
    $('refull').onclick = () => RcTouch.goFullscreen($('stage'), true);
  } else { fullscreenTip(); controlsTip(); }
  if (session) await clearNetSaves(session);
  sizeCanvas();
  const script = document.createElement('script');
  script.src = 'build/recomp.js?v=27';
  bootPhase('script');
  await new Promise((ok, fail) => { script.onload = ok; script.onerror = fail; document.head.appendChild(script); });
  bootPhase('module');
  const M = await createRecomp({
    arguments: ['/opfs/' + OPFS_DIR],
    print: (t) => { keepLog(t); console.log(t); },
    printErr: (t) => { keepLog(t); console.log(t); },
    preRun: [(m) => {
      if (params.has('mute')) m.ENV.RC_NO_AUDIO = '1';
      if (params.get('exe')) m.ENV.RC_EXE = params.get('exe');
      if (params.has('log')) m.ENV.RC_LOG = '1';
      if (params.has('pads')) m.ENV.RC_PADS = params.get('pads');
      if (params.has('input')) m.ENV.RC_INPUT = params.get('input');   // testing: scripted input (host/input.c)
      if (params.has('noyield')) m.ENV.RC_NO_YIELD = '1';                // testing: the game's poll spins unyielded (host/user32.c)
      if (params.has('gpucompat')) m.ENV.RC_GPU_COMPAT = '1';            // testing: a WebGPU compatibility-mode device
      if (params.has('gpuloop')) m.ENV.RC_GPU_LOOP = '1';                // testing: every new device after a loss arrives lost
      if (params.has('glprof')) m.ENV.RC_GL_PROF = '1';                  // testing: time per WebGL command type
      if (params.has('nobc')) m.ENV.RC_NO_BC = '1';                      // testing: no compressed textures (as most phones under WebGL)
      // Widescreen display modes (host/ddraw7.c) only when asked (?widemodes): the game runs in its original 4:3
      // modes everywhere, as it did until 2026-10-30; black screens on phones and desktops followed the switch.
      if (COHORT.on.wide) m.ENV.RC_WIDE_DISPLAY = '1';   // the wide experiment (A/B, see COHORT)
      if (params.has('peeklog')) m.ENV.RC_PEEK_LOG = '1';               // testing: poll interval log in the console
      if (params.has('poke')) m.ENV.RC_POKE = params.get('poke');      // testing: memory pokes (host/input.c)
      if (params.has('queue')) m.ENV.RC_QUEUE = params.get('queue');
      if (params.has('gpuskip')) m.ENV.RC_GPU_SKIP = '1';   // diagnostics: the executor issues no draws
      if (params.has('env')) for (const kv of params.get('env').split(';')) { const i = kv.indexOf('='); if (/^RC_[A-Z_]+$/.test(kv.slice(0, i))) m.ENV[kv.slice(0, i)] = kv.slice(i + 1); }   // testing: engine switches
      if (session) Object.assign(m.ENV, RcNetplay.gameEnv(session), { RC_USERDATA: '/opfs/' + netSaves(session) });
      m.ENV.RC_LOG_FPS = params.has('fps') ? '1' : '';
      if (!params.has('fps')) delete m.ENV.RC_LOG_FPS;
      // WebKit (Safari, and every browser on an iPhone or iPad) keeps a wasm memory resident in full from the
      // moment it is allocated and kills the page past ~2 GB, so there the guest memory is packed and grows
      // only as the game allocates (runtime.c, RC_MEM_COMPACT). ?mem=compact / ?mem=full force it either way.
      if (compactMemory()) m.ENV.RC_MEM_COMPACT = '1';
      // and there the frame's vertex/uniform uploads go up in one writeBuffer per batch instead of two per
      // draw (render_wgpu.c): WebKit's WebGPU holds a page-sized IPC copy per call for a while, which added
      // 1-2 GB to the page. ?gpu=coalesce / ?gpu=direct force it either way.
      const gpu = params.get('gpu');
      if (gpu === 'coalesce' || (gpu !== 'direct' && isWebKit())) m.ENV.RC_GPU_COALESCE = '1';
    }],
  });
  const canvas = $('canvas');
  if (window.RC_RENDERER === 'gl') {
    if (!M._rc_gl_init(canvas.width, canvas.height)) throw new Error('WebGL 2 could not start on this device.');
  } else M._rc_gpu_init(canvas.width, canvas.height);
  new ResizeObserver(() => { sizeCanvas(); M._rc_set_canvas_size(canvas.width, canvas.height); }).observe(canvas);
  hookInput(M, !!session);
  subtitles(M, touch);
  pauseAds(touch);
  if (touch) { RcTouch.attach(M, $('stage'), canvas); followVisualViewport($('stage')); }
  if (session) { session.client.attach(M); showNetStatus(M, session); }
  crashWatch(M, touch);
  startAudio(M, audio).catch((e) => console.warn('audio unavailable:', e));
  const poll = session ? pollLocalPad : pollGamepads;
  // Online, present at most 60 times a second: every Present is a lockstep frame, and the input delay and
  // the time a frame has to simulate are counted in those (netplay.js: autoDelay), so following a 120 Hz
  // display would halve both. Solo play follows the display. A frame is due every 1/60 s, with a little
  // tolerance so a 60 Hz display's own jitter doesn't skip one.
  let due = 0, booted = false, shownFrames = 0, lastPresent = 0, loading = false;
  const launchT = performance.now();
  setTimeout(() => { if (!booted) beacon('noframe', { touch: touch ? 1 : 0, n: shownFrames, t: 120 }); }, 120000);
  bootPhase('frames');
  // Picture check: players reported a black screen after Play with the game running behind it (frames
  // presented, nothing visible). Once the boot cover is off, every 15th presented frame is copied small onto a 2D
  // canvas in the same task as the present (the canvas still holds it then) and its lit pixels counted. A
  // picture that stays black for 20 s on WebGPU switches this browser to WebGL 2 and restarts.
  const pic = { el: null, cx: null, lit: 0, samples: 0, until: 0, done: false, f: 0 };
  const picCheck = () => {
    if (pic.done || ++pic.f % 15) return;
    try {
      if (!pic.el) { pic.el = document.createElement('canvas'); pic.el.width = 16; pic.el.height = 12; pic.cx = pic.el.getContext('2d', { willReadFrequently: true }); }
      pic.cx.clearRect(0, 0, 16, 12);
      pic.cx.drawImage($('canvas'), 0, 0, 16, 12);
      const d = pic.cx.getImageData(0, 0, 16, 12).data;
      let lit = 0;
      for (let i = 0; i < d.length; i += 4) if (d[i] + d[i + 1] + d[i + 2] > 24) lit++;
      pic.samples++; pic.lit = Math.max(pic.lit, lit);
    } catch (e) { pic.done = true; console.warn('picture check unavailable:', e.message); return; }
    if (pic.lit >= 4) { pic.done = true; return; }   // a picture: nothing to do
    if (!pic.until) pic.until = performance.now() + 20000;
    if (performance.now() < pic.until || session) return;
    pic.done = true;
    const r = window.RC_RENDERER || window.RC_GPU_KIND || 'gpu';
    console.warn(`picture check: ${pic.samples} samples over 20 s, all black (${r})`);
    gpuBeacon('blackpic', { r, n: pic.samples, t: Math.round((performance.now() - launchT) / 1000) });
    if (r !== 'gl' && webgl2Ok()) {
      try { localStorage.setItem(GL_STORE, String(Date.now())); } catch { /* storage blocked: the URL still says so */ }
      status('Switching to the compatible graphics mode…');
      const u = new URL(location.href); u.searchParams.set('renderer', 'gl'); u.searchParams.set('autoplay', '1');
      setTimeout(() => location.replace(u.href), 400);
    }
  };
  const frame = (t) => {
    poll(M);
    if (!session || t >= due - 4) {
      const n = M._rc_gpu_pump(1);
      if (n) {
        due = Math.max(due + 1000 / 60, t - 4);
        lastPresent = t;
        // Drop the boot cover only once the game is actually STREAMING frames (its menu/mission is up), not
        // on the first frame: while the game loads a cold install it submits nothing for up to ~2 minutes,
        // then a stray clear-frame, then the real render loop. Counting to a couple dozen frames waits for
        // that render loop so the cover never lifts onto a still-black screen.
        if (booted && !pic.done) picCheck();
        if (!booted) { shownFrames += n; if (shownFrames >= 24) { booted = true; boot.done = true; hideBoot(); runFlags.booted = 1; beacon('start', { touch: touch ? 1 : 0, t: Math.round(performance.now() / 1000) }); } }
        else if (loading) { loading = false; hideLoading(); }   // frames resumed: level finished loading
      } else if (booted && !loading && !session && lastPresent && t - lastPresent > 2500) {
        // Booted and running, but no frame for 2.5 s: the game is blocked loading a mission/level. Cover the
        // frozen black screen so it doesn't look crashed (solo only — online must never stall the loop).
        loading = true; showLoading();
      }
    } else M._rc_gpu_pump(0);   // mid-frame readbacks only
    requestAnimationFrame(frame);
  };
  requestAnimationFrame(frame);
  // readbacks mid-frame are answered without waiting for vsync; online, the pad is also sampled here
  // (8 ms, not 1: the browser clamps nested 1 ms timers to 4 ms anyway, and the game reads pixels back only for
  // screenshots, so a few ms of readback latency cost nothing while halving the page thread's idle wake-ups)
  setInterval(() => { if (session) poll(M); M._rc_gpu_pump(0); }, 8);
  // Online, a hidden tab still has to present frames (browsers pause requestAnimationFrame there), or the
  // lockstep would freeze every other player with it.
  if (session) setInterval(() => { if (document.hidden) { poll(M); M._rc_gpu_pump(1); } }, 16);
  canvas.focus();
  window.recomp = M;
}

// Players report the game going black mid-mission and the page coming back on the landing screen: that is
// the browser killing the page (out of memory, usually) and reloading it, which no code here can catch as it
// happens. So the running game leaves a heartbeat in sessionStorage (kept across a browser reload, gone when
// the tab closes) and clears it on an orderly unload; a start that finds a fresh heartbeat is the reload after
// such a kill. The player gets a note on the landing screen, and the site a report (crash.php: the device,
// its memory, how long the game ran, whether it was in a mission or in the background) to find the pattern.
const RUN_STORE = 'igi-run';
function crashWatch(M, touch) {
  const t0 = Date.now();
  // The heartbeat also keeps the state just before a freeze (the field reports show whole-process freezes
  // that no watchdog report precedes): the frame rate over the last beat, the page's memory (wasm, JS, GPU
  // copies: measureUserAgentSpecificMemory, sampled every 30 s), the threads' phases and the longest task.
  let lastFrames = -1, fps = 0, pageMB = 0, longTask = 0;
  try { new PerformanceObserver((l) => { for (const e of l.getEntries()) if (e.duration > longTask) longTask = e.duration; }).observe({ type: 'longtask', buffered: true }); } catch { /* no longtask support */ }
  const counters = () => {
    try { return M && M._rc_watch_addr ? Array.from(new Int32Array(M.wasmMemory.buffer, M._rc_watch_addr(), 8)) : []; } catch { return []; }
  };
  const beat = () => {
    try {
      const w = counters();
      if (w.length) { if (lastFrames >= 0) fps = Math.round((w[2] - lastFrames) / 5); lastFrames = w[2]; }
      if (!runFlags.play && RcControls.inMission()) { runFlags.play = 1; beacon('play', { touch: touch ? 1 : 0, t: Math.round((Date.now() - t0) / 1000) }); }
      sessionStorage.setItem(RUN_STORE, JSON.stringify({ t: Date.now(), t0, m: RcControls.inMission() ? 1 : 0, touch: touch ? 1 : 0, bg: document.hidden ? 1 : 0,
        x: COHORT.tag, booted: runFlags.booted || 0, play: runFlags.play || 0,
        fps, page: pageMB, lt: Math.round(longTask), pp: w[0], gp: w[1], gf: w[4], gpu: w[7], io: M && M._rc_io_activity ? M._rc_io_activity() : 0 }));
    } catch { /* storage blocked */ }
  };
  const measure = () => {
    try { if (performance.measureUserAgentSpecificMemory) performance.measureUserAgentSpecificMemory().then((r) => { pageMB = r.bytes >> 20; }).catch(() => {}); } catch { /* unsupported */ }
  };
  beat(); measure();
  setInterval(beat, 5000);
  setInterval(measure, 60000);   // the measurement waits for a garbage collection: once a minute is plenty
  window.addEventListener('pagehide', () => {
    try { sessionStorage.removeItem(RUN_STORE); } catch { /* storage blocked */ }
    // an experiment session that ends (a reload, closing the tab) without ever reaching a mission after 20 s may be
    // a black screen the player gave up on: leave the experiments in this browser (conservative by design)
    if (/^(?!base)/.test(COHORT.tag) && !runFlags.play && Date.now() - t0 > 20000) {
      leaveExperiments(COHORT.tag.split('+'));
      beacon('expoff', { booted: runFlags.booted || 0, played: Math.round((Date.now() - t0) / 1000) });
    }
  });
  hangWatch(M, touch);
}

// Watchdog worker. Players report two freezes the heartbeat above can't explain: the page thread stops
// answering (Chrome's "Page unresponsive" dialog; the heartbeat stops with it, so the reload after it shows
// a gap of a minute or more), and the picture stops while the page still answers. A worker keeps its own
// clock: the page thread posts a beat every second (and on visibility changes), and the worker reads the
// host's watchdog counters straight from the shared wasm memory (host/win32.h RcWatch: what the page thread
// and the game thread were doing). After 15 s without a beat in the foreground it reports a hang, and after
// 20 s with no frame presented and no file read in a running mission a stall, to crash.php, with those
// counters; the end of either is reported too, so a hang that recovers is told from one that lasts.
function hangWorker() {
  let mem = null, addr = 0, info = {}, t0 = Date.now(), last = 0, st = null, lastBg = 0;
  let hangAt = 0, hangSent = false, stallAt = 0, stallSent = false, lastFrames = -1, lastIo = -1, lastMoveT = 0, fpsBefore = 0;
  const counters = () => {
    if (!mem || !addr) return {};
    const a = new Int32Array(mem.buffer, addr, 8);
    return { pp: a[0], gp: a[1], frames: a[2], queued: a[3], gf: a[4], beat: a[5], rb: a[6], gpu: a[7] };
  };
  const hist = [];   // (time, frames presented) over the last 10 s, for the frame rate before a freeze
  const fps = () => {
    const w = counters();
    if (w.frames === undefined) return 0;
    const now = Date.now();
    hist.push([now, w.frames]);
    while (hist.length > 1 && now - hist[0][0] > 10000) hist.shift();
    const a = hist[0];
    return now - a[0] >= 1000 ? Math.round((w.frames - a[1]) * 1000 / (now - a[0])) : 0;
  };
  const report = (type, dur) => {
    const body = Object.assign({ type, dur, played: Math.round((Date.now() - t0) / 1000), wasm: mem ? mem.buffer.byteLength >> 20 : 0, fps: fpsBefore },
      info, st ? { mission: st.m, paused: st.p, bg: st.bg, io: st.io, lt: st.lt } : {}, counters());
    try { fetch('crash.php', { method: 'POST', body: JSON.stringify(body), headers: { 'Content-Type': 'application/json' }, keepalive: true }).catch(() => {}); } catch { /* offline */ }
    postMessage({ warn: 'watchdog: ' + JSON.stringify(body), ga: type });
  };
  onmessage = (e) => {
    const d = e.data;
    if (d.init) { mem = d.memory; addr = d.addr; info = d.info; t0 = Date.now(); return; }
    last = Date.now(); st = d;
  };
  let lastTick = 0;
  setInterval(() => {
    if (!last) return;
    const now = Date.now(), quiet = now - last, bg = st && st.bg;
    // this worker itself didn't run for a while: the whole process was paused (the device slept, the browser
    // froze the tab or the system paged it out), which is not a hang of the page thread; reported apart,
    // and the page thread's silence over that time is not counted as a hang
    const jump = lastTick ? now - lastTick : 0;
    lastTick = now;
    if (jump > 8000) { report('pause', Math.round(jump / 1000)); last = now; return; }
    const f = fps();
    if (quiet < 3000 && !bg) fpsBefore = f;   // the frame rate while the page still answered
    if (bg !== lastBg) { lastBg = bg; lastMoveT = now; }   // frames stop in a hidden tab: not a stall
    if (!bg && quiet > 10000) {
      if (!hangAt) hangAt = last;
      if (!hangSent && quiet > 15000) { hangSent = true; report('hang', Math.round(quiet / 1000)); }
    } else if (hangAt) {
      if (hangSent) report('hangend', Math.round((now - hangAt) / 1000));
      hangAt = 0; hangSent = false;
    }
    const w = counters();
    if (w.frames === undefined || !st || bg || quiet > 3000) return;
    if (w.frames !== lastFrames || st.io !== lastIo) {
      lastFrames = w.frames; lastIo = st.io; lastMoveT = now;
      if (stallAt) { if (stallSent) report('stallend', Math.round((now - stallAt) / 1000)); stallAt = 0; stallSent = false; }
    } else if (st.m && !st.p && lastMoveT && now - lastMoveT > 20000) {
      if (!stallAt) stallAt = lastMoveT;
      if (!stallSent) { stallSent = true; report('stall', Math.round((now - lastMoveT) / 1000)); }
    }
  }, 1000);
}
function hangWatch(M, touch) {
  if (!M || !M.wasmMemory || !M._rc_watch_addr || typeof Worker !== 'function') return;
  let w;
  try { w = new Worker(URL.createObjectURL(new Blob(['(' + hangWorker.toString() + ')()'], { type: 'text/javascript' }))); }
  catch (e) { console.warn('watchdog worker unavailable:', e); return; }
  w.onmessage = (e) => {
    if (!e.data) return;
    if (e.data.warn) console.warn(e.data.warn);
    try { if (e.data.ga && typeof gtag === 'function') gtag('event', 'freeze_' + e.data.ga); } catch { /* analytics blocked */ }
  };
  w.postMessage({ init: 1, memory: M.wasmMemory, addr: M._rc_watch_addr(), info: {
    v: 21, x: COHORT.tag, ua: navigator.userAgent, mem: navigator.deviceMemory || 0, cores: navigator.hardwareConcurrency || 0,
    touch: touch ? 1 : 0, screen: `${screen.width}x${screen.height}@${devicePixelRatio}` } });
  // the longest task the page thread ran (ms): a hang that ends shows as one long task; a stutter as many
  let lt = 0;
  try { new PerformanceObserver((l) => { for (const e of l.getEntries()) if (e.duration > lt) lt = e.duration; }).observe({ type: 'longtask', buffered: true }); } catch { /* no longtask support */ }
  const beat = () => {
    try { w.postMessage({ t: Date.now(), m: RcControls.inMission() ? 1 : 0, p: RcControls.paused() ? 1 : 0, bg: document.hidden ? 1 : 0, io: M._rc_io_activity ? M._rc_io_activity() : 0, lt: Math.round(lt) }); }
    catch { /* the game is gone */ }
  };
  beat();
  setInterval(beat, 1000);
  document.addEventListener('visibilitychange', beat);
}
// A browser whose WebGPU picture stayed black (picture check in run()) uses WebGL 2 for 14 days.
const GL_STORE = 'igi-gl-black';
const glRemembered = () => { try { const t = +localStorage.getItem(GL_STORE); return t > 0 && Date.now() - t < 14 * 86400000; } catch { return false; } };
const webgl2Ok = () => { try { return !!document.createElement('canvas').getContext('webgl2'); } catch { return false; } };
const runFlags = {};   // this run: booted (24 frames shown), play (the player reached a mission)
function crashCheck() {
  let r = null;
  try { r = JSON.parse(sessionStorage.getItem(RUN_STORE) || 'null'); sessionStorage.removeItem(RUN_STORE); } catch { r = null; }
  if (!r || typeof r.t !== 'number' || Date.now() - r.t > 120000) return;   // not the reload right after a running game
  const info = {
    v: 21, type: 'reload', ua: navigator.userAgent, mem: navigator.deviceMemory || 0, cores: navigator.hardwareConcurrency || 0,
    played: Math.round((r.t - r.t0) / 1000), gap: Math.round((Date.now() - r.t) / 1000), mission: r.m, touch: r.touch, bg: r.bg,
    nav: performance.getEntriesByType('navigation')[0]?.type || '', screen: `${screen.width}x${screen.height}@${devicePixelRatio}`,
    fps: r.fps, page: r.page, lt: r.lt, pp: r.pp, gp: r.gp, gf: r.gf, io: r.io, gpu: r.gpu,   // the state at the last beat before the end
    x: r.x || '', booted: r.booted || 0, play: r.play || 0,
  };
  // a run in an experiment that never showed the game, or was killed within two minutes: leave the experiments
  if (r.x && !/^base/.test(r.x) && (!r.booted || (r.fps === 0 && info.played < 120) || info.played < 120)) {
    leaveExperiments(r.x.split('+'));
    info.expoff = 1;
  }
  console.warn('the previous run ended without unloading (the browser killed the page?)', info);
  try { navigator.sendBeacon('crash.php', new Blob([JSON.stringify(info)], { type: 'application/json' })); } catch { /* no beacon */ }
  if (!r.bg) $('crashnote')?.classList.remove('hidden');   // a kill in the background is the OS reclaiming a hidden tab, not a crash
}

// Follow gate (index.html #iggate): the first Play shows a prompt to follow the site's Instagram; Play is
// enabled once the Follow link has been tapped (or "already following"), and the choice is remembered in
// this browser. A follow can't be checked from a website, so this is a prompt, not a lock. Test runs
// (?autoplay) skip it.
const FOLLOW_STORE = 'igi-ig-follow';
function followGate(next) {
  let done = false;
  try { done = localStorage.getItem(FOLLOW_STORE) === '1' || params.has('autoplay'); } catch { done = params.has('autoplay'); }
  const gate = $('iggate');
  if (done || !gate) { next(); return; }
  const play = $('ig-play');
  const unlock = () => { try { localStorage.setItem(FOLLOW_STORE, '1'); } catch { /* storage blocked */ } gate.classList.add('hidden'); next(); };
  $('ig-follow').onclick = () => { setTimeout(() => { play.disabled = false; play.focus(); }, 1500); };   // link opens in a new tab
  $('ig-already').onclick = (e) => { e.preventDefault(); unlock(); };
  play.onclick = unlock;
  gate.classList.remove('hidden');
}

async function boot() {
  // Friendly, device-specific message when the browser can't run the game (WebGPU + cross-origin isolation
  // arrived on iPhone/iPad in iOS 26). Avoids developer jargon the tester would read as an error.
  const isIOS = /iPhone|iPad|iPod/.test(navigator.userAgent) ||
    (navigator.platform === 'MacIntel' && navigator.maxTouchPoints > 1);
  const needHint = isIOS
    ? 'On iPhone or iPad this needs iOS 26 or later, opened in Safari. Please update iOS and try again.'
    : 'Please use an up-to-date Chrome or Edge (Android or desktop), or Safari 26+ on Apple devices.';
  // Links tapped inside Instagram, Facebook, WhatsApp or other apps open in the app's own browser view, which
  // lacks the features the game needs: say so and offer the way out (Android: open Chrome by intent; else copy).
  const inApp = /Instagram|FBAN|FBAV|FB_IAB|Line\/|Snapchat|; wv\)|WhatsApp|Twitter|TikTok|Telegram/i.test(navigator.userAgent);
  if (!crossOriginIsolated && inApp) {
    const android = /Android/i.test(navigator.userAgent);
    status('You opened this inside an app’s built-in browser, which can’t run the game. Open it in Chrome instead.');
    const btn = $('download');
    btn.textContent = android ? 'Open in Chrome' : 'Copy the link';
    btn.classList.remove('hidden');
    btn.onclick = () => {
      if (android) window.rcLeave = true, location.href = 'intent://' + location.host + location.pathname + '#Intent;scheme=https;package=com.android.chrome;S.browser_fallback_url=' + encodeURIComponent(location.href) + ';end';
      else { navigator.clipboard?.writeText(location.href).then(() => status('Link copied. Paste it in Safari or Chrome.')).catch(() => status('Copy this address and open it in Safari or Chrome: ' + location.href)); }
    };
    return;
  }
  if (!crossOriginIsolated) { status('This browser can’t run the game yet — it’s missing a feature the game needs (cross-origin isolation). ' + needHint); return; }
  // Count the visitors the game can't start for, and on what (crash.php type nowebgpu / nogpu): the size of
  // the audience a second renderer (WebGL) would reach. Device model is in the user agent on Android.
  const cannot = (type) => { try { navigator.sendBeacon('crash.php', new Blob([JSON.stringify({ v: 21, x: COHORT.tag, type, ua: navigator.userAgent, mem: navigator.deviceMemory || 0,
    cores: navigator.hardwareConcurrency || 0, screen: `${screen.width}x${screen.height}@${devicePixelRatio}` })], { type: 'application/json' })); } catch { /* no beacon */ } };
  // Renderer: WebGPU when the browser has a usable adapter, else WebGL 2 (render_gl.c), which reaches older iPhones
  // and the Android phones and PCs whose GPU Chrome keeps off WebGPU. ?renderer=gl forces WebGL 2 (testing).
  const webgl2 = webgl2Ok;
  if (params.get('renderer') === 'gl' && webgl2()) { window.RC_RENDERER = 'gl'; }
  else if (glRemembered() && !params.has('renderer') && webgl2()) { window.RC_RENDERER = 'gl'; }   // this browser showed black on WebGPU
  else if (!navigator.gpu) {
    cannot('nowebgpu');
    if (webgl2()) window.RC_RENDERER = 'gl';
    else { status('This browser doesn’t support WebGPU or WebGL 2, which the game needs for graphics. ' + needHint); return; }
  }
  // navigator.gpu can exist with no usable adapter (GPU or driver blocklisted, hardware acceleration off,
  // Remote Desktop / VM): say so instead of starting a game that can only show a black screen
  // (featureLevel 'compatibility': WebGPU on OpenGL ES 3.1, Chrome 146+, for phones without full WebGPU; the engine
  // asks in the same order, host/render_wgpu.c request_web_adapter)
  // window.RC_GPU_KIND: which request gave the adapter, reported as r (gpu, gpuc = compatibility mode, gpusw = software)
  const asked = async (opts, kind) => { const a = await navigator.gpu.requestAdapter(opts).catch(() => null); if (a) window.RC_GPU_KIND = kind; return a; };
  const adapter = window.RC_RENDERER === 'gl' ? { gl: 1 } : params.has('noadapter') ? null :   // ?noadapter: testing the fallback
    await asked({ powerPreference: 'high-performance' }, 'gpu') ||
    await asked(undefined, 'gpu') ||
    await asked({ featureLevel: 'compatibility' }, 'gpuc') ||
    await asked({ forceFallbackAdapter: true }, 'gpusw');
  if (!adapter && webgl2()) { cannot('nogpu'); window.RC_RENDERER = 'gl'; }
  else if (!adapter) {
    cannot('nogpu');
    const firefox = /Firefox\//.test(navigator.userAgent);
    // Chrome switches graphics off for a site after the graphics process crashed while it was open, until the
    // browser restarts: the commonest reason on machines that played before. Say that first.
    status('The browser didn’t give the game a graphics adapter. If the game worked before in this browser, close the browser ' +
      'completely (every tab, and on a phone swipe it away from recent apps) and open the site again: Chrome turns graphics off ' +
      'for a site after a graphics crash, until it restarts. ' +
      (firefox ? 'Firefox only enables WebGPU for some GPUs and drivers; Chrome or Edge usually work. '
        : 'Otherwise check that hardware acceleration is on in the browser settings and the graphics driver is up to date. ') +
      'Remote Desktop and virtual machines often have no GPU for the browser.');
    return;
  }
  if (adapter && !adapter.gl && (adapter.isFallbackAdapter || adapter.info?.isFallbackAdapter))
    console.warn('WebGPU: only a software (fallback) adapter is available; the game will be slow');
  if (!navigator.storage?.getDirectory) { status('This browser has no Origin Private File System.'); return; }
  let onlineReady = false;
  const goOnline = () => {
    if (!onlineReady) { setupOnline(); onlineReady = true; }
    show('online');
    ($('np-code').value ? $('np-join') : $('np-name')).focus();
  };
  const ready = () => {
    show('home');
    crashCheck();
    if (params.get('join')) goOnline();   // an invite link goes straight to joining
    if (params.has('autoplay')) $('play').click();
  };
  migratePreset(!!(await readMarker()));   // the keyboard layout for players from before the layouts existed
  $('play').onclick = () => followGate(() => run().catch(fail));
  $('go-online').onclick = () => followGate(goOnline);
  for (const b of document.querySelectorAll('[data-go]')) b.onclick = () => show(b.dataset.go);
  $('folder').onchange = async (e) => {
    $('pick').classList.add('hidden');
    try { await importFromPicker(e.target.files); ready(); }
    catch (err) { status(err.message); $('pick').classList.remove('hidden'); }
  };
  const showPicker = (msg) => {
    status(msg);
    $('pick').classList.remove('hidden');
    $('help').classList.remove('hidden');
  };

  // A hosted game folder (game/manifest.json) is downloaded; without one the player picks their own copy.
  // Phones and tablets can't pick folders, so there it's the only way in.
  const hosted = await hostedManifest();
  if (hosted) $('reset').textContent = 'Reload the game again';
  const offerDownload = (msg) => {
    status(msg);
    $('download').textContent = 'Download the game (one time)';
    $('download').classList.remove('hidden');
  };
  // The browser's private storage refused: Safari's private windows and some locked-down profiles have none
  // ("The operation failed for an unknown transient reason", QuotaExceededError), and the game can't be kept.
  const storageProblem = (err) => {
    const m = String(err && err.message || ''), n = err && err.name || '';
    if (!/transient|quota|storage|NotAllowed|Security|UnknownError/i.test(m + ' ' + n)) return '';
    return 'This browser window can\'t store the game (a private window, or the browser\'s storage is full or blocked). ' +
           'Open the site in a normal window with some free space and try again.';
  };
  $('download').onclick = async () => {
    $('download').classList.add('hidden');
    $('pick').classList.add('hidden');
    try { await importFromServer(hosted); ready(); }
    catch (err) { offerDownload(storageProblem(err) || err.message); }
  };
  $('reset').onclick = (e) => {
    e.preventDefault();
    show('loading');
    if (hosted) offerDownload('Reload the game again.'); else showPicker('Choose your Project I.G.I. folder.');
  };
  if (!(await readMarker())) {
    // Hosted page (always, on phone or desktop): start the one-time download straight away, no click to
    // waste. On failure the click handler re-shows the button to retry. Without a hosted folder (desktop
    // dev), the player picks their own copy.
    if (hosted) { $('download').click(); return; }
    showPicker('Choose your Project I.G.I. folder to start.');
    return;
  }
  if (hosted) await refreshConfig(hosted);
  ready();
}

// config.qvm holds the game's own key bindings. The game rewrites it when a mission ends, and a tap or two in
// its Controls menu rebinds keys by accident (a phone's tap is a mouse click to it), which then came back on
// every visit and fought the page's own key layout. On every start the shipped copy is fetched again (3.7 KB)
// over the one in storage, so the game always starts with its default bindings; keys are changed in the
// page's panel. A failed fetch keeps whatever is there, so an offline start still works.
// It also carries the mission unlock (GOActiveMission = 14, so "Finding the bomb" is listed). CONFIG_REV is in
// the URL because Cloudflare keeps /game/ files as immutable: bump it whenever the shipped config.qvm changes.
const CONFIG_REV = 2;
// The stock config also resets the game's own mouse settings, so a player's in-game sensitivity or invert
// was lost at every start (reported as "sensitivity changed but not applied", 2026-10-31). Carry those two over
// from the player's saved copy. Records in the compiled config (the LOOP script) read
//   0d <name#> 18 01 00 00 00 <u32 string offset> 14 <type> 00 00 00 <value>
// with name #5 = GOInMouSens (type 6: 05 + float) and #4 = GOInMouInv (type 2: one byte, 0f false / 10 true), one
// record per player profile, matched in order. Anything unexpected keeps the stock values.
function keepPlayerMouse(saved, fresh) {
  const find = (b, name, type) => {
    const at = [];
    for (let i = 0; i + 17 < b.length; i++)
      if (b[i] === 0x0d && b[i + 1] === name && b[i + 2] === 0x18 && b[i + 3] === 1 && b[i + 4] === 0 && b[i + 5] === 0 && b[i + 6] === 0 &&
          b[i + 11] === 0x14 && b[i + 12] === type && b[i + 13] === 0 && b[i + 14] === 0 && b[i + 15] === 0) at.push(i + 16);
    return at;
  };
  const out = fresh.slice();
  const sensS = find(saved, 5, 6), sensF = find(out, 5, 6);
  if (sensS.length && sensS.length === sensF.length)
    sensS.forEach((o, k) => {
      if (saved[o] !== 0x05) return;
      const v = new DataView(saved.buffer, saved.byteOffset + o + 1, 4).getFloat32(0, true);
      if (v > 0.02 && v < 3 && out[sensF[k]] === 0x05) out.set(saved.subarray(o, o + 5), sensF[k]);
    });
  const invS = find(saved, 4, 2), invF = find(out, 4, 2);
  if (invS.length && invS.length === invF.length)
    invS.forEach((o, k) => { if (saved[o] === 0x0f || saved[o] === 0x10) out[invF[k]] = saved[o]; });
  return out;
}
async function refreshConfig(list) {
  const e = list.find((f) => f.path.toLowerCase() === 'config.qvm');   // the root one (LANGUAGE/* has its own)
  if (!e) return;
  try {
    const r = await fetch('game/' + e.path.split('/').map(encodeURIComponent).join('/') + '?rev=' + CONFIG_REV, { cache: 'no-cache' });
    if (!r.ok) return;
    const buf = await r.arrayBuffer();
    if (buf.byteLength !== e.size) return;
    const fh = await fileHandle(await gameDir(true), e.path, true);
    let fresh = new Uint8Array(buf);
    try { fresh = keepPlayerMouse(new Uint8Array(await (await fh.getFile()).arrayBuffer()), fresh); } catch { /* no saved copy yet */ }
    const w = await openWriter(fh, OPFS_DIR + '/' + e.path);
    try { await w.write(fresh); await w.close(); } catch (err) { await w.abort(); throw err; }
  } catch (err) { console.warn('config.qvm refresh skipped:', err.message); }
}

// ------------------------------------------------------------------ fullscreen

// The stage (game + overlays) goes fullscreen; the canvas grows with it and the game letterboxes its 4:3
// picture inside. Alt+Enter (Option+Enter on a Mac) toggles it, as in PC games of the time; it is caught
// before the game's own key handling so Enter doesn't also reach a menu.
function setupFullscreen() {
  const stage = $('stage');
  const active = () => document.fullscreenElement || document.webkitFullscreenElement;
  // body.fs drives the touch re-fullscreen pill (index.html): shown only when playing and NOT fullscreen.
  // Registered unconditionally (even where the desktop button is unavailable) so the pill is always right.
  // opened from the Home Screen (iPhone, an installed app): already without browser bars, so no pill either
  const standalone = () => navigator.standalone === true || matchMedia('(display-mode: standalone), (display-mode: fullscreen)').matches;
  const syncFsClass = () => document.body.classList.toggle('fs', !!active() || standalone());
  document.addEventListener('fullscreenchange', syncFsClass);
  document.addEventListener('webkitfullscreenchange', syncFsClass);
  syncFsClass();
  // In fullscreen the page asks to keep Esc (Keyboard Lock, Chrome and Edge): a tap of Esc then opens the game's
  // menu as players expect, and holding Esc still leaves fullscreen (the browser's rule). Without the lock the
  // first Esc leaves fullscreen.
  const enter = () => {
    const p = (stage.requestFullscreen || stage.webkitRequestFullscreen).call(stage);
    const lock = () => { try { navigator.keyboard?.lock?.(['Escape'])?.catch?.(() => {}); } catch { /* not supported */ } };
    if (p && p.then) p.then(lock).catch(() => {}); else lock();
  };
  const toggle = () => {
    if (active()) { try { navigator.keyboard?.unlock?.(); } catch { /* not supported */ } (document.exitFullscreen || document.webkitExitFullscreen).call(document); }
    else enter();
    $('canvas').focus();
  };
  setupFullscreen.enter = () => { if (!active() && (stage.requestFullscreen || stage.webkitRequestFullscreen)) enter(); };
  if (!stage.requestFullscreen && !stage.webkitRequestFullscreen) { $('fs').classList.add('hidden'); return; }
  $('fs').onclick = toggle;
  const swallow = (e) => {
    if (e.code !== 'Enter' && e.code !== 'NumpadEnter') return;
    if (!e.altKey) return;
    e.preventDefault();
    e.stopImmediatePropagation();
    if (e.type === 'keydown' && !e.repeat) toggle();
  };
  window.addEventListener('keydown', swallow, true);
  window.addEventListener('keyup', swallow, true);
  const sync = () => {
    $('fs-icon').setAttribute('d', active() ? 'M9 4v5H4M15 4v5h5M9 20v-5H4M15 20v-5h5' : 'M4 9V4h5M20 9V4h-5M4 15v5h5M20 15v5h-5');
    $('fs').title = active() ? 'Exit fullscreen (Alt+Enter or Esc)' : 'Fullscreen (Alt+Enter)';
  };
  document.addEventListener('fullscreenchange', sync);
  document.addEventListener('webkitfullscreenchange', sync);
  // the button shows while the mouse moves over the stage and fades after a few still seconds
  let idle;
  const wake = () => { stage.classList.add('awake'); clearTimeout(idle); idle = setTimeout(() => stage.classList.remove('awake'), 2500); };
  stage.addEventListener('mousemove', wake);
  stage.addEventListener('mouseleave', () => stage.classList.remove('awake'));
  wake();
}

// once the game starts: a short hint, so players learn the shortcut
function fullscreenTip() {
  const tip = $('fstip');
  if (document.fullscreenElement || document.webkitFullscreenElement || $('fs').classList.contains('hidden')) return;
  tip.classList.remove('hidden');
  tip.style.opacity = '1';
  setTimeout(() => { tip.style.opacity = '0'; }, 5000);
  setTimeout(() => tip.classList.add('hidden'), 5500);
}

// and, a moment later, the controls that differ from what players of other shooters expect
function controlsTip() {
  const tip = $('ctltip');
  if (!tip) return;
  const C = RcControls.get(), right = RcControls.ACTIONS[C.mouse[2]]?.label || 'nothing';
  const k = (id) => `<kbd>${keyLabel(userKeys[id])}</kbd>`;
  tip.innerHTML = preset === 'original'
    ? `Original keys: arrows move · ${k('walkrun')} walk/run · ${k('crouch')} crouch · ${k('jump')} jump · ${k('zoomin')}/${k('zoomout')} zoom · right click: <b>${right}</b> — WASD and more in the Controls panel`
    : `Right click: <b>${right}</b> · scroll: weapon · ${k('walkrun')} walk/run · ${k('crouch')} crouch · ${k('jump')} jump — change them in the Controls panel`;
  setTimeout(() => { tip.classList.remove('hidden'); tip.style.opacity = '1'; }, 6000);
  setTimeout(() => { tip.style.opacity = '0'; }, 16000);
  setTimeout(() => tip.classList.add('hidden'), 16500);
}

// ------------------------------------------------------------------ online lobby

async function buildVersion() {
  // peers must run the same build and game files, or their simulations diverge
  const marker = await readMarker();
  const wasm = await fetch('build/recomp27.wasm', { method: 'HEAD' }).catch(() => null);
  // the ETag comes back weak (W/"...") when the response is compressed: compare only its value
  const etag = (wasm?.headers.get('ETag') || '').replace(/^W\//, '').replace(/"/g, '');
  return `${marker?.version || 'local'}:${etag || wasm?.headers.get('Content-Length') || '?'}`;
}

function setupOnline() {
  const msg = (t) => { $('np-msg').textContent = t; $('np-lobbymsg').textContent = t; };
  try {
    $('np-name').value = localStorage.getItem('rc-name') || '';
    const region = localStorage.getItem('rc-region');
    if ([...$('np-region').options].some((o) => o.value === region)) $('np-region').value = region;
  } catch { /* storage blocked */ }
  if (params.get('join')) $('np-code').value = params.get('join').toUpperCase();
  let client = null;
  let lastPlayers = [];

  // create: the host's first connection, which places the room in `region` (the Worker's location hint)
  async function enter(code, create) {
    if (client) return;
    const name = $('np-name').value.trim() || 'Player';
    try { localStorage.setItem('rc-name', name); localStorage.setItem('rc-region', $('np-region').value); } catch { /* storage blocked */ }
    msg('Connecting…');
    let iceServers;   // from the first ticket (TURN credentials last a day)
    const roomUrl = async () => {   // a fresh ticket each time (they expire after an hour)
      const res = await fetch(NETPLAY_BASE + '_netplay_ticket', { cache: 'no-store' });
      if (!res.ok) throw new Error('Online play is not available on this server.');
      const { ticket, iceServers: ice } = await res.json();
      iceServers ??= ice;
      const url = new URL(`netplay/${code}?ticket=${encodeURIComponent(ticket)}`, NETPLAY_BASE);
      if (create) { url.searchParams.set('create', '1'); url.searchParams.set('region', $('np-region').value); create = false; }
      url.protocol = url.protocol === 'https:' ? 'wss:' : 'ws:';
      return url.href;
    };
    let url;
    try { url = await roomUrl(); } catch (e) { msg(e.message); return; }
    const invite = new URL(location.href);
    invite.search = `?join=${code}` + (params.get('netplay') ? `&netplay=${encodeURIComponent(params.get('netplay'))}` : '');
    $('np-room').textContent = code;
    $('invite').textContent = invite.href;
    const version = await buildVersion();
    let session = null;
    const left = new Set();
    const me = client = RcNetplay.connect({
      url, name, version, reconnectUrl: roomUrl, iceServers, relayOnly: params.get('turn') === 'only',
      onReconnect(state) { if (session) session.reconnecting = state === 'connecting'; },
      onLobby(l) {
        if (client !== me) return;   // a room this page already left
        if (l.started) { msg('That game has already started.'); leave(); return; }
        if ($('view-lobby').classList.contains('hidden')) { msg(''); show('lobby'); }
        $('np-colo').textContent = l.colo ? `Server in ${l.colo}` : '';
        $('np-players').innerHTML = '';
        const mine = l.players.find((p) => p.slot === l.you);
        const el = (tag, cls, text) => { const e = document.createElement(tag); if (cls) e.className = cls; if (text != null) e.textContent = text; return e; };
        for (const p of l.players) {
          const li = el('li');
          const direct = p.slot === l.you ? 0 : Math.max(mine?.p2p?.[p.slot] || 0, p.p2p?.[l.you] || 0);
          const link = p.slot === l.you ? (p.rtt ? `${p.rtt} ms to the room` : 'Connected')
            : direct ? `Direct connection · ${direct} ms` : 'Connected through the room';
          const who = el('div', 'who');
          who.append(el('b', null, p.name), el('small', null, link));
          li.append(el('div', 'avatar', (p.name || '?').trim().charAt(0).toUpperCase() || '?'), who);
          if (p.slot === 0) li.append(el('span', 'tag', 'Host'));
          if (p.slot === l.you) li.append(el('span', 'tag', 'You'));
          $('np-players').appendChild(li);
        }
        if (l.players.length < 2) {
          const li = el('li', 'empty');
          li.append(el('div', 'avatar', '?'), el('div', 'who', 'Waiting for your friend to join…'));
          $('np-players').appendChild(li);
        }
        $('np-hostctl').classList.toggle('hidden', !l.host);
        $('np-start').disabled = l.players.length !== 2;
        if (l.players.length > 2) msg('This is a 1v1 game: only two players can be in the room.');
        lastPlayers = l.players;
        const auto = RcNetplay.autoDelay(l.players);
        $('np-delay').options[0].textContent = `Auto (${auto} frames, ${Math.round(auto * 1000 / 60)} ms)`;
        $('np-wait').classList.toggle('hidden', l.host);
      },
      onStart(s) {
        session = { ...s, client, left };
        run(session).catch(fail);
      },
      onLeft(slot) { left.add(slot); },
      onError(e) { msg(e); setTimeout(() => { if ($('np-msg').textContent === e) msg(''); }, 8000); },
      onClose() { if (!session && client === me) { client = null; show('online'); msg('Disconnected from the room.'); } },
    });
  }
  function leave() {
    const c = client;
    client = null;
    c?.close();
    show('online');
  }
  $('np-leave').onclick = () => { leave(); msg(''); };
  $('np-copy').onclick = async () => {
    const b = $('np-copy');
    try { await navigator.clipboard.writeText($('invite').textContent); b.textContent = 'Link copied'; }
    catch { b.textContent = 'Copy failed: share the code instead'; }
    setTimeout(() => { b.textContent = 'Copy invite link'; }, 2000);
  };

  const code5 = () => Array.from(crypto.getRandomValues(new Uint8Array(5)), (b) => 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789'[b % 32]).join('');
  $('np-host').onclick = () => enter(code5(), true).catch((e) => msg(e.message));
  $('np-join').onclick = () => {
    const code = $('np-code').value.trim().toUpperCase();
    if (!/^[A-Z0-9]{4,12}$/.test(code)) { msg('Enter the room code from the host.'); return; }
    enter(code).catch((e) => msg(e.message));
  };
  $('np-start').onclick = () => {
    const v = $('np-delay').value;
    client?.start(v === 'auto' ? RcNetplay.autoDelay(lastPlayers) : Number(v));
  };
}

// Keyboard cheat sheet beside the stage: in-game prompts use controller glyphs (triangle/circle/×/square,
// L1, R1), so keyboard players need a reference. The CSS (index.html) reserves the panel's space on
// viewports wide enough and shrinks the stage to match; the body's .kbd-off class (set when the player
// dismisses the panel, remembered in localStorage) hides the panel and lets the stage reclaim the space.
function setupKbdHelp() {
  const off = () => { try { return localStorage.getItem('rc-kbd') === 'hide'; } catch { return false; } };
  const save = (v) => { try { localStorage.setItem('rc-kbd', v); } catch { /* storage blocked */ } };
  const apply = () => document.body.classList.toggle('kbd-off', off());
  $('kbd-close').onclick = () => { save('hide'); apply(); };
  $('kbd-show').onclick = () => { save('show'); apply(); };
  apply();
}

// The device choice on the landing screen (index.html #device): touch controls + gyroscope, or keyboard and
// mouse. Preselected from the browser (a coarse pointer on a touch screen), remembered in this browser, and
// applied right away: the page's phone layout (body.coarse) and the hints follow it, and run() asks
// RcTouch.wanted() when the game starts.
function setupDevice() {
  const box = $('device');
  if (!box) return;
  const sync = () => {
    const touch = RcTouch.wanted();
    document.body.classList.toggle('coarse', touch);
    // iPhone Safari has no fullscreen for a page, and its toolbars slide in over the page when a swipe starts
    // at the screen's edge (looking down starts one at the top): the viewport shrinks, and a stage fixed to
    // it would resize the game's picture every time. There the stage keeps the largest viewport's size
    // instead (index.html body.nofs #stage); the toolbar overlaps the bottom until it hides again.
    document.body.classList.toggle('nofs', touch && !document.fullscreenEnabled && !document.webkitFullscreenEnabled);
    // the on-screen controls exist from here on (hidden until the game runs), so their settings and layout can
    // be set up on the landing screen and while the game loads
    if (touch) {
      RcTouch.setup($('stage'), $('canvas'));
      $('home-controls').onclick = () => RcTouch.openSettings();
      $('boot-controls').onclick = () => RcTouch.openSettings();
    }
    for (const b of box.querySelectorAll('button')) {
      const on = (b.dataset.device === 'touch') === touch;
      b.classList.toggle('on', on); b.setAttribute('aria-checked', on ? 'true' : 'false');
    }
  };
  for (const b of box.querySelectorAll('button')) b.onclick = () => {
    try { localStorage.setItem('igi-device', b.dataset.device); } catch { /* storage blocked */ }
    sync();
  };
  sync();
}
setupDevice();
setupFullscreen();
setupKbdHelp();
setupControls();
boot();
