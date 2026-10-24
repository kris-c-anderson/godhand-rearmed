# Changelog

Newest first. Each change is commented in the code where it was made.

## October 24, 2026

- **Near geometry** (`[Video] NearGeometry`, on; launcher Graphics; read at start): walls, pillars and floors next to
  the camera are drawn instead of vanishing. The PS2 cannot draw a triangle with a corner off its 16-bit screen range or
  behind the camera, so the game's scenery renderers drop such triangles whole, and a wall the camera comes close to
  disappears (it was never hidden on purpose: its triangles are sent every frame, then dropped). Their clip test is
  patched to the far plane only (in VU1 micro memory as each upload lands, and in the uploads compiled ahead of time),
  and every PATH1 packet goes through a filter that cuts what the GS cannot take from the full 32-bit positions the
  packet still carries: along the screen range (exact: everything the GS interpolates is affine on screen), and at the
  near plane in clip space for triangles reaching behind the camera. The game's strip stitching is read from each
  batch's input in VU1 memory, so stitches stay out and back faces are recognised (src/godhand/near_geometry.cpp;
  runtime hooks in ps2_vu1_hooks.h). F2 > Graphics shows its counters.
- **See-through** (`[Video] SeeThrough`, on; launcher Display; F2 > Camera sizes it live): walls, pillars and stalls
  between the camera and Gene dissolve in a soft-edged oval round him (an ordered dither, densest in the middle), so
  he and the enemies about him stay in view. Only what is more than `SeeThroughMargin` (1 m) nearer the camera than
  Gene dissolves; he, the enemies round him, the floor at his feet and the HUD are untouched. The game projects Gene
  with the camera's own matrix every frame (camera.cpp, hook in cCamera_update) and the renderer dissolves the
  depth-tested draws (pcsx2gs `see_through`, `GSRendererHW::ApplySeeThrough`, tfx.fx `PS_SEE_THROUGH`). Off in
  cutscenes.
- **Radar follows the Modern free camera:** while the free camera has the view, the radar turns with the camera
  (up = where it looks) instead of with Gene's facing (hook in func_00143D20, where it copies his rotation).
- **Precise depth keeps the PS2's ties:** draws that test depth without writing it (shadows, decals, glows laid on a
  surface) passed on the PS2 wherever their integer depth tied the surface's; against exact depths they passed or
  failed pixel by pixel (the roof beams' shadows on the town's walls flickered). They now get the top of their
  integer step.
- **Modern controls: mouse camera** (`[Game] MouseCamera`, on; `MouseSensitivity` 0.1 degrees per pixel; F2 >
  Controls): the mouse turns the free camera. A click in the game captures it, Esc frees it. It never reaches the
  game as the right stick, so it cannot dodge.
- **Precise depth** (`[Video] PreciseDepth`, on; F2 > Graphics live): the jagged, shifting slivers where distant
  surfaces meet (a pillar against a ledge) are the game's own z-fighting: its 16-bit depth falls as 1/distance from a
  near plane at 0.1 m, so a depth step is 6 cm at 20 m and 55 cm at 60 m (the same in PCSX2; under a pixel at the
  PS2's resolution). The renderer now takes the depth of perspective-textured surfaces from their Q (1/w, a full
  float), which the projection makes an exact line of the integer depth, keeping 8 bits of fraction. (A first try
  moved the near plane out instead; the game culls what comes nearer than it, so walls and floors near the camera
  vanished. Reverted.)
- **Walls no longer vanish near the camera, and things behind them no longer show through** (with a wider field of
  view): the game's depth range was put back for the player camera's projection only, but part of the scenery is
  drawn with a second projection built from the same widened view, so the two disagreed by the field-of-view factor
  and a near wall could lose to a cabinet behind it. Every projection keeps the game's own range again (the wider
  view shrinks it, as before); precise depth makes up the precision.
- **Modern controls:** Auto's fight range 3 m; free camera 200 degrees a second by default; it can look further up,
  as far as keeps it 0.35 m above Gene's feet.
- **Control Style: Modern** (`[Game] ControlStyle=modern`, Gameplay, F2 > Controls): the right stick turns a free
  camera round Gene and the left stick moves him relative to the camera (he faces where he walks). The original
  controls (tank movement, right stick = dodges, the camera behind Gene) while L1 is held (or toggled with
  `ModernL1=toggle`), and with `ModernAuto` in fights: from the battle camera coming on until no enemy has been within
  3 m for 1.5 s. Double-tap L1 = the original quick turn, the camera staying behind Gene through it. Whenever the game
  takes the camera (grab and special-move close-ups, cutscenes, menus) the controls are the original ones.
