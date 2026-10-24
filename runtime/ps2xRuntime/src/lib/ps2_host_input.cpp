#include "ps2_host_input.h"
#include "runtime/ps2_input_options.h"
#include "runtime/ps2_save_state.h"
#include "ps2_host_backend.h"
#include "ps2_host_gamepads.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <sstream>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#if defined(_WIN32)
#include <io.h> // _commit: the controls file reaches the disk before the replacing rename
#else
#include <unistd.h>
#endif

// raylib reports Caps Lock and Num Lock as held for as long as the lock light is on (GLFW_LOCK_KEY_MODS,
// rcore_desktop_glfw.c KeyCallback), so a button bound to them stayed held. Sweep 2026-10-17 CR-1: read the physical
// key instead. (The first version passed raylib's GetWindowHandle() - an HWND on Windows - to glfwGetKey, which expects a
// GLFWwindow*: undefined behaviour. Replaced by the Win32 async key state, gated by window focus; no windows.h here
// because its macros clash with raylib's names.)
#if defined(_WIN32)
extern "C" __declspec(dllimport) short __stdcall GetAsyncKeyState(int vKey);
// Sweep CR-9 (accessibility shortcut keys, see "Windows accessibility shortcut keys" below), same reason for no windows.h.
struct _EXCEPTION_POINTERS;
using Ps2xTopLevelExceptionFilter = long(__stdcall *)(_EXCEPTION_POINTERS *);
extern "C" __declspec(dllimport) int __stdcall SystemParametersInfoW(unsigned int action, unsigned int param, void *data, unsigned int winIni);
extern "C" __declspec(dllimport) void *__stdcall GetForegroundWindow();
extern "C" __declspec(dllimport) Ps2xTopLevelExceptionFilter __stdcall SetUnhandledExceptionFilter(Ps2xTopLevelExceptionFilter filter);
extern "C" __declspec(dllimport) int __stdcall SetConsoleCtrlHandler(int(__stdcall *handler)(unsigned long ctrlType), int add);
#endif

namespace ps2_host_input
{
    namespace
    {
        bool lockKeyDown(int code)
        {
#if defined(_WIN32)
            if (!IsWindowFocused())
                return false;
            const int vk = code == KEY_CAPS_LOCK ? 0x14 /* VK_CAPITAL */ : code == KEY_NUM_LOCK ? 0x90 /* VK_NUMLOCK */ : 0;
            return vk != 0 && (GetAsyncKeyState(vk) & 0x8000) != 0;
#else
            return IsKeyDown(code);
#endif
        }

        using ps2x::iop::PadInputState;
        using ps2x::iop::PadPressure;
        namespace PadButton = ps2x::iop::PadButton;

        // Controller bindings in PCSX2's syntax (controls.ini [Pad1]-[Pad8], the [PadN] sections of PCSX2.ini): each
        // PS2 button or stick direction lists "<source>/<element>" bindings, any of which presses it; "a & b" needs
        // both. urban.exe turns the file into PS2X_PAD<n>=1 plus PS2X_PAD<n>_<KEY>=<binding>;<binding>... variables.
        enum class Source
        {
            Keyboard,
            Pointer,
            PadButton,
            PadAxis
        };

        struct Element
        {
            Source source = Source::Keyboard;
            int device = 0;
            int code = 0;
            float sign = 1.0f; // PadAxis: +1 = the positive half (the whole range of a trigger), -1 = the negative half
        };

        using Chord = std::vector<Element>;    // all held ("&")
        using Bindings = std::vector<Chord>;   // any of them

        // Keyboard codes for "Shift"/"Control"/"Alt" (either side, as PCSX2 names them on Windows).
        constexpr int kAnyShift = -1;
        constexpr int kAnyControl = -2;
        constexpr int kAnyAlt = -3;

        constexpr std::array<std::pair<std::string_view, int>, 43> kKeyNames{{
            {"escape", KEY_ESCAPE}, {"tab", KEY_TAB}, {"backspace", KEY_BACKSPACE}, {"return", KEY_ENTER},
            {"enter", KEY_KP_ENTER}, {"insert", KEY_INSERT}, {"delete", KEY_DELETE}, {"pause", KEY_PAUSE},
            {"home", KEY_HOME}, {"end", KEY_END}, {"left", KEY_LEFT}, {"up", KEY_UP}, {"right", KEY_RIGHT},
            {"down", KEY_DOWN}, {"pageup", KEY_PAGE_UP}, {"pagedown", KEY_PAGE_DOWN}, {"shift", kAnyShift},
            {"control", kAnyControl}, {"alt", kAnyAlt}, {"capslock", KEY_CAPS_LOCK}, {"space", KEY_SPACE},
            {"comma", KEY_COMMA}, {"minus", KEY_MINUS}, {"period", KEY_PERIOD}, {"slash", KEY_SLASH},
            {"semicolon", KEY_SEMICOLON}, {"equal", KEY_EQUAL}, {"bracketleft", KEY_LEFT_BRACKET},
            {"backslash", KEY_BACKSLASH}, {"bracketright", KEY_RIGHT_BRACKET}, {"apostrophe", KEY_APOSTROPHE},
            {"quoteleft", KEY_GRAVE}, {"leftshift", KEY_LEFT_SHIFT}, {"rightshift", KEY_RIGHT_SHIFT},
            {"leftcontrol", KEY_LEFT_CONTROL}, {"rightcontrol", KEY_RIGHT_CONTROL}, {"leftalt", KEY_LEFT_ALT},
            {"rightalt", KEY_RIGHT_ALT}, {"numlock", KEY_NUM_LOCK}, {"scrolllock", KEY_SCROLL_LOCK},
            {"print", KEY_PRINT_SCREEN}, {"menu", KEY_KB_MENU}, {"keypad+enter", KEY_KP_ENTER},
        }};

        // SDL element names as PCSX2 writes them (SDLInputSource), in SDL's button order.
        constexpr std::array<std::string_view, ps2_host_gamepads::ButtonCount> kSdlButtonNames{
            "FaceSouth", "FaceEast", "FaceWest", "FaceNorth", "Back", "Guide", "Start", "LeftStick", "RightStick",
            "LeftShoulder", "RightShoulder", "DPadUp", "DPadDown", "DPadLeft", "DPadRight", "Misc1", "Paddle1",
            "Paddle2", "Paddle3", "Paddle4", "Touchpad"};
        constexpr std::array<std::string_view, ps2_host_gamepads::AxisCount> kSdlAxisNames{
            "LeftX", "LeftY", "RightX", "RightY", "LeftTrigger", "RightTrigger"};

        std::string lowerName(std::string_view text)
        {
            std::string name(text);
            for (char &ch : name)
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            return name;
        }

        std::string_view trimView(std::string_view text)
        {
            while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
                text.remove_prefix(1u);
            while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
                text.remove_suffix(1u);
            return text;
        }

        std::optional<int> keyCode(std::string_view name)
        {
            const std::string key = lowerName(name);
            if (key.size() == 1u && key[0] >= 'a' && key[0] <= 'z')
                return KEY_A + (key[0] - 'a');
            if (key.size() == 1u && key[0] >= '0' && key[0] <= '9')
                return KEY_ZERO + (key[0] - '0');
            // F2..F12 ("F1x" is not F1: every character after the F must be a digit). F1 is kept for the Controls
            // screen (ps2_controls_screen.cpp), so a binding to it would be unusable.
            if (key.size() >= 2u && key.size() <= 3u && key[0] == 'f' &&
                std::all_of(key.begin() + 1, key.end(), [](char ch) { return std::isdigit(static_cast<unsigned char>(ch)) != 0; }))
            {
                const int number = std::atoi(key.c_str() + 1);
                if (number >= 2 && number <= 12)
                    return KEY_F1 + number - 1;
                return std::nullopt;
            }
            if (key.size() == 8u && key.rfind("keypad+", 0) == 0 && key[7] >= '0' && key[7] <= '9')
                return KEY_KP_0 + (key[7] - '0');
            for (const auto &[keyName, code] : kKeyNames)
                if (key == keyName)
                    return code;
            return std::nullopt;
        }

        // "Keyboard/W", "Pointer-0/LeftButton", "SDL-0/FaceSouth", "SDL-0/+LeftTrigger", "SDL-1/-LeftY".
        std::optional<Element> parseElement(std::string_view text)
        {
            text = trimView(text);
            const size_t slash = text.find('/');
            if (slash == std::string_view::npos)
                return std::nullopt;
            const std::string source = lowerName(text.substr(0, slash));
            std::string_view name = text.substr(slash + 1u);
            Element element;
            if (source == "keyboard")
            {
                const auto code = keyCode(name);
                if (!code)
                    return std::nullopt;
                element.source = Source::Keyboard;
                element.code = *code;
                return element;
            }
            if (source.rfind("pointer", 0) == 0)
            {
                const std::string button = lowerName(name);
                element.source = Source::Pointer;
                if (button == "leftbutton")
                    element.code = MOUSE_BUTTON_LEFT;
                else if (button == "rightbutton")
                    element.code = MOUSE_BUTTON_RIGHT;
                else if (button == "middlebutton")
                    element.code = MOUSE_BUTTON_MIDDLE;
                else
                    return std::nullopt;
                return element;
            }
            if (source.rfind("sdl-", 0) != 0)
                return std::nullopt;
            element.device = std::atoi(source.c_str() + 4);
            if (element.device < 0 || element.device >= ps2_host_gamepads::kMaxDevices)
                return std::nullopt;
            if (!name.empty() && (name[0] == '+' || name[0] == '-'))
            {
                element.source = Source::PadAxis;
                element.sign = name[0] == '-' ? -1.0f : 1.0f;
                name.remove_prefix(1u);
                for (size_t i = 0; i < kSdlAxisNames.size(); ++i)
                {
                    if (lowerName(name) == lowerName(kSdlAxisNames[i]))
                    {
                        element.code = static_cast<int>(i);
                        return element;
                    }
                }
                return std::nullopt;
            }
            element.source = Source::PadButton;
            for (size_t i = 0; i < kSdlButtonNames.size(); ++i)
            {
                if (lowerName(name) == lowerName(kSdlButtonNames[i]))
                {
                    element.code = static_cast<int>(i);
                    return element;
                }
            }
            return std::nullopt;
        }

        std::vector<std::string_view> splitList(std::string_view text, char separator)
        {
            std::vector<std::string_view> parts;
            size_t begin = 0u;
            while (begin <= text.size())
            {
                const size_t end = std::min(text.find(separator, begin), text.size());
                if (const std::string_view part = trimView(text.substr(begin, end - begin)); !part.empty())
                    parts.push_back(part);
                begin = end + 1u;
            }
            return parts;
        }

        // PCSX2's [PadN] keys for the 16 digital buttons and the 8 stick directions.
        constexpr std::array<std::pair<std::string_view, uint16_t>, 16> kPadButtonKeys{{
            {"Up", PadButton::Up}, {"Right", PadButton::Right}, {"Down", PadButton::Down}, {"Left", PadButton::Left},
            {"Triangle", PadButton::Triangle}, {"Circle", PadButton::Circle}, {"Cross", PadButton::Cross},
            {"Square", PadButton::Square}, {"Select", PadButton::Select}, {"Start", PadButton::Start},
            {"L1", PadButton::L1}, {"L2", PadButton::L2}, {"R1", PadButton::R1}, {"R2", PadButton::R2},
            {"L3", PadButton::L3}, {"R3", PadButton::R3},
        }};
        enum StickHalf
        {
            LUp,
            LRight,
            LDown,
            LLeft,
            RUp,
            RRight,
            RDown,
            RLeft,
            StickHalfCount
        };
        constexpr std::array<std::string_view, StickHalfCount> kStickKeys{"LUp", "LRight", "LDown", "LLeft",
                                                                          "RUp", "RRight", "RDown", "RLeft"};

        struct PadConfig
        {
            std::array<Bindings, kPadButtonKeys.size()> buttons;
            std::array<Bindings, StickHalfCount> sticks;
            std::vector<int> largeMotor; // devices that play each motor
            std::vector<int> smallMotor;
            bool enabled = true;         // Type = None: no controller on this port/slot
            float deadzone = 0.0f;
            float axisScale = 1.33f;
            float buttonDeadzone = 0.0f;
            float largeMotorScale = 1.0f;
            float smallMotorScale = 1.0f;
            uint32_t invertL = 0u; // 1 = X, 2 = Y
            uint32_t invertR = 0u;
        };

        // Fills `config` from one [PadN] key (PCSX2's name, any case) and its bindings separated by ';'.
        void applyPadSetting(PadConfig &config, uint32_t pad, std::string_view key, std::string_view value)
        {
            const std::string name = lowerName(key);
            auto number = [&](float fallback)
            {
                char *end = nullptr;
                const std::string text(trimView(value));
                const float parsed = std::strtof(text.c_str(), &end);
                return end != text.c_str() && std::isfinite(parsed) ? parsed : fallback;
            };
            auto bindings = [&]()
            {
                Bindings list;
                for (std::string_view entry : splitList(value, ';'))
                {
                    Chord chord;
                    for (std::string_view part : splitList(entry, '&'))
                    {
                        if (const auto element = parseElement(part))
                            chord.push_back(*element);
                        else
                        {
                            std::cerr << "[input] Pad" << pad + 1u << " " << key << ": unknown binding '" << part << "'" << std::endl;
                            chord.clear();
                            break;
                        }
                    }
                    if (!chord.empty())
                        list.push_back(std::move(chord));
                }
                return list;
            };
            for (size_t i = 0; i < kPadButtonKeys.size(); ++i)
                if (name == lowerName(kPadButtonKeys[i].first))
                    return void(config.buttons[i] = bindings());
            for (size_t i = 0; i < kStickKeys.size(); ++i)
                if (name == lowerName(kStickKeys[i]))
                    return void(config.sticks[i] = bindings());
            if (name == "largemotor" || name == "smallmotor")
            {
                std::vector<int> &devices = name == "largemotor" ? config.largeMotor : config.smallMotor;
                devices.clear();
                for (std::string_view entry : splitList(value, ';'))
                {
                    const std::string text = lowerName(entry);
                    if (text.rfind("sdl-", 0) == 0)
                        devices.push_back(std::atoi(text.c_str() + 4));
                }
            }
            else if (name == "type")
                config.enabled = lowerName(trimView(value)) != "none";
            else if (name == "deadzone")
                config.deadzone = std::clamp(number(0.0f), 0.0f, 0.95f);
            else if (name == "axisscale")
                config.axisScale = std::clamp(number(1.33f), 0.01f, 4.0f);
            else if (name == "buttondeadzone")
                config.buttonDeadzone = std::clamp(number(0.0f), 0.0f, 0.95f);
            else if (name == "largemotorscale")
                config.largeMotorScale = std::max(number(1.0f), 0.0f);
            else if (name == "smallmotorscale")
                config.smallMotorScale = std::max(number(1.0f), 0.0f);
            else if (name == "invertl")
                config.invertL = static_cast<uint32_t>(number(0.0f)) & 3u;
            else if (name == "invertr")
                config.invertR = static_cast<uint32_t>(number(0.0f)) & 3u;
            // Other PCSX2 keys (Analog, PressureModifier, ...) are accepted and ignored.
        }

