// Control Style: Classic (the PS2's) or Modern.
//
// Modern: the right stick turns a free camera round Gene instead of dodging. The original controls (right stick =
// dodges, the camera behind Gene) are in effect while L1 is held (Style: Hold) or after a press of L1 until the next
// (Style: Toggle), and with Auto also while the game's battle camera is on (Gene has an enemy targeted). A double tap
// of L1 is the original quick turn (L1 reaches the game only then), and the camera swings back behind Gene with him.
// Whenever the game takes the camera (a close-up for a grab or a special move, a cutscene, a menu: no player camera
// update for a moment), everything is Classic, L1 included.
//
// Input side (window thread, the pad filter ps2_host_input calls for player 1): L1 and the right stick are taken
// from what the game sees and published in atomics. Camera side (game thread, camera.cpp's yaw hook): the camera's
// yaw round Gene and an extra pitch, eased back to the original camera when the original controls are in effect.
//
// Step 1 of the Modern scheme: the left stick still moves Gene as on the PS2 (relative to where he faces).
#include "godhand/modern_controls.h"
#include "godhand/camera.h"
#include "godhand/ini_file.h"
#include "godhand/log.h"
#include "ps2_host_input.h"
#include "runtime/ps2_game_menu.h"

#include <atomic>
#include <cctype>
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

namespace godhand::modern
{
    namespace
    {
        constexpr float kPi = 3.14159265f;
        constexpr float kDegToRad = kPi / 180.0f;
        constexpr int64_t kDoubleTapMs = 300;  // second press of L1 within this: the quick turn
        constexpr int64_t kQuickTurnMs = 90;   // how long L1 is held for the game then
        constexpr int64_t kNoCameraMs = 150;   // no player camera update for this long: the game has the camera
        constexpr float kStickDeadzone = 0.18f;
        constexpr float kPitchMin = -0.35f, kPitchMax = 0.9f; // extra pitch, radians

        // Settings (F2 menu on the GS thread, read by the window and game threads).
        std::atomic<bool> g_modern{false};
        std::atomic<bool> g_auto{true};
        std::atomic<bool> g_toggleStyle{false};
        std::atomic<float> g_yawSpeed{150.0f};  // degrees a second at full stick
        std::atomic<float> g_pitchSpeed{80.0f};
        std::atomic<bool> g_invertX{false};
        std::atomic<bool> g_invertY{false};

        // Camera side -> input side.
        std::atomic<bool> g_battle{false};
        std::atomic<bool> g_gameCamera{false};
        std::atomic<int64_t> g_lastCameraMs{0};

        // Input side -> camera side.
        std::atomic<bool> g_classicByL1{false}; // L1 held (Hold) or toggled on (Toggle)
        std::atomic<float> g_stickX{0.0f}, g_stickY{0.0f};
        std::atomic<uint32_t> g_recenter{0};    // bumped by each quick turn

        // Input side only (window thread).
        bool g_l1Was = false;
        bool g_toggled = false;
        int64_t g_lastPressMs = -10000;
        int64_t g_quickTurnUntilMs = 0;

        int64_t nowMs()
        {
            using namespace std::chrono;
            return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
        }

        bool gameHasCamera()
        {
            return g_gameCamera.load(std::memory_order_relaxed) ||
                   nowMs() - g_lastCameraMs.load(std::memory_order_relaxed) > kNoCameraMs;
        }

        // The original controls are in effect (besides the game having the camera).
        bool classicByChoice()
        {
            return g_classicByL1.load(std::memory_order_relaxed) ||
                   (g_auto.load(std::memory_order_relaxed) && g_battle.load(std::memory_order_relaxed));
        }

        float stickAxis(uint8_t value)
        {
            float axis = (static_cast<float>(value) - 128.0f) / 127.0f;
            axis = std::fmax(-1.0f, std::fmin(1.0f, axis));
            const float magnitude = std::fabs(axis);
            if (magnitude < kStickDeadzone)
                return 0.0f;
            return std::copysign((magnitude - kStickDeadzone) / (1.0f - kStickDeadzone), axis);
        }

