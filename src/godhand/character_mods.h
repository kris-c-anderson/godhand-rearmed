#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace godhand::characters
{
    // A playable character model in place of Gene: <exe>/mods/characters/<id>/ holds replacement game files under
    // their disc paths (pl/pl00.dat ...), built by 'build.py models' from your disc and the mod's own data, plus
    // character.ini ([Character] Name=, Credit=).
    struct Character
    {
        std::string id;     // folder name; "gene" = the game's own
        std::string name;   // shown in the launcher
        std::string credit; // shown under it
    };

    // Gene first, then every folder under mods/characters that holds files.
    std::vector<Character> list(const std::filesystem::path &exeDir);

    // After PS2Runtime::setIoPaths, before the game runs: [Mods] Character (GODHAND_CHARACTER). Each replacement is
    // written over its own slot in _DATA.AFS (with the archive's size fields) through PS2DiscOverlay; a file larger
    // than its slot is left out (logged).
    void install(const std::filesystem::path &exeDir);
}
