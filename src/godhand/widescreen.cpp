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
// computed float.
//
// Enemies "on screen": the enemy AI's visibility checks (func_00279F00 and its clones, cGameObj_isClip,
// IsTargetVisible_14B470) clip the model's bounding box against the current camera's matrix at +0x80, which
// cCamera_update builds every frame from the camera's fov and from 0x586B88, the value the camera-width hook sets
// (scrz = 448 / 0x586B88 / 2 / tan(fov/2); clip |x/z| <= 512/scrz, the visible screen is |x/z| <= 320/scrz: a 1.6x
// margin on the PS2 too). So the test widens with the picture and follows the FieldOfView option; nothing else
// decides it. The rest of the 2D (HUD widgets, menus, button prompts) is narrowed by hud_layout.cpp, which also
// decides whether the font and gauge hooks still narrow (HudLayout=off: only they do, as in the pnach).
#include "godhand/widescreen.h"
#include "godhand/hud_layout.h"
#include "godhand/log.h"
#include "ps2_runtime_macros.h" // SET_GPR_U32, GPR_U32
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

        // Field of view: the player camera (cPlCamera) eases its fov (degrees, cCamera +0x250) toward a target every
        // frame, fov = fov*0.9 + target*0.1, at two places in func_00139FF8 (called by cPlCamera_update), and resets
        // it to 31 in cPlCamera_setCamUpdate. These hooks replace those three stores so the eased value goes to
        // target*k instead: everything the game derives from the camera's fov then follows (the projection, the
        // drawing clip matrix, the enemies' "on screen" test, LOD), and cutscene and other cameras keep theirs.
        float g_fov = 1.0f;

        void storeFov(uint8_t *rdram, R5900Context *ctx, float value)
        {
            const uint32_t address = (GPR_U32(ctx, 17) + 0x250u) & (PS2_RAM_SIZE - 1u); // s1 = the camera
            std::memcpy(rdram + address, &value, sizeof(value));
        }
        // 0x13A6E4 swc1 $f1,0x250(s1): $f1 = fov*0.9 + $f20*0.1
        void fovEaseA(uint8_t *rdram, R5900Context *ctx, PS2Runtime *) { storeFov(rdram, ctx, ctx->f[1] + 0.1f * (g_fov - 1.0f) * ctx->f[20]); }
        // 0x13B368 swc1 $f0,0x250(s1): $f0 = fov*0.9 + $f3*0.1
        void fovEaseB(uint8_t *rdram, R5900Context *ctx, PS2Runtime *) { storeFov(rdram, ctx, ctx->f[0] + 0.1f * (g_fov - 1.0f) * ctx->f[3]); }
        // 0x13BBB8 swc1 $f0,0x250(s1): $f0 = 31.0, the player camera's starting fov
        void fovReset(uint8_t *rdram, R5900Context *ctx, PS2Runtime *) { storeFov(rdram, ctx, ctx->f[0] * g_fov); }
        void fontWidth(uint8_t *, R5900Context *ctx, PS2Runtime *) { setAt(ctx, hud::narrowFont() ? g_font : kFontWidth); }
        void gaugeWidth(uint8_t *, R5900Context *ctx, PS2Runtime *) { setAt(ctx, hud::narrowGauge() ? g_gauge : 0x3F800000u); }
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
        {
            // [Video] FieldOfView: percent of the game's own (100 = the PS2's), 50..200
            const char *fov = std::getenv("GODHAND_FOV");
            const float percent = (fov && *fov) ? static_cast<float>(std::atof(fov)) : 100.0f;
            g_fov = std::fmin(std::fmax(percent > 0.0f ? percent : 100.0f, 50.0f), 200.0f) / 100.0f;
            ps2xRegisterHostHook("gh_fov_ease_a", &fovEaseA);
            ps2xRegisterHostHook("gh_fov_ease_b", &fovEaseB);
            ps2xRegisterHostHook("gh_fov_reset", &fovReset);
            ps2xSetPatchFlag("fov", std::fabs(g_fov - 1.0f) > 0.001f);
            GH_INFO(Game, "field of view " << g_fov * 100.0f << "% of the game's (player camera)");
        }
        ps2xRegisterHostHook("gh_ws_font_width", &fontWidth);
        ps2xRegisterHostHook("gh_ws_gauge_width", &gaugeWidth);
        ps2xSetPatchFlag("widescreen", wide);
        hud::install(wide, 1.0f / wider);
        ps2_host_display::setAspect(aspect);
        // PCSX2's custom aspect: ours unless the environment set it (then it is left alone). Called again after the
        // launcher, the value is replaced (4:3 included, so a launcher change back to 4:3 takes effect).
        static bool s_ownAspect = std::getenv("PS2X_PCSX2_ASPECT") == nullptr;
        if (s_ownAspect)
        {
            char text[32];
            std::snprintf(text, sizeof(text), "%.6f", wide ? aspect : 0.0f);
#if defined(_WIN32)
            _putenv_s("PS2X_PCSX2_ASPECT", wide ? text : "");
#else
            setenv("PS2X_PCSX2_ASPECT", wide ? text : "", 1);
#endif
        }
        GH_INFO(Game, "aspect ratio " << aspect << (wide ? " (widescreen: camera, font and gauge widths adjusted)" : " (the PS2's 4:3)"));
    }
}