        // ps2_host_input's pad filter (window thread), player 1 only.
        void padFilter(uint32_t player, ps2x::iop::PadInputState &state)
        {
            if (player != 0u || !g_modern.load(std::memory_order_relaxed))
                return;
            const int64_t now = nowMs();
            if (gameHasCamera())
            {
                // The game's own moment: everything as on the PS2, L1 included; the L1 state starts over after it.
                g_l1Was = (state.buttons & ps2x::iop::PadButton::L1) != 0u;
                g_stickX = 0.0f;
                g_stickY = 0.0f;
                return;
            }
            const bool l1 = (state.buttons & ps2x::iop::PadButton::L1) != 0u;
            if (l1 && !g_l1Was)
            {
                if (now - g_lastPressMs <= kDoubleTapMs)
                {
                    g_quickTurnUntilMs = now + kQuickTurnMs;
                    g_recenter.fetch_add(1u, std::memory_order_relaxed);
                    g_lastPressMs = -10000;
                }
                else
                    g_lastPressMs = now;
                if (g_toggleStyle.load(std::memory_order_relaxed))
                    g_toggled = !g_toggled;
            }
            g_l1Was = l1;
            const bool toggle = g_toggleStyle.load(std::memory_order_relaxed);
            if (!toggle)
                g_toggled = false;
            g_classicByL1 = toggle ? g_toggled : l1;

            // L1 reaches the game only as the quick turn.
            constexpr size_t kL1Pressure = static_cast<size_t>(ps2x::iop::PadPressure::L1);
            const bool quickTurn = now < g_quickTurnUntilMs;
            state.buttons = static_cast<uint16_t>((state.buttons & ~ps2x::iop::PadButton::L1) |
                                                  (quickTurn ? ps2x::iop::PadButton::L1 : 0u));
            state.pressure[kL1Pressure] = quickTurn ? 0xFFu : 0u;

            if (classicByChoice())
            {
                g_stickX = 0.0f;
                g_stickY = 0.0f;
                return;
            }
            // The free camera has the right stick; the game sees it centred (no dodges).
            g_stickX = stickAxis(state.rightX);
            g_stickY = stickAxis(state.rightY);
            state.rightX = 0x80u;
            state.rightY = 0x80u;
        }

        // Camera side (game thread).
        float g_yaw = 0.0f;
        float g_pitch = 0.0f;
        bool g_tracking = true;      // the free camera is on the original one (eased in, or just reset)
        bool g_following = false;   // a quick turn: easing back behind Gene before it is free again
        uint32_t g_recenterSeen = 0;

        float wrapAngle(float angle)
        {
            while (angle > kPi)
                angle -= 2.0f * kPi;
            while (angle < -kPi)
                angle += 2.0f * kPi;
            return angle;
        }

        std::filesystem::path g_iniPath;
        std::mutex g_statusMutex;
        std::string g_status;

        void setStatus(std::string text)
        {
            std::lock_guard<std::mutex> lock(g_statusMutex);
            g_status = std::move(text);
        }

        std::string number(float value)
        {
            char text[32];
            std::snprintf(text, sizeof(text), "%g", std::round(value * 10.0f) / 10.0f);
            return text;
        }

