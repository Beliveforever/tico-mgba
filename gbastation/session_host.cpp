#include "session_host.hpp"
#include "session_data.hpp"
#include "woplay/emulation/display_config.hpp"
#include "woplay/emulation/game_storage.hpp"
#include "woplay/emulation/local_profiles.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <optional>
#ifdef __SWITCH__
#include <switch.h>
#endif

namespace woplay_host {
namespace fs = std::filesystem;
using emulation::Json;
namespace {
using Clock = std::chrono::steady_clock;
fs::path root, dataRoot, profileDirectory, moduleDirectory;
std::optional<emulation::Session> launch;
emulation::Module core;
emulation::GameStoragePaths storage;
std::string profile, uuid, system, romPath, gameTitle, iconPath, avatarPath;
std::optional<StateStore> states;
std::string stateError;
Json coreOptions, gameOptions, gameOverrides, themeSnapshot;
emulation::DisplayConfig displayConfig;
std::uint64_t displayRevision = 0;
std::string shaderPath, overlayFile;
bool loaded = false, counted = false;
Clock::time_point lastFrame{};
double elapsed = 0, sessionElapsed = 0;
fs::path legacyState(int index);
bool present(const fs::path& path);

void reload() {
    Json revisions;
    coreOptions = emulation::resolveConfig(root, core, {}, revisions, false);
    gameOptions = emulation::resolveConfig(root, core, uuid, revisions);
    gameOverrides = launchingGame() ? emulation::readJson(emulation::coreGameConfigFile(core,uuid)) : Json::object();
    displayConfig = emulation::resolveDisplay(core,uuid);
    shaderPath = emulation::displayAssetPath(core,root,"shaders",displayConfig.values.at("shaderPreset")).string();
    overlayFile = emulation::displayAssetPath(core,root,"overlays",displayConfig.values.at("overlay")).string();
    ++displayRevision;
}

std::string displayScope() { return displayConfig.independent ? uuid : std::string{}; }

void migrateSharedGameFiles() {
    const auto legacy = profileDirectory/"cheats/mgba"/uuid;
    const auto target = emulation::libraryCheatFile(root,system,uuid);
    fs::create_directories(dataRoot/"cache/cheats"/system);
    if (!emulation::insideDirectory(legacy,profileDirectory)) return;
    const auto oldDisplay = legacy/"game.display.json";
    if (present(oldDisplay) || present(oldDisplay.string()+".bak"))
        emulation::migrateGameDisplay(core,uuid,oldDisplay);
    const auto oldCheats = legacy/"game.cheats";
    if (!fs::exists(target) && present(oldCheats)) {
        fs::create_directories(target.parent_path());
        fs::copy_file(oldCheats,target,fs::copy_options::skip_existing);
    }
}

std::string text(const Json& value) {
    return value.is_string() ? value.get<std::string>() : value.dump();
}

bool present(const fs::path& path) {
    return emulation::insideDirectory(path, profileDirectory) && fs::is_regular_file(fs::symlink_status(path));
}

void flushStats() {
    if (!counted || elapsed <= 0) return;
    emulation::addProfilePlayTime(root, launch->request, elapsed);
    elapsed = 0;
}
}

bool active() { return launch.has_value(); }
bool launchingGame() { return !uuid.empty(); }
std::string platform() { return system; }
std::string rom() { return romPath; }
std::string title() { return gameTitle; }
std::string resumePolicy() {
    const auto mode = gameOverrides.find("resume_on_launch");
    return mode != gameOverrides.end() && *mode == "always" ? "always" : "never";
}
const emulation::Module& module() { return core; }
const Json& theme() { return themeSnapshot; }

bool initialize(int argc, char**) {
    if (argc != 1) return false;
#ifdef __SWITCH__
    root = "sdmc:/WoPlay";
    constexpr auto target = "switch";
#else
    constexpr auto target = "macos";
    const char* selected = std::getenv("WOPLAY_DATA_ROOT");
    if (!selected || !*selected) return false;
    root = selected;
#endif
    const auto file = root / "launch/session.bin";
    if (!fs::exists(file)) return false;
    auto current = emulation::readSession(file);
    const auto& request = current.request;
    const auto where = request.find("host");
    if (where == request.end() || !where->is_object() || where->value("core", "") != "mgba")
        return false;
    launch = std::move(current);
    const auto& saved = launch->request;
    const auto& host = saved.at("host");
    if (host.at("version") != 1) throw std::runtime_error("Unsupported WoPlay core session");
    system = saved.at("platform");
    profile = saved.at("profileId");
    profileDirectory = emulation::profileDirectory(root, profile);
    if (!fs::is_regular_file(profileDirectory / "profile.json"))
        throw std::runtime_error("WoPlay player profile is missing");
    const auto player = emulation::readJson(profileDirectory / "profile.json");
    if (player.contains("avatar") && player["avatar"].is_string()) {
        const auto candidate = profileDirectory / player["avatar"].get<std::string>();
        if (present(candidate)) avatarPath = candidate.string();
    }
    moduleDirectory = root / "modules/mgba";
    core = emulation::readModule(moduleDirectory, target);
    dataRoot = root / "system/mgba";
    themeSnapshot = host.value("ui", Json::object());
    if (saved.at("enter") == "game") {
        uuid = saved.at("uuid");
        romPath = host.at("rom");
        gameTitle = host.at("title");
        iconPath = host.value("icon", std::string{});
        if (!fs::is_regular_file(romPath) || !emulation::supports(core, system, romPath))
            throw std::runtime_error("WoPlay ROM is missing or unsupported");
        if (gameTitle.empty()) gameTitle = fs::path(romPath).stem().string();
        storage = emulation::resolveGameStorage(root, system, uuid, core.id, profile);
        fs::create_directories(storage.savepath);
        fs::create_directories(storage.stateWritePath);
        try {
            StateStore store;
            store.open(storage.stateWritePath, legacyState);
            states = std::move(store);
        } catch (const std::exception& error) {
            stateError = error.what();
            std::fprintf(stderr, "WoPlay save states: %s\n", error.what());
        }
        try { migrateSharedGameFiles(); }
        catch (const std::exception& error) { std::fprintf(stderr,"WoPlay game settings migration: %s\n",error.what()); }
        if (storage.stateFallback)
            emulation::writeCoreGameStorage(root, system, uuid, core.id,
                {{"statePath", storage.stateWritePath.string()}}, profile);
    }
    fs::create_directories(dataRoot / "cache");
    reload();
    return true;
}

std::string directory(const char* name) {
    const std::string_view kind(name);
    if (kind == "save") return storage.savepath.string() + "/";
    if (kind == "state") return storage.stateWritePath.string() + "/";
    if (kind == "cache") return (dataRoot / "cache").string();
    if (kind == "module") return moduleDirectory.string();
    if (kind == "overlays") return (root / "overlays").string();
    if (kind == "shaders") return (root / "shaders").string();
    if (kind == "root") return root.string();
    return dataRoot.string() + "/";
}

std::string batteryFile(const char* extension, bool loading) {
    const auto file = storage.savepath / (std::string("save") + extension);
    if (!emulation::insideDirectory(file, profileDirectory))
        throw std::runtime_error("Invalid WoPlay save path");
    if (!loading || present(file)) return file.string();
    const auto named = storage.savepath / (fs::path(romPath).stem().string() + extension);
    if (present(named)) return named.string();
    fs::path candidate;
    for (const auto& entry : fs::directory_iterator(storage.savepath)) {
        const auto ext = entry.path().extension();
        if ((ext != extension && (std::string_view(extension) != ".srm" || ext != ".sav")) || !present(entry.path()))
            continue;
        if (!candidate.empty()) return file.string();
        candidate = entry.path();
    }
    return candidate.empty() ? file.string() : candidate.string();
}

namespace {
fs::path legacyState(int index) {
    const auto filename = "slot-" + std::to_string(index + 1) + ".state";
    const auto file = storage.stateWritePath / filename;
    if (!emulation::insideDirectory(file, profileDirectory))
        throw std::runtime_error("Invalid WoPlay state path");
    if (present(file)) return file;
    const auto oldSuffix = ".state" + std::to_string(index);
    const auto oldName = fs::path(romPath).stem().string() + oldSuffix;
    for (const auto& dir : {storage.stateWritePath, storage.savepath}) {
        for (const auto& name : {filename, oldName})
            if (present(dir / name)) return dir / name;
    }
    return {};
}

StateStore& stateStore() {
    if (!states) throw std::runtime_error(stateError.empty() ? "Save states unavailable" : stateError);
    return *states;
}
}

const StateStore& savedStates() { return stateStore(); }
std::string stateFile(StateGroup group, int slot) {
    return stateStore().entry(group, group == StateGroup::Cycle ? 0 : slot).file.string();
}
int saveState(StateGroup group, int slot, const StateStore::Writer& write) {
    return stateStore().save(group, slot, write);
}
void deleteState(StateGroup group, int slot) { stateStore().erase(group, slot); }
void expandStates(StateGroup group) { stateStore().expand(group); }
std::string gameIcon() { return iconPath; }
std::string playerAvatar() { return avatarPath; }
std::uint64_t sessionSeconds() { return static_cast<std::uint64_t>(sessionElapsed); }

std::string cheatsFile(const char* extension) {
    if (std::string_view(extension) == ".cheats") return emulation::libraryCheatFile(root,system,uuid).string();
    return (dataRoot/"cache/cheats"/system/(uuid+extension)).string();
}

Json setting(const std::string& key, bool game) {
    const auto& options = game ? gameOptions : coreOptions;
    return options.value(key, Json{});
}

bool overrides(const std::string& key) {
    return gameOverrides.contains(key);
}

std::string option(std::string_view key, std::string_view fallback) {
    const auto found = gameOptions.find(std::string(key));
    return found == gameOptions.end() ? std::string(fallback) : text(*found);
}

std::string optionJson(std::string_view key) {
    if (key == "shader_preset") return Json(shaderPath).dump();
    if (key == "shader_parameters") return displayConfig.values.at("shaderParameters").dump();
    return {};
}

void setOptionJson(const std::string& key, const std::string& value) {
    try {
        const auto parsed = Json::parse(value);
        if (key == "resume_on_launch" && launchingGame()) {
            emulation::updateCoreConfig(core,uuid,[&](Json& next) { next[key] = parsed; });
        } else if (key == "shader_preset") {
            emulation::saveDisplayValue(core,displayScope(),"shaderPreset",
                emulation::displayAssetReference(core,root,"shaders",parsed.get<std::string>()));
        } else if (key == "shader_parameters") {
            emulation::saveDisplayValue(core,displayScope(),"shaderParameters",parsed);
        }
        reload();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "WoPlay display settings: %s\n", error.what());
    }
}

