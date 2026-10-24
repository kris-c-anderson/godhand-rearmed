// God Hand (USA, SLUS-21503) native port: the game layer's entry point (DRAFT, not compiled yet).
// Adapted from the Urban Reign port's src/main.cpp (forks/urban-reign-recomp @2fa53c4). Kept: the generic start-up
// (CPU check, crash reporter, settings file -> environment, disc check, runtime set-up, function hooks, HLE bindings,
// diagnostics). Removed: everything Urban Reign-specific (GODHAND_PORT_NOTES.md lists each part with its line numbers).
// Added: hidden (headless) runs, a wall-clock time limit with a hang dump, exit codes for scripted boots, and the
// SNR2 DLL load logger.

#include "runtime/ee_scheduler.h"
#include "runtime/ps2_async_vif.h"
#include "runtime/ps2_display_options.h"
#include "runtime/ps2_input_options.h"
#include "runtime/ps2_pnach.h"
#include "runtime/ps2_save_state.h"
#include "ps2_runtime.h"
#include "runtime/gs/gs_gpu_backend.h"
#include "runtime/gs/gs_pcsx2_backend.h"
#include "runtime/gs/gs_threaded_backend.h"
#include "godhand/call_profile.h"
#include "godhand/guest_symbols.h"
#include "godhand/button_prompts.h"
#include "godhand/call_trace.h"
#include "godhand/default_ini.h" // generated from godhand.ini by src/CMakeLists.txt
#include "godhand/function_hooks.h"
#include "godhand/game_disc.h"
#include "godhand/hang_dump.h"
#include "godhand/hle_bindings.h"
#include "godhand/host_window.h"
#include "godhand/log.h"
#include "godhand/movie_watchdog.h"
#include "godhand/frame_patches.h"
#include "godhand/game_options.h"
#include "godhand/character_mods.h"
#include "godhand/widescreen.h"
#if GODHAND_HAS_LAUNCHER
#include "godhand/launcher.h"
#endif
#include "godhand/pc_sampler.h"
#include "godhand/thread_policy.h"

#if defined(PS2X_ENABLE_DEBUG_UI)
#include "ps2_debug_panel.h"
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <xmmintrin.h>
#include <pmmintrin.h>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <intrin.h>
#include <io.h>
#include <csignal>
#include <cwchar>
#endif

#if GODHAND_HAS_VU1_PROGRAMS
void registerGeneratedVu1Programs(); // generated/vu1/vu1_programs.cpp (VU1 microcode compiled ahead of time)
#endif

#if !defined(GODHAND_ARCH_NAME)
#define GODHAND_ARCH_NAME "SSE41"
#endif

namespace
{
    namespace fs = std::filesystem;

    // Exit codes, for scripted boots (build.py boot reads them).
    constexpr int kExitOk = 0;              // the runtime returned by itself (window closed, game thread ended)
    constexpr int kExitInitFailed = 1;      // runtime/ELF initialisation failed or a fatal exception
    constexpr int kExitNoDisc = 2;          // no disc image found
    constexpr int kExitBadDisc = 3;         // not the God Hand (USA) disc, or the boot ELF hash differs
    constexpr int kExitCpu = 4;             // the CPU lacks SSE4.1 (or AVX2 for an AVX2 build)
    constexpr int kExitMissingFunction = 5; // stopped on a call to a guest address with no recompiled function (policy Stop)
    constexpr int kExitCrash = 6;           // unhandled exception, std::terminate or abort (see the [crash] lines)
    constexpr int kExitTimeLimit = 124;     // GODHAND_TIME_LIMIT_SECONDS reached (same code as GNU timeout)

    const fs::file_time_type g_startFileTime = fs::file_time_type::clock::now();

    void setEnv(const std::string &variable, const std::string &value)
    {
#if defined(_WIN32)
        _putenv_s(variable.c_str(), value.c_str());
#else
        setenv(variable.c_str(), value.c_str(), 1);
#endif
    }

    void setDefaultEnv(const std::string &variable, const std::string &value)
    {
        if (value.empty() || std::getenv(variable.c_str()))
            return;
        setEnv(variable, value);
    }

    bool envFlag(const char *variable, bool fallback)
    {
        const char *value = std::getenv(variable);
        if (!value || !*value)
            return fallback;
        return *value != '0';
    }

#if defined(_WIN32)
    // The exe needs SSE4.1 (the PCSX2 minimum; the runtime uses SSE4.1 intrinsics), and AVX2 when built with
    // GODHAND_ARCH=AVX2. Checked before any runtime code runs, so an old CPU gets a message instead of an
    // illegal-instruction crash. The default build's own compiler output is SSE2 only. GENERIC (Urban main.cpp:73-98).
    bool checkCpu(bool dialog)
    {
        int regs[4] = {};
        __cpuid(regs, 0);
        const int maxLeaf = regs[0];
        __cpuid(regs, 1);
        const bool sse41 = (regs[2] & (1 << 19)) != 0;
        const bool osAvx = (regs[2] & (1 << 27)) != 0 && (regs[2] & (1 << 28)) != 0 && (_xgetbv(0) & 6u) == 6u;
        bool avx2 = false;
        if (maxLeaf >= 7)
        {
            __cpuidex(regs, 7, 0);
            avx2 = osAvx && (regs[1] & (1 << 5)) != 0;
        }
        const bool needAvx2 = std::string_view(GODHAND_ARCH_NAME) == "AVX2";
        std::fprintf(stderr, "[cpu] SSE4.1 %s, AVX2 %s; build %s\n", sse41 ? "yes" : "NO", avx2 ? "yes" : "no", GODHAND_ARCH_NAME);
        const char *missing = !sse41 ? "SSE4.1" : (needAvx2 && !avx2 ? "AVX2" : nullptr);
        if (!missing)
            return true;
        std::fprintf(stderr, "[cpu] this processor does not support %s, which this build of God Hand needs\n", missing);
        if (dialog) // never a dialog in hidden runs: nothing may appear on screen
        {
            wchar_t text[160];
            std::swprintf(text, 160, L"This processor does not support %hs, which this build of God Hand needs.", missing);
            MessageBoxW(nullptr, text, L"God Hand: Rearmed", MB_OK | MB_ICONERROR);
        }
        return false;
    }

