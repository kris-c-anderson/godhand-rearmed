// The gameplay camera's field of view and framing, and the enemies' facing cones that follow what it shows.
//
// Camera (cPlCamera, s1 in the camera hooks). It looks at a point on Gene (+0x200, 1.06 m up) from an orbit around it
// (eye +0x210): pitch +0x410 (radians, > 0 = from above), yaw +0x414, distance +0x420 (cPlCamera_SetNormalZoomOutMode:
// 4 m, 4 degrees; the battle presets at 0x3BD548 / 0x3BD5F0 / 0x3BD698 that it eases to while Gene has a live target:
// 3.08 m and 10 degrees, or 4.38 m and 6 degrees, swung 20-24 degrees over the shoulder). It eases its fov (degrees,
// +0x250) toward a target every frame, fov = fov*0.9 + target*0.1, at two places in func_00139FF8 (called by
// cPlCamera_update), and resets it to 31 in cPlCamera_setCamUpdate. +0x51C counts down from 15 while the battle (or
// another preset) camera is on.
//
// The fov hooks replace those three stores so the eased value goes to target*m, where m eases (at the camera's own
// rate) toward FieldOfView outside fights and FightFieldOfView in them: everything the game derives from the fov
// follows (projection, drawing clip, LOD); cutscene and other cameras keep theirs. The orbit hooks replace the loads of
// the pitch and the distance where the eye is placed (both update paths and setCamUpdate):
//   distance * (1 - c + c * tan(fov/m / 2) / tan(fov / 2))   c = CameraDistance / 100: 1 keeps Gene the same size
//   pitch + CameraTilt * (m - 1) / (FieldOfView - 1)          the camera rises (looking at the same point) with the
//                                                            extra width, none at m = 1
// The hooks are always on: at 100% they compute exactly the game's values, and the F2 menu can change the settings
// while playing.
//
// Enemies (docs/enemy-ai.md). They never test the camera; they go by the angle between Gene's facing and the enemy
// (+0x768). With [Game] EnemiesFollowView, the angles they compare it with grow by how much further round Gene the
// picture now reaches: at the enemy's distance r (+0x618 = r squared), the screen's edge passes Gene's facing at
//   edge(r) = phi + asin(D sin(phi) / r)   (180 degrees when D sin(phi) >= r)
// with phi = atan(A tan(fov/2)) (A = the picture's aspect) and D = the camera's distance on the floor. Every angle is
// scaled by edge_now / edge_ps2 (the PS2: the game's own fov and distance, 4:3), so an enemy keeps the place it had
// relative to the screen's edge; at 100% and 4:3 nothing changes. The compares:
//   func_00211C08 0x2126C4, 0x2127D0  c.le.s $f0,$f1: $f0 = 60 degrees (the wedge), $f1 = the angle
//   func_00262AA8 0x263CF4 ... 0x267FC8 (9)  c.lt.s $f0,$f1: $f0 = the angle, $f1 = the level's lunge cone
// The hooks run before the compare and scale the cone's register (every path after it loads both registers again).
#include "godhand/camera.h"
#include "godhand/camera_presets.h"
#include "godhand/ini_file.h"
#include "godhand/log.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h" // GPR_U32
#include "runtime/ps2_game_menu.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#if __has_include("pcsx2gs_api.h")
#include "pcsx2gs_api.h"
#define GODHAND_HAS_GS_MENU 1
#endif

namespace godhand
{
    namespace
    {
        constexpr float kPi = 3.14159265f;
        constexpr float kDegToRad = kPi / 180.0f;
        constexpr float kNativeAspect = 4.0f / 3.0f;

        // Settings: written by the F2 menu (GS thread), read by the hooks (game thread).
        std::atomic<float> g_fovPercent{100.0f};
        std::atomic<float> g_fightPercent{100.0f};
        std::atomic<float> g_closerPercent{100.0f};
        std::atomic<float> g_tiltDegrees{0.0f};
        std::atomic<bool> g_enemyView{true};
        float g_aspect = kNativeAspect;
        std::filesystem::path g_iniPath;

