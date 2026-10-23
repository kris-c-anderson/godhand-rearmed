#include "godhand/call_profile.h"
#include "godhand/guest_symbols.h"
#include "godhand/hle_bindings.h"
#include "godhand/log.h"

#include "ps2_runtime.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

// The generated dispatch table (register_functions.cpp of the recompiler output).
extern PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[];
extern const uint32_t g_ps2RecompiledFunctionTableBase;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount;

namespace godhand::callprofile
{
    namespace
    {
        std::unique_ptr<PS2Runtime::RecompiledFunction[]> g_originals;
        std::unique_ptr<std::atomic<uint32_t>[]> g_counts;
        std::vector<uint32_t> g_lastCounts;
        std::mutex g_reportMutex;

        // The dispatcher stores the called address in ctx->pc before invoking the slot.
        void profiledEntry(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t slot = (ctx->pc - g_ps2RecompiledFunctionTableBase) >> 2;
            g_counts[slot].fetch_add(1, std::memory_order_relaxed);
            g_originals[slot](rdram, ctx, runtime);
        }
    }

    void installFromEnvironment(PS2Runtime &runtime)
    {
        (void)runtime;
        const char *value = std::getenv("GODHAND_CALL_PROFILE");
        if (!value || *value != '1')
            return;
        const uint32_t slots = g_ps2RecompiledFunctionTableSlotCount;
        g_originals = std::make_unique<PS2Runtime::RecompiledFunction[]>(slots);
        g_counts = std::make_unique<std::atomic<uint32_t>[]>(slots);
        g_lastCounts.assign(slots, 0u);
        uint32_t wrapped = 0;
        PS2Runtime::RecompiledFunction previous = nullptr;
        for (uint32_t slot = 0; slot < slots; ++slot)
        {
            const PS2Runtime::RecompiledFunction fn = g_ps2RecompiledFunctionTable[slot];
            g_originals[slot] = fn;
            // A slot starts a function when its pointer differs from the previous slot's (resume entries inside a
            // function share the owner's pointer and are left alone).
            if (fn && fn != previous)
            {
                g_ps2RecompiledFunctionTable[slot] = &profiledEntry;
                ++wrapped;
            }
            previous = fn;
        }
        GH_INFO(Hook, "call profile: wrapped " << wrapped << " function entries");
    }

    void report(int top)
    {
        if (!g_counts)
            return;
        std::lock_guard<std::mutex> lock(g_reportMutex);
        const uint32_t slots = g_ps2RecompiledFunctionTableSlotCount;
        std::vector<std::pair<uint32_t, uint32_t>> hot;
        uint64_t total = 0;
        for (uint32_t slot = 0; slot < slots; ++slot)
        {
            const uint32_t now = g_counts[slot].load(std::memory_order_relaxed);
            const uint32_t delta = now - g_lastCounts[slot];
            g_lastCounts[slot] = now;
            if (delta)
            {
                hot.emplace_back(delta, slot);
                total += delta;
            }
        }
        std::sort(hot.begin(), hot.end(), std::greater<>());
        std::fprintf(stderr, "[call-profile] %llu calls to %zu functions since last report; top %d:\n",
                     static_cast<unsigned long long>(total), hot.size(), top);
        for (int i = 0; i < top && i < static_cast<int>(hot.size()); ++i)
        {
            const uint32_t address = g_ps2RecompiledFunctionTableBase + hot[i].second * 4u;
            const SdkFunction *sdk = sdkFunctionAt(address);
            const char *name = symbols::functionAt(address);
            std::fprintf(stderr, "[call-profile]   %10u  0x%08x %.*s%s\n", hot[i].first, address,
                         sdk ? static_cast<int>(sdk->name.size()) : 0, sdk ? sdk->name.data() : "",
                         (name && !sdk) ? name : "");
        }
        static const std::pair<uint32_t, uint32_t> range = []
        {
            const char *value = std::getenv("GODHAND_CALL_PROFILE_RANGE");
            unsigned lo = 0u, hi = 0u;
            if (!value || std::sscanf(value, "%x-%x", &lo, &hi) != 2)
                return std::pair<uint32_t, uint32_t>{0u, 0u};
            return std::pair<uint32_t, uint32_t>{lo, hi};
        }();
        if (range.second > range.first)
        {
            std::sort(hot.begin(), hot.end(), [](const auto &a, const auto &b) { return a.second < b.second; });
            for (const auto &[count, slot] : hot)
            {
                const uint32_t address = g_ps2RecompiledFunctionTableBase + slot * 4u;
                if (address >= range.first && address < range.second)
                    std::fprintf(stderr, "[call-profile] range %10u  %s\n", count, symbols::describe(address).c_str());
            }
        }
        std::fflush(stderr);
    }
}
