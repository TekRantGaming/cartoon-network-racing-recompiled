#pragma once
// The settings file the TRG Launcher writes for the game: <exe dir>/<exe name>.toml, flat `name = value` lines
// (launcher/project.cpp lists every key). Without a file (development runs) every lookup returns its default.
#include <filesystem>
#include <string>

void cnrLoadSettings(const std::filesystem::path &file);
bool cnrSettingsLoaded();

std::string cnrSetting(const char *key, const std::string &def = {});
int cnrSettingInt(const char *key, int def);
bool cnrSettingBool(const char *key, bool def);