    void printStackTrace()
    {
        void *frames[48];
        const USHORT count = CaptureStackBackTrace(0, 48, frames, nullptr);
        HANDLE process = GetCurrentProcess();
        SymInitialize(process, nullptr, TRUE);
        alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 256];
        auto *symbol = reinterpret_cast<SYMBOL_INFO *>(storage);
        for (USHORT i = 0; i < count; ++i)
        {
            std::memset(storage, 0, sizeof(storage));
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = 255;
            DWORD64 displacement = 0;
            const DWORD64 address = reinterpret_cast<DWORD64>(frames[i]);
            IMAGEHLP_LINE64 line{};
            line.SizeOfStruct = sizeof(line);
            DWORD lineDisplacement = 0;
            const bool haveSymbol = SymFromAddr(process, address, &displacement, symbol) != FALSE;
            const bool haveLine = SymGetLineFromAddr64(process, address, &lineDisplacement, &line) != FALSE;
            std::fprintf(stderr, "[crash]   #%u %s+0x%llx %s:%lu\n", static_cast<unsigned>(i), haveSymbol ? symbol->Name : "?",
                         static_cast<unsigned long long>(displacement), haveLine ? line.FileName : "", haveLine ? line.LineNumber : 0ul);
        }
        std::fflush(stderr);
    }

    std::atomic<PS2Runtime *> g_crashRuntime{nullptr};

    // A native crash in a normal (windowed) run: the exception, the module and offset of the faulting code, the guest
    // PC the EE was at, and the host stack walked from the faulting context (function names when godhand.pdb is next
    // to godhand.exe), written to the log before Windows' own crash handling goes on.
    LONG WINAPI crashLogFilter(EXCEPTION_POINTERS *info)
    {
        static std::atomic<bool> entered{false};
        if (!info || !info->ExceptionRecord || entered.exchange(true))
            return EXCEPTION_CONTINUE_SEARCH;
        const EXCEPTION_RECORD &record = *info->ExceptionRecord;
        HANDLE process = GetCurrentProcess();
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
        SymInitialize(process, nullptr, TRUE);
        auto describe = [&](DWORD64 address)
        {
            char text[512];
            HMODULE module = nullptr;
            char modulePath[MAX_PATH] = "?";
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCSTR>(address), &module))
                GetModuleFileNameA(module, modulePath, MAX_PATH);
            const char *moduleName = std::strrchr(modulePath, '\\') ? std::strrchr(modulePath, '\\') + 1 : modulePath;
            alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 256] = {};
            auto *symbol = reinterpret_cast<SYMBOL_INFO *>(storage);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = 255;
            DWORD64 displacement = 0;
            IMAGEHLP_LINE64 line{};
            line.SizeOfStruct = sizeof(line);
            DWORD lineDisplacement = 0;
            const bool haveSymbol = SymFromAddr(process, address, &displacement, symbol) != FALSE;
            const bool haveLine = SymGetLineFromAddr64(process, address, &lineDisplacement, &line) != FALSE;
            std::snprintf(text, sizeof(text), "%s+0x%llx %s+0x%llx %s:%lu", moduleName,
                          static_cast<unsigned long long>(address - reinterpret_cast<DWORD64>(module)), haveSymbol ? symbol->Name : "?",
                          static_cast<unsigned long long>(displacement), haveLine ? line.FileName : "", haveLine ? line.LineNumber : 0ul);
            return std::string(text);
        };
        const DWORD64 faultAddress = reinterpret_cast<DWORD64>(record.ExceptionAddress);
        std::fprintf(stderr, "[crash] unhandled exception 0x%08lX on thread %lu at %s\n", static_cast<unsigned long>(record.ExceptionCode),
                     GetCurrentThreadId(), describe(faultAddress).c_str());
        if (record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record.NumberParameters >= 2)
            std::fprintf(stderr, "[crash] %s address 0x%llx\n", record.ExceptionInformation[0] == 1 ? "write to" : record.ExceptionInformation[0] == 8 ? "execute at" : "read from",
                         static_cast<unsigned long long>(record.ExceptionInformation[1]));
        if (PS2Runtime *runtime = g_crashRuntime.load())
        {
            if (const R5900Context *ctx = runtime->eeScheduler().currentContext())
                std::fprintf(stderr, "[crash] guest: pc 0x%08x (%s) ra 0x%08x (%s) sp 0x%08x\n", ctx->pc,
                             godhand::symbols::describe(ctx->pc).c_str(),
                             static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31])),
                             godhand::symbols::describe(static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]))).c_str(),
                             static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[29])));
        }
        if (record.ExceptionCode != EXCEPTION_STACK_OVERFLOW && info->ContextRecord)
        {
            CONTEXT context = *info->ContextRecord;
            STACKFRAME64 frame{};
            frame.AddrPC.Offset = context.Rip;
            frame.AddrPC.Mode = AddrModeFlat;
            frame.AddrFrame.Offset = context.Rbp;
            frame.AddrFrame.Mode = AddrModeFlat;
            frame.AddrStack.Offset = context.Rsp;
            frame.AddrStack.Mode = AddrModeFlat;
            for (int i = 0; i < 40; ++i)
            {
                if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(), &frame, &context, nullptr,
                                 SymFunctionTableAccess64, SymGetModuleBase64, nullptr) ||
                    frame.AddrPC.Offset == 0)
                    break;
                std::fprintf(stderr, "[crash]   #%d %s\n", i, describe(frame.AddrPC.Offset).c_str());
            }
        }
        std::fflush(stdout);
        std::fflush(stderr);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // GENERIC (Urban main.cpp:141-168).
    void installCrashReporter()
    {
        if (!envFlag("GODHAND_HEADLESS", false))
            SetUnhandledExceptionFilter(crashLogFilter);
        std::set_terminate([]()
                           {
                               std::fprintf(stderr, "[crash] std::terminate on thread %lu\n", GetCurrentThreadId());
                               if (const std::exception_ptr ep = std::current_exception())
                               {
                                   try
                                   {
                                       std::rethrow_exception(ep);
                                   }
                                   catch (const std::exception &e)
                                   {
                                       std::fprintf(stderr, "[crash] exception: %s\n", e.what());
                                   }
                                   catch (...)
                                   {
                                       std::fprintf(stderr, "[crash] non-std exception\n");
                                   }
                               }
                               printStackTrace();
                               std::_Exit(kExitCrash); });
        std::signal(SIGABRT, [](int)
                    {
                        std::fprintf(stderr, "[crash] abort on thread %lu\n", GetCurrentThreadId());
                        printStackTrace();
                        std::_Exit(kExitCrash); });
    }

    // Hidden runs: nothing may appear on screen, not even Windows Error Reporting's "stopped working" dialog, the
    // CRT abort() message box or a critical-error box. A native crash prints its code and address, a stack trace,
    // then exits with kExitCrash.
    LONG WINAPI hiddenRunExceptionFilter(EXCEPTION_POINTERS *info)
    {
        const DWORD code = info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0;
        const void *address = info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionAddress : nullptr;
        std::fprintf(stderr, "[crash] unhandled exception 0x%08lX at %p on thread %lu\n", static_cast<unsigned long>(code),
                     address, GetCurrentThreadId());
        if (code != EXCEPTION_STACK_OVERFLOW)
            printStackTrace();
        std::fflush(stdout);
        std::fflush(stderr);
        std::_Exit(kExitCrash);
    }

    void installHiddenRunErrorHandling()
    {
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
        SetUnhandledExceptionFilter(hiddenRunExceptionFilter);
    }
