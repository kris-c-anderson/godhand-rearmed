#include "godhand/pc_sampler.h"
#include "godhand/guest_symbols.h"
#include "godhand/log.h"
#include "ps2_runtime.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <sstream>
#include <string>
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
#include <dbghelp.h>
#include <timeapi.h> // timeBeginPeriod (left out by WIN32_LEAN_AND_MEAN)
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "dbghelp.lib")
#endif

namespace godhand::pcsampler
{
    namespace
    {
        // The game's main thread context, published by the frame hook on the EE thread. The scheduler's own lookup
        // (currentContext: a hash map and a vector of interrupt contexts) changes under a reader on another thread; a
        // guest thread's context itself stays at one address. Its pc changes with every guest instruction, so it is
        // read once into a local: used directly as a map key it was hashed and then stored as two different values
        // (a node in the wrong bucket), which hung the sampler (earlier builds) or crashed the game.
        std::atomic<const R5900Context *> g_mainContext{nullptr};
#if defined(_WIN32)
        std::atomic<HANDLE> g_eeThread{nullptr}; // the host thread that runs the EE (the frame hook's thread)
#endif

        // The recompiled function holding pc: its first table slot (resume entries inside a function share the owner's
        // pointer), or pc's 256-byte region outside the table (module code).
        uint32_t owner(uint32_t pc)
        {
            const uint32_t base = g_ps2RecompiledFunctionTableBase;
            if (pc < base || pc >= g_ps2RecompiledFunctionTableEnd || (pc & 3u) != 0u)
                return pc & ~0xFFu;
            static std::unordered_map<uint32_t, uint32_t> cache;
            auto cached = cache.find(pc);
            if (cached != cache.end())
                return cached->second;
            uint32_t slot = (pc - base) >> 2;
            const PS2Runtime::RecompiledFunction fn = g_ps2RecompiledFunctionTable[slot];
            uint32_t result = pc;
            if (fn)
            {
                // A long run of one pointer is a shared fallback (module code, stubs), not one function: capped.
                uint32_t steps = 0;
                while (slot > 0u && steps < 4096u && g_ps2RecompiledFunctionTable[slot - 1u] == fn)
                {
                    --slot;
                    ++steps;
                }
                result = steps >= 4096u ? (pc & ~0xFFu) : base + slot * 4u;
            }
            cache.emplace(pc, result);
            return result;
        }

        template <typename Key>
        std::vector<std::pair<Key, uint32_t>> sorted(const std::unordered_map<Key, uint32_t> &counts)
        {
            std::vector<std::pair<Key, uint32_t>> rows(counts.begin(), counts.end());
            std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
            return rows;
        }

#if defined(_WIN32)
        // Host function of an address in the EE thread ("module!function"; godhand.pdb next to godhand.exe for names).
        std::string hostFunction(DWORD64 address, DWORD64 &start)
        {
            HANDLE process = GetCurrentProcess();
            alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 256] = {};
            auto *symbol = reinterpret_cast<SYMBOL_INFO *>(storage);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = 255;
            DWORD64 displacement = 0;
            HMODULE module = nullptr;
            char path[MAX_PATH] = "?";
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCSTR>(address), &module))
                GetModuleFileNameA(module, path, MAX_PATH);
            const char *name = std::strrchr(path, '\\') ? std::strrchr(path, '\\') + 1 : path;
            if (SymFromAddr(process, address, &displacement, symbol))
            {
                start = address - displacement;
                return std::string(name) + "!" + symbol->Name;
            }
            start = address & ~0xFFFull;
            char text[64];
            std::snprintf(text, sizeof(text), "+0x%llx", static_cast<unsigned long long>(start - reinterpret_cast<DWORD64>(module)));
            return std::string(name) + text;
        }
