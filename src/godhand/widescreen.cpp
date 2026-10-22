// Any aspect ratio: a port of the PCSX2 widescreen patch for God Hand (SLUS-21503_6FB69282.pnach "Widescreen 16:9", by
// nemesis2000 and Arapapa, PCSX2/pcsx2_patches), with the constants computed for the chosen ratio A instead of 16:9.
// The game still renders its 4:3 frame; the renderer shows it A wide, so 3D gets a wider camera and the 2D that would
// be stretched is drawn narrower:
//
//   pnach (16:9)                                   here (A = width / height; 4:3 = 1.3333)
//   30B8F0/4 lui/ori at = 1.2444 (was 0.9333)      camera horizontal view: 0.93333 * A / (4/3)    (hook on the ori)
//   2AF4F8/C lui/ori at = 0.675  (was 0.9)         font width:             0.9     * (4/3) / A    (hook on the ori)
//   14300C   lui at = 0.75       (was 1.0)         health gauge width:     1.0     * (4/3) / A    (hook on the lui)
//   gsaspectratio=16:9                             PS2X_PCSX2_ASPECT = A (PCSX2's custom aspect ratio)
//
// recomp/godhand.toml has the three hooks (flag "widescreen"); they replace the instruction and set $at to the
// computed float. Other 2D (menus, some HUD parts, movies) is not adjusted yet and shows stretched.
#include "godhand/widescreen.h"
#include "godhand/log.h"
#include "ps2_runtime_macros.h" // SET_GPR_U32
#include "runtime/ps2_display_options.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace godhand
{
    namespace
    {
        constexpr float kNative = 4.0f / 3.0f;
        constexpr uint32_t kCameraWidth = 0x3F6EEEEFu; // 0.93333 (0x30B8F0)
        constexpr uint32_t kFontWidth = 0x3F666666u;   // 0.9 (0x2AF4F8)
        float g_aspect = kNative;
        uint32_t g_camera = kCameraWidth, g_font = kFontWidth, g_gauge = 0x3F800000u;

        float asFloat(uint32_t bits)
        {
            float value;
            std::memcpy(&value, &bits, sizeof(value));
            return value;
        }

        uint32_t asBits(float value)
        {
            uint32_t bits;
            std::memcpy(&bits, &value, sizeof(bits));
            return bits;
        }

        // "16:9", "21:9", "1.7778", "auto" (the primary monitor's shape), "4:3" / empty = the PS2's.
        float parseAspect(const char *text)
        {
            if (!text || !*text)
                return kNative;
            const std::string value(text);
            if (value == "auto")
            {
#if defined(_WIN32)
                const int width = GetSystemMetrics(SM_CXSCREEN), height = GetSystemMetrics(SM_CYSCREEN);
                if (width > 0 && height > 0)
                    return static_cast<float>(width) / static_cast<float>(height);
#endif
                return kNative;
            }
            float w = 0.0f, h = 0.0f;
            if (std::sscanf(value.c_str(), "%f:%f", &w, &h) == 2 && w > 0.0f && h > 0.0f)
                return w / h;
            const float ratio = static_cast<float>(std::atof(value.c_str()));
            return ratio > 0.0f ? ratio : kNative;
        }

        void setAt(R5900Context *ctx, uint32_t bits)
        {
            SET_GPR_U32(ctx, 1, bits);
        }

        void cameraWidth(uint8_t *, R5900Context *ctx, PS2Runtime *) { setAt(ctx, g_camera); }
        void fontWidth(uint8_t *, R5900Context *ctx, PS2Runtime *) { setAt(ctx, g_font); }
        void gaugeWidth(uint8_t *, R5900Context *ctx, PS2Runtime *) { setAt(ctx, g_gauge); }
    }

    void installWidescreen()
    {
        float aspect = parseAspect(std::getenv("GODHAND_ASPECT"));
        aspect = std::fmin(std::fmax(aspect, 1.0f), 4.0f);
        g_aspect = aspect;
        const bool wide = std::fabs(aspect - kNative) > 0.001f;
        const float wider = aspect / kNative;
        g_camera = asBits(asFloat(kCameraWidth) * wider);
        g_font = asBits(asFloat(kFontWidth) / wider);
        g_gauge = asBits(1.0f / wider);
        ps2xRegisterHostHook("gh_ws_camera_width", &cameraWidth);
        ps2xRegisterHostHook("gh_ws_font_width", &fontWidth);
        ps2xRegisterHostHook("gh_ws_gauge_width", &gaugeWidth);
        ps2xSetPatchFlag("widescreen", wide);
        ps2_host_display::setAspect(aspect);
        if (wide && !std::getenv("PS2X_PCSX2_ASPECT"))
        {
            char text[32];
            std::snprintf(text, sizeof(text), "%.6f", aspect);
#if defined(_WIN32)
            _putenv_s("PS2X_PCSX2_ASPECT", text);
#else
            setenv("PS2X_PCSX2_ASPECT", text, 0);
#endif
        }
        GH_INFO(Game, "aspect ratio " << aspect << (wide ? " (widescreen: camera, font and gauge widths adjusted)" : " (the PS2's 4:3)"));
    }
}
