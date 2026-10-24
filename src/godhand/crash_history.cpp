// Crashes the in-process reporter cannot see, made visible in the log (crash_history.h).
//
// Oct 2026: three runs ended mid-play with nothing in the log (no "[crash]" lines, no "runtime exited"): the process
// was ended by a fail-fast or a corrupted heap, which skip the unhandled-exception filter. Windows' error reporting
// writes every such end to the Application event log (event 1000, "Application Error": module, exception code,
// offset), which a normal user can read; so the next run reports them.
#include "godhand/crash_history.h"
#include "godhand/log.h"
#include "godhand/hang_dump.h"
#include "runtime/ps2_display_options.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#include <winevt.h>
#include <tlhelp32.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "wevtapi.lib")
#endif

namespace godhand
{
#if defined(_WIN32)
    namespace
    {
        std::string narrow(const std::wstring &text)
        {
            if (text.empty())
                return {};
            const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
            std::string out(static_cast<size_t>(size > 0 ? size : 0), '\0');
            if (size > 0)
                WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
            return out;
        }

        const char *codeMeaning(unsigned long code)
        {
            switch (code)
            {
            case 0xC0000005ul: return "access violation";
            case 0xC0000374ul: return "heap corruption (something wrote past a block it owned, or freed one twice)";
            case 0xC0000409ul: return "fail-fast (abort() or std::terminate in a DLL, a security check, a bad parameter)";
            case 0xC00000FDul: return "stack overflow";
            case 0xC000001Dul: return "illegal instruction";
            case 0xC0000420ul: return "assertion failure";
            case 0xE06D7363ul: return "C++ exception not caught";
            case 0x80000003ul: return "breakpoint";
            case 0xC0000602ul: return "fail-fast (unknown)";
            default: return "";
            }
        }

        // The <Data> values of an event, in order (AppName, AppVersion, AppTimeStamp, ModuleName, ModuleVersion,
        // ModuleTimeStamp, ExceptionCode, FaultingOffset, ProcessId, ProcessCreationTime, AppPath, ModulePath, ... for
        // event 1000), and its time.
        void parseEvent(const std::wstring &xml, std::vector<std::wstring> &data, std::wstring &time)
        {
            const size_t t = xml.find(L"SystemTime='");
            const size_t t2 = t == std::wstring::npos ? xml.find(L"SystemTime=\"") : t;
            if (t2 != std::wstring::npos)
            {
                const size_t start = t2 + 12;
                const size_t end = xml.find_first_of(L"'\"", start);
                time = xml.substr(start, end == std::wstring::npos ? 0 : std::min<size_t>(end - start, 19)); // to the second
            }
            size_t pos = 0;
            while ((pos = xml.find(L"<Data", pos)) != std::wstring::npos)
            {
                const size_t close = xml.find(L'>', pos);
                if (close == std::wstring::npos)
                    break;
                if (xml[close - 1] == L'/') // <Data/>: empty
                {
                    data.emplace_back();
                    pos = close + 1;
                    continue;
                }
                const size_t end = xml.find(L"</Data>", close);
                if (end == std::wstring::npos)
                    break;
                data.push_back(xml.substr(close + 1, end - close - 1));
                pos = end + 7;
            }
        }

        DWORD imageTimeStamp(HMODULE module)
        {
            if (!module)
                return 0;
            const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(module);
            const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(reinterpret_cast<const uint8_t *>(module) + dos->e_lfanew);
            return nt->FileHeader.TimeDateStamp;
        }

