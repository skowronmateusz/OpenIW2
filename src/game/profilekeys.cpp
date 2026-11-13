#include "game/profilekeys.h"

#include <cctype>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>

namespace iw2::game {

namespace {

std::optional<std::string> readFile(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(f), {});
}

// The bindings of a table in slot order, from its `unbindall` + `bind KEY "cmd"` text.
std::vector<std::pair<std::string, std::string>> bindList(const ClientInput& t) {
    std::vector<std::pair<std::string, std::string>> out;
    std::istringstream in(t.formatBindings());
    for (std::string line; std::getline(in, line);) {
        if (line.rfind("bind ", 0) != 0) continue;
        const size_t sp = line.find(' ', 5), q = line.find('"', sp);
        if (sp == std::string::npos || q == std::string::npos || line.size() < q + 2) continue;
        out.emplace_back(line.substr(5, sp - 5), line.substr(q + 1, line.size() - q - 2));
    }
    return out;
}

std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Binds `key` to `command` when the command has no key and the key is free.
void addIfFree(ClientInput& in, const std::string& key, const std::string& command) {
    if (!in.keysFor(command).empty() || !in.commandFromKey(key).empty()) return;
    in.loadConfig("bind " + key + " \"" + command + "\"\n");
}

} // namespace

void ProfileKeys::loadDefaults(const ReadGameFile& gameFile) {
    spDefaults_.clear();
    mpOnly_.clear();
    ClientInput sp, mp;
    if (gameFile) {
        if (const auto d = gameFile(kSpDefaults)) spDefaults_ = *d;
        if (const auto d = gameFile(kMpDefaults)) mp.loadConfig(*d);
    }
    sp.loadConfig(spDefaults_);
    for (const auto& [key, command] : bindList(mp)) {
        std::string spCommand = sp.commandFromKey(key);
        if (lower(spCommand) == lower(command)) continue; // the same binding in both files
        mpOnly_.push_back({key, command, !sp.keysFor(command).empty()});
    }
}

void ProfileKeys::setDefaults(ClientInput& in) const {
    in.loadConfig("unbindall\n"); // the shipped default.cfg starts with unbindall too; a missing file leaves no stale keys
    in.loadConfig(spDefaults_);
    addMpOnlyDefaults(in);
}

void ProfileKeys::load(ClientInput& in, const std::filesystem::path& dir) const {
    setDefaults(in);
    if (const auto p = readFile(dir / kConfig)) in.loadConfig(*p);
}

void ProfileKeys::addMpOnlyDefaults(ClientInput& in) const {
    for (const MpDefault& d : mpOnly_) {
        if (!in.commandFromKey(d.key).empty()) continue; // a user's binding stays
        const size_t keys = in.keysFor(d.command).size();
        // A command without a key gets its key; one that default.cfg already binds gets this one as its second key (F4 +scores
        // besides TAB) while it has a free slot.
        if (keys == 0 || (d.secondKey && keys < 2)) in.loadConfig("bind " + d.key + " \"" + d.command + "\"\n");
    }
}

void ProfileKeys::mergeProfile(ClientInput& in, const std::filesystem::path& dir) const {
    if (const auto p = readFile(dir / kMpConfig)) {
        ClientInput mp;
        mp.loadConfig(*p);
        for (const auto& [key, command] : bindList(mp)) addIfFree(in, key, command);
    }
    addMpOnlyDefaults(in);
}

bool ProfileKeys::save(const ClientInput& in, const std::filesystem::path& dir, const std::string& cvarText) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream f(dir / kConfig);
    return f && (f << cvarText << in.formatBindings());
}

} // namespace iw2::game
