// 60 FPS mode and L3 dash: a port of the PCSX2 patch SLUS-21503_6FB69282.pnach ("60 Fps", by Puggsy + Claude Ai +
// Gemini; Kris's copy, 2026-10-22). PCSX2 writes the patch into the game's code; here the game code is recompiled C++,
// so the code changes are compiled in as switchable patches (recomp/godhand.toml [[patches.conditional]] and
// [[patches.hooks]], flag "fps60") and this file holds the parts that are C++:
//
//   pnach                                         here
//   00747A09 byte 01 (every vblank)               frameStart: the game's frame divider byte = 1 (60 frames/s)
//   302D04/10D694/10B7B8 lui at,0x3F00            conditional patches (special attack, God Hand gauge drain, gene
//                                                  menu: 0.5 instead of 1.0)
//   2C81BC hook -> F0000 (hitbox sync)            hitboxCompare replaces c.eq.s f0,f2 at 0x2C81C0: the hitbox logic
//                                                  is skipped when the integer frame (cvt.w.s) of the last and the
//                                                  current position are equal, not only the exact floats
//   15765C nop (dodge sync)                       conditional patch: the add.s in the beql delay slot is not run
//   1147CC sw zero,0(at) (roulette time)          conditional patch: nop (the timer store is dropped; the pnach's
//                                                  store to whatever $at holds is left out)
//   29059C lui at,0x3F00 (enemy physics x0.5)     conditional patch
//   290584 hook -> F0038 (enemy animation x2)     conditional patch: mov.s f12,f1 -> add.s f12,f1,f1 (f1 * 2)
//   30ADF4 hook -> D0020 (left stick)             frameStart: a stick axis at full deflection last frame is held
//                                                  there for one more frame (state in g_stick* instead of 0xD0200;
//                                                  without the pnach's input loop, see frameStart)
//   D05E8242 == 2: 005E8147 = 0, 005E81C7 = 0     frameStart with L3Dash: L3 held pushes the left stick fully
//   (L3 dash, every vblank)                        forward
//
// The frame hook runs before the instruction at 0x30ADF4 (lw v0,0x3D84(s3)), right after the main loop's wait for the
// next frame (0x30ADD8..0x30ADF0), where the pnach's stick hook ran; the per-vblank writes run there too, once per
// game frame.
#include "godhand/frame_patches.h"
#include "godhand/hud_layout.h"
#include "godhand/call_profile.h"
#include "godhand/counter_finder.h"
#include "godhand/pc_sampler.h"
#include "godhand/log.h"
#include "ps2_runtime_macros.h" // FPU_CVT_W_S (the recompiled CVT.W.S)
#include "runtime/ps2_display_options.h"

#include <cstdlib>
#include <cstring>

namespace godhand
{
    namespace
    {
        constexpr uint32_t kFrameDivider = 0x747A09u;                                     // 1 = 60 frames/s
        constexpr uint32_t kStickBytes[4] = {0x5E8146u, 0x5E8147u, 0x5E81C6u, 0x5E81C7u}; // left stick X/Y, two pads
        constexpr uint32_t kPadButtons = 0x5E8242u;                                        // 16-bit; 2 = L3
        bool g_fps60 = false;
        bool g_l3Dash = false;
        uint8_t g_stickHeld[4] = {}; // the axis was at 0 or 0xFF last frame
        uint8_t g_stickLast[4] = {};
        int g_stickInjected[4] = {-1, -1, -1, -1}; // the value written over the game's last frame (-1: none)
        uint8_t g_stickRaw[4] = {0x80u, 0x80u, 0x80u, 0x80u};
        uint32_t g_staleRestores = 0, g_holds = 0, g_frames = 0;

        bool envOn(const char *name)
        {
            const char *value = std::getenv(name);
            return value && *value && *value != '0';
        }

        uint8_t &ram8(uint8_t *rdram, uint32_t address)
        {
            return rdram[address & (PS2_RAM_SIZE - 1u)];
        }

        // GODHAND_CALL_PROFILE=1 with GODHAND_CALL_PROFILE_EVERY=<game frames>: a call count report of every function
        // called in each window of that many frames (finding the code behind something seen on screen: compare the
        // windows where it shows with the others).
        void profileWindow()
        {
            static const uint32_t every = []
            {
                const char *value = std::getenv("GODHAND_CALL_PROFILE_EVERY");
                return value ? static_cast<uint32_t>(std::strtoul(value, nullptr, 10)) : 0u;
            }();
            static uint64_t frames = 0, window = 0;
            if (every == 0u || ++frames % every != 0u)
                return;
            GH_INFO(Game, "call profile window " << ++window << " (game frames " << frames - every << ".." << frames << ")");
            callprofile::report(100000);
        }

