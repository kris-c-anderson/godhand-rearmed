#pragma once

#include "ps2x/iop/iop_pad.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ps2_host_input
{
    constexpr uint32_t kPadPorts = 2u;
    // Two controller ports, each with a multitap (PS2X_MULTITAP): up to 8 players.
    constexpr uint32_t kMaxPlayers = 8u;

    void setFieldCounter(const std::atomic<uint64_t> *counter);
    void sample();
    void releaseMouse();
    // The pad at a controller port and multitap slot (0 = A, the pad plugged straight in).
    [[nodiscard]] ps2x::iop::PadInputState padState(uint32_t port, uint32_t slot = 0u);
    // The motors the game set on that pad (any thread); played on the controllers of its LargeMotor/SmallMotor bindings.
    void setVibration(uint32_t port, uint32_t slot, uint8_t smallMotor, uint8_t largeMotor);
    // While blocked (the in-game menu is open) every pad stays plugged in but idle, so the game keeps running.
    void setBlocked(bool blocked);
    // While set (the in-game menu is open and lets the game keep the controllers and keys), the mouse is the menu's:
    // no mouse camera, no mouse-button bindings, the cursor stays free.
    void setMenuHasMouse(bool menu);

    // Controls screen (F1, ps2_controls_screen.cpp; window thread). Open: every pad idle but plugged in. Closed again:
    // each player stays idle until its own buttons and sticks are released (at most 2 s).
    void setOverlayOpen(bool open);
    [[nodiscard]] bool overlayOpen();
    [[nodiscard]] bool inputHeld(uint32_t player);
    // Sweep CR-5: while on, a controller's Back or Start is held back from the game for a moment (the "Back+Start gate"
    // in ps2_host_input.cpp), so pressing one of them first does not reach the game before Back+Start opens the screen.
    // The Controls screen sets it every frame: on while it can be opened (drawable renderer, focused window).
    void setChordGate(bool enabled);
    // Sweep CR-9: Windows' Sticky/Toggle/Filter Keys hotkeys are off while the game window has the focus (sample()
    // follows the focus). This puts the user's settings back for good: the window closed or the run ended. Any thread.
    void restoreShortcutKeys();

    // The bindings model behind the Controls screen (window thread only). Keys are PCSX2's [PadN] keys ("Cross",
    // "LUp"), bindings PCSX2 strings ("Keyboard/K", "SDL-0/FaceSouth", "Pointer-0/LeftButton").
    namespace controls
    {
        enum class Kind
        {
            Keys,       // one keyboard key or mouse button
            Controller, // one controller button or axis half
            Other       // a chord or a binding this build cannot read (kept as written)
        };
        [[nodiscard]] const std::string &path(); // empty: no file, edits last until the game closes
        [[nodiscard]] bool dirty();
        [[nodiscard]] const std::string &fileError(); // why the file could not be read (empty: it was read)
        [[nodiscard]] bool envOverride(uint32_t pad); // PS2X_PAD<n> in the environment wins over the file
        [[nodiscard]] std::vector<std::string> bindings(uint32_t pad, std::string_view key);
        void setBindings(uint32_t pad, std::string_view key, std::vector<std::string> list); // live at once, unsaved
        void resetDefaults(uint32_t pad);
        // Writes the file when something changed (tmp + replacing rename, retried); false = still unsaved.
        bool save(std::string &message);
        [[nodiscard]] Kind kindOf(std::string_view binding);
        [[nodiscard]] bool sameBinding(std::string_view a, std::string_view b);
        [[nodiscard]] std::string displayName(std::string_view binding);
        [[nodiscard]] std::string keyBinding(int raylibKey); // empty: not bindable (Esc, F-keys, unnamed keys)
        [[nodiscard]] const std::vector<int> &bindableKeys();
        [[nodiscard]] std::string mouseBinding(int button);
        [[nodiscard]] std::string padButtonBinding(int device, int button);
        [[nodiscard]] std::string padAxisBinding(int device, int axis, bool negative);
        // Tests: whether the live table holds that binding; a file's bindings for one key; the default layout.
        [[nodiscard]] bool liveBinding(uint32_t pad, std::string_view key, std::string_view binding);
        bool fileBindings(const std::string &file, uint32_t pad, std::string_view key, std::vector<std::string> &out, std::string &error);
        [[nodiscard]] std::string defaultsDump(uint32_t pad);
    }

    // Button prompts (any thread). What player 1's bindings show where the game draws a PS2 button: for each input the
    // first keyboard/mouse binding and the first controller binding, i.e. the Controls screen's Key 1 and Controller
    // columns, taken from the live table (so PS2X_PAD1 in the environment counts too). Republished by every bindings
    // rebuild: the file load, the 1-s file poll, F1 edits and reset. Readers compare generation() with the one they
    // last saw and take snapshot() only when it moved; nothing here allocates.
    namespace prompts
    {
        // The 24 PS2 inputs, in Tokyo Jungle's pad_prompt_art.h PadPromptButton order.
        enum Input : uint8_t
        {
            Cross,
            Circle,
            Square,
            Triangle,
            L1,
            R1,
            L2,
            R2,
            L3,
            R3,
            Start,
            Select,
            Up,
            Down,
            Left,
            Right,
            LUp,
            LDown,
            LLeft,
            LRight,
            RUp,
            RDown,
            RLeft,
            RRight,
            InputCount
        };

        enum class Device : uint8_t
        {
            Keys,      // keyboard and mouse
            Controller
        };

        enum class PadStyle : uint8_t
        {
            Unknown, // no controller yet, or a name that says neither (positional layout: draw it like Xbox)
            Xbox,
            PlayStation
        };

        enum class Type : uint8_t
        {
            None, // nothing of that kind is bound to the input (labels empty)
            Key,
            Mouse,
            PadButton,
            PadAxis
        };

        // Key codes for "Keyboard/Shift", "Control", "Alt" (either side); every other key is its raylib KEY_* code.
        constexpr int16_t kKeyAnyShift = -1;
        constexpr int16_t kKeyAnyControl = -2;
        constexpr int16_t kKeyAnyAlt = -3;
        constexpr size_t kLabelBytes = 8u; // UTF-8, with the terminating 0

        struct Binding
        {
            Type type = Type::None;
            int8_t device = -1; // controller number (SDL-<n>); -1 for keys and mouse buttons
            int8_t sign = 0;    // PadAxis: +1 the positive half (a whole trigger), -1 the negative half
            int16_t code = 0;   // raylib KEY_* (or kKeyAny*), MOUSE_BUTTON_*, ps2_host_gamepads::Button or ::Axis
            // Short names: "F", "Space", "Ctrl", "Bksp", "Num8", "M1" (mouse left), "A", "LB", "LT", "View", "Menu".
            // Arrow keys, D-pad and stick directions use the arrows U+2190-U+2193 (up = "\xE2\x86\x91"; D-pad up is
            // "D" + up arrow, left stick left "LS" + left arrow), which pad_prompt_art draws as vector arrows.
            char label[kLabelBytes]{};
            char compact[kLabelBytes]{}; // at most 4 characters, for small cells: "Spc", "Shft", "N8"
        };

        struct Snapshot
        {
            uint32_t generation = 0u; // 0: not published yet
            std::array<Binding, InputCount> keys{};
            std::array<Binding, InputCount> controller{}; // Xbox-style names whatever the pad (see padStyle())
        };

        [[nodiscard]] uint32_t generation(); // bumped when a rebuild changed any label (never 0 once published)
        [[nodiscard]] Snapshot snapshot();   // a copy (~1 KB) under a short lock
        // The device player 1 last pressed a bound input with (past half: stick drift and unbound buttons never count),
        // at most one change per 500 ms, only while the window has focus. Starts as Keys.
        [[nodiscard]] Device lastDevice();
        // From the name of the controller that last pressed a bound input (before any press: player 1's first
        // controller binding's controller); Unknown while that controller is not connected.
        [[nodiscard]] PadStyle padStyle();
    }
}