#endif

    fs::path executableDirectory()
    {
#if defined(_WIN32)
        wchar_t buffer[MAX_PATH];
        const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
        if (length > 0 && length < MAX_PATH)
            return fs::path(buffer).parent_path();
#endif
        return fs::current_path();
    }

    // The EE FPU truncates and flushes denormals; PCSX2's default EE rounding mode is also Chop. GENERIC.
    void applyEeFloatingPointMode()
    {
        _MM_SET_ROUNDING_MODE(_MM_ROUND_TOWARD_ZERO);
        _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
        _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
    }

    // ---- godhand.ini -> environment ---------------------------------------------------------------------------------
    // GENERIC idea of Urban main.cpp:435-556 (config.ini lines become environment variables; the environment wins),
    // without Urban's legacy-file conversion, mods list and template upgrade.

    std::string trimText(const std::string &text)
    {
        const size_t begin = text.find_first_not_of(" \t\r");
        const size_t end = text.find_last_not_of(" \t\r");
        return begin == std::string::npos ? std::string() : text.substr(begin, end - begin + 1);
    }

    std::string lowerText(std::string text)
    {
        for (char &c : text)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return text;
    }

    struct IniSetting
    {
        const char *section; // lower case
        const char *key;     // lower case
        const char *variable;
    };
    constexpr IniSetting kIniSettings[] = {
        {"game", "iso", "GODHAND_ISO"},
        {"game", "fps60", "GODHAND_FPS60"},
        {"game", "l3dash", "GODHAND_L3_DASH"},
        {"game", "roulettetimer", "GODHAND_ROULETTE_TIMER"},
        {"game", "unlockhard", "GODHAND_UNLOCK_HARD"},
        {"sound", "mutemusic", "GODHAND_MUTE_MUSIC"},
        {"mods", "character", "GODHAND_CHARACTER"},
        {"run", "headless", "GODHAND_HEADLESS"},
        {"run", "timelimitseconds", "GODHAND_TIME_LIMIT_SECONDS"},
        {"run", "dumpontimelimit", "GODHAND_DUMP_ON_TIME_LIMIT"},
        {"run", "missingfunction", "GODHAND_MISSING_FUNCTION"},
        {"run", "dlllog", "GODHAND_DLL_LOG"},
        {"run", "logfile", "GODHAND_LOG_FILE"},
        {"video", "renderer", "PS2X_GS_RENDERER"},
        {"video", "internalresolution", "PS2X_GS_SCALE"},
        {"video", "startmaximized", "GODHAND_START_MAXIMIZED"},
        {"video", "windowsize", "PS2X_WINDOW_SIZE"},
        {"video", "aspectratio", "GODHAND_ASPECT"},
        {"video", "hudlayout", "GODHAND_HUD"},
        {"video", "menulayout", "GODHAND_MENUS"},
        {"video", "fieldofview", "GODHAND_FOV"},
        {"video", "startfullscreen", "GODHAND_START_FULLSCREEN"},
        {"video", "posteffects", "PS2X_PCSX2_NATIVE_SCALING"},
        {"launcher", "show", "GODHAND_LAUNCHER"},
    };

    // Variables holding a path: a relative value from godhand.ini is made absolute against the exe folder (a value
    // from the real environment is left as it is), and the folder it needs is created.
    enum class PathUse
    {
        Read,       // nothing created
        File,       // its parent folder is created
        Directory,  // the folder itself is created
    };
    struct PathVariable
    {
        const char *variable;
        PathUse use;
    };
    constexpr PathVariable kPathVariables[] = {
        {"GODHAND_ISO", PathUse::Read},
        {"GODHAND_DLL_LOG", PathUse::File},
        {"GODHAND_LOG_FILE", PathUse::File},
        {"GODHAND_DUMP_RAM", PathUse::File},
        {"GODHAND_DUMP_VRAM", PathUse::File},
        {"GODHAND_QUICK_STATE", PathUse::File},
        {"GODHAND_CONTROLS_FILE", PathUse::File},
        {"GODHAND_MC_DIR", PathUse::Directory},
        {"PS2X_DUMP_RAM_ON_MISSING", PathUse::File},
        {"PS2X_BRANCH_TRACE_FILE", PathUse::File},
        {"PS2X_FRAME_TIMES", PathUse::File},
        {"PS2X_SCREENSHOT_DIR", PathUse::Directory},
        {"PS2X_GS_SHADER_CACHE_DIR", PathUse::Directory},
    };

    std::string resolvePath(const fs::path &exeDir, const std::string &variable, const std::string &value)
    {
        for (const PathVariable &path : kPathVariables)
        {
            if (variable != path.variable)
                continue;
            fs::path resolved(value);
            if (resolved.is_relative())
                resolved = (exeDir / resolved).lexically_normal();
            std::error_code error;
            if (path.use == PathUse::File && resolved.has_parent_path())
                fs::create_directories(resolved.parent_path(), error);
            else if (path.use == PathUse::Directory)
                fs::create_directories(resolved, error);
            return resolved.string();
        }
        return value;
    }

    // PS2X_PAD1, PS2X_PAD1_CROSS, ...: the controller layout an older godhand.ini carried (not PS2X_PAD_SCRIPT/_FIFO).
    bool padLayoutVariable(const std::string &variable)
    {
        return variable.size() >= 9u && variable.rfind("PS2X_PAD", 0) == 0 && variable[8] >= '1' && variable[8] <= '8' &&
               (variable.size() == 9u || variable[9] == '_');
    }

    // Reads godhand.ini (written from the built-in default when missing). Returns the lines to log once the log is set
    // up ("ini: X=..." / "env: X=... (wins over godhand.ini)"). PS2X_PAD<n>* lines are not exported: they go to
    // `padLayout`, which only seeds the controls file (controls.ini) when that does not exist yet.
    std::set<std::string> g_iniVariables; // set from godhand.ini (not the real environment)

    std::vector<std::string> loadSettings(const fs::path &iniPath, const fs::path &exeDir,
                                          std::vector<std::pair<std::string, std::string>> &padLayout)
    {
        std::vector<std::string> notes;
        if (!fs::exists(iniPath))
        {
            std::ofstream(iniPath, std::ios::binary) << godhand::kDefaultIni;
            notes.push_back("wrote the default " + iniPath.string());
        }
        std::ifstream file(iniPath);
        if (!file)
        {
            notes.push_back("cannot read " + iniPath.string() + "; using the environment only");
            return notes;
        }
        std::string section;
        std::string line;
        while (std::getline(file, line))
        {
            line = trimText(line);
            if (line.empty() || line[0] == '#' || line[0] == ';')
                continue;
            if (line.front() == '[' && line.back() == ']')
            {
                section = lowerText(trimText(line.substr(1, line.size() - 2)));
                continue;
            }
            const size_t equals = line.find('=');
            if (equals == std::string::npos)
                continue;
            const std::string key = trimText(line.substr(0, equals));
            const std::string value = trimText(line.substr(equals + 1));
            std::string variable;
            for (const IniSetting &setting : kIniSettings)
                if (section == setting.section && lowerText(key) == setting.key)
                    variable = setting.variable;
            // PS2X_* / GODHAND_* switches, and SDL_* hints (SDL3 reads them from the environment, e.g.
            // SDL_JOYSTICK_HIDAPI_STEAM=1).
            if (variable.empty() && (key.rfind("PS2X_", 0) == 0 || key.rfind("GODHAND_", 0) == 0 || key.rfind("SDL_", 0) == 0))
                variable = key;
            if (variable.empty())
            {
                notes.push_back("unknown setting [" + section + "] " + key + " (ignored)");
                continue;
            }
            // Read again after the launcher changed the file: variables this file set before follow it (an emptied
            // value clears them); the real environment still wins.
            const bool ours = g_iniVariables.count(variable) != 0u;
            if (value.empty())
            {
                if (ours)
                    setEnv(variable, "");
                continue;
            }
            if (padLayoutVariable(variable))
            {
                padLayout.emplace_back(variable, value);
                continue;
            }
            if (const char *current = std::getenv(variable.c_str()); current && !ours)
            {
                notes.push_back("env: " + variable + "=" + current + " (wins over godhand.ini " + value + ")");
                continue;
            }
            const std::string resolved = resolvePath(exeDir, variable, value);
            setEnv(variable, resolved);
            g_iniVariables.insert(variable);
            notes.push_back("ini: " + variable + "=" + resolved);
        }
        return notes;
    }

    // GODHAND_LOG_FILE=<file>: stdout and stderr appended to that file (one handle, so the lines stay in order).
    void redirectOutput()
    {
#if defined(_WIN32)
        const char *path = std::getenv("GODHAND_LOG_FILE");
        if (!path || !*path)
            return;
        if (!std::freopen(path, "a", stdout))
            return;
        _dup2(_fileno(stdout), _fileno(stderr));
        std::setvbuf(stderr, nullptr, _IONBF, 0);
        const std::time_t now = std::time(nullptr);
        char stamp[64] = {};
        std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
        std::fprintf(stdout, "\n===== godhand.exe run %s (pid %lu) =====\n", stamp, static_cast<unsigned long>(GetCurrentProcessId()));
        std::fflush(stdout);
#endif
    }

    // ---- run time limit ---------------------------------------------------------------------------------------------
    std::atomic<PS2Runtime *> g_capRuntime{nullptr};
    std::atomic<bool> g_timeLimitHit{false};

    // GODHAND_TIME_LIMIT_SECONDS=<s>: after that wall-clock time, a hang dump (GODHAND_DUMP_ON_TIME_LIMIT, default on),
    // then requestStop() every 0.5 s (run() clears the flag when it starts) for 10 s, then _Exit(124) if still running.
    void startTimeLimit()
    {
        const char *value = std::getenv("GODHAND_TIME_LIMIT_SECONDS");
        const double seconds = value ? std::strtod(value, nullptr) : 0.0;
        if (!(seconds > 0.0))
            return;
        const bool dump = envFlag("GODHAND_DUMP_ON_TIME_LIMIT", true);
        GH_INFO(Boot, "time limit: " << seconds << " s" << (dump ? " (hang dump at the limit)" : ""));
        // Hard stop at the limit + 40 s, independent of the thread below: the hang dump suspends every thread and walks
        // their stacks, and it can deadlock if a suspended thread holds the heap or loader lock.
        std::thread([seconds]() {
            std::this_thread::sleep_for(std::chrono::duration<double>(seconds + 40.0));
            std::fprintf(stderr, "[timelimit] still running 40 s after the time limit; hard exit with code %d\n", kExitTimeLimit);
            std::fflush(stdout);
            std::fflush(stderr);
            std::_Exit(kExitTimeLimit);
        }).detach();
        std::thread([seconds, dump]() {
            std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
            g_timeLimitHit.store(true);
            GH_WARN(Boot, "time limit of " << seconds << " s reached; stopping the run");
            if (dump)
                godhand::hangdump::dumpNow(true);
            for (int i = 0; i < 20; ++i)
            {
                if (PS2Runtime *runtime = g_capRuntime.load())
                    runtime->requestStop();
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
            GH_ERROR(Boot, "the runtime did not stop within 10 s of the time limit; exiting with code " << kExitTimeLimit);
            std::fflush(stdout);
            std::fflush(stderr);
            std::_Exit(kExitTimeLimit);
        }).detach();
    }

    PS2Runtime::MissingFunctionPolicy missingFunctionPolicy()
    {
        const char *value = std::getenv("GODHAND_MISSING_FUNCTION");
        const std::string policy = value ? lowerText(value) : std::string("stop");
        if (policy == "continue")
            return PS2Runtime::MissingFunctionPolicy::ContinueToTarget;
        if (policy == "break")
            return PS2Runtime::MissingFunctionPolicy::BreakOnce;
        if (policy == "skip")
            return PS2Runtime::MissingFunctionPolicy::SkipCallDebug;
        return PS2Runtime::MissingFunctionPolicy::Stop;
    }

    // The runtime writes PS2X_DUMP_RAM_ON_MISSING on the first missing call whatever the policy, so a fresh dump means
    // "stopped" only under Stop.
    bool stoppedOnMissingFunction()
    {
        if (missingFunctionPolicy() != PS2Runtime::MissingFunctionPolicy::Stop)
            return false;
        const char *dump = std::getenv("PS2X_DUMP_RAM_ON_MISSING");
        if (!dump || !*dump)
            return false;
        std::error_code error;
        const fs::file_time_type written = fs::last_write_time(dump, error);
        return !error && written >= g_startFileTime;
    }

    void printUsage()
    {
        std::cout << "usage: godhand [--iso <path-to-" << godhand::kSerial << ".iso>] [--headless|--window] [--time-limit <seconds>]\n"
                  << "               [--write-settings]\n"
                  << "settings: godhand.ini next to godhand.exe (environment variables of the same name win)\n"
                  << "exit codes: 0 ok, 1 init failed, 2 no disc, 3 wrong disc, 4 CPU, 5 missing function (stop), 6 crash, 124 time limit\n";
    }
}

