#include "runtime/ps2_vu1_hooks.h"
#include "runtime/ps2_vu1.h"
#include "runtime/gs/ps2_gif_arbiter.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_memory.h"
#include "ps2_vu1_detail.h"
#define VU1_FORCE_INLINE inline
#include "ps2_vu1_impl.inl"

#include <algorithm>
#include <array>
#include <atomic>
#include <deque>
#include <map>
#include <bit>
#include <cfenv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <ps2_log.h>

namespace
{
    // Sweep R2: true when the last VU1 run on this thread stopped only because its cycle budget ran out: the
    // microprogram has not reached its E bit and resume() continues it exactly where it stopped.
    thread_local bool t_vu1BudgetHit = false;
    uint64_t fnv1a64(const uint8_t *data, size_t size)
    {
        uint64_t hash = 0xCBF29CE484222325ull;
        for (size_t i = 0; i < size; ++i)
        {
            hash ^= data[i];
            hash *= 0x100000001B3ull;
        }
        return hash;
    }

    struct Vu1ProgramCapture
    {
        std::filesystem::path directory;
        const PS2Memory *memory = nullptr;
        uint64_t generation = ~0ull;
        uint64_t imageHash = 0;
        std::unordered_map<uint64_t, uint64_t> entryCalls;
        uint64_t calls = 0;
    };

    Vu1ProgramCapture *vu1ProgramCapture()
    {
        static Vu1ProgramCapture *capture = []() -> Vu1ProgramCapture *
        {
            const char *value = std::getenv("PS2X_VU1_CAPTURE");
            if (!value || !*value)
                return nullptr;
            auto *result = new Vu1ProgramCapture();
            result->directory = value;
            std::error_code error;
            std::filesystem::create_directories(result->directory, error);
            return result;
        }();
        return capture;
    }

    struct Vu1ImageCacheEntry
    {
        std::vector<uint8_t> image;
        VU1InterpreterAccess::CompiledProgram program = nullptr;
        const VU1InterpreterAccess::FastBlock *table = nullptr; // runAssembledVu1's table (program == runAssembledVu1)
    };

    // A table of compiled blocks assembled for one micro memory image from the registered units (vu1_recomp: blocks of
    // captured images and of the microcode uploads found on the disc, each checked against the memory it would run).
    struct Vu1AssembledTable
    {
        std::array<VU1InterpreterAccess::FastBlock, VU1InterpreterAccess::kFastCodeSize / 8u> table{};
    };

    struct Vu1CompiledRegistry
    {
        std::unordered_map<uint64_t, VU1InterpreterAccess::CompiledProgram> programs;
        std::vector<Vu1CompiledUnit> units;
        std::unordered_map<uint64_t, std::unique_ptr<Vu1AssembledTable>> assembled; // by image hash; never evicted
        std::vector<Vu1ImageCacheEntry> recent;
        const PS2Memory *memory = nullptr;
        const uint8_t *code = nullptr;
        uint64_t generation = ~0ull;
        VU1InterpreterAccess::CompiledProgram current = nullptr;
        const VU1InterpreterAccess::FastBlock *currentTable = nullptr;
        bool enabled = true;
    };

    // The table runAssembledVu1 runs: set by lookupCompiledVu1Program on the thread that then runs the program.
    thread_local const VU1InterpreterAccess::FastBlock *t_vu1AssembledTable = nullptr;

    VU1InterpreterAccess::StepResult runAssembledVu1(VU1Interpreter &vu, const Vu1RunArgs &args)
    {
        if (!t_vu1AssembledTable)
            return VU1InterpreterAccess::StepResult::Fallback;
        return VU1InterpreterAccess::runFast(vu, args, t_vu1AssembledTable, VU1InterpreterAccess::kFastCodeSize / 8u);
    }

    // Log only (sweep R1): a VU1 micro memory image with no compiled (AOT) code runs every call in the interpreter, far
    // slower, with nothing in the log. Once per image: one line, and the image as vu1_<hash>.bin (the capture format
    // vu1_recomp reads) in PS2X_VU1_MISS_DIR (default logs/vu1_missing next to the exe's working folder; "0" = no file).
    void reportUncompiledVu1Image(uint64_t hash, const uint8_t *vuCode, uint32_t codeSize, const char *what = nullptr)
    {
        static std::unordered_map<uint64_t, bool> reported;
        if (reported.size() >= 256u || !reported.emplace(hash, true).second)
            return;
        static const std::string directory = []
        {
            const char *value = std::getenv("PS2X_VU1_MISS_DIR");
            return std::string(value && *value ? value : "logs/vu1_missing");
        }();
        std::string saved = "not saved";
        if (directory != "0")
        {
            std::error_code error;
            std::filesystem::create_directories(directory, error);
            char name[64];
            std::snprintf(name, sizeof(name), "vu1_%016llx.bin", static_cast<unsigned long long>(hash));
            const std::filesystem::path path = std::filesystem::path(directory) / name;
            if (std::filesystem::exists(path, error))
                saved = path.string() + " (already there)";
            else if (FILE *file = std::fopen(path.string().c_str(), "wb"))
            {
                const bool ok = std::fwrite(vuCode, 1, codeSize, file) == codeSize;
                saved = ok ? path.string() : "write failed";
                std::fclose(file);
            }
        }
        if (what)
            std::fprintf(stderr, "[vu1-aot] image %016llx: %s; %s\n", static_cast<unsigned long long>(hash), what, saved.c_str());
        else
            std::fprintf(stderr, "[vu1-aot] image %016llx has no compiled code: VU1 runs it in the interpreter (slower); %s\n",
                         static_cast<unsigned long long>(hash), saved.c_str());
    }

    // Assembles a table for this micro memory from the registered units: every block whose pairs [checkStart,
    // checkEnd) equal the memory serves its pairs (the first block found for a pair wins). Blocks compiled without
    // sticky status accumulation are only used when no instruction in the memory reads the status flags. Pairs no
    // block covers stay with the interpreter. Logs the coverage once per image; an image not fully covered is saved
    // to vu1_missing like an uncompiled one, so a later vu1_recomp run can compile it whole.
    const VU1InterpreterAccess::FastBlock *assembleVu1Image(Vu1CompiledRegistry &registry, uint64_t hash, const uint8_t *vuCode,
                                                            uint32_t codeSize)
    {
        constexpr uint32_t kPairs = VU1InterpreterAccess::kFastCodeSize / 8u;
        if (registry.units.empty() || codeSize != VU1InterpreterAccess::kFastCodeSize)
            return nullptr;
        if (const auto it = registry.assembled.find(hash); it != registry.assembled.end())
            return it->second ? it->second->table.data() : nullptr;
        if (registry.assembled.size() >= 4096u)
            return nullptr; // a game that keeps making new images: stop assembling (each table is 16 KB)
        bool readsStatus = false;
        uint32_t codePairs = 0u;
        for (uint32_t index = 0; index < kPairs; ++index)
        {
            uint32_t lower, upper;
            std::memcpy(&lower, vuCode + index * 8u, 4u);
            std::memcpy(&upper, vuCode + index * 8u + 4u, 4u);
            if (lower != 0u || upper != 0u)
                ++codePairs;
            const uint32_t opHi = (lower >> 25) & 0x7Fu;
            if (!(upper & 0x80000000u) && opHi >= 0x14u && opHi <= 0x17u)
                readsStatus = true;
        }
        auto assembled = std::make_unique<Vu1AssembledTable>();
        uint32_t blocks = 0u;
        // PS2X_VU1_ASSEMBLE_ONLY=uploads: only the blocks of uploads (tests the code compiled from the disc's uploads).
        static const bool uploadsOnly = []
        { const char *v = std::getenv("PS2X_VU1_ASSEMBLE_ONLY"); return v && std::strcmp(v, "uploads") == 0; }();
        for (const Vu1CompiledUnit &unit : registry.units)
        {
            if (uploadsOnly && unit.image)
                continue;
            for (uint32_t b = 0; b < unit.blockCount; ++b)
            {
                const Vu1CompiledUnitBlock &block = unit.blocks[b];
                if (block.noSticky && readsStatus)
                    continue;
                if (block.checkStart < unit.start || block.checkEnd > unit.end || block.checkEnd > kPairs)
                    continue;
                // The last pair in [checkStart, checkEnd) that differs from the unit (none: the whole block serves).
                uint32_t from = block.first;
                for (uint32_t index = block.checkEnd; index-- > block.checkStart;)
                    if (std::memcmp(vuCode + index * 8u, unit.bytes + (index - unit.start) * 8u, 8u) != 0)
                    {
                        // A later upload replaced code before this pair: a block compiled from its own pairs may still
                        // serve from the first safe entry after the difference.
                        from = block.last;
                        if (index >= block.first && index + 1u < block.last)
                            for (uint32_t p = index + 1u; p < block.last; ++p)
                            {
                                const uint32_t k = p - block.first;
                                if (k < 128u && (block.suffixSafe[k / 64u] >> (k % 64u)) & 1u)
                                {
                                    from = p;
                                    break;
                                }
                            }
                        break;
                    }
                bool used = false;
                for (uint32_t index = from; index < block.last && index < kPairs; ++index)
                    if (!assembled->table[index])
                    {
                        assembled->table[index] = block.fn;
                        used = true;
                    }
                blocks += used ? 1u : 0u;
            }
        }
        uint32_t compiledPairs = 0u;
        for (uint32_t index = 0; index < kPairs; ++index)
        {
            uint32_t pair[2];
            std::memcpy(pair, vuCode + index * 8u, 8u);
            if (assembled->table[index] && (pair[0] != 0u || pair[1] != 0u))
                ++compiledPairs;
        }
        if (compiledPairs < codePairs)
        {
            char what[160];
            std::snprintf(what, sizeof(what), "%u of %u code pairs compiled (assembled from %u blocks), the rest interpreted",
                          compiledPairs, codePairs, blocks);
            reportUncompiledVu1Image(hash, vuCode, codeSize, what);
        }
        else
            std::fprintf(stderr, "[vu1-aot] image %016llx: all %u code pairs compiled (assembled from %u blocks)\n",
                         static_cast<unsigned long long>(hash), codePairs, blocks);
        if (blocks == 0u)
            assembled.reset();
        const VU1InterpreterAccess::FastBlock *table = assembled ? assembled->table.data() : nullptr;
        registry.assembled.emplace(hash, std::move(assembled));
        return table;
    }

