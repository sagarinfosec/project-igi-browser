// Shared controls layer for the keyboard/mouse page (loader.js) and the on-screen touch controls (touch.js):
//  - the game's actions and the default key each one has in its config.qvm (the game's own bindings stay at
//    their defaults; everything here translates onto them in the browser)
//  - mouse button bindings (which action left / right / middle click does, saved per browser)
//  - the scope toggle: the game only has Zoom In / Zoom Out keys that zoom continuously while held (sniper
//    scope, binoculars); one button that snaps to a scope zoom and back is what players expect. The host
//    reports the actual zoom of the view (input_zoom: the projection's y scale, 2.31 at the normal view, up
//    to ~21 fully zoomed), so the toggle holds the key until the view really is there, at any frame rate.
//  - look settings: touch swipe sensitivity, a scope multiplier, PC mouse sensitivity
//  - the gyroscope on phones: turning the phone turns the view, like mobile shooters do
// Everything reaches the game as the keys and mouse it already reads (host/input.c), only in mouse-look mode
// (host input_in_mission: a mission with the game running): the 2D menus, and the Esc menu over a paused
// mission, always get a plain click and an unscaled cursor.
'use strict';

const RcControls = (() => {
  const STORE = 'igi-settings';
  const HOLD_MS = 70;   // the game samples input once a frame: hold taps at least this long (the host also keeps a press until sampled)

  // what the game does and the key it reads for it (HID usage; the game's defaults from config.qvm)
  const ACTIONS = {
    fire:       { label: 'Fire',            mouse: 0 },
    scope:      { label: 'Scope (toggle)',  scope: true },     // zoom to the scope level / back to the normal view
    zoomin:     { label: 'Zoom in',         key: 75 },         // Page Up
    zoomout:    { label: 'Zoom out',        key: 78 },         // Page Down
    nextweapon: { label: 'Next weapon',     mouse: 1 },        // the game's right click
    prevweapon: { label: 'Previous weapon', key: 18 },         // O
    altfire:    { label: 'Alternate fire',  key: 42 },         // Backspace
    reload:     { label: 'Reload',          key: 40 },         // Enter
    use:        { label: 'Use / activate',  key: 229 },        // Right Shift
    crouch:     { label: 'Crouch',          key: 228 },        // Right Ctrl
    jump:       { label: 'Jump',            key: 98 },         // Numpad 0
    peek:       { label: 'Peek',            key: 89 },         // Numpad 1
    bino:       { label: 'Binoculars',      key: 44 },         // Space
    walk:       { label: 'Walk / run',      key: 26 },         // W (held)
    map:        { label: 'Map computer',    key: 6 },          // C
    menu:       { label: 'Menu',            key: 41 },         // Esc
    none:       { label: 'Nothing' },
  };
  // the actions a mouse button can take, in menu order
  const MOUSE_CHOICES = ['fire', 'scope', 'zoomin', 'zoomout', 'nextweapon', 'prevweapon', 'altfire', 'reload', 'crouch', 'jump', 'use', 'bino', 'peek', 'walk', 'none'];

  const DEFAULTS = {
    mouse: { 0: 'fire', 1: 'altfire', 2: 'scope' },   // left, middle, right (MouseEvent.button)
    mouseSens: 0.6,     // PC: multiplier on mouse motion (the host makes the game's response linear, 0.212 deg per count)
    look: 1,            // touch: swipe sensitivity
    scopeSens: 1,       // touch, mouse and gyro while scoped. The game itself already slows the view by the zoom
                        // (measured: 0.7x at the binoculars' 1x, 0.11x at 5x), so this is the only extra factor
    scopeZoom: 4,       // the zoom the scope button snaps to (x the normal view)
    gyro: 'scope',      // 'off' | 'on' | 'scope'
    gyroSens: 1,
    gyroInvY: false,    // gyroscope: tilt the phone up to look down (some players want it, as in flight games)
    gyroInvX: false,    // and turn left to look right
    wide: true,         // widescreen: the 3D world fills the whole screen (host rc_set_wide); false = 4:3 letterboxed
    subs: true,         // large subtitles: the game's speech read back from its tiny font (host/subtitle.c) in a caption
  };
  let S = load();
  function load() {
    let saved = {};
    try { saved = JSON.parse(localStorage.getItem(STORE) || '{}') || {}; } catch { saved = {}; }
    const s = { ...DEFAULTS, mouse: { ...DEFAULTS.mouse } };
    for (const k of ['mouseSens', 'look', 'scopeSens', 'scopeZoom', 'gyroSens']) if (typeof saved[k] === 'number' && saved[k] > 0) s[k] = saved[k];
    if (['off', 'on', 'scope'].includes(saved.gyro)) s.gyro = saved.gyro;
    if (typeof saved.wide === 'boolean') s.wide = saved.wide;
    for (const k of ['gyroInvY', 'gyroInvX', 'subs']) if (typeof saved[k] === 'boolean') s[k] = saved[k];
    if (saved.mouse) for (const b of [0, 1, 2]) if (ACTIONS[saved.mouse[b]]) s.mouse[b] = saved.mouse[b];
    return s;
  }
  function save() { try { localStorage.setItem(STORE, JSON.stringify(S)); } catch { /* storage blocked */ } }
  const get = () => S;
  function set(k, v) { S[k] = v; save(); if (k === 'gyro') gyroApply(); if (k === 'wide') wideApply(); }
  const wideApply = () => { if (M && M._rc_set_wide) M._rc_set_wide(S.wide ? 1 : 0); };
  function setMouse(button, action) { S.mouse[button] = ACTIONS[action] ? action : 'none'; save(); }
  function reset() { S = { ...DEFAULTS, mouse: { ...DEFAULTS.mouse } }; save(); gyroApply(); wideApply(); }

  // ---- sending input: a key or mouse button held at least HOLD_MS so a quick tap still lasts a game frame
  let M = null;
  const inMission = () => !!(M && M._input_in_mission && M._input_in_mission());
  const paused = () => !!(M && M._input_paused && M._input_paused());
  // the mission's script failed the level: the game's menu is up with Resume greyed out (host/input.c)
  const levelFailed = () => !!(M && M._input_level_failed && M._input_level_failed());
  const downAt = new Map();
  function press(what, down) {
    if (!M) return;
    const k = what.key !== undefined ? 'k' + what.key : 'm' + what.mouse;
    const send = (v) => (what.key !== undefined ? M._input_key_hid(what.key, v) : M._input_mouse_button(what.mouse, v));
    if (down) { downAt.set(k, performance.now()); send(1); return; }
    const held = performance.now() - (downAt.get(k) ?? 0);
    downAt.delete(k);
    if (held >= HOLD_MS) send(0); else setTimeout(() => { if (!downAt.has(k)) send(0); }, HOLD_MS - held);
  }
  const tap = (what) => { press(what, true); press(what, false); };

  // ---- the scope. zoom() is the view's zoom as the host saw it last frame (1 = normal view). scoped() is
  // true once the view is zoomed in at all (the scope or binoculars are up, however it got there: the toggle,
  // the plain zoom keys, the game). The toggle drives the zoom keys towards a target and lets go when the
  // view gets there, stops moving (a weapon without a scope: nothing happens) or after a few seconds.
  const BASE_M11 = 2.3094;   // the projection's y scale at the normal view (fov 47 deg)
  const zoomRaw = () => (M && M._input_zoom ? M._input_zoom() : 0);
  const zoom = () => { const z = zoomRaw(); return z > 0 ? z / BASE_M11 : 1; };
  const scoped = () => zoom() > 1.12;
  const listeners = new Set();
  let lastScoped = false, lastShown = 0;
  const notify = () => { for (const f of listeners) f(scoped(), zoom()); };
  function onScope(f) { listeners.add(f); }
  let zoomKey = 0;          // +1 / -1 while a zoom key is held (by the toggle or the player), 0 otherwise
  let drive = null;         // the toggle's run: { dir, target, last, lastT, t0, timer }
  function zoomKeyDown(dir) {
    if (zoomKey === dir) return;
    if (zoomKey) press(zoomKey > 0 ? ACTIONS.zoomin : ACTIONS.zoomout, false);
    zoomKey = dir;
    press(dir > 0 ? ACTIONS.zoomin : ACTIONS.zoomout, true);
  }
  function zoomKeyUp(dir) {
    if (zoomKey !== dir) return;
    zoomKey = 0;
    press(dir > 0 ? ACTIONS.zoomin : ACTIONS.zoomout, false);
  }
  function driveStop(reached) {
    if (!drive) return;
    const d = drive;
    clearInterval(d.timer);
    zoomKeyUp(d.dir);
    drive = null;
    notify();
    if (d.done && reached !== undefined) d.done(reached, d.moved);   // not when cancelled
  }
  // target: zoom factor (1 = normal view); patience: ms the zoom may stand still before the drive gives up
  // (the zoom saturated, or this weapon has no scope); done(reached, moved) runs when the drive lets go.
  function zoomTo(target, done, patience = 700) {
    driveStop();
    const z = zoom(), dir = target > z ? 1 : -1;
    if (Math.abs(target - z) < 0.04 * target) { if (done) done(true, false); return; }
    const now = performance.now();
    drive = { dir, target, last: z, lastT: now, t0: now, timer: 0, done, moved: false };
    zoomKeyDown(dir);
    drive.timer = setInterval(() => {
      const t = performance.now(), cur = zoom();
      const reached = dir > 0 ? cur >= target * 0.97 : cur <= Math.max(1.02, target * 1.03);
      if (Math.abs(cur - drive.last) > 0.005 * cur) { drive.last = cur; drive.lastT = t; drive.moved = true; }
      if (reached) driveStop(true);
      else if (t - drive.lastT > patience || t - drive.t0 > 4000) driveStop(false);
    }, 16);
  }
  // The scope button. Only the sniper rifle's scope and the binoculars zoom in this game, and the zoom keys
  // only work once one of them is up. A tap first presses Alternate fire, which raises the rifle's scope when
  // the player holds one (the view zooms a little within a few frames; with the other weapons the key does
  // nothing): then the zoom keys drive the scope to the set level, and the next tap lowers the scope with
  // Alternate fire again (the game resets the zoom with it). With any other weapon the toggle brings up the
  // binoculars instead (the game toggles them on each press of their key) and zooms through those; the next
  // tap zooms back out and puts them away. A player who raised the scope or the binoculars themselves gets
  // the same taps.
  let viaBino = false, viaScope = false, wait = 0, zoomPress = 0, zoomFrom = 1, lastToggle = 0;
  const later = (ms, f) => { wait = setTimeout(() => { wait = 0; f(); }, ms); };
  function scopeToggle() {
    if (!inMission()) return;
    // a second tap while the first is still bringing the scope up: ignored (it used to cancel it, which read as a
    // missed touch to players who tap twice because nothing showed yet)
    const now = performance.now();
    if (wait && now - lastToggle < 600) return;
    if (wait) { clearTimeout(wait); wait = 0; }
    lastToggle = now;
    if (drive) { driveStop(); return; }   // a tap while it's still zooming: stop where it is
    if (scoped()) {
      if (viaScope) { viaScope = false; tap(ACTIONS.altfire); return; }
      zoomTo(1, () => { if (viaBino) { viaBino = false; tap(ACTIONS.bino); } });
      return;
    }
    viaBino = viaScope = false;
    const z0 = zoom();
    tap(ACTIONS.altfire);
    const t0 = performance.now();
    const probe = () => {
      if (Math.abs(zoom() - z0) > 0.01) { viaScope = true; zoomTo(S.scopeZoom, null, 700); return; }   // the rifle's scope came up
      if (performance.now() - t0 < 450) { later(40, probe); return; }   // a slow phone draws a frame in 70 ms
      // no scope on this weapon. The zoom keys still work when the player has the binoculars up at 1x:
      // try them before bringing up the binoculars (which would put the player's away)
      zoomTo(S.scopeZoom, (reached, moved) => {
        if (reached || moved) return;
        viaBino = true;
        tap(ACTIONS.bino);
        // the binoculars take a few frames to come up (longer on a slow phone), then zoom through them
        later(350, () => zoomTo(S.scopeZoom, null, 1500));
      }, 400);
    };
    later(40, probe);
  }
  function weaponChanged() { driveStop(); viaScope = false; }   // the game drops the scope and zoom itself
  // the scope button's state follows the real zoom, polled a few times a second
  setInterval(() => {
    if (!M) return;
    const s = scoped(), z = zoom();
    if (s !== lastScoped || (s && Math.abs(z - lastShown) > 0.15)) { lastScoped = s; lastShown = z; notify(); }
  }, 120);

  // ---- an action pressed / released (a mouse button or a touch button)
  function act(name, down) {
    const a = ACTIONS[name];
    if (!a) return;
    if (a.scope) { if (down) scopeToggle(); return; }
    if (name === 'menu' && down) { try { window.pauseAds?.menuPressed?.(); } catch { /* no ads */ } }
    // Zoom in / out. The game's zoom keys only work with the scope or binoculars up, and zoom while held: a quick
    // tap moved the view a barely visible amount and + without a scope did nothing, both felt like a missed touch
    // (2026-10-31). Now + without a scope raises it (the scope toggle), a tap steps the zoom 1.6x, a hold zooms on.
    if (name === 'zoomin' || name === 'zoomout') {
      const dir = name === 'zoomin' ? 1 : -1;
      if (down) {
        if (!scoped()) { zoomPress = 0; if (dir > 0) scopeToggle(); return; }
        driveStop(); zoomPress = performance.now(); zoomFrom = zoom(); zoomKeyDown(dir);
      } else if (zoomPress) {
        const held = performance.now() - zoomPress; zoomPress = 0;
        if (held < 220 && Math.abs(zoom() - zoomFrom) < 0.25 * zoomFrom) zoomTo(dir > 0 ? zoomFrom * 1.6 : Math.max(1.05, zoomFrom / 1.6), null, 500);
        else zoomKeyUp(dir);
      }
      return;
    }
    if (down && (name === 'nextweapon' || name === 'prevweapon')) weaponChanged();
    if (down && name === 'bino') viaBino = false;   // the player handles the binoculars themselves
    if (down && name === 'altfire') viaScope = !scoped();   // raising the rifle's scope by hand: the toggle lowers it
    press(a, down);
  }
  // which action a physical mouse button does right now: in mouse-look mode the player's binding, in the
  // menus (and the Esc menu over a paused mission) the plain button, so left click always clicks
  function mouseAction(button) {
    if (!inMission()) return button === 0 ? 'fire' : button === 2 ? 'nextweapon' : 'none';
    return S.mouse[button] || 'none';
  }
  // key events that change the weapon (the game's own keys: O, the weapon rows 1-9) end a scope toggle in progress
  function keyUsage(usage, down) {
    if (down && (usage === 18 || (usage >= 30 && usage <= 38))) weaponChanged();
  }

  // ---- look motion: fractions carry over so slow motion still moves
  let fx = 0, fy = 0;
  function look(dx, dy) {
    if (!M) return;
    fx += dx; fy += dy;
    const ix = Math.trunc(fx), iy = Math.trunc(fy);
    if (ix || iy) { M._input_mouse_move(ix, iy); fx -= ix; fy -= iy; }
  }
  // While scoped the view turns slower, in step with the zoom (at 4x a swipe turns about a third as far),
  // times the player's own scope multiplier; mobile shooters do the same.
  const lookScale = () => (zoom() > 1.12 ? S.scopeSens : 1);

  // ---- gyroscope (devicemotion): the rotation rate about the vertical axis turns the view, about the screen's
  // horizontal axis tilts it. Gravity (low-passed accelerationIncludingGravity) gives both axes in the device's
  // frame whatever way the phone is held, so the mapping holds in either landscape and when the phone is
  // tilted back. iOS reports the acceleration with the opposite sign to the spec, and needs permission.
  const webkit = /AppleWebKit\//.test(navigator.userAgent) && !/Chrome\/|Chromium\/|Edg\//.test(navigator.userAgent);
  const DEG = 4.7;   // mouse counts per degree of phone rotation at sensitivity 1 (the game turns 0.212 deg per count)
  let g = null, wf = null, gyroOn = false, gyroLast = 0, gyroStatus = 'idle';   // idle | on | denied | none
  const gyroState = () => gyroStatus;
  function onMotion(e) {
    const r = e.rotationRate, a = e.accelerationIncludingGravity;
    if (!r || r.alpha == null || !a || a.x == null) { if (gyroStatus === 'on') gyroStatus = 'none'; return; }
    gyroStatus = 'on';
    const now = performance.now();
    // Each event is one sensor sample: integrate over the sensor's own interval (ms; a few WebKits report
    // seconds), not the time it took to arrive. Browsers deliver these in bursts under load, which with
    // arrival times made the view stutter. Without an interval, the arrival time, capped.
    let dt = typeof e.interval === 'number' && e.interval > 0 ? (e.interval < 1 ? e.interval : e.interval / 1000) : 0;
    if (!dt) dt = gyroLast ? (now - gyroLast) / 1000 : 0;
    dt = Math.min(dt, 0.05);
    const first = !gyroLast;
    gyroLast = now;
    const s = webkit ? -1 : 1;
    const ax = a.x * s, ay = a.y * s, az = a.z * s;
    g = g ? [g[0] + (ax - g[0]) * 0.15, g[1] + (ay - g[1]) * 0.15, g[2] + (az - g[2]) * 0.15] : [ax, ay, az];
    const n = Math.hypot(g[0], g[1], g[2]);
    // a light two-sample smoothing of the rates takes the sensor noise out without a felt lag
    const raw = [r.alpha, r.beta, r.gamma];
    wf = wf && !first ? [wf[0] + (raw[0] - wf[0]) * 0.5, wf[1] + (raw[1] - wf[1]) * 0.5, wf[2] + (raw[2] - wf[2]) * 0.5] : raw;
    if (!dt || first || n < 1) return;
    if (!gyroActive() || !inMission()) return;
    const up = [g[0] / n, g[1] / n, g[2] / n];                 // world up in device axes (x right, y top, z out of screen)
    const right = [up[1], -up[0], 0];                          // up × z: horizontal, in the screen plane, pointing right
    const rn = Math.hypot(right[0], right[1]);
    if (rn < 0.3) return;                                      // phone lying flat: no horizontal reference
    right[0] /= rn; right[1] /= rn;
    // deg/s about device x, y, z. The spec names alpha the rate about z, but Chromium AND WebKit both fill
    // alpha/beta/gamma straight from the gyroscope's x/y/z (device_motion_event_pump.cc, WebCoreMotionManager.mm).
    const w = wf;
    // right-hand rule about each axis: turning left (counter-clockwise seen from above) is a positive yaw,
    // and tilting the phone so its camera (the back) points up is a positive pitch (+y rolls into +z, so -z,
    // the camera, rolls into +y). Players reported the first version looked down when they tilted up: the
    // pitch sign was the wrong way round. Mouse counts: +x looks right, +y looks down.
    const yaw = w[0] * up[0] + w[1] * up[1] + w[2] * up[2];
    const pitch = w[0] * right[0] + w[1] * right[1];
    if (gyroDbg) gyroDbg(raw, up, yaw, pitch, dt);
    if (Math.abs(yaw) < 0.3 && Math.abs(pitch) < 0.3) return;  // hand tremor: a small dead zone
    if (Math.abs(yaw) > 720 || Math.abs(pitch) > 720) return;   // a sensor glitch, not a hand
    const k = DEG * S.gyroSens * lookScale() * dt;
    look(-yaw * k * (S.gyroInvX ? -1 : 1), -pitch * k * (S.gyroInvY ? -1 : 1));
  }
  const gyroActive = () => S.gyro === 'on' || (S.gyro === 'scope' && scoped());
  // ?gyrotest: a readout of what the sensor says and what reaches the game, for a screen recording when the
  // gyroscope misbehaves on a phone we don't have (rates in deg/s in the device frame, the up vector, the
  // yaw / pitch rates, the event interval, the events per second and the counts sent per second).
  let gyroDbg = null;
  if (new URLSearchParams(location.search).has('gyrotest')) {
    const box = document.createElement('div');
    box.style.cssText = 'position:fixed;left:50%;top:40%;transform:translateX(-50%);z-index:9999;pointer-events:none;' +
      'background:rgba(0,0,0,.7);color:#9fe6b0;font:12px/1.4 ui-monospace,Menlo,monospace;padding:6px 10px;border-radius:8px;white-space:pre';
    document.body.appendChild(box);
    let shown = 0, events = 0, sentX = 0, sentY = 0, t0 = performance.now();
    const look0 = look;
    gyroDbg = (raw, up, yaw, pitch, dt) => {
      events++;
      const now = performance.now();
      if (now - shown < 100) return;
      shown = now;
      const secs = (now - t0) / 1000;
      box.textContent = `gyro raw ${raw.map((v) => v.toFixed(1).padStart(7)).join(' ')} deg/s\n` +
        `up   ${up.map((v) => v.toFixed(2).padStart(6)).join(' ')}   dt ${(dt * 1000).toFixed(1)} ms\n` +
        `yaw ${yaw.toFixed(1).padStart(7)}  pitch ${pitch.toFixed(1).padStart(7)} deg/s\n` +
        `${(events / secs).toFixed(0)} ev/s   sent ${(sentX / secs).toFixed(0)} ${(sentY / secs).toFixed(0)} counts/s   mode ${S.gyro} sens ${S.gyroSens}`;
      if (secs > 5) { t0 = now; events = 0; sentX = sentY = 0; }
    };
    look = (dx, dy) => { sentX += Math.abs(dx); sentY += Math.abs(dy); look0(dx, dy); };
  }
  // Starts listening (once): on iOS this must be called from a user gesture (a click). One request at a time.
  let gyroPending = null;
  function gyroEnable() {
    if (gyroOn) return Promise.resolve(gyroStatus);
    if (gyroPending) return gyroPending;
    gyroPending = (async () => {
      if (!window.DeviceMotionEvent) { gyroStatus = 'none'; return gyroStatus; }
      try {
        if (typeof DeviceMotionEvent.requestPermission === 'function') {
          const p = await DeviceMotionEvent.requestPermission();
          if (p !== 'granted') { gyroStatus = 'denied'; return gyroStatus; }
        }
      } catch { gyroStatus = 'denied'; return gyroStatus; }
      window.addEventListener('devicemotion', onMotion);
      gyroOn = true; gyroLast = 0; wf = null; gyroStatus = 'on';
      // no event within 1.5 s: the device has no motion sensors (or the browser blocks them)
      setTimeout(() => { if (gyroStatus === 'on' && !gyroLast) gyroStatus = 'none'; }, 1500);
      return gyroStatus;
    })().finally(() => { gyroPending = null; });
    return gyroPending;
  }
  // The saved setting wants the gyroscope: where the browser asks permission (iOS) that needs a gesture, so
  // it waits for the first tap on the page; elsewhere it starts right away.
  function gyroApply() {
    if (S.gyro === 'off' || gyroOn || gyroPending) return;
    if (typeof DeviceMotionEvent?.requestPermission !== 'function') { gyroEnable(); return; }
    const once = () => { window.removeEventListener('click', once, true); window.removeEventListener('touchend', once, true); gyroEnable(); };
    window.addEventListener('click', once, true);
    window.addEventListener('touchend', once, true);
  }

  function attach(module) { M = module; gyroApply(); wideApply(); }

  return { ACTIONS, MOUSE_CHOICES, DEFAULTS, get, set, setMouse, reset, attach, press, tap, act, mouseAction, keyUsage,
    look, lookScale, scoped, zoom, zoomTo, onScope, weaponChanged, inMission, paused, levelFailed, gyroEnable, gyroState };
})();
