// The key table of a player profile (docs/spec/controls.md "Profile key tables"). The original keeps two tables in two programs
// (default.cfg + config.cfg for single player, default_mp.cfg + config_mp.cfg for multiplayer); openiw2 keeps one table for both
// modes (a deviation): the unified defaults are default.cfg plus the multiplayer-only bindings of default_mp.cfg, the profile's
// config.cfg holds the user's table, and nothing but the menus and console text changes it (no level load, join or restart).
#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "game/clientinput.h"

namespace iw2::game {

class ProfileKeys {
public:
    // A shipped file of the game data (default.cfg, default_mp.cfg); nullopt when it cannot be read.
    using ReadGameFile = std::function<std::optional<std::string>(const std::string& name)>;
    static constexpr const char* kSpDefaults = "default.cfg";
    static constexpr const char* kMpDefaults = "default_mp.cfg";
    static constexpr const char* kConfig = "config.cfg";
    static constexpr const char* kMpConfig = "config_mp.cfg"; // written by openiw2 builds with two tables; read once to merge

    // Reads the shipped defaults. The multiplayer-only bindings are the binds of default_mp.cfg that default.cfg does not have
    // (chat, quick messages, voice, votes, the weapon menu, F4 scoreboard, ...), in file order.
    void loadDefaults(const ReadGameFile& gameFile);
    // The unified defaults alone (the controls reset: either shipped default file gives this table).
    void setDefaults(ClientInput& in) const;
    // The table of a profile folder (start, profile load): the unified defaults, then the lines of its config.cfg.
    void load(ClientInput& in, const std::filesystem::path& dir) const;
    // Adds each multiplayer-only default whose key is free, when its command has no key (or, for a command default.cfg binds too,
    // has a free second slot); a user's binding is never replaced. Part of the defaults, and run once per profile (iw2_keysver 3).
    void addMpOnlyDefaults(ClientInput& in) const;
    // One-time merge of a profile written with two tables: config.cfg (already in `in`) wins; from config_mp.cfg only a binding
    // whose command has no key in `in` and whose key is free is taken. Then the multiplayer-only defaults the same way.
    void mergeProfile(ClientInput& in, const std::filesystem::path& dir) const;
    // Writes config.cfg = `cvarText` + the table. False when it cannot be written.
    static bool save(const ClientInput& in, const std::filesystem::path& dir, const std::string& cvarText);

    struct MpDefault {
        std::string key, command;
        bool secondKey = false; // default.cfg binds the command to another key
    };
    const std::vector<MpDefault>& mpOnly() const { return mpOnly_; }

private:
    std::string spDefaults_;
    std::vector<MpDefault> mpOnly_;
};

} // namespace iw2::game