    VU1InterpreterAccess::CompiledProgram findCompiledVu1Image(Vu1CompiledRegistry &registry, const uint8_t *vuCode, uint32_t codeSize)
    {
        for (size_t index = 0; index < registry.recent.size(); ++index)
        {
            Vu1ImageCacheEntry &entry = registry.recent[index];
            if (entry.image.size() == codeSize && std::memcmp(entry.image.data(), vuCode, codeSize) == 0)
            {
                if (index != 0u)
                    std::swap(registry.recent[index], registry.recent[0]);
                registry.currentTable = registry.recent[0].table;
                return registry.recent[0].program;
            }
        }
        const uint64_t hash = fnv1a64(vuCode, codeSize);
        const auto it = registry.programs.find(hash);
        Vu1ImageCacheEntry entry;
        entry.image.assign(vuCode, vuCode + codeSize);
        entry.program = it == registry.programs.end() ? nullptr : it->second;
        // PS2X_VU1_ASSEMBLE_ONLY=1: images compiled whole are assembled from blocks too (tests the assembly, e.g. with
        // PS2X_VU1_VERIFY=obs, which ignores the stall bookkeeping the compiled code leaves out); =uploads: from the
        // blocks of uploads only.
        static const bool assembleOnly = []
        { const char *v = std::getenv("PS2X_VU1_ASSEMBLE_ONLY"); return v && (*v == '1' || std::strcmp(v, "uploads") == 0); }();
        if (assembleOnly && !registry.units.empty())
            entry.program = nullptr;
        if (!entry.program && !registry.units.empty())
        {
            entry.table = assembleVu1Image(registry, hash, vuCode, codeSize);
            if (entry.table)
                entry.program = &runAssembledVu1;
            else if (registry.assembled.size() >= 4096u)
                reportUncompiledVu1Image(hash, vuCode, codeSize);
        }
        else if (!entry.program)
            reportUncompiledVu1Image(hash, vuCode, codeSize);
        registry.currentTable = entry.table;
        constexpr size_t kRecentImages = 8u;
        if (registry.recent.size() < kRecentImages)
            registry.recent.push_back(std::move(entry));
        else
            registry.recent.back() = std::move(entry);
        std::swap(registry.recent.back(), registry.recent[0]);
        return registry.recent[0].program;
    }

    struct Vu1Trace
    {
        FILE *file = nullptr;
        std::filesystem::path directory;
        uint64_t first = 0;
        uint64_t last = 0;
        const PS2Memory *memory = nullptr;
        uint64_t generation = ~0ull;
        uint64_t imageHash = 0;
        std::vector<uint8_t> previous;
        uint64_t records = 0;
        uint64_t skipped = 0;
    };

    Vu1Trace *vu1Trace()
    {
        static Vu1Trace *trace = []() -> Vu1Trace *
        {
            const char *value = std::getenv("PS2X_VU1_TRACE");
            if (!value || !*value)
                return nullptr;
            std::string spec(value);
            const size_t second = spec.rfind(':');
            const size_t first = second == std::string::npos || second == 0u ? std::string::npos : spec.rfind(':', second - 1u);
            if (first == std::string::npos || first < 2u)
                return nullptr;
            auto *result = new Vu1Trace();
            const std::filesystem::path path = spec.substr(0, first);
            result->first = std::stoull(spec.substr(first + 1u, second - first - 1u));
            result->last = result->first + std::stoull(spec.substr(second + 1u));
            result->directory = path.parent_path();
            result->file = std::fopen(path.string().c_str(), "wb");
            if (!result->file)
            {
                delete result;
                return nullptr;
            }
            return result;
        }();
        return trace;
    }

    // F8 in the game window (ps2_runtime.cpp) asks for a trace of the next few fields without PS2X_VU1_TRACE.
    std::atomic<uint32_t> g_vu1TraceRequestFields{0u};
    Vu1Trace *g_vu1HotkeyTrace = nullptr;

    void traceVu1Run(VU1Interpreter &vu, const uint8_t *vuCode, uint32_t codeSize, const uint8_t *vuData, uint32_t dataSize,
                     PS2Memory *memory, uint32_t maxCycles)
    {
        if (const uint32_t fields = g_vu1TraceRequestFields.exchange(0u); fields != 0u && memory)
        {
            const uint64_t now = memory->gs().vsyncTick.load();
            char name[64];
            std::snprintf(name, sizeof(name), "vu1trace_%llu.bin", static_cast<unsigned long long>(now));
            auto *hotkey = new Vu1Trace();
            hotkey->directory = std::filesystem::current_path();
            hotkey->first = now;
            hotkey->last = now + fields;
            hotkey->file = std::fopen((hotkey->directory / name).string().c_str(), "wb");
            if (hotkey->file)
            {
                g_vu1HotkeyTrace = hotkey;
                std::fprintf(stderr, "[vu1-trace] recording %u field(s) from tick %llu to %s\n", fields,
                             static_cast<unsigned long long>(now), name);
            }
            else
                delete hotkey;
        }
        Vu1Trace *trace = g_vu1HotkeyTrace ? g_vu1HotkeyTrace : vu1Trace();
        if (!trace || !memory || dataSize != 0x4000u || codeSize != 0x4000u)
            return;
        const uint64_t tick = memory->gs().vsyncTick.load();
        if (tick < trace->first || tick >= trace->last)
        {
            if (tick >= trace->last && trace->file)
            {
                std::fclose(trace->file);
                trace->file = nullptr;
                std::fprintf(stderr, "[vu1-trace] %llu records, %llu skipped\n", static_cast<unsigned long long>(trace->records),
                             static_cast<unsigned long long>(trace->skipped));
                std::fflush(stderr);
            }
            return;
        }
        if (!trace->file)
            return;
        VU1InterpreterAccess::TraceState state;
        if (!VU1InterpreterAccess::traceCapture(vu, state))
        {
            ++trace->skipped;
            return;
        }
        const uint64_t generation = memory->getVU1CodeGeneration();
        if (trace->memory != memory || trace->generation != generation)
        {
            trace->memory = memory;
            trace->generation = generation;
            trace->imageHash = fnv1a64(vuCode, codeSize);
            char name[64];
            std::snprintf(name, sizeof(name), "vu1_%016llx.bin", static_cast<unsigned long long>(trace->imageHash));
            const std::filesystem::path path = trace->directory / name;
            std::error_code error;
            if (!std::filesystem::exists(path, error))
                if (FILE *file = std::fopen(path.string().c_str(), "wb"))
                {
                    std::fwrite(vuCode, 1, codeSize, file);
                    std::fclose(file);
                }
        }
        if (trace->previous.size() != dataSize)
            trace->previous.assign(dataSize, 0u);
        const uint32_t magic = 0x52543156u;
        std::fwrite(&magic, 4, 1, trace->file);
        std::fwrite(&maxCycles, 4, 1, trace->file);
        std::fwrite(&trace->imageHash, 8, 1, trace->file);
        std::fwrite(&tick, 8, 1, trace->file);
        std::fwrite(&state, sizeof(state), 1, trace->file);
        const uint32_t qwords = dataSize / 16u;
        for (uint32_t index = 0; index < qwords;)
        {
            if (std::memcmp(vuData + index * 16u, trace->previous.data() + index * 16u, 16u) == 0)
            {
                ++index;
                continue;
            }
            uint32_t end = index + 1u;
            while (end < qwords && end - index < 0xFFFFu &&
                   std::memcmp(vuData + end * 16u, trace->previous.data() + end * 16u, 16u) != 0)
                ++end;
            const uint16_t header[2] = {static_cast<uint16_t>(index), static_cast<uint16_t>(end - index)};
            std::fwrite(header, sizeof(header), 1, trace->file);
            std::fwrite(vuData + index * 16u, 16u, end - index, trace->file);
            index = end;
        }
        const uint16_t terminator[2] = {0xFFFFu, 0u};
        std::fwrite(terminator, sizeof(terminator), 1, trace->file);
        std::memcpy(trace->previous.data(), vuData, dataSize);
        ++trace->records;
    }

    // Diagnostic (God Hand round 9, missing clipped stage geometry), inert unless PS2X_VU1_FRAMESTATS=<csv path>:
    // one CSV line per field with VU1 work: calls, XGKICK GIF tags with PRE and PRIM fan (5) / strip (4), and per
    // (image hash, start pc) calls/fans/strips. The VU1 calls of the last fields are kept in memory; a field with
    // >= 500 strip tags and no fan tag is written with the fields before it as a PS2X_VU1_TRACE-format file
    // vu1trig_<tick>.bin (+ vu1_<hash>.bin images) next to the CSV, at most 3 times (PS2X_VU1_FRAMESTATS_DUMP=0: no
    // call ring, counts only).
    struct Vu1FrameStats
    {
        struct Call
        {
            uint64_t tick = 0;
            uint64_t hash = 0;
            uint32_t maxCycles = 0;
            VU1InterpreterAccess::TraceState state{};
            std::vector<uint8_t> data;
        };
        FILE *csv = nullptr;
        std::filesystem::path directory;
        const PS2Memory *memory = nullptr;
        uint64_t generation = ~0ull;
        uint64_t imageHash = 0;
        uint64_t tick = ~0ull;
        uint64_t calls = 0, fans = 0, strips = 0;
        std::map<std::pair<uint64_t, uint32_t>, std::array<uint64_t, 3>> perEntry;
        std::deque<Call> ring;
        std::unordered_map<uint64_t, std::vector<uint8_t>> images;
        uint32_t dumps = 0;
    };

    Vu1FrameStats *vu1FrameStats()
    {
        static Vu1FrameStats *stats = []() -> Vu1FrameStats *
        {
            const char *value = std::getenv("PS2X_VU1_FRAMESTATS");
            if (!value || !*value)
                return nullptr;
            auto *result = new Vu1FrameStats();
            result->directory = std::filesystem::path(value).parent_path();
            result->csv = std::fopen(value, "w");
            if (!result->csv)
            {
                delete result;
                return nullptr;
            }
            std::fprintf(result->csv, "tick,calls,fans,strips,entries\n");
            return result;
        }();
        return stats;
    }

