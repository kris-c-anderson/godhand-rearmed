// Names of the boot ELF's functions (godhand_function_names.inc, from the God Hand decomp via
// tools/symbols/import_decomp_names.py), for diagnostics that print guest addresses.
#pragma once

#include <cstdint>
#include <string>

namespace godhand::symbols
{
    // "cPlCamera_update" for an address inside that function (start = where it begins), nullptr when no named function
    // holds the address.
    const char *functionAt(uint32_t address, uint32_t *start = nullptr);

    // "cPlCamera_update+0x1c" when the address is inside a named function (no "+0x0" at its start), otherwise the hex
    // address ("0x00139FC4").
    std::string describe(uint32_t address);
}
