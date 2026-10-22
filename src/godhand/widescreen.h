#pragma once

#include "ps2_runtime.h"

namespace godhand
{
    // [Video] AspectRatio (GODHAND_ASPECT): the picture's shape. Before runtime.initialize (the window takes this
    // shape) and before the guest starts (the camera and HUD constants are switched in the game code).
    void installWidescreen();
}
