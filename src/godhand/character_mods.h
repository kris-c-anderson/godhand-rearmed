#pragma once

#include <cstdint>
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
    // written over the game's file in _DATA.AFS through PS2DiscOverlay, zero-padded to its size (sizes and offsets in
    // the archive never change); a file larger than the game's is left out (logged).
    void install(const std::filesystem::path &exeDir);

    // Diagnostics for the cloth attachment (Gene's coat, object 0x101 parented to his bone 17 by the player's init at
    // 0x108068): the hook at 0x108088 remembers the player and the coat, and onFrame logs their bone matrices once
    // a few seconds later ([cloth] lines), to work out how an attached model's root is placed.
    void installProbe();
    void onFrame(uint8_t *rdram);
}
