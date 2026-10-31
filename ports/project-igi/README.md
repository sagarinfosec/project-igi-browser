# Project I.G.I. port

`profile.h` names the game's executable (`igi.exe`), its install path as the game sees it, and the addresses
the touch controls use to steer the menu cursor. The game talks to DirectDraw 7 / Direct3D 7, which
`host/ddraw7.c` provides on top of the same WebGPU renderer the Direct3D 8 layer uses.

Build and run (from the repo root, with the tools listed in the top-level README):

```bash
GHIDRA=~/ghidra_12.1.4_PUBLIC ghidra/export.sh /path/to/igi.exe work/functions.json
PROFILE=ports/project-igi/profile.h ./build.sh /path/to/igi.exe work/functions.json   # native app
PROFILE=ports/project-igi/profile.h ./build_web.sh                                   # browser build
python3 web/serve.py --game /path/to/Project_IGI_folder                              # http://localhost:8007/?devgame
```

Useful switches: `?log` (host log in the console), `?input=500:tap=320:200,...` (scripted input, see
`host/input.c`), `?mem=compact|full` and `?gpu=coalesce|direct` (the WebKit memory paths, on any browser).

Controls on the page: `web/controls.js` holds the game's actions with their default keys (from its
`config.qvm`), the mouse-button bindings, the scope toggle, the sensitivity settings and the gyroscope;
`web/loader.js` renders the Controls panel (keyboard layout presets, keys, mouse, screen) and `web/touch.js`
the on-screen buttons, settings sheet, layout editor and guide. The game's own Configuration menu must stay
at its defaults for these to match (loader.js refetches the shipped `config.qvm` on every start). Gyroscope
signs (right-hand rule in the device frame, `rotationRate.alpha/beta/gamma` = rates about device x/y/z in
Chromium and WebKit): turning left is a positive yaw, tilting the camera up a positive pitch, both sent as
negative mouse counts (+x looks right, +y looks down); the first version had the pitch backwards. The touch
layout is saved as fractions of the stage per control (`igi-layout`), with a size factor (`--k`), per-control
factors (`--bk` on the element) and opacity (`--a`) the stylesheet multiplies in. The stick's sprint lock
engages when, pushing forward in a mission, the thumb reaches the runner icon 2.1 stick radii above the stick's
centre, and holds the forward arrow until the next touch on the stick (a lock in every direction, with a
ring around the stick, was tried on 2026-10-29 and players preferred this). `?gyrotest` shows a gyroscope readout. Project I.G.I. has no prone / crawl: crouch is the lowest stance. Crouch and walk are held keys (verified
natively: a tapped crouch key does nothing lasting, a held one lowers the camera), binoculars a toggle, and
only the sniper rifle's scope and the binoculars zoom: the zoom keys do nothing on their own, even with the
Dragunov. Alternate fire (Backspace) raises the rifle's scope (the view zooms to 1.18x within 4 frames, then
the zoom keys zoom it further) and lowers it again, resetting the zoom; with the other weapons (knife, grenade
checked) it does nothing. The page's scope toggle presses Alternate fire first and watches the zoom, then
falls back to the binoculars.

Mission flow: a level ends when the LevelFlow task's Complete / Failed expressions (from `OBJECTS.QVM`, e.g.
`CutScene_2222.isFinished`) come true; the game loop then saves `config.qvm`, checks the first byte of
`ymbe.afp` is `@` (the `.AFP` files are that key, not profiles), and loads the next mission by itself
(verified natively and on the web by forcing the flag: `RC_POKE=frame:bc2360>7c:00000000,frame:bc2360>60:01`,
the first write disables the expression that would overwrite the flag). A failed level opens the game's menu
on its own, with Resume greyed out; mission 4 (GOD) fails 45 s after its intro if the player hasn't acted
(`LevelTimer_760`, "Too many casualties mission aborted") — the game's rule, not the port's; the page shows
a "Mission failed, tap Restart Mission" note (`input_level_failed`, `PROFILE_LEVELFLOW`). Mission 4 starts
with knife, grenade, Dragunov and MP5SD (previous-weapon cycles knife > grenade > Dragunov > MP5SD).

