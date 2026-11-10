// The shell shock builtins (precacheshellshock 0x4f35e0, shellshock 0x4f7820, stopshellshock 0x4f79d0).
#include "game/shellshock.h"
#include "game/mpgame.h"
#include "game/world.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace iw2::game {

using namespace script;

namespace {
bool sameName(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}
} // namespace

World::ShellShockState World::shellShock() const {
    if (mp_) {
        const int lc = mp_->localClient;
        if (lc < 0 || lc >= int(mp_->clients.size())) return {};
        // The killcam copies the followed client's archived player state, its shell shock with it (docs/spec/mp-hud.md
        // "What the killcam client draws"): the victim's own shock is not shown in the replay.
        if (ShellShockState k; mpKillcamShellShock(k)) return k;
        const MpClient& cl = mp_->clients[size_t(lc)];
        return {cl.shockIndex, cl.shockStartMs, cl.shockDurationMs};
    }
    return shellShock_;
}

const ShockRecord* World::shellShockRecord() const {
    const int index = shellShock().index;
    if (index <= 0 || index > int(shockRecords_.size())) return nullptr;
    return shockRecords_[size_t(index - 1)].get();
}

int World::precacheShellShock(const std::string& name) {
    for (size_t i = 0; i < shockNames_.size(); ++i)
        if (sameName(shockNames_[i], name)) return int(i) + 1;
    if (shockNames_.size() >= 15) return 0;
    addShellShock(name);
    return int(shockNames_.size());
}

void World::mpSetRemoteShellShock(const std::string& name, int64_t startMs, int durationMs) {
    if (!mp_ || mp_->localClient < 0 || mp_->localClient >= int(mp_->clients.size())) return;
    MpClient& cl = mp_->clients[size_t(mp_->localClient)];
    const int index = name.empty() ? 0 : precacheShellShock(name);
    cl.shockIndex = index;
    cl.shockStartMs = index ? startMs : 0;
    cl.shockDurationMs = index ? durationMs : 0;
}

void World::addShellShock(const std::string& name) {
    ShockRecord rec;
    if (const auto data = vfs_.read("shock/" + name + ".shock"))
        rec = parseShockFile(std::string(data->begin(), data->end()));
    // A missing file is a drop error in the cgame (0x4a3111 -> Com_Error 0x42c7f0 code 1, "couldn't register shell shock '%s'"
    // 0x57e27c); here the dvar defaults stay. Every shipped shock file exists, so the case is not reachable with the game data.
    shockNames_.push_back(name);
    shockRecords_.push_back(std::make_shared<const ShockRecord>(std::move(rec)));
}

void World::registerShellShockBuiltins() {
    // precacheshellshock(name): a config string (base 0x45d, 16 slots); the cgame then reads "shock/<name>.shock"
    // (0x4b1740) into the record table at 0xf56bdc. The original raises a script error when it is called after the level script
    // has waited (flag 0x1c60528; the same rule as precacheItem / precacheMenu). Not reproduced: it can only fire for a script
    // that precaches late, and every shipped script precaches in the load phase. A full table (15 slots) is dropped quietly here.
    vm_.registerBuiltin("precacheshellshock", [this](CallContext& c) {
        precacheShellShock(c.arg(0).toString());
        return Value();
    });
    // shellshock(name, seconds) on the player: stores the name index, the level time and the duration in
    // milliseconds (the client's shellshockIndex / shellshockTime / shellshockDuration, 0x57588c...). The duration is
    // rounded like the exe does; it must lie in 0..60 seconds.
    vm_.registerBuiltin("shellshock", [this](CallContext& c) {
        if (!c.self.isObject()) return Value();
        const Entity* e = entityOf(c.self);
        if (!e || !e->isPlayer) return Value();
        const std::string name = c.arg(0).toString();
        int index = 0;
        for (size_t i = 0; i < shockNames_.size(); ++i)
            if (sameName(shockNames_[i], name)) index = int(i) + 1;
        const int ms = shockMs(c.arg(1).asFloat());
        // 0x4f78f4 / 0x4f7969: the script errors "shellshock '<name>' was not precached" and "duration <seconds> should be >= 0 and <= 60"
        // (the duration printed is the rounded milliseconds * 0.001).
        if (index == 0) throw std::runtime_error("shellshock '" + name + "' was not precached");
        if (ms < 0 || ms > 60000) {
            char buf[64];
            std::snprintf(buf, sizeof buf, "duration %g should be >= 0 and <= 60", double(float(ms) * 0.001f));
            throw std::runtime_error(buf);
        }
        // Multiplayer (1.3 Linux dedicated server 0x81162a2): the state goes into the player state of the client `self`,
        // which sends it to that client only (docs/spec/mp.md "Per-client player state").
        if (mp_) {
            const int cl = mpClientOf(*e);
            if (cl < 0) return Value();
            MpClient& m = mp_->clients[size_t(cl)];
            m.shockIndex = index;
            m.shockStartMs = timeMs_;
            m.shockDurationMs = ms;
            return Value();
        }
        shellShock_ = {index, timeMs_, ms};
        return Value();
    });
    vm_.registerBuiltin("stopshellshock", [this](CallContext& c) {
        if (!c.self.isObject()) return Value();
        const Entity* e = entityOf(c.self);
        if (!e || !e->isPlayer) return Value();
        if (mp_) { // 1.3 Linux dedicated server 0x811642a: clears the three fields of the client `self`
            if (const int cl = mpClientOf(*e); cl >= 0) {
                MpClient& m = mp_->clients[size_t(cl)];
                m.shockIndex = 0;
                m.shockStartMs = 0;
                m.shockDurationMs = 0;
            }
            return Value();
        }
        shellShock_ = {};
        return Value();
    });
}

} // namespace iw2::game
