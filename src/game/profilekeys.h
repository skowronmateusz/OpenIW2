// The two key tables of a player profile (docs/spec/controls.md "Profile key tables"): single player (default.cfg, then
// players/<profile>/config.cfg) and multiplayer (default_mp.cfg, then players/<profile>/config_mp.cfg). The original keeps them
// in two programs; here one of them runs in the client's ClientInput and the other is kept as config text until the client
// switches mode.
#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

#include "game/clientinput.h"

namespace iw2::game {

class ProfileKeys {
public:
    // A shipped file of the game data (default.cfg, default_mp.cfg); nullopt when it cannot be read.
    using ReadGameFile = std::function<std::optional<std::string>(const std::string& name)>;
    static constexpr const char* kSpDefaults = "default.cfg";
    static constexpr const char* kMpDefaults = "default_mp.cfg";
    static constexpr const char* kSpConfig = "config.cfg";
    static constexpr const char* kMpConfig = "config_mp.cfg";

    // Builds both tables for the profile folder `dir` (start, profile load): each is the shipped defaults, then the lines of the
    // profile's file when it exists. The table of `multiplayer` goes into `in`.
    void load(ClientInput& in, const ReadGameFile& gameFile, const std::filesystem::path& dir, bool multiplayer);
    // The running table becomes that of `multiplayer`; the one left is kept as it is. Nothing happens when it already runs.
    void setMultiplayer(ClientInput& in, bool multiplayer);
    bool multiplayer() const { return mp_; }
    // Runs `edit` on the single-player table, whether it runs in `in` or is kept.
    void editSp(ClientInput& in, const std::function<void(ClientInput&)>& edit);
    // `unbindall` + `bind` lines of a table.
    std::string bindings(const ClientInput& in, bool multiplayer) const;
    // Writes the profile into `dir`: config.cfg = `cvarText` + the SP table; config_mp.cfg = the MP table, once the MP table ran
    // since the load (a profile never taken into multiplayer keeps its config_mp.cfg as it is). False when a file cannot be written.
    bool save(const ClientInput& in, const std::filesystem::path& dir, const std::string& cvarText) const;

private:
    bool mp_ = false;
    bool mpEntered_ = false;
    std::string other_; // the table that is not running, as config text
};

} // namespace iw2::game