- **Depth of field switch** (`[Video] DepthOfField`, Graphics; F2 > Graphics switches it while playing): the game's
  distance blur is composited from a quarter-size copy and mask of the picture, which at high internal resolutions
  shows as dark, jagged, shimmering fringes along the edges of distant geometry. The renderer takes the switch live
  (`pcsx2gs_set_option`).
- **Enemies follow the wider view** (`[Game] EnemiesFollowView`, on by default; Options > Show advanced settings):
  the angles round Gene in which enemies close in and lunge widen by as much as the picture reaches further round him
  than the PS2's did (`docs/enemy-ai.md`), so enemies at the edges of a wide view fight as they did at the PS2's
  edges. Nothing changes at 4:3 and 100%.
- **F2 in-game menu, Camera tab:** field of view, field of view in fights, camera distance and tilt, the enemy
  setting and the presets, all applied while playing; "Keep playing" leaves the controller and keyboard to the game
  and the mouse to the menu; Save writes godhand.ini and shows the values as a preset line.
- **Launcher:** Display > Camera offers presets ("As on the PS2", "Wider", "Widest", "Action"); "Show advanced settings" shows
  the camera's own values.
- **Field of view** (Display > Field of view, `[Video] FieldOfView`, 100-150%): the gameplay camera's own fov is
  scaled where the game eases it toward its target, so the projection and culling follow; cutscenes keep their
  framing. **Field of view in fights** (`FightFieldOfView`, default 100%): the same while the game's battle camera is
  on, so fights are framed as on the PS2 by default; the camera eases between the two. **Camera distance**
  (`CameraDistance`, default: comes all the way in) moves the camera toward Gene as the view widens so he keeps his
  size instead of the view looking zoomed out; **Camera tilt** (`CameraTilt`, degrees) raises it to look down at him.
- **How enemies pick their moment** is written up in `docs/enemy-ai.md`, from the code: a queue by distance to Gene,
  a wedge of 60 degrees either side of the way he faces, lunges inside a cone that widens with the level gauge, and
  Hard as the gauge locked at Level DIE. No camera test is involved, so widescreen and the field of view do not
  change how enemies fight (an earlier note here said the game's on-screen test decided it; that test only decides
  what is drawn).
- **HD packs:** replacement textures keep to the alpha levels of the game's own texture (no more see-through enemies
  or grainy distance haze with packs made from edited dumps).

## October 23, 2026

- **The project is God Hand: Rearmed**, on its own `main` branch; the launcher and window carry the name, and the
  launcher draws `resources/images/logo.png` when it is there.
- **Function names from the decomp:** `recomp/godhand_functions.csv` carries the decomp's names (4,255 of the 10,553
  functions; `tools/symbols/import_decomp_names.py` refreshes them), the generated code is named after them, and the
  crash log, the hang dump, the PC sampler and the call profiler print `cPlCamera_update+0x1c` instead of an address.
- **Post effects option** (Graphics): the game's blur and heat-haze passes at the PS2's resolution (PCSX2's fix, grainy
  at 3x and up) or upscaled.
- **Xavier:** the arm stump is capped, Gene's God Hand arm is clipped exactly where it enters the stump and its skin
  fades in from the fur colour, the gauntlet stays under the shoulder pad, the cheeks sit inside the loincloth, the
  hair strips no longer z-fight (lifted off the scalp and each other), the hair a little shorter and off the neck.
- **Playable characters:** `[Mods] Character=gene|xavier`; Xavier (from *Xavier: Renegade Angel*) in place of Gene,
  with Gene's God Hand arm and his loincloth on the coat's cloth physics, full-resolution textures through the
  renderer's replacement system. Character files are written over the game's at disc level (the AFS table is never
  changed: a changed size broke later files).
- **Launcher** in the game window: Install (pick and check the disc image), Play, Options (Display, Graphics, Gameplay,
  Mods, Controls), "don't show again".
- **Unlock Hard** and **mute music** options. **Roulette**: d-pad scrolls the God Reel, up steps back with the
  animation following, `RouletteTimer=double` for twice the time.
- **Menus** drawn as on the PS2 (4:3 with side bars) or stretched; the in-game HUD at the screen edges (16:9 layout)
  or where the PS2 had it.

## October 22, 2026

- **PCSX2's hardware renderer** (Direct3D 11, `pcsx2gs.dll`) is the default, with God Hand's PCSX2 GameIndex fixes.
- **VU1 microcode compiled ahead of time** per upload (`build.py vu`), captured uploads included; MTVU on by default.
  The in-game slowdowns of the first builds are gone.
- **60 FPS mode** (`[Game] Fps60=1`): a port of the "60 Fps" PCSX2 patch as switchable recompiler patches, with the
  places where the port's timing differed from PCSX2 fixed (a stuck left stick, the roulette timer and dial, HUD and
  menu animation speed, stick-wiggle and scrolling-arrow prompts).