        void readHistory()
        {
            // The last three days, newest first.
            const wchar_t *query = L"*[System[(Provider[@Name='Application Error'] or Provider[@Name='Application Hang']) and "
                                   L"TimeCreated[timediff(@SystemTime) <= 259200000]]]";
            EVT_HANDLE results = EvtQuery(nullptr, L"Application", query, EvtQueryChannelPath | EvtQueryReverseDirection);
            if (!results)
            {
                GH_INFO(Boot, "crash history: the Application event log could not be read (error " << GetLastError() << ")");
                return;
            }
            // No symbol lookup here: dbghelp is single-threaded and the PC sampler and crash reporter use it.
            int reported = 0, seen = 0;
            EVT_HANDLE events[16];
            DWORD returned = 0;
            while (reported < 6 && seen < 200 && EvtNext(results, 16, events, 1000, 0, &returned))
            {
                for (DWORD i = 0; i < returned; ++i)
                {
                    ++seen;
                    DWORD used = 0, properties = 0;
                    EvtRender(nullptr, events[i], EvtRenderEventXml, 0, nullptr, &used, &properties);
                    std::wstring xml(used / sizeof(wchar_t) + 1, L'\0');
                    const bool ok = used && EvtRender(nullptr, events[i], EvtRenderEventXml, static_cast<DWORD>(xml.size() * sizeof(wchar_t)),
                                                      xml.data(), &used, &properties);
                    EvtClose(events[i]);
                    if (!ok || reported >= 6)
                        continue;
                    std::vector<std::wstring> data;
                    std::wstring time;
                    parseEvent(xml, data, time);
                    if (data.empty() || _wcsicmp(data[0].c_str(), L"godhand.exe") != 0)
                        continue;
                    ++reported;
                    const bool hang = xml.find(L"Application Hang") != std::wstring::npos;
                    if (hang || data.size() < 8)
                    {
                        GH_INFO(Boot, "crash history: " << narrow(time) << " UTC godhand.exe "
                                                        << (hang ? "stopped responding and was closed" : "ended (no details)"));
                        continue;
                    }
                    const unsigned long code = std::wcstoul(data[6].c_str(), nullptr, 16);
                    const unsigned long long offset = std::wcstoull(data[7].c_str(), nullptr, 16);
                    const std::string where = _wcsicmp(data[3].c_str(), L"godhand.exe") == 0 &&
                                                      std::wcstoul(data[5].c_str(), nullptr, 16) == imageTimeStamp(GetModuleHandleW(nullptr))
                                                  ? "(this build)"
                                                  : "(another build)";
                    GH_INFO(Boot, "crash history: " << narrow(time) << " UTC godhand.exe crashed: exception 0x" << std::hex << code
                                                    << std::dec << " " << codeMeaning(code) << " in " << narrow(data[3]) << "+0x"
                                                    << std::hex << offset << std::dec << " " << where);
                }
            }
            EvtClose(results);
            if (reported == 0)
                GH_INFO(Boot, "crash history: no crash of godhand.exe in the last three days");
        }