        void frameStart(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            pcsampler::noteMainContext(ctx);
            ps2_host_display::noteGameFrame();
            profileWindow();
            counterfinder::onFrame(rdram);
            hud::onFrame(rdram);
            if (g_l3Dash)
            {
                uint16_t buttons = 0;
                std::memcpy(&buttons, &ram8(rdram, kPadButtons), sizeof(buttons));
                if (buttons == 2u)
                {
                    ram8(rdram, kStickBytes[1]) = 0u;
                    ram8(rdram, kStickBytes[3]) = 0u;
                }
            }
            if (!g_fps60)
                return;
            ram8(rdram, kFrameDivider) = 1u;
            // The pnach writes the held value over the game's and reads it back the next frame as if it came from the
            // pad. The game does not rewrite these bytes every frame here, so a written 0xFF came back as a new full
            // deflection and was held again, forever (stick stuck at full right/left; the pnach's "input loop" with
            // PCSX2 overclock). A byte that still holds what was written last frame is the game's earlier reading:
            // it is put back and used as the reading. A held value is written only when it differs from the reading.
            for (int axis = 0; axis < 4; ++axis)
            {
                uint8_t &value = ram8(rdram, kStickBytes[axis]);
                if (g_stickInjected[axis] >= 0 && value == static_cast<uint8_t>(g_stickInjected[axis]))
                {
                    value = g_stickRaw[axis];
                    ++g_staleRestores;
                }
                g_stickInjected[axis] = -1;
                const uint8_t raw = value;
                g_stickRaw[axis] = raw;
                const bool wasHeld = g_stickHeld[axis] != 0u;
                const uint8_t last = g_stickLast[axis];
                if (raw == 0u || raw == 0xFFu)
                {
                    g_stickHeld[axis] = 1u;
                    g_stickLast[axis] = raw;
                }
                else
                    g_stickHeld[axis] = 0u;
                if (wasHeld && raw != last)
                {
                    value = last;
                    g_stickInjected[axis] = last;
                    ++g_holds;
                }
            }
            if (++g_frames % 3600u == 0u) // once a minute at 60 frames/s
                GH_INFO(Game, "fps60 stick hold: " << g_holds << " held frames, " << g_staleRestores
                                                   << " readings restored in the last 3600 frames; left stick now "
                                                   << int(ram8(rdram, kStickBytes[0])) << "," << int(ram8(rdram, kStickBytes[1])));
            if (g_frames % 3600u == 0u)
                g_holds = g_staleRestores = 0u;
        }

        // Replaces c.eq.s f0,f2 at 0x2C81C0: the condition is "same integer frame" (cvt.w.s of both).
        void hitboxCompare(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            const int32_t last = FPU_CVT_W_S(ctx->f[0]);
            const int32_t current = FPU_CVT_W_S(ctx->f[2]);
            ctx->fcr31 = last == current ? (ctx->fcr31 | 0x800000u) : (ctx->fcr31 & ~0x800000u);
        }

        // Replaces sw v0,0x15B0(s1) at 0x1147CC (the player's roulette timer, decremented once per game frame): every
        // other frame the store is skipped, so at 60 frames/s it lasts as long as on the PS2.
        void rouletteTimer(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            static bool skip = false;
            skip = !skip;
            if (skip)
                return;
            const uint32_t value = GPR_U32(ctx, 2), address = GPR_U32(ctx, 17) + 0x15B0u;
            std::memcpy(&ram8(rdram, address), &value, sizeof(value));
        }

        // Replaces addiu v0,v0,-1 at 0x14530C (the roulette TIME dial, cCockRoulette +0x4D0): every other frame.
        void rouletteDial(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            static bool skip = false;
            skip = !skip;
            if (!skip)
                SET_GPR_S32(ctx, 2, GPR_S32(ctx, 2) - 1);
        }
    }

    void installFramePatches(PS2Runtime &runtime)
    {
        counterfinder::install(runtime);
        g_fps60 = envOn("GODHAND_FPS60");
        g_l3Dash = envOn("GODHAND_L3_DASH");
        ps2xRegisterHostHook("gh_frame_start", &frameStart);
        ps2xRegisterHostHook("gh_fps60_hitbox_compare", &hitboxCompare);
        ps2xRegisterHostHook("gh_fps60_roulette_timer", &rouletteTimer);
        ps2xRegisterHostHook("gh_fps60_roulette_dial", &rouletteDial);
        ps2xSetPatchFlag("frame_hook", true); // always: it also counts game frames for the F3 overlay
        ps2xSetPatchFlag("fps60", g_fps60);
        GH_INFO(Game, "frame rate: " << (g_fps60 ? "60 FPS mode (Fps60=1)" : "30 FPS, as on the PS2") << "; L3 dash: "
                                     << (g_l3Dash ? "on" : "off"));
    }
}
