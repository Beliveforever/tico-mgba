#pragma once

#include "overlay/overlay_ui.h"
#include "session_host.hpp"
#include <optional>
#include <functional>
#include <imgui.h>
#include <string>

class TicoCore;
struct _SDL_GameController;
struct VkCommandBuffer_T;
namespace woplay_menu {
using Action = SwitchFrontend::OverlayUI::Action;
using ShaderCallbacks = SwitchFrontend::OverlayUI::ShaderCallbacks;
#ifdef WOPLAY_HOST
bool loadFonts();
bool initialize(std::function<void()> settingsChanged);
void visible(bool show);
void offerResume();
void shaders(ShaderCallbacks callbacks);
void draw(TicoCore* core, ImVec4 frame);
std::optional<ImVec4> previewFrame(ImVec2 size);
bool gamePointer(ImVec2 position, bool down);
void prepareEffects(VkCommandBuffer_T* cmd, ImDrawData& frame);
void drawOverlay(ImDrawList& draw, ImVec2 bounds);
std::optional<woplay_host::StateRequest> takeStateAction();
void hud(float fps, int width, int height, bool fast);
void input(_SDL_GameController* pad);
void touch(const SwitchFrontend::OverlayUI::TouchInput& input);
void notify(const std::string& text);
std::string translate(const std::string& key);
Action takeAction();
void shutdown();
#else
inline bool loadFonts() { return false; }
inline bool initialize(std::function<void()>) { return false; }
inline void visible(bool) {}
inline void offerResume() {}
inline void shaders(ShaderCallbacks) {}
inline void draw(TicoCore*, ImVec4) {}
inline std::optional<ImVec4> previewFrame(ImVec2) { return {}; }
inline bool gamePointer(ImVec2, bool) { return false; }
inline void prepareEffects(VkCommandBuffer_T*, ImDrawData&) {}
inline void drawOverlay(ImDrawList&, ImVec2) {}
inline std::optional<woplay_host::StateRequest> takeStateAction() { return {}; }
inline void hud(float, int, int, bool) {}
inline void input(_SDL_GameController*) {}
inline void touch(const SwitchFrontend::OverlayUI::TouchInput&) {}
inline void notify(const std::string&) {}
inline std::string translate(const std::string& key) { return key; }
inline Action takeAction() { return Action::None; }
inline void shutdown() {}
#endif
}