void changeSetting(const std::string& key, const Json& value, bool game) {
    if (game && !launchingGame()) throw std::runtime_error("No active game for this setting");
    emulation::saveOption(core, key, value, game ? uuid : std::string{});
    reload();
}

Json displaySetting(const std::string& key) {
    const auto& prefs = displayConfig.values;
    if (key == "integer") return std::clamp(prefs.value(key,0), 0, 8);
    if (key == "offsetX") return std::clamp(prefs.value(key,0.F), -1280.F,1280.F);
    if (key == "offsetY") return std::clamp(prefs.value(key,0.F), -720.F,720.F);
    if (key == "scale") return std::clamp(prefs.value(key,1.F), .25F,4.F);
    if (key == "customFrame") return prefs.value(key,Json::object());
    if (key == "keepAspect") return prefs.value("customFrame",Json::object()).value(key,true);
    if (key == "overlayEnabled") return prefs.value(key,false);
    if (key == "shaderEnabled") return prefs.value(key,true);
    if (key == "overlay") return overlayFile;
    if (prefs.contains(key)) return prefs.at(key);
    return prefs.value(key,std::string{});
}

void changeDisplay(const std::string& key, const Json& value) {
    const auto stored = key == "overlay" && !value.is_null() ?
        Json(emulation::displayAssetReference(core,root,"overlays",value.get<std::string>())) : value;
    emulation::saveDisplayValue(core,displayScope(),key,stored);
    reload();
}

