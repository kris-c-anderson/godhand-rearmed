#pragma once

#include <cstdint>

// Widescreen HUD: the game draws its 2D for a 4:3 screen and the renderer shows the frame wider, so every 2D element
// comes out stretched. This narrows the 2D again where it is drawn (the Gp_draw_* primitives every HUD, menu and text
// quad goes through), each element about its own anchor, so circles stay round and HUD parts stay where they were.
//
// [Video] HudLayout (GODHAND_HUD), F7 switches it while playing (F6 logs one frame's 2D draws). Menus and screens are
// always drawn as on the PS2, in the middle 4:3; the layout is where the in-game HUD widgets (player bar, radar,
// roulette, boss bars, items) go:
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

    // For the font and gauge width hooks of widescreen.cpp: whether those draws still need narrowing there.
    bool narrowFont();
    bool narrowGauge();
}
