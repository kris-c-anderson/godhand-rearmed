#include "godhand/hang_dump.h"
#include "godhand/guest_symbols.h"
#include "godhand/call_profile.h"
#include "godhand/hle_bindings.h"

#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_pnach.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#endif

namespace godhand::hangdump
{
    namespace
    {
        std::atomic<PS2Runtime *> g_runtime{nullptr};

        const char *statusName(EeThreadStatus status)
        {
            switch (status)
            {
            case EeThreadStatus::Running: return "RUN";
            case EeThreadStatus::Ready: return "READY";
            case EeThreadStatus::Waiting: return "WAIT";
            case EeThreadStatus::WaitingSuspended: return "WAIT+SUSP";
            case EeThreadStatus::Suspended: return "SUSP";
            case EeThreadStatus::Dormant: return "DORMANT";
            }
            return "?";
        }

        const char *waitName(EeWaitReason reason)
        {
            switch (reason)
            {
            case EeWaitReason::None: return "";
            case EeWaitReason::Sleep: return "sleep";
            case EeWaitReason::Semaphore: return "sema";
            case EeWaitReason::EventFlag: return "evflag";
            case EeWaitReason::VSync: return "vsync";
            case EeWaitReason::External: return "external";
            case EeWaitReason::Mpeg: return "mpeg";
            }
            return "?";
        }

        std::string guestLabel(uint32_t pc)
        {
            char text[96];
            if (const SdkFunction *function = sdkFunctionAt(pc))
                std::snprintf(text, sizeof(text), "0x%08x (%.*s)", pc, static_cast<int>(function->name.size()), function->name.data());
            else if (symbols::functionAt(pc))
                std::snprintf(text, sizeof(text), "0x%08x (%s)", pc, symbols::describe(pc).c_str());
            else
                std::snprintf(text, sizeof(text), "0x%08x", pc);
            return text;
        }

        void writeFile(const char *variable, const void *data, size_t size, const char *what)
        {
            const char *path = std::getenv(variable);
            if (!path || !*path)
                return;
            if (std::FILE *file = std::fopen(path, "wb"))
            {
                std::fwrite(data, 1, size, file);
                std::fclose(file);
                std::fprintf(stderr, "[hang-dump]   %s written to %s\n", what, path);
            }
            else
            {
                std::fprintf(stderr, "[hang-dump]   cannot write %s to %s\n", what, path);
            }
        }

