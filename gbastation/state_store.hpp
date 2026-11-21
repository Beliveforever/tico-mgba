#pragma once

#include "session_host.hpp"
#include "woplay/emulation/session.hpp"
#include <functional>

namespace woplay_host {
struct SavedState {
    std::filesystem::path file;
    std::int64_t savedAt = 0;
    int slot = 0;
};

class StateStore {
public:
    using Writer = std::function<bool(const std::string&)>;
    using LegacyPath = std::function<std::filesystem::path(int)>;

    void open(std::filesystem::path directory, const LegacyPath& legacy);
    int capacity(StateGroup group) const;
    SavedState entry(StateGroup group, int slot = 0) const;
    std::string name(StateGroup group,int slot) const;
    void rename(StateGroup group,int slot,const std::string& name);
    int save(StateGroup group, int slot, const Writer& write);
    void erase(StateGroup group, int slot);
    void expand(StateGroup group);

private:
    std::filesystem::path root_;
    gbastation::emulation::Json index_;
    void commit(gbastation::emulation::Json next);
    void removeUnused() const;
};
}
