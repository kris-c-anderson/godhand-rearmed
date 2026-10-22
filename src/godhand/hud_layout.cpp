// Widescreen HUD layouts (see hud_layout.h). Hooks in recomp/godhand.toml (flag "widescreen"):
//
//   Gp_draw_line/poly3/poly4/polyFT4/polyG4 and the three sprite drawers (0x15B138..0x15DDF8): after each converts
//   its vertices to GS coordinates (x = (screen x + 0x800 - width/2) * 16, the screen width at 0x3C1180), the x of
//   the n vertices at $s0+0x10.. is narrowed. The same instruction also ends their 3D path, where $s0 is a counter
//   (0); only a $s0 inside the stack frame is the 2D vertex array. func_0015C410 (strips: health gauge, roulette dial)
//   is hooked per vertex ($a3 before it is stored).
//
//   cIDBase_trans (0x2AAC28, every cIDBase layout: HUD widgets, menus, button prompts): before each entry is drawn
//   ($a1 = the entry, 0xAC bytes, parent at +0x20, parent index +0x29 < 0 for a root, final x at +0x50) the context
//   becomes that entry's root, so all quads of one widget get one anchor.
//
//   cCockPlBar / cCockRader trans (whole widget one group, including their own gauge and blip draws) and cCockRoulette
//   trans (its dial fill takes the group of the layout drawn just before it).
//
// An anchor comes from the group's x extent in the previous frame (before narrowing): wider than half the screen =
// screen centre; narrower than 12% = its centre; else left third = left edge, right third = right edge, otherwise
// centre. A group not seen last frame uses its root entry's x.
#include "godhand/hud_layout.h"
#include "godhand/log.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

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
            Off,
        };

        constexpr uint32_t kScreenWidth = 0x3C1180u;
        constexpr uint32_t kScreenYOffset = 0x586B80u;
        constexpr uint32_t kPlayerBar = 0xF0000001u, kRadar = 0xF0000002u, kRoulette = 0xF0000003u;

        Layout g_layout = Layout::Anchored;
        bool g_wide = false;
        float g_k = 1.0f;
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

        float anchorFor(uint32_t key, float fallback, float width)
        {
            const Extent *extent = nullptr;
            auto last = g_last.find(key);
            if (last != g_last.end())
                extent = &last->second;
            else
            {
                auto seen = g_seen.find(key);
                if (seen != g_seen.end())
                    extent = &seen->second;
            }
            if (!extent)
                return fallback;
            const float span = extent->x1 - extent->x0, centre = 0.5f * (extent->x0 + extent->x1);
            if (span >= 0.5f * width)
                return 0.5f * width;
            if (span < 0.12f * width)
                return centre;
            if (centre < width / 3.0f)
                return extent->x0;
            if (centre > 2.0f * width / 3.0f)
                return extent->x1;
            return centre;
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

        // n vertices, x at address + i*16 (y at +4).
        void narrowVertices(uint8_t *rdram, uint32_t address, int n, uint32_t caller)
        {
            if (g_layout == Layout::Off)
                return;
            const float width = screenWidth(rdram);
            const int32_t offset = gsOffset(rdram);
            float xs[4], x0 = 1e9f, x1 = -1e9f;
            for (int i = 0; i < n; ++i)
            {
                xs[i] = static_cast<float>(static_cast<int32_t>(read32(rdram, address + 16u * i)) - offset) / 16.0f;
                x0 = std::min(x0, xs[i]);
                x1 = std::max(x1, xs[i]);
            }
            if (n >= 2 && x1 - x0 >= 0.9f * width)
                return; // full-screen quad: backgrounds, fades
            float anchor;
            if (g_layout == Layout::Centered)
                anchor = 0.5f * width;
            else if (g_ctx.active)
            {
                extend(g_ctx.key, x0, x1);
                anchor = g_ctx.anchor;
            }
            else
            {
                const int32_t yOffset = static_cast<int32_t>(read32(rdram, kScreenYOffset)) * 16;
                float y0 = 1e9f, y1 = -1e9f;
                for (int i = 0; i < n; ++i)
                {
                    const float y = static_cast<float>(static_cast<int32_t>(read32(rdram, address + 16u * i + 4u)) - yOffset) / 16.0f;
                    y0 = std::min(y0, y);
                    y1 = std::max(y1, y);
                }
                noteLoose(caller, x0, x1, y0, y1);
                return;
            }
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
            narrowVertices(rdram, s0 + 0x10u, N, read32(rdram, sp + RaSlot));
        }

        // func_0015C410 before "sw $a3,0x40($sp)": one strip vertex's GS x in $a3.
        void stripVertex(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            if (g_layout == Layout::Off)
                return;
            const float width = screenWidth(rdram);
            const int32_t offset = gsOffset(rdram);
            const float xs = static_cast<float>(GPR_S32(ctx, 7) - offset) / 16.0f;
            float anchor;
            if (g_layout == Layout::Centered)
                anchor = 0.5f * width;
            else if (g_ctx.active)
            {
                extend(g_ctx.key, xs, xs);
                anchor = g_ctx.anchor;
            }
            else
            {
                const uint32_t sp = GPR_U32(ctx, 29);
                noteLoose(read32(rdram, sp + 0x70u), xs, xs, 0.0f, 0.0f);
                return;
            }
            const float x = anchor + (xs - anchor) * g_k;
            SET_GPR_S32(ctx, 7, static_cast<int32_t>(std::lround(x * 16.0f)) + offset);
        }

        void idBegin(uint8_t *, R5900Context *, PS2Runtime *) { g_inIdTrans = true; }

        // cIDBase_trans, before an entry is drawn: $a1 = the entry.
        void idEntry(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            if (g_region == Region::Forced)
                return; // g_ctx is the region's
            uint32_t root = GPR_U32(ctx, 5);
            for (int depth = 0; depth < 32; ++depth)
            {
                const int8_t parentIndex = static_cast<int8_t>(rdram[(root + 0x29u) & (PS2_RAM_SIZE - 1u)]);
                const uint32_t parent = read32(rdram, root + 0x20u);
                if (parentIndex < 0 || parent == 0u)
                    break;
                root = parent;
            }
            float rootX;
            const uint32_t bits = read32(rdram, root + 0x50u);
            std::memcpy(&rootX, &bits, sizeof(rootX));
            const float width = screenWidth(rdram);
            if (!std::isfinite(rootX))
                rootX = 0.5f * width;
            g_ctx = Context{true, root & 0x01FFFFFFu, anchorFor(root & 0x01FFFFFFu, rootX, width)};
            g_lastEntry = g_ctx;
        }

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
                return "anchored";
            case Layout::Centered:
                return "centered";
            default:
                return "off";
            }
        }

        Layout parseLayout(const char *text)
        {
            if (!text)
                return Layout::Anchored;
            std::string value(text);
            for (char &c : value)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (value == "centered" || value == "centred" || value == "center" || value == "centre")
                return Layout::Centered;
            if (value == "off" || value == "stretched" || value == "pnach" || value == "0")
                return Layout::Off;
            return Layout::Anchored;
        }

        bool cycleKeyPressed()
        {
#if defined(_WIN32)
            static bool wasDown = false;
            const bool down = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
            const bool pressed = down && !wasDown;
            wasDown = down;
            if (!pressed)
                return false;
            DWORD process = 0;
            GetWindowThreadProcessId(GetForegroundWindow(), &process);
            return process == GetCurrentProcessId();
#else
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
            out << "HUD (anchored): 2D drawn outside any widget, left stretched, by caller (x,y range):";
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

    bool narrowFont()
    {
        switch (g_layout)
        {
        case Layout::Off:
            return true;
        case Layout::Centered:
            return false;
        default:
            return !g_inIdTrans; // cIDBase text is narrowed with its widget
        }
    }

    bool narrowGauge() { return g_layout == Layout::Off; }

    void onFrame(uint8_t *)
    {
        if (!g_wide)
            return;
        g_last.swap(g_seen);
        g_seen.clear();
        g_ctx = Context{};
        g_region = Region::None;
        g_inIdTrans = false;
        if (cycleKeyPressed())
        {
            g_layout = g_layout == Layout::Anchored ? Layout::Centered : g_layout == Layout::Centered ? Layout::Off : Layout::Anchored;
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
        g_layout = parseLayout(std::getenv("GODHAND_HUD"));
        ps2xRegisterHostHook("gh_hud_line", &gpVertices<2, 0x70u>);
        ps2xRegisterHostHook("gh_hud_poly3", &gpVertices<3, 0x80u>);
        ps2xRegisterHostHook("gh_hud_poly4", &gpVertices<4, 0x90u>);
        ps2xRegisterHostHook("gh_hud_polyft4", &gpVertices<4, 0xA0u>);
        ps2xRegisterHostHook("gh_hud_polyg4", &gpVertices<4, 0x90u>);
        ps2xRegisterHostHook("gh_hud_sprite_d168", &gpVertices<2, 0x40u>);
        ps2xRegisterHostHook("gh_hud_sprite_d6e8", &gpVertices<2, 0x40u>);
        ps2xRegisterHostHook("gh_hud_sprite_db30", &gpVertices<2, 0x30u>);
        ps2xRegisterHostHook("gh_hud_strip", &stripVertex);
        ps2xRegisterHostHook("gh_hud_id_begin", &idBegin);
        ps2xRegisterHostHook("gh_hud_id_entry", &idEntry);
        ps2xRegisterHostHook("gh_hud_id_end", &idEnd);
        ps2xRegisterHostHook("gh_hud_plbar_begin", &regionBegin<kPlayerBar, Region::Forced>);
        ps2xRegisterHostHook("gh_hud_radar_begin", &regionBegin<kRadar, Region::Forced>);
        ps2xRegisterHostHook("gh_hud_roulette_begin", &regionBegin<kRoulette, Region::Inherit>);
        ps2xRegisterHostHook("gh_hud_region_end", &regionEnd);
        if (wide)
            GH_INFO(Game, "HUD layout: " << layoutName(g_layout) << " (F7 cycles anchored / centered / off)");
    }
}
