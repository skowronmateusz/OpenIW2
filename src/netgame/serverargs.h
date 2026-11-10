// The command line of iw2server (docs/dedicated-server.md): parsing, validation and the usage text. Kept out of
// tools/iw2server/main.cpp so every option and every error case can be tested without starting a server.
#pragma once

#include <string>
#include <vector>

#include "netgame/dedicated.h"

namespace iw2::netgame {

struct ServerArgs {
    std::string mainDir;       // <main-dir>: the folder with the game's .iwd files
    DedicatedSettings settings;
    bool help = false;         // --help: print serverUsage() and exit 0
    std::string error;         // set when parseServerArgs returns false
};

// The gametypes the server can run (the scripts under maps/mp/gametypes).
const std::vector<std::string>& serverGametypes();

// Parses the arguments after the program name. Returns false and sets `out.error` for an unknown option, a missing or
// malformed value, a value out of range, or an invalid --weapons list.
bool parseServerArgs(const std::vector<std::string>& args, ServerArgs& out);

// Usage text (the options with their ranges).
std::string serverUsage();

// The cvars --scorelimit / --timelimit / --roundlimit set for `gametype` (scr_<gametype>_scorelimit, ...), in order.
// Returns an error text when --roundlimit is given for a gametype other than sd (empty otherwise).
std::string serverLimitCvars(const DedicatedSettings& s, const std::string& gametype,
                             std::vector<std::pair<std::string, std::string>>& out);

} // namespace iw2::netgame
