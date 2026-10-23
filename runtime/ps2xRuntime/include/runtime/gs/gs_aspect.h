#pragma once

// The aspect ratio of the picture while the game runs (PCSX2's GS only; nothing with the built-in GS).
// Its own header so that changing it does not rebuild the generated code.
namespace ps2x::gs
{
    enum class Aspect
    {
        R4_3,     // the game's shape (its custom widescreen aspect when it set one)
        R16_9,
        Stretch,  // fill the window
        Force4_3, // 4:3 even when the game set a widescreen aspect (menus drawn for 4:3: pillarboxed)
    };

    void setAspect(Aspect aspect);
}