        // The camera as the hooks saw it last (game thread; the menu reads them for its readout).
        float g_mul = 1.0f;                      // m
        std::atomic<float> g_camFov{31.0f};      // the camera's fov now, degrees
        std::atomic<float> g_eyeDistance{4.0f};  // eye to look point on the floor, metres
        std::atomic<float> g_dolly{1.0f};        // our distance / the game's
        std::atomic<bool> g_battle{false};       // +0x51C > 0

        float fovFactor() { return g_fovPercent.load(std::memory_order_relaxed) / 100.0f; }
        float fightFactor() { return g_fightPercent.load(std::memory_order_relaxed) / 100.0f; }

        uint32_t cameraAt(R5900Context *ctx, uint32_t offset) { return (GPR_U32(ctx, 17) + offset) & (PS2_RAM_SIZE - 1u); }

        float loadFloat(const uint8_t *rdram, uint32_t address)
        {
            float value;
            std::memcpy(&value, rdram + (address & (PS2_RAM_SIZE - 1u)), sizeof(value));
            return value;
        }

        void storeFov(uint8_t *rdram, R5900Context *ctx, float value)
        {
            std::memcpy(rdram + cameraAt(ctx, 0x250u), &value, sizeof(value));
        }

        // Once per camera update: m eases toward the fight or the exploration value, and the readout is taken.
        void easeMultiplier(const uint8_t *rdram, R5900Context *ctx)
        {
            int32_t preset;
            std::memcpy(&preset, rdram + cameraAt(ctx, 0x51Cu), sizeof(preset));
            const bool battle = preset > 0;
            g_mul += ((battle ? fightFactor() : fovFactor()) - g_mul) * 0.1f;
            g_battle.store(battle, std::memory_order_relaxed);
            g_camFov.store(loadFloat(rdram, cameraAt(ctx, 0x250u)), std::memory_order_relaxed);
            const float dx = loadFloat(rdram, cameraAt(ctx, 0x210u)) - loadFloat(rdram, cameraAt(ctx, 0x200u));
            const float dz = loadFloat(rdram, cameraAt(ctx, 0x218u)) - loadFloat(rdram, cameraAt(ctx, 0x208u));
            const float floor = std::sqrt(dx * dx + dz * dz);
            if (floor > 0.1f && floor < 50.0f)
                g_eyeDistance.store(floor, std::memory_order_relaxed);
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
            g_mul = fovFactor();
            storeFov(rdram, ctx, ctx->f[0] * g_mul);
        }

        float orbitDistance(const uint8_t *rdram, R5900Context *ctx)
        {
            const float distance = loadFloat(rdram, cameraAt(ctx, 0x420u));
            const float fov = loadFloat(rdram, cameraAt(ctx, 0x250u));
            const float closer = g_closerPercent.load(std::memory_order_relaxed) / 100.0f;
            float dolly = 1.0f;
            if (closer > 0.0f && fov > 1.0f && fov < 170.0f && std::fabs(g_mul - 1.0f) >= 0.0005f)
            {
                const float same = std::tan(fov / g_mul * (kDegToRad * 0.5f)) / std::tan(fov * (kDegToRad * 0.5f));
                dolly = 1.0f - closer + closer * same;
            }
            g_dolly.store(dolly, std::memory_order_relaxed);
            return distance * dolly;
        }

        float orbitPitch(const uint8_t *rdram, R5900Context *ctx)
        {
            const float pitch = loadFloat(rdram, cameraAt(ctx, 0x410u));
            const float tilt = g_tiltDegrees.load(std::memory_order_relaxed) * kDegToRad;
            const float fov = fovFactor();
            if (tilt == 0.0f || std::fabs(fov - 1.0f) < 0.0005f)
                return pitch;
            const float share = std::fmin(std::fmax((g_mul - 1.0f) / (fov - 1.0f), 0.0f), 1.0f);
            return pitch + tilt * share;
        }