        // ---- bindings: controls file, environment, defaults ----------------------------------------------------------
        // The 24 binding keys of a [PadN] section (16 buttons, then the 8 stick halves); every other key is a setting.
        constexpr size_t kBindingKeyCount = kPadButtonKeys.size() + kStickKeys.size();

        std::string_view bindingKeyName(size_t index)
        {
            return index < kPadButtonKeys.size() ? kPadButtonKeys[index].first : kStickKeys[index - kPadButtonKeys.size()];
        }

        std::optional<size_t> bindingKeyIndex(std::string_view key)
        {
            const std::string name = lowerName(trimView(key));
            for (size_t i = 0; i < kBindingKeyCount; ++i)
                if (name == lowerName(bindingKeyName(i)))
                    return i;
            return std::nullopt;
        }

        // One [PadN] section as the file holds it: the settings in file order and each key's bindings, one per line.
        struct PadSection
        {
            bool present = false;
            std::vector<std::pair<std::string, std::string>> settings;
            std::array<std::vector<std::string>, kBindingKeyCount> bindings;
            std::vector<std::string> comments; // '#' / ';' lines, written back right after the [PadN] header
        };

        struct ControlsModel
        {
            std::array<PadSection, kMaxPlayers> pads;
            std::string preamble;  // the lines before the first [section] (the header comment), kept as written
            std::string otherText; // sections other than [Pad1]-[Pad8], kept as written
        };

        void addSectionLine(PadSection &section, std::string_view key, std::string_view value)
        {
            key = trimView(key);
            if (const auto index = bindingKeyIndex(key))
            {
                for (std::string_view part : splitList(value, ';'))
                    section.bindings[*index].emplace_back(part);
                return;
            }
            const std::string name = lowerName(key);
            const bool motor = name == "largemotor" || name == "smallmotor"; // PCSX2 repeats these, one per controller
            for (auto &[otherKey, otherValue] : section.settings)
                if (lowerName(otherKey) == name)
                {
                    const std::string_view added = trimView(value);
                    if (motor && !otherValue.empty() && !added.empty())
                        otherValue += ";" + std::string(added);
                    else
                        otherValue = std::string(added);
                    return;
                }
            section.settings.emplace_back(std::string(key), std::string(trimView(value)));
        }

        PadSection sectionFromSettings(const std::vector<std::pair<std::string, std::string>> &lines)
        {
            PadSection section;
            section.present = true;
            for (const auto &[key, value] : lines)
                addSectionLine(section, key, value);
            return section;
        }

        PadSection defaultSection(uint32_t pad)
        {
            return sectionFromSettings(defaultPadSettings(pad));
        }

        PadConfig configFromSection(const PadSection &section, uint32_t pad)
        {
            PadConfig config;
            for (const auto &[key, value] : section.settings)
                applyPadSetting(config, pad, key, value);
            for (size_t i = 0; i < kBindingKeyCount; ++i)
            {
                std::string joined;
                for (const std::string &binding : section.bindings[i])
                    joined += (joined.empty() ? "" : ";") + binding;
                applyPadSetting(config, pad, bindingKeyName(i), joined);
            }
            return config;
        }

        // [PadN] from the PS2X_PAD<n>_* variables (tests set PS2X_PAD<n>=1 in the real environment).
        PadConfig envPadConfig(uint32_t pad)
        {
            PadConfig config;
            const std::string prefix = "PS2X_PAD" + std::to_string(pad + 1u);
            constexpr std::string_view kSettingKeys[] = {"Type", "Deadzone", "AxisScale", "ButtonDeadzone",
                                                         "LargeMotorScale", "SmallMotorScale", "InvertL", "InvertR",
                                                         "LargeMotor", "SmallMotor"};
            auto read = [&](std::string_view key)
            {
                std::string variable = prefix + "_";
                for (char ch : key)
                    variable += static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                if (const char *value = std::getenv(variable.c_str()))
                    applyPadSetting(config, pad, key, value);
            };
            for (std::string_view key : kSettingKeys)
                read(key);
            for (const auto &[key, button] : kPadButtonKeys)
                read(key);
            for (std::string_view key : kStickKeys)
                read(key);
            return config;
        }

        bool envPad(uint32_t pad)
        {
            const std::string variable = "PS2X_PAD" + std::to_string(pad + 1u);
            return std::getenv(variable.c_str()) != nullptr;
        }

        // Window thread only: sample() (the only reader of the tables), the Controls screen and setControlsFile (before
        // the run loop) all run there, so the tables need no lock.
        ControlsModel g_model;                    // the file's content (or the defaults without a file)
        std::string g_controlsPath;               // empty: no file, edits last until the game closes
        bool g_controlsDirty = false;             // edited since the last save
        bool g_configsValid = false;
        std::array<PadConfig, kMaxPlayers> g_configs;
        std::filesystem::file_time_type g_loadedTime{};
        uintmax_t g_loadedSize = 0u;
        bool g_haveLoadedStamp = false;
        std::chrono::steady_clock::time_point g_lastFileCheck{};
        std::string g_lastFileError;
        std::atomic<bool> g_overlayOpen{false};

        // ---- button prompts (prompts:: in the header) ------------------------------------------------------------------
        // Player 1's labels for the game's PS2 button art (God Hand: the text icon sheet, texture 0x18 of core_usa.bin
        // drawn by func_002B1300, and the action prompt sheet, texture 0x0F; src/godhand/button_prompts.cpp redraws
        // them in guest RAM). Written on the window thread (rebuildConfigs, readPort); read from any thread, the EE
        // thread's vblank hook included: atomics, plus g_promptMutex held only for the ~1 KB copy.
        std::mutex g_promptMutex;
        prompts::Snapshot g_prompts; // g_promptMutex
        std::atomic<uint32_t> g_promptGeneration{0u};
        std::atomic<prompts::Device> g_promptDevice{prompts::Device::Keys};
        std::atomic<prompts::PadStyle> g_promptStyle{prompts::PadStyle::Unknown};
        // Last-device detector (window thread). Bit = binding key index (the 16 buttons, then the 8 stick halves).
        uint32_t g_promptKeysHeld = 0u; // held past half by a key or mouse button binding
        uint32_t g_promptPadsHeld = 0u; // held past half by a controller binding
        int g_promptPad = -1;           // the controller that last pressed a bound input (-1: none yet)
        int g_promptBoundPad = 0;       // player 1's first controller binding's controller (the style before any press)
        std::chrono::steady_clock::time_point g_promptSwitchAt{};
        constexpr auto kPromptSwitchDelay = std::chrono::milliseconds(500); // Tokyo Jungle's PAD_PROMPT_SWITCH_MS
        static_assert(prompts::kKeyAnyShift == kAnyShift && prompts::kKeyAnyControl == kAnyControl && prompts::kKeyAnyAlt == kAnyAlt);
        static_assert(kBindingKeyCount <= 32u, "the held masks have one bit per binding key");

        struct PromptName
        {
            int code;
            std::string_view label;
            std::string_view compact; // empty: same as the label
        };

        // Short key names after Tokyo Jungle's pad_prompt_key_names (pad_prompt_art.c), keyed by raylib code: every key
        // keyCode() can return besides letters, digits, F-keys and keypad digits.
        constexpr PromptName kPromptKeyNames[] = {
            {KEY_ESCAPE, "Esc", ""}, {KEY_TAB, "Tab", ""}, {KEY_BACKSPACE, "Bksp", ""}, {KEY_ENTER, "Enter", "Ent"},
            {KEY_KP_ENTER, "NumEnt", "NEnt"}, {KEY_INSERT, "Ins", ""}, {KEY_DELETE, "Del", ""}, {KEY_PAUSE, "Pause", "Paus"},
            {KEY_HOME, "Home", "Hm"}, {KEY_END, "End", ""}, {KEY_LEFT, "\xE2\x86\x90", ""}, {KEY_UP, "\xE2\x86\x91", ""},
            {KEY_RIGHT, "\xE2\x86\x92", ""}, {KEY_DOWN, "\xE2\x86\x93", ""}, {KEY_PAGE_UP, "PgUp", "PU"},
            {KEY_PAGE_DOWN, "PgDn", "PD"}, {kAnyShift, "Shift", "Shft"}, {kAnyControl, "Ctrl", ""}, {kAnyAlt, "Alt", ""},
            {KEY_CAPS_LOCK, "Caps", ""}, {KEY_SPACE, "Space", "Spc"}, {KEY_COMMA, ",", ""}, {KEY_MINUS, "-", ""},
            {KEY_PERIOD, ".", ""}, {KEY_SLASH, "/", ""}, {KEY_SEMICOLON, ";", ""}, {KEY_EQUAL, "=", ""},
            {KEY_LEFT_BRACKET, "[", ""}, {KEY_BACKSLASH, "\\", ""}, {KEY_RIGHT_BRACKET, "]", ""}, {KEY_APOSTROPHE, "'", ""},
            {KEY_GRAVE, "`", ""}, {KEY_LEFT_SHIFT, "Shift", "Shft"}, {KEY_RIGHT_SHIFT, "RShift", "RShf"},
            {KEY_LEFT_CONTROL, "Ctrl", ""}, {KEY_RIGHT_CONTROL, "RCtrl", "RCtl"}, {KEY_LEFT_ALT, "Alt", ""},
            {KEY_RIGHT_ALT, "RAlt", ""}, {KEY_NUM_LOCK, "NumLk", "NmLk"}, {KEY_SCROLL_LOCK, "ScrLk", "ScLk"},
            {KEY_PRINT_SCREEN, "PrtSc", "Prt"}, {KEY_KB_MENU, "Menu", ""},
        };

        // Xbox-style names in ps2_host_gamepads::Button order (the layout GLFW and SDL report every controller with).
        // Paddles as on an Elite pad: SDL's right paddle 1/2 are P1/P2, left 1/2 are P3/P4.
        constexpr std::array<std::string_view, ps2_host_gamepads::ButtonCount> kPromptPadButtons{
            "A", "B", "X", "Y", "View", "Guide", "Menu", "LS", "RS", "LB", "RB", "D\xE2\x86\x91", "D\xE2\x86\x93",
            "D\xE2\x86\x90", "D\xE2\x86\x92", "Misc", "P1", "P3", "P2", "P4", "Touch"};
        // Per ps2_host_gamepads::Axis: the negative half, then the positive half.
        constexpr std::array<std::string_view, ps2_host_gamepads::AxisCount * 2> kPromptPadAxes{
            "LS\xE2\x86\x90", "LS\xE2\x86\x92", "LS\xE2\x86\x91", "LS\xE2\x86\x93", "RS\xE2\x86\x90", "RS\xE2\x86\x92",
            "RS\xE2\x86\x91", "RS\xE2\x86\x93", "LT-", "LT", "RT-", "RT"};

        void copyLabel(char (&out)[prompts::kLabelBytes], std::string_view text)
        {
            size_t size = std::min(text.size(), prompts::kLabelBytes - 1u);
            while (size > 0u && size < text.size() && (static_cast<unsigned char>(text[size]) & 0xC0u) == 0x80u)
                --size; // never cut a UTF-8 character in two
            std::fill(std::begin(out), std::end(out), '\0');
            std::copy_n(text.data(), size, out);
        }

        prompts::Binding promptBinding(const Element &element)
        {
            prompts::Binding binding;
            binding.code = static_cast<int16_t>(element.code);
            char full[prompts::kLabelBytes]{};
            char small[prompts::kLabelBytes]{};
            std::string_view label = "?";
            std::string_view compact;
            switch (element.source)
            {
            case Source::Keyboard:
                binding.type = prompts::Type::Key;
                if (element.code >= KEY_A && element.code <= KEY_Z)
                    std::snprintf(full, sizeof(full), "%c", 'A' + (element.code - KEY_A));
                else if (element.code >= KEY_ZERO && element.code <= KEY_NINE)
                    std::snprintf(full, sizeof(full), "%c", '0' + (element.code - KEY_ZERO));
                else if (element.code >= KEY_F1 && element.code <= KEY_F12)
                    std::snprintf(full, sizeof(full), "F%d", element.code - KEY_F1 + 1);
                else if (element.code >= KEY_KP_0 && element.code <= KEY_KP_9)
                {
                    std::snprintf(full, sizeof(full), "Num%d", element.code - KEY_KP_0);
                    std::snprintf(small, sizeof(small), "N%d", element.code - KEY_KP_0);
                    compact = small;
                }
                else
                    for (const PromptName &name : kPromptKeyNames)
                        if (name.code == element.code)
                        {
                            label = name.label;
                            compact = name.compact;
                            break;
                        }
                if (full[0] != '\0')
                    label = full;
                break;
            case Source::Pointer:
                binding.type = prompts::Type::Mouse;
                label = element.code == MOUSE_BUTTON_LEFT ? "M1" : element.code == MOUSE_BUTTON_RIGHT ? "M2" : "M3";
                break;
            case Source::PadButton:
                binding.type = prompts::Type::PadButton;
                binding.device = static_cast<int8_t>(element.device);
                label = kPromptPadButtons[static_cast<size_t>(element.code)];
                if (element.code == ps2_host_gamepads::Guide)
                    compact = "Gde"; // the two names over the compact limit of 4 characters
                else if (element.code == ps2_host_gamepads::Touchpad)
                    compact = "TPad";
                break;
            case Source::PadAxis:
                binding.type = prompts::Type::PadAxis;
                binding.device = static_cast<int8_t>(element.device);
                binding.sign = element.sign < 0.0f ? int8_t{-1} : int8_t{1};
                label = kPromptPadAxes[static_cast<size_t>(element.code) * 2u + (element.sign < 0.0f ? 0u : 1u)];
                break;
            }
            copyLabel(binding.label, label);
            copyLabel(binding.compact, compact.empty() ? label : compact);
            return binding;
        }