        void saveToIni()
        {
            IniFile ini;
            if (g_iniPath.empty() || !ini.load(g_iniPath))
            {
                setStatus("Could not read godhand.ini.");
                return;
            }
            ini.set("Game", "ControlStyle", g_modern.load() ? "modern" : "classic");
            ini.setBool("Game", "ModernAuto", g_auto.load());
            ini.set("Game", "ModernL1", g_toggleStyle.load() ? "toggle" : "hold");
            ini.set("Game", "CameraSpeed", number(g_yawSpeed.load()));
            ini.set("Game", "CameraPitchSpeed", number(g_pitchSpeed.load()));
            ini.setBool("Game", "CameraInvertX", g_invertX.load());
            ini.setBool("Game", "CameraInvertY", g_invertY.load());
            setStatus(ini.save() ? "Saved to godhand.ini." : "Could not write godhand.ini.");
        }

#if defined(GODHAND_HAS_GS_MENU)
        void slider(const Pcsx2GsUi *ui, const char *label, std::atomic<float> &setting, float low, float high, const char *format)
        {
            float value = setting.load(std::memory_order_relaxed);
            ui->set_next_width(340.0f);
            if (ui->slider_float(label, &value, low, high, format))
                setting.store(value, std::memory_order_relaxed);
        }

        void checkbox(const Pcsx2GsUi *ui, const char *label, std::atomic<bool> &setting)
        {
            int value = setting.load() ? 1 : 0;
            if (ui->checkbox(label, &value))
                setting = value != 0;
        }

        void drawControlsPanel(const Pcsx2GsUi *ui)
        {
            if (ui->version < 3u)
            {
                ui->text_wrapped("This pcsx2gs.dll is older than the game: build again to get these controls.");
                return;
            }
            ui->separator_text("Control style");
            static const char *const kStyles[] = {"Classic: as on the PS2", "Modern: free camera on the right stick"};
            int style = g_modern.load() ? 1 : 0;
            ui->set_next_width(340.0f);
            if (ui->combo("##style", &style, kStyles, 2))
                g_modern = style == 1;
            ui->begin_disabled(style == 0);
            checkbox(ui, "Auto: the original controls while the battle camera is on##auto", g_auto);
            static const char *const kL1[] = {"Hold L1 for the original controls", "Press L1 to switch to them and back"};
            int l1 = g_toggleStyle.load() ? 1 : 0;
            ui->set_next_width(340.0f);
            if (ui->combo("##l1", &l1, kL1, 2))
                g_toggleStyle = l1 == 1;
            ui->text_disabled("Double-tap L1: the original quick turn; the camera swings back behind Gene.");
            ui->separator_text("Free camera");
            slider(ui, "Turn speed##yaw", g_yawSpeed, 40.0f, 400.0f, "%.0f deg/s");
            slider(ui, "Up/down speed##pitch", g_pitchSpeed, 20.0f, 250.0f, "%.0f deg/s");
            checkbox(ui, "Invert left/right##invx", g_invertX);
            checkbox(ui, "Invert up/down##invy", g_invertY);
            ui->end_disabled();

            ui->separator_text("Now");
            const char *now = !g_modern.load()    ? "Classic"
                              : gameHasCamera()   ? "The game has the camera: original controls"
                              : classicByChoice() ? (g_classicByL1.load() ? "Original controls (L1)" : "Original controls (battle camera)")
                                                  : "Free camera";
            ui->text(now);
            ui->text_disabled("Step 1: the left stick still moves Gene as on the PS2 (relative to where he faces).");

            ui->separator_text("godhand.ini");
            if (ui->button("Save##controls"))
                saveToIni();
            std::lock_guard<std::mutex> lock(g_statusMutex);
            if (!g_status.empty())
            {
                ui->same_line();
                ui->text(g_status.c_str());
            }
        }
#endif

        float envFloat(const char *name, float fallback, float low, float high)
        {
            const char *text = std::getenv(name);
            const float value = (text && *text) ? static_cast<float>(std::atof(text)) : fallback;
            return std::fmin(std::fmax(value, low), high);
        }

        bool envIs(const char *name, const char *value)
        {
            const char *text = std::getenv(name);
            if (!text)
                return false;
            std::string lower(text);
            for (char &c : lower)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return lower == value;
        }