        // lwc1 $f4/$f0,0x410(s1) and lwc1 $f0,0x420(s1) where the eye is placed
        void orbitPitchF4(uint8_t *rdram, R5900Context *ctx, PS2Runtime *) { ctx->f[4] = orbitPitch(rdram, ctx); }  // 0x13A2A8, 0x13BACC
        void orbitPitchF0(uint8_t *rdram, R5900Context *ctx, PS2Runtime *) { ctx->f[0] = orbitPitch(rdram, ctx); }  // 0x13B12C
        void orbitDistanceF0(uint8_t *rdram, R5900Context *ctx, PS2Runtime *) { ctx->f[0] = orbitDistance(rdram, ctx); } // 0x13A2F4, 0x13B15C, 0x13BB18

        // ---- Enemies' cones ---------------------------------------------------------------------------------------

        // Angle from Gene's facing (radians) at which the screen's edge passes at distance r: a camera D behind him
        // (on the floor) whose half-width is atan(halfWidth).
        float edgeAngle(float r, float D, float halfWidth)
        {
            const float phi = std::atan(halfWidth);
            const float x = D * std::sin(phi) / r;
            return x >= 1.0f ? kPi : phi + std::asin(x);
        }

        // edge_now / edge_ps2 at distance r (1 when the setting is off).
        float coneScale(float r)
        {
            if (!g_enemyView.load(std::memory_order_relaxed))
                return 1.0f;
            const float fov = g_camFov.load(std::memory_order_relaxed);
            const float mul = g_mul > 0.1f ? g_mul : 1.0f;
            const float dolly = g_dolly.load(std::memory_order_relaxed);
            const float eye = g_eyeDistance.load(std::memory_order_relaxed);
            if (!(fov > 1.0f && fov < 170.0f) || !(dolly > 0.05f))
                return 1.0f;
            r = std::fmax(r, 0.3f);
            const float now = edgeAngle(r, eye, g_aspect * std::tan(fov * (kDegToRad * 0.5f)));
            const float ps2 = edgeAngle(r, eye / dolly, kNativeAspect * std::tan(fov / mul * (kDegToRad * 0.5f)));
            return std::fmin(std::fmax(now / ps2, 0.5f), 3.0f);
        }

        float scaledCone(float cone, float r)
        {
            const float scaled = cone * coneScale(r);
            constexpr float kMax = 150.0f * kDegToRad; // never all the way round behind him
            return cone >= kMax ? cone : std::fmin(scaled, kMax);
        }

        float enemyDistance(const uint8_t *rdram, R5900Context *ctx)
        {
            const float squared = loadFloat(rdram, GPR_U32(ctx, 18) + 0x618u); // s2 = the enemy
            return std::sqrt(std::fmax(squared, 0.0f));
        }

        // Before c.le.s $f0,$f1 in func_00211C08 ($f0 = the 60-degree wedge)
        void enemyWedge(uint8_t *rdram, R5900Context *ctx, PS2Runtime *) { ctx->f[0] = scaledCone(ctx->f[0], enemyDistance(rdram, ctx)); }
        // Before c.lt.s $f0,$f1 in func_00262AA8 ($f1 = the level's lunge cone)
        void enemyLunge(uint8_t *rdram, R5900Context *ctx, PS2Runtime *) { ctx->f[1] = scaledCone(ctx->f[1], enemyDistance(rdram, ctx)); }

        // ---- Settings in godhand.ini ------------------------------------------------------------------------------

        float envValue(const char *name, float fallback, float low, float high)
        {
            const char *text = std::getenv(name);
            const float value = (text && *text) ? static_cast<float>(std::atof(text)) : fallback;
            return std::fmin(std::fmax(value, low), high);
        }

        float iniValue(const IniFile &ini, const char *section, const char *key, float fallback, float low, float high)
        {
            const std::string text = ini.get(section, key, "");
            const float value = text.empty() ? fallback : static_cast<float>(std::atof(text.c_str()));
            return std::fmin(std::fmax(value, low), high);
        }

        std::string number(float value)
        {
            char text[32];
            std::snprintf(text, sizeof(text), "%g", std::round(value * 10.0f) / 10.0f);
            return text;
        }

        std::string presetLine()
        {
            return "{\"Name\", " + number(g_fovPercent.load()) + "f, " + number(g_fightPercent.load()) + "f, " +
                   number(g_closerPercent.load()) + "f, " + number(g_tiltDegrees.load()) + "f},";
        }