        bool samePrompts(const prompts::Snapshot &a, const prompts::Snapshot &b)
        {
            auto same = [](const prompts::Binding &x, const prompts::Binding &y)
            {
                return x.type == y.type && x.device == y.device && x.sign == y.sign && x.code == y.code &&
                       std::equal(std::begin(x.label), std::end(x.label), std::begin(y.label)) &&
                       std::equal(std::begin(x.compact), std::end(x.compact), std::begin(y.compact));
            };
            return std::equal(a.keys.begin(), a.keys.end(), b.keys.begin(), same) &&
                   std::equal(a.controller.begin(), a.controller.end(), b.controller.begin(), same);
        }

        // Player 1's labels from the live table (the end of rebuildConfigs); a new generation only when one changed.
        void publishPrompts(const PadConfig &config)
        {
            // prompts::Input order -> binding key index (kPadButtonKeys, then kStickKeys).
            static const std::array<size_t, prompts::InputCount> kInputKeys = []
            {
                constexpr std::array<std::string_view, prompts::InputCount> kNames{
                    "Cross", "Circle", "Square", "Triangle", "L1", "R1", "L2", "R2", "L3", "R3", "Start", "Select",
                    "Up", "Down", "Left", "Right", "LUp", "LDown", "LLeft", "LRight", "RUp", "RDown", "RLeft", "RRight"};
                std::array<size_t, prompts::InputCount> keys{};
                for (size_t input = 0; input < kNames.size(); ++input)
                    keys[input] = bindingKeyIndex(kNames[input]).value();
                return keys;
            }();
            prompts::Snapshot next;
            int boundPad = -1;
            for (size_t input = 0; input < kInputKeys.size(); ++input)
            {
                const size_t index = kInputKeys[input];
                const Bindings &list = index < kPadButtonKeys.size() ? config.buttons[index] : config.sticks[index - kPadButtonKeys.size()];
                for (const Chord &chord : list)
                {
                    if (chord.size() != 1u)
                        continue; // a chord ("a & b") is no prompt; the Controls screen keeps it apart too
                    const Element &element = chord.front();
                    const bool keys = element.source == Source::Keyboard || element.source == Source::Pointer;
                    prompts::Binding &slot = keys ? next.keys[input] : next.controller[input];
                    if (slot.type == prompts::Type::None)
                        slot = promptBinding(element);
                    if (!keys && boundPad < 0)
                        boundPad = element.device;
                }
            }
            g_promptBoundPad = std::max(boundPad, 0);
            std::lock_guard<std::mutex> lock(g_promptMutex);
            if (g_prompts.generation != 0u && samePrompts(next, g_prompts))
                return;
            next.generation = g_prompts.generation + 1u == 0u ? 1u : g_prompts.generation + 1u;
            g_prompts = next;
            g_promptGeneration.store(next.generation, std::memory_order_release);
        }

        // Precedence per pad: PS2X_PAD<n> in the environment, then the file's [PadN], then the defaults.
        void rebuildConfigs()
        {
            for (uint32_t pad = 0; pad < kMaxPlayers; ++pad)
            {
                if (envPad(pad))
                    g_configs[pad] = envPadConfig(pad);
                else if (g_model.pads[pad].present)
                    g_configs[pad] = configFromSection(g_model.pads[pad], pad);
                else
                    g_configs[pad] = configFromSection(defaultSection(pad), pad);
            }
            g_configsValid = true;
            publishPrompts(g_configs[0]); // every rebuild: file load, the 1-s file poll, F1 edits, reset
        }

        const std::array<PadConfig, kMaxPlayers> &padConfigs()
        {
            if (!g_configsValid)
                rebuildConfigs();
            return g_configs;
        }

        ControlsModel defaultModel()
        {
            ControlsModel model;
            for (uint32_t pad = 0; pad < kPadPorts; ++pad)
                model.pads[pad] = defaultSection(pad);
            return model;
        }

        // Opens the file first: a missing or unreadable file leaves `model` alone (the caller keeps its tables).
        bool parseControlsFile(const std::string &path, ControlsModel &model, std::string &error)
        {
            std::ifstream file(path, std::ios::binary);
            if (!file)
            {
                error = "cannot open " + path;
                return false;
            }
            std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            if (text.size() >= 2u && ((static_cast<unsigned char>(text[0]) == 0xFFu && static_cast<unsigned char>(text[1]) == 0xFEu) ||
                                      (static_cast<unsigned char>(text[0]) == 0xFEu && static_cast<unsigned char>(text[1]) == 0xFFu)))
            {
                error = path + " is UTF-16; save it as UTF-8 or ANSI";
                return false;
            }
            if (text.size() >= 3u && text.compare(0, 3, "\xEF\xBB\xBF") == 0)
                text.erase(0, 3);
            if (text.find('\0') != std::string::npos)
            {
                error = path + " is not a text file (UTF-16 without a BOM?); save it as UTF-8 or ANSI";
                return false;
            }
            ControlsModel parsed;
            int pad = -1;
            bool other = false;
            bool sectionSeen = false;
            const std::string_view all(text);
            for (size_t begin = 0u; begin < all.size();)
            {
                const size_t end = std::min(all.find('\n', begin), all.size());
                std::string_view raw = all.substr(begin, end - begin);
                begin = end + 1u;
                if (!raw.empty() && raw.back() == '\r')
                    raw.remove_suffix(1u);
                const std::string_view line = trimView(raw);
                std::string_view header = line;
                if (!header.empty() && header.front() == '[')
                    header = trimView(header.substr(0u, std::min(header.find(';'), header.find('#')))); // "[Pad2] ; player 2"
                if (header.size() >= 2u && header.front() == '[' && header.back() == ']')
                {
                    const std::string name = lowerName(trimView(header.substr(1u, header.size() - 2u)));
                    sectionSeen = true;
                    pad = -1;
                    other = true;
                    if (name.size() == 4u && name.rfind("pad", 0) == 0 && name[3] >= '1' && name[3] < static_cast<char>('1' + kMaxPlayers))
                    {
                        pad = name[3] - '1';
                        other = false;
                        parsed.pads[static_cast<size_t>(pad)].present = true;
                        continue;
                    }
                }
                if (!sectionSeen)
                {
                    parsed.preamble.append(raw).push_back('\n');
                    continue;
                }
                if (other)
                {
                    parsed.otherText.append(raw).push_back('\n');
                    continue;
                }
                if (pad < 0 || line.empty())
                    continue;
                if (line[0] == '#' || line[0] == ';')
                {
                    parsed.pads[static_cast<size_t>(pad)].comments.emplace_back(line);
                    continue;
                }
                const size_t equals = line.find('=');
                if (equals == std::string_view::npos)
                {
                    std::cerr << "[controls] " << path << ": ignoring line '" << line << "'" << std::endl;
                    continue;
                }
                addSectionLine(parsed.pads[static_cast<size_t>(pad)], line.substr(0, equals), line.substr(equals + 1u));
            }
            model = std::move(parsed);
            return true;
        }

        // Without its trailing blank lines (each save would otherwise add one).
        std::string_view trimTrailingBlank(std::string_view text)
        {
            while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' || text.back() == '\t'))
                text.remove_suffix(1u);
            return text;
        }

        std::string serializeControls(const ControlsModel &model)
        {
            std::ostringstream out;
            if (const std::string_view preamble = trimTrailingBlank(model.preamble); !preamble.empty())
                out << preamble << '\n'; // the file's own header (and the player's notes in it), as written
            else
                out << "# God Hand controls (godhand.exe). F1 in the game opens the Controls screen, which edits and saves this file.\n"
                   "# PCSX2 syntax, one binding per line: <Key> = Keyboard/<key> | Pointer-0/LeftButton, RightButton, MiddleButton |\n"
                   "#   SDL-<n>/<button> (controller n: FaceSouth FaceEast FaceWest FaceNorth Back Start LeftStick RightStick\n"
                   "#   LeftShoulder RightShoulder DPadUp DPadDown DPadLeft DPadRight) | SDL-<n>/+<axis> or -<axis> (LeftX LeftY\n"
                   "#   RightX RightY LeftTrigger RightTrigger). Repeat a key for more bindings; \"<Key> =\" = unbound; \"a & b\" = both.\n"
                   "# Keys: Up Right Down Left Triangle Circle Cross Square Select Start L1 L2 R1 R2 L3 R3, LUp LRight LDown LLeft\n"
                   "#   (left stick), RUp RRight RDown RLeft (right stick); settings Type Deadzone AxisScale ButtonDeadzone InvertL InvertR\n"
                   "#   LargeMotor SmallMotor LargeMotorScale SmallMotorScale. F1 cannot be bound (it opens the Controls screen).\n"
                   "# Changes saved here are picked up while the game runs. Delete this file to get the default layout back.\n";
            for (uint32_t pad = 0; pad < kMaxPlayers; ++pad)
            {
                const PadSection &section = model.pads[pad];
                if (!section.present)
                    continue;
                out << "\n[Pad" << pad + 1u << "]\n";
                for (const std::string &comment : section.comments)
                    out << comment << '\n';
                for (const auto &[key, value] : section.settings)
                    out << key << " = " << value << '\n';
                for (size_t i = 0; i < kBindingKeyCount; ++i)
                {
                    if (section.bindings[i].empty())
                        out << bindingKeyName(i) << " =\n";
                    for (const std::string &binding : section.bindings[i])
                        out << bindingKeyName(i) << " = " << binding << '\n';
                }
            }
            if (const std::string_view rest = trimTrailingBlank(model.otherText); !rest.empty())
                out << '\n' << rest << '\n';
            return out.str();
        }

        void rememberFileStamp()
        {
            std::error_code error;
            const auto time = std::filesystem::last_write_time(g_controlsPath, error);
            const auto size = error ? 0u : std::filesystem::file_size(g_controlsPath, error);
            g_haveLoadedStamp = !error;
            if (!error)
            {
                g_loadedTime = time;
                g_loadedSize = size;
            }
        }

