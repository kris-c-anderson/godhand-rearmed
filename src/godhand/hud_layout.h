#pragma once

#include <cstdint>

// Widescreen HUD: the game draws its 2D for a 4:3 screen and the renderer shows the frame wider, so every 2D element
// comes out stretched. This narrows the 2D again where it is drawn (the Gp_draw_* primitives every HUD, menu and text
// quad goes through), each element about its own anchor, so circles stay round and HUD parts stay where they were.
//
// Menus and screens (a full-screen background drawn by a non-HUD layout) are shown 4:3 with side bars, nothing
// narrowed ([Video] MenuLayout=4:3, GODHAND_MENUS), or stretched (MenuLayout=stretch). Other 2D in gameplay (text
// windows) is narrowed toward the centre.
// [Video] HudLayout (GODHAND_HUD), F7 switches it while playing (F6 logs one frame's 2D draws): where the in-game HUD
// widgets (player bar, radar, roulette, boss bars, items) go:
//   16:9  at the screen edges (default). Wider screens keep them at the edges of a 16:9 area in the middle;
//         narrower than 16:9 places them as the 4:3 layout does.
//   4:3   where the PS2 had them (the middle 4:3 of the screen).
// Button prompts placed on things in the 3D view keep their place in both. Full-width quads (backgrounds, fades)
// stay stretched.
namespace godhand::hud
{
    // Before the guest starts; wide = the aspect ratio is not 4:3 (then the hooks run), k = (4/3) / aspect.
    void install(bool wide, float k);

    // Top of every game frame.
    void onFrame(uint8_t *rdram);

    // A menu or screen is up (a full-screen background in a non-HUD layout in the last few frames). Tracked at every
    // aspect ratio (the menu d-pad support uses it).
    bool menuActive();

    // For the font and gauge width hooks of widescreen.cpp: whether those draws still need narrowing there.
    bool narrowFont();
    bool narrowGauge();
}
