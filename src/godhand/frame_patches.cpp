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
//                                                  there for one more frame (state in kStick* instead of 0xD0200)
//   D05E8242 == 2: 005E8147 = 0, 005E81C7 = 0     frameStart with L3Dash: L3 held pushes the left stick fully
//   (L3 dash, every vblank)                        forward
//
// The frame hook runs before the instruction at 0x30ADF4 (lw v0,0x3D84(s3)), right after the main loop's wait for the
// next frame (0x30ADD8..0x30ADF0), where the pnach's stick hook ran; the per-vblank writes run there too, once per
// game frame.
#include "godhand/frame_patches.h"
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

        bool envOn(const char *name)
        {
            const char *value = std::getenv(name);
            return value && *value && *value != '0';
        }

        uint8_t &ram8(uint8_t *rdram, uint32_t address)
        {
            return rdram[address & (PS2_RAM_SIZE - 1u)];
        }

        void frameStart(uint8_t *rdram, R5900Context *, PS2Runtime *)
        {
            ps2_host_display::noteGameFrame();
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
            for (int axis = 0; axis < 4; ++axis)
            {
                uint8_t &value = ram8(rdram, kStickBytes[axis]);
                const bool wasHeld = g_stickHeld[axis] != 0u;
                const uint8_t last = g_stickLast[axis];
                if (value == 0u || value == 0xFFu)
                {
                    g_stickHeld[axis] = 1u;
                    g_stickLast[axis] = value;
                }
                else
                    g_stickHeld[axis] = 0u;
                if (wasHeld)
                    value = last;
            }
        }

        // Replaces c.eq.s f0,f2 at 0x2C81C0: the condition is "same integer frame" (cvt.w.s of both).
        void hitboxCompare(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            const int32_t last = FPU_CVT_W_S(ctx->f[0]);
            const int32_t current = FPU_CVT_W_S(ctx->f[2]);
            ctx->fcr31 = last == current ? (ctx->fcr31 | 0x800000u) : (ctx->fcr31 & ~0x800000u);
        }
    }

    void installFramePatches(PS2Runtime &)
    {
        g_fps60 = envOn("GODHAND_FPS60");
        g_l3Dash = envOn("GODHAND_L3_DASH");
        ps2xRegisterHostHook("gh_frame_start", &frameStart);
        ps2xRegisterHostHook("gh_fps60_hitbox_compare", &hitboxCompare);
        ps2xSetPatchFlag("frame_hook", true); // always: it also counts game frames for the F3 overlay
        ps2xSetPatchFlag("fps60", g_fps60);
        GH_INFO(Game, "frame rate: " << (g_fps60 ? "60 FPS mode (Fps60=1)" : "30 FPS, as on the PS2") << "; L3 dash: "
                                     << (g_l3Dash ? "on" : "off"));
    }
}