        std::mutex g_statusMutex;
        std::string g_status; // the menu's last Save / Revert result

        void setStatus(std::string text)
        {
            std::lock_guard<std::mutex> lock(g_statusMutex);
            g_status = std::move(text);
        }

        void saveToIni()
        {
            IniFile ini;
            if (g_iniPath.empty() || !ini.load(g_iniPath))
            {
                setStatus("Could not read godhand.ini.");
                return;
            }
            ini.set("Video", "FieldOfView", number(g_fovPercent.load()));
            ini.set("Video", "FightFieldOfView", number(g_fightPercent.load()));
            ini.set("Video", "CameraDistance", number(g_closerPercent.load()));
            ini.set("Video", "CameraTilt", number(g_tiltDegrees.load()));
            ini.setBool("Game", "EnemiesFollowView", g_enemyView.load());
            if (!ini.save())
            {
                setStatus("Could not write godhand.ini.");
                return;
            }
            GH_INFO(Game, "camera saved to godhand.ini; as a preset (camera_presets.h): " << presetLine());
            setStatus("Saved to godhand.ini.");
        }

        void revertToIni()
        {
            IniFile ini;
            if (g_iniPath.empty() || !ini.load(g_iniPath))
            {
                setStatus("Could not read godhand.ini.");
                return;
            }
            g_fovPercent = iniValue(ini, "Video", "FieldOfView", 100.0f, 50.0f, 200.0f);
            g_fightPercent = iniValue(ini, "Video", "FightFieldOfView", 100.0f, 50.0f, 200.0f);
            g_closerPercent = iniValue(ini, "Video", "CameraDistance", 100.0f, 0.0f, 100.0f);
            g_tiltDegrees = iniValue(ini, "Video", "CameraTilt", 0.0f, -30.0f, 30.0f);
            g_enemyView = ini.getBool("Game", "EnemiesFollowView", true);
            setStatus("Back to the values in godhand.ini.");
        }

        // ---- F2 menu: the Camera tab ------------------------------------------------------------------------------

#if defined(GODHAND_HAS_GS_MENU)
        void slider(const Pcsx2GsUi *ui, const char *label, std::atomic<float> &setting, float low, float high, const char *format)
        {
            float value = setting.load(std::memory_order_relaxed);
            ui->set_next_width(340.0f);
            if (ui->slider_float(label, &value, low, high, format))
                setting.store(value, std::memory_order_relaxed);
        }

        void drawCameraPanel(const Pcsx2GsUi *ui)
        {
            if (ui->version < 3u)
            {
                ui->text_wrapped("This pcsx2gs.dll is older than the game: build again to get the camera controls.");
                return;
            }
            ui->text_wrapped("Changes apply at once. Turn on \"Keep playing\" above to walk around and fight while you "
                             "tune with the mouse.");
            ui->separator_text("Presets");
            for (const CameraPreset &preset : kCameraPresets)
            {
                if (ui->button(preset.name))
                {
                    g_fovPercent = preset.fieldOfView;
                    g_fightPercent = preset.fightFieldOfView;
                    g_closerPercent = preset.cameraDistance;
                    g_tiltDegrees = preset.cameraTilt;
                }
                ui->same_line();
            }
            ui->text("");

            ui->separator_text("Camera");
            slider(ui, "Field of view##fov", g_fovPercent, 50.0f, 200.0f, "%.0f%%");
            slider(ui, "Field of view in fights##fight", g_fightPercent, 50.0f, 200.0f, "%.0f%%");
            slider(ui, "Camera comes in##closer", g_closerPercent, 0.0f, 100.0f, "%.0f%%");
            ui->text_disabled("0% = stays where it was (the view looks zoomed out); 100% = Gene keeps his size.");
            slider(ui, "Camera tilt##tilt", g_tiltDegrees, -15.0f, 30.0f, "%.1f deg");
            ui->text_disabled("How much the camera rises and looks down at Gene at the field of view above.");

            ui->separator_text("Enemies");
            int follow = g_enemyView.load() ? 1 : 0;
            if (ui->checkbox("Enemies follow the wider view##enemies", &follow))
                g_enemyView = follow != 0;
            ui->text_disabled("Their facing cones widen by as much as the picture reaches further round Gene, so the "
                              "ones you can see at the edges join in as they did at the PS2's edges.");

            ui->separator_text("Now");
            char line[256];
            const float scale = coneScale(3.0f);
            std::snprintf(line, sizeof(line), "%s camera, fov %.1f deg (x%.2f), camera %.2f m from Gene (x%.2f of the game's)",
                          g_battle.load() ? "Battle" : "Exploring", g_camFov.load(), g_mul, g_eyeDistance.load(), g_dolly.load());
            ui->text(line);
            std::snprintf(line, sizeof(line), "Enemies at 3 m: cones x%.2f (the 60 deg wedge is %.0f deg, DIE's 90 deg lunge %.0f deg)",
                          scale, scaledCone(60.0f * kDegToRad, 3.0f) / kDegToRad, scaledCone(90.0f * kDegToRad, 3.0f) / kDegToRad);
            ui->text(line);

            ui->separator_text("godhand.ini");
            if (ui->button("Save"))
                saveToIni();
            ui->same_line();
            if (ui->button("Revert"))
                revertToIni();
            {
                std::lock_guard<std::mutex> lock(g_statusMutex);
                if (!g_status.empty())
                {
                    ui->same_line();
                    ui->text(g_status.c_str());
                }
            }
            const std::string preset = "As a preset: " + presetLine();
            ui->text_disabled(preset.c_str());
        }
#endif
    }