        void dumpGuestKernel()
        {
            PS2Runtime *runtime = g_runtime.load();
            if (!runtime)
                return;
            const EeKernelSnapshot snapshot = runtime->eeScheduler().snapshot();
            std::fprintf(stderr, "[hang-dump] ===== guest EE kernel (seq %llu, cycle %llu, slice end %llu, next event %llu, running thread %d) =====\n",
                         static_cast<unsigned long long>(snapshot.sequence), static_cast<unsigned long long>(snapshot.eeCycle),
                         static_cast<unsigned long long>(snapshot.sliceEndCycle), static_cast<unsigned long long>(snapshot.nextEventCycle),
                         snapshot.runningThreadId);
            for (const EeThreadSnapshot &t : snapshot.threads)
            {
                std::fprintf(stderr, "[hang-dump]   thread %2d %-9s %-8s id=%-3d prio=%-3d pc=%s ra=%s entry=0x%08x sp=0x%08x\n", t.id,
                             statusName(t.status), waitName(t.waitReason), t.waitId, t.currentPriority, guestLabel(t.pc).c_str(),
                             guestLabel(t.ra).c_str(), t.entry, t.sp);
            }
            // DMA channel CHCR/MADR/QWC: a channel stuck with STR (0x100) set stalls code that waits for DMA idle.
            static constexpr struct { const char *name; uint32_t base; } kDmaChannels[] = {
                {"VIF0", 0x10008000u}, {"VIF1", 0x10009000u}, {"GIF", 0x1000A000u}, {"IPUfrom", 0x1000B000u},
                {"IPUto", 0x1000B400u}, {"SIF0", 0x1000C000u}, {"SIF1", 0x1000C400u}, {"SIF2", 0x1000C800u},
                {"SPRfrom", 0x1000D000u}, {"SPRto", 0x1000D400u},
            };
            for (const auto &channel : kDmaChannels)
            {
                const uint32_t chcr = runtime->memory().readIORegister(channel.base);
                const uint32_t madr = runtime->memory().readIORegister(channel.base + 0x10u);
                const uint32_t qwc = runtime->memory().readIORegister(channel.base + 0x20u);
                std::fprintf(stderr, "[hang-dump]   dma %-7s @0x%08x chcr=0x%08x%s madr=0x%08x qwc=0x%x\n", channel.name, channel.base, chcr,
                             (chcr & 0x100u) ? " STR" : "    ", madr, qwc);
            }
            for (uint32_t timer = 0; timer < 4; ++timer)
            {
                const uint32_t base = 0x10000000u + timer * 0x800u;
                std::fprintf(stderr, "[hang-dump]   timer T%u count=0x%04x mode=0x%04x comp=0x%04x hold=0x%04x\n", timer,
                             runtime->memory().readIORegister(base), runtime->memory().readIORegister(base + 0x10u),
                             runtime->memory().readIORegister(base + 0x20u), runtime->memory().readIORegister(base + 0x30u));
            }
            std::fprintf(stderr, "[hang-dump]   INTC stat=0x%08x mask=0x%08x\n", runtime->memory().readIORegister(0x1000F000u),
                         runtime->memory().readIORegister(0x1000F010u));
            std::fprintf(stderr, "[hang-dump]   GS csr=0x%llx imr=0x%llx\n",
                         static_cast<unsigned long long>(runtime->memory().gs().csr.load()),
                         static_cast<unsigned long long>(runtime->memory().gs().imr));
            {
                const GSRegisters &gs = runtime->memory().gs();
                std::fprintf(stderr, "[hang-dump]   GS pmode=0x%llx smode2=0x%llx dispfb1=0x%llx display1=0x%llx dispfb2=0x%llx display2=0x%llx bgcolor=0x%llx\n",
                             (unsigned long long)gs.pmode, (unsigned long long)gs.smode2, (unsigned long long)gs.dispfb1,
                             (unsigned long long)gs.display1, (unsigned long long)gs.dispfb2, (unsigned long long)gs.display2,
                             (unsigned long long)gs.bgcolor);
            }
            std::fprintf(stderr, "[hang-dump]   counters dmaStarts=%llu gifCopies=%llu gsWrites=%llu vifWrites=%llu\n",
                         static_cast<unsigned long long>(runtime->memory().dmaStartCount()),
                         static_cast<unsigned long long>(runtime->memory().gifCopyCount()),
                         static_cast<unsigned long long>(runtime->memory().gsWriteCount()),
                         static_cast<unsigned long long>(runtime->memory().vifWriteCount()));
            writeFile("GODHAND_DUMP_RAM", runtime->memory().getRDRAM(), 32u * 1024u * 1024u, "RAM");
            writeFile("GODHAND_DUMP_VRAM", runtime->memory().getGSVRAM(), 4u * 1024u * 1024u, "VRAM");
            // GODHAND_DUMP_MEM=addr:len[,addr:len...] (hex addr, decimal or hex len): hex dump of guest memory.
            if (const char *spec = std::getenv("GODHAND_DUMP_MEM"); spec && *spec)
            {
                std::string_view list(spec);
                const uint8_t *rdram = runtime->memory().getRDRAM();
                while (!list.empty())
                {
                    const size_t comma = list.find(',');
                    const std::string item(list.substr(0, comma));
                    list = comma == std::string_view::npos ? std::string_view() : list.substr(comma + 1);
                    const size_t colon = item.find(':');
                    const uint32_t address = static_cast<uint32_t>(std::stoul(item.substr(0, colon), nullptr, 16));
                    const uint32_t length = colon == std::string::npos ? 16u : static_cast<uint32_t>(std::stoul(item.substr(colon + 1), nullptr, 0));
                    for (uint32_t offset = 0; offset < length; offset += 16)
                    {
                        std::fprintf(stderr, "[hang-dump]   mem %08x:", address + offset);
                        for (uint32_t i = 0; i < 16 && offset + i < length; ++i)
                            std::fprintf(stderr, " %02x", rdram[(address + offset + i) & 0x01FFFFFFu]);
                        std::fprintf(stderr, "\n");
                    }
                }
            }
            // Live PC sampling: generated code stores ctx->pc before every instruction. Racy by design (diagnostic only).
            std::map<uint32_t, int> samples;
            for (int i = 0; i < 2000; ++i)
            {
                if (const R5900Context *ctx = runtime->eeScheduler().currentContext())
                    ++samples[ctx->pc];
                std::this_thread::sleep_for(std::chrono::microseconds(500));
            }
            std::vector<std::pair<int, uint32_t>> hot;
            for (const auto &[pc, count] : samples)
                hot.emplace_back(count, pc);
            std::sort(hot.begin(), hot.end(), std::greater<>());
            for (size_t i = 0; i < hot.size() && i < 12; ++i)
                std::fprintf(stderr, "[hang-dump]   pc-sample %4d x %s\n", hot[i].first, guestLabel(hot[i].second).c_str());
            for (const EeSemaphoreSnapshot &s : snapshot.semaphores)
                std::fprintf(stderr, "[hang-dump]   sema %2d count=%d/%d waiters=%u\n", s.id, s.count, s.maxCount, s.waiters);
            for (const EeEventFlagSnapshot &e : snapshot.eventFlags)
                std::fprintf(stderr, "[hang-dump]   evflag %2d bits=0x%08x waiters=%u\n", e.id, e.bits, e.waiters);
            std::fflush(stderr);
        }