    void countGifTags(const std::vector<std::vector<uint8_t>> &packets, uint64_t &fans, uint64_t &strips)
    {
        for (const auto &packet : packets)
        {
            size_t pos = 0;
            while (pos + 16u <= packet.size())
            {
                uint64_t lo;
                std::memcpy(&lo, packet.data() + pos, 8u);
                const uint32_t nloop = static_cast<uint32_t>(lo & 0x7FFFu);
                const uint32_t flg = static_cast<uint32_t>((lo >> 58) & 3u);
                uint32_t nreg = static_cast<uint32_t>((lo >> 60) & 0xFu);
                if (nreg == 0u)
                    nreg = 16u;
                if (((lo >> 46) & 1u) != 0u && nloop != 0u)
                {
                    const uint32_t prim = static_cast<uint32_t>((lo >> 47) & 7u);
                    fans += prim == 5u ? 1u : 0u;
                    strips += prim == 4u ? 1u : 0u;
                }
                pos += 16u;
                pos += flg == 0u ? size_t(nloop) * nreg * 16u
                       : flg == 1u ? ((size_t(nloop) * nreg + 1u) & ~size_t(1)) * 8u
                                   : size_t(nloop) * 16u;
                if (((lo >> 15) & 1u) != 0u)
                    break;
            }
        }
    }

    void dumpVu1FrameStatsRing(Vu1FrameStats &stats, uint64_t badTick)
    {
        char name[64];
        std::snprintf(name, sizeof(name), "vu1trig_%llu.bin", static_cast<unsigned long long>(badTick));
        const std::filesystem::path tracePath = stats.directory / name;
        FILE *file = std::fopen(tracePath.string().c_str(), "wb");
        if (!file)
            return;
        std::vector<uint8_t> previous(0x4000u, 0u);
        for (const Vu1FrameStats::Call &call : stats.ring)
        {
            const uint32_t magic = 0x52543156u;
            std::fwrite(&magic, 4, 1, file);
            std::fwrite(&call.maxCycles, 4, 1, file);
            std::fwrite(&call.hash, 8, 1, file);
            std::fwrite(&call.tick, 8, 1, file);
            std::fwrite(&call.state, sizeof(call.state), 1, file);
            for (uint32_t index = 0; index < 0x400u;)
            {
                if (std::memcmp(call.data.data() + index * 16u, previous.data() + index * 16u, 16u) == 0)
                {
                    ++index;
                    continue;
                }
                uint32_t end = index + 1u;
                while (end < 0x400u && std::memcmp(call.data.data() + end * 16u, previous.data() + end * 16u, 16u) != 0)
                    ++end;
                const uint16_t header[2] = {static_cast<uint16_t>(index), static_cast<uint16_t>(end - index)};
                std::fwrite(header, sizeof(header), 1, file);
                std::fwrite(call.data.data() + index * 16u, 16u, end - index, file);
                index = end;
            }
            const uint16_t terminator[2] = {0xFFFFu, 0u};
            std::fwrite(terminator, sizeof(terminator), 1, file);
            previous = call.data;
        }
        std::fclose(file);
        for (const auto &[hash, image] : stats.images)
        {
            std::snprintf(name, sizeof(name), "vu1_%016llx.bin", static_cast<unsigned long long>(hash));
            if (FILE *out = std::fopen((stats.directory / name).string().c_str(), "wb"))
            {
                std::fwrite(image.data(), 1, image.size(), out);
                std::fclose(out);
            }
        }
        std::fprintf(stderr, "[vu1-framestats] field %llu: no fan tags; %zu calls written to %s\n",
                     static_cast<unsigned long long>(badTick), stats.ring.size(), tracePath.string().c_str());
    }

    void vu1FrameStatsFinishField(Vu1FrameStats &stats)
    {
        if (stats.tick == ~0ull || stats.calls == 0u)
            return;
        std::fprintf(stats.csv, "%llu,%llu,%llu,%llu,", static_cast<unsigned long long>(stats.tick),
                     static_cast<unsigned long long>(stats.calls), static_cast<unsigned long long>(stats.fans),
                     static_cast<unsigned long long>(stats.strips));
        for (const auto &[key, value] : stats.perEntry)
            std::fprintf(stats.csv, "%04llx:%x=%llu/%llu/%llu;", static_cast<unsigned long long>(key.first >> 48), key.second,
                         static_cast<unsigned long long>(value[0]), static_cast<unsigned long long>(value[1]),
                         static_cast<unsigned long long>(value[2]));
        std::fprintf(stats.csv, "\n");
        std::fflush(stats.csv);
        if (stats.fans == 0u && stats.strips >= 500u && stats.dumps < 3u)
        {
            ++stats.dumps;
            dumpVu1FrameStatsRing(stats, stats.tick);
        }
        // Keep the calls of the last 3 fields with VU1 work (the next field's calls follow).
        std::vector<uint64_t> ticks;
        for (const auto &call : stats.ring)
            if (ticks.empty() || ticks.back() != call.tick)
                ticks.push_back(call.tick);
        while (ticks.size() > 3u && !stats.ring.empty() && stats.ring.front().tick == ticks.front())
        {
            stats.ring.pop_front();
            if (stats.ring.empty() || stats.ring.front().tick != ticks.front())
                ticks.erase(ticks.begin());
        }
    }

    // Before a VU1 run: field bookkeeping + the call record. Returns the stats (non-null = count this run's packets).
    Vu1FrameStats *vu1FrameStatsBegin(VU1Interpreter &vu, const uint8_t *vuCode, uint32_t codeSize, const uint8_t *vuData,
                                      uint32_t dataSize, PS2Memory *memory, uint32_t maxCycles)
    {
        Vu1FrameStats *stats = vu1FrameStats();
        if (!stats || !memory || dataSize != 0x4000u || codeSize != 0x4000u)
            return nullptr;
        const uint64_t tick = memory->gs().vsyncTick.load();
        if (tick != stats->tick)
        {
            vu1FrameStatsFinishField(*stats);
            stats->tick = tick;
            stats->calls = stats->fans = stats->strips = 0u;
            stats->perEntry.clear();
        }
        const uint64_t generation = memory->getVU1CodeGeneration();
        if (stats->memory != memory || stats->generation != generation)
        {
            stats->memory = memory;
            stats->generation = generation;
            stats->imageHash = fnv1a64(vuCode, codeSize);
            if (stats->images.find(stats->imageHash) == stats->images.end())
                stats->images.emplace(stats->imageHash, std::vector<uint8_t>(vuCode, vuCode + codeSize));
        }
        static const bool keepRing = []
        {
            const char *value = std::getenv("PS2X_VU1_FRAMESTATS_DUMP");
            return !(value && std::strcmp(value, "0") == 0);
        }();
        Vu1FrameStats::Call call;
        if (keepRing && stats->dumps < 3u && VU1InterpreterAccess::traceCapture(vu, call.state))
        {
            call.tick = tick;
            call.hash = stats->imageHash;
            call.maxCycles = maxCycles;
            call.data.assign(vuData, vuData + dataSize);
            stats->ring.push_back(std::move(call));
        }
        return stats;
    }

    bool vu1VerifyEnabled()
    {
        static const bool enabled = []
        {
            const char *value = std::getenv("PS2X_VU1_VERIFY");
            return value && *value && std::strcmp(value, "0") != 0;
        }();
        return enabled;
    }

    Vu1CompiledRegistry &vu1CompiledRegistry()
    {
        static Vu1CompiledRegistry registry = []
        {
            Vu1CompiledRegistry result;
            const char *value = std::getenv("PS2X_VU1_RECOMP");
            result.enabled = !(value && std::strcmp(value, "0") == 0);
            return result;
        }();
        return registry;
    }

    VU1InterpreterAccess::CompiledProgram lookupCompiledVu1Program(const uint8_t *vuCode, uint32_t codeSize, const PS2Memory *memory)
    {
        Vu1CompiledRegistry &registry = vu1CompiledRegistry();
        if (!registry.enabled || (registry.programs.empty() && registry.units.empty()) || !memory)
            return nullptr;
        const uint64_t generation = memory->getVU1CodeGeneration();
        if (registry.memory != memory || registry.code != vuCode || registry.generation != generation)
        {
            registry.memory = memory;
            registry.code = vuCode;
            registry.generation = generation;
            registry.current = findCompiledVu1Image(registry, vuCode, codeSize);
        }
        t_vu1AssembledTable = registry.currentTable;
        return registry.current;
    }

    // VU0 micro mode, ahead-of-time compiled like VU1. The runtime backs the 4 KB VU0 code/data memories with 16 KB
    // (zero above 4 KB) and runs VU0 microprograms with the 16 KB sizes, so a VU0 image is captured, recompiled
    // (vu1_recomp, unchanged) and registered exactly like a VU1 image; it is found by its hash.
    // PS2X_VU0_CAPTURE=<dir> writes the images (vu1_<hash>.bin), PS2X_VU0_RECOMP=0 disables the compiled code and
    // PS2X_VU0_VERIFY=1 runs every call twice (compiled and interpreted) and reports differences.
    bool vu0VerifyEnabled()
    {
        static const bool enabled = []
        {
            const char *value = std::getenv("PS2X_VU0_VERIFY");
            return value && *value && std::strcmp(value, "0") != 0;
        }();
        return enabled;
    }

    VU1InterpreterAccess::CompiledProgram lookupCompiledVu0Program(const uint8_t *vuCode, uint32_t codeSize, const PS2Memory *memory)
    {
        static const bool enabled = []
        {
            const char *value = std::getenv("PS2X_VU0_RECOMP");
            return !(value && std::strcmp(value, "0") == 0);
        }();
        struct Cache
        {
            const PS2Memory *memory = nullptr;
            const uint8_t *code = nullptr;
            uint64_t generation = ~0ull;
            VU1InterpreterAccess::CompiledProgram program = nullptr;
        };
        static thread_local Cache cache;
        const auto &programs = vu1CompiledRegistry().programs;
        if (!enabled || programs.empty() || !memory || codeSize != VU1InterpreterAccess::kFastCodeSize)
            return nullptr;
        const uint64_t generation = memory->getVU0CodeGeneration();
        if (cache.memory != memory || cache.code != vuCode || cache.generation != generation)
        {
            cache.memory = memory;
            cache.code = vuCode;
            cache.generation = generation;
            // vu1_recomp emits a VU0 image twice: fast FMACs under its hash, exact FMACs under hash ^ "VU0EXACT".
            // PS2X_VU0_FAST=0 runs the exact one (older generated code only has the exact one, under the plain hash).
            static const bool exact = []
            {
                const char *value = std::getenv("PS2X_VU0_FAST");
                return value && std::strcmp(value, "0") == 0;
            }();
            constexpr uint64_t kVu0ExactTag = 0x5655304558414354ull; // same constant in ps2xTest/tools/vu1_recomp.cpp
            const uint64_t hash = fnv1a64(vuCode, codeSize);
            auto it = exact ? programs.find(hash ^ kVu0ExactTag) : programs.end();
            if (it == programs.end())
                it = programs.find(hash);
            cache.program = it == programs.end() ? nullptr : it->second;
        }
        return cache.program;
    }

