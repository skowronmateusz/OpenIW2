// iw2server: headless dedicated server (macOS / Linux). Runs the multiplayer World (docs/spec/mp.md) behind the
// protocol core (src/net/server.cpp, docs/spec/net.md) with the server console of docs/spec/dedicated.md (stdin and
// rcon). No authorisation, no master server, no PunkBuster. Options and their validation: src/netgame/serverargs.cpp,
// documented in docs/dedicated-server.md.
//
//   iw2server <main-dir> [<map> [gametype]] [options] [+command args ...]
//
// The server loop is src/netgame/dedicated.cpp (shared with the dedicated start of iw2view). The game behind the
// protocol core is src/netgame/worldgame.cpp (docs/spec/net.md section 11): network clients map to MP clients, the snapshots carry players, bots, script models, health, weapons, HUD elements, objectives and
// obituaries. TODO(spec): acceptance by the original 1.3 client is untested.

#include <cstdio>
#include <exception>
#include <string>
#include <vector>

#include "fs/gamedir.h"
#include "fs/vfs.h"
#include "netgame/dedicated.h"
#include "netgame/serverargs.h"

int main(int argc, char** argv) {
    iw2::netgame::ServerArgs args;
    if (!iw2::netgame::parseServerArgs(std::vector<std::string>(argv + 1, argv + argc), args)) {
        std::fprintf(stderr, "iw2server: error: %s\n\n%s", args.error.c_str(), iw2::netgame::serverUsage().c_str());
        return 2;
    }
    if (args.help) {
        std::fputs(iw2::netgame::serverUsage().c_str(), stdout);
        return 0;
    }
    if (const std::string e = iw2::fs::mainDirError(args.mainDir); !e.empty()) {
        std::fprintf(stderr, "iw2server: error: %s\n", e.c_str());
        return 1;
    }
    try {
        iw2::fs::Vfs vfs;
        vfs.mountDirectory(args.mainDir);
        return iw2::netgame::runDedicated(vfs, args.settings);
    } catch (const std::exception& e) { // a clear message instead of an abort on any uncaught error
        std::fprintf(stderr, "iw2server: error: %s\n", e.what());
        return 1;
    }
}