    void setCameraIniPath(const std::filesystem::path &path)
    {
        g_iniPath = path;
    }

    void installCamera(float aspect)
    {
        g_aspect = aspect;
        // [Video] FieldOfView / FightFieldOfView: percent of the game's own (100 = the PS2's), 50..200;
        // CameraDistance: 0..100 (100 = Gene keeps his size on screen); CameraTilt: degrees, -30..30;
        // [Game] EnemiesFollowView: 0/1 (default 1)
        g_fovPercent = envValue("GODHAND_FOV", 100.0f, 50.0f, 200.0f);
        g_fightPercent = envValue("GODHAND_FIGHT_FOV", 100.0f, 50.0f, 200.0f);
        g_closerPercent = envValue("GODHAND_CAMERA_DISTANCE", 100.0f, 0.0f, 100.0f);
        g_tiltDegrees = envValue("GODHAND_CAMERA_TILT", 0.0f, -30.0f, 30.0f);
        {
            const char *text = std::getenv("GODHAND_ENEMY_VIEW");
            g_enemyView = !(text && *text == '0');
        }
        g_mul = fovFactor();
        ps2xRegisterHostHook("gh_fov_ease_a", &fovEaseA);
        ps2xRegisterHostHook("gh_fov_ease_b", &fovEaseB);
        ps2xRegisterHostHook("gh_fov_reset", &fovReset);
        ps2xRegisterHostHook("gh_cam_pitch_f4", &orbitPitchF4);
        ps2xRegisterHostHook("gh_cam_pitch_f0", &orbitPitchF0);
        ps2xRegisterHostHook("gh_cam_distance", &orbitDistanceF0);
        ps2xRegisterHostHook("gh_em_wedge", &enemyWedge);
        ps2xRegisterHostHook("gh_em_lunge", &enemyLunge);
        ps2xSetPatchFlag("fov", true);
        ps2xSetPatchFlag("enemy_view", true);
        GH_INFO(Game, "camera: field of view " << g_fovPercent.load() << "%, " << g_fightPercent.load()
                                               << "% in fights; comes in " << g_closerPercent.load() << "%, tilt "
                                               << g_tiltDegrees.load() << " degrees; enemies follow the view: "
                                               << (g_enemyView.load() ? "on" : "off"));
#if defined(GODHAND_HAS_GS_MENU)
        static bool s_panel = false;
        if (!s_panel)
        {
            s_panel = true;
            ps2x::menu::setTitle("God Hand: Rearmed", "F2 closes this menu. The game keeps running behind it.");
            ps2x::menu::addPanel("Camera", &drawCameraPanel);
        }
#endif
    }
}