    void captureVu0Program(const uint8_t *vuCode, uint32_t codeSize, const PS2Memory *memory)
    {
        static const std::filesystem::path directory = []
        {
            const char *value = std::getenv("PS2X_VU0_CAPTURE");
            std::filesystem::path path = value && *value ? value : "";
            if (!path.empty())
            {
                std::error_code error;
                std::filesystem::create_directories(path, error);
            }
            return path;
        }();
        static thread_local uint64_t lastGeneration = ~0ull;
        if (directory.empty() || !memory || codeSize != VU1InterpreterAccess::kFastCodeSize)
            return;
        const uint64_t generation = memory->getVU0CodeGeneration();
        if (generation == lastGeneration)
            return;
        lastGeneration = generation;
        char name[64];
        std::snprintf(name, sizeof(name), "vu1_%016llx.bin", static_cast<unsigned long long>(fnv1a64(vuCode, codeSize)));
        const std::filesystem::path path = directory / name;
        std::error_code error;
        if (!std::filesystem::exists(path, error))
        {
            if (FILE *file = std::fopen(path.string().c_str(), "wb"))
            {
                std::fwrite(vuCode, 1, codeSize, file);
                std::fclose(file);
            }
        }
    }

    // Microcode uploads (VIF1 MPG) while PS2X_VU1_CAPTURE is set: consecutive MPGs that continue each other are merged
    // into one upload, which is written as vup_<pair address>_cap_<hash>.bin (the format of tools/vu/extract_uploads.py)
    // when the next one starts somewhere else or VU1 runs. vu1_recomp compiles each upload once, whatever else is in
    // micro memory next to it; uploads the game places itself (not where the disc's MPG says) are found this way.
    struct Vu1UploadCapture
    {
        std::vector<uint8_t> bytes;
        uint32_t address = 0; // bytes
        std::unordered_map<uint64_t, bool> written;
        std::mutex mutex; // the VIF1 upload and the VU1 run can be on different threads (MTVU)
    };
    Vu1UploadCapture g_vu1Uploads;

    // Caller holds g_vu1Uploads.mutex.
    void flushVu1UploadCaptureLocked(const Vu1ProgramCapture &capture)
    {
        Vu1UploadCapture &up = g_vu1Uploads;
        if (up.bytes.empty())
            return;
        const uint64_t hash = fnv1a64(up.bytes.data(), up.bytes.size()) ^ (static_cast<uint64_t>(up.address) << 48);
        if (up.written.size() < 65536u && up.written.emplace(hash, true).second)
        {
            char name[80];
            std::snprintf(name, sizeof(name), "vup_%03x_cap_%016llx.bin", up.address / 8u, static_cast<unsigned long long>(hash));
            const std::filesystem::path path = capture.directory / name;
            std::error_code error;
            if (!std::filesystem::exists(path, error))
                if (FILE *file = std::fopen(path.string().c_str(), "wb"))
                {
                    std::fwrite(up.bytes.data(), 1, up.bytes.size(), file);
                    std::fclose(file);
                }
        }
        up.bytes.clear();
    }

    void captureVu1Program(const uint8_t *vuCode, uint32_t codeSize, const PS2Memory *memory, uint32_t pc)
    {
        Vu1ProgramCapture *capture = vu1ProgramCapture();
        if (!capture || !memory)
            return;
        {
            std::lock_guard<std::mutex> lock(g_vu1Uploads.mutex);
            flushVu1UploadCaptureLocked(*capture);
        }
        const uint64_t generation = memory->getVU1CodeGeneration();
        if (capture->memory != memory || capture->generation != generation)
        {
            capture->memory = memory;
            capture->generation = generation;
            capture->imageHash = fnv1a64(vuCode, codeSize);
            char name[64];
            std::snprintf(name, sizeof(name), "vu1_%016llx.bin", static_cast<unsigned long long>(capture->imageHash));
            const std::filesystem::path path = capture->directory / name;
            std::error_code error;
            // The file system is asked once per image (a set of the images seen): asking on every upload took ~10%
            // of the EE thread in gameplay.
            static std::unordered_set<uint64_t> seen;
            if (seen.insert(capture->imageHash).second && !std::filesystem::exists(path, error))
            {
                if (FILE *file = std::fopen(path.string().c_str(), "wb"))
                {
                    std::fwrite(vuCode, 1, codeSize, file);
                    std::fclose(file);
                }
            }
        }
        const uint64_t key = (capture->imageHash & ~0x3FFFull) ^ pc;
        if (capture->entryCalls[key]++ == 0u)
        {
            if (FILE *file = std::fopen((capture->directory / "entries.txt").string().c_str(), "a"))
            {
                std::fprintf(file, "%016llx %04x\n", static_cast<unsigned long long>(capture->imageHash), pc);
                std::fclose(file);
            }
        }
        if (++capture->calls % 100000u == 0u)
        {
            if (FILE *file = std::fopen((capture->directory / "calls.txt").string().c_str(), "w"))
            {
                std::fprintf(file, "calls %llu entries %zu\n", static_cast<unsigned long long>(capture->calls), capture->entryCalls.size());
                std::fclose(file);
            }
        }
    }
}

void VU1Interpreter::addVfRead(InstructionUsage &usage, uint8_t reg, uint8_t lanes)
{
    if (lanes == 0u)
        return;
    for (uint32_t index = 0; index < usage.vfReadCount; ++index)
    {
        if (usage.vfRead[index].reg == reg)
        {
            usage.vfRead[index].lanes |= lanes;
            return;
        }
    }
    if (usage.vfReadCount < usage.vfRead.size())
        usage.vfRead[usage.vfReadCount++] = {reg, lanes};
}

void VU1Interpreter::addVfWrite(InstructionUsage &usage, uint8_t reg, uint8_t lanes)
{
    if (reg == 0u || lanes == 0u)
        return;
    if (usage.vfWrite.reg == 0u)
        usage.vfWrite = {reg, lanes};
    else if (usage.vfWrite.reg == reg)
        usage.vfWrite.lanes |= lanes;
}

uint8_t VU1Interpreter::vfReadLanes(const InstructionUsage &usage, uint8_t reg)
{
    for (uint32_t index = 0; index < usage.vfReadCount; ++index)
    {
        if (usage.vfRead[index].reg == reg)
            return usage.vfRead[index].lanes;
    }
    return 0u;
}

VU1Interpreter::VU1Interpreter(Unit unit)
    : m_unit(unit)
{
    reset();
}

void VU1Interpreter::resetScheduler()
{
    std::memset(m_flagPipeline.data(), 0, sizeof(m_flagPipeline));
    std::memset(&m_fdiv, 0, sizeof(m_fdiv));
    std::memset(m_efu.data(), 0, sizeof(m_efu));
    std::memset(m_storePipeline.data(), 0, sizeof(m_storePipeline));
    std::memset(m_vfWritePipeline.data(), 0, sizeof(m_vfWritePipeline));
    std::memset(m_viWritePipeline.data(), 0, sizeof(m_viWritePipeline));
    std::memset(m_accWritePipeline.data(), 0, sizeof(m_accWritePipeline));
    m_flagValid = 0u;
    m_storeValid = 0u;
    m_vfWriteValid = 0u;
    m_viWriteValid = 0u;
    m_accWriteValid = 0u;
    m_nextCommitCycle = std::numeric_limits<uint64_t>::max();
    resetXgkick();
    std::memset(m_vfReady.data(), 0, sizeof(m_vfReady));
    std::memset(m_viReady.data(), 0, sizeof(m_viReady));
    std::memset(m_accReady.data(), 0, sizeof(m_accReady));
    std::memset(m_vfLatestWrite.data(), 0, sizeof(m_vfLatestWrite));
    std::memset(m_viLatestWrite.data(), 0, sizeof(m_viLatestWrite));
    std::memset(m_accLatestWrite.data(), 0, sizeof(m_accLatestWrite));
    m_nextWriteSequence = 0;
    m_efuResourceReady = 0;
    m_workingClip = m_state.clip;
    m_viBranchBackupValue = 0;
    m_viBranchBackupReg = 0;
    m_viBranchBackupValid = false;
    m_stopRequested = false;
    m_pendingHaltD = false;
    m_pendingHaltT = false;
}

void VU1Interpreter::reset()
{
    std::memset(&m_state, 0, sizeof(m_state));
    m_state.vf[0][3] = 1.0f;
    m_state.q = 1.0f;
    m_state.r = 0x3F800000u;
    m_cycle = 0;
    resetScheduler();
    if (m_unit == Unit::VU1)
        vu1NormShadow.valid = false;
    else
        vu0NormShadow.valid = false;
}

// See VU1InterpreterAccess::resetStateOnly, which also invalidates this unit's normalized-register shadow like reset().
void ps2xVuResetStateForExecute(VU1Interpreter &vu)
{
    VU1InterpreterAccess::resetStateOnly(vu);
}

// PS2X_VU0_LEAN (ps2_runtime.cpp, executeVU0Microprogram): see VU1InterpreterAccess::loadVu0Registers/executeVu0Lean.
void ps2xVu0LoadRegisters(VU1Interpreter &vu, const __m128 *vf, __m128 acc)
{
    VU1InterpreterAccess::loadVu0Registers(vu, vf, acc);
}

void ps2xVu0ExecuteLean(VU1Interpreter &vu, uint8_t *vuCode, uint32_t codeSize, uint8_t *vuData, uint32_t dataSize, GS &gs,
                        PS2Memory *memory, uint32_t startPC, uint32_t itop, uint32_t maxCycles)
{
    VU1InterpreterAccess::executeVu0Lean(vu, vuCode, codeSize, vuData, dataSize, gs, memory, startPC, itop, maxCycles);
}

bool VU1Interpreter::pipelinesPending() const
{
    if (m_fdiv.valid || m_xgkick.active)
        return true;
    for (const ScalarPipelineEntry &entry : m_efu)
        if (entry.valid)
            return true;
    return (m_flagValid | m_storeValid | m_vfWriteValid | m_viWriteValid | m_accWriteValid) != 0u;
}

void VU1Interpreter::flushPipelines()
{
    while (pipelinesPending())
    {
        if (!m_xgkick.active && m_nextCommitCycle != std::numeric_limits<uint64_t>::max() &&
            m_nextCommitCycle > m_cycle + 1u)
        {
            m_cycle = m_nextCommitCycle - 1u;
        }
        advanceOneCycle();
    }
}

