#include "guest_symbols.h"

#include <algorithm>
#include <cstdio>

namespace godhand::symbols
{
    namespace
    {
        struct Range
        {
            uint32_t start;
            uint32_t end;
            const char *name;
        };
        constexpr Range kFunctions[] = {
#define X(start, end, name) {start, end, name},
#include "godhand_function_names.inc"
#undef X
        };
    }

    const char *functionAt(uint32_t address, uint32_t *start)
    {
        const Range *first = std::begin(kFunctions);
        const Range *last = std::end(kFunctions);
        // The ranges are sorted by start: the last one starting at or before the address is the candidate.
        const Range *it = std::upper_bound(first, last, address, [](uint32_t a, const Range &r) { return a < r.start; });
        if (it == first)
            return nullptr;
        --it;
        if (address >= it->end)
            return nullptr;
        if (start)
            *start = it->start;
        return it->name;
    }

    std::string describe(uint32_t address)
    {
        uint32_t start = 0;
        char text[64];
        if (const char *name = functionAt(address, &start))
        {
            if (address == start)
                return name;
            std::snprintf(text, sizeof(text), "+0x%x", address - start);
            return std::string(name) + text;
        }
        std::snprintf(text, sizeof(text), "0x%08X", address);
        return text;
    }
}
