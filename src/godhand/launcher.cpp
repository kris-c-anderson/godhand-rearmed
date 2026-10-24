#include "godhand/launcher.h"

#include "godhand/character_mods.h"
#include "godhand/game_disc.h"
#include "godhand/ini_file.h"
#include "godhand/launcher_platform.h"
#include "godhand/log.h"
#include "ps2_host_gamepads.h"
#include "runtime/ps2_display_options.h"

#include "imgui.h"
#include "raylib.h"
#include "rlImGui.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace godhand::launcher
{
    namespace
    {
        namespace fs = std::filesystem;

        // ---- choices ------------------------------------------------------------------------------------------------
        struct Choice
        {
            const char *label;
            const char *value;
        };

        constexpr Choice kAspects[] = {{"4:3 (the PS2's)", "4:3"}, {"16:9", "16:9"},  {"16:10", "16:10"},
                                       {"21:9", "21:9"},          {"32:9", "32:9"},  {"Match the screen", "auto"}};
        constexpr Choice kHudLayouts[] = {{"16:9: HUD at the screen edges", "16:9"}, {"4:3: HUD where the PS2 had it", "4:3"}};
        constexpr Choice kMenuLayouts[] = {{"4:3: as on the PS2, with side bars", "4:3"}, {"Stretched to the screen", "stretch"}};
        constexpr Choice kRouletteTimers[] = {{"Normal", "normal"}, {"Double time", "double"}};
        constexpr Choice kPostEffects[] = {{"PS2 resolution (PCSX2's fix)", "ps2"}, {"Upscaled", "upscaled"}, {"No fix", "off"}};
        constexpr Choice kWindowModes[] = {{"Window", "window"}, {"Fullscreen (borderless)", "fullscreen"}, {"Maximized window", "maximized"}};
        constexpr Choice kFovs[] = {{"100%: as on the PS2", "100"}, {"110%", "110"}, {"120%", "120"}, {"130%", "130"},
                                    {"140%", "140"}, {"150%", "150"}};
        constexpr Choice kCameraDistances[] = {{"Stays: wider view looks zoomed out", "0"}, {"Halfway in", "50"},
                                               {"Comes in: Gene keeps his size", "100"}};
        constexpr Choice kCameraTilts[] = {{"None", "0"}, {"5 degrees", "5"}, {"10 degrees", "10"}, {"15 degrees", "15"}};
        constexpr Choice kWindowSizes[] = {{"Automatic (3/4 of the screen)", "auto"}, {"PS2 size", "native"}};
        constexpr Choice kResolutions[] = {{"1x  native (512x448)", "1"}, {"2x  1024x896", "2"}, {"3x  1536x1344", "3"},
                                           {"4x  2048x1792", "4"},        {"5x  2560x2240", "5"}, {"6x  3072x2688", "6"},
                                           {"8x  4096x3584", "8"}};

        std::string utf8(const fs::path &path)
        {
            const std::u8string text = path.u8string();
            return std::string(text.begin(), text.end());
        }

        fs::path fromUtf8(const std::string &text) { return fs::path(std::u8string(text.begin(), text.end())); }

        std::string lower(std::string text)
        {
            for (char &c : text)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return text;
        }

        // A combo over fixed choices; a value that is none of them (set by hand) is shown as one more entry and kept
        // unless another is picked. Returns true when changed (value updated).
        template <size_t N>
        bool choiceCombo(const char *id, const Choice (&choices)[N], std::string &value)
        {
            int current = -1;
            for (size_t i = 0; i < N; ++i)
                if (lower(value) == lower(choices[i].value))
                    current = static_cast<int>(i);
            std::vector<const char *> labels;
            for (const Choice &choice : choices)
                labels.push_back(choice.label);
            const std::string custom = "Custom: " + value;
            if (current < 0)
            {
                labels.push_back(custom.c_str());
                current = static_cast<int>(N);
            }
            int picked = current;
            ImGui::SetNextItemWidth(-1.0f);
            if (!ImGui::Combo(id, &picked, labels.data(), static_cast<int>(labels.size())) || picked == current || picked >= static_cast<int>(N))
                return false;
            value = choices[picked].value;
            return true;
        }

        // ---- look ---------------------------------------------------------------------------------------------------
        const ImVec4 kAccent(0.91f, 0.38f, 0.16f, 1.0f);      // God Hand's fire orange
        const ImVec4 kAccentHover(1.00f, 0.50f, 0.22f, 1.0f);
        const ImVec4 kAccentActive(0.78f, 0.28f, 0.10f, 1.0f);
        const ImVec4 kPanel(0.10f, 0.09f, 0.09f, 0.92f);
        const ImVec4 kTextDim(0.66f, 0.62f, 0.58f, 1.0f);

        void applyStyle()
        {
            ImGuiStyle &style = ImGui::GetStyle();
            ImGui::StyleColorsDark(&style);
            style.WindowRounding = 0.0f;
            style.FrameRounding = 6.0f;
            style.PopupRounding = 6.0f;
            style.GrabRounding = 6.0f;
            style.TabRounding = 6.0f;
            style.FramePadding = ImVec2(12.0f, 8.0f);
            style.ItemSpacing = ImVec2(12.0f, 12.0f);
            style.WindowPadding = ImVec2(0.0f, 0.0f);
            ImVec4 *colors = style.Colors;
            colors[ImGuiCol_WindowBg] = ImVec4(0, 0, 0, 0);
            colors[ImGuiCol_ChildBg] = kPanel;
            colors[ImGuiCol_PopupBg] = ImVec4(0.12f, 0.11f, 0.10f, 0.98f);
            colors[ImGuiCol_Border] = ImVec4(0.35f, 0.22f, 0.15f, 0.6f);
            colors[ImGuiCol_FrameBg] = ImVec4(0.18f, 0.16f, 0.15f, 1.0f);
            colors[ImGuiCol_FrameBgHovered] = ImVec4(0.26f, 0.20f, 0.17f, 1.0f);
            colors[ImGuiCol_FrameBgActive] = ImVec4(0.32f, 0.22f, 0.17f, 1.0f);
            colors[ImGuiCol_Button] = ImVec4(0.20f, 0.17f, 0.15f, 1.0f);
            colors[ImGuiCol_ButtonHovered] = kAccentHover;
            colors[ImGuiCol_ButtonActive] = kAccentActive;
            colors[ImGuiCol_Header] = ImVec4(0.30f, 0.20f, 0.15f, 1.0f);
            colors[ImGuiCol_HeaderHovered] = kAccentHover;
            colors[ImGuiCol_HeaderActive] = kAccentActive;
            colors[ImGuiCol_CheckMark] = kAccent;
            colors[ImGuiCol_Tab] = ImVec4(0.18f, 0.15f, 0.13f, 1.0f);
            colors[ImGuiCol_TabHovered] = kAccentHover;
            colors[ImGuiCol_TabSelected] = kAccent;
            colors[ImGuiCol_NavCursor] = kAccentHover;
            colors[ImGuiCol_Separator] = ImVec4(0.35f, 0.22f, 0.15f, 0.6f);
            colors[ImGuiCol_Text] = ImVec4(0.95f, 0.92f, 0.88f, 1.0f);
            colors[ImGuiCol_TextDisabled] = kTextDim;
        }

        void drawBackground()
        {
            const int width = GetScreenWidth(), height = GetScreenHeight();
            DrawRectangleGradientV(0, 0, width, height, Color{20, 15, 13, 255}, Color{48, 20, 10, 255});
            // A low glow behind the title.
            DrawCircleGradient(width / 2, height / 4, static_cast<float>(std::max(width, height)) * 0.45f, Color{120, 45, 15, 70},
                               Color{0, 0, 0, 0});
        }

        // Every controller, as ImGui's gamepad keys (SDL3 through ps2_host_gamepads: the same devices the game uses).
        void feedGamepads(ImGuiIO &io)
        {
            using namespace ps2_host_gamepads;
            update();
            bool held[16] = {};
            float lx = 0.0f, ly = 0.0f;
            for (int device = 0; device < kMaxDevices; ++device)
            {
                if (!connected(device))
                    continue;
                held[0] |= button(device, DPadUp);
                held[1] |= button(device, DPadDown);
                held[2] |= button(device, DPadLeft);
                held[3] |= button(device, DPadRight);
                held[4] |= button(device, South);
                held[5] |= button(device, East);
                held[6] |= button(device, West);
                held[7] |= button(device, North);
                held[8] |= button(device, LeftShoulder);
                held[9] |= button(device, RightShoulder);
                held[10] |= button(device, Start);
                held[11] |= button(device, Back);
                if (std::fabs(axis(device, LeftX)) > std::fabs(lx))
                    lx = axis(device, LeftX);
                if (std::fabs(axis(device, LeftY)) > std::fabs(ly))
                    ly = axis(device, LeftY);
            }
            const ImGuiKey keys[] = {ImGuiKey_GamepadDpadUp,   ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight,
                                     ImGuiKey_GamepadFaceDown, ImGuiKey_GamepadFaceRight, ImGuiKey_GamepadFaceLeft, ImGuiKey_GamepadFaceUp,
                                     ImGuiKey_GamepadL1,       ImGuiKey_GamepadR1,       ImGuiKey_GamepadStart,    ImGuiKey_GamepadBack};
            for (int i = 0; i < 12; ++i)
                io.AddKeyEvent(keys[i], held[i]);
            auto stick = [&](ImGuiKey negative, ImGuiKey positive, float value)
            {
                const float dead = 0.35f;
                const float n = value < -dead ? std::min(1.0f, (-value - dead) / (1.0f - dead)) : 0.0f;
                const float p = value > dead ? std::min(1.0f, (value - dead) / (1.0f - dead)) : 0.0f;
                io.AddKeyAnalogEvent(negative, n > 0.0f, n);
                io.AddKeyAnalogEvent(positive, p > 0.0f, p);
            };
            stick(ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight, lx);
            stick(ImGuiKey_GamepadLStickUp, ImGuiKey_GamepadLStickDown, ly);
        }

        // ---- the screens --------------------------------------------------------------------------------------------
        enum class Screen
        {
            Home,
            Install,
            Options,
        };

        struct State
        {
            const Context *context = nullptr;
            IniFile ini;
            Screen screen = Screen::Home;
            bool installed = false;
            std::string installMessage;
            bool installError = false;
            char isoText[1024] = {};
            bool focusFirst = true; // put the gamepad/keyboard cursor on the screen's first button
            bool done = false;
            Result result = Result::Quit;
            ImFont *font = nullptr;
            float unit = 1.0f; // UI scale: window height / 720
            Texture2D logo{};  // resources/images/logo.png when present (the project's own logo)
        };

        float fontSize(const State &state, float base) { return std::round(base * state.unit); }

        void pushSize(const State &state, float base) { ImGui::PushFont(state.font, fontSize(state, base)); }

        void centeredText(const char *text, const ImVec4 &color)
        {
            const float width = ImGui::CalcTextSize(text).x;
            ImGui::SetCursorPosX(std::max(0.0f, (ImGui::GetWindowWidth() - width) * 0.5f));
            ImGui::TextColored(color, "%s", text);
        }

        bool bigButton(const State &state, const char *label, float width, bool accent)
        {
            if (accent)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.08f, 0.05f, 0.04f, 1.0f));
            }
            pushSize(state, 30.0f);
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - width) * 0.5f);
            const bool clicked = ImGui::Button(label, ImVec2(width, fontSize(state, 64.0f)));
            ImGui::PopFont();
            if (accent)
                ImGui::PopStyleColor(2);
            return clicked;
        }

        void title(const State &state)
        {
            ImGui::Dummy(ImVec2(0.0f, fontSize(state, 70.0f)));
            if (state.logo.id != 0u && state.logo.height > 0)
            {
                const float width = std::min(ImGui::GetWindowWidth() * 0.8f, fontSize(state, 820.0f));
                const float height = width * static_cast<float>(state.logo.height) / static_cast<float>(state.logo.width);
                ImGui::SetCursorPosX((ImGui::GetWindowWidth() - width) * 0.5f);
                rlImGuiImageSize(&state.logo, static_cast<int>(width), static_cast<int>(height));
                ImGui::Dummy(ImVec2(0.0f, fontSize(state, 6.0f)));
            }
            else
            {
                pushSize(state, 92.0f);
                centeredText("G O D   H A N D", ImVec4(1.0f, 0.62f, 0.30f, 1.0f));
                ImGui::PopFont();
                pushSize(state, 40.0f);
                centeredText("R E A R M E D", ImVec4(1.0f, 0.62f, 0.30f, 1.0f));
                ImGui::PopFont();
            }
            pushSize(state, 22.0f);
            centeredText("A native PC port of God Hand", kTextDim);
            ImGui::PopFont();
            ImGui::Dummy(ImVec2(0.0f, fontSize(state, 40.0f)));
        }

        bool checkImage(State &state, const fs::path &image)
        {
            if (image.empty())
            {
                state.installMessage = "Choose your God Hand (USA, SLUS-21503) disc image.";
                state.installError = true;
                return false;
            }
            std::error_code error;
            if (!fs::is_regular_file(image, error))
            {
                state.installMessage = "There is no file at " + utf8(image) + ".";
                state.installError = true;
                return false;
            }
            const DiscCheckResult check = verifyDiscAndPrepare(image, state.context->cacheDir);
            if (!check.ok)
            {
                state.installMessage = check.error;
                state.installError = true;
                return false;
            }
            state.ini.set("Game", "Iso", utf8(image));
            state.ini.save();
            state.installed = true;
            state.installMessage.clear();
            state.installError = false;
            GH_INFO(Boot, "launcher: disc image installed: " << utf8(image));
            return true;
        }

        void drawHome(State &state)
        {
            title(state);
            const float buttonWidth = fontSize(state, 420.0f);
            if (!state.installed)
            {
                pushSize(state, 22.0f);
                centeredText("To play, point the port to your own copy of the game.", kTextDim);
                ImGui::PopFont();
                ImGui::Dummy(ImVec2(0.0f, fontSize(state, 10.0f)));
                if (bigButton(state, "Install", buttonWidth, true))
                {
                    state.screen = Screen::Install;
                    state.focusFirst = true;
                }
                if (state.focusFirst)
                    ImGui::SetItemDefaultFocus();
            }
            else
            {
                if (bigButton(state, "Play", buttonWidth, true))
                {
                    state.result = Result::Play;
                    state.done = true;
                }
                if (state.focusFirst)
                    ImGui::SetItemDefaultFocus();
                if (bigButton(state, "Options", buttonWidth, false))
                {
                    state.screen = Screen::Options;
                    state.focusFirst = true;
                }
                ImGui::Dummy(ImVec2(0.0f, fontSize(state, 6.0f)));
                pushSize(state, 20.0f);
                bool hide = !state.ini.getBool("Launcher", "Show", true);
                const char *label = "Don't show this again (hold Shift while starting to see it)";
                ImGui::SetCursorPosX((ImGui::GetWindowWidth() - ImGui::CalcTextSize(label).x - ImGui::GetFrameHeight() -
                                      ImGui::GetStyle().ItemInnerSpacing.x) *
                                     0.5f);
                if (ImGui::Checkbox(label, &hide))
                {
                    state.ini.setBool("Launcher", "Show", !hide);
                    state.ini.save();
                }
                ImGui::PopFont();
            }
            ImGui::Dummy(ImVec2(0.0f, fontSize(state, 24.0f)));
            pushSize(state, 22.0f);
            const float quitWidth = fontSize(state, 160.0f);
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - quitWidth) * 0.5f);
            if (ImGui::Button("Quit", ImVec2(quitWidth, 0.0f)))
            {
                state.result = Result::Quit;
                state.done = true;
            }
            ImGui::PopFont();
            state.focusFirst = false;
        }

        void drawInstall(State &state)
        {
            title(state);
            const float width = fontSize(state, 760.0f);
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - width) * 0.5f);
            pushSize(state, 22.0f);
            ImGui::BeginChild("install", ImVec2(width, 0.0f), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders |
                                                                  ImGuiChildFlags_AlwaysUseWindowPadding);
            ImGui::TextWrapped("The port plays from your own God Hand disc image (USA, SLUS-21503, the .iso of the disc). "
                               "Nothing is copied: the game reads it while you play, so keep it where it is.");
            ImGui::Spacing();
            if (ImGui::Button("Choose disc image..."))
            {
                const fs::path picked = platform::pick(GetWindowHandle(), false);
                if (!picked.empty())
                {
                    const std::string text = utf8(picked);
                    std::snprintf(state.isoText, sizeof(state.isoText), "%s", text.c_str());
                    checkImage(state, picked);
                }
            }
            if (state.focusFirst)
                ImGui::SetItemDefaultFocus();
            ImGui::TextColored(kTextDim, "or type its path:");
            ImGui::SetNextItemWidth(-1.0f);
            const bool entered = ImGui::InputText("##iso", state.isoText, sizeof(state.isoText), ImGuiInputTextFlags_EnterReturnsTrue);
            if (ImGui::Button("Check and install") || entered)
            {
                std::string text = state.isoText;
                if (text.size() >= 2 && text.front() == '"' && text.back() == '"')
                    text = text.substr(1, text.size() - 2);
                checkImage(state, fromUtf8(text));
            }
            if (!state.installMessage.empty())
            {
                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_Text, state.installError ? ImVec4(1.0f, 0.45f, 0.35f, 1.0f) : kTextDim);
                ImGui::TextWrapped("%s", state.installMessage.c_str());
                ImGui::PopStyleColor();
            }
            if (state.installed)
            {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.55f, 0.9f, 0.45f, 1.0f), "Installed. Your disc image checks out.");
            }
            ImGui::Spacing();
            if (ImGui::Button(state.installed ? "Continue" : "Back"))
            {
                state.screen = Screen::Home;
                state.focusFirst = true;
            }
            ImGui::EndChild();
            ImGui::PopFont();
            state.focusFirst = false;
        }

        // A setting: name and help on the left, the control on the right.
        void settingLabel(const char *name, const char *help)
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(name);
            if (help && *help)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
                ImGui::TextWrapped("%s", help);
                ImGui::PopStyleColor();
            }
            ImGui::TableSetColumnIndex(1);
        }

        bool beginSettings(const char *id)
        {
            if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX))
                return false;
            ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 0.58f);
            ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.42f);
            return true;
        }

        void iniCombo(State &state, const char *id, const char *section, const char *key, const char *fallback,
                      bool (*combo)(const char *, std::string &))
        {
            std::string value = state.ini.get(section, key, fallback);
            if (value.empty())
                value = fallback;
            if (combo(id, value))
                state.ini.set(section, key, value);
        }

        void iniCheckbox(State &state, const char *id, const char *section, const char *key, bool fallback)
        {
            bool value = state.ini.getBool(section, key, fallback);
            if (ImGui::Checkbox(id, &value))
                state.ini.setBool(section, key, value);
        }

        void drawOptions(State &state)
        {
            ImGui::Dummy(ImVec2(0.0f, fontSize(state, 30.0f)));
            pushSize(state, 46.0f);
            centeredText("Options", ImVec4(1.0f, 0.62f, 0.30f, 1.0f));
            ImGui::PopFont();
            const float width = std::min(ImGui::GetWindowWidth() - fontSize(state, 60.0f), fontSize(state, 1040.0f));
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - width) * 0.5f);
            pushSize(state, 21.0f);
            const float footer = ImGui::GetFrameHeightWithSpacing() + fontSize(state, 36.0f);
            ImGui::BeginChild("options", ImVec2(width, ImGui::GetContentRegionAvail().y - footer),
                              ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
            if (ImGui::BeginTabBar("tabs"))
            {
                const std::string aspect = state.ini.get("Video", "AspectRatio", "4:3");
                if (ImGui::BeginTabItem("Display"))
                {
                    if (beginSettings("display"))
                    {
                        settingLabel("Aspect ratio", "The picture's shape. Wider than 4:3 shows more of the scene to the "
                                                     "sides; menus keep the PS2's proportions.");
                        iniCombo(state, "##aspect", "Video", "AspectRatio", "4:3",
                                 [](const char *id, std::string &value) { return choiceCombo(id, kAspects, value); });
                        settingLabel("HUD layout", "Where the health bar, radar and the rest of the in-game HUD go on a "
                                                   "wide screen. Wider than 16:9 keeps them at the edges of a 16:9 area.");
                        ImGui::BeginDisabled(lower(aspect) == "4:3");
                        iniCombo(state, "##hud", "Video", "HudLayout", "16:9",
                                 [](const char *id, std::string &value) { return choiceCombo(id, kHudLayouts, value); });
                        ImGui::EndDisabled();
                        settingLabel("Menus", "Menus and screens (pause, options, roulette setup) are drawn for 4:3: "
                                              "shown 4:3 with side bars, or stretched to the screen.");
                        ImGui::BeginDisabled(lower(aspect) == "4:3");
                        iniCombo(state, "##menus", "Video", "MenuLayout", "4:3",
                                 [](const char *id, std::string &value) { return choiceCombo(id, kMenuLayouts, value); });
                        ImGui::EndDisabled();
                        settingLabel("Field of view", "How much the gameplay camera shows, both ways (widescreen widens "
                                                      "it sideways on its own). Cutscenes keep their framing; enemies "
                                                      "behave the same either way.");
                        iniCombo(state, "##fov", "Video", "FieldOfView", "100",
                                 [](const char *id, std::string &value) { return choiceCombo(id, kFovs, value); });
                        settingLabel("Field of view in fights", "The same while the game's battle camera is on (Gene "
                                                                "has an enemy targeted). 100% frames fights as on the PS2.");
                        iniCombo(state, "##fightfov", "Video", "FightFieldOfView", "100",
                                 [](const char *id, std::string &value) { return choiceCombo(id, kFovs, value); });
                        settingLabel("Camera distance", "At a wider field of view, how far the camera comes in toward "
                                                        "Gene. Coming all the way in keeps him the size he is at 100%.");
                        iniCombo(state, "##camdist", "Video", "CameraDistance", "100",
                                 [](const char *id, std::string &value) { return choiceCombo(id, kCameraDistances, value); });
                        settingLabel("Camera tilt", "At a wider field of view, the camera also rises and looks down at "
                                                    "Gene by this much: more of the floor ahead, less sky.");
                        iniCombo(state, "##camtilt", "Video", "CameraTilt", "0",
                                 [](const char *id, std::string &value) { return choiceCombo(id, kCameraTilts, value); });
                        settingLabel("Window", "F11 switches between window and fullscreen while playing.");
                        {
                            std::string mode = state.ini.getBool("Video", "StartFullscreen", false)  ? "fullscreen"
                                               : state.ini.getBool("Video", "StartMaximized", false) ? "maximized"
                                                                                                     : "window";
                            if (choiceCombo("##window", kWindowModes, mode))
                            {
                                state.ini.setBool("Video", "StartFullscreen", mode == "fullscreen");
                                state.ini.setBool("Video", "StartMaximized", mode == "maximized");
                            }
                        }
                        settingLabel("Window size", "The window's size when it opens (not maximized or fullscreen).");
                        iniCombo(state, "##size", "Video", "WindowSize", "auto",
                                 [](const char *id, std::string &value) { return choiceCombo(id, kWindowSizes, value); });
                        ImGui::EndTable();
                    }
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Graphics"))
                {
                    if (beginSettings("graphics"))
                    {
                        settingLabel("Internal resolution", "How sharp the 3D is drawn. 3x suits a 1440p screen, 4x and "
                                                            "up a 4K one; higher costs more GPU time.");
                        iniCombo(state, "##scale", "Video", "InternalResolution", "3",
                                 [](const char *id, std::string &value) { return choiceCombo(id, kResolutions, value); });
                        settingLabel("Post effects", "The blur effects (depth of field, heat haze) are drawn from small "
                                                     "copies of the frame. PS2 resolution keeps them aligned but grainy "
                                                     "at high internal resolutions; Upscaled draws them sharp.");
                        iniCombo(state, "##posteffects", "Video", "PostEffects", "ps2",
                                 [](const char *id, std::string &value) { return choiceCombo(id, kPostEffects, value); });
                        settingLabel("HD texture pack", "Replacement textures in PCSX2's format.");
                        iniCheckbox(state, "Use replacement textures##tex", "Runtime", "PS2X_PCSX2_TEXTURES", true);
                        settingLabel("Texture pack folder", "The folder that holds replacements/. Empty: textures/SLUS-21503 "
                                                            "next to the game.");
                        {
                            const std::string folder = state.ini.get("Runtime", "PS2X_PCSX2_TEXTURE_DIR");
                            ImGui::TextWrapped("%s", folder.empty() ? "(next to the game)" : folder.c_str());
                            if (ImGui::Button("Choose folder..."))
                            {
                                const fs::path picked = platform::pick(GetWindowHandle(), true);
                                if (!picked.empty())
                                    state.ini.set("Runtime", "PS2X_PCSX2_TEXTURE_DIR", utf8(picked));
                            }
                            if (!folder.empty())
                            {
                                ImGui::SameLine();
                                if (ImGui::Button("Use default"))
                                    state.ini.set("Runtime", "PS2X_PCSX2_TEXTURE_DIR", "");
                            }
                        }
                        ImGui::EndTable();
                    }
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Gameplay"))
                {
                    if (beginSettings("gameplay"))
                    {
                        settingLabel("60 FPS", "Runs the game at 60 frames a second instead of 30, with its timers kept "
                                               "at the PS2's speed.");
                        iniCheckbox(state, "On##fps60", "Game", "Fps60", false);
                        settingLabel("L3 dash", "Clicking the left stick dashes (a quick double tap forward).");
                        iniCheckbox(state, "On##l3", "Game", "L3Dash", false);
                        settingLabel("Roulette timer", "How long you have to pick a roulette move. Double time gives "
                                                       "twice as long (the dial runs half as fast), at 30 and 60 FPS.");
                        iniCombo(state, "##roulette", "Game", "RouletteTimer", "normal",
                                 [](const char *id, std::string &value) { return choiceCombo(id, kRouletteTimers, value); });
                        settingLabel("Unlock Hard", "Hard can be picked from the start instead of after a clear. Your "
                                                    "save is not changed.");
                        iniCheckbox(state, "On##hard", "Game", "UnlockHard", false);
                        settingLabel("Mute music", "Stage, boss, menu and minigame music plays silent. Cutscenes keep "
                                                   "their sound (their music and voices are one track).");
                        iniCheckbox(state, "On##music", "Sound", "MuteMusic", false);
                        ImGui::EndTable();
                    }
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Mods"))
                {
                    if (beginSettings("mods"))
                    {
                        settingLabel("Character", "Who you play as. Other characters use Gene's moves and his God Hand "
                                                  "arm.");
                        static std::vector<characters::Character> list;
                        if (list.empty())
                            list = characters::list(state.context->exeDir);
                        const std::string current = lower(state.ini.get("Mods", "Character", "gene"));
                        int picked = 0;
                        std::vector<const char *> names;
                        for (size_t i = 0; i < list.size(); ++i)
                        {
                            names.push_back(list[i].name.c_str());
                            if (lower(list[i].id) == current)
                                picked = static_cast<int>(i);
                        }
                        ImGui::SetNextItemWidth(-1.0f);
                        if (ImGui::Combo("##character", &picked, names.data(), static_cast<int>(names.size())))
                            state.ini.set("Mods", "Character", list[static_cast<size_t>(picked)].id);
                        if (!list[static_cast<size_t>(picked)].credit.empty())
                            ImGui::TextColored(kTextDim, "%s", list[static_cast<size_t>(picked)].credit.c_str());
                        ImGui::EndTable();
                    }
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Controls"))
                {
                    ImGui::TextWrapped("Controllers (Xbox, PlayStation, Steam and others) work without setup, and the "
                                       "button prompts in the game show your controller's buttons.");
                    ImGui::Spacing();
                    ImGui::TextWrapped("Keyboard and controller bindings are kept in controls.ini next to the game. "
                                       "Remapping them on this screen is coming.");
                    ImGui::Spacing();
                    if (ImGui::Button("Open controls.ini"))
                        platform::open(state.context->exeDir / "controls.ini");
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
            ImGui::EndChild();
            ImGui::Dummy(ImVec2(0.0f, fontSize(state, 6.0f)));
            const float backWidth = fontSize(state, 200.0f);
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - backWidth) * 0.5f);
            const bool back = ImGui::Button("Back", ImVec2(backWidth, 0.0f));
            if (state.focusFirst)
                ImGui::SetItemDefaultFocus();
            const bool cancel = !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) &&
                                (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false));
            ImGui::PopFont();
            if (back || cancel)
            {
                state.ini.save();
                state.screen = Screen::Home;
                state.focusFirst = true;
                return;
            }
            state.focusFirst = false;
        }
    }

    bool wanted(bool haveImage)
    {
        if (!haveImage || platform::shiftHeld())
            return true;
        const char *value = std::getenv("GODHAND_LAUNCHER");
        return !(value && *value == '0');
    }

    Result run(const Context &context)
    {
        State state;
        state.context = &context;
        if (!state.ini.load(context.iniPath))
            GH_WARN(Boot, "launcher: cannot read " << utf8(context.iniPath));
        std::error_code error;
        state.installed = !context.iso.empty() && fs::is_regular_file(context.iso, error) && context.message.empty();
        if (!context.iso.empty())
            std::snprintf(state.isoText, sizeof(state.isoText), "%s", utf8(context.iso).c_str());
        if (!context.message.empty())
        {
            state.installMessage = context.message;
            state.installError = true;
            state.screen = Screen::Install;
        }

        ps2_host_display::openHostWindow("God Hand: Rearmed");
        if (!ps2_host_display::vsyncEnabled())
            SetTargetFPS(60);
        rlImGuiSetup(true);
        ImGuiIO &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
        const fs::path fontPath = context.exeDir / "resources" / "fonts" / "Roboto-Regular.ttf";
        if (fs::is_regular_file(fontPath, error))
            state.font = io.Fonts->AddFontFromFileTTF(utf8(fontPath).c_str(), 20.0f);
        applyStyle();
        const fs::path logoPath = context.exeDir / "resources" / "images" / "logo.png";
        if (fs::is_regular_file(logoPath, error))
        {
            state.logo = LoadTexture(utf8(logoPath).c_str());
            if (state.logo.id != 0u)
            {
                GenTextureMipmaps(&state.logo);
                SetTextureFilter(state.logo, TEXTURE_FILTER_TRILINEAR);
            }
        }
        GH_INFO(Boot, "launcher: shown (" << (state.installed ? "installed" : "not installed") << ")");

        while (!state.done)
        {
            if (WindowShouldClose())
            {
                state.result = Result::Quit;
                break;
            }
            state.unit = std::clamp(static_cast<float>(GetScreenHeight()) / 900.0f, 0.55f, 3.0f);
            feedGamepads(io);
            BeginDrawing();
            drawBackground();
            rlImGuiBegin();
            ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
            ImGui::SetNextWindowSize(io.DisplaySize);
            if (ImGui::Begin("##launcher", nullptr,
                             ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoBringToFrontOnFocus))
            {
                switch (state.screen)
                {
                case Screen::Home:
                    drawHome(state);
                    break;
                case Screen::Install:
                    drawInstall(state);
                    break;
                case Screen::Options:
                    drawOptions(state);
                    break;
                }
            }
            ImGui::End();
            rlImGuiEnd();
            EndDrawing();
        }
        state.ini.save();
        if (state.logo.id != 0u)
            UnloadTexture(state.logo);
        rlImGuiShutdown(); // the F12 debugger sets ImGui up again later
        // One empty frame, so the window does not keep showing the launcher until the game's first picture.
        BeginDrawing();
        ClearBackground(BLACK);
        EndDrawing();
        GH_INFO(Boot, "launcher: " << (state.result == Result::Play ? "play" : "quit"));
        return state.result;
    }
}
