#include "state_store.hpp"
#include "TicoSafeFile.h"
#include "woplay/emulation/local_profiles.hpp"
#include <ctime>
#include <limits>
#include <set>
#include <stdexcept>
#include <sys/stat.h>

namespace woplay_host {
namespace fs = std::filesystem;
namespace emulation = gbastation::emulation;
using emulation::Json;
namespace {
constexpr int defaultCapacity = 10;
const char* key(StateGroup group) {
    switch (group) {
    case StateGroup::Cycle: return "cycle";
    case StateGroup::Manual: return "manual";
    case StateGroup::Resume: return "resume";
    }
    throw std::logic_error("Invalid save group");
}

bool validId(const std::string& id) {
    return id.size() == 36 && id.find_first_not_of("0123456789abcdef-") == std::string::npos;
}

void copyLegacy(const fs::path& source, const fs::path& target) {
    std::vector<unsigned char> data;
    if (!TicoSafeFile::ReadAll(source.string(), data) || data.empty() ||
        !TicoSafeFile::Write(target.string(), data.data(), data.size(), 0))
        throw std::runtime_error("Cannot migrate save state: " + source.string());
}

void flushFile(const fs::path& path) {
    FILE* file = std::fopen(path.string().c_str(), "r+b");
    if (!file) throw std::runtime_error("Cannot open save file: " + path.string());
    const bool synced = fsync(fileno(file)) == 0;
    const bool closed = std::fclose(file) == 0;
    if (!synced || !closed) throw std::runtime_error("Cannot flush save file: " + path.string());
}

void validate(const Json& index) {
    if (index.at("schemaVersion") != 1) throw std::runtime_error("Unsupported save state index");
    for (const auto group : {StateGroup::Cycle, StateGroup::Manual, StateGroup::Resume}) {
        const auto& entries = index.at(key(group));
        if (!entries.is_array() || entries.empty() ||
            entries.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
            throw std::runtime_error("Invalid save state slots");
        for (const auto& entry : entries) {
            if (entry.is_null()) continue;
            if (!validId(entry.at("snapshot").get<std::string>()) || !entry.at("time").is_number_integer() ||
                !entry.at("sequence").is_number_unsigned())
                throw std::runtime_error("Invalid save state entry");
        }
    }
    index.at("sequence").get<std::uint64_t>();
}

std::pair<std::int64_t, std::uint64_t> order(const Json& entry) {
    return {entry.at("time").get<std::int64_t>(), entry.at("sequence").get<std::uint64_t>()};
}
}

void StateStore::open(fs::path directory, const LegacyPath& legacy) {
    root_ = std::move(directory);
    index_ = emulation::readJson(root_ / "index.json");
    const bool fresh = index_.empty();
    if (!fresh) validate(index_);
    Json next = fresh ? Json{{"schemaVersion",1},{"sequence",0u}} : index_;
    if(next.value("slotLayout",0)==0){
        for(const auto group:{StateGroup::Manual,StateGroup::Resume,StateGroup::Cycle}){
            auto& entries=next[key(group)];
            if(fresh)entries=Json::array();
            while(entries.size()<defaultCapacity)entries.push_back(nullptr);
        }
        next["slotLayout"]=1;
    }
    if (!next.contains("legacySlots"))
        next["legacySlots"] = {false,false,false,false,false,false};
    auto sequence = next.at("sequence").get<std::uint64_t>();
    std::vector<fs::path> created;
    try {
        for (int old = 0; old < 6; ++old) {
            if (next.at("legacySlots").at(old).get<bool>()) continue;
            const auto source = legacy(old);
            struct stat info{};
            if (stat(source.string().c_str(), &info) != 0 || !S_ISREG(info.st_mode) || info.st_size <= 0) continue;
            const auto id = emulation::newProfileId();
            const auto snapshot = root_ / "snapshots" / id;
            created.push_back(snapshot);
            if (!emulation::insideDirectory(snapshot,root_))
                throw std::runtime_error("Invalid snapshot directory");
            fs::create_directories(snapshot);
            const auto target = snapshot / "state.bin";
            copyLegacy(source, target);
            for (const auto* suffix : {".png", ".ra"}) {
                const auto sidecar = fs::path(source.string() + suffix);
                if (fs::is_regular_file(fs::symlink_status(sidecar)) && fs::file_size(sidecar) > 0)
                    copyLegacy(sidecar, suffix[1] == 'p' ? snapshot / "preview.png" : snapshot / "state.bin.ra");
            }
            const Json entry{{"snapshot", id}, {"time", static_cast<std::int64_t>(info.st_mtime)},
                             {"sequence", ++sequence}};
            auto& manual = next["manual"];
            if (old == 5 && next["resume"][0].is_null()) next["resume"][0] = entry;
            else if (old < 5 && (old >= static_cast<int>(manual.size()) || manual[old].is_null()))
                manual[old] = entry;
            else {
                auto empty = std::find(manual.begin(),manual.end(),nullptr);
                if (empty == manual.end()) manual.push_back(entry);
                else *empty = entry;
            }
            next["legacySlots"][old] = true;
        }
        next["sequence"] = sequence;
        if (next != index_) commit(std::move(next));
        else removeUnused();
    } catch (...) {
        std::error_code error;
        for (const auto& path : created) fs::remove_all(path, error);
        throw;
    }
}

int StateStore::capacity(StateGroup group) const {
    return static_cast<int>(index_.at(key(group)).size());
}

std::string StateStore::name(StateGroup group,int slot) const {
    const auto groups=index_.find("names");
    if(groups==index_.end()||!groups->is_object())return {};
    const auto names=groups->find(key(group));
    if(names==groups->end()||!names->is_object())return {};
    const auto field=names->find(std::to_string(slot));
    return field!=names->end()&&field->is_string()?field->get<std::string>():std::string{};
}

void StateStore::rename(StateGroup group,int slot,const std::string& name) {
    if(slot<1||slot>capacity(group)||name.size()>255)throw std::runtime_error("Invalid save slot name");
    if(name==this->name(group,slot))return;
    auto next=index_;
    auto& names=next["names"][key(group)];
    if(name.empty())names.erase(std::to_string(slot));
    else names[std::to_string(slot)]=name;
    commit(std::move(next));
}

SavedState StateStore::entry(StateGroup group, int slot) const {
    const auto& entries = index_.at(key(group));
    if (slot == 0) {
        for (int i = 0; i < static_cast<int>(entries.size()); ++i)
            if (!entries[i].is_null() && (!slot || order(entries[slot - 1]) < order(entries[i])))
                slot = i + 1;
    }
    if (slot <= 0 || slot > static_cast<int>(entries.size()) || entries[slot - 1].is_null()) return {};
    const auto& item = entries[slot - 1];
    const auto path = root_ / "snapshots" / item.at("snapshot").get<std::string>() / "state.bin";
    if (!emulation::insideDirectory(path, root_))
        throw std::runtime_error("Invalid save state path");
    return {path, item.at("time").get<std::int64_t>(), slot};
}

int StateStore::save(StateGroup group, int slot, const Writer& write) {
    auto next = index_;
    auto& entries = next.at(key(group));
    if (slot == 0 && group != StateGroup::Manual) {
        int oldest = 1;
        for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
            if (entries[i].is_null()) {
                slot = i + 1;
                break;
            }
            if (order(entries[i]) < order(entries[oldest - 1])) oldest = i + 1;
        }
        if (!slot) slot = oldest;
    }
    if (slot < 1 || slot > static_cast<int>(entries.size()))
        throw std::runtime_error("Invalid save slot");

