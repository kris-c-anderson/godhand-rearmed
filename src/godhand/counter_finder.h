#pragma once

#include <cstdint>

class PS2Runtime;

namespace godhand::counterfinder
{
    // GODHAND_FIND_COUNTERS=<guest function address>: the first call of that function after 5 seconds without one
    // starts a capture: three copies of guest RAM 10 game frames apart, then every 32-bit word that changed by the same
    // amount between them (an int or a float counting steadily) is logged ([counters]). For finding the variable
    // behind something animated on screen (the roulette timer dial: GODHAND_FIND_COUNTERS=0x114608).
    void install(PS2Runtime &runtime);
    // From the main-loop hook, once per game frame.
    void onFrame(uint8_t *rdram);
}
