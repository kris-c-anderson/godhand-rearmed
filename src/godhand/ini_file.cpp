#include "godhand/ini_file.h"

#include <cctype>
#include <fstream>
#include <sstream>

namespace godhand
{
    namespace
    {
        std::string trim(const std::string &text)
        {
            const size_t begin = text.find_first_not_of(" \t\r");
            const size_t end = text.find_last_not_of(" \t\r");
            return begin == std::string::npos ? std::string() : text.substr(begin, end - begin + 1);
        }

        std::string lower(std::string text)
        {
            for (char &c : text)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return text;
        }

        // "[name]" -> "name" (lower case); anything else -> no section.
        bool sectionOf(const std::string &line, std::string &section)
        {
            const std::string text = trim(line);
            if (text.size() < 2 || text.front() != '[' || text.back() != ']')
                return false;
            section = lower(trim(text.substr(1, text.size() - 2)));
            return true;
        }

        bool keyOf(const std::string &line, std::string &key, std::string &value)
        {
            const std::string text = trim(line);
            if (text.empty() || text[0] == '#' || text[0] == ';')
                return false;
            const size_t equals = text.find('=');
            if (equals == std::string::npos)
                return false;
            key = lower(trim(text.substr(0, equals)));
            value = trim(text.substr(equals + 1));
            return true;
        }
    }

    bool IniFile::load(const std::filesystem::path &path)
    {
        m_path = path;
        m_lines.clear();
        std::ifstream file(path, std::ios::binary);
        if (!file)
            return false;
        std::stringstream buffer;
        buffer << file.rdbuf();
        const std::string text = buffer.str();
        m_newline = text.find("\r\n") != std::string::npos || text.empty() ? "\r\n" : "\n";
        size_t start = 0;
        while (start < text.size())
        {
            size_t end = text.find('\n', start);
            if (end == std::string::npos)
                end = text.size();
            std::string line = text.substr(start, end - start);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            m_lines.push_back(std::move(line));
            start = end + 1;
        }
        return true;
    }

    bool IniFile::save() const
    {
        std::ofstream file(m_path, std::ios::binary | std::ios::trunc);
        if (!file)
            return false;
        for (const std::string &line : m_lines)
            file << line << m_newline;
        return static_cast<bool>(file);
    }

    int IniFile::find(const std::string &section, const std::string &key) const
    {
        const std::string wantSection = lower(section), wantKey = lower(key);
        std::string current, lineKey, value;
        for (size_t i = 0; i < m_lines.size(); ++i)
        {
            if (sectionOf(m_lines[i], current))
                continue;
            if (current == wantSection && keyOf(m_lines[i], lineKey, value) && lineKey == wantKey)
                return static_cast<int>(i);
        }
        return -1;
    }

    std::string IniFile::get(const std::string &section, const std::string &key, const std::string &fallback) const
    {
        const int index = find(section, key);
        if (index < 0)
            return fallback;
        std::string lineKey, value;
        keyOf(m_lines[static_cast<size_t>(index)], lineKey, value);
        return value;
    }

    bool IniFile::getBool(const std::string &section, const std::string &key, bool fallback) const
    {
        const std::string value = lower(get(section, key));
        if (value.empty())
            return fallback;
        return !(value == "0" || value == "false" || value == "off" || value == "no");
    }

    void IniFile::set(const std::string &section, const std::string &key, const std::string &value)
    {
        const int index = find(section, key);
        if (index >= 0)
        {
            std::string &line = m_lines[static_cast<size_t>(index)];
            const size_t equals = line.find('=');
            line = line.substr(0, equals + 1) + value;
            return;
        }
        // Add it after the last setting or comment of the section.
        const std::string wantSection = lower(section);
        std::string current;
        int sectionLine = -1, insertAt = -1;
        for (size_t i = 0; i < m_lines.size(); ++i)
        {
            if (sectionOf(m_lines[i], current))
            {
                if (current == wantSection)
                    sectionLine = static_cast<int>(i);
                continue;
            }
            if (current == wantSection && !trim(m_lines[i]).empty())
                insertAt = static_cast<int>(i) + 1;
        }
        if (sectionLine < 0)
        {
            if (!m_lines.empty() && !trim(m_lines.back()).empty())
                m_lines.emplace_back();
            m_lines.push_back("[" + section + "]");
            m_lines.push_back(key + "=" + value);
            return;
        }
        if (insertAt < 0)
            insertAt = sectionLine + 1;
        m_lines.insert(m_lines.begin() + insertAt, key + "=" + value);
    }
}