        // <file>.tmp written and flushed to the disk, then a replacing rename (retried: a virus scanner or editor may hold
        // the file for a moment). A read-only file is reported at once, without retries.
        bool writeControlsFile(const ControlsModel &model, std::string &message)
        {
            namespace fs = std::filesystem;
            const fs::path target(g_controlsPath);
            const fs::path temp(g_controlsPath + ".tmp");
            {
                std::error_code statusError;
                const fs::file_status status = fs::status(target, statusError);
                if (!statusError && fs::exists(status) && (status.permissions() & fs::perms::owner_write) == fs::perms::none)
                {
                    message = "Could not save " + target.string() + " (the file is read-only)";
                    return false;
                }
            }
            {
                const std::string text = serializeControls(model);
#if defined(_WIN32)
                std::FILE *file = nullptr;
                if (_wfopen_s(&file, temp.c_str(), L"wb") != 0)
                    file = nullptr;
#else
                std::FILE *file = std::fopen(temp.c_str(), "wb");
#endif
                bool ok = file != nullptr;
                if (ok)
                {
                    ok = std::fwrite(text.data(), 1u, text.size(), file) == text.size() && std::fflush(file) == 0;
#if defined(_WIN32)
                    ok = ok && _commit(_fileno(file)) == 0; // FlushFileBuffers: on the disk before the rename
#else
                    ok = ok && fsync(fileno(file)) == 0;
#endif
                    ok = std::fclose(file) == 0 && ok;
                }
                if (!ok)
                {
                    std::error_code ignored;
                    fs::remove(temp, ignored);
                    message = "Could not write " + temp.string();
                    return false;
                }
            }
            std::error_code error;
            for (int attempt = 0; attempt < 6; ++attempt)
            {
                fs::rename(temp, target, error);
                if (!error)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            if (error)
            {
                std::error_code ignored;
                fs::remove(temp, ignored);
                message = "Could not save " + target.string() + " (" + error.message() + ")";
                return false;
            }
            rememberFileStamp(); // our own write is not a change to reload
            g_lastFileError.clear();
            message = "Saved " + target.string();
            return true;
        }

        // Checked at most once a second while the Controls screen is closed and nothing is unsaved.
        void pollControlsFile()
        {
            if (g_controlsPath.empty() || g_controlsDirty || g_overlayOpen.load(std::memory_order_relaxed))
                return;
            const auto now = std::chrono::steady_clock::now();
            if (now - g_lastFileCheck < std::chrono::seconds(1))
                return;
            g_lastFileCheck = now;
            std::error_code error;
            const auto time = std::filesystem::last_write_time(g_controlsPath, error);
            if (error)
                return; // deleted or locked: keep the current tables
            const auto size = std::filesystem::file_size(g_controlsPath, error);
            if (error || (g_haveLoadedStamp && time == g_loadedTime && size == g_loadedSize))
                return;
            ControlsModel model;
            std::string message;
            if (!parseControlsFile(g_controlsPath, model, message))
            {
                if (message != g_lastFileError)
                    std::cerr << "[controls] " << message << "; keeping the current bindings" << std::endl;
                g_lastFileError = message;
                return; // the stamp is not advanced, so the next check retries
            }
            g_lastFileError.clear();
            g_model = std::move(model);
            g_loadedTime = time;
            g_loadedSize = size;
            g_haveLoadedStamp = true;
            rebuildConfigs();
            std::cerr << "[controls] reloaded " << g_controlsPath << std::endl;
        }

        constexpr std::array<std::pair<PadPressure, uint16_t>, static_cast<size_t>(PadPressure::Count)> kPressureButtons{{
            {PadPressure::Right, PadButton::Right},
            {PadPressure::Left, PadButton::Left},
            {PadPressure::Up, PadButton::Up},
            {PadPressure::Down, PadButton::Down},
            {PadPressure::Triangle, PadButton::Triangle},
            {PadPressure::Circle, PadButton::Circle},
            {PadPressure::Cross, PadButton::Cross},
            {PadPressure::Square, PadButton::Square},
            {PadPressure::L1, PadButton::L1},
            {PadPressure::R1, PadButton::R1},
            {PadPressure::L2, PadButton::L2},
            {PadPressure::R2, PadButton::R2},
        }};

        struct ScriptStep
        {
            double start = 0.0;
            double end = 0.0;
            bool fields = false;
            uint16_t buttons = 0u;
            std::array<uint8_t, 4> axes{0x80u, 0x80u, 0x80u, 0x80u};
        };

        struct AxisName
        {
            std::string_view name;
            size_t axis;
            uint8_t value;
        };

        constexpr std::array<AxisName, 8> kAxisNames{{
            {"lleft", 0u, 0x00u},
            {"lright", 0u, 0xFFu},
            {"lup", 1u, 0x00u},
            {"ldown", 1u, 0xFFu},
            {"rleft", 2u, 0x00u},
            {"rright", 2u, 0xFFu},
            {"rup", 3u, 0x00u},
            {"rdown", 3u, 0xFFu},
        }};

        constexpr std::array<std::pair<std::string_view, uint16_t>, 16> kButtonNames{{
            {"select", PadButton::Select},
            {"l3", PadButton::L3},
            {"r3", PadButton::R3},
            {"start", PadButton::Start},
            {"up", PadButton::Up},
            {"right", PadButton::Right},
            {"down", PadButton::Down},
            {"left", PadButton::Left},
            {"l2", PadButton::L2},
            {"r2", PadButton::R2},
            {"l1", PadButton::L1},
            {"r1", PadButton::R1},
            {"triangle", PadButton::Triangle},
            {"circle", PadButton::Circle},
            {"cross", PadButton::Cross},
            {"square", PadButton::Square},
        }};

        std::vector<std::string_view> split(std::string_view text, char separator)
        {
            std::vector<std::string_view> parts;
            size_t begin = 0u;
            while (begin <= text.size())
            {
                const size_t end = std::min(text.find(separator, begin), text.size());
                parts.push_back(text.substr(begin, end - begin));
                begin = end + 1u;
            }
            return parts;
        }

        std::optional<ScriptStep> parseStep(std::string_view text)
        {
            constexpr double kDefaultHoldSeconds = 1.0;
            constexpr double kDefaultHoldFields = 10.0;
            const std::vector<std::string_view> fields = split(text, ':');
            if (fields.size() < 2u || fields.size() > 3u)
                return std::nullopt;
            try
            {
                ScriptStep step;
                std::string_view start = fields[0];
                step.fields = !start.empty() && start.back() == 'v';
                if (step.fields)
                    start.remove_suffix(1u);
                step.start = std::stod(std::string(start));
                const double defaultHold = step.fields ? kDefaultHoldFields : kDefaultHoldSeconds;
                step.end = step.start + (fields.size() == 3u ? std::stod(std::string(fields[2])) : defaultHold);
                for (std::string_view name : split(fields[1], '+'))
                {
                    const auto axis = std::find_if(kAxisNames.begin(), kAxisNames.end(), [&](const AxisName &entry)
                                                   { return entry.name == name; });
                    if (axis != kAxisNames.end())
                    {
                        step.axes[axis->axis] = axis->value;
                        continue;
                    }
                    const auto found = std::find_if(kButtonNames.begin(), kButtonNames.end(), [&](const auto &entry)
                                                    { return entry.first == name; });
                    if (found == kButtonNames.end())
                        return std::nullopt;
                    step.buttons |= found->second;
                }
                return step;
            }
            catch (...)
            {
                return std::nullopt;
            }
        }

        // PS2X_PAD_SCRIPT for player 1, PS2X_PAD_SCRIPT<n> for player n (2..8).
        std::vector<ScriptStep> loadScript(uint32_t player = 0u)
        {
            std::vector<ScriptStep> script;
            const std::string variable = player == 0u ? "PS2X_PAD_SCRIPT" : "PS2X_PAD_SCRIPT" + std::to_string(player + 1u);
            const char *value = std::getenv(variable.c_str());
            if (!value || !*value)
                return script;
            for (std::string_view entry : split(value, ','))
            {
                if (const auto step = parseStep(entry))
                    script.push_back(*step);
                else
                    std::cerr << "[input] ignoring pad script step '" << entry << "'" << std::endl;
            }
            std::cerr << "[input] pad script: " << script.size() << " step(s) for player " << (player + 1u) << std::endl;
            return script;
        }

        struct ScriptedInput
        {
            uint16_t buttons = 0u;
            std::array<uint8_t, 4> axes{0x80u, 0x80u, 0x80u, 0x80u};
            bool active = false;
        };

        // PS2X_PAD_FIFO=<file>: a test driver appends lines while the game runs, each "buttons[:seconds]" with the
        // same names as PS2X_PAD_SCRIPT (default hold 0.3 s). Every new complete line is pressed right away, so the
        // driver can react to what is on screen instead of relying on fixed timings.
        struct FifoPress
        {
            uint16_t buttons = 0u;
            std::array<uint8_t, 4> axes{0x80u, 0x80u, 0x80u, 0x80u};
            std::chrono::steady_clock::time_point end;
        };

        void pollPadFifo(std::vector<FifoPress> &active)
        {
            static const char *path = std::getenv("PS2X_PAD_FIFO");
            if (!path || !*path)
                return;
            static std::streamoff offset = 0;
            static auto lastCheck = std::chrono::steady_clock::time_point{};
            const auto now = std::chrono::steady_clock::now();
            if (now - lastCheck < std::chrono::milliseconds(30))
                return;
            lastCheck = now;
            std::ifstream file(path, std::ios::binary);
            if (!file)
                return;
            file.seekg(0, std::ios::end);
            const std::streamoff size = file.tellg();
            if (size < offset)
                offset = 0; // the driver truncated the file
            if (size <= offset)
                return;
            std::string chunk(static_cast<size_t>(size - offset), '\0');
            file.seekg(offset);
            file.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
            const size_t lastNewline = chunk.rfind('\n');
            if (lastNewline == std::string::npos)
                return; // wait for a complete line
            offset += static_cast<std::streamoff>(lastNewline + 1u);
            for (std::string_view line : split(std::string_view(chunk).substr(0, lastNewline), '\n'))
            {
                while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                    line.remove_suffix(1u);
                if (line.empty())
                    continue;
                // "!save" / "!load": quick save state slot, like F5 / F9.
                if (line == "!save" || line == "!load")
                {
                    line == "!save" ? ps2x::savestate::requestQuickSave() : ps2x::savestate::requestQuickLoad();
                    continue;
                }
                const std::string entry = "0:" + std::string(line) + (line.find(':') == std::string_view::npos ? ":0.3" : "");
                if (const auto step = parseStep(entry))
                {
                    FifoPress press;
                    press.buttons = step->buttons;
                    press.axes = step->axes;
                    press.end = now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                          std::chrono::duration<double>(step->end - step->start));
                    active.push_back(press);
                }
                else
                    std::cerr << "[input] ignoring pad fifo line '" << line << "'" << std::endl;
            }
        }

        void applyScript(const std::vector<ScriptStep> &script, double now, uint64_t vsyncTick, ScriptedInput &input)
        {
            for (const ScriptStep &step : script)
            {
                const double clock = step.fields ? static_cast<double>(vsyncTick) : now;
                if (clock >= step.start && clock < step.end)
                {
                    input.buttons |= step.buttons;
                    for (size_t i = 0; i < input.axes.size(); ++i)
                    {
                        if (step.axes[i] != 0x80u)
                        {
                            input.axes[i] = step.axes[i];
                            input.active = true;
                        }
                    }
                }
            }
            input.active = input.active || input.buttons != 0u;
        }

        ScriptedInput scriptedInput(uint64_t vsyncTick)
        {
            static const std::vector<ScriptStep> script = loadScript();
            static const auto origin = std::chrono::steady_clock::now();
            static std::vector<FifoPress> fifoPresses;
            ScriptedInput input;
            pollPadFifo(fifoPresses);
            if (!fifoPresses.empty())
            {
                const auto nowTime = std::chrono::steady_clock::now();
                std::erase_if(fifoPresses, [&](const FifoPress &press) { return press.end <= nowTime; });
                for (const FifoPress &press : fifoPresses)
                {
                    input.buttons |= press.buttons;
                    for (size_t i = 0; i < input.axes.size(); ++i)
                        if (press.axes[i] != 0x80u)
                        {
                            input.axes[i] = press.axes[i];
                            input.active = true; // a stick-only line ("lup:0.5") was dropped: active came from buttons only
                        }
                }
            }
            if (script.empty())
            {
                input.active = input.active || input.buttons != 0u;
                return input;
            }
            applyScript(script, std::chrono::duration<double>(std::chrono::steady_clock::now() - origin).count(), vsyncTick, input);
            return input;
        }

        // Players 2..8's own scripts (tests), on the same clock as player 1's.
        ScriptedInput playerScriptedInput(uint32_t player, uint64_t vsyncTick)
        {
            static const std::array<std::vector<ScriptStep>, kMaxPlayers> scripts = []
            {
                std::array<std::vector<ScriptStep>, kMaxPlayers> loaded;
                for (uint32_t p = 1u; p < kMaxPlayers; ++p)
                    loaded[p] = loadScript(p);
                return loaded;
            }();
            static const auto origin = std::chrono::steady_clock::now();
            ScriptedInput input;
            if (player < kMaxPlayers && !scripts[player].empty())
                applyScript(scripts[player], std::chrono::duration<double>(std::chrono::steady_clock::now() - origin).count(), vsyncTick, input);
            return input;
        }

        std::mutex g_mutex;
        // One state per player (controls.ini [Pad1]-[Pad8]); by default player 1 is the keyboard plus SDL-0, player n
        // SDL-<n-1>.
        std::array<PadInputState, kMaxPlayers> g_state = []
        {
            std::array<PadInputState, kMaxPlayers> state{};
            state[0].connected = true;
            return state;
        }();

        // Multitap ports (bit 0 = controller port 1, bit 1 = port 2), from PS2X_MULTITAP like the IOP's SIO2.
        uint32_t multitapPorts()
        {
            static const uint32_t ports = []
            {
                const char *v = std::getenv("PS2X_MULTITAP");
                const std::string_view text = v ? v : "0";
                return (text.find('1') != std::string_view::npos ? 1u : 0u) | (text.find('2') != std::string_view::npos ? 2u : 0u);
            }();
            return ports;
        }

        // The player behind (port, slot): 1A, 2A, then 2B-2D and 1B-1D for the ports with a multitap (the slots of a port
        // without one are skipped). PCSX2 puts port 1's slots first; here port 2's come first, the ones the game uses for
        // P3/P4, so port 1's multitap only adds players 6-8 (P5-P8 as humans with CoopTool) after the same first five.
        int playerFor(uint32_t port, uint32_t slot)
        {
            if (port >= kPadPorts || slot >= 4u)
                return -1;
            if (slot == 0u)
                return static_cast<int>(port);
            const uint32_t taps = multitapPorts();
            if ((taps & (1u << port)) == 0u)
                return -1;
            int player = 2 + static_cast<int>(slot) - 1;
            if (port == 0u && (taps & 2u) != 0u)
                player += 3;
            return player;
        }

        constexpr uint8_t kAxisNeutral = 0x7Fu;

        uint8_t axisToByte(float value)
        {
            const float clamped = std::clamp(value, -1.0f, 1.0f);
            const float scaled = std::round(static_cast<float>(kAxisNeutral) + clamped * (clamped < 0.0f ? 127.0f : 128.0f));
            return static_cast<uint8_t>(std::clamp(scaled, 0.0f, 255.0f));
        }

        std::atomic<const std::atomic<uint64_t> *> g_fieldCounter{nullptr};
        std::atomic<uint32_t> g_invertRightDefault{0u};
        std::atomic<bool> g_mouseCameraEnabled{false};
        std::atomic<bool> g_mouseStickEnabled{true};
        bool g_mouseLocked = false;
        int g_ignoredMouseButton = -1;
        bool g_skipMouseDelta = false;
        bool g_mouseMoved = false;
        std::atomic<bool> g_menuMouse{false}; // setMenuHasMouse: the in-game menu has the mouse
        float g_mouseRightX = 0.0f;
        float g_mouseRightY = 0.0f;
        MouseCameraInput g_mouseCameraInput;
        std::chrono::steady_clock::time_point g_mouseInputStart;

