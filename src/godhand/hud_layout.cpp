// Widescreen HUD layouts (see hud_layout.h). Hooks in recomp/godhand.toml (flag "hud2d", always on: the menu
// detection runs at 4:3 too; the narrowing only when wide):
//
//   Gp_draw_line/poly3/poly4/polyFT4/polyG4 and the three sprite drawers (0x15B138..0x15DDF8): after each converts
//   its vertices to GS coordinates (x = (screen x + 0x800 - width/2) * 16, the screen width at 0x3C1180), the x of
//   the n vertices at $s0+0x10.. is narrowed. The same instruction also ends their 3D path, where $s0 is a counter
//   (0); only a $s0 inside the stack frame is the 2D vertex array. func_0015C410 (strips: health gauge, roulette dial)
//   is hooked per vertex ($a3 before it is stored).
//
//   func_002A53A8 (text glyphs and the button icons inside text): its two corners at 0x2A5704, before the offset.
//
//   cIDBase_trans (0x2AAC28, every cIDBase layout): layouts drawn by the in-game HUD widgets (isHudCaller, by the
//   caller's return address) are groups that keep to the screen edges; all other 2D (menus, pause and status
//   screens, text windows) is narrowed toward the screen centre, so it looks as on the PS2 in the middle 4:3.
//
//   cCockPlBar, cCockRader and cCockRoulette trans: the whole widget is one HUD group, including their own gauge,
//   blip and dial draws and every layout they draw.
//
// F6 writes every 2D primitive of the next frame to the log (drawer, caller, group, anchor, x before and after).
//
// A HUD group's anchor comes from its x extent in the previous frame (before narrowing): narrower than 12% of the
// screen = its own centre (button prompts over things in the 3D view); else by the third its centre is in: the
// screen's left edge, centre or right edge.
#include "godhand/hud_layout.h"
#include "godhand/log.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/gs/gs_aspect.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace godhand::hud
{
    namespace
    {
        enum class Layout
        {
            Anchored,
            Centered,
        };

        constexpr uint32_t kScreenWidth = 0x3C1180u;
        constexpr uint32_t kScreenYOffset = 0x586B80u;
        constexpr uint32_t kPlayerBar = 0xF0000001u, kRadar = 0xF0000002u, kRoulette = 0xF0000003u;

        Layout g_layout = Layout::Anchored;

        // Menus and screens (the pause menu, options, the roulette setup...) are drawn for 4:3 and partly through
        // drawers the 2D narrowing does not reach, so their layers cannot be lined up by narrowing. A frame that
        // draws a full-screen background inside a non-HUD layout is a menu frame: then nothing is narrowed and the
        // picture is shown 4:3 (pillarboxed), or stretched with GODHAND_MENUS=stretch ([Video] MenuLayout).
        enum class MenuMode
        {
            Pillarbox,
            Stretch,
        };
        MenuMode g_menuMode = MenuMode::Pillarbox;
        bool g_menuSeen = false;  // a menu background this frame
        int g_menuHold = 0;       // frames the menu state lasts without one (covers a few frames of transitions)
        bool g_menuAspect = false; // the picture is forced to 4:3 now

        void setMenuAspect(bool on)
        {
            if (on == g_menuAspect)
                return;
            g_menuAspect = on;
            ps2x::gs::setAspect(on ? ps2x::gs::Aspect::Force4_3 : ps2x::gs::Aspect::R4_3);
        }

        bool menuNow() { return g_menuSeen || g_menuHold > 0; }
        bool g_wide = false;
        float g_k = 1.0f;
        float g_edgeShare = 0.0f; // HUD edges: the 16:9 area's edges, as a share of the half width from the centre
        uint64_t g_frame = 0;

        struct Extent
        {
            float x0, x1;
        };
        std::unordered_map<uint32_t, Extent> g_seen, g_last; // group -> x extent this frame / last frame

        struct Context
        {
            bool active = false;
            uint32_t key = 0;
            float anchor = 0.0f;
        };
        Context g_ctx; // what the draws use

        enum class Region
        {
            None,
            Forced,  // everything inside is one group
            Inherit, // own draws take the group of the layout entry drawn last
        };
        Region g_region = Region::None;
        Context g_regionCtx, g_lastEntry;
        bool g_inIdTrans = false;

        struct Loose
        {
            uint32_t prims = 0;
            float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
        };
        std::unordered_map<uint32_t, Loose> g_loose; // anchored: draws outside any group, by caller

        uint32_t read32(const uint8_t *rdram, uint32_t address)
        {
            uint32_t value;
            std::memcpy(&value, rdram + (address & (PS2_RAM_SIZE - 1u)), sizeof(value));
            return value;
        }

        void write32(uint8_t *rdram, uint32_t address, uint32_t value)
        {
            std::memcpy(rdram + (address & (PS2_RAM_SIZE - 1u)), &value, sizeof(value));
        }

        float screenWidth(const uint8_t *rdram)
        {
            const int32_t width = static_cast<int32_t>(read32(rdram, kScreenWidth));
            return width > 0 && width < 4096 ? static_cast<float>(width) : 640.0f;
        }

        // GS x (12.4 fixed, with the 2048 - width/2 offset) <-> screen x.
        int32_t gsOffset(const uint8_t *rdram)
        {
            const int32_t width = static_cast<int32_t>(read32(rdram, kScreenWidth));
            return (0x800 - width / 2) * 16;
        }

        // Small groups (button prompts, icons, often placed on things in the 3D view) keep their own centre. Others
        // are narrowed toward the screen edge or centre of the third they are in: every group in one third gets the
        // same mapping, so layouts drawn over each other (a menu's background and its highlight) stay lined up.
        // The HUD's edges: at 16:9 the screen's edges; wider screens keep the HUD at the edges of a 16:9 area in the
        // middle; narrower than 16:9 (and the 4:3 layout) puts the HUD where the PS2 had it.
        float anchorFromExtent(float x0, float x1, float width)
        {
            const float span = x1 - x0, centre = 0.5f * (x0 + x1);
            if (span < 0.12f * width)
                return centre;
            if (g_layout != Layout::Anchored || g_edgeShare <= 0.0f)
                return 0.5f * width;
            const float left = 0.5f * width * (1.0f - g_edgeShare);
            if (centre < width / 3.0f)
                return left;
            if (centre > 2.0f * width / 3.0f)
                return width - left;
            return 0.5f * width;
        }

        float anchorFor(uint32_t key, float fallback, float width)
        {
            auto last = g_last.find(key);
            if (last != g_last.end())
                return anchorFromExtent(last->second.x0, last->second.x1, width);
            auto seen = g_seen.find(key);
            if (seen != g_seen.end())
                return anchorFromExtent(seen->second.x0, seen->second.x1, width);
            return fallback;
        }

        void extend(uint32_t key, float x0, float x1)
        {
            auto [it, fresh] = g_seen.try_emplace(key, Extent{x0, x1});
            if (!fresh)
            {
                it->second.x0 = std::min(it->second.x0, x0);
                it->second.x1 = std::max(it->second.x1, x1);
            }
        }

        void noteLoose(uint32_t caller, float x0, float x1, float y0, float y1)
        {
            Loose &loose = g_loose[caller];
            ++loose.prims;
            loose.x0 = std::min(loose.x0, x0);
            loose.x1 = std::max(loose.x1, x1);
            loose.y0 = std::min(loose.y0, y0);
            loose.y1 = std::max(loose.y1, y1);
        }

        // F6: every 2D primitive of one frame written to the log (which drawer, caller, group, anchor, x before/after).
        bool g_dumpArmed = false, g_dumping = false;
        uint32_t g_entry = 0; // the cIDBase entry being drawn
        std::vector<std::string> g_dump;

        void dumpPrim(const char *tag, uint32_t caller, float x0, float x1, float y0, float y1, const char *what, float anchor)
        {
            if (!g_dumping || g_dump.size() >= 1500u)
                return;
            char line[200];
            const float n0 = std::isnan(anchor) ? x0 : anchor + (x0 - anchor) * g_k;
            const float n1 = std::isnan(anchor) ? x1 : anchor + (x1 - anchor) * g_k;
            std::snprintf(line, sizeof(line), "%-6s ra %08X grp %08X ent %08X  x %6.1f..%6.1f -> %6.1f..%6.1f  y %6.1f..%6.1f  %s a=%.1f",
                          tag, caller, g_ctx.active ? g_ctx.key : 0u, g_inIdTrans ? g_entry : 0u, x0, x1, n0, n1, y0, y1, what,
                          std::isnan(anchor) ? -1.0f : anchor);
            g_dump.emplace_back(line);
        }

        // n vertices, x at address + i*16 (y at +4), in GS units: screen*16 + offset (offset 0 for the font drawer).
        void narrowVertices(uint8_t *rdram, uint32_t address, int n, uint32_t caller, const char *tag, bool withOffset = true)
        {
            const float width = screenWidth(rdram);
            const int32_t offset = withOffset ? gsOffset(rdram) : 0;
            const int32_t yOffset = withOffset ? static_cast<int32_t>(read32(rdram, kScreenYOffset)) * 16 : 0;
            float xs[4], x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
            for (int i = 0; i < n; ++i)
            {
                xs[i] = static_cast<float>(static_cast<int32_t>(read32(rdram, address + 16u * i)) - offset) / 16.0f;
                const float y = static_cast<float>(static_cast<int32_t>(read32(rdram, address + 16u * i + 4u)) - yOffset) / 16.0f;
                x0 = std::min(x0, xs[i]);
                x1 = std::max(x1, xs[i]);
                y0 = std::min(y0, y);
                y1 = std::max(y1, y);
            }
            const float none = std::nanf("");
            if (n >= 2 && x1 - x0 >= 0.9f * width)
            {
                // Full height too: letterbox bars in cutscenes are full width but thin.
                const float height = 448.0f; // the game's 2D screen (0..448, NTSC)
                if (g_inIdTrans && !g_ctx.active && !g_menuSeen && y1 - y0 >= 0.8f * height)
                {
                    g_menuSeen = true; // a menu's background (fades are drawn outside any layout)
                    if (g_wide && g_menuMode == MenuMode::Pillarbox)
                        setMenuAspect(true);
                }
                dumpPrim(tag, caller, x0, x1, y0, y1, "fullwidth", none);
                return; // full-screen quad: backgrounds, fades
            }
            if (!g_wide || menuNow())
            {
                dumpPrim(tag, caller, x0, x1, y0, y1, "menu", none);
                return;
            }
            float anchor;
            if (g_ctx.active)
            {
                extend(g_ctx.key, x0, x1);
                anchor = g_ctx.anchor;
            }
            else
            {
                if (!g_inIdTrans)
                    noteLoose(caller, x0, x1, y0, y1);
                anchor = 0.5f * width; // menus and everything else: the PS2's 4:3 layout, in the middle
            }
            dumpPrim(tag, caller, x0, x1, y0, y1, g_ctx.active ? "hud" : "centred", anchor);
            (void)none;
            for (int i = 0; i < n; ++i)
            {
                const float x = anchor + (xs[i] - anchor) * g_k;
                write32(rdram, address + 16u * i, static_cast<uint32_t>(static_cast<int32_t>(std::lround(x * 16.0f)) + offset));
            }
        }

        // After the vertex loop of a Gp_draw_* function: n vertices at $s0+0x10, the caller's $ra saved at sp+raSlot.
        template <int N, uint32_t RaSlot>
        void gpVertices(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t sp = GPR_U32(ctx, 29), s0 = GPR_U32(ctx, 16);
            const int32_t delta = static_cast<int32_t>(s0 - sp);
            if (delta < -0x20 || delta > 0x100)
                return; // the 3D path
            narrowVertices(rdram, s0 + 0x10u, N, read32(rdram, sp + RaSlot), N == 2 ? "sprite" : N == 3 ? "poly3" : "quad");
        }

        // func_002A53A8 (text glyphs and inline button icons) at 0x2A5704: its two corners converted (screen*16, the
        // offset not added yet) at sp+0x20 and sp+0x30; caller's $ra at sp+0x40.
        void fontSprite(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t sp = GPR_U32(ctx, 29);
            narrowVertices(rdram, sp + 0x20u, 2, read32(rdram, sp + 0x40u), "glyph", false);
        }

        // func_0015C410 before "sw $a3,0x40($sp)": one strip vertex's GS x in $a3.
        void stripVertex(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            if (!g_wide || menuNow())
                return;
            const float width = screenWidth(rdram);
            const int32_t offset = gsOffset(rdram);
            const float xs = static_cast<float>(GPR_S32(ctx, 7) - offset) / 16.0f;
            const uint32_t caller = read32(rdram, GPR_U32(ctx, 29) + 0x70u);
            float anchor;
            if (g_ctx.active)
            {
                extend(g_ctx.key, xs, xs);
                anchor = g_ctx.anchor;
            }
            else
            {
                if (!g_inIdTrans)
                    noteLoose(caller, xs, xs, 0.0f, 0.0f);
                anchor = 0.5f * width;
            }
            dumpPrim("strip", caller, xs, xs, 0.0f, 0.0f, g_ctx.active ? "hud" : "centred", anchor);
            const float x = anchor + (xs - anchor) * g_k;
            SET_GPR_S32(ctx, 7, static_cast<int32_t>(std::lround(x * 16.0f)) + offset);
        }

        // Return addresses of the cIDBase_trans calls in the HUD widgets' trans functions: cCockActButton (button
        // prompts, 0x13C168), cCockBossBar (0x13CF20), cCockItem (0x13E6E0), cCockLastBossBar (0x13EBA0),
        // cCockMiddleBoss (0x140638). cCockPlBar, cCockRader and cCockRoulette are regions of their own.
        bool isHudCaller(uint32_t ra)
        {
            switch (ra)
            {
            case 0x0013C18Cu:
            case 0x0013CFA0u:
            case 0x0013E728u:
            case 0x0013E73Cu:
            case 0x0013EC38u:
            case 0x001406CCu:
            case 0x001406FCu:
                return true;
            default:
                return false;
            }
        }

        // cIDBase_trans at the start of its entry loop: $s2 = the layout. One layout is one group: its entries (icons,
        // labels, frames) are designed around each other.
        void idBegin(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            g_inIdTrans = true;
            if (g_region == Region::Forced)
                return; // g_ctx is the region's
            // Only the in-game HUD widgets (by who called cIDBase_trans) keep to the screen edges; every other layout
            // (menus, the pause and status screens) is drawn as on the PS2, in the middle 4:3 of the screen.
            const uint32_t caller = read32(rdram, GPR_U32(ctx, 29) + 0x40u);
            if (!isHudCaller(caller))
            {
                g_ctx = Context{};
                return;
            }
            const uint32_t layout = GPR_U32(ctx, 18) & 0x01FFFFFFu;
            const float width = screenWidth(rdram);
            float fallback = 0.5f * width;
            if (g_last.find(layout) == g_last.end() && g_seen.find(layout) == g_seen.end())
            {
                const int32_t count = static_cast<int32_t>(read32(rdram, layout + 0xCu));
                const uint32_t entries = read32(rdram, layout + 0x4u);
                float x0 = 1e9f, x1 = -1e9f;
                for (int32_t i = 0; entries != 0u && i < count && i < 512; ++i)
                {
                    float x;
                    const uint32_t bits = read32(rdram, entries + 0xACu * static_cast<uint32_t>(i) + 0x50u);
                    std::memcpy(&x, &bits, sizeof(x));
                    if (!std::isfinite(x) || x < -width || x > 2.0f * width)
                        continue;
                    x0 = std::min(x0, x);
                    x1 = std::max(x1, x);
                }
                if (x0 <= x1)
                    fallback = anchorFromExtent(x0, x1, width);
            }
            g_ctx = Context{true, layout, anchorFor(layout, fallback, width)};
            g_lastEntry = g_ctx;
        }

        // cIDBase_trans, before an entry is drawn: $a1 = the entry (only noted, for the F6 dump).
        void idEntry(uint8_t *, R5900Context *ctx, PS2Runtime *) { g_entry = GPR_U32(ctx, 5); }

        void idEnd(uint8_t *, R5900Context *, PS2Runtime *)
        {
            g_inIdTrans = false;
            g_ctx = g_region == Region::Forced ? g_regionCtx : g_region == Region::Inherit ? g_lastEntry : Context{};
        }

        template <uint32_t Key, Region Mode>
        void regionBegin(uint8_t *rdram, R5900Context *, PS2Runtime *)
        {
            g_region = Mode;
            g_lastEntry = Context{};
            if (Mode == Region::Forced)
            {
                const float width = screenWidth(rdram);
                g_regionCtx = Context{true, Key, anchorFor(Key, 0.5f * width, width)};
                g_ctx = g_regionCtx;
            }
            else
                g_ctx = Context{};
        }

        void regionEnd(uint8_t *, R5900Context *, PS2Runtime *)
        {
            g_region = Region::None;
            g_ctx = Context{};
            g_lastEntry = Context{};
        }

        const char *layoutName(Layout layout)
        {
            switch (layout)
            {
            case Layout::Anchored:
                return "16:9 (HUD at the edges)";
            default:
                return "4:3 (HUD where the PS2 had it)";
            }
        }

        Layout parseLayout(const char *text)
        {
            if (!text)
                return Layout::Anchored;
            std::string value(text);
            for (char &c : value)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (value == "4:3" || value == "classic" || value == "centered" || value == "centred" || value == "center" ||
                value == "centre")
                return Layout::Centered;
            return Layout::Anchored; // "16:9", "wide", and the old "anchored" / "off"
        }

        bool keyPressed(int key, bool &wasDown)
        {
#if defined(_WIN32)
            const bool down = (GetAsyncKeyState(key) & 0x8000) != 0;
            const bool pressed = down && !wasDown;
            wasDown = down;
            if (!pressed)
                return false;
            DWORD process = 0;
            GetWindowThreadProcessId(GetForegroundWindow(), &process);
            return process == GetCurrentProcessId();
#else
            (void)key;
            (void)wasDown;
            return false;
#endif
        }

        void reportLoose()
        {
            if (g_loose.empty())
                return;
            std::vector<std::pair<uint32_t, Loose>> rows(g_loose.begin(), g_loose.end());
            std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b)
                      { return a.second.prims > b.second.prims; });
            std::ostringstream out;
            out << "HUD (anchored): 2D drawn outside any layout (centred), by caller (x,y range):";
            const size_t shown = std::min<size_t>(rows.size(), 16);
            for (size_t i = 0; i < shown; ++i)
            {
                char line[160];
                std::snprintf(line, sizeof(line), "\n    %08X %6u prims  x %.0f..%.0f  y %.0f..%.0f", rows[i].first,
                              rows[i].second.prims, rows[i].second.x0, rows[i].second.x1, rows[i].second.y0, rows[i].second.y1);
                out << line;
            }
            GH_INFO(Game, out.str());
            g_loose.clear();
        }
    }

    // All text and gauges go through the 2D narrowing: the pnach's own font and gauge narrowing is not used.
    bool narrowFont() { return false; }
    bool narrowGauge() { return false; }

    bool menuActive() { return menuNow(); }

    void onFrame(uint8_t *)
    {
        g_menuHold = g_menuSeen ? 8 : std::max(0, g_menuHold - 1);
        g_menuSeen = false;
        if (g_wide)
            setMenuAspect(g_menuHold > 0 && g_menuMode == MenuMode::Pillarbox);
        if (!g_wide)
        {
            g_ctx = Context{};
            g_region = Region::None;
            g_inIdTrans = false;
            return;
        }
        g_last.swap(g_seen);
        g_seen.clear();
        g_ctx = Context{};
        g_region = Region::None;
        g_inIdTrans = false;
        if (g_dumping)
        {
            std::ostringstream out;
            out << "HUD F6 dump (" << layoutName(g_layout) << ", k " << g_k << "): " << g_dump.size() << " 2D primitives";
            for (const std::string &line : g_dump)
                out << "\n    " << line;
            GH_INFO(Game, out.str());
            g_dump.clear();
            g_dumping = false;
        }
        if (g_dumpArmed)
        {
            g_dumpArmed = false;
            g_dumping = true;
        }
#if defined(_WIN32)
        static bool f7 = false, f6 = false;
        if (keyPressed(VK_F6, f6))
            g_dumpArmed = true;
        if (keyPressed(VK_F7, f7))
#else
        if (false)
#endif
        {
            g_layout = g_layout == Layout::Anchored ? Layout::Centered : Layout::Anchored;
            g_loose.clear();
            GH_INFO(Game, "HUD layout (F7): " << layoutName(g_layout));
        }
        if (++g_frame % 600u == 0u && g_layout == Layout::Anchored)
            reportLoose();
    }

    void install(bool wide, float k)
    {
        g_wide = wide;
        g_k = k;
        ps2xSetPatchFlag("hud2d", true);
        // k = (4/3) / aspect; the 16:9 area takes (16/9) / aspect of the width. Below 16:9 the HUD has no edges.
        const float aspect = (4.0f / 3.0f) / k;
        g_edgeShare = aspect >= 16.0f / 9.0f - 0.01f ? std::min(1.0f, (16.0f / 9.0f) / aspect) : 0.0f;
        g_layout = parseLayout(std::getenv("GODHAND_HUD"));
        {
            const char *menus = std::getenv("GODHAND_MENUS");
            std::string value = menus ? menus : "";
            for (char &c : value)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            g_menuMode = value == "stretch" || value == "stretched" ? MenuMode::Stretch : MenuMode::Pillarbox;
        }
        ps2xRegisterHostHook("gh_hud_line", &gpVertices<2, 0x70u>);
        ps2xRegisterHostHook("gh_hud_poly3", &gpVertices<3, 0x80u>);
        ps2xRegisterHostHook("gh_hud_poly4", &gpVertices<4, 0x90u>);
        ps2xRegisterHostHook("gh_hud_polyft4", &gpVertices<4, 0xA0u>);
        ps2xRegisterHostHook("gh_hud_polyg4", &gpVertices<4, 0x90u>);
        ps2xRegisterHostHook("gh_hud_sprite_d168", &gpVertices<2, 0x40u>);
        ps2xRegisterHostHook("gh_hud_sprite_d6e8", &gpVertices<2, 0x40u>);
        ps2xRegisterHostHook("gh_hud_sprite_db30", &gpVertices<2, 0x30u>);
        ps2xRegisterHostHook("gh_hud_strip", &stripVertex);
        ps2xRegisterHostHook("gh_hud_font_sprite", &fontSprite);
        ps2xRegisterHostHook("gh_hud_id_begin", &idBegin);
        ps2xRegisterHostHook("gh_hud_id_entry", &idEntry);
        ps2xRegisterHostHook("gh_hud_id_end", &idEnd);
        ps2xRegisterHostHook("gh_hud_plbar_begin", &regionBegin<kPlayerBar, Region::Forced>);
        ps2xRegisterHostHook("gh_hud_radar_begin", &regionBegin<kRadar, Region::Forced>);
        ps2xRegisterHostHook("gh_hud_roulette_begin", &regionBegin<kRoulette, Region::Forced>);
        ps2xRegisterHostHook("gh_hud_region_end", &regionEnd);
        if (wide)
            GH_INFO(Game, "HUD layout: " << layoutName(g_layout) << " (F7 switches 16:9 / 4:3 HUD; F6 writes one frame's 2D to the log)");
    }
}
