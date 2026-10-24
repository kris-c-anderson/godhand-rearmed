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
// The camera's own "on screen" test (func_00279F00 and its clones, cGameObj_isClip, IsTargetVisible_14B470: virtual
// slot 0x30, called by cModel_Trans) clips the model's bounding box against the matrix cCamera_update builds from the
// camera's fov and from 0x586B88, the value the camera-width hook sets (a 1.6x margin around the visible screen, on
// the PS2 too). It decides only whether a model is drawn and animated: the enemies' decisions to attack never look at
// the camera (they go by Gene's facing, docs/enemy-ai.md), so neither the aspect ratio nor the field of view changes
// how enemies behave. The rest of the 2D (HUD widgets, menus, button prompts) is narrowed by hud_layout.cpp, which
// also decides whether the font and gauge hooks still narrow (HudLayout=off: only they do, as in the pnach).
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

        // Field of view and framing of the gameplay camera (cPlCamera, s1 in all the hooks below).
        //
        // The camera looks at a point on Gene (+0x200, 1.06 m up) from an orbit around it: pitch +0x410 (radians, >0 =
        // from above), yaw +0x414, distance +0x420 (cPlCamera_SetNormalZoomOutMode: 4 m, 4 degrees; the battle
        // presets at 0x3BD548/0x3BD5F0/0x3BD698 that it eases to while Gene has a live target: 3.08 m and 10 degrees,
        // or 4.38 m and 6 degrees, swung 20-24 degrees over the shoulder). It eases its fov (degrees, +0x250) toward a
        // target every frame, fov = fov*0.9 + target*0.1, at two places in func_00139FF8 (called by
        // cPlCamera_update), and resets it to 31 in cPlCamera_setCamUpdate. +0x51C counts down from 15 while the
        // battle (or another preset) camera is on.
        //
        // The fov hooks replace those three stores so the eased value goes to target*m, where m eases (like the fov)
        // toward FieldOfView outside fights and FightFieldOfView in them: everything the game derives from the fov
        // follows (projection, drawing clip, LOD); cutscene and other cameras keep theirs. The orbit hooks replace the
        // loads of the pitch and the distance where the eye is placed (both update paths and setCamUpdate):
        //   distance * (1 - c + c * tan(fov/m / 2) / tan(fov / 2))   c = CameraDistance / 100: 1 keeps Gene the
        //                                                            same size on screen however wide the view
        //   pitch + CameraTilt * (m - 1) / (FieldOfView - 1)          the camera rises (looking at the same point)
        //                                                            with the extra width, none at m = 1
        float g_fov = 1.0f;    // FieldOfView
        float g_fight = 1.0f;  // FightFieldOfView
        float g_closer = 1.0f; // CameraDistance
        float g_tilt = 0.0f;   // CameraTilt, radians at m = g_fov
        float g_mul = 1.0f;    // m, the multiplier the camera has now

        uint32_t cameraAt(R5900Context *ctx, uint32_t offset) { return (GPR_U32(ctx, 17) + offset) & (PS2_RAM_SIZE - 1u); }

        float loadFloat(const uint8_t *rdram, R5900Context *ctx, uint32_t offset)
        {
            float value;
            std::memcpy(&value, rdram + cameraAt(ctx, offset), sizeof(value));
            return value;
        }

        void storeFov(uint8_t *rdram, R5900Context *ctx, float value)
        {
            std::memcpy(rdram + cameraAt(ctx, 0x250u), &value, sizeof(value));
        }

        // Once per camera update: m eases toward the fight or the exploration value, at the camera's own rate.
        void easeMultiplier(const uint8_t *rdram, R5900Context *ctx)
        {
            int32_t preset;
            std::memcpy(&preset, rdram + cameraAt(ctx, 0x51Cu), sizeof(preset));
            g_mul += ((preset > 0 ? g_fight : g_fov) - g_mul) * 0.1f;
        }

        // 0x13A6E4 swc1 $f1,0x250(s1): $f1 = fov*0.9 + $f20*0.1
        void fovEaseA(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            easeMultiplier(rdram, ctx);
            storeFov(rdram, ctx, ctx->f[1] + 0.1f * (g_mul - 1.0f) * ctx->f[20]);
        }
        // 0x13B368 swc1 $f0,0x250(s1): $f0 = fov*0.9 + $f3*0.1
        void fovEaseB(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            easeMultiplier(rdram, ctx);
            storeFov(rdram, ctx, ctx->f[0] + 0.1f * (g_mul - 1.0f) * ctx->f[3]);
        }
        // 0x13BBB8 swc1 $f0,0x250(s1): $f0 = 31.0, the player camera's starting fov (setCamUpdate also clears +0x51C)
        void fovReset(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            g_mul = g_fov;
            storeFov(rdram, ctx, ctx->f[0] * g_mul);
        }

        float orbitDistance(const uint8_t *rdram, R5900Context *ctx)
        {
            const float distance = loadFloat(rdram, ctx, 0x420u);
            const float fov = loadFloat(rdram, ctx, 0x250u);
            if (g_closer <= 0.0f || !(fov > 1.0f && fov < 170.0f) || std::fabs(g_mul - 1.0f) < 0.0005f)
                return distance;
            constexpr float kHalfRad = 3.14159265f / 360.0f; // degrees to half-angle radians
            const float same = std::tan(fov / g_mul * kHalfRad) / std::tan(fov * kHalfRad);
            return distance * (1.0f - g_closer + g_closer * same);
        }

        float orbitPitch(const uint8_t *rdram, R5900Context *ctx)
        {
            const float pitch = loadFloat(rdram, ctx, 0x410u);
            if (g_tilt == 0.0f || std::fabs(g_fov - 1.0f) < 0.0005f)
                return pitch;
            const float share = std::fmin(std::fmax((g_mul - 1.0f) / (g_fov - 1.0f), 0.0f), 1.0f);
            return pitch + g_tilt * share;
        }

        // lwc1 $f4/$f0,0x410(s1) and lwc1 $f0,0x420(s1) where the eye is placed
        void orbitPitchF4(uint8_t *rdram, R5900Context *ctx, PS2Runtime *) { ctx->f[4] = orbitPitch(rdram, ctx); }  // 0x13A2A8, 0x13BACC
        void orbitPitchF0(uint8_t *rdram, R5900Context *ctx, PS2Runtime *) { ctx->f[0] = orbitPitch(rdram, ctx); }  // 0x13B12C
        void orbitDistanceF0(uint8_t *rdram, R5900Context *ctx, PS2Runtime *) { ctx->f[0] = orbitDistance(rdram, ctx); } // 0x13A2F4, 0x13B15C, 0x13BB18

        float percentEnv(const char *name, float fallback, float low, float high)
        {
            const char *text = std::getenv(name);
            const float value = (text && *text) ? static_cast<float>(std::atof(text)) : fallback;
            return std::fmin(std::fmax(value, low), high);
        }
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
            // [Video] FieldOfView / FightFieldOfView: percent of the game's own (100 = the PS2's), 50..200;
            // CameraDistance: 0..100 (100 = Gene keeps his size on screen); CameraTilt: degrees, -30..30
            g_fov = percentEnv("GODHAND_FOV", 100.0f, 50.0f, 200.0f) / 100.0f;
            g_fight = percentEnv("GODHAND_FIGHT_FOV", 100.0f, 50.0f, 200.0f) / 100.0f;
            g_closer = percentEnv("GODHAND_CAMERA_DISTANCE", 100.0f, 0.0f, 100.0f) / 100.0f;
            g_tilt = percentEnv("GODHAND_CAMERA_TILT", 0.0f, -30.0f, 30.0f) * (3.14159265f / 180.0f);
            g_mul = g_fov;
            ps2xRegisterHostHook("gh_fov_ease_a", &fovEaseA);
            ps2xRegisterHostHook("gh_fov_ease_b", &fovEaseB);
            ps2xRegisterHostHook("gh_fov_reset", &fovReset);
            ps2xRegisterHostHook("gh_cam_pitch_f4", &orbitPitchF4);
            ps2xRegisterHostHook("gh_cam_pitch_f0", &orbitPitchF0);
            ps2xRegisterHostHook("gh_cam_distance", &orbitDistanceF0);
            const bool on = std::fabs(g_fov - 1.0f) > 0.001f || std::fabs(g_fight - 1.0f) > 0.001f;
            ps2xSetPatchFlag("fov", on);
            if (on)
                GH_INFO(Game, "field of view " << g_fov * 100.0f << "% of the game's, " << g_fight * 100.0f
                                               << "% in fights; camera distance " << g_closer * 100.0f << "%, tilt "
                                               << g_tilt * (180.0f / 3.14159265f) << " degrees");
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
