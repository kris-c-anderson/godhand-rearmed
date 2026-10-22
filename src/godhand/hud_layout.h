#pragma once

#include <cstdint>

// Widescreen HUD: the game draws its 2D for a 4:3 screen and the renderer shows the frame wider, so every 2D element
// comes out stretched. This narrows the 2D again where it is drawn (the Gp_draw_* primitives every HUD, menu and text
// quad goes through), each element about its own anchor, so circles stay round and HUD parts stay where they were.
//
// [Video] HudLayout (GODHAND_HUD), F7 cycles it while playing (F6 logs one frame's 2D draws):
//   anchored  HUD widgets on the left third narrowed toward the screen's left edge, on the right third toward the
//             right edge, others toward the centre; small ones (button prompts) about their own centre. Widgets: the game's cIDBase layouts, the player
//             bar, the radar and the roulette menu. Default.
//   centered  all 2D narrowed toward the screen centre (the whole HUD and menus inside a 4:3 area).
//   off       only text and the health gauge narrowed (the PCSX2 16:9 patch); the rest stretched.
// Quads as wide as the screen (backgrounds, fades) are left stretched in every layout.
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
