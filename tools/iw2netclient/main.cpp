// iw2netclient: a minimal test client of the patch 1.3 protocol (docs/spec/net.md).
//   iw2netclient host[:port] [seconds] [-v] [-f] [-s] [--dump file]     -v: print packets, -f: send forward movement,
//                                                     -c: send a reliable client command, -s: the standard
//                                                     scenario (team, weapon, walk, fire), --dump: decoded stream
// Connects without authorisation, receives the gamestate, sends user commands and decodes snapshots; prints a
// summary. Used to check our coders against a server.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "net/client.h"

using namespace iw2::net;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: iw2netclient host[:port] [seconds] [-v] [-f] [-s] [-c command] [--at ms command] [--until-map map] [--dump file]\n");
        return 1;
    }
    NetAddress to;
    if (!NetAddress::parse(argv[1], to, 28960)) {
        std::fprintf(stderr, "bad address %s\n", argv[1]);
        return 1;
    }
    int seconds = argc >= 3 ? std::atoi(argv[2]) : 5;
    TestClient client;
    std::string untilMap;
    for (int i = 3; i < argc; ++i) {
        if (std::strcmp(argv[i], "-v") == 0) client.verbose = true;
        if (std::strcmp(argv[i], "-f") == 0) client.forwardMove = 127;
        if (std::strcmp(argv[i], "-c") == 0 && i + 1 < argc) client.addReliableCommand(argv[++i]);
        if (std::strcmp(argv[i], "-s") == 0) client.addStandardScenario();
        if (std::strcmp(argv[i], "--buttons-at") == 0 && i + 2 < argc) {
            client.script.push_back({std::atoi(argv[i + 1]), "", 0, int(std::strtol(argv[i + 2], nullptr, 0))});
            i += 2;
        }
        if (std::strcmp(argv[i], "--at") == 0 && i + 2 < argc) {
            client.script.push_back({std::atoi(argv[i + 1]), argv[i + 2], 0, 0});
            i += 2;
        }
        if (std::strcmp(argv[i], "--until-map") == 0 && i + 1 < argc) untilMap = argv[++i];
        if (std::strcmp(argv[i], "--dump") == 0 && i + 1 < argc) {
            client.dump = std::fopen(argv[++i], "w");
            if (!client.dump) {
                std::fprintf(stderr, "cannot write %s\n", argv[i]);
                return 1;
            }
        }
    }
    // With --until-map the server may still be loading (e.g. generating bot navigation), so connecting is retried
    // within the time budget.
    bool connected = client.connect(to, 3000);
    for (int t = 3; !connected && !untilMap.empty() && t < seconds; t += 3) connected = client.connect(to, 3000);
    if (!connected) {
        std::fprintf(stderr, "connect failed: %s\n", client.error.c_str());
        return 2;
    }
    if (untilMap.empty()) {
        client.run(seconds * 1000);
    } else {
        // Stops as soon as a gamestate of the named map arrived (seconds is then only the upper bound).
        const std::string key = "\\mapname\\" + untilMap + "\\";
        for (int waited = 0; waited < seconds * 1000; waited += 100) {
            client.run(100);
            const auto it = client.configstrings.find(0);
            if (it != client.configstrings.end() && it->second.find(key) != std::string::npos) break;
        }
    }
    if (client.dump) std::fclose(client.dump);
    std::printf("%s", client.summary().c_str());
    return client.ok() ? 0 : 3;
}