int main(int argc, char *argv[])
{
    const fs::path exeDir = executableDirectory();

    // Command line first: it wins over the environment, which wins over godhand.ini.
    fs::path iso;
    bool writeSettingsOnly = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if ((arg == "--iso" || arg == "-iso") && i + 1 < argc)
            iso = argv[++i];
        else if (arg == "--headless")
            setEnv("GODHAND_HEADLESS", "1");
        else if (arg == "--window")
            setEnv("GODHAND_HEADLESS", "0");
        else if (arg == "--time-limit" && i + 1 < argc)
            setEnv("GODHAND_TIME_LIMIT_SECONDS", argv[++i]);
        else if (arg == "--write-settings")
            writeSettingsOnly = true;
        else if (arg == "--help" || arg == "-h")
        {
            printUsage();
            return kExitOk;
        }
        else if (iso.empty() && arg.rfind("-", 0) != 0)
            iso = arg;
    }

    // The self-test rewrites the controls file several times: only with a file named in the real environment.
    const bool controlsFileFromEnvironment = std::getenv("GODHAND_CONTROLS_FILE") && *std::getenv("GODHAND_CONTROLS_FILE");
    std::vector<std::pair<std::string, std::string>> padLayout;
    const std::vector<std::string> settingNotes = loadSettings(exeDir / "godhand.ini", exeDir, padLayout);
    if (writeSettingsOnly)
        return kExitOk;
    redirectOutput();
    const bool headless = envFlag("GODHAND_HEADLESS", false);

