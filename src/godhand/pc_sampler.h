#pragma once

class PS2Runtime;

namespace godhand::pcsampler
{
    // GODHAND_PC_SAMPLE=<seconds>: a host thread samples the running guest thread's PC about 2000 times a second and
    // every <seconds> logs where the game's own code spends its time ([pc-sample], by recompiled function; module code
    // by 256-byte region). For finding what slows the game down (the F3 Speed below 100%).
    void installFromEnvironment(PS2Runtime &runtime);
}
