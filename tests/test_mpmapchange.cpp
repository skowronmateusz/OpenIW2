// A map change from the dedicated server's console with players connected (docs/spec/net.md section 10 "Map change",
// docs/spec/dedicated.md section 1 `map`): two network clients play mp_toujane dm with bots (10 from --bots and 40
// more from `addtestclient`), the console types `map mp_carentan`. Both clients must stay connected (no drop of any
// reason), get a new gamestate naming mp_carentan, join again through the gametype's menus and spawn at one of
// mp_carentan's deathmatch spawn points. Every spawn a client sees is checked against the spawn points of the map its
// gamestate names, so a player of one level shown with the other level's spawn (under the ground) fails too.
// Needs IW2_MAIN (and IW2_NAV_CACHE for the bots' graph).
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "assets/d3dbsp.h"
#include "fs/vfs.h"
#include "net/gameclient.h"
#include "net/oob.h"
#include "netgame/dedicated.h"
#include "netgame/netmove.h"

using namespace iw2;

namespace {

int failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                             \
        }                                                                           \
    } while (0)

int64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

struct Console {
    std::mutex m;
    std::deque<std::string> lines;
    void push(const std::string& s) {
        std::lock_guard<std::mutex> l(m);
        lines.push_back(s);
    }
    std::vector<std::string> take() {
        std::lock_guard<std::mutex> l(m);
        std::vector<std::string> out(lines.begin(), lines.end());
        lines.clear();
        return out;
    }
};

struct Spawn {
    std::string map;
    float origin[3];
};

// The deathmatch spawn points of a map (its entity list).
std::vector<std::array<float, 3>> dmSpawns(fs::Vfs& vfs, const std::string& map) {
    std::vector<std::array<float, 3>> out;
    const auto bytes = vfs.read("maps/mp/" + map + ".d3dbsp");
    if (!bytes) return out;
    const assets::D3dBsp bsp(*bytes);
    for (const auto& e : bsp.entities()) {
        const std::string* cls = e.get("classname");
        const std::string* o = e.get("origin");
        if (!cls || !o || *cls != "mp_dm_spawn") continue;
        std::array<float, 3> v{};
        std::istringstream in(*o);
        in >> v[0] >> v[1] >> v[2];
        out.push_back(v);
    }
    return out;
}

// A spawn at a spawn point: the same x and y, the height at the point or below it (the gametype drops its spawn points
// to the ground).
bool atSpawnPoint(const float (&o)[3], const std::vector<std::array<float, 3>>& points) {
    for (const auto& p : points)
        if (std::fabs(o[0] - p[0]) < 1.0f && std::fabs(o[1] - p[1]) < 1.0f && o[2] <= p[2] + 1.0f && o[2] >= p[2] - 128.0f)
            return true;
    return false;
}

struct Player {
    net::GameClient client;
    bool alive = false;
    int gamestates = 0;
    std::vector<Spawn> spawns;
    std::string map() const {
        auto it = client.configstrings.find(0);
        return it == client.configstrings.end() ? std::string() : net::infoValueForKey(it->second, "mapname");
    }
    // Answers the gametype's menus as a player would (team, then weapon), sends a command, notes each spawn.
    void step(int64_t now) {
        client.poll(now, 0);
        for (const std::string& sc : client.takeServerCommands()) {
            const auto tok = net::tokenize(sc);
            if (tok.size() >= 2 && tok[0] == "t") {
                const auto cs = client.configstrings.find(netgame::kMenuConfigstrings + std::atoi(tok[1].c_str()));
                const std::string name = cs == client.configstrings.end() ? "" : cs->second;
                const std::string resp = name.rfind("team_", 0) == 0 ? "autoassign"
                                         : name == "weapon_german" ? "mp40_mp"
                                         : name.rfind("weapon_", 0) == 0 ? "thompson_mp" : "close";
                client.addReliableCommand("mr " + std::to_string(client.serverId) + " " + tok[1] + " " + resp);
            }
        }
        if (client.gamestates != gamestates) {
            gamestates = client.gamestates;
            alive = false;
        }
        if (client.state() < net::ClientState::Primed) return;
        client.sendCommand(netgame::makeUserCmd(client.commandTime(now), 0u, 0, 0, 0, 0, 0), now);
        const net::ClientSnapshot* snap = client.latest();
        if (!snap) return;
        const netgame::PsFields f = netgame::readPs(snap->ps);
        // A player state still all zero (the client has not been placed in this level yet) is no spawn.
        const bool placed = f.origin[0] != 0.0f || f.origin[1] != 0.0f || f.origin[2] != 0.0f;
        const bool nowAlive = f.pmType == 0 && placed;
        if (nowAlive && !alive) spawns.push_back({map(), {f.origin[0], f.origin[1], f.origin[2]}});
        alive = nowAlive;
    }
};

} // namespace