Game state the host reads (`profile.h`, found by diffing `RC_MEMSNAP` snapshots of `.data`): the main menus'
cursor (`PROFILE_CURSOR_X/Y`, ints, 640x480 units), the Esc menu's cursor sprite (`PROFILE_CURSOR2_X/Y`,
floats in display-mode pixels, valid while paused) and the game clock (`PROFILE_TICK`, frozen while the Esc
menu is up). The zoom of the view comes from the projection matrix of the world draws (`input_zoom`).
Measured (native, `RC_DUMP` of the camera matrix at 0x5ca140): the view turns 0.212° per mouse count at speed,
only ~0.11° per count below 50 counts a frame, and at most ~54° a frame; `host/dinput.c` undoes that curve.
While zoomed the game already scales turning by the zoom (0.7x in the binoculars at 1x, 0.11x at 5x), so the
page adds only the player's "scoped sensitivity" factor, nothing automatic. All missions are selectable in the
mission list (`tap=380:218+16*n` on the 640x480 screen, n = 0 Trainyard, 3 GOD, 4 Radar Base ...).

Widescreen (2026-10-30): the game clips the polygons nearest the camera itself (software transform, submitted
pre-transformed with the depth test on, clipped to its screen), so widening only the projection showed sky at
the sides when looking down. Now `host/ddraw7.c` EnumDisplayModes offers every mode reshaped to the window's aspect
(16:9 at least, 2.4:1 at most) with its pixel count kept (800x600 becomes 924x520 at 16:9, 640x480 800x450),
because the game takes the offered mode nearest in pixel count to the one it wants (keeping heights instead,
it took 854x480 for a mission wanting 800x600, and 640x480 on a 2.14:1 phone), so it picks and sets the wide
counterpart of its setting itself;
`patch.sh` multiplies the 20 generated-code reads of its camera's horizontal half-FOV tangent (0xbcab20) by
`rc_aspect_x` (runtime.h), which `host/render_rec.c` sets per frame: (display aspect)/(4/3) on frames drawn
wide (`g_frame_wide`: wide wanted, the previous frame drew a world, and its 2D quads drawn after the last world
draw covered under half the screen — the sniper scope and binocular masks are tiles that cover it all, the
sky's cloud strips come before the world, a fade is one quad over everything and doesn't count), 1 otherwise.
The executor (`host/render_wgpu.c` exec_draw, `CmdDraw.wide`) draws a wide frame's world over the fitted
picture and its 2D (no depth test) squeezed into a 4:3 box, full-width quads over a world excepted (cutscene
bars, fades); other frames are the 4:3 picture squeezed back from the wide screen, in the largest 4:3 box,
with clears kept inside the box. `RC_WIDE_DISPLAY=0` offers the 4:3 modes again (testing).

CPU and I/O (2026-10-30): the game's loop polls PeekMessage ~6.7 M times a second between frames (a spin that
burned a core); `host/user32.c` sleeps 1 ms on every 128th poll in a row with no message (RC_NO_YIELD=1 /
`?noyield` off, RC_PEEK_LOG=1 / `?peeklog` logs intervals): 35% of a core instead of 102% natively at the same
30 fps. In a mission the game reopens its ambient WAVs every few seconds; `host/kernel32.c` keeps small files'
contents after close (48 entries, 2 MB each, 48 MB; dropped on write/delete/rename) and serves the next open from
memory (mission 6: 345 of 600 opens). `web/loader.js` now defaults every browser to the compact memory layout
(engine memory ~0.5 GB instead of ~1.8 GB grown at start; `?mem=full` restores the old one) and polls the GPU
queue every 8 ms instead of 1.

COM objects (2026-10-30): `host/com.c` has 65536 slots; sound buffers (no destroy) leaked ~56/s in a mission and
the engine aborted ("too many live COM objects") after ~16 min, which is what players called "stuck". Fixed with
destroy routines in `host/dsound.c` (buffer + notify/3D views, shared sample memory) and `com_free` for texture
views in `host/ddraw7.c`. `RC_COM_LOG=1` logs live counts per interface every 2048 creations.