        void recordMouseCameraInput(Vector2 delta, bool active)
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (!active)
            {
                g_mouseCameraInput = {};
                return;
            }
            g_mouseCameraInput.active = true;
            if (delta.x == 0.0f && delta.y == 0.0f)
                return;
            const auto now = std::chrono::steady_clock::now();
            if ((g_mouseCameraInput.x == 0.0f && g_mouseCameraInput.y == 0.0f) ||
                now - g_mouseInputStart > std::chrono::milliseconds(100))
            {
                g_mouseCameraInput.x = 0.0f;
                g_mouseCameraInput.y = 0.0f;
                g_mouseInputStart = now;
            }
            g_mouseCameraInput.x += delta.x;
            g_mouseCameraInput.y += delta.y;
        }

        // ---- Back+Start gate (window thread) ---------------------------------------------------------------------------
        // Back+Start opens the Controls screen, so the game must never see that chord. Back or Start pressed alone is held
        // back for kGateDelay; a second button arriving inside it makes a chord, and both stay hidden until both are
        // released. A tap shorter than kGateDelay still reaches the game, as a kGatePulse press after its release.
        // Sweep CR-5: this gate was never switched on (nothing called setChordGate), so Start pressed a moment before
        // Back paused the game or skipped a cutscene behind the Controls screen. ps2x::controls::hostTick now turns it
        // on while the screen can open. With one of the two already held, the other is a chord from its first frame
        // and never reaches the game. Cost: a lone Back or Start reaches the game kGateDelay late (a tap: when it is
        // released): about 5 fields, on the pause/menu buttons only; keys and every other button are never delayed.
        constexpr auto kGateDelay = std::chrono::milliseconds(80);
        constexpr auto kGatePulse = std::chrono::milliseconds(50);
        struct ChordGate
        {
            std::array<bool, 2> down{}; // [Back, Start] as last read
            std::array<std::chrono::steady_clock::time_point, 2> since{};
            std::array<std::chrono::steady_clock::time_point, 2> pulseUntil{};
            bool chord = false; // both were held: hidden until both are released
        };
        std::array<ChordGate, ps2_host_gamepads::kMaxDevices> g_chordGates{};
        std::array<std::array<bool, 2>, ps2_host_gamepads::kMaxDevices> g_gateHide{};
        std::array<std::array<bool, 2>, ps2_host_gamepads::kMaxDevices> g_gateForce{};
        std::atomic<bool> g_chordGateEnabled{false}; // only while the Controls screen can open (set by ps2x::controls::hostTick)
        // Self-test input: player 1's buttons (PS2 bits) and controller 1's Back (bit 0) / Start (bit 1), window thread.
        std::atomic<uint16_t> g_testButtons{0u};
        std::atomic<uint8_t> g_testGateButtons{0u};

        bool rawGateButton(int device, int which)
        {
            const bool test = device == 0 && (g_testGateButtons.load(std::memory_order_relaxed) & (1u << which)) != 0u;
            return test || ps2_host_gamepads::button(device, which == 0 ? ps2_host_gamepads::Back : ps2_host_gamepads::Start);
        }

        void updateChordGate()
        {
            const bool enabled = g_chordGateEnabled.load(std::memory_order_relaxed);
            const auto now = std::chrono::steady_clock::now();
            for (int device = 0; device < ps2_host_gamepads::kMaxDevices; ++device)
            {
                ChordGate &gate = g_chordGates[static_cast<size_t>(device)];
                const std::array<bool, 2> down{rawGateButton(device, 0), rawGateButton(device, 1)};
                if (down[0] && down[1])
                    gate.chord = true;
                for (int which = 0; which < 2; ++which)
                {
                    bool hide = false;
                    if (gate.chord)
                    {
                        hide = true;
                        gate.pulseUntil[which] = {};
                    }
                    else if (down[which])
                    {
                        if (!gate.down[which])
                            gate.since[which] = now;
                        hide = now - gate.since[which] < kGateDelay;
                    }
                    else if (gate.down[which])
                    {
                        // Released. A quick tap (shorter than kGateDelay): the game still gets it, as a kGatePulse press.
                        // Review 2026-10-18: a press released just after kGateDelay was shown for a frame or less, which
                        // the game's pad read can miss (a lost Start); it now stays shown until it was up for kGatePulse.
                        gate.pulseUntil[which] = now - gate.since[which] < kGateDelay ? now + kGatePulse
                                                                                      : gate.since[which] + kGateDelay + kGatePulse;
                    }
                    gate.down[which] = down[which];
                    g_gateHide[static_cast<size_t>(device)][which] = enabled && hide;
                    g_gateForce[static_cast<size_t>(device)][which] = enabled && !down[which] && now < gate.pulseUntil[which];
                }
                if (!down[0] && !down[1])
                    gate.chord = false;
            }
        }

