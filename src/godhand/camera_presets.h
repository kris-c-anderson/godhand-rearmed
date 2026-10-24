#pragma once

// The camera presets the launcher offers (Display > Camera) and the F2 menu's Camera tab applies. A preset is just
// the four [Video] values; godhand.ini keeps the values, and the launcher shows the preset they match ("Custom"
// otherwise). Tune them live in the F2 menu, then copy its "preset line" here.
namespace godhand
{
    struct CameraPreset
    {
        const char *name;
        float fieldOfView;      // [Video] FieldOfView, percent
        float fightFieldOfView; // [Video] FightFieldOfView, percent
        float cameraDistance;   // [Video] CameraDistance, percent (100 = Gene keeps his size)
        float cameraTilt;       // [Video] CameraTilt, degrees
    };

    inline constexpr CameraPreset kCameraPresets[] = {
        {"As on the PS2", 100.0f, 100.0f, 100.0f, 0.0f},
        {"Wider", 125.0f, 100.0f, 100.0f, 0.0f},
        {"Widest", 150.0f, 110.0f, 100.0f, 5.0f},
    };
}
