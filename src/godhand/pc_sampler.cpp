#include "godhand/pc_sampler.h"
#include "godhand/log.h"
#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <sstream>
#include <thread>
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
#include <timeapi.h> // timeBeginPeriod (left out by WIN32_LEAN_AND_MEAN)
#pragma comment(lib, "winmm.lib")
#endif

namespace godhand::pcsampler
{
    namespace
    {
        // The recompiled function holding pc: its first table slot (resume entries inside a function share the owner's
        // pointer), or pc's 256-byte region outside the table (module code).
        uint32_t owner(uint32_t pc)
        {
            const uint32_t base = g_ps2RecompiledFunctionTableBase;
            if (pc < base || pc >= g_ps2RecompiledFunctionTableEnd || (pc & 3u) != 0u)
                return pc & ~0xFFu;
            uint32_t slot = (pc - base) >> 2;
            const PS2Runtime::RecompiledFunction fn = g_ps2RecompiledFunctionTable[slot];
            if (!fn)
                return pc;
            while (slot > 0u && g_ps2RecompiledFunctionTable[slot - 1u] == fn)
                --slot;
            return base + slot * 4u;
        }
    }

    void installFromEnvironment(PS2Runtime &runtime)
    {
        const char *value = std::getenv("GODHAND_PC_SAMPLE");
        const int seconds = value ? std::atoi(value) : 0;
        if (seconds <= 0)
            return;
        PS2Runtime *rt = &runtime;
        std::thread([rt, seconds]() {
#if defined(_WIN32)
            timeBeginPeriod(1);
#endif
            std::unordered_map<uint32_t, uint32_t> pcs;
            auto reportAt = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
            uint32_t samples = 0, none = 0;
            for (;;)
            {
                std::this_thread::sleep_for(std::chrono::microseconds(500));
                const R5900Context *ctx = rt->eeScheduler().currentContext();
                ++samples;
                if (ctx)
                    ++pcs[ctx->pc];
                else
                    ++none;
                if (std::chrono::steady_clock::now() < reportAt)
                    continue;
                reportAt += std::chrono::seconds(seconds);
                std::unordered_map<uint32_t, uint32_t> functions;
                uint32_t topPc = 0, topPcCount = 0;
                for (const auto &[pc, count] : pcs)
                {
                    functions[owner(pc)] += count;
                    if (count > topPcCount)
                    {
                        topPc = pc;
                        topPcCount = count;
                    }
                }
                std::vector<std::pair<uint32_t, uint32_t>> top(functions.begin(), functions.end());
                std::sort(top.begin(), top.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
                std::ostringstream line;
                line << "[pc-sample] " << samples << " samples (" << none << " with no guest thread); hottest pc 0x" << std::hex
                     << topPc << std::dec << " " << (100.0 * topPcCount / std::max(1u, samples)) << "%:";
                for (size_t i = 0; i < top.size() && i < 25u; ++i)
                    line << " 0x" << std::hex << top[i].first << std::dec << "=" << (1000 * top[i].second / std::max(1u, samples)) / 10.0 << "%";
                GH_INFO(Game, line.str());
                pcs.clear();
                samples = none = 0;
            }
        }).detach();
        GH_INFO(Game, "[pc-sample] sampling the guest PC, a report every " << seconds << " s");
    }
}