#if defined(_WIN32)
    if (headless)
        installHiddenRunErrorHandling();
    if (!checkCpu(!headless))
        return kExitCpu;
    installCrashReporter();
#endif
    godhand::log::configureFromEnvironment();
    for (const std::string &note : settingNotes)
        GH_INFO(Boot, "settings: " << note);
    if (!padLayout.empty())
        GH_INFO(Boot, "settings: " << padLayout.size() << " PS2X_PAD<n> line(s) in godhand.ini are not used: the layout lives in the "
                                   "controls file (they only seed it when it is missing)");
    // GODHAND_CONTROLS_FILE: the bindings file the F1 Controls screen edits (default controls.ini next to the exe).
    {
        const char *controls = std::getenv("GODHAND_CONTROLS_FILE");
        const fs::path controlsPath = controls && *controls ? fs::path(controls) : exeDir / "controls.ini";
        std::error_code sameError;
        const fs::path playerControls = exeDir / "controls.ini";
        const bool playerFile = fs::exists(controlsPath, sameError) && fs::exists(playerControls, sameError) &&
                                fs::equivalent(controlsPath, playerControls, sameError);
        if (envFlag("GODHAND_CONTROLS_SELFTEST", false) && (!controlsFileFromEnvironment || playerFile))
        {
            GH_WARN(Boot, "GODHAND_CONTROLS_SELFTEST ignored: it needs GODHAND_CONTROLS_FILE=<scratch file> in the environment"
                          " (not the player's controls.ini)");
            setEnv("GODHAND_CONTROLS_SELFTEST", "0");
        }
        GH_INFO(Boot, "controls file: " << controlsPath.string());
        ps2_host_input::setControlsFile(controlsPath.string(), padLayout);
    }
    godhand::hangdump::startFromEnvironment();
    godhand::threadpolicy::startFromEnvironment(); // PS2X_THREAD_POLICY (godhand.ini: off)
    startTimeLimit();                                // counts from here: covers disc check, init and the run

    // The disc image: --iso, then GODHAND_ISO / [Game] Iso, then an .iso in a game folder near the exe. Without one (or
    // with one that fails the check) a windowed run shows the launcher's Install screen; the launcher also shows at
    // start unless hidden ([Launcher] Show=0, Shift held brings it back).
    const fs::path cacheDir = exeDir / "game_data";
    const bool isoFromCommandLine = !iso.empty();
    auto resolveIso = [&]() -> fs::path
    {
        if (isoFromCommandLine)
            return iso;
        if (const char *env = std::getenv("GODHAND_ISO"); env && *env)
            return fs::path(env);
        return godhand::findDiscImage({exeDir / "game", exeDir.parent_path() / "game", exeDir.parent_path().parent_path() / "game",
                                       exeDir.parent_path().parent_path().parent_path() / "game", fs::current_path() / "game",
                                       fs::current_path()});
    };
    iso = resolveIso();
