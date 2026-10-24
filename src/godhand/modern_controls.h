#pragma once

#include <cstdint>

// Control Style (modern_controls.cpp): Classic, or Modern (a free camera on the right stick; the original controls
// while L1 is held or toggled, and, with Auto, while the game's battle camera is on; double-tap L1 = the original
// quick turn). The input side runs on the window thread (a pad filter), the camera side in the camera hooks
// (camera.cpp, game thread); they meet in the atomics behind these calls.
namespace godhand::modern
{
    // [Game] ControlStyle, ModernAuto, ModernL1 (and the camera speeds), read at install; registers the pad filter
    // and the F2 menu's Controls tab.
    void install();

    // Camera side (game thread, once per player camera update; dt in seconds).
    struct CameraState
    {
        bool battle = false;       // the battle (or a boss) preset camera is on (+0x51C > 0)
        bool gameCamera = false;   // the game has taken the camera (a close-up preset, the second update path)
    };
    void noteCamera(const CameraState &state);

    // The free camera: false = the original camera (behind Gene). While free, yaw is the camera's direction around
    // Gene in world radians and pitch the extra pitch to add. Advances the camera by the right stick (dt seconds);
    // gameYaw / gamePitch: where the original camera would be this frame, which the free camera eases back to while
    // the original controls are in effect.
    struct FreeCamera
    {
        bool free = false;
        float yaw = 0.0f;
        float pitch = 0.0f;
    };
    FreeCamera updateCamera(float gameYaw, float dt);
    // setCamUpdate (a cut, a new room): back behind Gene.
    void resetCamera();
}
