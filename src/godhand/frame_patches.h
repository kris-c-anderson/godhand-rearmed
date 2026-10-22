#pragma once

#include "ps2_runtime.h"

namespace godhand
{
    // The 60 FPS mode and the L3 dash ([Game] Fps60 / L3Dash in godhand.ini; GODHAND_FPS60 / GODHAND_L3_DASH), and
    // the per-frame hook at the top of God Hand's main loop (also counts game frames for the F3 overlay). Before
    // runtime.run().
    void installFramePatches(PS2Runtime &runtime);
}