        int callProfileTop()
        {
            static const int top = [] { const char *v = std::getenv("GODHAND_CALL_PROFILE_TOP"); return v ? std::atoi(v) : 30; }();
            return top;
        }
    }

    void setRuntime(PS2Runtime *runtime)
    {
        g_runtime.store(runtime);
    }

#if defined(_WIN32)
    namespace
    {
        std::string symbolize(HANDLE process, DWORD64 pc)
        {
            alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 256]{};
            auto *symbol = reinterpret_cast<SYMBOL_INFO *>(storage);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = 255;
            DWORD64 displacement = 0;
            char text[512];
            if (SymFromAddr(process, pc, &displacement, symbol))
            {
                IMAGEHLP_LINE64 line{};
                line.SizeOfStruct = sizeof(line);
                DWORD lineDisplacement = 0;
                if (SymGetLineFromAddr64(process, pc, &lineDisplacement, &line))
                    std::snprintf(text, sizeof(text), "%s+0x%llx (%s:%lu)", symbol->Name, static_cast<unsigned long long>(displacement),
                                  line.FileName, line.LineNumber);
                else
                    std::snprintf(text, sizeof(text), "%s+0x%llx", symbol->Name, static_cast<unsigned long long>(displacement));
            }
            else
            {
                std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(pc));
            }
            return text;
        }

        std::string threadName(HANDLE thread)
        {
            PWSTR description = nullptr;
            std::string name;
            if (SUCCEEDED(GetThreadDescription(thread, &description)) && description)
            {
                for (const wchar_t *c = description; *c; ++c)
                    name.push_back(*c < 128 ? static_cast<char>(*c) : '?');
                LocalFree(description);
            }
            return name;
        }

        void dumpAllThreads()
        {
            HANDLE process = GetCurrentProcess();
            static bool symbolsReady = false;
            if (!symbolsReady)
            {
                SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
                symbolsReady = SymInitialize(process, nullptr, TRUE) == TRUE;
            }

            const DWORD self = GetCurrentThreadId();
            const DWORD pid = GetCurrentProcessId();
            HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snapshot == INVALID_HANDLE_VALUE)
                return;
            THREADENTRY32 entry{};
            entry.dwSize = sizeof(entry);
            std::fprintf(stderr, "[hang-dump] ===== native thread stacks =====\n");
            for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry))
            {
                if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == self)
                    continue;
                HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE,
                                           entry.th32ThreadID);
                if (!thread)
                    continue;
                std::vector<DWORD64> pcs;
                if (SuspendThread(thread) != static_cast<DWORD>(-1))
                {
                    CONTEXT context{};
                    context.ContextFlags = CONTEXT_FULL;
                    if (GetThreadContext(thread, &context))
                    {
                        STACKFRAME64 frame{};
                        frame.AddrPC.Offset = context.Rip;
                        frame.AddrPC.Mode = AddrModeFlat;
                        frame.AddrFrame.Offset = context.Rbp;
                        frame.AddrFrame.Mode = AddrModeFlat;
                        frame.AddrStack.Offset = context.Rsp;
                        frame.AddrStack.Mode = AddrModeFlat;
                        for (int i = 0; i < 40; ++i)
                        {
                            if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &context, nullptr,
                                             SymFunctionTableAccess64, SymGetModuleBase64, nullptr) ||
                                frame.AddrPC.Offset == 0)
                                break;
                            pcs.push_back(frame.AddrPC.Offset);
                        }
                    }
                    ResumeThread(thread);
                }
                std::fprintf(stderr, "[hang-dump] thread %lu %s\n", entry.th32ThreadID, threadName(thread).c_str());
                for (DWORD64 pc : pcs)
                    std::fprintf(stderr, "[hang-dump]     %s\n", symbolize(process, pc).c_str());
                CloseHandle(thread);
            }
            CloseHandle(snapshot);
            std::fflush(stderr);
        }
    }

    namespace
    {
        // Stall catcher (2026-10-18, user: "a few small freezes in the cutscene and the boss fight" with the EE only
        // 36-64% busy, so something blocks it). The EE thread stamps every vblank it processes; a watchdog thread
        // prints every thread's native stack once when no vblank came for GODHAND_STALL_DUMP milliseconds, and how long
        // the stall lasted when the next vblank arrives. At most 12 dumps, 5 s apart.
        std::atomic<int64_t> g_lastVblankNs{0};

        int64_t steadyNs()
        {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        void runStallCatcher(int64_t thresholdMs)
        {
            const int64_t threshold = thresholdMs * 1000000;
            int dumps = 0;
            int64_t lastDump = 0;
            int64_t dumpedStallFrom = 0; // the vblank stamp the open stall started from, 0 = none
            for (;;)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                const int64_t last = g_lastVblankNs.load(std::memory_order_relaxed);
                if (last == 0)
                    continue;
                const int64_t now = steadyNs();
                if (dumpedStallFrom != 0 && last != dumpedStallFrom)
                {
                    std::fprintf(stderr, "[stall-dump] #%d ended: no EE vblank for %lld ms in total\n", dumps,
                                 static_cast<long long>((last - dumpedStallFrom) / 1000000));
                    std::fflush(stderr);
                    dumpedStallFrom = 0;
                }
                if (now - last < threshold || dumpedStallFrom != 0 || dumps >= 12 || (lastDump != 0 && now - lastDump < 5000000000))
                    continue;
                ++dumps;
                lastDump = now;
                dumpedStallFrom = last;
                std::fprintf(stderr, "[stall-dump] #%d: no EE vblank for %lld ms; stacks of every thread follow\n", dumps,
                             static_cast<long long>((now - last) / 1000000));
                dumpAllThreads();
            }
        }
    }

    void dumpNow(bool nativeStacks)
    {
        if (nativeStacks)
            dumpAllThreads();
        dumpGuestKernel();
        callprofile::report(callProfileTop());
    }

    namespace
    {
        // GODHAND_SAMPLE_PROFILE=<from s>:<to s>[:<thread name>] (round 10 diagnostic, off by default): between those
        // seconds after start, about every millisecond the named thread (default GameThread) is suspended, its
        // instruction pointer read and the thread resumed; every 8th sample also walks its stack. At the end the
        // samples are printed per function: self (leaf) and inclusive (on the walked stack) shares. The suspension
        // itself costs the thread a little time; counts are relative.
        void runSampler(double from, double to, std::string name)
        {
            const auto start = std::chrono::steady_clock::now();
            std::this_thread::sleep_until(start + std::chrono::duration<double>(from));
            HANDLE process = GetCurrentProcess();
            HANDLE thread = nullptr;
            HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snapshot != INVALID_HANDLE_VALUE)
            {
                THREADENTRY32 entry{};
                entry.dwSize = sizeof(entry);
                for (BOOL ok = Thread32First(snapshot, &entry); ok && !thread; ok = Thread32Next(snapshot, &entry))
                {
                    if (entry.th32OwnerProcessID != GetCurrentProcessId())
                        continue;
                    HANDLE candidate = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION,
                                                  FALSE, entry.th32ThreadID);
                    if (candidate && threadName(candidate) == name)
                        thread = candidate;
                    else if (candidate)
                        CloseHandle(candidate);
                }
                CloseHandle(snapshot);
            }
            if (!thread)
            {
                std::fprintf(stderr, "[sample-profile] thread %s not found\n", name.c_str());
                return;
            }
            SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
            SymInitialize(process, nullptr, TRUE);
            std::map<DWORD64, uint32_t> leaf;
            std::vector<std::vector<DWORD64>> stacks;
            uint64_t samples = 0u;
            const auto end = start + std::chrono::duration<double>(to);
            while (std::chrono::steady_clock::now() < end)
            {
                if (SuspendThread(thread) != static_cast<DWORD>(-1))
                {
                    CONTEXT context{};
                    context.ContextFlags = CONTEXT_FULL;
                    if (GetThreadContext(thread, &context))
                    {
                        ++leaf[context.Rip];
                        ++samples;
                        if ((samples & 7u) == 0u)
                        {
                            std::vector<DWORD64> pcs;
                            STACKFRAME64 frame{};
                            frame.AddrPC.Offset = context.Rip;
                            frame.AddrPC.Mode = AddrModeFlat;
                            frame.AddrFrame.Offset = context.Rbp;
                            frame.AddrFrame.Mode = AddrModeFlat;
                            frame.AddrStack.Offset = context.Rsp;
                            frame.AddrStack.Mode = AddrModeFlat;
                            for (int i = 0; i < 32; ++i)
                            {
                                if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &context, nullptr,
                                                 SymFunctionTableAccess64, SymGetModuleBase64, nullptr) ||
                                    frame.AddrPC.Offset == 0)
                                    break;
                                pcs.push_back(frame.AddrPC.Offset);
                            }
                            stacks.push_back(std::move(pcs));
                        }
                    }
                    ResumeThread(thread);
                }
                std::this_thread::sleep_for(std::chrono::microseconds(1000));
            }
            CloseHandle(thread);
            // Symbol name per pc (no displacement), cached.
            std::map<DWORD64, std::string> names;
            auto nameOf = [&](DWORD64 pc) -> const std::string & {
                auto it = names.find(pc);
                if (it != names.end())
                    return it->second;
                alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 256]{};
                auto *symbol = reinterpret_cast<SYMBOL_INFO *>(storage);
                symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
                symbol->MaxNameLen = 255;
                DWORD64 displacement = 0;
                std::string text;
                if (SymFromAddr(process, pc, &displacement, symbol))
                    text = symbol->Name;
                else
                {
                    char raw[32];
                    std::snprintf(raw, sizeof(raw), "0x%llx", static_cast<unsigned long long>(pc));
                    text = raw;
                }
                return names.emplace(pc, std::move(text)).first->second;
            };
            std::map<std::string, uint64_t> self, inclusive;
            for (const auto &[pc, count] : leaf)
                self[nameOf(pc)] += count;
            for (const std::vector<DWORD64> &pcs : stacks)
            {
                std::vector<std::string_view> seen;
                for (DWORD64 pc : pcs)
                {
                    const std::string &n = nameOf(pc);
                    if (std::find(seen.begin(), seen.end(), std::string_view(n)) == seen.end())
                    {
                        seen.push_back(n);
                        ++inclusive[n];
                    }
                }
            }
            auto print = [](const char *what, const std::map<std::string, uint64_t> &table, uint64_t total, size_t top) {
                std::vector<std::pair<uint64_t, std::string>> rows;
                for (const auto &[n, c] : table)
                    rows.emplace_back(c, n);
                std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
                for (size_t i = 0; i < rows.size() && i < top; ++i)
                    std::fprintf(stderr, "[sample-profile] %s %6.2f%% %8llu  %s\n", what, 100.0 * static_cast<double>(rows[i].first) / static_cast<double>(total ? total : 1),
                                 static_cast<unsigned long long>(rows[i].first), rows[i].second.c_str());
            };
            std::fprintf(stderr, "[sample-profile] %s %.0f-%.0f s: %llu samples, %zu stacks\n", name.c_str(), from, to,
                         static_cast<unsigned long long>(samples), stacks.size());
            print("self", self, samples, 60);
            print("incl", inclusive, stacks.size(), 80);
            std::fflush(stderr);
        }
    }

    void startFromEnvironment()
    {
        if (const char *stall = std::getenv("GODHAND_STALL_DUMP"); stall && std::atoi(stall) > 0)
        {
            const int64_t thresholdMs = std::atoi(stall);
            ps2x::pnach::addFrameHook([] { g_lastVblankNs.store(steadyNs(), std::memory_order_relaxed); });
            std::thread(runStallCatcher, thresholdMs).detach();
            std::fprintf(stderr, "[stall-dump] on: thread stacks when no EE vblank comes for %lld ms\n", static_cast<long long>(thresholdMs));
        }
        if (const char *spec = std::getenv("GODHAND_SAMPLE_PROFILE"); spec && *spec)
        {
            double from = 0.0, to = 0.0;
            char name[64] = "GameThread";
            if (std::sscanf(spec, "%lf:%lf:%63s", &from, &to, name) >= 2 && to > from)
                std::thread(runSampler, from, to, std::string(name)).detach();
        }
        const char *delay = std::getenv("GODHAND_HANG_DUMP");
        if (!delay || !*delay)
            return;
        const int delaySeconds = std::atoi(delay);
        const char *repeat = std::getenv("GODHAND_HANG_DUMP_REPEAT");
        const int repeatSeconds = repeat ? std::atoi(repeat) : 0;
        const char *native = std::getenv("GODHAND_HANG_DUMP_NATIVE");
        const bool nativeStacks = !native || *native != '0';
        std::thread([delaySeconds, repeatSeconds, nativeStacks]() {
            std::this_thread::sleep_for(std::chrono::seconds(delaySeconds));
            do
            {
                dumpNow(nativeStacks);
                if (repeatSeconds > 0)
                    std::this_thread::sleep_for(std::chrono::seconds(repeatSeconds));
            } while (repeatSeconds > 0);
        }).detach();
    }
#else
    void dumpNow(bool nativeStacks)
    {
        (void)nativeStacks;
        dumpGuestKernel();
        callprofile::report(callProfileTop());
    }

    void startFromEnvironment() {}
#endif
}