VU1Interpreter::InstructionUsage VU1Interpreter::decodeUpperUsage(uint32_t upper) const
{
    InstructionUsage usage;
    usage.pipeline = PipelineFmac;
    usage.latency = kFmacLatency;

    const uint8_t op = static_cast<uint8_t>(upper & 0x3Fu);
    const uint8_t dest = DEST(upper);
    const uint8_t fs = FS(upper);
    const uint8_t ft = FT(upper);
    const uint8_t fd = FD(upper);

    if (op <= 0x2Fu)
    {
        addVfRead(usage, fs, dest);
        addVfWrite(usage, fd, dest);
        if (op <= 0x1Bu)
            addVfRead(usage, ft, laneForComponent(op & 3u));
        else if (op >= 0x28u)
            addVfRead(usage, ft, op == 0x2Eu ? 0xEu : dest);
        if (op == 0x08u || op == 0x09u || op == 0x0Au || op == 0x0Bu ||
            op == 0x0Cu || op == 0x0Du || op == 0x0Eu || op == 0x0Fu ||
            op == 0x21u || op == 0x23u || op == 0x25u || op == 0x27u ||
            op == 0x29u || op == 0x2Du || op == 0x2Eu)
        {
            usage.accRead = dest;
        }
        return usage;
    }

    if (op >= 0x3Cu)
    {
        const uint8_t special = static_cast<uint8_t>((upper & 3u) | ((upper >> 4) & 0x7Cu));
        const bool writesAcc =
            special <= 0x0Fu ||
            (special >= 0x18u && special <= 0x1Cu) ||
            special == 0x1Eu ||
            (special >= 0x20u && special <= 0x2Au) ||
            (special >= 0x2Cu && special <= 0x2Eu);
        if (writesAcc)
        {
            addVfRead(usage, fs, dest);
            if (special <= 0x1Bu)
                addVfRead(usage, ft, laneForComponent(special & 3u));
            else if ((special >= 0x28u && special <= 0x2Eu))
                addVfRead(usage, ft, special == 0x2Eu ? 0xEu : dest);
            usage.accWrite = dest;
            if ((special >= 0x08u && special <= 0x0Fu) ||
                special == 0x21u || special == 0x23u || special == 0x25u ||
                special == 0x27u || special == 0x29u || special == 0x2Du)
            {
                usage.accRead = dest;
            }
        }
        else if (special >= 0x10u && special <= 0x17u)
        {
            addVfRead(usage, fs, dest);
            addVfWrite(usage, ft, dest);
        }
        else if (special == 0x1Du)
        {
            addVfRead(usage, fs, dest);
            addVfWrite(usage, ft, dest);
        }
        else if (special == 0x1Fu)
        {
            addVfRead(usage, fs, 0xEu);
            addVfRead(usage, ft, 0x1u);
            usage.writesClip = true;
        }
        else if (special != 0x2Fu && special != 0x30u)
        {
            usage.reserved = true;
        }
        return usage;
    }

    usage.reserved = true;
    return usage;
}

VU1Interpreter::InstructionUsage VU1Interpreter::decodeLowerUsage(uint32_t lower) const
{
    InstructionUsage usage;
    if (lower == 0u || lower == 0x8000033Cu)
        return usage;

    const uint8_t opHi = static_cast<uint8_t>((lower >> 25) & 0x7Fu);
    const uint8_t vfT = FT(lower);
    const uint8_t vfS = FS(lower);
    const uint8_t viT = VIT(lower);
    const uint8_t viS = VIS(lower);
    const uint8_t viD = VID(lower);
    const uint8_t dest = DEST(lower);
    auto readVi = [&](uint8_t reg)
    {
        if (reg != 0u)
            usage.viRead |= static_cast<uint16_t>(1u << reg);
    };
    auto writeVi = [&](uint8_t reg)
    {
        if (reg != 0u)
            usage.viWrite |= static_cast<uint16_t>(1u << reg);
    };

    switch (opHi)
    {
    case 0x00:
        usage.pipeline = PipelineLsu;
        usage.latency = 4u;
        readVi(viS);
        addVfWrite(usage, vfT, dest);
        return usage;
    case 0x01:
        usage.pipeline = PipelineLsu;
        usage.latency = 1u;
        readVi(viT);
        addVfRead(usage, vfS, dest);
        return usage;
    case 0x04:
        usage.pipeline = PipelineLsu;
        usage.latency = 4u;
        readVi(viS);
        writeVi(viT);
        return usage;
    case 0x05:
        usage.pipeline = PipelineLsu;
        usage.latency = 1u;
        readVi(viS);
        readVi(viT);
        return usage;
    case 0x08:
    case 0x09:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.delaysNextBranchRead = true;
        readVi(viS);
        writeVi(viT);
        return usage;
    case 0x10:
    case 0x12:
    case 0x13:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.readsClip = true;
        writeVi(1u);
        return usage;
    case 0x11:
        usage.pipeline = PipelineFmac;
        usage.latency = kFmacLatency;
        usage.writesClip = true;
        return usage;
    case 0x14:
    case 0x16:
    case 0x17:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        writeVi(viT);
        return usage;
    case 0x15:
        usage.pipeline = PipelineFmac;
        usage.latency = kFmacLatency;
        return usage;
    case 0x18:
    case 0x1A:
    case 0x1B:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        readVi(viS);
        writeVi(viT);
        return usage;
    case 0x1C:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.readsClip = true;
        writeVi(viT);
        return usage;
    case 0x20:
        usage.pipeline = PipelineBranch;
        return usage;
    case 0x21:
        usage.pipeline = PipelineBranch;
        usage.latency = 1u;
        writeVi(viT);
        return usage;
    case 0x24:
        usage.pipeline = PipelineBranch;
        readVi(viS);
        return usage;
    case 0x25:
        usage.pipeline = PipelineBranch;
        usage.latency = 1u;
        readVi(viS);
        writeVi(viT);
        return usage;
    case 0x28:
    case 0x29:
        usage.pipeline = PipelineBranch;
        readVi(viS);
        readVi(viT);
        return usage;
    case 0x2C:
    case 0x2D:
    case 0x2E:
    case 0x2F:
        usage.pipeline = PipelineBranch;
        readVi(viS);
        return usage;
    case 0x40:
        break;
    default:
        usage.reserved = true;
        return usage;
    }

    const uint8_t direct = static_cast<uint8_t>(lower & 0x3Fu);
    if (direct == 0x30u || direct == 0x31u || direct == 0x34u || direct == 0x35u)
    {
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.delaysNextBranchRead = true;
        readVi(viS);
        readVi(viT);
        writeVi(viD);
        return usage;
    }
    if (direct == 0x32u)
    {
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.delaysNextBranchRead = true;
        readVi(viS);
        writeVi(viT);
        return usage;
    }
    if (direct < 0x3Cu)
    {
        usage.reserved = true;
        return usage;
    }

    const uint8_t special = static_cast<uint8_t>((lower & 3u) | ((lower >> 4) & 0x7Cu));
    switch (special)
    {
    case 0x30:
    case 0x31:
        usage.pipeline = PipelineFmac;
        usage.latency = 4u;
        addVfRead(usage, vfS, special == 0x31u ? 0xFu : dest);
        addVfWrite(usage, vfT, dest);
        break;
    case 0x34:
    case 0x36:
        usage.pipeline = PipelineLsu;
        usage.latency = 4u;
        usage.viLatency = 1u;
        usage.delaysNextBranchRead = true;
        readVi(viS);
        writeVi(viS);
        addVfWrite(usage, vfT, dest);
        break;
    case 0x35:
    case 0x37:
        usage.pipeline = PipelineLsu;
        usage.latency = 1u;
        usage.delaysNextBranchRead = true;
        readVi(viT);
        writeVi(viT);
        addVfRead(usage, vfS, dest);
        break;
    case 0x38:
        usage.pipeline = PipelineFdiv;
        usage.latency = 7u;
        addVfRead(usage, vfS, laneForComponent((lower >> 21) & 3u));
        addVfRead(usage, vfT, laneForComponent((lower >> 23) & 3u));
        break;
    case 0x39:
        usage.pipeline = PipelineFdiv;
        usage.latency = 7u;
        addVfRead(usage, vfT, laneForComponent((lower >> 23) & 3u));
        break;
    case 0x3A:
        usage.pipeline = PipelineFdiv;
        usage.latency = 13u;
        addVfRead(usage, vfS, laneForComponent((lower >> 21) & 3u));
        addVfRead(usage, vfT, laneForComponent((lower >> 23) & 3u));
        break;
    case 0x3B:
        usage.pipeline = PipelineFdiv;
        usage.waitQ = true;
        break;
    case 0x3C:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.delaysNextBranchRead = true;
        addVfRead(usage, vfS, laneForComponent((lower >> 21) & 3u));
        writeVi(viT);
        break;
    case 0x3D:
        usage.pipeline = PipelineFmac;
        usage.latency = 4u;
        readVi(viS);
        addVfWrite(usage, vfT, dest);
        break;
    case 0x3E:
        usage.pipeline = PipelineLsu;
        usage.latency = 4u;
        readVi(viS);
        writeVi(viT);
        break;
    case 0x3F:
        usage.pipeline = PipelineLsu;
        usage.latency = 1u;
        readVi(viS);
        readVi(viT);
        break;
    case 0x40:
    case 0x41:
        usage.pipeline = PipelineFmac;
        usage.latency = 4u;
        addVfWrite(usage, vfT, dest);
        break;
    case 0x42:
    case 0x43:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        addVfRead(usage, vfS, laneForComponent((lower >> 21) & 3u));
        break;
    case 0x64:
        if (m_unit == Unit::VU0)
        {
            usage.reserved = true;
            break;
        }
        usage.pipeline = PipelineFmac;
        usage.latency = 4u;
        addVfWrite(usage, vfT, dest);
        break;
    case 0x68:
    case 0x69:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        writeVi(viT);
        break;
    case 0x6C:
        if (m_unit == Unit::VU0)
        {
            usage.reserved = true;
            break;
        }
        usage.pipeline = PipelineXgkick;
        usage.latency = 2u;
        readVi(viS);
        break;
    case 0x70:
    case 0x71:
    case 0x72:
    case 0x73:
    case 0x74:
    case 0x75:
    case 0x76:
    case 0x77:
    case 0x78:
    case 0x79:
    case 0x7A:
    case 0x7C:
    case 0x7D:
        if (m_unit == Unit::VU0)
        {
            usage.reserved = true;
            break;
        }
        usage.pipeline = PipelineEfu;
        switch (special)
        {
        case 0x70:
            usage.latency = 11u;
            break;
        case 0x71:
        case 0x72:
        case 0x77:
            usage.latency = 18u;
            break;
        case 0x73:
            usage.latency = 24u;
            break;
        case 0x74:
        case 0x75:
        case 0x7C:
            usage.latency = 54u;
            break;
        case 0x76:
        case 0x78:
        case 0x7A:
            usage.latency = 12u;
            break;
        case 0x79:
            usage.latency = 29u;
            break;
        case 0x7D:
            usage.latency = 44u;
            break;
        default:
            break;
        }
        if (special >= 0x70u && special <= 0x73u)
            addVfRead(usage, vfS, 0xEu);
        else if (special == 0x74u)
            addVfRead(usage, vfS, 0xCu);
        else if (special == 0x75u)
            addVfRead(usage, vfS, 0xAu);
        else if (special == 0x76u)
            addVfRead(usage, vfS, 0xFu);
        else
            addVfRead(usage, vfS, laneForComponent((lower >> 21) & 3u));
        break;
    case 0x7B:
        if (m_unit == Unit::VU0)
        {
            usage.reserved = true;
            break;
        }
        usage.pipeline = PipelineEfu;
        usage.waitP = true;
        break;
    default:
        usage.reserved = true;
        break;
    }
    return usage;
}

