// The dedicated server loop (docs/spec/dedicated.md): the MP World behind the protocol core with the server console
// (stdin, rcon, an optional front end), map rotation and quit. Shared by tools/iw2server and by iw2view, which turns
// into a dedicated server when the create-server screen starts one with the dedicated choice (docs/spec/mp-ui.md).
#pragma once

#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace iw2::fs {
class Vfs;
}

namespace iw2::netgame {

struct DedicatedSettings {
    std::string map;       // first map (empty: the start-up commands must load one, e.g. +map_rotate)
    std::string gametype;  // g_gametype (empty: from the configuration, else dm)
    int port = -1;         // UDP port (-1: net_port, default 28960; 0: any free port)
    int maxClients = -1;   // sv_maxclients (-1: the cvar, default 8)
    int seconds = 0;       // stop after this many seconds (0: until quit)
    int bots = 0;          // bots added on every map
    int botSkill = 3;
    bool readStdin = true; // console lines from stdin
    // Cvars set after the default configuration and before the start-up commands (the menu's or --cvar values).
    std::vector<std::pair<std::string, std::string>> cvars;
    // Cvars set after the start-up commands that come before the first map command (so after +exec server.cfg): the
    // command line wins over the configuration (--weapons).
    std::vector<std::pair<std::string, std::string>> overrideCvars;
    // Score, time (minutes) and round limits of the gametype (--scorelimit, --timelimit, --roundlimit; -1: not given,
    // the configuration's scr_<gametype>_* stays). Set as overrides for the gametype that is final at that point.
    int scoreLimit = -1;
    double timeLimit = -1;
    int roundLimit = -1;
    // Console commands run at start-up in order (+set, +exec, +map_rotate, ...).
    std::vector<std::string> startup;
    // Front end (optional): called once per server frame; returns the console lines typed since the last call.
    std::function<std::vector<std::string>()> frontEnd;
    // Called once the first map is loaded with the bound UDP port (optional).
    std::function<void(int port)> onStarted;
};

// Runs the server until quit (console, rcon, SIGINT / SIGTERM) or the end of `seconds`. Returns the exit code
// (0 after quit, 1 when no map could be loaded or the port could not be bound).
int runDedicated(fs::Vfs& vfs, const DedicatedSettings& settings);

// The create-server screen's start (docs/spec/mp-ui.md "Dedicated start"): whether StartServer's `dedicated` value
// (from ui_dedicated: 0 no, 1 LAN, 2 internet) turns the client into a dedicated server, ...
bool menuStartIsDedicated(const std::string& dedicatedValue);
// ... and the server's settings: the map of StartServer's console text ("wait ; wait ; map <map>"), g_gametype, the
// menu's cvars (host name, password, slots, weapon classes, gametype settings), the bots of the extension panel and
// the port (`port`: the listen server's rule, IW2_LISTEN_PORT or net_port or 28960). No stdin console.
DedicatedSettings dedicatedFromMenu(const std::map<std::string, std::string>& menuCvars, const std::string& startCommand,
                                    int bots, int botSkill, int port);

} // namespace iw2::netgame