int main() {
    const char* mainDir = std::getenv("IW2_MAIN");
    if (!mainDir) {
        std::cout << "IW2_MAIN not set, skipped\n";
        return 0;
    }
    fs::Vfs vfs;
    vfs.mountDirectory(mainDir);
    if (!vfs.exists("maps/mp/mp_toujane.d3dbsp") || !vfs.exists("maps/mp/mp_carentan.d3dbsp")) {
        std::cout << "mp_toujane or mp_carentan missing, skipped\n";
        return 0;
    }
    const auto toujane = dmSpawns(vfs, "mp_toujane"), carentan = dmSpawns(vfs, "mp_carentan");
    CHECK(!toujane.empty() && !carentan.empty());

    Console console;
    std::atomic<int> port{0};
    netgame::DedicatedSettings s;
    s.map = "mp_toujane";
    s.gametype = "dm";
    s.port = 0;
    s.maxClients = 64;
    s.bots = 10;
    s.botSkill = 0;
    s.seconds = 240; // a deadline only: the test quits it
    s.readStdin = false;
    s.cvars = {{"scr_dm_timelimit", "0"}, {"scr_dm_scorelimit", "0"}};
    s.frontEnd = [&] { return console.take(); };
    s.onStarted = [&](int p) { port = p; };
    int rc = -1;
    std::thread server([&] { rc = netgame::runDedicated(vfs, s); });
    for (int64_t end = nowMs() + 120000; nowMs() < end && port == 0;) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(port != 0);
    if (port == 0) {
        console.push("quit");
        server.join();
        return 1;
    }

    net::NetAddress addr;
    net::NetAddress::parse("127.0.0.1:" + std::to_string(port.load()), addr, 28960);
    std::vector<std::unique_ptr<Player>> players;
    for (const char* name : {"alpha", "beta"}) {
        players.push_back(std::make_unique<Player>());
        players.back()->client.name = name;
        players.back()->client.connect(addr, nowMs());
    }
    // Runs both clients at about 60 frames a second until `done` holds or `ms` have passed.
    auto run = [&](int ms, const std::function<bool()>& done) {
        for (int64_t end = nowMs() + ms; nowMs() < end;) {
            for (auto& p : players) p->step(nowMs());
            if (done && done()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        return false;
    };
    auto allOn = [&](const std::string& map) {
        return [&players, map] {
            for (const auto& p : players)
                if (p->client.state() != net::ClientState::Active || p->map() != map || !p->alive) return false;
            return true;
        };
    };
    CHECK(run(30000, allOn("mp_toujane")));
    console.push("addtestclient 40");
    run(3000, {});
    for (const auto& p : players) CHECK(p->client.state() == net::ClientState::Active);
    const int gamestatesBefore = players[0]->client.gamestates;

    console.push("map mp_carentan");
    // The level loads (with a cached bot graph well under a second), then both players join and spawn again.
    CHECK(run(60000, allOn("mp_carentan")));
    for (const auto& p : players) {
        std::cout << p->client.name << ": state " << net::clientStateName(p->client.state()) << ", map " << p->map()
                  << ", gamestates " << p->client.gamestates << ", spawns " << p->spawns.size()
                  << (p->client.dropReason.empty() ? "" : ", dropped: " + p->client.dropReason) << "\n";
        CHECK(p->client.state() == net::ClientState::Active);
        CHECK(p->client.dropReason.empty());
        CHECK(p->client.gamestates > gamestatesBefore);
        bool onCarentan = false;
        for (const Spawn& sp : p->spawns) {
            const bool ok = atSpawnPoint(sp.origin, sp.map == "mp_carentan" ? carentan : toujane);
            if (!ok)
                std::cerr << p->client.name << ": spawn on " << sp.map << " at " << sp.origin[0] << " " << sp.origin[1] << " "
                          << sp.origin[2] << " is no spawn point of that map\n";
            CHECK(ok);
            onCarentan |= sp.map == "mp_carentan";
        }
        CHECK(onCarentan);
    }
    // They keep playing on the new level.
    run(3000, {});
    for (const auto& p : players) CHECK(p->client.state() == net::ClientState::Active && p->map() == "mp_carentan");

    console.push("quit");
    run(2000, [&] {
        for (const auto& p : players)
            if (p->client.state() != net::ClientState::Disconnected) return false;
        return true;
    });
    server.join();
    CHECK(rc == 0);
    std::cout << (failures ? "FAILED" : "OK") << " (" << failures << " failures)\n";
    return failures ? 1 : 0;
}
