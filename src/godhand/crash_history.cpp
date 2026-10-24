// Crashes the in-process reporter cannot see, made visible in the log (crash_history.h).
//
// Oct 2026: three runs ended mid-play with nothing in the log (no "[crash]" lines, no "runtime exited"): the process
// was ended by a fail-fast or a corrupted heap, which skip the unhandled-exception filter. Windows' error reporting
// writes every such end to the Application event log (event 1000, "Application Error": module, exception code,
// offset), which a normal user can read; so the next run reports them.
#include "godhand/crash_history.h"
#include "godhand/log.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#include <winevt.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
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

        // "function+0x.. file:line" for an offset into a module of this process, when it is the same build.
        std::string symbolize(const std::wstring &moduleName, const std::wstring &moduleStamp, unsigned long long offset)
        {
            HMODULE module = GetModuleHandleW(moduleName.c_str());
            if (!module)
                return "(module not loaded now: not named)";
            const unsigned long stamp = std::wcstoul(moduleStamp.c_str(), nullptr, 16);
            if (stamp != imageTimeStamp(module))
                return "(another build of the module: not named)";
            HANDLE process = GetCurrentProcess();
            const DWORD64 address = reinterpret_cast<DWORD64>(module) + offset;
            alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 256] = {};
            auto *symbol = reinterpret_cast<SYMBOL_INFO *>(storage);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = 255;
            DWORD64 displacement = 0;
            IMAGEHLP_LINE64 line{};
            line.SizeOfStruct = sizeof(line);
            DWORD lineDisplacement = 0;
            if (!SymFromAddr(process, address, &displacement, symbol))
                return "(no symbol)";
            char text[600];
            if (SymGetLineFromAddr64(process, address, &lineDisplacement, &line))
                std::snprintf(text, sizeof(text), "%s+0x%llx %s:%lu", symbol->Name, static_cast<unsigned long long>(displacement),
                              line.FileName, line.LineNumber);
            else
                std::snprintf(text, sizeof(text), "%s+0x%llx", symbol->Name, static_cast<unsigned long long>(displacement));
            return text;
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
            HANDLE process = GetCurrentProcess();
            SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
            const bool symbols = SymInitialize(process, nullptr, TRUE) != FALSE;
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
                    const std::string where = symbols ? symbolize(data[3], data[5], offset) : std::string("(no symbols)");
                    GH_INFO(Boot, "crash history: " << narrow(time) << " UTC godhand.exe crashed: exception 0x" << std::hex << code
                                                    << std::dec << " " << codeMeaning(code) << " in " << narrow(data[3]) << "+0x"
                                                    << std::hex << offset << std::dec << " " << where);
                }
            }
            if (symbols)
                SymCleanup(process);
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

    void installFatalExceptionLog()
    {
        AddVectoredExceptionHandler(0, &fatalExceptionHandler);
    }
#else
    void reportPreviousCrashes() {}
    void installFatalExceptionLog() {}
#endif
}
