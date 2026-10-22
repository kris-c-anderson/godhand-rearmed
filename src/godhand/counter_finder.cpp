#include "godhand/counter_finder.h"
#include "godhand/function_hooks.h"
#include "godhand/log.h"
#include "ps2_runtime.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace godhand::counterfinder
{
    namespace
    {
        constexpr uint32_t kGap = 10u;          // game frames between the copies
        constexpr uint32_t kQuiet = 300u;       // frames without a trigger call before a new capture can start
        bool g_enabled = false;
        uint64_t g_frame = 0, g_lastTrigger = 0;
        int64_t g_captureStart = -1;            // frame of the first copy, -1 = idle
        uint32_t g_captures = 0;
        std::vector<uint8_t> g_copies[3];

        void analyse()
        {
            const uint8_t *a = g_copies[0].data(), *b = g_copies[1].data(), *c = g_copies[2].data();
            uint32_t ints = 0, floats = 0;
            for (uint32_t at = 0; at + 4u <= PS2_RAM_SIZE; at += 4u)
            {
                int32_t ia, ib, ic;
                std::memcpy(&ia, a + at, 4);
                std::memcpy(&ib, b + at, 4);
                std::memcpy(&ic, c + at, 4);
                if (ia == ib || ib == ic)
                    continue;
                // A steady int: the same step both times, small.
                const int64_t s1 = int64_t(ib) - ia, s2 = int64_t(ic) - ib;
                if (s1 == s2 && std::llabs(s1) <= 4096 && std::llabs(int64_t(ia)) < (1 << 20))
                {
                    if (ints++ < 400u)
                        GH_INFO(Game, "[counters] int   0x" << std::hex << at << std::dec << ": " << ia << " -> " << ib << " -> " << ic
                                                           << " (" << s1 << " per " << kGap << " frames)");
                    continue;
                }
                float fa, fb, fc;
                std::memcpy(&fa, a + at, 4);
                std::memcpy(&fb, b + at, 4);
                std::memcpy(&fc, c + at, 4);
                if (!std::isfinite(fa) || !std::isfinite(fb) || !std::isfinite(fc) || std::fabs(fa) > 1.0e6f || std::fabs(fa) < 1.0e-6f)
                    continue;
                const float d1 = fb - fa, d2 = fc - fb;
                if (d1 == 0.0f || std::fabs(d1 - d2) > 1.0e-4f * std::fmax(1.0f, std::fabs(d1)) || std::fabs(d1) > 1.0e4f)
                    continue;
                if (floats++ < 600u)
                    GH_INFO(Game, "[counters] float 0x" << std::hex << at << std::dec << ": " << fa << " -> " << fb << " -> " << fc
                                                       << " (" << d1 << " per " << kGap << " frames)");
            }
            GH_INFO(Game, "[counters] capture " << g_captures << ": " << ints << " steady ints, " << floats << " steady floats");
        }
    }

    void install(PS2Runtime &)
    {
        const char *value = std::getenv("GODHAND_FIND_COUNTERS");
        if (!value || !*value)
            return;
        const uint32_t address = static_cast<uint32_t>(std::strtoul(value, nullptr, 16));
        g_enabled = FunctionHooks::instance().observeEntry(address, "counter_finder_trigger", [](uint8_t *, R5900Context *, PS2Runtime *) {
            if (g_captureStart < 0 && g_frame - g_lastTrigger > kQuiet)
            {
                g_captureStart = static_cast<int64_t>(g_frame) + 2; // a couple of frames in, once the screen is up
                GH_INFO(Game, "[counters] trigger call at game frame " << g_frame << ": capture starts");
            }
            g_lastTrigger = g_frame;
        });
        GH_INFO(Game, "[counters] watching for calls of 0x" << std::hex << address << (g_enabled ? "" : " (could not hook)"));
    }

    void onFrame(uint8_t *rdram)
    {
        ++g_frame;
        if (!g_enabled || g_captureStart < 0)
            return;
        const int64_t step = static_cast<int64_t>(g_frame) - g_captureStart;
        if (step < 0 || step % kGap != 0 || step / kGap > 2)
            return;
        const size_t index = static_cast<size_t>(step / kGap);
        g_copies[index].assign(rdram, rdram + PS2_RAM_SIZE);
        if (index == 2u)
        {
            ++g_captures;
            analyse();
            g_captureStart = -1;
            for (auto &copy : g_copies)
                std::vector<uint8_t>().swap(copy);
        }
    }
}