        LONG CALLBACK fatalExceptionHandler(EXCEPTION_POINTERS *info)
        {
            if (!info || !info->ExceptionRecord)
                return EXCEPTION_CONTINUE_SEARCH;
            const DWORD code = info->ExceptionRecord->ExceptionCode;
            if (code == 0xC00000FDul)
            {
                // Little stack is left: a fixed line only.
                std::fputs("[crash] stack overflow\n", stderr);
                std::fflush(stderr);
            }
            else if (code == 0xC0000374ul || code == 0xC0000409ul || code == 0xC0000420ul)
            {
                static volatile LONG once = 0;
                if (InterlockedExchange(&once, 1) == 0)
                {
                    char module[MAX_PATH] = "?";
                    HMODULE handle = nullptr;
                    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                           static_cast<LPCSTR>(info->ExceptionRecord->ExceptionAddress), &handle))
                        GetModuleFileNameA(handle, module, MAX_PATH);
                    std::fprintf(stderr, "[crash] exception 0x%08lX (%s) on thread %lu at %s+0x%llx\n", static_cast<unsigned long>(code),
                                 codeMeaning(code), GetCurrentThreadId(), std::strrchr(module, '\\') ? std::strrchr(module, '\\') + 1 : module,
                                 static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(info->ExceptionRecord->ExceptionAddress) -
                                                                 reinterpret_cast<uintptr_t>(handle)));
                    std::fflush(stderr);
                }
            }
            return EXCEPTION_CONTINUE_SEARCH;
        }
    }

    void reportPreviousCrashes()
    {
        // Late enough for the GS DLL to be loaded (its offsets are named too), off the start-up path.
        std::thread([]
                    {
                        std::this_thread::sleep_for(std::chrono::seconds(8));
                        readHistory(); })
            .detach();
    }

    namespace
    {
        std::string moduleOf(const void *address)
        {
            HMODULE module = nullptr;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    reinterpret_cast<LPCWSTR>(address), &module) ||
                !module)
                return "no module";
            wchar_t path[MAX_PATH]{};
            GetModuleFileNameW(module, path, MAX_PATH);
            std::wstring name = path;
            const size_t slash = name.find_last_of(L"\\/");
            if (slash != std::wstring::npos)
                name = name.substr(slash + 1u);
            char text[320];
            std::snprintf(text, sizeof(text), "%s+0x%llx", narrow(name).c_str(),
                          static_cast<unsigned long long>(reinterpret_cast<const uint8_t *>(address) - reinterpret_cast<const uint8_t *>(module)));
            return text;
        }

        // Whether a user32 entry point starts with a jump somewhere else (an overlay or input layer hooking the message
        // pump: Steam's gameoverlayrenderer64.dll hooks PeekMessage/GetMessage in game processes), and where to.
        std::string hookOf(const char *name)
        {
            const HMODULE user32 = GetModuleHandleW(L"user32.dll");
            const auto *code = user32 ? reinterpret_cast<const uint8_t *>(GetProcAddress(user32, name)) : nullptr;
            if (!code)
                return std::string(name) + ": not found";
            const uint8_t *target = nullptr;
            if (code[0] == 0xE9) // jmp rel32
            {
                int32_t rel;
                std::memcpy(&rel, code + 1, 4);
                target = code + 5 + rel;
            }
            else if (code[0] == 0xFF && code[1] == 0x25) // jmp [rip+disp32]
            {
                int32_t disp;
                std::memcpy(&disp, code + 2, 4);
                std::memcpy(&target, code + 6 + disp, sizeof(target));
            }
            else if (code[0] == 0x48 && code[1] == 0xB8 && code[10] == 0xFF && code[11] == 0xE0) // mov rax,imm64; jmp rax
                std::memcpy(&target, code + 2, sizeof(target));
            if (!target)
                return std::string(name) + ": not hooked";
            HMODULE targetModule = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(target), &targetModule);
            if (targetModule == user32)
                return std::string(name) + ": not hooked (jumps inside user32)";
            return std::string(name) + ": HOOKED, jumps to " + moduleOf(target);
        }

        // Modules loaded from outside Windows and outside the game's folder: injected overlays and input layers.
        std::string foreignModules()
        {
            wchar_t windows[MAX_PATH]{}, exe[MAX_PATH]{};
            GetWindowsDirectoryW(windows, MAX_PATH);
            GetModuleFileNameW(nullptr, exe, MAX_PATH);
            std::wstring exeDir = exe;
            exeDir = exeDir.substr(0, exeDir.find_last_of(L"\\/") + 1u);
            std::wstring windowsDir = windows;
            auto startsWith = [](const std::wstring &text, const std::wstring &prefix) {
                return text.size() >= prefix.size() && _wcsnicmp(text.c_str(), prefix.c_str(), prefix.size()) == 0;
            };
            std::string out;
            HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, 0);
            if (snapshot == INVALID_HANDLE_VALUE)
                return "(module list unavailable)";
            MODULEENTRY32W entry{};
            entry.dwSize = sizeof(entry);
            int count = 0;
            for (BOOL ok = Module32FirstW(snapshot, &entry); ok; ok = Module32NextW(snapshot, &entry))
            {
                const std::wstring path = entry.szExePath;
                if (startsWith(path, windowsDir) || startsWith(path, exeDir))
                    continue;
                if (++count > 24)
                {
                    out += " ...";
                    break;
                }
                out += (out.empty() ? "" : ", ") + narrow(path);
            }
            CloseHandle(snapshot);
            return out.empty() ? "none" : out;
        }

        std::string windowName(HWND window)
        {
            wchar_t cls[128]{}, title[128]{};
            GetClassNameW(window, cls, 128);
            GetWindowTextW(window, title, 128);
            return "class \"" + narrow(cls) + "\" title \"" + narrow(title) + "\"";
        }

        // Does the window's thread process sent messages (it does inside PeekMessage/GetMessage)? Up to 1.5 s.
        std::string probePump(HWND window)
        {
            const uint64_t start = ps2_host_display::hostSteadyMs();
            DWORD_PTR result = 0;
            const LRESULT ok = SendMessageTimeoutW(window, WM_NULL, 0, 0, SMTO_BLOCK, 1500, &result);
            char text[96];
            if (ok)
                std::snprintf(text, sizeof(text), "answered WM_NULL in %llu ms", static_cast<unsigned long long>(ps2_host_display::hostSteadyMs() - start));
            else
                std::snprintf(text, sizeof(text), "no answer to WM_NULL in 1.5 s (error %lu)", GetLastError());
            return text;
        }

        std::string heartbeat(uint64_t &iterations)
        {
            uint64_t lastPump = 0;
            ps2_host_display::hostLoopHeartbeat(iterations, lastPump);
            char text[128];
            std::snprintf(text, sizeof(text), "host loop: %llu iterations, last message pump %llu ms ago",
                          static_cast<unsigned long long>(iterations),
                          static_cast<unsigned long long>(lastPump ? ps2_host_display::hostSteadyMs() - lastPump : 0));
            return text;
        }
    }

    // Oct 2026: the "crashes" were hangs: Windows' records say "stopped responding" (the game window's thread stopped
    // taking messages while the game itself ran on), and Kris closed them. Once the window has not answered for a few
    // seconds, every thread's native stack goes to the log (hang_dump), once per hang.
    // The first dump (Oct 24) showed the main thread alive in its loop (sleeping between presents, the pads still
    // sampled) while Windows kept the window flagged for four minutes: so the watch now also logs the loop's own
    // heartbeat, whether the window's thread answers a sent message, whether user32's message pump is hooked by another
    // module, and the modules loaded from outside Windows and the game folder; and looks again 15 s later.
    // Ghosting (Windows' "Not Responding" copy of the window, which hides the GS's picture) is off unless
    // PS2X_WINDOW_GHOSTING=1: the game keeps drawing into its own window either way.
    void startWindowWatch()
    {
        if (const char *ghosting = std::getenv("PS2X_WINDOW_GHOSTING"); !ghosting || *ghosting != '1')
            DisableProcessWindowsGhosting();
        GH_INFO(Boot, "window watch: " << hookOf("PeekMessageW") << "; " << hookOf("GetMessageW") << "; modules from outside Windows and the game: "
                                       << foreignModules());
        std::thread([]
                    {
                        struct Find
                        {
                            DWORD pid;
                            HWND hung;
                        };
                        int seconds = 0;
                        uint64_t iterationsAtDump = 0;
                        for (;;)
                        {
                            std::this_thread::sleep_for(std::chrono::seconds(1));
                            Find find{GetCurrentProcessId(), nullptr};
                            EnumWindows([](HWND window, LPARAM param) -> BOOL
                                        {
                                            auto *f = reinterpret_cast<Find *>(param);
                                            DWORD pid = 0;
                                            GetWindowThreadProcessId(window, &pid);
                                            if (pid == f->pid && IsWindowVisible(window) && IsHungAppWindow(window))
                                            {
                                                f->hung = window;
                                                return FALSE;
                                            }
                                            return TRUE; },
                                        reinterpret_cast<LPARAM>(&find));
                            if (!find.hung)
                            {
                                if (seconds >= 2)
                                    GH_WARN(Boot, "hang: the game window answers again after ~" << seconds + 5 << " s");
                                seconds = 0;
                                continue;
                            }
                            if (++seconds == 2)
                            {
                                const DWORD thread = GetWindowThreadProcessId(find.hung, nullptr);
                                GH_WARN(Boot, "hang: the game window (thread " << thread << ", " << windowName(find.hung)
                                                                               << ") has not taken messages for ~7 s; " << heartbeat(iterationsAtDump)
                                                                               << "; " << probePump(find.hung) << "; " << hookOf("PeekMessageW") << "; "
                                                                               << hookOf("GetMessageW") << "; foreign modules: " << foreignModules()
                                                                               << "; every thread's stack follows");
                                godhand::hangdump::dumpNow(true);
                            }
                            else if (seconds == 17)
                            {
                                uint64_t iterations = 0;
                                const std::string beat = heartbeat(iterations);
                                GH_WARN(Boot, "hang: still hung 15 s after the dump; " << beat << " (" << iterations - iterationsAtDump
                                                                                         << " since the dump); " << probePump(find.hung)
                                                                                         << (iterations == iterationsAtDump ? "; the loop did not move: every thread's stack follows" : "; the loop runs: Windows is not seeing its message pump"));
                                if (iterations == iterationsAtDump)
                                    godhand::hangdump::dumpNow(true);
                            }
                        } })
            .detach();
    }

    void installFatalExceptionLog()
    {
        AddVectoredExceptionHandler(0, &fatalExceptionHandler);
    }
#else
    void startWindowWatch() {}
    void reportPreviousCrashes() {}
    void installFatalExceptionLog() {}
#endif
}