#if GODHAND_HAS_LAUNCHER
    const bool launcherAllowed = !headless && !isoFromCommandLine;
#else
    const bool launcherAllowed = false;
#endif
    // Shows the launcher; false = the player quit. Afterwards godhand.ini is read again (the settings it changed).
    auto showLauncher = [&](const std::string &message) -> bool
    {
#if GODHAND_HAS_LAUNCHER
        godhand::installWidescreen(); // the window's shape, as the game will have it
        ps2_host_display::setStartMaximized(envFlag("GODHAND_START_MAXIMIZED", false));
        ps2_host_display::setStartFullscreen(envFlag("GODHAND_START_FULLSCREEN", false));
        const godhand::launcher::Result result =
            godhand::launcher::run(godhand::launcher::Context{exeDir, exeDir / "godhand.ini", cacheDir, iso, message});
        if (result == godhand::launcher::Result::Quit)
            return false;
        std::vector<std::pair<std::string, std::string>> unusedPadLayout;
        for (const std::string &note : loadSettings(exeDir / "godhand.ini", exeDir, unusedPadLayout))
            GH_INFO(Boot, "settings (after the launcher): " << note);
        iso = resolveIso();
        return true;
#else
        (void)message;
        return false;
#endif
    };
    {
        std::error_code isoError;
#if GODHAND_HAS_LAUNCHER
        const bool showAtStart = launcherAllowed && godhand::launcher::wanted(!iso.empty() && fs::is_regular_file(iso, isoError));
#else
        const bool showAtStart = false;
#endif
        if (showAtStart)
        {
            if (!showLauncher(""))
            {
                GH_INFO(Boot, "quit from the launcher");
                return kExitOk;
            }
        }
    }
    godhand::DiscCheckResult disc;
    for (;;)
    {
        if (iso.empty())
        {
            if (!launcherAllowed)
            {
                GH_ERROR(Boot, "no disc image found. Pass --iso <path> (or [Game] Iso=) to your own " << godhand::kGameTitle
                                                                                                     << " (" << godhand::kSerial << ") disc image.");
                printUsage();
                return kExitNoDisc;
            }
            if (!showLauncher("Choose your God Hand (USA, SLUS-21503) disc image."))
                return kExitOk;
            continue;
        }
        GH_INFO(Boot, "disc image: " << iso.string());
        disc = godhand::verifyDiscAndPrepare(iso, cacheDir);
        if (disc.ok)
            break;
        GH_ERROR(Boot, disc.error);
        if (!launcherAllowed)
            return kExitBadDisc;
        if (!showLauncher("The disc image " + iso.string() + " cannot be used: " + disc.error))
            return kExitOk;
    }

    PS2Runtime::IoPaths paths;
    paths.elfPath = disc.bootElfPath;
    paths.elfDirectory = cacheDir;
    paths.hostRoot = cacheDir;
    paths.cdRoot = cacheDir;
    paths.mcRoot = exeDir / "memcards" / "mc0";
    // GODHAND_MC_DIR=<folder>: memory cards from another folder (test drivers use a copy).
    if (const char *mcDir = std::getenv("GODHAND_MC_DIR"); mcDir && *mcDir)
        paths.mcRoot = fs::path(mcDir) / "mc0";
    setDefaultEnv("PS2X_GS_SHADER_CACHE_DIR", (cacheDir / "gs_shaders").string());
    paths.cdImage = fs::absolute(iso);
    // Disc image prefetch (2026-10-18): the game's disc reads are synchronous here (sceCdRead on the EE thread), so a
    // read that misses the OS file cache stalls the game; one took 1.2 s in stage 3. Reading the whole image once in
    // the background, at the lowest thread priority, puts it in the OS cache. GODHAND_ISO_PREFETCH=0 turns it off.
    if (!headless && envFlag("GODHAND_ISO_PREFETCH", true))
    {
        std::thread([image = paths.cdImage]
        {
#if defined(_WIN32)
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
#endif
            const auto started = std::chrono::steady_clock::now();
            std::ifstream file(image, std::ios::binary);
            std::vector<char> chunk(8u << 20u);
            uint64_t total = 0;
            while (file)
            {
                file.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
                total += static_cast<uint64_t>(file.gcount());
            }
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
            GH_INFO(Boot, "disc image prefetched into the OS file cache: " << (total >> 20) << " MB in " << ms << " ms");
        }).detach();
    }
    std::error_code error;
    fs::create_directories(paths.mcRoot, error);
    // GODHAND_QUICK_STATE=<file>: the F5/F9 save-state slot (default states/quick.state).
    const char *quickState = std::getenv("GODHAND_QUICK_STATE");
    ps2x::savestate::setQuickSlotPath(quickState && *quickState ? fs::path(quickState) : exeDir / "states" / "quick.state");

    // Runtime defaults both the SotC and the Urban Reign ports ship with (each still overridable: PS2X_GS_THREAD,
    // PS2X_GS_GPU, PS2X_GS_ASYNC_PRESENT, PS2X_EE_HOST_PACING, PS2X_MTVU). Urban's game-tuned values (EE loop cycles,
    // MTVU on, direct present, async readback, MCSERV switch) are left at the runtime's defaults.
    // PS2X_SIO2_RECV1=2: with the runtime default (0, sticky) one unanswered command in PADMAN's multi-port transfer
    // reports the whole transfer as no response, so God Hand's libpad never leaves DISCONN (padArea+0x70 = 0) and no
    // button reaches the game. Same PADMAN quirk Urban found. Hidden runs 2026-10-17 06:30-06:47: pad 0 mode 0x79,
    // padArea+0x70 = 6 (STABLE), title menus and gameplay take input. Still overridable.
    setDefaultEnv("PS2X_SIO2_RECV1", "2");
    // Audio time-stretch off by default (2026-10-17): during a slowdown the WSOLA stretch repeats 5-20 ms slices, which the
    // user heard as sounds "playing twice". With VU1 AOT + GPU GS the game holds 60 fps, so a rare stall now gives a short
    // dropout instead. PS2X_AUDIO_STRETCH=1 turns it back on.
    setDefaultEnv("PS2X_AUDIO_STRETCH", "0");
    GSThreadedBackend::SetEnabledByDefault(true);
    GSThreadedBackend::SetGpuByDefault(true);
    GSGpuBackend::SetAsyncPresentDefault(true);
    // MTVU (VU1 and the GIF on threads of their own, as PCSX2's MTVU) on by default since 2026-10-22: in gameplay the EE
    // thread spent ~27% in VU1 and ~5% in VIF/GIF and had speed drops into the 80s at 60 FPS with widescreen; with
    // MTVU it waits ~70% of the time and held 100%. PS2X_MTVU=0 (godhand.ini [Runtime]) runs VU1 on the EE thread.
    ps2x::asyncvif::setDefaultEnabled(true);
    ps2x::asyncvif::setHostPacingDefault(true);
    EeScheduler::setHostPacingDefault(true);

    // Hidden runs: no visible window, no sound, no monitor-driven vsync, no dialogs.
    if (headless)
    {
        const godhand::hostwindow::HeadlessMethod method = godhand::hostwindow::requestHeadless();
        setDefaultEnv("PS2X_AUDIO_MUTE", "1"); // the audio device still opens without the runtime's setHeadless
        setDefaultEnv("PS2X_VSYNC", "0");      // the software 60 Hz limiter instead of a hidden window's swap interval
        GH_INFO(Boot, "headless: " << godhand::hostwindow::methodName(method) << "; PS2X_AUDIO_MUTE=" << std::getenv("PS2X_AUDIO_MUTE")
                                    << " PS2X_VSYNC=" << std::getenv("PS2X_VSYNC"));
    }

    try
    {
        // Never destroyed: the process ends with std::_Exit while it is alive, so the time-limit thread can always use it.
        PS2Runtime &runtime = *new PS2Runtime();
        g_capRuntime.store(&runtime);
#if defined(_WIN32)
        g_crashRuntime.store(&runtime);
#endif
#if defined(PS2X_ENABLE_DEBUG_UI)
        // ImGui runtime debugger from PS2Recomp: F12 toggles it (F1 = Controls screen); GODHAND_DEBUG_UI=1 opens it at start-up.
        static PS2DebugPanel debugPanel;
        debugPanel.setVisible(!headless && envFlag("GODHAND_DEBUG_UI", false));
        runtime.setDebugUiCallbacks(
            [](PS2Runtime &, void *userData) { static_cast<PS2DebugPanel *>(userData)->initialize(); },
            [](PS2Runtime &rt, void *userData)
            {
                auto *panel = static_cast<PS2DebugPanel *>(userData);
                panel->draw(rt);
                // With PCSX2's GS its window covers this one: hidden while the debugger is open.
                GSPcsx2Backend::SetHostUiVisible(panel->isVisible());
            },
            [](PS2Runtime &, void *userData) { static_cast<PS2DebugPanel *>(userData)->shutdown(); },
            &debugPanel);
#endif
        ps2_host_display::setStartMaximized(!headless && envFlag("GODHAND_START_MAXIMIZED", false));
        godhand::installWidescreen(); // the picture's shape: before the window is made
        ps2_host_display::setStartFullscreen(!headless && envFlag("GODHAND_START_FULLSCREEN", false));
        if (!runtime.initialize(godhand::kGameTitle))
        {
            GH_ERROR(Boot, "runtime initialization failed");
            std::fflush(stdout);
            std::_Exit(kExitInitFailed);
        }
        GH_INFO(Boot, "runtime initialized; loading " << disc.bootElfPath.string());
        if (!runtime.loadELF(disc.bootElfPath.string()))
        {
            GH_ERROR(Boot, "failed to load " << disc.bootElfPath.string());
            std::fflush(stdout);
            std::_Exit(kExitInitFailed);
        }
        GH_INFO(Boot, "ELF loaded, entry 0x" << std::hex << runtime.cpu().pc);
        // PS2X_PNACH=<file>[;<file>...]: PCSX2 patches, patch=0 lines now (patch=1 lines at every vblank). Off by default.
        if (ps2x::pnach::loadFromEnvironment() != 0u)
            ps2x::pnach::applyOnce(runtime.memory().getRDRAM());
        PS2Runtime::setIoPaths(paths);
        godhand::characters::install(exeDir); // [Mods] Character: a replacement model for Gene

        runtime.setMissingFunctionPolicy(missingFunctionPolicy());
        GH_INFO(Boot, "missing-function policy: " << (std::getenv("GODHAND_MISSING_FUNCTION") ? std::getenv("GODHAND_MISSING_FUNCTION") : "stop")
                                                  << "; RAM dump on missing: "
                                                  << (std::getenv("PS2X_DUMP_RAM_ON_MISSING") ? std::getenv("PS2X_DUMP_RAM_ON_MISSING") : "(off)"));
        godhand::FunctionHooks::instance().attach(runtime);
        godhand::FunctionHooks::instance().observeEntry(runtime.cpu().pc, "boot_entry", [](uint8_t *, R5900Context *, PS2Runtime *) {
            applyEeFloatingPointMode(); // on the game thread
            GH_INFO(Ee, "EE FPU host mode: round toward zero, denormals flushed");
        });
        godhand::installHleBindings(runtime); // port_prep/hle-bindings owns the God Hand table
#if GODHAND_HAS_VU1_PROGRAMS
        registerGeneratedVu1Programs();
#endif
        godhand::installDllLoadLogger(runtime); // observe only: [DLL] lines + GODHAND_DLL_LOG CSV
        godhand::installHiresFlipCopy(runtime); // PS2X_GS_SCALE > 1 only: flip copy without the flicker filter
        godhand::button_prompts::install(runtime); // the button prompts show the bound keys (GODHAND_PROMPTS=0: PS2 art)
        godhand::installMovieWatchdog(runtime); // sweep R3: ending/movie-list movie polls cannot spin forever
        // 60 FPS mode, L3 dash, and the game-frame count for the F3 overlay (a hook at the top of the main loop).
        godhand::installFramePatches(runtime);
        godhand::installGameOptions(runtime); // [Game] UnlockHard, [Sound] MuteMusic
        godhand::hangdump::setRuntime(&runtime);
        godhand::calltrace::installFromEnvironment(runtime);
        godhand::pcsampler::installFromEnvironment(runtime);
        godhand::FunctionHooks::instance().logBindings();
        godhand::callprofile::installFromEnvironment(runtime); // last: wraps whatever the slots hold now

        GH_INFO(Boot, "starting guest at 0x" << std::hex << runtime.cpu().pc);
        runtime.run();

        int code = kExitOk;
        const char *reason = "the runtime returned";
        if (g_timeLimitHit.load())
        {
            code = kExitTimeLimit;
            reason = "time limit";
        }
        else if (stoppedOnMissingFunction())
        {
            code = kExitMissingFunction;
            reason = "stopped on a missing function (see [guest-branch:missing-target] above)";
        }
        GH_INFO(Boot, "runtime exited: " << reason << "; exit code " << code << " after " << godhand::log::elapsedSeconds() << " s");
        std::cout.flush();
        std::fflush(stdout);
        std::fflush(stderr);
        std::_Exit(code);
    }
    catch (const std::exception &e)
    {
        GH_ERROR(Boot, "fatal: " << e.what());
        std::cout.flush();
        std::fflush(stderr);
        std::_Exit(kExitInitFailed);
    }
}