        bool envBool(const char *name, bool fallback)
        {
            const char *text = std::getenv(name);
            return (text && *text) ? *text != '0' : fallback;
        }
    }

    void noteCamera(const CameraState &state)
    {
        g_battle.store(state.battle, std::memory_order_relaxed);
        g_gameCamera.store(state.gameCamera, std::memory_order_relaxed);
        g_lastCameraMs.store(nowMs(), std::memory_order_relaxed);
    }

    FreeCamera updateCamera(float gameYaw, float dt)
    {
        FreeCamera out;
        if (!g_modern.load(std::memory_order_relaxed))
        {
            g_tracking = true;
            return out;
        }
        dt = std::fmin(std::fmax(dt, 0.0f), 0.1f);
        const uint32_t recenter = g_recenter.load(std::memory_order_relaxed);
        if (recenter != g_recenterSeen)
        {
            g_recenterSeen = recenter;
            g_following = true;
        }
        const bool original = gameHasCamera() || classicByChoice();
        if (g_tracking && !original && !g_following)
        {
            g_yaw = gameYaw; // starts free from where the original camera is
            g_pitch = 0.0f;
            g_tracking = false;
        }
        if (original || g_following)
        {
            // Ease back to the original camera; once there, it is the original camera exactly.
            const float ease = 1.0f - std::exp(-dt * 7.0f);
            g_yaw = wrapAngle(g_yaw + wrapAngle(gameYaw - g_yaw) * ease);
            g_pitch += (0.0f - g_pitch) * ease;
            if (std::fabs(wrapAngle(gameYaw - g_yaw)) < 0.01f && std::fabs(g_pitch) < 0.005f)
            {
                g_following = false;
                if (original)
                    g_tracking = true;
            }
            if (g_tracking)
                return out;
        }
        else
        {
            const float x = g_stickX.load(std::memory_order_relaxed) * (g_invertX.load() ? -1.0f : 1.0f);
            const float y = g_stickY.load(std::memory_order_relaxed) * (g_invertY.load() ? -1.0f : 1.0f);
            g_yaw = wrapAngle(g_yaw - x * g_yawSpeed.load(std::memory_order_relaxed) * kDegToRad * dt);
            g_pitch = std::fmin(std::fmax(g_pitch + y * g_pitchSpeed.load(std::memory_order_relaxed) * kDegToRad * dt, kPitchMin), kPitchMax);
        }
        out.free = true;
        out.yaw = g_yaw;
        out.pitch = g_pitch;
        return out;
    }

    void resetCamera()
    {
        g_tracking = true;
        g_following = false;
    }

    void install()
    {
        g_modern = envIs("GODHAND_CONTROL_STYLE", "modern");
        g_auto = envBool("GODHAND_MODERN_AUTO", true);
        g_toggleStyle = envIs("GODHAND_MODERN_L1", "toggle");
        g_yawSpeed = envFloat("GODHAND_CAMERA_SPEED", 150.0f, 40.0f, 400.0f);
        g_pitchSpeed = envFloat("GODHAND_CAMERA_PITCH_SPEED", 80.0f, 20.0f, 250.0f);
        g_invertX = envBool("GODHAND_CAMERA_INVERT_X", false);
        g_invertY = envBool("GODHAND_CAMERA_INVERT_Y", false);
        g_iniPath = cameraIniPath();
        ps2_host_input::setPadFilter(&padFilter);
        GH_INFO(Game, "control style: " << (g_modern.load() ? "modern" : "classic")
                                        << (g_modern.load() ? (g_toggleStyle.load() ? " (L1 toggles" : " (L1 held") : "")
                                        << (g_modern.load() ? (g_auto.load() ? ", auto in battle)" : ")") : ""));
#if defined(GODHAND_HAS_GS_MENU)
        static bool s_panel = false;
        if (!s_panel)
        {
            s_panel = true;
            ps2x::menu::addPanel("Controls", &drawControlsPanel);
        }
#endif
    }
}
