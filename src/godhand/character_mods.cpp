// Character model replacements (see character_mods.h).
//
// God Hand's files live in _DATA.AFS on the disc: "AFS\0", u32 count, count x (u32 offset, u32 size) relative to the
// archive start, then (u32 offset, u32 size) of CRI's attribute table (count x 48 bytes: name[32], six u16 date
// fields, u32 file size). GODHAND.FST maps paths to archive indices (tools/snr2/snr2disc.py has the layout). A
// replacement no larger than the original is written over the original's bytes, zero-padded to its size; no size or
// offset on the disc changes.
#include "godhand/character_mods.h"
#include "godhand/ini_file.h"
#include "godhand/log.h"
#include "runtime/ps2_cd_image.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>

namespace fs = std::filesystem;

namespace godhand::characters
{
    namespace
    {
        uint32_t le32(const std::vector<uint8_t> &b, size_t o)
        {
            return o + 4 > b.size() ? 0u : static_cast<uint32_t>(b[o]) | (static_cast<uint32_t>(b[o + 1]) << 8) |
                                               (static_cast<uint32_t>(b[o + 2]) << 16) | (static_cast<uint32_t>(b[o + 3]) << 24);
        }

        std::string lower(std::string s)
        {
            for (char &c : s)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }

        std::string utf8(const fs::path &p)
        {
            const std::u8string s = p.generic_u8string();
            return std::string(s.begin(), s.end());
        }

        bool readImage(uint64_t offset, std::vector<uint8_t> &out, size_t size)
        {
            out.assign(size, 0u);
            return PS2CdImage::instance().read(offset, out.data(), size);
        }

        // GODHAND.FST: lower-case path -> AFS index.
        std::map<std::string, uint32_t> readFst(const std::vector<uint8_t> &buf)
        {
            std::map<std::string, uint32_t> out;
            if (buf.size() < 16 || std::memcmp(buf.data(), "FST", 3) != 0)
                return out;
            const uint32_t stringsOff = le32(buf, 8), entriesOff = le32(buf, 12);
            if (stringsOff > buf.size() || entriesOff > buf.size() || stringsOff > entriesOff)
                return out;
            auto nameAt = [&](uint32_t o) {
                std::string s;
                for (size_t i = stringsOff + o; i < entriesOff && buf[i] != 0; ++i)
                    s.push_back(static_cast<char>(buf[i]));
                return s;
            };
            std::function<void(uint32_t, const std::string &, int)> walk = [&](uint32_t o, const std::string &prefix, int depth) {
                if (depth > 16)
                    return;
                for (;; o += 12u)
                {
                    const size_t at = entriesOff + o;
                    if (at + 12 > buf.size())
                        return;
                    const uint8_t type = buf[at];
                    const uint32_t name = le32(buf, at + 4), value = le32(buf, at + 8);
                    if (type == 3)
                        return;
                    const std::string path = prefix.empty() ? nameAt(name) : prefix + "/" + nameAt(name);
                    if (type == 1)
                        walk(value, path, depth + 1);
                    else if (type == 2)
                        out[lower(path)] = value;
                    else
                        return;
                }
            };
            walk(0, {}, 0);
            return out;
        }

        Character describe(const fs::path &folder)
        {
            Character c;
            c.id = utf8(folder.filename());
            IniFile ini;
            if (ini.load(folder / "character.ini"))
            {
                c.name = ini.get("Character", "Name");
                c.credit = ini.get("Character", "Credit");
            }
            if (c.name.empty())
                c.name = c.id;
            return c;
        }
    }

    std::vector<Character> list(const fs::path &exeDir)
    {
        std::vector<Character> out{{"gene", "Gene", "The game's own"}};
        std::error_code error;
        const fs::path root = exeDir / "mods" / "characters";
        std::vector<fs::path> folders;
        for (const auto &entry : fs::directory_iterator(root, error))
            if (entry.is_directory(error))
                folders.push_back(entry.path());
        std::sort(folders.begin(), folders.end());
        for (const fs::path &folder : folders)
            if (lower(utf8(folder.filename())) != "gene")
                out.push_back(describe(folder));
        return out;
    }

    void install(const fs::path &exeDir)
    {
        const char *chosen = std::getenv("GODHAND_CHARACTER");
        const std::string id = chosen ? lower(chosen) : std::string();
        if (id.empty() || id == "gene")
            return;
        const fs::path folder = exeDir / "mods" / "characters" / id;
        std::error_code error;
        if (!fs::is_directory(folder, error))
        {
            GH_WARN(Boot, "character '" << id << "': no folder " << utf8(folder) << "; playing as Gene");
            return;
        }
        PS2CdImage &image = PS2CdImage::instance();
        PS2CdImage::Entry afsEntry{}, fstEntry{};
        if (!image.ensureOpenFromIoPaths() || !image.find("_DATA.AFS", afsEntry) || !image.find("GODHAND.FST", fstEntry))
        {
            GH_WARN(Boot, "character '" << id << "': the disc image has no _DATA.AFS / GODHAND.FST; playing as Gene");
            return;
        }
        const uint64_t afsBase = static_cast<uint64_t>(afsEntry.lba) * PS2CdImage::kSectorSize;
        std::vector<uint8_t> head, fst;
        if (!readImage(afsBase, head, 8) || std::memcmp(head.data(), "AFS", 3) != 0)
            return;
        const uint32_t count = le32(head, 4);
        std::vector<uint8_t> toc;
        if (count == 0 || count > 100000 || !readImage(afsBase, toc, 16 + 8u * count) ||
            !readImage(static_cast<uint64_t>(fstEntry.lba) * PS2CdImage::kSectorSize, fst, static_cast<size_t>(fstEntry.size)))
            return;
        const auto fstIndex = readFst(fst);

        std::vector<PS2DiscOverlay::Patch> patches;
        int replaced = 0;
        for (const auto &entry : fs::recursive_directory_iterator(folder, error))
        {
            if (!entry.is_regular_file(error))
                continue;
            const std::string rel = lower(utf8(fs::relative(entry.path(), folder, error)));
            const auto found = fstIndex.find(rel);
            if (found == fstIndex.end())
                continue; // character.ini and anything else that is not a disc file
            const uint32_t index = found->second;
            if (index >= count)
                continue;
            const uint32_t offset = le32(toc, 8 + 8u * index), size = le32(toc, 12 + 8u * index);
            std::ifstream in(entry.path(), std::ios::binary);
            std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (data.empty() || data.size() > size)
            {
                GH_ERROR(Boot, "character '" << id << "': " << rel << " is " << data.size() << " bytes, the game's file "
                                             << size << "; left out (a replacement must not be larger)");
                continue;
            }
            // The sizes in the archive stay as they are: CRI's file system works out where each file starts from the
            // sizes before it, so a changed size moves every later file (the title room DLL then read as garbage, a
            // black screen after the logo). The replacement is padded with zeros to the original size instead.
            const uint32_t newSize = static_cast<uint32_t>(data.size());
            data.resize(size, 0u);
            patches.emplace_back(afsBase + offset, std::move(data));
            GH_INFO(Boot, "character '" << id << "': " << rel << " (AFS " << index << ", " << size << " -> " << newSize << " bytes)");
            ++replaced;
        }
        if (replaced == 0)
        {
            GH_WARN(Boot, "character '" << id << "': no replacement files in " << utf8(folder) << "; playing as Gene");
            return;
        }
        PS2DiscOverlay::set(std::move(patches));
        GH_INFO(Boot, "playing as " << describe(folder).name << " (" << replaced << " files)");
    }
}
