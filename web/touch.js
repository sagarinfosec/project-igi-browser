// On-screen controls for phones and tablets, laid out like mobile shooters: a stick on the left for the arrow
// keys (move and strafe, and up / down in menus), dragging anywhere else to move the mouse (aim in a mission,
// the cursor in menus), a tap there to click at that spot, and buttons for the game's actions (controls.js: the
// game's default keys) clustered around a big FIRE button at the bottom right, with a second FIRE above the
// stick for two-thumb play. Pushing the stick past its top edge locks it forward (the player keeps running
// with the thumb off the screen; a tap on the stick frees it). A gear opens the settings (sensitivity, scope
// zoom, gyroscope) and the layout editor (drag any button where you want it, size and opacity), a first start
// shows the controls guide. The overlay is built as soon as the page knows it's on a phone (setup), so the
// settings and the layout can be changed while the game downloads or loads; attach() wires it to the game.
// Everything goes in as the keyboard and mouse the game already reads (host/input.c), so remapping keys in
// the game's Configuration menu breaks the matching button.
'use strict';

const RcTouch = (() => {
  const BUTTONS = [     // id, label, action (controls.js) or key (HID usage), placement class, toggle?
    ['fire', 'FIRE', { act: 'fire' }, 'b-fire'],
    ['fire2', 'FIRE', { act: 'fire' }, 'b-fire2'],
    ['scope', '<i class="eye"></i><small>SCOPE</small>', { act: 'scope' }, 'b-scope', 'scope'],
    ['jump', 'JUMP', { act: 'jump' }, 'b-jump'],
    ['crouch', 'CROUCH', { act: 'crouch' }, 'b-crouch', 'hold'],
    ['use', 'USE', { act: 'use' }, 'b-use'],
    ['reload', 'RELOAD', { act: 'reload' }, 'b-reload'],
    ['peek', 'PEEK', { act: 'peek' }, 'b-peek'],
    ['weapon', 'WEAPON<small>next</small>', { act: 'nextweapon' }, 'b-weapon'],
    ['walk', 'WALK<small>run</small>', { act: 'walk' }, 'b-walk', 'hold'],
    ['bino', 'BINO', { act: 'bino' }, 'b-bino', 'latch'],
    ['zin', '+', { act: 'zoomin' }, 'b-zin'],
    ['zout', '−', { act: 'zoomout' }, 'b-zout'],
    ['map', 'MAP', { act: 'map' }, 'b-map'],
    ['menu', 'MENU', { act: 'menu' }, 'b-menu'],
    ['gear', '⚙', { settings: true }, 'b-gear'],
  ];
  const ARROWS = { up: 82, down: 81, left: 80, right: 79 };
  const GUIDE_SEEN = 'igi-guide-seen';
  const LAYOUT_STORE = 'igi-layout';   // { pos: { id: [cx, cy] fractions of the stage }, size, alpha }

  // The player's choice on the landing screen (loader.js, localStorage 'igi-device') wins; without one, a touch
  // screen with a coarse pointer gets the touch controls.
  const wanted = () => {
    if (new URLSearchParams(location.search).has('touch')) return true;
    let pref = null;
    try { pref = localStorage.getItem('igi-device'); } catch { /* storage blocked */ }
    if (pref === 'touch') return true;
    if (pref === 'pc') return false;
    return matchMedia('(pointer: coarse)').matches && navigator.maxTouchPoints > 0;
  };

  let ui = null;   // built once by setup(): { root, openSettings, openLayout, live }
  let Mod = null;  // the game module once attach() ran

  function setup(stage, canvas) {
    if (ui) return ui;
    const C = RcControls;
    const root = document.createElement('div');
    root.id = 'touch';
    root.className = 'off';   // hidden until the game runs (attach), except while the layout is being edited
    root.innerHTML = '<div class="t-look"></div><div class="t-stick"><div class="t-lock" aria-hidden="true"><span>🏃</span></div><div class="t-knob"></div></div>' +
      BUTTONS.map(([id, label, , cls]) => `<button class="t-btn ${cls}" data-id="${id}">${label}</button>`).join('') +
      '<div class="t-edit"><b>Drag any button or the stick where you want it. Tap one to size it.</b>' +
      '<label>All sizes <input type="range" min="0.7" max="1.5" step="0.05" data-k="size"></label>' +
      '<label>Opacity <input type="range" min="0.3" max="1" step="0.05" data-k="alpha"></label>' +
      '<label class="te-one hidden"><span class="te-sel">Button</span> <input type="range" min="0.6" max="1.8" step="0.05" data-k="one"></label>' +
      '<button type="button" class="te-reset">Reset layout</button><button type="button" class="te-done">Done</button></div>';
    stage.appendChild(root);
    const byId = (id) => root.querySelector(`.t-btn[data-id="${id}"]`);

    // Crouch and walk are held keys in the game (let go and you stand up / run again), which a thumb can't do
    // while it also steers and fires: a tap on those buttons holds the key until the next tap ('hold'). The
    // binoculars are a toggle in the game, so that button just mirrors the state ('latch'). The scope button
    // follows the real zoom (controls.js) and shows it.
    const latched = {};
    const press = (what, down) => {
      if (what.act) C.act(what.act, down);
      else if (what.key !== undefined) C.press(what, down);
    };
    const scopeBtn = byId('scope');
    C.onScope((on, z) => {
      scopeBtn.classList.toggle('on', on);
      scopeBtn.querySelector('small').textContent = on ? `${z.toFixed(1)}×` : 'SCOPE';
    });
    // the buttons only mean something in a mission; in the menus they fade so the menu is readable
    // A failed mission (a timed objective missed, say) makes the game open its own menu with Resume greyed
    // out, which reads like a freeze: say what happened and what to tap. Lives beside the overlay so the
    // menus fade doesn't take it, and ignores touches so the menu underneath still gets them.
    const failedNote = document.createElement('div');
    failedNote.id = 'tfailed'; failedNote.hidden = true;
    failedNote.textContent = 'Mission failed. Tap RESTART MISSION in the menu to try again.';
    root.parentElement.appendChild(failedNote);
    setInterval(() => {
      const inm = C.inMission();
      root.classList.toggle('menus', !inm);
      failedNote.hidden = !C.levelFailed();
      if (locked && !inm) unlock();   // the stick lock is for running in a mission, not for the menus
    }, 250);

    // Finger motion -> mouse counts. In the 2D menus the game turns a count into about 1.33 px across and
    // 0.8 px down of its 640x480 screen, which the canvas letterboxes at 4:3: these factors make the menu
    // cursor follow the finger. In a mission both axes get the same scale (what a swipe feels like), times
    // the player's sensitivity and the scope multiplier.
    const legacy = Number(new URLSearchParams(location.search).get('touchsens')) || 1;
    const move = (dx, dy) => {
      const r = canvas.getBoundingClientRect();
      const h = Math.min(r.height, (r.width * 3) / 4), w = (h * 4) / 3;
      if (C.inMission()) {
        const k = (640 / w / 1.333) * C.get().look * C.lookScale() * legacy;
        C.look(dx * k, dy * k);
      } else C.look((dx * 640) / w / 1.333, (dy * 480) / h / 0.8);
    };

    // a point on the page -> pixels of the game's 640x480 screen, letterboxed at 4:3 in the canvas
    const gamePoint = (x, y) => {
      const r = canvas.getBoundingClientRect();
      const h = Math.min(r.height, (r.width * 3) / 4), w = (h * 4) / 3;
      const gx = ((x - r.left - (r.width - w) / 2) * 640) / w, gy = ((y - r.top - (r.height - h) / 2) * 480) / h;
      return { x: Math.round(Math.max(0, Math.min(639, gx))), y: Math.round(Math.max(0, Math.min(479, gy))) };
    };

    // every finger is tracked by pointer id: one can steer while another aims and a third fires
    const fingers = new Map();   // pointerId -> { kind, x, y, t0, moved, what }
    const stick = root.querySelector('.t-stick'), knob = root.querySelector('.t-knob'), lockEl = root.querySelector('.t-lock');
    let arrows = new Set();
    const setArrows = (next) => {
      for (const a of arrows) if (!next.has(a)) C.press({ key: ARROWS[a] }, false);
      for (const a of next) if (!arrows.has(a)) C.press({ key: ARROWS[a] }, true);
      arrows = next;
    };
    // The sprint lock, as mobile shooters do it: pushing the stick forward shows a running figure well above
    // the stick (with a dotted line up to it); sliding the thumb up onto that figure locks running forward
    // (the forward arrow stays down, the knob pinned at the top, the figure lit) and the thumb can leave the
    // screen. The next touch on the stick frees it. Only in a mission (the menus use the stick for their
    // items). Running "forward" follows the view: turning with the other thumb turns the run, as in PUBG.
    // (A lock in every direction, with a ring around the stick, was tried and players preferred this.)
    let locked = false;
    const unlock = () => {
      locked = false;
      stick.classList.remove('locked', 'fwd');
      knob.style.transform = '';
      setArrows(new Set());
    };
    const lock = (f) => {
      locked = true;
      stick.classList.add('locked');
      const R = stick.getBoundingClientRect().width / 2;
      knob.style.transform = `translate(0px, ${-R * 0.6}px)`;
      setArrows(new Set(['up']));
      f.kind = 'lockedstick';   // the rest of this touch does nothing
    };
    const steer = (f, x, y) => {
      const r = stick.getBoundingClientRect(), R = r.width / 2;
      let dx = x - (r.left + R), dy = y - (r.top + R);
      const len = Math.hypot(dx, dy), lim = R * 0.6;
      // the thumb reached the running figure: lock
      if (C.inMission() && stick.classList.contains('fwd')) {
        const l = lockEl.getBoundingClientRect();
        if (Math.hypot(x - (l.left + l.width / 2), y - (l.top + l.height / 2)) < l.width * 0.6) { lock(f); return; }
      }
      if (len > lim) { dx *= lim / len; dy *= lim / len; }
      knob.style.transform = `translate(${dx}px, ${dy}px)`;
      // 8 directions with a dead zone; diagonals press two arrows, as on a keyboard
      const next = new Set();
      if (len > R * 0.22) {
        const a = Math.atan2(dy, dx), s = Math.sin(a), c = Math.cos(a);
        if (s < -0.38) next.add('up'); if (s > 0.38) next.add('down');
        if (c < -0.38) next.add('left'); if (c > 0.38) next.add('right');
      }
      stick.classList.toggle('fwd', next.has('up') && C.inMission());
      setArrows(next);
    };
    const release = () => {
      knob.style.transform = ''; stick.classList.remove('fwd');
      setArrows(new Set());
    };

    // ---- the layout: positions (fractions of the stage, saved per browser), size (all, and per button) and opacity
    let layout = loadLayout();
    function loadLayout() {
      let s = {};
      try { s = JSON.parse(localStorage.getItem(LAYOUT_STORE) || '{}') || {}; } catch { s = {}; }
      const l = { pos: {}, scale: {}, size: 1, alpha: 1 };
      if (typeof s.size === 'number' && s.size >= 0.5 && s.size <= 2) l.size = s.size;
      if (typeof s.alpha === 'number' && s.alpha >= 0.2 && s.alpha <= 1) l.alpha = s.alpha;
      if (s.pos && typeof s.pos === 'object')
        for (const [id, p] of Object.entries(s.pos))
          if (Array.isArray(p) && p.length === 2 && p.every((v) => typeof v === 'number' && v >= 0 && v <= 1)) l.pos[id] = p;
      if (s.scale && typeof s.scale === 'object')
        for (const [id, k] of Object.entries(s.scale)) if (typeof k === 'number' && k >= 0.4 && k <= 2.5) l.scale[id] = k;
      return l;
    }
    const saveLayout = () => { try { localStorage.setItem(LAYOUT_STORE, JSON.stringify(layout)); } catch { /* storage blocked */ } };
    const placeable = () => [stick, ...root.querySelectorAll('.t-btn')];
    const idOf = (el) => (el === stick ? 'stick' : el.dataset.id);
    const applyLayout = () => {
      root.style.setProperty('--k', String(layout.size));
      root.style.setProperty('--a', String(layout.alpha));
      for (const el of placeable()) {
        const p = layout.pos[idOf(el)], k = layout.scale[idOf(el)];
        if (p) { el.style.left = `${p[0] * 100}%`; el.style.top = `${p[1] * 100}%`; el.style.right = 'auto'; el.style.bottom = 'auto'; el.style.transform = 'translate(-50%, -50%)'; }
        else { el.style.left = el.style.top = el.style.right = el.style.bottom = el.style.transform = ''; }
        if (k) el.style.setProperty('--bk', String(k)); else el.style.removeProperty('--bk');
      }
    };
    applyLayout();
    // Edit mode: the overlay comes up over everything (the boot cover too), every button and the stick drag,
    // a tap on one selects it for its own size slider; the bar at the top has the sliders. Works before the
    // game runs as well.
    const editBar = root.querySelector('.t-edit');
    let editing = false, selected = null;
    const syncEdit = () => {
      for (const r of editBar.querySelectorAll('input[type=range]'))
        r.value = r.dataset.k === 'one' ? (selected ? layout.scale[selected] || 1 : 1) : layout[r.dataset.k];
      editBar.querySelector('.te-one').classList.toggle('hidden', !selected);
      if (selected) editBar.querySelector('.te-sel').textContent = (selected === 'stick' ? 'Stick' : (BUTTONS.find((b) => b[0] === selected)?.[1] || selected).replace(/<[^>]*>/g, ' ').trim()) + ' size';
      for (const el of placeable()) el.classList.toggle('sel', idOf(el) === selected);
    };
    const openLayout = () => { editing = true; selected = null; sheet.classList.add('hidden'); syncEdit(); root.classList.add('edit'); };
    const closeLayout = () => { editing = false; selected = null; syncEdit(); root.classList.remove('edit'); for (const [id] of fingers) end({ pointerId: id }); };
    for (const r of editBar.querySelectorAll('input[type=range]'))
      r.addEventListener('input', () => {
        if (r.dataset.k === 'one') { if (selected) layout.scale[selected] = Number(r.value); } else layout[r.dataset.k] = Number(r.value);
        applyLayout(); saveLayout();
      });
    editBar.querySelector('.te-reset').addEventListener('click', () => { layout = { pos: {}, scale: {}, size: 1, alpha: 1 }; selected = null; applyLayout(); saveLayout(); syncEdit(); });
    editBar.querySelector('.te-done').addEventListener('click', closeLayout);
    const editDown = (e) => {
      const el = e.target.closest('.t-btn, .t-stick');
      if (!el) return;
      const r = el.getBoundingClientRect(), R = root.getBoundingClientRect();
      fingers.set(e.pointerId, { kind: 'edit', el, x: e.clientX, y: e.clientY, moved: 0, cx: r.left + r.width / 2 - R.left, cy: r.top + r.height / 2 - R.top, w: r.width, h: r.height });
      try { root.setPointerCapture?.(e.pointerId); } catch { /* synthetic */ }
    };
    const editMove = (f, e) => {
      const R = root.getBoundingClientRect();
      const cx = Math.max(f.w / 2, Math.min(R.width - f.w / 2, f.cx + e.clientX - f.x));
      const cy = Math.max(f.h / 2, Math.min(R.height - f.h / 2, f.cy + e.clientY - f.y));
      f.moved = Math.max(f.moved, Math.hypot(e.clientX - f.x, e.clientY - f.y));
      f.pos = [cx / R.width, cy / R.height];
      f.el.style.left = `${f.pos[0] * 100}%`; f.el.style.top = `${f.pos[1] * 100}%`;
      f.el.style.right = 'auto'; f.el.style.bottom = 'auto'; f.el.style.transform = 'translate(-50%, -50%)';
    };

    root.addEventListener('pointerdown', (e) => {
      if (e.target.closest('.t-edit')) return;   // the edit bar's own controls
      e.preventDefault();
      if (editing) { editDown(e); return; }
      try { root.setPointerCapture?.(e.pointerId); } catch { /* a synthetic event: nothing to capture */ }
      const btn = e.target.closest('.t-btn');
      const f = { x: e.clientX, y: e.clientY, t0: performance.now(), moved: 0 };
      if (btn) {
        const def = BUTTONS.find((b) => b[0] === btn.dataset.id);
        f.kind = 'btn';
        f.btn = btn;
        f.what = def[2];
        if (f.what.settings) { f.kind = 'ui'; openSettings(); }
        else {
          if (def[4] === 'hold') { latched[def[0]] = !latched[def[0]]; btn.classList.toggle('on', latched[def[0]]); press(f.what, latched[def[0]]); }
          else {
            if (def[4] === 'latch') { latched[def[0]] = !latched[def[0]]; btn.classList.toggle('on', latched[def[0]]); }
            else if (def[4] !== 'scope') btn.classList.add('on');
            press(f.what, true);
          }
        }
      } else if (e.target.closest('.t-stick')) {
        f.kind = 'stick';
        if (locked) unlock();   // a touch on a locked stick frees it (and steers from here)
        steer(f, e.clientX, e.clientY);
      } else f.kind = 'look';
      fingers.set(e.pointerId, f);
    }, { passive: false });

    root.addEventListener('pointermove', (e) => {
      const f = fingers.get(e.pointerId);
      if (!f) return;
      e.preventDefault();
      if (f.kind === 'edit') { editMove(f, e); return; }
      if (f.kind === 'lockedstick') return;
      if (f.kind === 'stick') { steer(f, e.clientX, e.clientY); return; }
      // look, and the fire buttons too: hold fire and drag to aim while shooting
      if (f.kind === 'look' || f.btn?.dataset.id === 'fire' || f.btn?.dataset.id === 'fire2') {
        const dx = e.clientX - f.x, dy = e.clientY - f.y;
        f.moved += Math.abs(dx) + Math.abs(dy);
        move(dx, dy);
      }
      f.x = e.clientX; f.y = e.clientY;
    }, { passive: false });

    const end = (e) => {
      const f = fingers.get(e.pointerId);
      if (!f) return;
      fingers.delete(e.pointerId);
      if (f.kind === 'edit') {
        if (f.pos && f.moved > 6) { layout.pos[idOf(f.el)] = f.pos; saveLayout(); }
        else { if (f.pos) applyLayout(); selected = idOf(f.el); syncEdit(); }   // a tap: select it for sizing
        return;
      }
      if (f.kind === 'btn') {
        const def = BUTTONS.find((b) => b[0] === f.btn.dataset.id);
        if (def[4] !== 'hold') {   // a held button's key stays down until its next tap
          if (!def[4]) f.btn.classList.remove('on');
          press(f.what, false);
        }
      } else if (f.kind === 'stick') release();
      // A tap (not a drag) clicks where the finger is: the game steers its cursor there first when its profile
      // says where the cursor is (host/input.c: input_tap; in the menus and the Esc menu). In a mission, with
      // no cursor, a tap does nothing. Otherwise it clicks wherever the cursor is.
      else if (f.kind === 'look' && f.moved < 12 && performance.now() - f.t0 < 300) {
        const p = gamePoint(f.x, f.y);
        if (!(Mod && Mod._input_tap && Mod._input_tap(p.x, p.y))) { C.press({ mouse: 0 }, true); C.press({ mouse: 0 }, false); }
      }
    };
    root.addEventListener('pointerup', end);
    root.addEventListener('pointercancel', end);
    root.addEventListener('contextmenu', (e) => e.preventDefault());
    // no double-tap zoom, pinch or page scroll while playing (the edit bar's sliders keep their default)
    const block = (e) => { if (!e.target.closest('.t-edit')) e.preventDefault(); };
    root.addEventListener('touchstart', block, { passive: false });
    root.addEventListener('touchmove', block, { passive: false });
    window.addEventListener('blur', () => {
      for (const [id] of fingers) end({ pointerId: id });
    });

    // ---- settings sheet: sensitivity, scope zoom, gyroscope, layout; the game keeps running underneath
    const sheet = document.createElement('div');
    sheet.id = 'tsettings';
    sheet.className = 'hidden';
    sheet.innerHTML = `
      <div class="ts-card">
        <div class="ts-head"><b>Controls &amp; settings</b><button type="button" class="ts-close" aria-label="Close">×</button></div>
        <div class="ts-row"><span>Button layout</span>
          <button type="button" class="ts-layout">Edit layout: move buttons, size, opacity</button></div>
        <label class="ts-row"><span>Swipe sensitivity <output data-for="look"></output></span>
          <input type="range" min="0.3" max="3" step="0.1" data-k="look"></label>
        <label class="ts-row"><span>Scoped sensitivity (swipes and gyro while zoomed) <output data-for="scopeSens"></output></span>
          <input type="range" min="0.3" max="3" step="0.1" data-k="scopeSens"></label>
        <div class="ts-row"><span>SCOPE button zooms to</span>
          <div class="ts-seg" data-k="scopeZoom">
            <button type="button" data-v="2">2×</button><button type="button" data-v="4">4×</button><button type="button" data-v="6">6×</button><button type="button" data-v="8">8×</button>
          </div></div>
        <div class="ts-row"><span>Gyroscope aiming</span>
          <div class="ts-seg" data-k="gyro">
            <button type="button" data-v="off">Off</button><button type="button" data-v="scope">Scoped only</button><button type="button" data-v="on">Always</button>
          </div><small class="ts-note" id="ts-gyro-note"></small></div>
        <div class="ts-row"><span>Gyroscope direction</span>
          <div class="ts-checks">
            <label><input type="checkbox" data-k="gyroInvY"> Invert up / down</label>
            <label><input type="checkbox" data-k="gyroInvX"> Invert left / right</label>
          </div></div>
        <div class="ts-row"><span>Subtitles</span>
          <div class="ts-checks">
            <label><input type="checkbox" data-k="subs"> Large subtitles for the game's speech</label>
          </div></div>
        <label class="ts-row"><span>Gyroscope sensitivity <output data-for="gyroSens"></output></span>
          <input type="range" min="0.2" max="3" step="0.1" data-k="gyroSens"></label>
        <div class="ts-row"><span>Screen</span>
          <div class="ts-seg" data-k="wide">
            <button type="button" data-v="1">Full screen (wide)</button><button type="button" data-v="0">Original 4:3</button>
          </div></div>
        <div class="ts-foot">
          <button type="button" class="ts-guide">How to play</button>
          <button type="button" class="ts-reset">Reset</button>
        </div>
      </div>`;
    stage.appendChild(sheet);
    const fmt = (v) => `${Number(v).toFixed(1)}×`;
    const syncSheet = () => {
      const s = C.get();
      for (const r of sheet.querySelectorAll('input[type=range]')) {
        r.value = s[r.dataset.k];
        sheet.querySelector(`output[data-for="${r.dataset.k}"]`).textContent = fmt(r.value);
      }
      for (const c of sheet.querySelectorAll('input[type=checkbox]')) c.checked = !!s[c.dataset.k];
      for (const b of sheet.querySelectorAll('.ts-seg[data-k="gyro"] button')) b.classList.toggle('on', b.dataset.v === s.gyro);
      for (const b of sheet.querySelectorAll('.ts-seg[data-k="scopeZoom"] button')) b.classList.toggle('on', Number(b.dataset.v) === s.scopeZoom);
      for (const b of sheet.querySelectorAll('.ts-seg[data-k="wide"] button')) b.classList.toggle('on', (b.dataset.v === '1') === s.wide);
      const note = sheet.querySelector('#ts-gyro-note'), st = C.gyroState();
      note.textContent = s.gyro === 'off' ? 'Turn the phone to aim, like PUBG. Needs a gyroscope.'
        : st === 'denied' ? 'Motion access was refused. Allow it in the browser\'s site settings, then try again.'
        : st === 'none' ? 'No gyroscope found on this device.'
        : st === 'on' ? (s.gyro === 'scope' ? 'On while scoped: turn the phone to fine-aim; swipe still works.' : 'On. Turn the phone to look around; swipe still works.') : 'Starting…';
    };
    for (const r of sheet.querySelectorAll('input[type=range]'))
      r.addEventListener('input', () => { C.set(r.dataset.k, Number(r.value)); syncSheet(); });
    for (const c of sheet.querySelectorAll('input[type=checkbox]'))
      c.addEventListener('change', () => { C.set(c.dataset.k, c.checked); syncSheet(); });
    for (const b of sheet.querySelectorAll('.ts-seg[data-k="gyro"] button'))
      b.addEventListener('click', async () => {
        C.set('gyro', b.dataset.v);
        if (b.dataset.v !== 'off') { syncSheet(); await C.gyroEnable(); setTimeout(syncSheet, 1600); }
        syncSheet();
      });
    for (const b of sheet.querySelectorAll('.ts-seg[data-k="scopeZoom"] button'))
      b.addEventListener('click', () => { C.set('scopeZoom', Number(b.dataset.v)); syncSheet(); });
    for (const b of sheet.querySelectorAll('.ts-seg[data-k="wide"] button'))
      b.addEventListener('click', () => { C.set('wide', b.dataset.v === '1'); syncSheet(); });
    sheet.querySelector('.ts-reset').addEventListener('click', () => { C.reset(); syncSheet(); });
    sheet.querySelector('.ts-close').addEventListener('click', () => sheet.classList.add('hidden'));
    sheet.querySelector('.ts-guide').addEventListener('click', () => { sheet.classList.add('hidden'); showGuide(); });
    sheet.querySelector('.ts-layout').addEventListener('click', openLayout);
    sheet.addEventListener('pointerdown', (e) => { if (e.target === sheet) sheet.classList.add('hidden'); });
    const openSettings = () => { syncSheet(); sheet.classList.remove('hidden'); byId('gear').classList.remove('on'); };

    // ---- the controls guide: shown on the first start (over the boot screen), later from the settings. Its
    // gyroscope switch starts the gyroscope from the "Let's go" tap, the user gesture iOS needs for permission.
    const guide = $('tguide');
    const showGuide = () => {
      if (!guide) return;
      const sw = guide.querySelector('#tg-gyro');
      if (sw) sw.checked = C.get().gyro !== 'off';
      guide.classList.remove('hidden');
    };
    if (guide) {
      guide.querySelector('.tg-ok').addEventListener('click', () => {
        guide.classList.add('hidden');
        try { localStorage.setItem(GUIDE_SEEN, '1'); } catch { /* storage blocked */ }
        const sw = guide.querySelector('#tg-gyro');
        if (sw) {
          if (sw.checked) { if (C.get().gyro === 'off') C.set('gyro', 'scope'); C.gyroEnable().then((st) => { if (st === 'denied') C.set('gyro', 'off'); }); }
          else C.set('gyro', 'off');
        }
      });
    }
    const firstGuide = () => {
      let seen = false;
      try { seen = localStorage.getItem(GUIDE_SEEN) === '1'; } catch { /* storage blocked */ }
      if (!seen) showGuide();
    };
    ui = { root, openSettings, openLayout, showGuide, firstGuide, live: () => { root.classList.remove('off'); document.body.classList.add('touch'); } };
    return ui;
  }
  const $ = (id) => document.getElementById(id);

  // The game is running: wire the overlay to it and show it
  function attach(M, stage, canvas) {
    const u = setup(stage, canvas);
    Mod = M;
    RcControls.attach(M);
    u.live();
    u.firstGuide();
  }
  const openSettings = () => ui?.openSettings();
  const openLayout = () => ui?.openLayout();

  // Landscape, fullscreen where the browser allows it (not on iPhone; there, Add to Home Screen gives the
  // most room). Called from the Play tap, which counts as the user gesture both need.
  function goFullscreen(stage) {
    const fs = stage.requestFullscreen || stage.webkitRequestFullscreen;
    const p = fs?.call(stage, { navigationUI: 'hide' });
    Promise.resolve(p).then(() => screen.orientation?.lock?.('landscape')).catch(() => {});
  }

  return { wanted, setup, attach, openSettings, openLayout, goFullscreen };
})();