Pause detection (2026-10-30): the Esc-menu detector used the level timer (`PROFILE_TICK`) standing still, but the
last mission never starts that timer, so there the page thought the game was paused the whole time: no look
inversion (swipes felt inverted), the scope toggle refused (it needs a running mission) and taps were routed as
menu taps. Snapshots taken playing and paused in missions 1 and 14 (`RC_MEMSNAP`, four each, then "constant in
play, constant in pause, different between, same in both missions") found the game's control flag at 0x5468cc
(`PROFILE_PAUSE_FLAG`: 9 with control, 0 in the Esc menu, cutscenes and when dead); it can stay 0 after a resume,
so paused = flag 0 AND timer still. `RC_TICK_LOG=1` prints the detector's inputs every 60 frames.

Map computer (2026-10-31): 0x543c58 (`PROFILE_MAP_FLAG`) is 1 in play and 0 while the map is open (Esc leaves it
1); with the map open the host stops treating the mouse as look (`g_map`: no Y inversion, no look scaling, the
page sees "not in a mission"), which had made the map cursor feel inverted.

Subtitles (2026-10-31): the game draws speech in a 3-11 px bitmap font. Every glyph is a pre-transformed quad whose
texture rectangle identifies the character (packed atlases, one per font). `glyphs.h` holds the 113 rectangles of
the two subtitle fonts, learned by logging glyph quads every 20 frames in six missions (`RC_GLYPH_EVERY=20`) and
solving them against the game's English text files (`LANGUAGE/ENGLISH/*.RES`: a glyph line must match a substring
of a known string, spaces removed). `host/subtitle.c` reads each frame's glyphs back, groups lines, infers spaces
from gaps, keeps the lines that start with a speaker ("ANYA: ") and the lines that continue them, and exports the
text (`rc_subtitle`); the page shows it in a caption (`#subcap`, setting "Large subtitles", on by default).
`RC_SUB_LOG=1` prints each new subtitle natively. A new font or language needs the harvest and solve again.

Pause detection, third version (2026-10-31): 0x5468cc also dropped to 0 in normal play (mission 1 before the
first move, the last mission at times), and the last mission's timer never runs, so the page thought the game
paused: right-click fell through to the game's next weapon, swipes lost their inversion and smoothing.
`PROFILE_PAUSE_FLAG` is now 0xa4438c, next to the level timer: 0 in the Esc menu, loading screens and cutscenes,
2 or more in play (it counts up while the player moves). Found with snapshots across two pause/resume cycles in
missions 1 and 14, verified through the intro, the map, Esc twice and 3000 frames of walking.

Subtitle placement: `rc_subtitle_box` (left, top, right, bottom, line pitch as fractions of the game screen) and
`rc_subtitle_color`; the page draws the larger text over the game's own lines, phones only.

Device-lost recovery (render_wgpu.c): on a lost WebGPU device (Android Chrome restarts its GPU process when a system
dialog covers the browser; ~5-10% of sessions ended there) the next `rc_gpu_pump` drops every object of the old
device, requests a new adapter and device, rebuilds, and bumps `g_gpu_gen` (render_rec.c) so every texture is sent
again from guest memory. Tested with DevTools `Browser.crashGpuProcess`: restored in 0.2 s, textures intact.

WebGL 2 renderer (2026-10-31, `host/render_gl.c`): a second executor for the same commands, used when WebGPU is
missing or gives no adapter (about a quarter of the visitors who tried that day). Rows top first (clip y negated,
flipped blit at present), clip z mapped to OpenGL's range, `glFrontFace(GL_CCW)` for Direct3D's clockwise fronts,
fixed function only, S3TC when the browser has it. Draws are batched: vertices and uniforms staged in host memory and
uploaded once per batch (`glBufferSubData` per draw ran at 1.5 fps). Test switches: `?renderer=gl`, `?noadapter`
(simulates WebGPU without an adapter), `?nobc`, `?glprof`. Every report carries `r` (gpu / gl).

Testing switches: `RC_WATCH=frame:hexaddr` (native write watchpoint from that frame), `RC_MEMSNAP=frame:addr:len:path`, `RC_POKE=frame:addr:hexbytes`, `RC_DUMP=frame:addr:len`
(an address may be a pointer chain, `base>off>off`: each `>off` loads the dword so far and adds off, resolved
at that frame — for objects on the heap), `RC_ZOOM_LOG=1`
(the projection's zoom whenever it changes), `RC_PROJ_LOG=frame`, `RC_WINDOW=WxH` and `RC_WIDE=1` (native
widescreen), `RC_FORCE_MODE=WxH` (override the display mode the game asks for).
