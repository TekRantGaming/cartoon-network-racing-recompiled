#include "cnr_settings.h"

#include <cstdlib>
#include <fstream>
#include <map>

namespace
{
    std::map<std::string, std::string> g_values;
    bool g_loaded = false;

    std::string trim(const std::string &s)
    {
        const size_t a = s.find_first_not_of(" \t\r");
        if (a == std::string::npos)
            return {};
        return s.substr(a, s.find_last_not_of(" \t\r") - a + 1);
    }
}

void cnrLoadSettings(const std::filesystem::path &file)
{
    std::ifstream in(file);
    if (!in)
        return;
    g_loaded = true;
    std::string line;
    while (std::getline(in, line))
    {
        const std::string t = trim(line);
        const size_t eq = t.find('=');
        if (t.empty() || t[0] == '#' || t[0] == '[' || eq == std::string::npos)
            continue;
        std::string value = trim(t.substr(eq + 1));
        if (value.size() >= 2 && value.front() == '"')
            value = value.substr(1, value.find('"', 1) - 1);
        g_values[trim(t.substr(0, eq))] = value;
    }
}

bool cnrSettingsLoaded() { return g_loaded; }

std::string cnrSetting(const char *key, const std::string &def)
{
    const auto it = g_values.find(key);
    return it == g_values.end() || it->second.empty() ? def : it->second;
}

int cnrSettingInt(const char *key, int def)
{
    const std::string v = cnrSetting(key);
    return v.empty() ? def : std::atoi(v.c_str());
}

bool cnrSettingBool(const char *key, bool def)
{
    const std::string v = cnrSetting(key);
    return v.empty() ? def : v == "true";
}