const emulation::DisplayConfig& displayPreferences() { return displayConfig; }
const std::string& configuredShaderPath() { return shaderPath; }
void changeDisplayScope(bool independent) { emulation::saveDisplayScope(core,uuid,independent); reload(); }
void chooseDisplayPreset(const std::string& id) { emulation::selectDisplayPreset(core,uuid,id); reload(); }
std::string shaderPreferenceKey() { return displayConfig.values.value("shaderPreset",std::string{}); }
std::uint64_t displayConfigRevision() { return displayRevision; }

DisplayGeometry displayGeometry() {
    DisplayGeometry geometry{displaySetting("mode"),displaySetting("integer"),displaySetting("offsetX"),
                             displaySetting("offsetY"),displaySetting("scale")};
    if (geometry.mode == "custom") {
        const auto frame = displaySetting("customFrame");
        const auto width = std::clamp(frame.value("width",0.F),0.F,1.F);
        const auto height = std::clamp(frame.value("height",0.F),0.F,1.F);
        geometry.customFrame = {std::clamp(frame.value("x",0.F),0.F,1.F-width),
                                std::clamp(frame.value("y",0.F),0.F,1.F-height),width,height};
    }
    return geometry;
}

bool shaderEnabled() { return displaySetting("shaderEnabled").get<bool>(); }
std::string overlayPath() {
    if (!displaySetting("overlayEnabled").get<bool>()) return {};
    return displaySetting("overlay");
}

std::vector<std::pair<std::string, std::string>> nativeOptions(bool running) {
    std::vector<std::pair<std::string, std::string>> values;
    for (const auto& definition : core.options)
        if (definition.key.rfind("mgba_", 0) == 0 && (!running || !definition.definition.value("restart", false)))
            values.emplace_back(definition.key, option(definition.key));
    return values;
}

void beginGame() {
    loaded = launchingGame();
    lastFrame = {};
}

void frame() {
    if (!loaded) return;
    try {
        if (!counted) {
            emulation::markProfileGameLoaded(root, launch->request);
            counted = true;
        }
        const auto now = Clock::now();
        if (lastFrame != Clock::time_point{}) {
            const auto interval = std::min(.25, std::chrono::duration<double>(now - lastFrame).count());
            elapsed += interval;
            sessionElapsed += interval;
        }
        lastFrame = now;
        if (elapsed >= 15.) flushStats();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "WoPlay player statistics: %s\n", error.what());
        loaded = false;
    }
}

void pause() { lastFrame = {}; }

void finish(const std::string& error) {
    if (!launch) return;
    try {
        flushStats();
        if (!error.empty()) emulation::status(*launch, "failed", 0, 0, error);
    } catch (const std::exception& failure) {
        std::fprintf(stderr, "WoPlay session close: %s\n", failure.what());
    }
#ifdef __SWITCH__
    const auto program = launch->request.value("returnProgram", std::string{});
    if (launch->request.value("return", false) && !program.empty()) {
        const auto args = "\"" + program + "\"";
        envSetNextLoad(program.c_str(), args.c_str());
    }
#endif
}
}
