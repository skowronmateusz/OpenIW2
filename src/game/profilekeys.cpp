#include "game/profilekeys.h"

#include <fstream>
#include <iterator>
#include <system_error>

namespace iw2::game {

namespace {

std::optional<std::string> readFile(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(f), {});
}

// The defaults, then the profile file (SP 0x42e170, MP 0x4343a0): both shipped files start with unbindall; the table is cleared
// first all the same, so a missing default file leaves no stale keys.
void build(ClientInput& in, const ProfileKeys::ReadGameFile& gameFile, const char* defaults, const std::filesystem::path& cfg) {
    in.loadConfig("unbindall\n");
    if (gameFile)
        if (const auto d = gameFile(defaults)) in.loadConfig(*d);
    if (const auto p = readFile(cfg)) in.loadConfig(*p);
}

} // namespace

void ProfileKeys::load(ClientInput& in, const ReadGameFile& gameFile, const std::filesystem::path& dir, bool multiplayer) {
    build(in, gameFile, multiplayer ? kMpDefaults : kSpDefaults, dir / (multiplayer ? kMpConfig : kSpConfig));
    ClientInput other;
    build(other, gameFile, multiplayer ? kSpDefaults : kMpDefaults, dir / (multiplayer ? kSpConfig : kMpConfig));
    other_ = other.formatBindings();
    mp_ = multiplayer;
    mpEntered_ = multiplayer;
}

void ProfileKeys::setMultiplayer(ClientInput& in, bool multiplayer) {
    if (multiplayer == mp_) return;
    std::string running = in.formatBindings();
    in.loadConfig(other_); // starts with unbindall
    other_ = std::move(running);
    mp_ = multiplayer;
    mpEntered_ = mpEntered_ || multiplayer;
}

void ProfileKeys::editSp(ClientInput& in, const std::function<void(ClientInput&)>& edit) {
    if (!mp_) {
        edit(in);
        return;
    }
    ClientInput sp;
    sp.loadConfig(other_);
    edit(sp);
    other_ = sp.formatBindings();
}

std::string ProfileKeys::bindings(const ClientInput& in, bool multiplayer) const {
    return multiplayer == mp_ ? in.formatBindings() : other_;
}

bool ProfileKeys::save(const ClientInput& in, const std::filesystem::path& dir, const std::string& cvarText) const {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    bool ok = false;
    if (std::ofstream sp(dir / kSpConfig); sp) ok = bool(sp << cvarText << bindings(in, false));
    if (mpEntered_) {
        std::ofstream mp(dir / kMpConfig);
        ok = mp && (mp << bindings(in, true)) && ok;
    }
    return ok;
}

} // namespace iw2::game
