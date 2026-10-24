<!-- Logo: drop the image at assets/rearmed-logo.png and uncomment the next line. -->
<!-- ![God Hand: Rearmed](assets/rearmed-logo.png) -->

# God Hand: Rearmed

**God Hand (PS2, 2006) as a native Windows game, and the modern port it never got: 60 FPS, any aspect ratio,
HD textures, a launcher, quality-of-life options, and new playable characters.**

Not an emulator: the game's own code is rebuilt as a PC program (static recompilation), with PCSX2's hardware renderer
for the graphics. You need your own copy of the game (God Hand, USA, SLUS-21503). **No game files are included.**

> [!WARNING]
> **Work in progress.** The port plays from the start of the game into stage 3; the later stages, the ending and the
> extras have not been played through on it yet. Expect bugs, and please report them ([Reporting bugs](#reporting-bugs)).

God Hand: Rearmed started as a fork of LasVola's [godhand-recomp](https://github.com/LasVola/godhand-recomp), which
got the game running; everything below the first two bullets of [What it adds](#what-it-adds) is this project's own
work. Improvements and discoveries from the recomp and from the
[God Hand decompilation](https://github.com/LucasPicoli/god-hand-decomp) are brought in as they land.

---

## What it adds

- **Runs natively** on Windows 10/11: the PS2 program recompiled to C++, the PS2 hardware reimplemented in `runtime/`.
- **PCSX2's hardware renderer** (Direct3D 11) with God Hand's own PCSX2 fixes, at 1x to 8x the PS2's resolution.
- **60 FPS mode:** the game runs at 60 instead of 30 with its timers, roulette, menus and prompts kept at the PS2's
  speed (a port of the "60 Fps" PCSX2 patch, fixed where the port's timing differed).
- **Any aspect ratio:** 16:9, 21:9, 32:9 or anything else, with the camera widened and the HUD placed at the screen
  edges (or kept where the PS2 had it); menus stay in the PS2's proportions or stretch, your choice.
- **A wider camera that still plays like the original:** a field of view option that brings the camera in so Gene
  keeps his size, fights framed as on the PS2, and enemies whose sense of "in front of Gene" widens with the picture,
  so the ones you can now see at the edges fight as they did at the PS2's edges. Walls between the camera and Gene
  dissolve round him, and the camera glides back after grabs instead of cutting. Tunable live in the F2 menu.
- **HD texture packs:** PCSX2-format replacement packs, with or without the character mods' own textures.
- **A launcher:** install (pick and check your disc image), play, and every option in one place: display, graphics,
  gameplay, mods, controls.
- **Quality of life:** L3 dash (click the stick to dash), d-pad scrolling and stepping back in the God Reel, a double-time
  roulette timer, Hard unlocked from the start, music mute, the game's blur effects at full resolution.
- **Playable characters:** a character system that swaps the player model at the disc level, with an example mod:
  **Xavier** from *Xavier: Renegade Angel*, wearing Gene's God Hand. More characters can follow the same way.
- **Keyboard, mouse and controllers**, rebindable in game (F1). Button prompts show the keys and buttons you use.
- **Diagnostics for developers:** crash log with function names (from the decomp), PC sampler, call profiler,
  frame and RAM dumps.

See [CHANGELOG.md](CHANGELOG.md) for what changed and when.

## Progress

| Part of the game | Status |
|------------------|--------|
| Opening, title menu, Clover logo movie | ✅ Played |
| Stage 1 and stage 2 (all areas, the bosses) | ✅ Played |
| Stage 3 (the amusement park) | 🟡 Gorilla Mask and Mad Midget Five beaten |
| Stages 4 to 8, ending, credits, Coliseum and extras | ⬜ Not played yet on the port |

| Game feature | Status |
|--------------|--------|
| Fighting, combos, Tension gauge, God Reel, dying and continuing | ✅ Works |
| Story cutscenes, saving and loading, casino and pirate ship minigames | ✅ Works |
| Movies other than the Clover logo (ending, staff roll, trailers) | ⬜ Not seen yet |

---

## How to set it up

### What you need

1. **Windows 10 or 11** and a reasonably modern graphics card.
2. **Visual Studio 2022 Build Tools** with "Desktop development with C++" (free from Microsoft).
3. **Python 3.10 or newer**.
4. **Your own God Hand (USA) disc image** (`.iso`, serial SLUS-21503). It must be this exact version: check it against
   [DISC_HASHES.md](DISC_HASHES.md). The port refuses any other disc.

### Build it

Open a terminal (PowerShell) and run these one at a time:

```powershell
git clone https://github.com/kris-c-anderson/godhand-rearmed.git
cd godhand-rearmed
copy "D:\path\to\God Hand (USA).iso" game\
python -I build.py generate
python -I tools\snr2\snr2.py --iso "game\God Hand (USA).iso" extract
python -I tools\snr2\gen_modules.py
python -I build.py modules
python -I build.py vu
python -I build.py configure -DGODHAND_PCSX2_GS=ON -DGODHAND_SDL_INPUT=ON
python -I build.py build
python -I build.py characters
```

- `generate` and the SNR2 steps turn the code from your disc into C++ and take a while; `vu` compiles the PS2's VU1
  microcode ahead of time; `characters` builds the playable characters from your disc.
- `build` downloads a few libraries the first time and then compiles everything (expect 10+ minutes).

### Play

Double-click **`Start-God-Hand.cmd`**, or run `build\port\bin\godhand.exe`. The launcher opens first: point it at your
disc image once, then Play. The very first start can sit on a black screen for a couple of minutes while the graphics
are prepared for your card; later starts take seconds.

Your settings are in `build\port\bin\godhand.ini` (every launcher option is a line there, with a comment), your saves in
`build\port\bin\memcards\`, your controls in `build\port\bin\controls.ini`.

### Options

| Where | What |
|---|---|
| Display | Aspect ratio, HUD layout (16:9 edges or 4:3), menu layout (4:3 or stretched), camera (presets; with "Show advanced settings": field of view, in fights, distance, tilt, enemies following the view), see-through walls, camera ease back, window / full screen, window size |
| Graphics | Internal resolution (1x–8x), post effects (PS2 resolution or upscaled), HD texture pack and its folder |
| Gameplay | 60 FPS, L3 dash, control style (classic / modern with mouse camera, work in progress), roulette timer (normal / double), Unlock Hard, mute music |
| Mods | Character: Gene or Xavier |
| Controls | Every key, mouse button and controller button (also F1 in game) |

In game: **F1** controls, **F2** in-game menu (the camera, tuned live), **F6** / **F7** menu and HUD layout, **F11** full screen, **F12** debugger.

---

## Mods

### Playable characters

A character is a folder `mods/characters/<id>/` next to the game with the disc files it replaces (`pl/pl00.dat` is
Gene's model) and, optionally, `textures/` with full-resolution replacements in PCSX2's format. The port writes them
over the game's files as the disc is read, so the game itself is never patched. Xavier is the example mod:
`assets/characters/xavier/` is its source; `tools/models/` has the model and texture writers for the game's formats and the Blender pipeline
that put Xavier onto Gene's skeleton (`tools/models/xavier/README.md`).

### HD textures

Any PCSX2 texture pack for God Hand (USA) works: set its folder in Graphics, or put it at `textures/SLUS-21503/`
next to `godhand.exe`.

---

## Troubleshooting

| Problem | What to do |
|---------|------------|
| "Wrong disc" message | Your image is a different version. Compare it with [DISC_HASHES.md](DISC_HASHES.md). |
| Black screen on the very first start | Wait: the graphics are being prepared. Later starts are fast. |
| Short pauses the first time an effect appears | Its shaders are being prepared; they do not come back once seen. |
| Controls screen (F1) does not draw | With the PCSX2 renderer it cannot be drawn yet: use the launcher's Controls tab. |
| The game crashes or freezes | Please report it with the log (next section). |

## Reporting bugs

Open an issue and attach the log of your last session: `build\port\bin\logs\play_last.log` (the session before that is
kept as `logs\play_<time>.log`), and `logs\crash.txt` if there is one. Say where in the game it happened and which
options were on (60 FPS, aspect ratio, character).

---

## For developers

- **How it works:** the main program (`SLUS_215.03`) is recompiled to C++ with the recompiler in `runtime/ps2xRecomp`;
  the 81 code overlays the game loads (rooms, events, memory card) are relocated and recompiled by `tools/snr2`, and
  `src/godhand` installs the right one as the game loads it. The PS2 hardware (graphics, sound, I/O processor) is in
  `runtime/`; graphics go through PCSX2's renderer (`thirdparty/pcsx2-gs`). Every game-specific fix is commented where
  it is made.
- **Function names:** `recomp/godhand_functions.csv` carries the boundaries and names of the game's functions; the names
  come from the [decomp](https://github.com/LucasPicoli/god-hand-decomp) and are refreshed with
  `python -I tools/symbols/import_decomp_names.py <decomp checkout>`. The generated code, the crash log, the hang dump,
  the PC sampler and the call profiler all use them.
- **Switchable patches:** `[[patches.conditional]]` and `[[patches.hooks]]` in `recomp/godhand.toml` (and per module in
  `recomp/modules/`) are compiled in both forms and picked by a run-time flag; `src/godhand/frame_patches.cpp` sets the
  flags from `godhand.ini`.
- **Faster 3D:** `python -I build.py vu` compiles every VU1 microcode upload found on the disc. A few uploads are placed
  by the game itself: play with `PS2X_VU1_CAPTURE=<folder>` set, then `python -I build.py vu <folder>` and build again.
- **Diagnostics** (environment or `[Runtime]` in `godhand.ini`): `GODHAND_PC_SAMPLE`, `GODHAND_CALL_PROFILE`,
  `GODHAND_FIND_COUNTERS`, `GODHAND_TRACE`; F8 in game dumps RAM and the VU1 trace.
- **Watching upstream:** changes in LasVola's recomp and in the decomp are reviewed weekly and ported when they help.

---

## Credits and legal

- [LasVola/godhand-recomp](https://github.com/LasVola/godhand-recomp): the recompilation this project started from
  (the running game, the SNR2 overlay loader, the button prompts, the first fixes), built on
  [PS2Recomp](https://github.com/ran-j/PS2Recomp) by ran-j and contributors, the
  [sotc-port runtime fork](https://github.com/LightVelox/PS2Recomp) by LightVelox, and the
  [Urban Reign port](https://github.com/thiagordsfx/urban-reign-recomp) by thiagordsfx (runtime, VU1 compiler, sound,
  PCSX2 graphics bridge).
- [LucasPicoli/god-hand-decomp](https://github.com/LucasPicoli/god-hand-decomp) and its contributors: function names
  and boundaries.
- [PCSX2](https://github.com/PCSX2/pcsx2): the hardware renderer in `thirdparty/`, and the God Hand GameIndex fixes.
- The "60 Fps" and "Widescreen 16:9" PCSX2 patches for SLUS-21503 (Puggsy and others): the basis of the 60 FPS mode and
  the aspect-ratio support.
- [Ace Combat 5 static recompilation](https://github.com/sal063/Ace-Combat-5-Static-recompilation): the movie decoder
  (Apache-2.0).
- Xavier and *Xavier: Renegade Angel* belong to Adult Swim. The Xavier mod is a fan-made example of the character
  system and is not affiliated with or endorsed by Adult Swim.
- God Hand: Rearmed is directed, play-tested and written by Kris, built with AI assistance (Claude
  by Anthropic), with ideas and testing from his friends.

The code is GPL-3.0 ([LICENSE](LICENSE)), like PS2Recomp. Third-party licenses are in `docs/licenses/` and the
`thirdparty/` folders. **No game data is included.** God Hand is a trademark of Capcom. This project is not affiliated
with Capcom or Clover Studio.
