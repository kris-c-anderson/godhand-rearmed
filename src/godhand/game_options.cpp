// Optional changes to the game ([Game] UnlockHard / MuteMusic). See game_options.h.
//
// UnlockHard: the difficulty select is in the title room DLL r020 (cR020::titleLevelSel, 0x79B0E0). It reads the save's
// "Hard unlocked" bit (0x747A28 & 0x200000, set by clearing the game) three times: for the menu's item animation
// (0x79B25C), the confirm (0x79B374, a locked Hard buzzes) and the item sprites (0x79B924, Hard or the locked plate).
// recomp/modules/r020.toml turns each of those 'and' into 'or rX,zero,0x200000' under the flag "unlock_hard".
//
// MuteMusic: the music is CRI ADX streams driven by the SDE sound library. Every stream volume change goes through
// func_00373FF0(node a0, category a1, volume f12) for a one-channel node, or func_003745C0(node a0, category a1,
// channel a2, volume f12) for a node with 2+ channels (node+0x58 > 1; the stereo music takes this one, which the
// first version of this option missed): volume x the cSnd category levels (0x5FEE00) -> the dB table at 0x3F2060 ->
// ADXT_SetOutVol (0x32A9F0) / func_0032EC78 per channel. Music streams are cSndBgmNode objects (vtable 0x44CEE8 at
// +0x80, set by its constructor cSndBgmNode_Ctor 0x2CC500); for those the volume is made 0 at both functions' entry
// (table[0] = -96.0 dB, silent). Voices and sound effects are other node types; movies use Sofdec's own ADXT handles.
// GODHAND_TRACE=GAME logs the first calls (node, vtable, category, volume) to check the match.
#include "godhand/game_options.h"
#include "godhand/log.h"
#include "ps2_runtime_macros.h"

#include <cstdlib>
#include <cstring>

namespace godhand
{
    namespace
    {
        constexpr uint32_t kBgmNodeVtable = 0x44CEE8u;

        bool envOn(const char *name)
        {
            const char *value = std::getenv(name);
            return value && *value && *value != '0';
        }

        // Before the first instruction of func_00373FF0 (the stream volume setter).
        void muteMusicVolume(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t node = GPR_U32(ctx, 4);
            if (node == 0u || node + 0x84u > PS2_RAM_SIZE)
                return;
            uint32_t vtable = 0;
            std::memcpy(&vtable, rdram + ((node + 0x80u) & (PS2_RAM_SIZE - 1u)), sizeof(vtable));
            static int logged = 0;
            if (logged < 40)
            {
                ++logged;
                GH_TRACE(Game, "[mute] volume call: node 0x" << std::hex << node << " vtable 0x" << vtable << std::dec
                                   << " category " << GPR_U32(ctx, 5) << " volume " << ctx->f[12]
                                   << (vtable == kBgmNodeVtable ? " (music: muted)" : ""));
            }
            if (vtable == kBgmNodeVtable)
                ctx->f[12] = 0.0f;
        }
    }

    void installGameOptions(PS2Runtime &)
    {
        const bool unlockHard = envOn("GODHAND_UNLOCK_HARD");
        const bool muteMusic = envOn("GODHAND_MUTE_MUSIC");
        ps2xRegisterHostHook("gh_mute_music_volume", &muteMusicVolume);
        ps2xSetPatchFlag("unlock_hard", unlockHard);
        ps2xSetPatchFlag("mute_music", muteMusic);
        GH_INFO(Game, "options: Hard " << (unlockHard ? "unlocked from the start" : "as saved") << "; music "
                                       << (muteMusic ? "muted" : "on"));
    }
}