VU1Interpreter::DecodedInstructionPair VU1Interpreter::decodeInstructionPair(const uint8_t *vuCode, uint32_t pc) const
{
    DecodedInstructionPair decoded;
    std::memcpy(&decoded.lower, vuCode + pc, sizeof(decoded.lower));
    std::memcpy(&decoded.upper, vuCode + pc + sizeof(decoded.lower), sizeof(decoded.upper));
    decoded.iBit = (decoded.upper & 0x80000000u) != 0u;
    decoded.eBit = (decoded.upper & 0x40000000u) != 0u;
    decoded.mBit = (decoded.upper & 0x20000000u) != 0u;
    decoded.dBit = (decoded.upper & 0x10000000u) != 0u;
    decoded.tBit = (decoded.upper & 0x08000000u) != 0u;
    decoded.upperUsage = decodeUpperUsage(decoded.upper);
    if (!decoded.iBit)
        decoded.lowerUsage = decodeLowerUsage(decoded.lower);

    const uint8_t upperWriteReg = decoded.upperUsage.vfWrite.reg;
    if (upperWriteReg != 0u && (vfReadLanes(decoded.lowerUsage, upperWriteReg) != 0u || decoded.lowerUsage.vfWrite.reg == upperWriteReg))
    {
        decoded.upperVfShadowReg = upperWriteReg;
        if (decoded.lowerUsage.vfWrite.reg == upperWriteReg)
            decoded.suppressedLowerVf = upperWriteReg;
    }
    return decoded;
}

void VU1Interpreter::rebuildDecodedCodeCache(const uint8_t *vuCode, uint32_t codeSize,
                                             const PS2Memory *memory, uint64_t generation)
{
    const uint32_t pairCount = std::min<uint32_t>(codeSize / 8u, kMaxDecodedPairs);
    for (uint32_t i = 0; i < pairCount; ++i)
        m_decodedCodeCache[i] = decodeInstructionPair(vuCode, i * 8u);

    m_cachedVuCode = vuCode;
    m_cachedMemory = memory;
    m_cachedCodeSize = codeSize;
    m_cachedCodeGeneration = generation;
    m_decodedCodeCacheValid = true;
}

const VU1Interpreter::DecodedInstructionPair &VU1Interpreter::getDecodedInstructionPairForPc(
    const uint8_t *vuCode, uint32_t codeSize, PS2Memory *memory, uint32_t pc)
{
    if ((pc & 7u) != 0u)
    {
        m_fallbackDecoded = decodeInstructionPair(vuCode, pc);
        return m_fallbackDecoded;
    }

    const bool trackedVu1Code = memory != nullptr &&
                                ((m_unit == Unit::VU1 && vuCode == memory->getVU1Code()) ||
                                 (m_unit == Unit::VU0 && vuCode == memory->getVU0Code()));
    if (!trackedVu1Code)
    {
        m_fallbackDecoded = decodeInstructionPair(vuCode, pc);
        return m_fallbackDecoded;
    }

    const uint64_t generation = m_unit == Unit::VU1 ? memory->getVU1CodeGeneration() : memory->getVU0CodeGeneration();
    if (!m_decodedCodeCacheValid ||
        m_cachedVuCode != vuCode ||
        m_cachedMemory != memory ||
        m_cachedCodeSize != codeSize ||
        m_cachedCodeGeneration != generation)
    {
        rebuildDecodedCodeCache(vuCode, codeSize, memory, generation);
    }
    const uint32_t pairIndex = pc / 8u;
    if (pairIndex >= kMaxDecodedPairs)
    {
        m_fallbackDecoded = decodeInstructionPair(vuCode, pc);
        return m_fallbackDecoded;
    }
    return m_decodedCodeCache[pairIndex];
}

void VU1Interpreter::reportReservedInstruction(bool upper, uint32_t instruction)
{
    RUNTIME_ERROR(
        "[VU" << (m_unit == Unit::VU1 ? "1" : "0")
              << " reserved " << (upper ? "upper" : "lower")
              << "] cycle=" << m_cycle
              << " pc=0x" << std::hex << m_state.pc
              << " instruction=0x" << instruction
              << std::dec << '\n');
    m_stopRequested = true;
}

void VU1Interpreter::execute(uint8_t *vuCode, uint32_t codeSize,
                             uint8_t *vuData, uint32_t dataSize,
                             GS &gs, PS2Memory *memory,
                             uint32_t startPC, uint32_t top, uint32_t itop,
                             uint32_t maxCycles)
{
    resetScheduler();
    m_state.pc = startPC & microAddressMask();
    m_state.ebit = false;
    m_state.haltAfterDelaySlot = false;
    m_state.stoppedByD = false;
    m_state.stoppedByT = false;
    m_state.top = top;
    m_state.itop = itop;
    m_state.branchPending = false;
    m_state.branchTarget = 0;
    m_state.branchDelay = 0;
    m_state.vf[0][0] = 0.0f;
    m_state.vf[0][1] = 0.0f;
    m_state.vf[0][2] = 0.0f;
    m_state.vf[0][3] = 1.0f;
    run(vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles);
}

void VU1Interpreter::resume(uint8_t *vuCode, uint32_t codeSize,
                            uint8_t *vuData, uint32_t dataSize,
                            GS &gs, PS2Memory *memory,
                            uint32_t top, uint32_t itop, uint32_t maxCycles)
{
    m_state.top = top;
    m_state.itop = itop;
    m_state.stoppedByD = false;
    m_state.stoppedByT = false;
    run(vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles);
}

void VU1Interpreter::run(uint8_t *vuCode, uint32_t codeSize,
                         uint8_t *vuData, uint32_t dataSize,
                         GS &gs, PS2Memory *memory, uint32_t maxCycles)
{
    m_runStartPc = m_state.pc;
    if (m_unit == Unit::VU1)
    {
        captureVu1Program(vuCode, codeSize, memory, m_state.pc);
        traceVu1Run(*this, vuCode, codeSize, vuData, dataSize, memory, maxCycles);
        if (vu1VerifyEnabled() && lookupCompiledVu1Program(vuCode, codeSize, memory))
        {
            VU1InterpreterAccess::runVerified(*this, vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles);
            return;
        }
        if (Vu1FrameStats *stats = vu1FrameStatsBegin(*this, vuCode, codeSize, vuData, dataSize, memory, maxCycles))
        {
            const uint32_t startPc = m_state.pc;
            Vu1PacketSink sink; // forward = true: the packets still go to the GIF as without the stats
            packetSink = &sink;
            VU1InterpreterAccess::runLoop(*this, vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles, true);
            packetSink = nullptr;
            uint64_t fans = 0u, strips = 0u;
            countGifTags(sink.packets, fans, strips);
            ++stats->calls;
            stats->fans += fans;
            stats->strips += strips;
            auto &entry = stats->perEntry[{stats->imageHash, startPc}];
            ++entry[0];
            entry[1] += fans;
            entry[2] += strips;
            return;
        }
    }
    else
    {
        captureVu0Program(vuCode, codeSize, memory);
        if (vu0VerifyEnabled())
        {
            if (const auto compiled = lookupCompiledVu0Program(vuCode, codeSize, memory))
            {
                VU1InterpreterAccess::runVerifiedWith(*this, vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles, compiled, true);
                return;
            }
        }
    }
    VU1InterpreterAccess::runLoop(*this, vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles, true);
}

bool VU1InterpreterAccess::runLoop(VU1Interpreter &vu, uint8_t *vuCode, uint32_t codeSize, uint8_t *vuData, uint32_t dataSize,
                                   GS &gs, PS2Memory *memory, uint32_t maxCycles, bool allowCompiled)
{
    const CompiledProgram compiled =
        !allowCompiled ? nullptr
        : vu.m_unit == VU1Interpreter::Unit::VU1 ? lookupCompiledVu1Program(vuCode, codeSize, memory)
                                                  : lookupCompiledVu0Program(vuCode, codeSize, memory);
    return runLoopWith(vu, vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles, compiled);
}