        // 0..1: how far the element is pressed (keys and buttons are 0 or 1).
        float elementValue(const Element &element)
        {
            switch (element.source)
            {
            case Source::Keyboard:
                if (element.code == kAnyShift)
                    return IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT) ? 1.0f : 0.0f;
                if (element.code == kAnyControl)
                    return IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL) ? 1.0f : 0.0f;
                if (element.code == kAnyAlt)
                    return IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT) ? 1.0f : 0.0f;
                if (element.code == KEY_CAPS_LOCK || element.code == KEY_NUM_LOCK)
                    return lockKeyDown(element.code) ? 1.0f : 0.0f;
                return IsKeyDown(element.code) ? 1.0f : 0.0f;
            case Source::Pointer:
                // With the mouse camera on, only while the mouse is captured (the click that captures it is skipped).
                if (g_mouseCameraEnabled.load(std::memory_order_relaxed) && (!g_mouseLocked || element.code == g_ignoredMouseButton))
                    return 0.0f;
                return IsWindowFocused() && !g_menuMouse.load(std::memory_order_relaxed) && IsMouseButtonDown(element.code) ? 1.0f : 0.0f;
            case Source::PadButton:
                if ((element.code == ps2_host_gamepads::Back || element.code == ps2_host_gamepads::Start) && element.device >= 0 &&
                    element.device < ps2_host_gamepads::kMaxDevices)
                {
                    const int which = element.code == ps2_host_gamepads::Back ? 0 : 1;
                    const size_t device = static_cast<size_t>(element.device);
                    if (g_gateForce[device][which])
                        return 1.0f;
                    if (g_gateHide[device][which])
                        return 0.0f;
                    return rawGateButton(element.device, which) ? 1.0f : 0.0f;
                }
                return ps2_host_gamepads::button(element.device, static_cast<ps2_host_gamepads::Button>(element.code)) ? 1.0f : 0.0f;
            case Source::PadAxis:
                return std::max(0.0f, element.sign * ps2_host_gamepads::axis(element.device, static_cast<ps2_host_gamepads::Axis>(element.code)));
            }
            return 0.0f;
        }

        // Button prompts: which kinds of binding hold an input past half (the device in use).
        struct Fired
        {
            bool keys = false;       // a key or mouse button binding
            bool controller = false; // a controller button or axis half
            int pad = -1;            // that controller
        };

        // fired (optional): the kinds of the bindings held past half.
        float bindingsValue(const Bindings &bindings, Fired *fired = nullptr)
        {
            float value = 0.0f;
            for (const Chord &chord : bindings)
            {
                float held = 1.0f;
                for (const Element &element : chord)
                    held = std::min(held, elementValue(element));
                value = std::max(value, held);
                if (fired && held > 0.5f)
                {
                    for (const Element &element : chord)
                    {
                        if (element.source == Source::Keyboard || element.source == Source::Pointer)
                            fired->keys = true;
                        else
                        {
                            fired->controller = true;
                            fired->pad = element.device;
                        }
                    }
                }
            }
            return value;
        }

        // A pad counts as plugged in when one of its bindings can be pressed: the keyboard and the mouse always, a
        // controller while it is connected.
        bool padPresent(const PadConfig &config)
        {
            if (!config.enabled)
                return false;
            auto present = [](const Bindings &bindings)
            {
                for (const Chord &chord : bindings)
                    for (const Element &element : chord)
                        if (element.source == Source::Keyboard || element.source == Source::Pointer ||
                            ps2_host_gamepads::connected(element.device))
                            return true;
                return false;
            };
            for (const Bindings &bindings : config.buttons)
                if (present(bindings))
                    return true;
            for (const Bindings &bindings : config.sticks)
                if (present(bindings))
                    return true;
            return false;
        }

        float positiveSetting(const char *name, float fallback)
        {
            if (const char *value = std::getenv(name))
            {
                char *end = nullptr;
                const float parsed = std::strtof(value, &end);
                if (end != value && *end == '\0' && std::isfinite(parsed) && parsed > 0.0f)
                    return parsed;
            }
            return fallback;
        }

        float mouseSensitivity()
        {
            static const float sensitivity = positiveSetting("PS2X_MOUSE_SENSITIVITY", 1.0f);
            return sensitivity;
        }

        float mouseVerticalSensitivity()
        {
            static const float sensitivity = positiveSetting("PS2X_MOUSE_VERTICAL_SENSITIVITY", 1.0f);
            return sensitivity;
        }

        Vector2 mouseStick(Vector2 desired)
        {
            const float distance = std::hypot(desired.x, desired.y);
            if (distance <= 0.0f)
                return {};
            constexpr float deadzone = 0.4f;
            const float magnitude = deadzone + (1.0f - deadzone) * std::min(distance, 1.0f);
            const float scale = magnitude / distance;
            return {desired.x * scale, desired.y * scale};
        }

        void sampleMouse()
        {
            g_mouseMoved = false;
            if (!g_mouseCameraEnabled.load(std::memory_order_relaxed) || g_menuMouse.load(std::memory_order_relaxed))
            {
                if (g_mouseLocked)
                    EnableCursor();
                g_mouseLocked = false;
                g_ignoredMouseButton = -1;
                recordMouseCameraInput({}, false);
                return;
            }
            if (!IsWindowFocused() || IsKeyPressed(KEY_ESCAPE))
            {
                if (g_mouseLocked)
                    EnableCursor();
                g_mouseLocked = false;
                g_ignoredMouseButton = -1;
                recordMouseCameraInput({}, false);
                return;
            }
            if (!g_mouseLocked)
            {
                int button = -1;
                if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
                    button = MOUSE_BUTTON_LEFT;
                else if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT))
                    button = MOUSE_BUTTON_RIGHT;
                else if (IsMouseButtonPressed(MOUSE_BUTTON_MIDDLE))
                    button = MOUSE_BUTTON_MIDDLE;
                if (button != -1)
                {
                    DisableCursor();
                    g_mouseLocked = true;
                    g_ignoredMouseButton = button;
                    g_skipMouseDelta = true;
                }
                recordMouseCameraInput({}, g_mouseLocked);
                return;
            }
            if (g_ignoredMouseButton != -1 && !IsMouseButtonDown(g_ignoredMouseButton))
                g_ignoredMouseButton = -1;
            if (g_skipMouseDelta)
            {
                g_skipMouseDelta = false;
                recordMouseCameraInput({}, true);
                return;
            }

            const Vector2 delta = GetMouseDelta();
            recordMouseCameraInput(delta, true);
            if (!g_mouseStickEnabled.load(std::memory_order_relaxed))
                return;
            const float distance = std::hypot(delta.x, delta.y);
            if (distance <= 0.0f)
                return;
            const float frameTime = std::max(GetFrameTime(), 1.0f / 240.0f);
            const float scale = mouseSensitivity() / (frameTime * 900.0f);
            const Vector2 stick = mouseStick({delta.x * scale, delta.y * scale * mouseVerticalSensitivity()});
            g_mouseRightX = stick.x;
            g_mouseRightY = stick.y;
            g_mouseMoved = true;
        }

        uint32_t rightStickInvert()
        {
            if (const char *value = std::getenv("PS2X_INVERT_RIGHT_STICK"))
            {
                const std::string_view text(value);
                uint32_t axes = 0u;
                if (text.find_first_of("xX") != std::string_view::npos || text == "1")
                    axes |= kInvertRightX;
                if (text.find_first_of("yY") != std::string_view::npos || text == "1")
                    axes |= kInvertRightY;
                return axes;
            }
            return g_invertRightDefault.load(std::memory_order_relaxed);
        }

        // Full pressure for the pressed buttons (the scripted input has no analog values).
        void applyPressure(PadInputState &state)
        {
            for (const auto &[pressure, button] : kPressureButtons)
                state.pressure[static_cast<size_t>(pressure)] = (state.buttons & button) != 0u ? 0xFFu : 0x00u;
        }

        // A stick from its four directions: radial deadzone, then PCSX2's axis scale (1.33 reaches the corners).
        std::pair<float, float> stick(const PadConfig &config, const std::array<float, StickHalfCount> &halves, size_t up, uint32_t invert)
        {
            float x = halves[up + 1u] - halves[up + 3u]; // right - left
            float y = halves[up + 2u] - halves[up];      // down - up
            const float magnitude = std::hypot(x, y);
            if (magnitude <= config.deadzone || magnitude <= 0.0f)
                return {0.0f, 0.0f};
            if (config.deadzone > 0.0f)
            {
                const float scale = (magnitude - config.deadzone) / (1.0f - config.deadzone) / magnitude;
                x *= scale;
                y *= scale;
            }
            x = std::clamp(x * config.axisScale, -1.0f, 1.0f);
            y = std::clamp(y * config.axisScale, -1.0f, 1.0f);
            return {(invert & 1u) != 0u ? -x : x, (invert & 2u) != 0u ? -y : y};
        }

        // Button prompts: a bound input newly held past half picks the device the prompts show, keys/mouse or controller
        // (Tokyo Jungle's cellPad.c rule). At most one change per kPromptSwitchDelay, so using both at once does not
        // flicker; a key and a controller input newly pressed in the same frame change nothing. Counted only while the
        // window has focus (GLFW reads controllers in the background too). Self-test, scripted and FIFO input are merged
        // after readPort, so they never switch it. keysHeld/padsHeld: bit = binding key index.
        void notePromptInput(uint32_t keysHeld, uint32_t padsHeld, int pad)
        {
            const uint32_t keyEdge = keysHeld & ~g_promptKeysHeld;
            const uint32_t padEdge = padsHeld & ~g_promptPadsHeld;
            g_promptKeysHeld = keysHeld;
            g_promptPadsHeld = padsHeld;
            if ((keyEdge != 0u || padEdge != 0u) && IsWindowFocused())
            {
                if (padEdge != 0u && pad >= 0)
                    g_promptPad = pad;
                const prompts::Device wanted = keyEdge != 0u ? prompts::Device::Keys : prompts::Device::Controller;
                const auto now = std::chrono::steady_clock::now();
                if ((keyEdge == 0u || padEdge == 0u) && wanted != g_promptDevice.load(std::memory_order_relaxed) &&
                    now - g_promptSwitchAt >= kPromptSwitchDelay)
                {
                    g_promptSwitchAt = now;
                    g_promptDevice.store(wanted, std::memory_order_relaxed);
                    std::cerr << "[prompts] device: " << (wanted == prompts::Device::Keys ? "keyboard/mouse" : "controller") << std::endl;
                }
            }
            prompts::PadStyle style = prompts::PadStyle::Unknown;
            switch (ps2_host_gamepads::style(g_promptPad >= 0 ? g_promptPad : g_promptBoundPad))
            {
            case ps2_host_gamepads::Style::Xbox: style = prompts::PadStyle::Xbox; break;
            case ps2_host_gamepads::Style::PlayStation: style = prompts::PadStyle::PlayStation; break;
            default: break;
            }
            g_promptStyle.store(style, std::memory_order_relaxed);
        }

        // Player n (0-based): controls.ini [Pad<n+1>], plus the mouse as the right stick for player 1 (mouse camera).
        // rawStick (optional): the larger stick deflection before the deadzone and the axis scale (0..~1.4).
        PadInputState readPort(uint32_t player, float *rawStick = nullptr)
        {
            PadInputState state;
            if (rawStick)
                *rawStick = 0.0f;
            const PadConfig &config = padConfigs()[player];
            // PS2X_FAKE_PADS=<n> (tests): players 1..n count as connected, idle, without a controller behind them.
            static const uint32_t fakePads = []
            { const char *v = std::getenv("PS2X_FAKE_PADS"); return v ? static_cast<uint32_t>(std::max(std::atoi(v), 0)) : 0u; }();
            state.connected = padPresent(config) || player < fakePads;
            if (!state.connected)
                return state;

            // Button prompts (player 1 only): the inputs held past half, by kind of binding.
            uint32_t keysHeld = 0u;
            uint32_t padsHeld = 0u;
            int pad = -1;
            auto noteFired = [&](size_t bit, const Fired &fired)
            {
                if (fired.keys)
                    keysHeld |= 1u << bit;
                if (fired.controller)
                {
                    padsHeld |= 1u << bit;
                    pad = fired.pad;
                }
            };
            std::array<float, kPadButtonKeys.size()> values{};
            for (size_t i = 0; i < kPadButtonKeys.size(); ++i)
            {
                Fired fired;
                values[i] = bindingsValue(config.buttons[i], player == 0u ? &fired : nullptr);
                noteFired(i, fired);
                if (values[i] > std::max(config.buttonDeadzone, 0.2f))
                    state.buttons |= kPadButtonKeys[i].second;
            }
            for (const auto &[pressure, button] : kPressureButtons)
            {
                for (size_t i = 0; i < kPadButtonKeys.size(); ++i)
                {
                    if (kPadButtonKeys[i].second == button && (state.buttons & button) != 0u)
                        state.pressure[static_cast<size_t>(pressure)] = static_cast<uint8_t>(std::clamp(values[i], 0.0f, 1.0f) * 255.0f + 0.5f);
                }
            }

            std::array<float, StickHalfCount> halves{};
            for (size_t i = 0; i < halves.size(); ++i)
            {
                Fired fired;
                halves[i] = bindingsValue(config.sticks[i], player == 0u ? &fired : nullptr);
                noteFired(kPadButtonKeys.size() + i, fired);
            }
            if (player == 0u)
                notePromptInput(keysHeld, padsHeld, pad);
            if (rawStick)
                for (const size_t up : {static_cast<size_t>(LUp), static_cast<size_t>(RUp)})
                    *rawStick = std::max(*rawStick, std::hypot(halves[up + 1u] - halves[up + 3u], halves[up + 2u] - halves[up]));
            const auto [leftX, leftY] = stick(config, halves, LUp, config.invertL);
            const uint32_t invertRight = config.invertR | rightStickInvert();
            auto [rightX, rightY] = stick(config, halves, RUp, invertRight);
            if (player == 0u && g_mouseMoved && g_mouseStickEnabled.load(std::memory_order_relaxed))
            {
                rightX = (invertRight & kInvertRightX) != 0u ? -g_mouseRightX : g_mouseRightX;
                rightY = (invertRight & kInvertRightY) != 0u ? -g_mouseRightY : g_mouseRightY;
            }
            state.leftX = axisToByte(leftX);
            state.leftY = axisToByte(leftY);
            state.rightX = axisToByte(rightX);
            state.rightY = axisToByte(rightY);
            return state;
        }

        // The motors each player's DualShock 2 was told to run (IOP thread), played on the controllers its
        // LargeMotor/SmallMotor bindings name (window thread). small: on/off, as on the pad; large: 0-255.
        std::array<std::atomic<uint16_t>, kMaxPlayers> g_motors{};
        std::atomic<bool> g_blocked{false}; // the in-game menu has the input
        // Controls screen hold (window thread): players still idled after it closed, until released or the 2 s cap.
        uint32_t g_holdMask = 0u;
        std::chrono::steady_clock::time_point g_holdStart{};
        std::chrono::steady_clock::time_point g_holdUntil{};

        // Released: no button, and both sticks at rest, measured before the axis scale so stick drift does not count
        // (at least 25% of the travel, or the pad's own Deadzone when larger).
        bool inputReleased(const PadInputState &state, float rawStick, const PadConfig &config)
        {
            return state.buttons == 0u && rawStick <= std::max(config.deadzone, 0.25f);
        }

        void updateVibration()
        {
            std::array<float, ps2_host_gamepads::kMaxDevices> large{};
            std::array<float, ps2_host_gamepads::kMaxDevices> small{};
            for (uint32_t player = 0; player < kMaxPlayers; ++player)
            {
                const uint16_t motors = g_motors[player].load(std::memory_order_relaxed);
                const PadConfig &config = padConfigs()[player];
                const float largeValue = static_cast<float>(motors >> 8u) / 255.0f * config.largeMotorScale;
                const float smallValue = (motors & 0xFFu) != 0u ? config.smallMotorScale : 0.0f;
                for (int device : config.largeMotor)
                    if (device >= 0 && device < ps2_host_gamepads::kMaxDevices)
                        large[static_cast<size_t>(device)] = std::max(large[static_cast<size_t>(device)], largeValue);
                for (int device : config.smallMotor)
                    if (device >= 0 && device < ps2_host_gamepads::kMaxDevices)
                        small[static_cast<size_t>(device)] = std::max(small[static_cast<size_t>(device)], smallValue);
            }
            for (int device = 0; device < ps2_host_gamepads::kMaxDevices; ++device)
                ps2_host_gamepads::setRumble(device, large[static_cast<size_t>(device)], small[static_cast<size_t>(device)]);
        }

        // ---- Windows accessibility shortcut keys (window thread; the restore from any thread) ---------------------------
        // Sweep 2026-10-17 CR-9: Taunt is on LeftShift, and five quick Shift presses open Windows' "Turn on Sticky Keys?"
        // dialog over the game (right Shift held 8 s does the same for Filter Keys, Num Lock held 5 s for Toggle Keys).
        // Microsoft's "Disabling Shortcut Keys in Games" pattern: while the game window has the focus, clear only the
        // HOTKEYACTIVE and CONFIRMHOTKEY flags of each feature that is not already on (one that is on is in use: left
        // alone), and put the user's values back when the window loses the focus and when the game ends. Written with
        // fWinIni = 0, never to the user profile, so even a killed process changes nothing past sign-out; the restore
        // also runs from atexit, from the crash paths (unhandled exception, abort, terminate) as far as they run, and when
        // the console the game runs in is closed (Ctrl+C, Ctrl+Break, its window closed, sign-out).
        struct AccessKeys // STICKYKEYS / TOGGLEKEYS {cbSize, dwFlags}; FILTERKEYS adds four DWORDs (wait/delay/repeat/bounce)
        {
            unsigned int size = 0u;
            unsigned long flags = 0u;
            unsigned long filterTimes[4]{};
        };
        struct AccessFeature
        {
            unsigned int get; // SPI_GET...
            unsigned int set; // SPI_SET...
            unsigned int size;
        };
        constexpr std::array<AccessFeature, 3> kAccessFeatures{{
            {0x003Au, 0x003Bu, 8u},  // SPI_GETSTICKYKEYS / SPI_SETSTICKYKEYS, sizeof(STICKYKEYS)
            {0x0034u, 0x0035u, 8u},  // SPI_GETTOGGLEKEYS / SPI_SETTOGGLEKEYS, sizeof(TOGGLEKEYS)
            {0x0032u, 0x0033u, 24u}, // SPI_GETFILTERKEYS / SPI_SETFILTERKEYS, sizeof(FILTERKEYS)
        }};
        constexpr unsigned long kAccessFeatureOn = 0x1u; // SKF_STICKYKEYSON / TKF_TOGGLEKEYSON / FKF_FILTERKEYSON
        constexpr unsigned long kAccessHotkeyFlags = 0x4u | 0x8u; // ..._HOTKEYACTIVE | ..._CONFIRMHOTKEY (same bits in all three)
        std::array<AccessKeys, 3> g_accessSaved{}; // the user's values, written before the feature's bit is set
        std::atomic<uint32_t> g_accessChanged{0u}; // bit i: kAccessFeatures[i] holds our value now
        std::atomic<bool> g_accessFinal{false};    // restored for good: never cleared again
        bool g_accessFocused = false;              // window thread: the focus updateAccessKeys last acted on

        void restoreAccessKeys() noexcept
        {
#if defined(_WIN32)
            const uint32_t changed = g_accessChanged.exchange(0u, std::memory_order_acq_rel);
            for (size_t i = 0; i < kAccessFeatures.size(); ++i)
                if ((changed & (1u << i)) != 0u)
                {
                    AccessKeys saved = g_accessSaved[i];
                    SystemParametersInfoW(kAccessFeatures[i].set, kAccessFeatures[i].size, &saved, 0u);
                }
#endif
        }

        // For good, the game is ending: sample() never clears them again (it re-checks after a clear, so a clear racing
        // this on the window thread is undone too). Any thread.
        void restoreAccessKeysFinal() noexcept
        {
            g_accessFinal.store(true, std::memory_order_release);
            restoreAccessKeys();
        }

#if defined(_WIN32)
        // The crash paths chain to whatever was installed before (godhand main's crash reporter, the CRT's filter).
        Ps2xTopLevelExceptionFilter g_previousCrashFilter = nullptr;
        void(__cdecl *g_previousAbortHandler)(int) = nullptr;
        std::terminate_handler g_previousTerminate = nullptr;

        long __stdcall accessCrashFilter(_EXCEPTION_POINTERS *info)
        {
            restoreAccessKeysFinal();
            return g_previousCrashFilter ? g_previousCrashFilter(info) : 0 /* EXCEPTION_CONTINUE_SEARCH */;
        }

        void __cdecl accessAbortHandler(int signal)
        {
            restoreAccessKeysFinal();
            if (g_previousAbortHandler != nullptr && g_previousAbortHandler != SIG_DFL && g_previousAbortHandler != SIG_IGN &&
                g_previousAbortHandler != SIG_ERR)
                g_previousAbortHandler(signal);
        }

        void accessTerminate()
        {
            restoreAccessKeysFinal();
            if (g_previousTerminate)
                g_previousTerminate();
            std::abort();
        }

        // Review 2026-10-18: godhand.exe is a console program and Start-God-Hand.cmd runs it in the minimized "God Hand
        // log" console. Closing that window (or Ctrl+C / Ctrl+Break in it) ends the process through ExitProcess: no
        // atexit, no end of the run loop, so the hotkeys stayed off until sign-out. Runs on a thread of its own; FALSE
        // lets the default handling (the exit) go on.
        int __stdcall accessConsoleHandler(unsigned long)
        {
            restoreAccessKeysFinal();
            return 0;
        }

        // Once, the first time a hotkey is cleared: hidden runs (never focused) keep their crash handling untouched.
        void installAccessRestoreHooks()
        {
            static bool installed = false;
            if (installed)
                return;
            installed = true;
            std::atexit([] { restoreAccessKeysFinal(); });
            g_previousCrashFilter = SetUnhandledExceptionFilter(&accessCrashFilter);
            g_previousAbortHandler = std::signal(SIGABRT, &accessAbortHandler);
            g_previousTerminate = std::set_terminate(&accessTerminate);
            SetConsoleCtrlHandler(&accessConsoleHandler, 1);
        }
