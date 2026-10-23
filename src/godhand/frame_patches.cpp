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
#include "godhand/character_mods.h"
#include "godhand/hud_layout.h"
#include "godhand/call_profile.h"
#include "godhand/counter_finder.h"
#include "godhand/pc_sampler.h"
#include "godhand/log.h"
#include "ps2_runtime_macros.h" // FPU_CVT_W_S (the recompiled CVT.W.S)
#include "runtime/ps2_display_options.h"

#include <cstdlib>
#include <cstring>
#include <string>

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
        uint64_t g_gameFrame = 0; // every frame hook; at 60 frames/s the odd ones are the extra frames

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

        // L3 alone (pad buttons == 2): a dash, as a quick double tap of the left stick forward (from centre) and then
        // forward held while L3 is. The pnach this comes from wrote "stick up" every frame from PCSX2's patch engine, between the
        // game's own pad reads, which the game saw as tapping; here the game reads the byte once and kept seeing a
        // plain "up" (Gene walked). Each step lasts 2 frames at 30 frames/s (4 in the 60 FPS mode). Written after the
        // stick hold, and marked as injected so the next frame puts the reading back.
        uint32_t g_dashFrames = 0; // frames since L3 went down (0 = not held)

        // A stick value written over the game's: the reading is kept and the next frame puts it back.
        void injectStick(uint8_t *rdram, int axis, uint8_t value)
        {
            if (g_stickInjected[axis] < 0)
                g_stickRaw[axis] = ram8(rdram, kStickBytes[axis]);
            ram8(rdram, kStickBytes[axis]) = value;
            g_stickInjected[axis] = value;
        }

        // At 30 frames/s (the 60 FPS stick hold does this itself): stick values written last frame that the game did
        // not replace go back to the reading.
        void restoreInjected(uint8_t *rdram)
        {
            for (int axis = 0; axis < 4; ++axis)
            {
                if (g_stickInjected[axis] >= 0 && ram8(rdram, kStickBytes[axis]) == static_cast<uint8_t>(g_stickInjected[axis]))
                    ram8(rdram, kStickBytes[axis]) = g_stickRaw[axis];
                g_stickInjected[axis] = -1;
            }
        }

        // The in-game roulette (the move wheel in a fight): its list moves only with the left stick; while it is up, the
        // d-pad's up and down push the stick too. Seen through cCockRoulette move's dial step (0x145308, only while the
        // wheel is up), stamped with the game frame.
        uint64_t g_rouletteSeen = 0;
        bool g_rouletteBack = false; // the last wheel step went up (see rouletteSlideBegin)

        void rouletteSeen(uint8_t *, R5900Context *, PS2Runtime *) { g_rouletteSeen = g_gameFrame; }

        // Replaces addiu v1,v1,1 at 0x1458CC (cCockRoulette, the wheel's move: any of its command bits advances the
        // cursor +0x422 by one, wrapping at the count in $a0). Pushing up (stick or d-pad) goes back one instead, so
        // an overshot move is one step away, not a whole lap.
        void rouletteStep(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const int32_t cursor = GPR_S32(ctx, 3), count = GPR_S32(ctx, 4);
            uint16_t buttons = 0;
            std::memcpy(&buttons, &ram8(rdram, kPadButtons), sizeof(buttons));
            const bool up = (buttons & 0x10u) != 0u || ram8(rdram, kStickBytes[1]) < 0x40u;
            g_rouletteBack = up && count > 1;
            SET_GPR_S32(ctx, 3, up && count > 0 ? (cursor + count - 1) % count : cursor + 1);
        }

        // The wheel's slide (the rest of that function: progress +0x338 from 1.0 to 0 and a 32.0 offset +0x2A4, both
        // x0.7 a frame) only knows forward steps. A back step to c is drawn as the forward step c -> c+1 played
        // backwards: cursor c+1 and progress 1-p while the slide runs, then put back after the slots are placed.
        bool g_rouletteSwapped = false;
        uint16_t g_rouletteSavedCursor = 0;
        float g_rouletteSavedP = 0.0f, g_rouletteSavedA = 0.0f;

        float ramF32(uint8_t *rdram, uint32_t address)
        {
            float value = 0.0f;
            std::memcpy(&value, &ram8(rdram, address), sizeof(value));
            return value;
        }

        void setRamF32(uint8_t *rdram, uint32_t address, float value) { std::memcpy(&ram8(rdram, address), &value, sizeof(value)); }

        // Before lwc1 f0,0x2A4(s1) at 0x1458E8 (after the step), s1 = the wheel.
        void rouletteSlideBegin(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            g_rouletteSwapped = false;
            const uint32_t wheel = GPR_U32(ctx, 17);
            const float p = ramF32(rdram, wheel + 0x338u), a = ramF32(rdram, wheel + 0x2A4u);
            if (!g_rouletteBack || !(a > 1.0f))
                return;
            int16_t count = 0;
            uint16_t cursor = 0;
            std::memcpy(&count, &ram8(rdram, wheel + 0x3B2u), sizeof(count));
            std::memcpy(&cursor, &ram8(rdram, wheel + 0x422u), sizeof(cursor));
            if (count <= 1)
                return;
            g_rouletteSavedCursor = cursor;
            g_rouletteSavedP = p;
            g_rouletteSavedA = a;
            g_rouletteSwapped = true;
            const uint16_t next = static_cast<uint16_t>((cursor + 1) % count);
            const float back = 1.0f - p;
            std::memcpy(&ram8(rdram, wheel + 0x422u), &next, sizeof(next));
            setRamF32(rdram, wheel + 0x338u, back);
            setRamF32(rdram, wheel + 0x2A4u, 32.0f * back);
            // The incoming slot (+0xE4) is hidden (+0x2C bit 8) whenever the wheel is at rest; same for the reversed
            // progress, so the first frame shows the old wheel exactly.
            uint32_t incoming = 0, flags = 0;
            std::memcpy(&incoming, &ram8(rdram, wheel + 0xE4u), sizeof(incoming));
            if (incoming == 0u)
                return;
            std::memcpy(&flags, &ram8(rdram, incoming + 0x2Cu), sizeof(flags));
            flags = 32.0f * back > 1.0f ? (flags & ~8u) : (flags | 8u);
            std::memcpy(&ram8(rdram, incoming + 0x2Cu), &flags, sizeof(flags));
        }

        // Before lw v0,0x90(s1) at 0x145C88 (after the slot loop).
        void rouletteSlideEnd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            if (!g_rouletteSwapped)
                return;
            g_rouletteSwapped = false;
            const uint32_t wheel = GPR_U32(ctx, 17);
            std::memcpy(&ram8(rdram, wheel + 0x422u), &g_rouletteSavedCursor, sizeof(uint16_t));
            setRamF32(rdram, wheel + 0x338u, g_rouletteSavedP);
            setRamF32(rdram, wheel + 0x2A4u, g_rouletteSavedA);
        }

        void rouletteDpad(uint8_t *rdram)
        {
            if (g_rouletteSeen == 0u || g_gameFrame - g_rouletteSeen > 2u)
                return;
            uint16_t buttons = 0;
            std::memcpy(&buttons, &ram8(rdram, kPadButtons), sizeof(buttons));
            const bool up = (buttons & 0x10u) != 0u, down = (buttons & 0x40u) != 0u;
            if (up == down)
                return;
            injectStick(rdram, 1, up ? 0x00u : 0xFFu);
            injectStick(rdram, 3, up ? 0x00u : 0xFFu);
        }

        void l3Dash(uint8_t *rdram)
        {
            if (!g_l3Dash)
                return;
            uint16_t buttons = 0;
            std::memcpy(&buttons, &ram8(rdram, kPadButtons), sizeof(buttons));
            if (buttons != 2u)
            {
                g_dashFrames = 0;
                return;
            }
            const uint32_t step = g_fps60 ? 4u : 2u;
            const uint32_t phase = g_dashFrames++ / step; // 0 neutral, 1 forward, 2 neutral, 3.. forward
            // While the tap is being made the stick is centred sideways too (a diagonal is not a forward tap, which is
            // why a dash started while walking did nothing); once dashing, sideways is the player's again.
            const bool tapping = phase < 3u;
            const uint8_t forward = (phase == 1u || phase >= 3u) ? 0x00u : 0x80u;
            for (int axis = 0; axis < 4; ++axis)
            {
                const bool vertical = (axis & 1) != 0;
                if (!vertical && !tapping)
                    continue;
                injectStick(rdram, axis, vertical ? forward : 0x80u);
            }
        }

        void frameStart(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            pcsampler::noteMainContext(ctx);
            ++g_gameFrame;
            ps2_host_display::noteGameFrame();
            profileWindow();
            counterfinder::onFrame(rdram);
            hud::onFrame(rdram);
            characters::onFrame(rdram);
            if (!g_fps60)
            {
                restoreInjected(rdram);
                l3Dash(rdram);
                rouletteDpad(rdram);
                return;
            }
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
            l3Dash(rdram);
            rouletteDpad(rdram);
        }

        // Replaces c.eq.s f0,f2 at 0x2C81C0: the condition is "same integer frame" (cvt.w.s of both).
        void hitboxCompare(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            const int32_t last = FPU_CVT_W_S(ctx->f[0]);
            const int32_t current = FPU_CVT_W_S(ctx->f[2]);
            ctx->fcr31 = last == current ? (ctx->fcr31 | 0x800000u) : (ctx->fcr31 & ~0x800000u);
        }

        // The roulette's time: one count every g_roulettePeriod game frames. 1 at 30 frames/s, 2 at 60 (the PS2's
        // time); [Game] RouletteTimer=double doubles it (2 at 30, 4 at 60), for the timer and the time to choose.
        uint32_t g_roulettePeriod = 1;

        // Replaces sw v0,0x15B0(s1) at 0x1147CC (the player's roulette timer, decremented once per game frame): the
        // store happens once every g_roulettePeriod calls.
        void rouletteTimer(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            static uint32_t calls = 0;
            if (++calls % g_roulettePeriod != 0u)
                return;
            const uint32_t value = GPR_U32(ctx, 2), address = GPR_U32(ctx, 17) + 0x15B0u;
            std::memcpy(&ram8(rdram, address), &value, sizeof(value));
        }

        // Replaces lbu v0,0x1C(s1) at 0x2AAA88 in cIDBase_move (0 = the layout's animations do not step): every
        // other frame the animations of all HUD and menu layouts (button prompts, the stick-wiggle prompt, pulses,
        // slides) are not stepped, so they play at the PS2's speed; positions are still worked out every frame.
        void uiAnimStep(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint8_t value = (g_gameFrame & 1u) ? 0u : ram8(rdram, GPR_U32(ctx, 17) + 0x1Cu);
            SET_GPR_U32(ctx, 2, value);
        }

        // Replaces addiu v0,v1,-1 at 0x1F6F8C in TimedButton_tick (an action button prompt's time, +0x32): counts
        // down every other frame, so timed prompts (grab escapes and the like) last as long as on the PS2.
        void buttonTimer(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            const int32_t value = GPR_S32(ctx, 3);
            SET_GPR_S32(ctx, 2, (g_gameFrame & 1u) ? value : value - 1);
        }

        // The stick-wiggle button prompt (cCockActButton move, type 4, 0x13BF08..0x13C020) swings its stick icon a
        // fixed 30 degrees and 5 units per frame (a 4-frame cycle). At 60 frames/s the swing of every other frame is
        // undone: the icon's angle (+0x48) and x (+0x38) of the entry at +0xD8 and the swing direction (0x3BD6E0).
        constexpr uint32_t kWiggleDirection = 0x3BD6E0u;
        uint32_t g_wiggleEntry = 0, g_wiggleAngle = 0, g_wiggleX = 0;
        uint8_t g_wiggleDirection = 0;

        uint32_t ram32(uint8_t *rdram, uint32_t address)
        {
            uint32_t value;
            std::memcpy(&value, &ram8(rdram, address), sizeof(value));
            return value;
        }

        void wiggleBegin(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            g_wiggleEntry = ram32(rdram, GPR_U32(ctx, 6) + 0xD8u);
            g_wiggleAngle = ram32(rdram, g_wiggleEntry + 0x48u);
            g_wiggleX = ram32(rdram, g_wiggleEntry + 0x38u);
            g_wiggleDirection = ram8(rdram, kWiggleDirection);
        }

        void wiggleEnd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            if (!(g_gameFrame & 1u) || g_wiggleEntry == 0u || ram32(rdram, GPR_U32(ctx, 6) + 0xD8u) != g_wiggleEntry)
                return;
            std::memcpy(&ram8(rdram, g_wiggleEntry + 0x48u), &g_wiggleAngle, sizeof(g_wiggleAngle));
            std::memcpy(&ram8(rdram, g_wiggleEntry + 0x38u), &g_wiggleX, sizeof(g_wiggleX));
            ram8(rdram, kWiggleDirection) = g_wiggleDirection;
        }

        // Replaces addiu v0,v0,-1 at 0x14530C (the roulette TIME dial, cCockRoulette +0x4D0): every other frame.
        void rouletteDial(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            static uint32_t calls = 0;
            if (++calls % g_roulettePeriod == 0u)
                SET_GPR_S32(ctx, 2, GPR_S32(ctx, 2) - 1);
        }
    }

    void installFramePatches(PS2Runtime &runtime)
    {
        counterfinder::install(runtime);
        g_fps60 = envOn("GODHAND_FPS60");
        g_l3Dash = envOn("GODHAND_L3_DASH");
        ps2xRegisterHostHook("gh_frame_start", &frameStart);
        characters::installProbe();
        ps2xRegisterHostHook("gh_fps60_hitbox_compare", &hitboxCompare);
        ps2xRegisterHostHook("gh_fps60_roulette_timer", &rouletteTimer);
        ps2xRegisterHostHook("gh_fps60_roulette_dial", &rouletteDial);
        ps2xRegisterHostHook("gh_roulette_seen", &rouletteSeen);
        ps2xRegisterHostHook("gh_roulette_step", &rouletteStep);
        ps2xRegisterHostHook("gh_roulette_slide_begin", &rouletteSlideBegin);
        ps2xRegisterHostHook("gh_roulette_slide_end", &rouletteSlideEnd);
        ps2xRegisterHostHook("gh_fps60_ui_anim", &uiAnimStep);
        ps2xRegisterHostHook("gh_fps60_button_timer", &buttonTimer);
        ps2xRegisterHostHook("gh_fps60_wiggle_begin", &wiggleBegin);
        ps2xRegisterHostHook("gh_fps60_wiggle_end", &wiggleEnd);
        ps2xSetPatchFlag("frame_hook", true); // always: it also counts game frames for the F3 overlay
        ps2xSetPatchFlag("fps60", g_fps60);
        {
            const char *timer = std::getenv("GODHAND_ROULETTE_TIMER");
            const std::string value = timer ? timer : "";
            const bool slow = value == "double" || value == "Double" || value == "2";
            g_roulettePeriod = (g_fps60 ? 2u : 1u) * (slow ? 2u : 1u);
            ps2xSetPatchFlag("roulette_timer", g_roulettePeriod > 1u);
            GH_INFO(Game, "roulette timer: " << (slow ? "double" : "normal") << " (one count every " << g_roulettePeriod << " frames)");
        }
        GH_INFO(Game, "frame rate: " << (g_fps60 ? "60 FPS mode (Fps60=1)" : "30 FPS, as on the PS2") << "; L3 dash: "
                                     << (g_l3Dash ? "on" : "off"));
    }
}