    const auto id = emulation::newProfileId();
    const auto directory = root_ / "snapshots" / id;
    if (!emulation::insideDirectory(directory,root_))
        throw std::runtime_error("Invalid snapshot directory");
    fs::create_directories(directory);
    try {
        const auto file = directory / "state.bin";
        if (!write(file.string())) throw std::runtime_error("Cannot write save state");
        flushFile(file);
        flushFile(directory / "preview.png");
        if (fs::is_regular_file(directory / "state.bin.ra")) flushFile(directory / "state.bin.ra");
        const auto sequence = next.at("sequence").get<std::uint64_t>() + 1;
        next["sequence"] = sequence;
        entries[slot - 1] = {{"snapshot", id}, {"time", static_cast<std::int64_t>(std::time(nullptr))},
                             {"sequence", sequence}};
        commit(std::move(next));
    } catch (...) {
        std::error_code error;
        fs::remove_all(directory, error);
        throw;
    }
    return slot;
}

void StateStore::erase(StateGroup group, int slot) {
    if (!entry(group, slot).slot || slot < 1) return;
    auto next = index_;
    next.at(key(group)).at(slot - 1) = nullptr;
    commit(std::move(next));
}

void StateStore::expand(StateGroup group) {
    auto next = index_;
    next.at(key(group)).push_back(nullptr);
    commit(std::move(next));
}

void StateStore::commit(Json next) {
    const auto file = root_ / "index.json";
    if (emulation::readJson(file) != index_)
        throw std::runtime_error("Save state index changed on disk");
    try {
        emulation::writeJson(file,next);
    } catch (...) {
        if (emulation::readJson(file) != next) throw;
    }
    index_ = std::move(next);
    removeUnused();
}

void StateStore::removeUnused() const {
    // Keep the files referenced by the previous index too so its recovery remains usable.
    try {
        std::set<std::string> live;
        const auto remember = [&](const Json& index) {
            if (index.empty()) return;
            validate(index);
            for (const auto group : {StateGroup::Cycle, StateGroup::Manual, StateGroup::Resume})
                for (const auto& entry : index.at(key(group)))
                    if (!entry.is_null()) live.insert(entry.at("snapshot").get<std::string>());
        };
        remember(index_);
        remember(emulation::readJson(root_ / "index.json.bak"));
        const auto directory = root_ / "snapshots";
        if (!fs::is_directory(directory)) return;
        for (const auto& entry : fs::directory_iterator(directory)) {
            const auto id = entry.path().filename().string();
            if (validId(id) && !live.count(id)) {
                std::error_code error;
                fs::remove_all(entry.path(), error);
            }
        }
    } catch (const std::exception&) {
        // Cleanup is optional. Preserve files when either index cannot be checked.
    }
}
}
