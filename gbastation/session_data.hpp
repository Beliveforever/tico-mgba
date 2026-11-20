#pragma once
#include "state_store.hpp"

#include "woplay/emulation/display_config.hpp"

namespace woplay_host {
const StateStore& savedStates();
std::string gameIcon();
std::string playerAvatar();
namespace emulation = gbastation::emulation;
const emulation::Module& module();
const emulation::Json& theme();
emulation::Json displaySetting(const std::string& key);
void changeDisplay(const std::string& key, const emulation::Json& value);
const emulation::DisplayConfig& displayPreferences();
const std::string& configuredShaderPath();
void changeDisplayScope(bool independent);
void chooseDisplayPreset(const std::string& id);
emulation::Json setting(const std::string& key, bool game);
bool overrides(const std::string& key);
void changeSetting(const std::string& key, const emulation::Json& value, bool game);
}