- **L3 dash** (`L3Dash=1`): clicking the left stick dashes (a quick double tap forward), also while walking.
- **Any aspect ratio** (`[Video] AspectRatio`): a port of the PCSX2 widescreen patch plus a HUD layout system that
  places each on-screen group per screen third; low-health corners and other screen-edge overlays stay stretched.
- **Crash log** for normal runs (exception, module and offset, guest pc, host stack), a PC sampler
  (`GODHAND_PC_SAMPLE`), a call profiler (`GODHAND_CALL_PROFILE`), a counter finder, HUD frame dumps (F6) and the RAM
  dump (F8).
- Window opens at a usable size; SDL3 controllers on; build fixes.

## Before the fork: LasVola's godhand-recomp (to October 20, 2026)

The entries below are from the project God Hand: Rearmed was forked from.

## October 20, 2026

- **Minigames show the proper buttons:** the crates in the pirate ship cannon minigame (stage 3) showed the PS2
  symbols. They now show your bound keys, or A/B/X/Y on an Xbox-style controller, and switch the moment you change
  between keyboard and controller (`src/godhand/crate_prompts.cpp`, `src/godhand/button_prompts.cpp`). A PlayStation
  controller keeps the original symbols.
- **Sound cutting out in stage 3:** the same kind of gap as in stage 2 (the streamed music reaching the sound
  processor late). Data transfers to the sound side are now reported finished after a short, PCSX2-like delay
  (`Kernel/Stubs/SIF.cpp`). Not heard in the next play session. New log lines (`[sif-dma]`, `[spu2-adma]`) show it if it comes back.
- Note for developers: `GODHAND_STALL_DUMP` pauses the game itself for up to about 1.5 s each time it fires (it loads
  the debug symbols). Use it only to catch a freeze, not during normal play.

## October 19, 2026

- **"Heat wave" ripples in cutscenes** (stage 3): the game's depth-of-field blur on the far background was drawn
  wrong (wavy bands at 1x, sharp with ripples at 3x). It now matches the PS2: an ordered path for the game's
  in-place depth shuffle, and the blur passes keep their original resolution (`gs_gpu_backend.cpp`).
- **Freezes up to about a second** when the game read from the disc: the port reopened the whole disc image for
  every read. It now stays open, and the image is loaded into memory in the background at start
  (`Kernel/Stubs/Helpers/Support.h`, `src/main.cpp`; `GODHAND_ISO_PREFETCH=0` turns the background load off).

## October 18, 2026 (evening)

- **Sound cutting out** (tiny gaps in music and voices, worst in stage 2 and the God Reel): the port told the game
  that every data transfer to the sound processor was still busy for one frame too long, which slowly starved the
  streamed music. Fixed in the SIF DMA status (`runtime/ps2xRuntime/src/lib/Kernel/Stubs/SIF.cpp`).
- **Grid of lines over the sepia flashback cutscenes** at 3x: some 1:1 copies were filtered slightly differently from
  the PS2. They now read pixels exactly like the PS2 does (`gs_gpu_backend.cpp`, `gs_sprite_rules.h`).
- **Thin line under movies** at 3x: the movie picture was filtered past its own edge. Fixed the same way.
- **The game could close while saving** after about 30 minutes of play: a small memory area for game threads was
  never given back. It is now reused (`EeScheduler.cpp`).
- **Button prompts show your bound keys** (for example "F" instead of the PS2 circle), or Xbox-style names with a
  controller (`src/godhand/button_prompts.cpp`).
- **Controls:**
  - Start/Select no longer reach the game while you press Back + Start for the Controls screen.
  - Alt+Tab and similar shortcuts are no longer recorded as a key while rebinding.
  - Keys bound to two buttons are flagged on the F1 screen.
  - The Windows Sticky Keys popup no longer appears while playing.
- **Rare missing 3D pieces** in busy scenes: long-running VU1 programs are now finished instead of cut off.
- **The ending credits cannot hang** if a movie ever stalls (a 20-second safety check).
- Diagnostics for developers: `GODHAND_STALL_DUMP=<ms>` prints every thread's stack when the game stalls.

## October 18, 2026 (morning)

- **Boss "CHECK" freeze** (stage 1), second cause: a "stop sound" request could interrupt the sound mixer. Such
  requests now wait for the sound driver, as on a PS2.
- **Slow cutscenes:** missing ahead-of-time VU1 programs are now logged and saved to `logs\vu1_missing\`.
- Caps Lock / Num Lock bindings follow the key, not the keyboard light.
- The launcher keeps the previous session's log.

## October 17, 2026

- First public release: playable through stage 1. Fixes for boot, pad input, graphics speckle, sound effects,
  doubled audio, the floor rendering, the boss "CHECK" freeze (first cause), and the in-game Controls screen.