bool VU1InterpreterAccess::runLoopWith(VU1Interpreter &vu, uint8_t *vuCode, uint32_t codeSize, uint8_t *vuData, uint32_t dataSize,
                                       GS &gs, PS2Memory *memory, uint32_t maxCycles, CompiledProgram compiled)
{
    vu.m_activeVuData = vuData;
    vu.m_activeVuDataSize = dataSize;
    vu.m_activeGs = &gs;
    vu.m_activeMemory = memory;

    const unsigned previousCsr = _mm_getcsr();
    const bool changeRounding = (previousCsr & 0x6000u) != 0x6000u;
    if (changeRounding)
        _mm_setcsr(previousCsr | 0x6000u);
    const uint64_t budgetEnd = vu.m_cycle + maxCycles;
    bool programEnded = false;
    const Vu1RunArgs compiledArgs{vuData, dataSize, &gs, memory, codeSize, budgetEnd};
    bool interpretNextPair = false;
    while (vu.m_cycle < budgetEnd && !vu.m_stopRequested)
    {
        if (compiled && !interpretNextPair)
        {
            const StepResult result = compiled(vu, compiledArgs);
            if (result == StepResult::Budget)
                break;
            if (result == StepResult::Ended)
            {
                programEnded = true;
                break;
            }
            interpretNextPair = true;
            continue;
        }
        interpretNextPair = false;
        if (vu.m_unit == VU1Interpreter::Unit::VU1)
            vu1NormShadow.valid = false;
        else
        {
            vu0NormShadow.valid = false;
            vu0SchedulerClean = false; // the interpreter fills the scheduler arrays (PS2X_VU0_LEAN)
        }

        vu.commitReadyPipelines();
        if (vu.m_state.pc + 8u > codeSize)
            break;

        const Decoded &decoded = vu.getDecodedInstructionPairForPc(vuCode, codeSize, memory, vu.m_state.pc);
        if (decoded.upperUsage.reserved || decoded.lowerUsage.reserved)
        {
            vu.reportReservedInstruction(decoded.upperUsage.reserved, decoded.upperUsage.reserved ? decoded.upper : decoded.lower);
            break;
        }

        const StepResult step = stepPair(vu, decoded, vuData, dataSize, gs, memory, codeSize, budgetEnd);
        if (step == StepResult::Budget)
            break;
        if (step == StepResult::Ended)
        {
            programEnded = true;
            break;
        }
    }

    if (programEnded)
    {
        vu.flushPipelines();
        vu.m_state.ebit = false;
        vu.m_state.haltAfterDelaySlot = false;
        vu.m_pendingHaltD = false;
        vu.m_pendingHaltT = false;
    }
    if (vu.m_unit == VU1Interpreter::Unit::VU1)
        t_vu1BudgetHit = !programEnded && !vu.m_stopRequested && vu.m_cycle >= budgetEnd;
    vu.m_state.cycles = vu.m_cycle;
    if (changeRounding)
        _mm_setcsr((_mm_getcsr() & ~0x6000u) | (previousCsr & 0x6000u));
    return programEnded;
}

VU1State VU1InterpreterAccess::effectiveState(const VU1Interpreter &vu)
{
    VU1State state = vu.m_state;
    for (uint32_t mask = vu.m_vfWriteValid; mask != 0u; mask &= mask - 1u)
    {
        const auto &write = vu.m_vfWritePipeline[static_cast<size_t>(std::countr_zero(mask))];
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((write.laneMask & laneForComponent(component)) != 0u &&
                vu.m_vfLatestWrite[write.reg][component] == write.sequence)
                state.vf[write.reg][component] = write.value[component];
        }
    }
    for (uint32_t mask = vu.m_viWriteValid; mask != 0u; mask &= mask - 1u)
    {
        const auto &write = vu.m_viWritePipeline[static_cast<size_t>(std::countr_zero(mask))];
        if (vu.m_viLatestWrite[write.reg] == write.sequence)
            state.vi[write.reg] = static_cast<int16_t>(write.value);
    }
    for (uint32_t mask = vu.m_accWriteValid; mask != 0u; mask &= mask - 1u)
    {
        const auto &write = vu.m_accWritePipeline[static_cast<size_t>(std::countr_zero(mask))];
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((write.laneMask & laneForComponent(component)) != 0u &&
                vu.m_accLatestWrite[component] == write.sequence)
                state.acc[component] = write.value[component];
        }
    }
    return state;
}

namespace
{
    std::atomic<uint64_t> g_vu1WindowAccesses{0u};
}

void ps2xVu1RegisterWindowAccess(uint32_t qword) noexcept
{
    const uint64_t n = g_vu1WindowAccesses.fetch_add(1u, std::memory_order_relaxed);
    if (n < 20u)
    {
        const char *what = qword < 0x420u ? "VF" : qword < 0x430u ? "VI" : qword == 0x430u ? "STATUS" : "control";
        std::fprintf(stderr, "[vu1-window] VU0 accessed VU1 register window qword %03x (%s)%s\n", qword, what,
                     qword == 0x430u ? ": VU1 status read from outside VU1, rebuild with VU1_RECOMP_STICKY=1" : "");
        std::fflush(stderr);
    }
}

namespace
{
    struct Vu1VerifyStats
    {
        uint64_t runs = 0;
        uint64_t mismatches = 0;
        uint64_t unobservableStatus = 0;
        uint64_t vu0StatusOnly = 0; // VU0: final status differences only the EE's CFC2 vi16/vi17 could read
    };

    // True when some instruction of the loaded microcode reads or sets the status flags (FSEQ/FSSET/FSAND/FSOR).
    // When none does, a VU1 program's final status register cannot be observed by later microprograms, nor by the EE
    // (which only reads VU0's), so PS2X_VU1_VERIFY=obs counts a difference there separately instead of as a mismatch.
    bool microcodeReadsStatus(const uint8_t *code, uint32_t codeSize)
    {
        for (uint32_t pc = 0; pc + 8u <= codeSize; pc += 8u)
        {
            uint32_t lower, upper;
            std::memcpy(&lower, code + pc, 4u);
            std::memcpy(&upper, code + pc + 4u, 4u);
            if (upper & 0x80000000u) // I bit: the lower word is an immediate
                continue;
            const uint32_t opHi = (lower >> 25) & 0x7Fu;
            if (opHi >= 0x14u && opHi <= 0x17u)
                return true;
        }
        return false;
    }

    template <typename T>
    bool sameBits(const T &a, const T &b)
    {
        return std::memcmp(&a, &b, sizeof(T)) == 0;
    }
}

void VU1InterpreterAccess::runVerified(VU1Interpreter &vu, uint8_t *vuCode, uint32_t codeSize, uint8_t *vuData, uint32_t dataSize,
                                       GS &gs, PS2Memory *memory, uint32_t maxCycles)
{
    runVerifiedWith(vu, vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles, lookupCompiledVu1Program(vuCode, codeSize, memory), true);
}

