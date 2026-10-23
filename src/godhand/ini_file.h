#pragma once

// godhand.ini as text: values read and changed in place, so the comments, order and line endings stay as they are
// (the launcher's Options write here; the file stays editable by hand).
#include <filesystem>
#include <string>
#include <vector>

namespace godhand
{
    class IniFile
    {
    public:
        bool load(const std::filesystem::path &path);
        bool save() const;

        // Section and key ignore case. Commented lines ("#key=") are not values.
        std::string get(const std::string &section, const std::string &key, const std::string &fallback = {}) const;
        bool getBool(const std::string &section, const std::string &key, bool fallback) const;
        // Replaces the value, or adds the key at the end of its section (the section at the end of the file if it
        // does not exist).
        void set(const std::string &section, const std::string &key, const std::string &value);
        void setBool(const std::string &section, const std::string &key, bool value) { set(section, key, value ? "1" : "0"); }

    private:
        int find(const std::string &section, const std::string &key) const; // line index or -1
        std::filesystem::path m_path;
        std::vector<std::string> m_lines;
        std::string m_newline = "\r\n";
    };
}
