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
#include "godhand/modern_controls.h"
#include "godhand/log.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h" // GPR_U32
#include "runtime/gs/gs_pcsx2_backend.h"
#include "runtime/ps2_game_menu.h"

#include <atomic>
#include <chrono>
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
        std::atomic<bool> g_depthOfField{true}; // [Video] DepthOfField (PS2X_PCSX2_DOF), the Graphics tab
        std::atomic<bool> g_preciseDepth{true}; // [Video] PreciseDepth (PS2X_PCSX2_PRECISE_DEPTH), the Graphics tab
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

        float g_lastDistance = 4.0f; // the eye's distance from the look point last update (orbitDistance)

        // Depth range. cCamera_update builds the projection with sceVu0ViewScreenMatrix (func_0030AA40, called at
        // 0x138864): zmin 1, zmax 2031584 / fov (65535 at the player camera's 31 degrees), near 0.1, far 600, into a
        // 16-bit depth buffer (Z16S): depth falls as 1/distance, coarse in the distance (the renderer makes it precise
        // again from Q: pcsx2gs, GSRendererHW::ApplyPreciseDepth). A wider fov shrinks zmax with it, and that is left
        // alone: every projection of the frame has to share one depth scale. Two fixes were tried and undone:
        // - the near plane moved out: the game culls what comes nearer than it, so walls near the camera vanished;
        // - zmax put back for the player camera (a hook before 0x13884C): the stage is drawn with its own projection,
        //   built the same way from the camera's fov (zmax 2031584 / fov at 0x2A3A88, also 0x29E9A8 and Fil_clut_set),
        //   so it kept the widened fov's smaller range while the models had the PS2's, and walls lost the depth test to
        //   models behind them (dump gif_11967: Z = 4285 Q for a wall, 6569 Q for the cabinet behind it).
        // Some passes use a fixed zmax of 65535 (func_0014C7E8, func_0014D0E8, trails, most 2D): at a wider fov they sit
        // a little nearer in depth than the scene, as they already did at the game's own wider battle fovs.

        // Once per camera update: m eases toward the fight or the exploration value, and the readout is taken.
        // The camera preset the battle camera eases to (D_003BD540, an index into 0x3BD548/0x3BD5F0/0x3BD698): 0, 1
        // the usual battle framings; 2, 5, 6, 7, 10, 11, 12 bosses'; 3, 4, 8, 9, 13 the close-ups of grabs and
        // special moves (the game's own moment: the Modern controls step aside).
        constexpr uint32_t kCameraPreset = 0x3BD540u;

        // The player camera (cPlCamera, s1 in its own hooks; a0 / s4 in cCamera_update), when it was last updated by its
        // own logic, and whether the game has taken it (a close-up preset, the second update path): the ease back and
        // the see-through oval below work from these.
        std::atomic<uint32_t> g_playerCamera{0};
        std::atomic<int64_t> g_playerCameraMs{0};
        std::atomic<bool> g_gameTookCamera{false};
        std::atomic<bool> g_cameraCut{false}; // setCamUpdate (a cut): no ease back across it

        int64_t steadyMs()
        {
            using namespace std::chrono;
            return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
        }

        void easeMultiplier(const uint8_t *rdram, R5900Context *ctx, bool secondPath)
        {
            int32_t preset;
            std::memcpy(&preset, rdram + cameraAt(ctx, 0x51Cu), sizeof(preset));
            const bool battle = preset > 0;
            int32_t mode;
            std::memcpy(&mode, rdram + kCameraPreset, sizeof(mode));
            const bool closeUp = battle && (mode == 3 || mode == 4 || mode == 8 || mode == 9 || mode == 13);
            modern::noteCamera(rdram, battle, secondPath || closeUp);
            g_playerCamera.store(GPR_U32(ctx, 17), std::memory_order_relaxed);
            g_playerCameraMs.store(steadyMs(), std::memory_order_relaxed);
            g_gameTookCamera.store(secondPath || closeUp, std::memory_order_relaxed);
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
            easeMultiplier(rdram, ctx, false);
            storeFov(rdram, ctx, ctx->f[1] + 0.1f * (g_mul - 1.0f) * ctx->f[20]);
        }
        // 0x13B368 swc1 $f0,0x250(s1): $f0 = fov*0.9 + $f3*0.1
        void fovEaseB(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            easeMultiplier(rdram, ctx, true);
            storeFov(rdram, ctx, ctx->f[0] + 0.1f * (g_mul - 1.0f) * ctx->f[3]);
        }
        // 0x13BBB8 swc1 $f0,0x250(s1): $f0 = 31.0, the player camera's starting fov (setCamUpdate also clears +0x51C)
        void fovReset(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            g_mul = fovFactor();
            modern::resetCamera();
            g_cameraCut.store(true, std::memory_order_relaxed);
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
            g_lastDistance = distance * dolly;
            return distance * dolly;
        }

        float g_freePitch = 0.0f; // the free camera's extra pitch (Modern controls), from the last yaw hook
        bool g_freeNow = false;   // the free camera has the view (the player turns it: never eased)

        // 0x13A2B8 lwc1 $f2,0x414(s1): the camera's yaw offset from Gene's facing ($f1 = his yaw, loaded just before;
        // the two are added at 0x13A2C4). With the Modern controls' free camera, the offset that puts the camera at
        // the free camera's own yaw instead.
        void orbitYaw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            static auto s_last = std::chrono::steady_clock::now();
            const auto now = std::chrono::steady_clock::now();
            const float dt = std::chrono::duration<float>(now - s_last).count();
            s_last = now;
            const float offset = loadFloat(rdram, cameraAt(ctx, 0x414u));
            const modern::FreeCamera free = modern::updateCamera(rdram, ctx->f[1] + offset, ctx->f[1], dt);
            g_freePitch = free.free ? free.pitch : 0.0f;
            g_freeNow = free.free;
            if (!free.free)
            {
                ctx->f[2] = offset;
                return;
            }
            float relative = free.yaw - ctx->f[1];
            while (relative > kPi)
                relative -= 2.0f * kPi;
            while (relative < -kPi)
                relative += 2.0f * kPi;
            ctx->f[2] = relative;
        }

        float orbitPitch(const uint8_t *rdram, R5900Context *ctx)
        {
            float pitch = loadFloat(rdram, cameraAt(ctx, 0x410u));
            const float tilt = g_tiltDegrees.load(std::memory_order_relaxed) * kDegToRad;
            const float fov = fovFactor();
            if (tilt != 0.0f && std::fabs(fov - 1.0f) >= 0.0005f)
                pitch += tilt * std::fmin(std::fmax((g_mul - 1.0f) / (fov - 1.0f), 0.0f), 1.0f);
            // The free camera's pitch, kept so the eye stays 0.35 m above Gene's feet: the look point is the look
            // offset's height (+0x404) plus +0x510 above them, the eye that plus distance * sin(pitch).
            constexpr float kClearance = 0.35f;
            const float lookHeight = loadFloat(rdram, cameraAt(ctx, 0x404u)) + loadFloat(rdram, cameraAt(ctx, 0x510u));
            const float lowest = std::asin(std::fmin(std::fmax((kClearance - lookHeight) / std::fmax(g_lastDistance, 0.5f), -1.0f), 1.0f));
            modern::setPitchFloor(lowest - pitch);
            return pitch + std::fmax(g_freePitch, std::fmin(lowest - pitch, 0.0f));
        }

        // lwc1 $f4/$f0,0x410(s1) and lwc1 $f0,0x420(s1) where the eye is placed
        void orbitPitchF4(uint8_t *rdram, R5900Context *ctx, PS2Runtime *) { ctx->f[4] = orbitPitch(rdram, ctx); }  // 0x13A2A8, 0x13BACC
        void orbitPitchF0(uint8_t *rdram, R5900Context *ctx, PS2Runtime *) { ctx->f[0] = orbitPitch(rdram, ctx); }  // 0x13B12C
        void orbitDistanceF0(uint8_t *rdram, R5900Context *ctx, PS2Runtime *) { ctx->f[0] = orbitDistance(rdram, ctx); } // 0x13A2F4, 0x13B15C, 0x13BB18

        // ---- Ease back after the game's own camera moves ------------------------------------------------------------
        //
        // A grab, a special move or a scripted moment takes the camera; when it gives it back the camera cuts to
        // wherever the gameplay camera is. With [Video] CameraEaseBack, a jump of the player camera's view (other than
        // into a close-up preset or the second update path, and other than the Modern free camera turned by the
        // player) starts a glide instead (CameraEaseBackSeconds, 0.5 s by default): from the last view drawn to the camera's own view of each frame, the look point moving straight
        // and the eye round it (direction and distance eased apart). Written over the camera's look point (+0x200) and
        // eye (+0x210) before cCamera_update builds the frame's matrices (0x138744, a0 = the camera), so everything
        // drawn from the camera follows; the camera's logic computes both afresh every frame. A shot with a camera of
        // its own (D_005CAFF0 another camera) counts as the game taking it: the glide then starts from that camera's
        // last view. A cut (setCamUpdate: a cutscene, a new room) never eases.
        std::atomic<bool> g_easeBackOn{true};
        std::atomic<float> g_easeBackSeconds{0.5f};

        struct Pose
        {
            float look[3];
            float eye[3];
        };

        Pose readPose(const uint8_t *rdram, uint32_t camera)
        {
            Pose pose;
            for (int i = 0; i < 3; ++i)
            {
                pose.look[i] = loadFloat(rdram, camera + 0x200u + 4u * i);
                pose.eye[i] = loadFloat(rdram, camera + 0x210u + 4u * i);
            }
            return pose;
        }

        void writePose(uint8_t *rdram, uint32_t camera, const Pose &pose)
        {
            for (int i = 0; i < 3; ++i)
            {
                std::memcpy(rdram + ((camera + 0x200u + 4u * i) & (PS2_RAM_SIZE - 1u)), &pose.look[i], sizeof(float));
                std::memcpy(rdram + ((camera + 0x210u + 4u * i) & (PS2_RAM_SIZE - 1u)), &pose.eye[i], sizeof(float));
            }
        }

        float length3(const float *v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

        // A jump between two frames' views: the look point or the eye moved further than a camera glides in a frame,
        // or the direction from the look point to the eye turned by more than 10 degrees.
        bool jumped(const Pose &a, const Pose &b)
        {
            float dl[3], de[3], oa[3], ob[3];
            for (int i = 0; i < 3; ++i)
            {
                dl[i] = b.look[i] - a.look[i];
                de[i] = b.eye[i] - a.eye[i];
                oa[i] = a.eye[i] - a.look[i];
                ob[i] = b.eye[i] - b.look[i];
            }
            if (length3(dl) > 0.3f || length3(de) > 0.4f)
                return true;
            const float la = length3(oa), lb = length3(ob);
            if (la < 0.01f || lb < 0.01f)
                return false;
            const float cosine = (oa[0] * ob[0] + oa[1] * ob[1] + oa[2] * ob[2]) / (la * lb);
            return cosine < std::cos(10.0f * kDegToRad);
        }

        Pose blendPose(const Pose &from, const Pose &to, float s)
        {
            Pose out;
            float of[3], ot[3], dir[3];
            for (int i = 0; i < 3; ++i)
            {
                out.look[i] = from.look[i] + (to.look[i] - from.look[i]) * s;
                of[i] = from.eye[i] - from.look[i];
                ot[i] = to.eye[i] - to.look[i];
            }
            const float lf = length3(of), lt = length3(ot);
            const float dot = (lf > 0.01f && lt > 0.01f) ? (of[0] * ot[0] + of[1] * ot[1] + of[2] * ot[2]) / (lf * lt) : -1.0f;
            if (dot < -0.9f)
            {
                // Nearly opposite sides: straight across (a turn would have no one way round).
                for (int i = 0; i < 3; ++i)
                    out.eye[i] = from.eye[i] + (to.eye[i] - from.eye[i]) * s;
                return out;
            }
            for (int i = 0; i < 3; ++i)
                dir[i] = of[i] / lf + (ot[i] / lt - of[i] / lf) * s;
            const float ld = std::fmax(length3(dir), 1e-4f), distance = lf + (lt - lf) * s;
            for (int i = 0; i < 3; ++i)
                out.eye[i] = out.look[i] + dir[i] / ld * distance;
            return out;
        }

        // Game thread only.
        Pose g_drawnPose{};          // the view the last frame was drawn from
        bool g_haveDrawn = false;
        bool g_wasTaken = false;
        Pose g_easeFrom{};
        float g_easeTime = -1.0f;    // < 0: not easing
        float g_easeLength = 0.5f;
        int64_t g_lastUpdateMs = 0;

        int64_t g_otherCameraMs = -100000; // another camera was the one drawn from (D_005CAFF0), last at this time
        constexpr uint32_t kCurrentCamera = 0x5CAFF0u; // D_005CAFF0: the camera the frame is drawn from

        // 0x138744 (cCamera_update, sq s0,192(sp); a0 = the camera)
        void cameraUpdateStart(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t camera = GPR_U32(ctx, 4);
            if (camera < 0x100000u || camera >= PS2_RAM_SIZE - 0x600u)
                return;
            const int64_t now = steadyMs();
            if (camera != g_playerCamera.load(std::memory_order_relaxed))
            {
                // Another camera drawing the frame (a shot with a camera of its own): its view is the one to ease from
                // when the player camera takes over again.
                uint32_t current;
                std::memcpy(&current, rdram + kCurrentCamera, sizeof(current));
                if (current == camera)
                {
                    g_drawnPose = readPose(rdram, camera);
                    g_haveDrawn = true;
                    g_otherCameraMs = now;
                }
                return;
            }
            const float dt = std::fmin(std::fmax(static_cast<float>(now - g_lastUpdateMs) / 1000.0f, 0.0f), 0.1f);
            g_lastUpdateMs = now;
            const Pose game = readPose(rdram, camera);
            const bool taken = g_gameTookCamera.load(std::memory_order_relaxed);
            if (g_cameraCut.exchange(false, std::memory_order_relaxed) || !g_easeBackOn.load(std::memory_order_relaxed))
            {
                g_easeTime = -1.0f;
                g_wasTaken = taken;
                g_drawnPose = game;
                g_haveDrawn = true;
                return;
            }
            if (taken)
                g_easeTime = -1.0f; // into a close-up as the game cuts to it
            const bool other = now - g_otherCameraMs < 300;

            // Any jump of the camera's view, except into a close-up and while the free camera is turned by the
            // player: the game's own cuts back (after grabs, special moves, shots of their own) glide. Each jump is
            // logged with what the game was doing, so one that should have eased can be traced.
            if (g_haveDrawn && jumped(g_drawnPose, game))
            {
                int32_t preset = 0, mode = 0;
                std::memcpy(&preset, rdram + ((camera + 0x51Cu) & (PS2_RAM_SIZE - 1u)), sizeof(preset));
                std::memcpy(&mode, rdram + kCameraPreset, sizeof(mode));
                const bool ease = g_easeTime < 0.0f && !taken && !g_freeNow;
                GH_TRACE(Game, "camera: jump (" << (ease ? "easing back" : "not eased") << "): taken " << taken
                                                << " (before " << g_wasTaken << "), other camera " << other
                                                << ", free " << g_freeNow << ", battle timer " << preset << ", preset "
                                                << mode << ", eye moved "
                                                << std::hypot(game.eye[0] - g_drawnPose.eye[0], game.eye[1] - g_drawnPose.eye[1],
                                                              game.eye[2] - g_drawnPose.eye[2]) << " m");
                if (ease)
                {
                    g_easeFrom = g_drawnPose;
                    g_easeTime = 0.0f;
                    g_easeLength = std::fmax(g_easeBackSeconds.load(std::memory_order_relaxed), 0.05f);
                }
            }
            g_wasTaken = taken;
            Pose drawn = game;
            if (g_easeTime >= 0.0f)
            {
                g_easeTime += dt;
                const float x = std::fmin(g_easeTime / g_easeLength, 1.0f);
                const float s = x * x * (3.0f - 2.0f * x);
                if (x >= 1.0f)
                    g_easeTime = -1.0f;
                else
                {
                    drawn = blendPose(g_easeFrom, game, s);
                    writePose(rdram, camera, drawn);
                }
            }
            g_drawnPose = drawn;
            g_haveDrawn = true;
        }

        // ---- See-through oval -----------------------------------------------------------------------------------------
        //
        // Things between the camera and Gene (walls, pillars, the stalls in town) dissolve in an oval round him, so he
        // and the enemies about him stay in view. Every frame, once cCamera_update has the player camera's world-to-screen
        // matrix (+0x00 = view-screen * view, 0x1388F0, s4 = the camera), Gene's middle (0.95 m above his feet, +0xF0 of
        // the player) is projected to the screen; the oval's radii are SeeThroughWidth / SeeThroughHeight metres at his
        // distance, and two points in front of him toward the eye give the depths where dissolving starts
        // (SeeThroughMargin) and where it is full (0.8 m further). The renderer dissolves the depth-tested triangles
        // nearer than that inside the oval (pcsx2gs: pcsx2gs_set_option "see_through", GSRendererHW::ApplySeeThrough,
        // tfx.fx SeeThroughHides: an ordered dither, densest in the middle, thinning over the soft edge). Gene, the
        // enemies round him and the floor at his feet are at his depth or behind it; the HUD has no depth test.
        // Only while the player camera's own logic runs (gameplay: not in cutscenes); the DLL turns the oval off by
        // itself when no frame brings one.
        constexpr uint32_t kPlayerPointer = 0x747A94u;
        std::atomic<bool> g_seeThrough{true};
        std::atomic<float> g_seeWidth{1.6f};     // metres, half-width
        std::atomic<float> g_seeHeight{1.3f};    // metres, half-height
        std::atomic<float> g_seeMargin{1.0f};    // metres in front of Gene
        std::atomic<float> g_seeStrength{100.0f}; // percent
        std::atomic<float> g_seeSoftness{40.0f};  // percent of the radius
        bool g_seeSent = false;                   // game thread: an oval went to the renderer last frame
        std::atomic<float> g_seeReadout[4] = {};  // centre x, y, radii x, y (GS pixels), for the menu

        struct Clip
        {
            float x, y, z, w;
        };

        Clip project(const float *m, const float *p)
        {
            Clip c;
            c.x = m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12];
            c.y = m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13];
            c.z = m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14];
            c.w = m[3] * p[0] + m[7] * p[1] + m[11] * p[2] + m[15];
            return c;
        }

        void seeThroughOff()
        {
            if (g_seeSent)
                GSPcsx2Backend::SetOption("see_through", "0");
            g_seeSent = false;
        }

        // 0x1388F0 (cCamera_update, lui at,0x4233 after MtxMultiply; s4 = the camera)
        void cameraMatrixReady(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t camera = GPR_U32(ctx, 20);
            if (camera != g_playerCamera.load(std::memory_order_relaxed))
                return;
            const auto valid = [](uint32_t p) { return p >= 0x100000u && p < PS2_RAM_SIZE - 0x1000u; };
            if (!g_seeThrough.load(std::memory_order_relaxed) || !valid(camera) ||
                steadyMs() - g_playerCameraMs.load(std::memory_order_relaxed) > 100)
            {
                seeThroughOff();
                return;
            }
            uint32_t player, at;
            std::memcpy(&player, rdram + kPlayerPointer, sizeof(player));
            if (!valid(player))
            {
                seeThroughOff();
                return;
            }
            std::memcpy(&at, rdram + player + 0xF0u, sizeof(at));
            if (!valid(at))
            {
                seeThroughOff();
                return;
            }
            float m[16];
            std::memcpy(m, rdram + camera, sizeof(m));
            const float centre[3] = {loadFloat(rdram, at), loadFloat(rdram, at + 4u) + 0.95f, loadFloat(rdram, at + 8u)};
            float eye[3];
            for (int i = 0; i < 3; ++i)
                eye[i] = loadFloat(rdram, camera + 0x210u + 4u * i);
            float toEye[3] = {eye[0] - centre[0], eye[1] - centre[1], eye[2] - centre[2]};
            const float distance = length3(toEye);
            const float margin = g_seeMargin.load(std::memory_order_relaxed);
            const Clip c = project(m, centre);
            if (!(c.w > 0.2f) || !(distance > margin + 0.2f) || !std::isfinite(c.x + c.y + c.z))
            {
                seeThroughOff();
                return;
            }
            for (float &v : toEye)
                v /= distance;
            const float flat = std::sqrt(toEye[0] * toEye[0] + toEye[2] * toEye[2]);
            const float side[3] = {flat > 1e-3f ? -toEye[2] / flat : 1.0f, 0.0f, flat > 1e-3f ? toEye[0] / flat : 0.0f};
            const float sidePoint[3] = {centre[0] + side[0], centre[1], centre[2] + side[2]};
            const float upPoint[3] = {centre[0], centre[1] + 1.0f, centre[2]};
            const Clip cs = project(m, sidePoint), cu = project(m, upPoint);
            if (!(cs.w > 0.05f) || !(cu.w > 0.05f))
            {
                seeThroughOff();
                return;
            }
            const float cx = c.x / c.w, cy = c.y / c.w;
            const float perMetreX = std::hypot(cs.x / cs.w - cx, cs.y / cs.w - cy);
            const float perMetreY = std::hypot(cu.x / cu.w - cx, cu.y / cu.w - cy);
            const float rx = g_seeWidth.load(std::memory_order_relaxed) * perMetreX;
            const float ry = g_seeHeight.load(std::memory_order_relaxed) * perMetreY;
            const float full = std::fmin(margin + 0.8f, distance - 0.15f);
            const float startPoint[3] = {centre[0] + toEye[0] * margin, centre[1] + toEye[1] * margin, centre[2] + toEye[2] * margin};
            const float fullPoint[3] = {centre[0] + toEye[0] * full, centre[1] + toEye[1] * full, centre[2] + toEye[2] * full};
            const Clip c0 = project(m, startPoint), c1 = project(m, fullPoint);
            if (!(c0.w > 0.01f) || !(c1.w > 0.01f) || !(rx > 1.0f) || !(ry > 1.0f) || rx > 20000.0f || ry > 20000.0f)
            {
                seeThroughOff();
                return;
            }
            char text[192];
            std::snprintf(text, sizeof(text), "%.2f %.2f %.2f %.2f %.3f %.3f %.3f %.3f", cx, cy, rx, ry, c0.z / c0.w,
                          c1.z / c1.w, g_seeStrength.load(std::memory_order_relaxed) / 100.0f,
                          g_seeSoftness.load(std::memory_order_relaxed) / 100.0f);
            if (!g_seeSent)
                GH_TRACE(Game, "see-through on: " << text);
            g_seeSent = GSPcsx2Backend::SetOption("see_through", text);
            g_seeReadout[0].store(cx - 2048.0f, std::memory_order_relaxed);
            g_seeReadout[1].store(cy - 2048.0f, std::memory_order_relaxed);
            g_seeReadout[2].store(rx, std::memory_order_relaxed);
            g_seeReadout[3].store(ry, std::memory_order_relaxed);
        }

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
            std::snprintf(text, sizeof(text), "%g", std::round(value * 100.0f) / 100.0f);
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

        // [Video] SeeThrough*, CameraEaseBack*: read straight from godhand.ini (at install and on Revert).
        void loadExtras(const IniFile &ini)
        {
            g_seeThrough = ini.getBool("Video", "SeeThrough", true);
            g_seeWidth = iniValue(ini, "Video", "SeeThroughWidth", 1.6f, 0.3f, 6.0f);
            g_seeHeight = iniValue(ini, "Video", "SeeThroughHeight", 1.3f, 0.3f, 6.0f);
            g_seeMargin = iniValue(ini, "Video", "SeeThroughMargin", 1.0f, 0.2f, 5.0f);
            g_seeStrength = iniValue(ini, "Video", "SeeThroughStrength", 100.0f, 0.0f, 100.0f);
            g_seeSoftness = iniValue(ini, "Video", "SeeThroughSoftness", 40.0f, 2.0f, 100.0f);
            g_easeBackOn = ini.getBool("Video", "CameraEaseBack", true);
            g_easeBackSeconds = iniValue(ini, "Video", "CameraEaseBackSeconds", 0.5f, 0.05f, 2.0f);
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
            ini.setBool("Video", "DepthOfField", g_depthOfField.load());
            ini.setBool("Video", "PreciseDepth", g_preciseDepth.load());
            ini.setBool("Video", "SeeThrough", g_seeThrough.load());
            ini.set("Video", "SeeThroughWidth", number(g_seeWidth.load()));
            ini.set("Video", "SeeThroughHeight", number(g_seeHeight.load()));
            ini.set("Video", "SeeThroughMargin", number(g_seeMargin.load()));
            ini.set("Video", "SeeThroughStrength", number(g_seeStrength.load()));
            ini.set("Video", "SeeThroughSoftness", number(g_seeSoftness.load()));
            ini.setBool("Video", "CameraEaseBack", g_easeBackOn.load());
            ini.set("Video", "CameraEaseBackSeconds", number(g_easeBackSeconds.load()));
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
            g_depthOfField = ini.getBool("Video", "DepthOfField", true);
            g_preciseDepth = ini.getBool("Video", "PreciseDepth", true);
            loadExtras(ini);
            GSPcsx2Backend::SetOption("precise_depth", g_preciseDepth.load() ? "1" : "0");
            GSPcsx2Backend::SetOption("dof", g_depthOfField.load() ? "1" : "0");
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

            ui->separator_text("See-through");
            int see = g_seeThrough.load() ? 1 : 0;
            if (ui->checkbox("Dissolve what stands between the camera and Gene##see", &see))
                g_seeThrough = see != 0;
            ui->begin_disabled(see == 0);
            slider(ui, "Oval half-width##seew", g_seeWidth, 0.3f, 4.0f, "%.2f m");
            slider(ui, "Oval half-height##seeh", g_seeHeight, 0.3f, 4.0f, "%.2f m");
            slider(ui, "Starts this far in front of Gene##seem", g_seeMargin, 0.2f, 3.0f, "%.2f m");
            slider(ui, "Strength##sees", g_seeStrength, 0.0f, 100.0f, "%.0f%%");
            slider(ui, "Soft edge##seesoft", g_seeSoftness, 2.0f, 100.0f, "%.0f%%");
            ui->end_disabled();
            ui->text_disabled("Sizes are in metres round Gene's middle. Strength 100% = gone in the middle; the soft "
                              "edge is the part of the oval over which it fades out.");

            ui->separator_text("Ease back");
            int ease = g_easeBackOn.load() ? 1 : 0;
            if (ui->checkbox("Glide back after the game moves the camera##ease", &ease))
                g_easeBackOn = ease != 0;
            ui->begin_disabled(ease == 0);
            slider(ui, "Glide time##easet", g_easeBackSeconds, 0.1f, 1.5f, "%.2f s");
            ui->end_disabled();

            ui->separator_text("Now");
            char line[256];
            const float scale = coneScale(3.0f);
            std::snprintf(line, sizeof(line), "%s camera, fov %.1f deg (x%.2f), camera %.2f m from Gene (x%.2f of the game's)",
                          g_battle.load() ? "Battle" : "Exploring", g_camFov.load(), g_mul, g_eyeDistance.load(), g_dolly.load());
            ui->text(line);
            std::snprintf(line, sizeof(line), "Enemies at 3 m: cones x%.2f (the 60 deg wedge is %.0f deg, DIE's 90 deg lunge %.0f deg)",
                          scale, scaledCone(60.0f * kDegToRad, 3.0f) / kDegToRad, scaledCone(90.0f * kDegToRad, 3.0f) / kDegToRad);
            ui->text(line);
            if (g_seeThrough.load())
            {
                std::snprintf(line, sizeof(line), "See-through oval: centre %.0f, %.0f, radii %.0f x %.0f (game pixels, 512 x 448)",
                              g_seeReadout[0].load() + 256.0f, g_seeReadout[1].load() + 224.0f, g_seeReadout[2].load(), g_seeReadout[3].load());
                ui->text_disabled(line);
            }

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

        // The Graphics tab: switches the renderer applies at once (pcsx2gs_set_option).
        void drawGraphicsPanel(const Pcsx2GsUi *ui)
        {
            int dof = g_depthOfField.load() ? 1 : 0;
            if (ui->checkbox("Depth of field##dof", &dof))
            {
                g_depthOfField = dof != 0;
                if (!GSPcsx2Backend::SetOption("dof", dof ? "1" : "0"))
                    setStatus("This pcsx2gs.dll cannot switch it while playing: build again.");
            }
            ui->text_disabled("The game's blur of the distance: a small blurred copy of the picture blended over it by "
                              "depth. At high internal resolutions its edges show as dark, jagged fringes that "
                              "shimmer as the camera moves.");
            int precise = g_preciseDepth.load() ? 1 : 0;
            if (ui->checkbox("Precise depth##precise", &precise))
            {
                g_preciseDepth = precise != 0;
                if (!GSPcsx2Backend::SetOption("precise_depth", precise ? "1" : "0"))
                    setStatus("This pcsx2gs.dll cannot switch it while playing: build again.");
            }
            ui->text_disabled("The game's 16-bit depth is coarse in the distance, so surfaces that meet there cut into "
                              "each other in jagged, shifting slivers (on the PS2 too, under a pixel). On: the renderer "
                              "takes each surface's exact depth from its texture perspective instead. Off: as the PS2.");
            ui->separator_text("godhand.ini");
            if (ui->button("Save##graphics"))
                saveToIni();
            {
                std::lock_guard<std::mutex> lock(g_statusMutex);
                if (!g_status.empty())
                {
                    ui->same_line();
                    ui->text(g_status.c_str());
                }
            }
        }
#endif
    }

    void setCameraIniPath(const std::filesystem::path &path)
    {
        g_iniPath = path;
    }

    const std::filesystem::path &cameraIniPath()
    {
        return g_iniPath;
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
            const char *precise = std::getenv("PS2X_PCSX2_PRECISE_DEPTH");
            g_preciseDepth = !(precise && *precise == '0'); // the DLL reads the same variable when it opens
            const char *dof = std::getenv("PS2X_PCSX2_DOF");
            g_depthOfField = !(dof && *dof == '0'); // the DLL reads the same variable when it opens
        }
        {
            IniFile ini;
            if (!g_iniPath.empty() && ini.load(g_iniPath))
                loadExtras(ini);
        }
        g_mul = fovFactor();
        ps2xRegisterHostHook("gh_cam_update", &cameraUpdateStart);
        ps2xRegisterHostHook("gh_cam_matrix", &cameraMatrixReady);
        ps2xRegisterHostHook("gh_fov_ease_a", &fovEaseA);
        ps2xRegisterHostHook("gh_fov_ease_b", &fovEaseB);
        ps2xRegisterHostHook("gh_fov_reset", &fovReset);
        ps2xRegisterHostHook("gh_cam_pitch_f4", &orbitPitchF4);
        ps2xRegisterHostHook("gh_cam_pitch_f0", &orbitPitchF0);
        ps2xRegisterHostHook("gh_cam_distance", &orbitDistanceF0);
        ps2xRegisterHostHook("gh_cam_yaw", &orbitYaw);
        ps2xRegisterHostHook("gh_em_wedge", &enemyWedge);
        ps2xRegisterHostHook("gh_em_lunge", &enemyLunge);
        ps2xSetPatchFlag("fov", true);
        ps2xSetPatchFlag("enemy_view", true);
        GH_INFO(Game, "camera: field of view " << g_fovPercent.load() << "%, " << g_fightPercent.load()
                                               << "% in fights; comes in " << g_closerPercent.load() << "%, tilt "
                                               << g_tiltDegrees.load() << " degrees; enemies follow the view: "
                                               << (g_enemyView.load() ? "on" : "off") << "; see-through: "
                                               << (g_seeThrough.load() ? "on" : "off") << "; ease back: "
                                               << (g_easeBackOn.load() ? "on" : "off"));
        modern::install(); // the Control Style (its Controls tab comes after Camera and Graphics)
#if defined(GODHAND_HAS_GS_MENU)
        static bool s_panel = false;
        if (!s_panel)
        {
            s_panel = true;
            ps2x::menu::setTitle("God Hand: Rearmed", "F2 closes this menu. The game keeps running behind it.");
            ps2x::menu::addPanel("Camera", &drawCameraPanel);
            ps2x::menu::addPanel("Graphics", &drawGraphicsPanel);
        }
#endif
    }
}