bool VU1InterpreterAccess::runVerifiedWith(VU1Interpreter &vu, uint8_t *vuCode, uint32_t codeSize, uint8_t *vuData, uint32_t dataSize,
                                           GS &gs, PS2Memory *memory, uint32_t maxCycles, CompiledProgram compiled, bool forward)
{
    // thread_local: VU1 verifies on the VU worker, VU0 (PS2X_VU0_VERIFY) on the game thread.
    static thread_local std::unique_ptr<VU1Interpreter> shadow;
    static thread_local std::vector<uint8_t> shadowData;
    static thread_local Vu1VerifyStats stats;
    if (!shadow)
        shadow = std::make_unique<VU1Interpreter>(VU1Interpreter::Unit::VU1);
    *shadow = vu;
    shadowData.assign(vuData, vuData + dataSize);
    const uint32_t startPc = vu.m_state.pc;
    const uint64_t startCycle = vu.m_cycle;

    Vu1PacketSink realSink;
    realSink.forward = forward;
    packetSink = &realSink;
    const bool realEnded = runLoopWith(vu, vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles, compiled);
    Vu1PacketSink shadowSink;
    shadowSink.forward = false;
    packetSink = &shadowSink;
    const bool shadowEnded = runLoopWith(*shadow, vuCode, codeSize, shadowData.data(), dataSize, gs, memory, maxCycles, nullptr);
    packetSink = nullptr;

    std::string diff;
    const auto note = [&diff](const std::string &text)
    {
        if (diff.size() < 1500u)
            diff += " " + text;
    };
    // PS2X_VU1_VERIFY=obs (or PS2X_VU0_VERIFY=obs): compare only what later code can observe (VU memory, GS packets,
    // architectural registers with pipelined results resolved, final MAC/status/clip, cycles), not the internal
    // pipeline bookkeeping (register ready times, the queue of flag records) that optimized code no longer keeps.
    static const bool observableOnly = []
    {
        for (const char *name : {"PS2X_VU1_VERIFY", "PS2X_VU0_VERIFY"})
            if (const char *value = std::getenv(name); value && std::strcmp(value, "obs") == 0)
                return true;
        return false;
    }();
    const VU1State a = effectiveState(vu);
    const VU1State b = effectiveState(*shadow);
    for (uint32_t reg = 0; reg < 32u; ++reg)
        if (!sameBits(a.vf[reg], b.vf[reg]))
        {
            char text[160];
            uint32_t x[4], y[4];
            std::memcpy(x, a.vf[reg], 16u);
            std::memcpy(y, b.vf[reg], 16u);
            std::snprintf(text, sizeof(text), "vf%u=%08x,%08x,%08x,%08x/%08x,%08x,%08x,%08x", reg, x[0], x[1], x[2], x[3], y[0], y[1],
                          y[2], y[3]);
            note(text);
        }
    for (uint32_t reg = 0; reg < 16u; ++reg)
        if (a.vi[reg] != b.vi[reg])
            note("vi" + std::to_string(reg) + "=" + std::to_string(a.vi[reg]) + "/" + std::to_string(b.vi[reg]));
    if (!sameBits(a.acc, b.acc))
        note("acc");
    if (!sameBits(a.q, b.q))
        note("q");
    if (!sameBits(a.p, b.p))
        note("p");
    if (!sameBits(a.i, b.i))
        note("i");
    if (a.r != b.r)
        note("r");
    if (a.pc != b.pc)
        note("pc");
    if (observableOnly && !realEnded)
    {
        // Mid-program (budget exit) the committed flags depend on which records are still in flight, and optimized
        // code pushes fewer records by design. Compare what they resolve to once the pipelines drain, on copies.
        if (vu.m_storeValid == 0u && shadow->m_storeValid == 0u)
        {
            Vu1PacketSink drop;
            drop.forward = false;
            packetSink = &drop;
            auto drainedA = std::make_unique<VU1Interpreter>(vu);
            auto drainedB = std::make_unique<VU1Interpreter>(*shadow);
            drainedA->flushPipelines();
            drainedB->flushPipelines();
            packetSink = nullptr;
            if (drainedA->m_state.mac != drainedB->m_state.mac)
                note("mac(resolved)");
            if (drainedA->m_state.clip != drainedB->m_state.clip)
                note("clip(resolved)");
            if (drainedA->m_state.status != drainedB->m_state.status)
            {
                char text[56];
                std::snprintf(text, sizeof(text), "status(resolved)=%x/%x", static_cast<unsigned>(drainedA->m_state.status),
                              static_cast<unsigned>(drainedB->m_state.status));
                note(text);
            }
        }
    }
    else
    {
        if (a.mac != b.mac)
            note("mac");
        if (a.clip != b.clip)
            note("clip");
        if (a.status != b.status)
        {
            char text[48];
            std::snprintf(text, sizeof(text), "status=%x/%x", static_cast<unsigned>(a.status), static_cast<unsigned>(b.status));
            note(text);
        }
    }
    if (a.ebit != b.ebit || a.haltAfterDelaySlot != b.haltAfterDelaySlot || a.stoppedByD != b.stoppedByD ||
        a.stoppedByT != b.stoppedByT)
        note("halt");
    if (a.branchPending != b.branchPending || (a.branchPending && (a.branchTarget != b.branchTarget || a.branchDelay != b.branchDelay)))
        note("branch");
    if (realEnded != shadowEnded)
        note("ended");
    if (!realEnded && vu.m_cycle != shadow->m_cycle)
        note("cycle " + std::to_string(vu.m_cycle - startCycle) + "/" + std::to_string(shadow->m_cycle - startCycle));
    if (!observableOnly && (!sameBits(vu.m_vfReady, shadow->m_vfReady) || !sameBits(vu.m_viReady, shadow->m_viReady) ||
                            !sameBits(vu.m_accReady, shadow->m_accReady)))
        note("ready");
    if (vu.m_efuResourceReady != shadow->m_efuResourceReady)
        note("efuReady");
    if (vu.m_workingClip != shadow->m_workingClip)
        note("workingClip");
    if (vu.m_viBranchBackupValid != shadow->m_viBranchBackupValid ||
        (vu.m_viBranchBackupValid && (vu.m_viBranchBackupReg != shadow->m_viBranchBackupReg ||
                                      vu.m_viBranchBackupValue != shadow->m_viBranchBackupValue)))
        note("branchBackup");
    if (vu.m_stopRequested != shadow->m_stopRequested)
        note("stop");
    const auto flagEntries = [](const VU1Interpreter &unit)
    {
        std::vector<std::array<uint64_t, 8>> entries;
        for (uint32_t mask = unit.m_flagValid; mask != 0u; mask &= mask - 1u)
        {
            const auto &e = unit.m_flagPipeline[static_cast<size_t>(std::countr_zero(mask))];
            entries.push_back({e.readyCycle, e.issueCycle, e.mac, e.status, e.extraSticky, e.clip,
                               static_cast<uint64_t>(e.writesMac) | (static_cast<uint64_t>(e.writesStatus) << 1) |
                                   (static_cast<uint64_t>(e.writesSticky) << 2) | (static_cast<uint64_t>(e.writesClip) << 3),
                               0u});
        }
        std::sort(entries.begin(), entries.end());
        return entries;
    };
    if (!observableOnly && !realEnded && flagEntries(vu) != flagEntries(*shadow))
        note("flags");
    const auto scalarEqual = [](const auto &x, const auto &y)
    {
        return x.valid == y.valid && (!x.valid || (x.readyCycle == y.readyCycle && sameBits(x.value, y.value)));
    };
    if (!realEnded && (!scalarEqual(vu.m_fdiv, shadow->m_fdiv) || vu.m_fdiv.statusDi != shadow->m_fdiv.statusDi))
        note("fdiv");
    if (!realEnded && (!scalarEqual(vu.m_efu[0], shadow->m_efu[0]) || !scalarEqual(vu.m_efu[1], shadow->m_efu[1])))
        note("efu");
    if (vu.m_storeValid != 0u || shadow->m_storeValid != 0u)
        note("stores");
    const auto &xa = vu.m_xgkick;
    const auto &xb = shadow->m_xgkick;
    if (xa.active != xb.active || xa.sourceAddress != xb.sourceAddress || xa.totalBytes != xb.totalBytes ||
        xa.copiedBytes != xb.copiedBytes || xa.currentTagEnd != xb.currentTagEnd || xa.cycleCredit != xb.cycleCredit ||
        xa.issueCycle != xb.issueCycle || xa.currentTagEop != xb.currentTagEop ||
        std::memcmp(xa.packet.data(), xb.packet.data(), std::min<size_t>(xa.copiedBytes, xa.packet.size())) != 0)
        note("xgkick");
    if (std::memcmp(vuData, shadowData.data(), dataSize) != 0)
    {
        uint32_t first = 0;
        while (first < dataSize && vuData[first] == shadowData[first])
            ++first;
        char text[64];
        std::snprintf(text, sizeof(text), "mem@%04x", first);
        note(text);
    }
    if (realSink.packets != shadowSink.packets)
        note("packets " + std::to_string(realSink.packets.size()) + "/" + std::to_string(shadowSink.packets.size()));

    ++stats.runs;
    if (observableOnly && memory)
    {
        // Final status differences are unobservable when no instruction of the microcode reads status (see above).
        // For VU0 the EE can still read them with CFC2 vi16/vi17, so they get their own count instead: Urban Reign
        // never executes those (docs/PROGRESS.md §62), which is what lets its VU0 image use fast FMACs.
        const bool unitVu0 = vu.m_unit != VU1Interpreter::Unit::VU1;
        static thread_local const uint8_t *cachedCode = nullptr;
        static thread_local uint64_t cachedGeneration = ~0ull;
        static thread_local bool readsStatus = true;
        const uint64_t generation = unitVu0 ? memory->getVU0CodeGeneration() : memory->getVU1CodeGeneration();
        if (cachedCode != vuCode || cachedGeneration != generation)
        {
            cachedCode = vuCode;
            cachedGeneration = generation;
            readsStatus = microcodeReadsStatus(vuCode, codeSize);
        }
        if (!readsStatus)
        {
            // Strip the status notes; if that is all that differed, it is not a mismatch.
            std::string kept;
            size_t start = 0;
            bool stripped = false;
            while (start < diff.size())
            {
                size_t end = diff.find(' ', start + 1);
                if (end == std::string::npos)
                    end = diff.size();
                const std::string item = diff.substr(start, end - start);
                if (item.find("status") != std::string::npos)
                    stripped = true;
                else
                    kept += item;
                start = end;
            }
            if (stripped)
                ++(unitVu0 ? stats.vu0StatusOnly : stats.unobservableStatus);
            diff = kept;
        }
    }
    if (!diff.empty())
    {
        ++stats.mismatches;
        if (stats.mismatches <= 50u)
        {
            std::fprintf(stderr, "[vu1-verify] mismatch #%llu run %llu image %016llx entry %04x -> pc %04x/%04x:%s\n",
                         static_cast<unsigned long long>(stats.mismatches), static_cast<unsigned long long>(stats.runs),
                         static_cast<unsigned long long>(fnv1a64(vuCode, codeSize)), startPc, a.pc, b.pc, diff.c_str());
            std::fflush(stderr);
        }
    }
    if ((stats.runs & 0x3FFFu) == 0u && g_vu1WindowAccesses.load(std::memory_order_relaxed) != 0u)
        std::fprintf(stderr, "[vu1-verify] ERROR: %llu accesses to VU1's register window (see [vu1-window])\n",
                     static_cast<unsigned long long>(g_vu1WindowAccesses.load(std::memory_order_relaxed)));
    if ((stats.runs & 0x3FFFu) == 0u)
    {
        std::fprintf(stderr, "[vu1-verify] %llu runs, %llu mismatches (+%llu final status differences nothing can read, "
                             "+%llu VU0 final status differences only CFC2 vi16/vi17 could read)\n",
                     static_cast<unsigned long long>(stats.runs), static_cast<unsigned long long>(stats.mismatches),
                     static_cast<unsigned long long>(stats.unobservableStatus),
                     static_cast<unsigned long long>(stats.vu0StatusOnly));
        std::fflush(stderr);
    }
    return diff.empty();
}

VU1InterpreterAccess::CompiledProgram findCompiledVu1ProgramByHash(uint64_t imageHash)
{
    const auto &programs = vu1CompiledRegistry().programs;
    const auto it = programs.find(imageHash);
    return it == programs.end() ? nullptr : it->second;
}

void registerVu1CompiledProgram(uint64_t imageHash, VU1InterpreterAccess::CompiledProgram program)
{
    vu1CompiledRegistry().programs[imageHash] = program;
}

namespace ps2x::vu1hooks
{
    namespace
    {
        std::atomic<UploadHook> g_uploadHook{nullptr};
        std::atomic<Path1Filter> g_path1Filter{nullptr};
    }

    void setUploadHook(UploadHook hook) { g_uploadHook.store(hook, std::memory_order_release); }

    void runUploadHook(uint8_t *code, uint32_t codeSize, uint32_t byteAddress, uint32_t size)
    {
        if (const UploadHook hook = g_uploadHook.load(std::memory_order_acquire))
            hook(code, codeSize, byteAddress, size);
    }

    void setPath1Filter(Path1Filter filter) { g_path1Filter.store(filter, std::memory_order_release); }

    Path1Filter path1Filter() { return g_path1Filter.load(std::memory_order_acquire); }
}

void ps2xNoteVu1Upload(uint32_t byteAddress, const uint8_t *data, uint32_t size)
{
    const Vu1ProgramCapture *capture = vu1ProgramCapture();
    if (!capture || !data || size == 0u || (byteAddress & 7u) != 0u || (size & 7u) != 0u)
        return;
    Vu1UploadCapture &up = g_vu1Uploads;
    std::lock_guard<std::mutex> lock(up.mutex);
    if (!up.bytes.empty() && (up.address + up.bytes.size() != byteAddress || up.bytes.size() + size > 0x4000u))
        flushVu1UploadCaptureLocked(*capture);
    if (up.bytes.empty())
        up.address = byteAddress;
    up.bytes.insert(up.bytes.end(), data, data + size);
}

void registerVu1CompiledUnit(const Vu1CompiledUnit &unit)
{
    vu1CompiledRegistry().units.push_back(unit);
}

void saveVuState(VU1Interpreter &vu, ps2x::state::Writer &out)
{
    VU1InterpreterAccess::serializeState(vu, out);
}

void loadVuState(VU1Interpreter &vu, ps2x::state::Reader &in)
{
    VU1InterpreterAccess::serializeState(vu, in);
}

// Called on F8 (ps2_runtime.cpp): trace every VU1 run of the next `fields` fields to vu1trace_<tick>.bin.
void ps2xRequestVu1Trace(uint32_t fields)
{
    g_vu1TraceRequestFields.store(fields, std::memory_order_release);
}

// Sweep R2 (ps2_runtime.cpp MSCAL/MSCNT callbacks).
bool ps2xVu1LastRunHitBudget()
{
    return t_vu1BudgetHit;
}