#endif
    }

    void noteMainContext(const R5900Context *ctx)
    {
        g_mainContext.store(ctx, std::memory_order_relaxed);
#if defined(_WIN32)
        if (!g_eeThread.load(std::memory_order_relaxed))
        {
            HANDLE handle = nullptr;
            if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &handle,
                                THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0))
                g_eeThread.store(handle);
        }
#endif
    }

    void installFromEnvironment(PS2Runtime &runtime)
    {
        const char *value = std::getenv("GODHAND_PC_SAMPLE");
        const int seconds = value ? std::atoi(value) : 0;
        if (seconds <= 0)
            return;
        (void)runtime;
        std::thread([seconds]() {
#if defined(_WIN32)
            timeBeginPeriod(1);
            SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
            SymInitialize(GetCurrentProcess(), nullptr, TRUE);
            std::unordered_map<DWORD64, uint32_t> hostRips;
#endif
            std::unordered_map<uint32_t, uint32_t> pcs;
            auto reportAt = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
            uint32_t samples = 0, none = 0;
            for (;;)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                const R5900Context *ctx = g_mainContext.load(std::memory_order_relaxed);
                ++samples;
                if (ctx)
                {
                    const uint32_t pc = *static_cast<const volatile uint32_t *>(&ctx->pc);
                    ++pcs[pc];
                }
                else
                    ++none;
#if defined(_WIN32)
                // The EE thread's host instruction: suspended only for the register read (nothing allocated meanwhile:
                // it may hold the heap lock).
                if (HANDLE thread = g_eeThread.load(std::memory_order_relaxed))
                {
                    CONTEXT context{};
                    context.ContextFlags = CONTEXT_CONTROL;
                    DWORD64 rip = 0;
                    if (SuspendThread(thread) != static_cast<DWORD>(-1))
                    {
                        if (GetThreadContext(thread, &context))
                            rip = context.Rip;
                        ResumeThread(thread);
                    }
                    if (rip)
                        ++hostRips[rip];
                }
#endif
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
                const auto top = sorted(functions);
                std::ostringstream line;
                line << "[pc-sample] " << samples << " samples of the main game thread (" << none << " before its first frame); hottest pc 0x" << std::hex
                     << topPc << std::dec << " (" << symbols::describe(topPc) << ") " << (100.0 * topPcCount / std::max(1u, samples)) << "%:";
                for (size_t i = 0; i < top.size() && i < 25u; ++i)
                    line << " " << symbols::describe(top[i].first) << "=" << (1000 * top[i].second / std::max(1u, samples)) / 10.0 << "%";
                GH_INFO(Game, line.str());
#if defined(_WIN32)
                if (!hostRips.empty())
                {
                    static std::unordered_map<DWORD64, std::pair<DWORD64, std::string>> names; // rip -> function
                    std::unordered_map<std::string, uint32_t> hostFunctions;
                    uint32_t hostSamples = 0;
                    for (const auto &[rip, count] : hostRips)
                    {
                        auto it = names.find(rip);
                        if (it == names.end())
                        {
                            DWORD64 start = 0;
                            std::string name = hostFunction(rip, start);
                            it = names.emplace(rip, std::make_pair(start, std::move(name))).first;
                        }
                        hostFunctions[it->second.second] += count;
                        hostSamples += count;
                    }
                    const auto hostTop = sorted(hostFunctions);
                    std::ostringstream host;
                    host << "[pc-sample] EE host thread, " << hostSamples << " samples:";
                    for (size_t i = 0; i < hostTop.size() && i < 30u; ++i)
                        host << "\n    " << (1000 * hostTop[i].second / std::max(1u, hostSamples)) / 10.0 << "%  " << hostTop[i].first;
                    GH_INFO(Game, host.str());
                    hostRips.clear();
                }
#endif
                pcs.clear();
                samples = none = 0;
            }
        }).detach();
        GH_INFO(Game, "[pc-sample] sampling the game thread (guest PC and host code), a report every " << seconds << " s");
    }
}