#endif

        // Once per sample(). The focus is read from Windows too: raylib's flag stays "focused" for a window created
        // hidden (hidden test runs), and those must never touch the user's settings.
        void updateAccessKeys()
        {
#if defined(_WIN32)
            if (g_accessFinal.load(std::memory_order_relaxed))
                return;
            const bool focused = IsWindowReady() && !IsWindowHidden() && IsWindowFocused() && GetWindowHandle() != nullptr &&
                                 GetForegroundWindow() == GetWindowHandle();
            if (focused == g_accessFocused)
                return;
            g_accessFocused = focused;
            if (!focused)
                return restoreAccessKeys();
            installAccessRestoreHooks();
            for (size_t i = 0; i < kAccessFeatures.size(); ++i)
            {
                const AccessFeature &feature = kAccessFeatures[i];
                AccessKeys current;
                current.size = feature.size;
                if (!SystemParametersInfoW(feature.get, feature.size, &current, 0u) || (current.flags & kAccessFeatureOn) != 0u ||
                    (current.flags & kAccessHotkeyFlags) == 0u)
                    continue;
                g_accessSaved[i] = current; // re-read at every focus gain: a change made in Settings meanwhile is kept
                AccessKeys off = current;
                off.flags &= ~kAccessHotkeyFlags;
                if (SystemParametersInfoW(feature.set, feature.size, &off, 0u))
                    g_accessChanged.fetch_or(1u << i, std::memory_order_acq_rel);
            }
            // A final restore on another thread (console closed, crash) between a clear and its bit missed that clear.
            if (g_accessFinal.load(std::memory_order_acquire))
                return restoreAccessKeys();
            static bool logged = false;
            if (!logged && g_accessChanged.load(std::memory_order_relaxed) != 0u)
            {
                logged = true;
                std::cerr << "[input] Sticky/Toggle/Filter Keys shortcut keys off while the game window has the focus" << std::endl;
            }
#endif
        }
    }

    void setRightStickInvertDefault(uint32_t axes)
    {
        g_invertRightDefault.store(axes, std::memory_order_relaxed);
    }

    void setMouseCameraEnabled(bool enabled)
    {
        g_mouseCameraEnabled.store(enabled, std::memory_order_relaxed);
    }

    void setFieldCounter(const std::atomic<uint64_t> *counter)
    {
        g_fieldCounter.store(counter);
    }

    void sample()
    {
        ps2_host_gamepads::update();
        updateChordGate(); // right after the controllers are read: Back/Start never reach the game a frame early
        updateVibration();
        sampleMouse();
        updateAccessKeys(); // sweep CR-9: also runs while minimized (the run loop's minimized branch calls sample())
        // Only the players that have a (port, slot): 2 without a multitap, 5 with one, 8 with two.
        const uint32_t taps = multitapPorts();
        const uint32_t players = 2u + ((taps & 1u) ? 3u : 0u) + ((taps & 2u) ? 3u : 0u);
        std::array<PadInputState, kMaxPlayers> state{};
        const bool blocked = g_blocked.load(std::memory_order_relaxed);
        const bool overlay = g_overlayOpen.load(std::memory_order_relaxed);
        const auto now = std::chrono::steady_clock::now();
        g_holdMask &= (1u << players) - 1u; // players without a (port, slot) are never read, so never released
        if (g_holdMask != 0u && now >= g_holdUntil)
        {
            std::cerr << "[controls] input hold: 2 s cap reached (mask 0x" << std::hex << g_holdMask << std::dec << ")" << std::endl;
            g_holdMask = 0u;
        }
        for (uint32_t player = 0; player < players; ++player)
        {
            float rawStick = 0.0f;
            state[player] = readPort(player, &rawStick);
            if (player == 0u)
            {
                // Self-test buttons (GODHAND_CONTROLS_SELFTEST "press:"): through the same idle and hold rules as a key.
                if (const uint16_t test = g_testButtons.load(std::memory_order_relaxed); test != 0u)
                {
                    state[0].connected = true;
                    state[0].buttons |= test;
                    for (const auto &[pressure, button] : kPressureButtons)
                        if ((test & button) != 0u)
                            state[0].pressure[static_cast<size_t>(pressure)] = 0xFFu;
                }
            }
            bool idleIt = blocked || overlay;
            // After the Controls screen closes, each player stays idle until its own buttons and sticks are released,
            // so the key or button that closed it (or one held while it was open) does not reach the game.
            if (!idleIt && (g_holdMask & (1u << player)) != 0u)
            {
                if (inputReleased(state[player], rawStick, padConfigs()[player]))
                {
                    g_holdMask &= ~(1u << player);
                    if (player == 0u)
                        std::cerr << "[controls] input hold released for player 1 after "
                                  << std::chrono::duration_cast<std::chrono::milliseconds>(now - g_holdStart).count() << " ms" << std::endl;
                }
                else
                    idleIt = true;
            }
            if (idleIt)
            {
                PadInputState idle;
                idle.connected = state[player].connected;
                state[player] = idle;
            }
        }
        pollControlsFile();
        std::lock_guard<std::mutex> lock(g_mutex);
        g_state = state;
    }

    void setMouseStickEnabled(bool enabled)
    {
        g_mouseStickEnabled.store(enabled, std::memory_order_relaxed);
    }

    MouseCameraInput consumeMouseCameraInput()
    {
        MouseCameraInput input;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            input = g_mouseCameraInput;
            g_mouseCameraInput.x = 0.0f;
            g_mouseCameraInput.y = 0.0f;
            if (std::chrono::steady_clock::now() - g_mouseInputStart > std::chrono::milliseconds(100))
            {
                input.x = 0.0f;
                input.y = 0.0f;
            }
        }
        const uint32_t invert = rightStickInvert();
        input.x *= mouseSensitivity() * ((invert & kInvertRightX) != 0u ? -1.0f : 1.0f);
        input.y *= mouseSensitivity() * mouseVerticalSensitivity() * ((invert & kInvertRightY) != 0u ? -1.0f : 1.0f);
        return input;
    }

    void releaseMouse()
    {
        if (g_mouseLocked)
            EnableCursor();
        g_mouseLocked = false;
        g_ignoredMouseButton = -1;
        g_mouseMoved = false;
        recordMouseCameraInput({}, false);
    }

    void setBlocked(bool blocked)
    {
        g_blocked.store(blocked, std::memory_order_relaxed);
    }

    void setMenuHasMouse(bool menu)
    {
        g_menuMouse.store(menu, std::memory_order_relaxed);
    }

    // ---- Controls screen support (window thread) --------------------------------------------------------------------
    void setOverlayOpen(bool open)
    {
        const bool was = g_overlayOpen.exchange(open, std::memory_order_relaxed);
        const auto now = std::chrono::steady_clock::now();
        if (open)
            g_holdMask = 0u;
        else if (was)
        {
            g_holdMask = (1u << kMaxPlayers) - 1u;
            g_holdStart = now;
            g_holdUntil = now + std::chrono::seconds(2);
        }
    }

    bool overlayOpen()
    {
        return g_overlayOpen.load(std::memory_order_relaxed);
    }

    bool inputHeld(uint32_t player)
    {
        return player < kMaxPlayers && (g_holdMask & (1u << player)) != 0u;
    }

    void setChordGate(bool enabled)
    {
        g_chordGateEnabled.store(enabled, std::memory_order_relaxed);
    }

    void restoreShortcutKeys()
    {
        restoreAccessKeysFinal(); // sample() never clears them again
    }

    void setTestButtons(uint16_t buttons)
    {
        g_testButtons.store(buttons, std::memory_order_relaxed);
    }

    void setTestGateButtons(bool back, bool start)
    {
        g_testGateButtons.store(static_cast<uint8_t>((back ? 1u : 0u) | (start ? 2u : 0u)), std::memory_order_relaxed);
    }

    PadInputState sampledState(uint32_t player)
    {
        if (player >= kMaxPlayers)
            return {};
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_state[player];
    }

    void setControlsFile(const std::string &path, const std::vector<std::pair<std::string, std::string>> &legacy)
    {
        g_controlsPath = path;
        g_model = defaultModel();
        g_controlsDirty = false;
        g_haveLoadedStamp = false;
        if (!path.empty())
        {
            std::error_code error;
            if (!std::filesystem::exists(path, error))
            {
                // First run: the PS2X_PAD<n>=1 / PS2X_PAD<n>_<KEY>=a;b lines godhand.ini used to carry, else the defaults.
                constexpr std::string_view kSettingKeys[] = {"Type", "Deadzone", "AxisScale", "ButtonDeadzone",
                                                             "LargeMotorScale", "SmallMotorScale", "InvertL", "InvertR",
                                                             "LargeMotor", "SmallMotor"};
                bool fromLegacy = false;
                for (uint32_t pad = 0; pad < kMaxPlayers; ++pad)
                {
                    const std::string prefix = "PS2X_PAD" + std::to_string(pad + 1u);
                    const bool listed = std::any_of(legacy.begin(), legacy.end(), [&](const auto &entry) { return entry.first == prefix; });
                    if (!listed)
                        continue;
                    PadSection section;
                    section.present = true;
                    for (const auto &[variable, value] : legacy)
                    {
                        if (variable.size() <= prefix.size() + 1u || variable.compare(0, prefix.size() + 1u, prefix + "_") != 0)
                            continue;
                        std::string key = variable.substr(prefix.size() + 1u);
                        if (const auto index = bindingKeyIndex(key))
                            key = std::string(bindingKeyName(*index));
                        else
                            for (std::string_view setting : kSettingKeys)
                                if (lowerName(setting) == lowerName(key))
                                    key = std::string(setting);
                        addSectionLine(section, key, value);
                    }
                    g_model.pads[pad] = std::move(section);
                    fromLegacy = true;
                }
                std::string message;
                if (writeControlsFile(g_model, message))
                    std::cerr << "[controls] wrote " << path << (fromLegacy ? " from the godhand.ini PS2X_PAD lines" : " with the default layout") << std::endl;
                else
                    std::cerr << "[controls] " << message << "; using that layout without a file" << std::endl;
            }
            else
            {
                ControlsModel model;
                std::string message;
                if (parseControlsFile(path, model, message))
                {
                    g_model = std::move(model);
                    rememberFileStamp();
                    std::cerr << "[controls] loaded " << path << std::endl;
                }
                else
                {
                    g_lastFileError = message;
                    std::cerr << "[controls] " << message << "; the default layout is used until the file can be read" << std::endl;
                }
            }
        }
        rebuildConfigs();
        for (uint32_t pad = 0; pad < kMaxPlayers; ++pad)
            if (envPad(pad))
                std::cerr << "[controls] PS2X_PAD" << pad + 1u << " is set in the environment: player " << pad + 1u
                          << " uses the PS2X_PAD" << pad + 1u << "_* variables, not the controls file" << std::endl;
    }

    // ---- button prompts (any thread) ---------------------------------------------------------------------------------
    namespace prompts
    {
        uint32_t generation()
        {
            return g_promptGeneration.load(std::memory_order_acquire);
        }

        Snapshot snapshot()
        {
            std::lock_guard<std::mutex> lock(g_promptMutex);
            return g_prompts;
        }

        Device lastDevice()
        {
            return g_promptDevice.load(std::memory_order_relaxed);
        }

        PadStyle padStyle()
        {
            return g_promptStyle.load(std::memory_order_relaxed);
        }
    }

    namespace controls
    {
        namespace
        {
            constexpr std::array<std::pair<int, std::string_view>, 39> kKeyDisplay{{
                {KEY_ESCAPE, "Escape"}, {KEY_TAB, "Tab"}, {KEY_BACKSPACE, "Backspace"}, {KEY_ENTER, "Return"},
                {KEY_KP_ENTER, "Keypad+Enter"}, {KEY_INSERT, "Insert"}, {KEY_DELETE, "Delete"}, {KEY_PAUSE, "Pause"},
                {KEY_HOME, "Home"}, {KEY_END, "End"}, {KEY_LEFT, "Left"}, {KEY_UP, "Up"}, {KEY_RIGHT, "Right"},
                {KEY_DOWN, "Down"}, {KEY_PAGE_UP, "PageUp"}, {KEY_PAGE_DOWN, "PageDown"}, {KEY_CAPS_LOCK, "CapsLock"},
                {KEY_SPACE, "Space"}, {KEY_COMMA, "Comma"}, {KEY_MINUS, "Minus"}, {KEY_PERIOD, "Period"},
                {KEY_SLASH, "Slash"}, {KEY_SEMICOLON, "Semicolon"}, {KEY_EQUAL, "Equal"}, {KEY_LEFT_BRACKET, "BracketLeft"},
                {KEY_BACKSLASH, "Backslash"}, {KEY_RIGHT_BRACKET, "BracketRight"}, {KEY_APOSTROPHE, "Apostrophe"},
                {KEY_GRAVE, "QuoteLeft"}, {KEY_LEFT_SHIFT, "LeftShift"}, {KEY_RIGHT_SHIFT, "RightShift"},
                {KEY_LEFT_CONTROL, "LeftControl"}, {KEY_RIGHT_CONTROL, "RightControl"}, {KEY_LEFT_ALT, "LeftAlt"},
                {KEY_RIGHT_ALT, "RightAlt"}, {KEY_NUM_LOCK, "NumLock"}, {KEY_SCROLL_LOCK, "ScrollLock"},
                {KEY_PRINT_SCREEN, "Print"}, {KEY_KB_MENU, "Menu"},
            }};

            // Xbox-style names: GLFW (and SDL) report every controller with that layout.
            constexpr std::array<std::string_view, ps2_host_gamepads::ButtonCount> kPadButtonDisplay{
                "A", "B", "X", "Y", "Back", "Guide", "Start", "LS click", "RS click", "LB", "RB", "D-pad up",
                "D-pad down", "D-pad left", "D-pad right", "Misc", "Paddle 1", "Paddle 2", "Paddle 3", "Paddle 4", "Touchpad"};

            std::string keyName(int code)
            {
                if (code >= KEY_A && code <= KEY_Z)
                    return std::string(1, static_cast<char>('A' + (code - KEY_A)));
                if (code >= KEY_ZERO && code <= KEY_NINE)
                    return std::string(1, static_cast<char>('0' + (code - KEY_ZERO)));
                if (code >= KEY_KP_0 && code <= KEY_KP_9)
                    return "Keypad+" + std::string(1, static_cast<char>('0' + (code - KEY_KP_0)));
                if (code >= KEY_F1 && code <= KEY_F12)
                    return "F" + std::to_string(code - KEY_F1 + 1);
                if (code == kAnyShift)
                    return "Shift";
                if (code == kAnyControl)
                    return "Control";
                if (code == kAnyAlt)
                    return "Alt";
                for (const auto &[key, name] : kKeyDisplay)
                    if (key == code)
                        return std::string(name);
                return {};
            }

            std::string elementDisplay(const Element &element)
            {
                std::string text;
                switch (element.source)
                {
                case Source::Keyboard:
                    return keyName(element.code);
                case Source::Pointer:
                    return element.code == MOUSE_BUTTON_LEFT ? "Mouse left" : element.code == MOUSE_BUTTON_RIGHT ? "Mouse right" : "Mouse middle";
                case Source::PadButton:
                    text = std::string(kPadButtonDisplay[static_cast<size_t>(element.code)]);
                    break;
                case Source::PadAxis:
                {
                    const bool negative = element.sign < 0.0f;
                    switch (element.code)
                    {
                    case ps2_host_gamepads::LeftX: text = negative ? "LS left" : "LS right"; break;
                    case ps2_host_gamepads::LeftY: text = negative ? "LS up" : "LS down"; break;
                    case ps2_host_gamepads::RightX: text = negative ? "RS left" : "RS right"; break;
                    case ps2_host_gamepads::RightY: text = negative ? "RS up" : "RS down"; break;
                    case ps2_host_gamepads::LeftTrigger: text = negative ? "LT (-)" : "LT"; break;
                    default: text = negative ? "RT (-)" : "RT"; break;
                    }
                    break;
                }
                }
                if (element.device != 0)
                    text += " #" + std::to_string(element.device + 1);
                return text;
            }

            bool sameElement(const Element &a, const Element &b)
            {
                return a.source == b.source && a.device == b.device && a.code == b.code && a.sign == b.sign;
            }

            std::optional<Chord> parseChord(std::string_view binding)
            {
                Chord chord;
                for (std::string_view part : splitList(binding, '&'))
                {
                    const auto element = parseElement(part);
                    if (!element)
                        return std::nullopt;
                    chord.push_back(*element);
                }
                if (chord.empty())
                    return std::nullopt;
                return chord;
            }

            bool sameChord(const Chord &a, const Chord &b)
            {
                if (a.size() != b.size())
                    return false;
                for (size_t i = 0; i < a.size(); ++i)
                    if (!sameElement(a[i], b[i]))
                        return false;
                return true;
            }

            const Bindings *liveBindings(uint32_t pad, size_t index)
            {
                const PadConfig &config = padConfigs()[pad];
                return index < kPadButtonKeys.size() ? &config.buttons[index] : &config.sticks[index - kPadButtonKeys.size()];
            }
        }

        const std::string &path()
        {
            return g_controlsPath;
        }

        bool dirty()
        {
            return g_controlsDirty;
        }

        const std::string &fileError()
        {
            return g_lastFileError;
        }

        uint16_t buttonMask(std::string_view key)
        {
            const std::string name = lowerName(trimView(key));
            for (const auto &[button, mask] : kPadButtonKeys)
                if (name == lowerName(button))
                    return mask;
            return 0u;
        }

        bool envOverride(uint32_t pad)
        {
            return pad < kMaxPlayers && envPad(pad);
        }

        std::vector<std::string> bindings(uint32_t pad, std::string_view key)
        {
            const auto index = bindingKeyIndex(key);
            if (pad >= kMaxPlayers || !index)
                return {};
            if (!g_model.pads[pad].present)
                return defaultSection(pad).bindings[*index];
            return g_model.pads[pad].bindings[*index];
        }

        void setBindings(uint32_t pad, std::string_view key, std::vector<std::string> list)
        {
            const auto index = bindingKeyIndex(key);
            if (pad >= kMaxPlayers || !index)
                return;
            if (!g_model.pads[pad].present)
                g_model.pads[pad] = defaultSection(pad);
            g_model.pads[pad].bindings[*index] = std::move(list);
            g_controlsDirty = true;
            rebuildConfigs();
        }

        void resetDefaults(uint32_t pad)
        {
            if (pad >= kMaxPlayers)
                return;
            // The bindings only: the player's settings (Deadzone, AxisScale, Invert, motors, unknown keys) and notes stay.
            PadSection fresh = defaultSection(pad);
            if (g_model.pads[pad].present)
            {
                fresh.settings = g_model.pads[pad].settings;
                fresh.comments = g_model.pads[pad].comments;
            }
            g_model.pads[pad] = std::move(fresh);
            g_controlsDirty = true;
            rebuildConfigs();
        }

        bool save(std::string &message)
        {
            if (!g_controlsDirty)
            {
                message.clear();
                return true;
            }
            if (g_controlsPath.empty())
            {
                message = "No controls file set: changes last until the game closes.";
                return false;
            }
            // A change made outside the game while the screen was open (or edits were unsaved) is replaced: say so.
            std::string note;
            {
                std::error_code error;
                const auto time = std::filesystem::last_write_time(g_controlsPath, error);
                const auto size = error ? 0u : std::filesystem::file_size(g_controlsPath, error);
                if (!error && g_haveLoadedStamp && (time != g_loadedTime || size != g_loadedSize))
                    note = " (replaced a change made to the file outside the game)";
                else if (!error && !g_lastFileError.empty())
                    note = " (replaced the file that could not be read)";
            }
            if (!writeControlsFile(g_model, message))
                return false; // stays dirty: the next close (or the window closing) tries again
            message += note;
            g_controlsDirty = false;
            return true;
        }

        Kind kindOf(std::string_view binding)
        {
            const auto chord = parseChord(binding);
            if (!chord || chord->size() != 1u)
                return Kind::Other;
            const Source source = chord->front().source;
            return source == Source::Keyboard || source == Source::Pointer ? Kind::Keys : Kind::Controller;
        }

        bool sameBinding(std::string_view a, std::string_view b)
        {
            const auto first = parseChord(a);
            const auto second = parseChord(b);
            if (!first || !second)
                return lowerName(trimView(a)) == lowerName(trimView(b));
            return sameChord(*first, *second);
        }

        std::string displayName(std::string_view binding)
        {
            const auto chord = parseChord(binding);
            if (!chord)
                return std::string(trimView(binding)) + " (?)";
            std::string text;
            for (const Element &element : *chord)
                text += (text.empty() ? "" : " + ") + elementDisplay(element);
            return text;
        }

        std::string keyBinding(int code)
        {
            if (code == KEY_ESCAPE || (code >= KEY_F1 && code <= KEY_F12))
                return {}; // Esc cancels, F-keys are the port's shortcuts
            const std::string name = keyName(code);
            if (name.empty() || !keyCode(name))
                return {};
            return "Keyboard/" + name;
        }

        const std::vector<int> &bindableKeys()
        {
            static const std::vector<int> keys = []
            {
                std::vector<int> list;
                for (int code = KEY_A; code <= KEY_Z; ++code)
                    list.push_back(code);
                for (int code = KEY_ZERO; code <= KEY_NINE; ++code)
                    list.push_back(code);
                for (int code = KEY_KP_0; code <= KEY_KP_9; ++code)
                    list.push_back(code);
                for (const auto &[code, name] : kKeyDisplay)
                    if (code != KEY_ESCAPE)
                        list.push_back(code);
                return list;
            }();
            return keys;
        }

        std::string mouseBinding(int button)
        {
            switch (button)
            {
            case MOUSE_BUTTON_LEFT: return "Pointer-0/LeftButton";
            case MOUSE_BUTTON_RIGHT: return "Pointer-0/RightButton";
            case MOUSE_BUTTON_MIDDLE: return "Pointer-0/MiddleButton";
            default: return {};
            }
        }

        std::string padButtonBinding(int device, int button)
        {
            if (button < 0 || button >= ps2_host_gamepads::ButtonCount)
                return {};
            return "SDL-" + std::to_string(device) + "/" + std::string(kSdlButtonNames[static_cast<size_t>(button)]);
        }

        std::string padAxisBinding(int device, int axis, bool negative)
        {
            if (axis < 0 || axis >= ps2_host_gamepads::AxisCount)
                return {};
            return "SDL-" + std::to_string(device) + "/" + (negative ? "-" : "+") + std::string(kSdlAxisNames[static_cast<size_t>(axis)]);
        }

        bool liveBinding(uint32_t pad, std::string_view key, std::string_view binding)
        {
            const auto index = bindingKeyIndex(key);
            const auto chord = parseChord(binding);
            if (pad >= kMaxPlayers || !index || !chord)
                return false;
            for (const Chord &live : *liveBindings(pad, *index))
                if (sameChord(live, *chord))
                    return true;
            return false;
        }

        bool fileBindings(const std::string &file, uint32_t pad, std::string_view key, std::vector<std::string> &out, std::string &error)
        {
            ControlsModel model;
            const auto index = bindingKeyIndex(key);
            if (pad >= kMaxPlayers || !index)
            {
                error = "bad pad or key";
                return false;
            }
            if (!parseControlsFile(file, model, error))
                return false;
            if (!model.pads[pad].present)
            {
                error = "no [Pad" + std::to_string(pad + 1u) + "] section";
                return false;
            }
            out = model.pads[pad].bindings[*index];
            return true;
        }

        std::string defaultsDump(uint32_t pad)
        {
            const PadSection section = defaultSection(pad);
            std::string text;
            for (const auto &[key, value] : section.settings)
                text += key + "=" + value + "\n";
            for (size_t i = 0; i < kBindingKeyCount; ++i)
                for (const std::string &binding : section.bindings[i])
                    text += std::string(bindingKeyName(i)) + "=" + binding + "\n";
            return text;
        }
    }

    void setVibration(uint32_t port, uint32_t slot, uint8_t smallMotor, uint8_t largeMotor)
    {
        const int player = playerFor(port, slot);
        if (player < 0)
            return;
        const uint16_t motors = static_cast<uint16_t>((largeMotor << 8u) | (smallMotor != 0u ? 1u : 0u));
        const uint16_t previous = g_motors[static_cast<size_t>(player)].exchange(motors, std::memory_order_relaxed);
        static const bool log = [] { const char *v = std::getenv("PS2X_LOG_VIBRATION"); return v && *v && *v != '0'; }();
        if (log && previous != motors)
            std::cerr << "[input] player " << player + 1 << " vibration: small " << unsigned(smallMotor) << " large "
                      << unsigned(largeMotor) << std::endl;
    }

    std::vector<std::pair<std::string, std::string>> defaultPadSettings(uint32_t pad)
    {
        std::vector<std::pair<std::string, std::string>> settings{
            {"Type", "DualShock2"},
            {"InvertL", "0"},
            {"InvertR", "0"},
            {"Deadzone", "0"},
            {"AxisScale", "1.33"},
            {"ButtonDeadzone", "0"},
            {"LargeMotorScale", "1"},
            {"SmallMotorScale", "1"},
        };
        const std::string sdl = "SDL-" + std::to_string(pad) + "/";
        // Player 1 also has the keyboard. God Hand layout (2026-10-17, the godhand.ini PS2X_PAD1_* lines it replaces): D-pad
        // on the arrow keys (menus move with the D-pad), face buttons on I/L/K/J, left stick on WASD, Start = Return,
        // Select = Backspace, L1/R1 = Q/E, L2/R2 = 1/3, L3/R3 = F/G; the right stick (evade) has no keys by default.
        const bool keyboard = pad == 0u;
        auto add = [&](const char *key, const char *keyboardBinding, const char *padElement)
        {
            if (keyboard && keyboardBinding)
                settings.emplace_back(key, keyboardBinding);
            settings.emplace_back(key, sdl + padElement);
        };
        add("Up", "Keyboard/Up", "DPadUp");
        add("Right", "Keyboard/Right", "DPadRight");
        add("Down", "Keyboard/Down", "DPadDown");
        add("Left", "Keyboard/Left", "DPadLeft");
        add("Triangle", "Keyboard/I", "FaceNorth");
        add("Circle", "Keyboard/L", "FaceEast");
        add("Cross", "Keyboard/K", "FaceSouth");
        add("Square", "Keyboard/J", "FaceWest");
        add("Select", "Keyboard/Backspace", "Back");
        add("Start", "Keyboard/Return", "Start");
        add("L1", "Keyboard/Q", "LeftShoulder");
        add("L2", "Keyboard/1", "+LeftTrigger");
        add("R1", "Keyboard/E", "RightShoulder");
        add("R2", "Keyboard/3", "+RightTrigger");
        add("L3", "Keyboard/F", "LeftStick");
        add("R3", "Keyboard/G", "RightStick");
        add("LUp", "Keyboard/W", "-LeftY");
        add("LRight", "Keyboard/D", "+LeftX");
        add("LDown", "Keyboard/S", "+LeftY");
        add("LLeft", "Keyboard/A", "-LeftX");
        add("RUp", nullptr, "-RightY");
        add("RRight", nullptr, "+RightX");
        add("RDown", nullptr, "+RightY");
        add("RLeft", nullptr, "-RightX");
        settings.emplace_back("LargeMotor", sdl + "LargeMotor");
        settings.emplace_back("SmallMotor", sdl + "SmallMotor");
        return settings;
    }

    ps2x::iop::PadInputState padState(uint32_t port, uint32_t slot)
    {
        const int player = playerFor(port, slot);
        if (player < 0)
            return {};
        PadInputState state;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            state = g_state[static_cast<size_t>(player)];
        }
        {
            const std::atomic<uint64_t> *counter = g_fieldCounter.load();
            const uint64_t tick = counter ? counter->load() : 0u;
            const ScriptedInput scripted = player == 0 ? scriptedInput(tick) : playerScriptedInput(static_cast<uint32_t>(player), tick);
            if (scripted.active)
            {
                state.connected = true;
                state.buttons |= scripted.buttons;
                if (scripted.axes[0] != 0x80u)
                    state.leftX = scripted.axes[0];
                if (scripted.axes[1] != 0x80u)
                    state.leftY = scripted.axes[1];
                if (scripted.axes[2] != 0x80u)
                    state.rightX = scripted.axes[2];
                if (scripted.axes[3] != 0x80u)
                    state.rightY = scripted.axes[3];
                applyPressure(state);
            }
        }
        return state;
    }
}
