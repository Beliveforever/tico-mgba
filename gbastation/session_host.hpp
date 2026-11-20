#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>
#include <utility>

namespace woplay_host {
enum class StateGroup { Cycle, Manual, Resume };
enum class StateOperation { Save, Load, Delete, Expand };
struct DisplayGeometry {
    std::string mode;
    int integer = 0;
    float offsetX = 0, offsetY = 0, scale = 1;
    std::array<float, 4> customFrame{};
};
struct StateRequest {
    StateOperation operation;
    StateGroup group;
    int slot = 0;
};
#ifdef WOPLAY_HOST
bool active();
bool initialize(int argc, char** argv);
bool launchingGame();
std::string platform();
std::string rom();
std::string title();
std::string resumePolicy();
std::string directory(const char* name);
std::string batteryFile(const char* extension, bool loading);
std::string stateFile(StateGroup group, int slot = 0);
int saveState(StateGroup group, int slot, const std::function<bool(const std::string&)>& write);
void deleteState(StateGroup group, int slot);
void expandStates(StateGroup group);
std::uint64_t sessionSeconds();
DisplayGeometry displayGeometry();
bool shaderEnabled();
std::string shaderPreferenceKey();
std::uint64_t displayConfigRevision();
std::string overlayPath();
std::string cheatsFile(const char* extension);
std::string option(std::string_view key, std::string_view fallback = {});
std::string optionJson(std::string_view key);
void setOptionJson(const std::string& key, const std::string& value);
std::vector<std::pair<std::string, std::string>> nativeOptions(bool running);
void beginGame();
void frame();
void pause();
void finish(const std::string& error = {});
#else
inline bool active() { return false; }
inline bool initialize(int, char**) { return false; }
inline bool launchingGame() { return false; }
inline std::string platform() { return {}; }
inline std::string rom() { return {}; }
inline std::string title() { return {}; }
inline std::string resumePolicy() { return "never"; }
inline std::string directory(const char*) { return {}; }
inline std::string batteryFile(const char*, bool) { return {}; }
inline std::string stateFile(StateGroup, int = 0) { return {}; }
inline int saveState(StateGroup, int, const std::function<bool(const std::string&)>&) { return 0; }
inline void deleteState(StateGroup, int) {}
inline void expandStates(StateGroup) {}
inline std::uint64_t sessionSeconds() { return 0; }
inline DisplayGeometry displayGeometry() { return {}; }
inline bool shaderEnabled() { return true; }
inline std::string shaderPreferenceKey() { return {}; }
inline std::uint64_t displayConfigRevision() { return 0; }
inline std::string overlayPath() { return {}; }
inline std::string cheatsFile(const char*) { return {}; }
inline std::string option(std::string_view, std::string_view value = {}) { return std::string(value); }
inline std::string optionJson(std::string_view) { return {}; }
inline void setOptionJson(const std::string&, const std::string&) {}
inline std::vector<std::pair<std::string, std::string>> nativeOptions(bool) { return {}; }
inline void beginGame() {}
inline void frame() {}
inline void pause() {}
inline void finish(const std::string& = {}) {}
#endif
}
