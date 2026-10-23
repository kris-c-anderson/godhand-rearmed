#pragma once

#include "ps2_runtime.h"

namespace godhand
{
    // Optional gameplay/sound changes from godhand.ini [Game] (the launcher's Gameplay tab). Before runtime.run().
    //   UnlockHard (GODHAND_UNLOCK_HARD)  Hard can be picked on the difficulty select without a clear. The save's own
    //                                     unlock flag is only read, so a save is left as it was.
    //   MuteMusic  (GODHAND_MUTE_MUSIC)   the game's music streams (cSndBgmNode) play at volume 0; voices, sound
    //                                     effects and movies are unchanged.
    void installGameOptions(PS2Runtime &runtime);
}
