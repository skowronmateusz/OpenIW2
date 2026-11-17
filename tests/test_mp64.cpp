// 64 clients (docs/spec/mp.md "sv_maxclients"). Needs IW2_MAIN (and IW2_NAV_CACHE for the bots' graph).
//
// `latch`: the dedicated server loop (src/netgame/dedicated.cpp) on mp_toujane with bots: a `set sv_maxclients` while a
// map runs waits, a fast_restart keeps the slots, the next map load applies it, a smaller count never takes the slot
// of a connected client, and the bots get what is left.
//
// `soak`: a dedicated World with sv_maxclients 64, 63 bots and one network client over loopback, in simulated time.
// The client gets the last client number (63), its snapshots carry the client states of all 64, the scores command
// lists 64 rows, a 65th client is refused, and the game goes on (kills, no script errors). Prints the measurements:
// server CPU per frame, snapshot sizes at 60 snapshots a second, the rate they need, peak memory.
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "fs/vfs.h"
#include "game/world.h"
#include "net/gameclient.h"
#include "net/oob.h"
#include "net/server.h"
#include "net/udp.h"
#include "netgame/dedicated.h"
#include "netgame/worldgame.h"

namespace {

int failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                             \
        }                                                                           \
    } while (0)

using namespace iw2;

double cpuSeconds() {
    rusage u{};
    getrusage(RUSAGE_SELF, &u);
    return double(u.ru_utime.tv_sec + u.ru_stime.tv_sec) + double(u.ru_utime.tv_usec + u.ru_stime.tv_usec) * 1e-6;
}

// ---------------------------------------------------------------------------------------------------------------
// latch

std::string query(int port, const std::string& what) {
    net::UdpSocket sock;
    if (!sock.open(0, 0)) return {};
    net::NetAddress to;
    net::NetAddress::parse("127.0.0.1:" + std::to_string(port), to, 28960);
    auto pkt = net::makeOutOfBand(what + " xxx");
    sock.sendTo(to, pkt.data(), static_cast<int>(pkt.size()));
    static uint8_t buf[0x20000];
    net::NetAddress from;
    const int n = sock.receive(buf, sizeof buf, from, 2000);
    if (n <= 0) return {};
    auto t = net::outOfBandText(buf, n);
    return t ? *t : std::string();
}

std::string infoKey(int port, const std::string& key) {
    const std::string info = query(port, "getinfo");
    const size_t nl = info.find('\n');
    return nl == std::string::npos ? std::string() : net::infoValueForKey(info.substr(nl + 1), key);
}

void latch(const char* mainDir) {
    fs::Vfs vfs;
    vfs.mountDirectory(mainDir);
    netgame::DedicatedSettings s;
    s.map = "mp_toujane";
    s.gametype = "dm";
    s.port = 0;
    s.maxClients = 4;
    s.bots = 3;
    s.botSkill = 0;
    s.readStdin = false;
    s.seconds = 240; // a backstop
    s.cvars = {{"scr_dm_timelimit", "0"}, {"scr_dm_scorelimit", "0"}};
    std::atomic<int> port{0};
    std::mutex m;
    std::vector<std::string> typed;
    s.onStarted = [&](int p) { port = p; };
    s.frontEnd = [&] {
        std::lock_guard<std::mutex> lock(m);
        return std::exchange(typed, {});
    };
    int rc = -1;
    std::thread t([&] { rc = netgame::runDedicated(vfs, s); });
    for (int i = 0; i < 2400 && port == 0; ++i) usleep(50000);
    CHECK(port != 0);
    if (port != 0) {
        CHECK(infoKey(port, "sv_maxclients") == "4" && infoKey(port, "clients") == "3");
        // The client takes the last free slot; the next one finds the server full.
        net::GameClient client;
        client.name = "joiner";
        net::NetAddress to;
        net::NetAddress::parse("127.0.0.1:" + std::to_string(port.load()), to, 28960);
        int64_t now = 1000;
        auto pollFor = [&](int ms) {
            for (int i = 0; i < ms / 20; ++i) {
                client.poll(now += 20, 20);
                if (client.state() >= net::ClientState::Primed) {
                    net::UserCmd c;
                    c.serverTime = client.commandTime(now);
                    client.sendCommand(c, now);
                }
            }
        };
        CHECK(client.connect(to, now));
        for (int i = 0; i < 400 && client.state() < net::ClientState::Active && client.state() != net::ClientState::Disconnected; ++i) pollFor(20);
        CHECK(client.state() == net::ClientState::Active);
        CHECK(infoKey(port, "clients") == "4");
        {
            net::GameClient full;
            full.connect(to, now);
            for (int i = 0; i < 200 && full.state() != net::ClientState::Disconnected && full.state() < net::ClientState::Primed; ++i)
                full.poll(now += 20, 20), pollFor(0);
            CHECK(full.state() == net::ClientState::Disconnected && full.dropReason.find("EXE_SERVERISFULL") != std::string::npos);
        }
        auto type = [&](const std::string& line) {
            std::lock_guard<std::mutex> lock(m);
            typed.push_back(line);
        };
        auto waitFor = [&](const std::string& key, const std::string& value) {
            for (int i = 0; i < 300; ++i) {
                if (infoKey(port, key) == value) return true;
                pollFor(100);
            }
            return false;
        };
        // A change while the map runs waits; a fast_restart of the same gametype keeps the slots.
        const int gamestates = client.gamestates;
        type("set sv_maxclients 6");
        type("fast_restart");
        for (int i = 0; i < 300 && client.gamestates == gamestates; ++i) pollFor(100);
        CHECK(client.gamestates > gamestates);
        pollFor(1000);
        CHECK(infoKey(port, "sv_maxclients") == "4" && infoKey(port, "clients") == "4");
        // The next map load applies it: the client keeps its connection, the three bots come back, two slots stay free.
        type("map mp_toujane");
        CHECK(waitFor("sv_maxclients", "6"));
        pollFor(1500);
        CHECK(infoKey(port, "clients") == "4");
        CHECK(client.state() == net::ClientState::Active);
        // A smaller count: the network client (network slot 0) keeps its slot, one bot fits beside it.
        type("set sv_maxclients 2");
        type("map_restart");
        CHECK(waitFor("sv_maxclients", "2"));
        pollFor(1500);
        CHECK(infoKey(port, "clients") == "2");
        CHECK(client.state() == net::ClientState::Active);
        // Out of range: refused, nothing waits.
        type("set sv_maxclients 65");
        type("map mp_toujane");
        pollFor(3000);
        CHECK(waitFor("sv_maxclients", "2"));
        type("quit");
        for (int i = 0; i < 200 && client.state() != net::ClientState::Disconnected; ++i) pollFor(20);
    }
    t.join();
    CHECK(rc == 0);
}

// ---------------------------------------------------------------------------------------------------------------
// soak

void soak(const char* mainDir, int seconds) {
    fs::Vfs vfs;
    vfs.mountDirectory(mainDir);
    game::World w(vfs, "mp_toujane", true);
    w.vm().setLoopGuardIterations(200000u);
    w.cvars["mapname"] = "mp_toujane";
    w.cvars["sv_maxclients"] = "64";
    w.cvars["scr_dm_timelimit"] = "0";
    w.cvars["scr_dm_scorelimit"] = "0";
    w.cvars["scr_forcerespawn"] = "1"; // the network client stands still; it plays again after each death
    w.mpSetDedicated();
    w.mpStart("dm");
    CHECK(w.mpState().clients.size() == 64);
    (void)w.mpNav();
    w.mpSetBotSkill(1);
    netgame::WorldGame game(nullptr, "", "", 7, 64);
    game.setWorld(&w, "mp_toujane", "dm", 7);
    net::Server server(game, 5);
    server.maxClients = 64;
    server.serverId = 7;
    game.sendCommand = [&server](int slot, const std::string& text) { server.sendServerCommand(slot, text); };
    game.clientPing = [&server](int slot) { return server.client(slot).ping; };
    server.info().mapname = "mp_toujane";
    server.info().gametype = "dm";
    CHECK(server.open(0));
    int bots = 0;
    for (int i = 0; i < 63; ++i) {
        if (w.mpAddBot() >= 0) ++bots;
        w.frame();
    }
    CHECK(bots == 63);
    size_t inUse = 0;
    for (const auto& e : w.entities()) inUse += e && !e->obj->removed;
    std::printf("mp64 soak: %d bots, %zu entities in use\n", bots, inUse);

    net::NetAddress to;
    net::NetAddress::parse("127.0.0.1:" + std::to_string(server.port()), to, 28960);
    int64_t now = 100000;
    net::GameClient client;
    client.name = "human";
    CHECK(client.connect(to, now));
    net::UserCmd cmd;
    int64_t nextCmd = now;
    auto step = [&](int ms) {
        // Server and client share the simulated clock; the client sends a command every 16 ms (a 60 Hz frame).
        for (int i = 0; i < ms; i += 4) {
            now += 4;
            server.poll(now, 0);
            client.poll(now, 0);
            if (client.state() >= net::ClientState::Primed && now >= nextCmd) {
                net::UserCmd c = cmd;
                c.serverTime = client.commandTime(now);
                client.sendCommand(c, now);
                nextCmd = now + 16;
            }
        }
    };
    for (int i = 0; i < 1000 && client.state() != net::ClientState::Active && client.state() != net::ClientState::Disconnected; ++i) step(20);
    CHECK(client.state() == net::ClientState::Active);
    CHECK(client.clientNum == 63);
    // It joins the game (the menu choice of a team and a weapon) like a player: its snapshots are those of a player.
    w.mpAutoJoin(63);
    int used = 0;
    for (const auto& c : w.mpState().clients) used += c.state != game::MpClientState::Free;
    CHECK(used == 64);
    // A 65th client: the network slots are free but the game has no client left.
    {
        net::GameClient extra;
        extra.connect(to, now);
        for (int i = 0; i < 200 && extra.state() != net::ClientState::Disconnected && extra.state() < net::ClientState::Primed; ++i) {
            step(20);
            extra.poll(now, 0);
        }
        CHECK(extra.state() == net::ClientState::Disconnected);
    }
    int slot = -1;
    for (const auto& [s, n] : game.slots())
        if (n == 63) slot = s;
    CHECK(slot >= 0);
    if (slot < 0) return;
    step(2000); // spawns settle
    game::Entity* human = w.mpClientEntity(63);
    CHECK(human && human->obj->get("sessionstate").toString() == "playing");
    const int64_t snaps0 = server.client(slot).snapshotsSent, bytes0 = server.client(slot).snapshotBytes;
    const int64_t simStart = now;
    const double cpu0 = cpuSeconds();
    const auto wall0 = std::chrono::steady_clock::now();
    size_t maxClientStates = 0, maxEntities = 0, maxPlayers = 0;
    std::vector<int> sizes;
    int64_t lastBytes = bytes0, lastSnaps = snaps0;
    for (int sec = 0; sec < seconds; ++sec) {
        for (int k = 0; k < 50; ++k) {
            step(20);
            const auto& sc = server.client(slot);
            if (sc.snapshotsSent > lastSnaps) {
                // the average of the snapshots of these 20 ms (one or two)
                sizes.push_back(int((sc.snapshotBytes - lastBytes) / (sc.snapshotsSent - lastSnaps)));
                lastBytes = sc.snapshotBytes, lastSnaps = sc.snapshotsSent;
            }
            if (const net::ClientSnapshot* sn = client.latest()) {
                maxClientStates = std::max(maxClientStates, sn->clients.size());
                maxEntities = std::max(maxEntities, sn->entities.size());
                size_t players = 0;
                for (const auto& [n, e] : sn->entities) players += n < 64;
                maxPlayers = std::max(maxPlayers, players);
            }
        }
        if (sec == seconds / 2) client.addReliableCommand("score");
    }
    const double cpu = cpuSeconds() - cpu0;
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - wall0).count();
    const double simSeconds = double(now - simStart) / 1000.0;
    const auto& sc = server.client(slot);
    const int64_t snaps = sc.snapshotsSent - snaps0;
    const double meanBytes = snaps ? double(sc.snapshotBytes - bytes0) / double(snaps) : 0.0;
    std::sort(sizes.begin(), sizes.end());
    const int p95 = sizes.empty() ? 0 : sizes[sizes.size() * 95 / 100];
    size_t over1400 = 0;
    for (int b : sizes) over1400 += b > 1400;
    int kills = 0, deaths = 0;
    for (const auto& c : w.mpState().clients)
        if (c.ent) {
            const auto sv = c.ent->obj->get("score"), dv = c.ent->obj->get("deaths");
            kills += sv.isInt() ? sv.asInt() : 0;
            deaths += dv.isInt() ? dv.asInt() : 0;
        }
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    // At the server's rate cap a remote client gets the next snapshot after (min(bytes,1500) * 1000 + 48000) / rate
    // ms (docs/spec/net.md section 4a); 60 a second needs a rate of at least this.
    const double rateFor60 = (std::min(meanBytes, 1500.0) * 1000.0 + 48000.0) / (1000.0 / 60.0);
    std::printf("mp64 soak: %.0f s simulated in %.1f s wall, %.1f s CPU: %.2f ms CPU per 50 ms server frame (server, 63 bots, "
                "one client's decode)\n", simSeconds, wall, cpu, cpu * 1000.0 / (simSeconds * 20.0));
    std::printf("mp64 soak: snapshots %lld (%.1f/s), mean %.0f bytes, p95 %d, max %d, %zu of %zu samples over 1400 bytes;"
                " a remote client needs rate %.0f for 60/s (cap 90000: %.1f/s at the mean)\n",
                (long long)snaps, double(snaps) / simSeconds, meanBytes, p95, sc.maxSnapshotBytes, over1400, sizes.size(),
                rateFor60, 90000.0 / ((std::min(meanBytes, 1500.0) * 1000.0 + 48000.0) / 1000.0));
    std::printf("mp64 soak: up to %zu client states, %zu entities, %zu players in a snapshot; kills %d deaths %d; peak RSS %.0f MB\n",
                maxClientStates, maxEntities, maxPlayers, kills, deaths, double(ru.ru_maxrss) / (1024.0 * 1024.0));
    CHECK(maxClientStates == 64);
    CHECK(maxPlayers > 0);
    // 60 a second while it plays; a killcam gets the frame snapshots only (20 a second, docs/spec/net.md section 10).
    CHECK(double(snaps) / simSeconds > 30.0);
    CHECK(deaths > 0);
    // Client 63 died (its body, the killcam of the highest client number) and plays again.
    human = w.mpClientEntity(63);
    const auto hd = human ? human->obj->get("deaths") : script::Value();
    std::printf("mp64 soak: client 63 deaths %d, session state %s\n", hd.isInt() ? hd.asInt() : -1,
                human ? human->obj->get("sessionstate").toString().c_str() : "-");
    CHECK(hd.isInt() && hd.asInt() > 0);
    // The scores command with all 64 rows.
    bool scores = false;
    for (const std::string& c : client.takeServerCommands())
        if (c.rfind("b 64 ", 0) == 0) scores = true;
    CHECK(scores);
    CHECK(w.vm().errors().empty());
    for (const auto& e : w.vm().errors()) std::cerr << "script error: " << e << "\n";
}

} // namespace

int main(int argc, char** argv) {
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cerr << "IW2_MAIN not set\n";
        return 1;
    }
    const std::string mode = argc > 1 ? argv[1] : "latch";
    if (mode == "latch") latch(main);
    else if (mode == "soak") soak(main, argc > 2 ? std::max(1, std::atoi(argv[2])) : 30);
    else {
        std::cerr << "usage: test_mp64 latch | soak [seconds]\n";
        return 2;
    }
    if (failures) std::cerr << failures << " check(s) failed\n";
    else std::cout << "mp64 " << mode << ": ok\n";
    return failures ? 1 : 0;
}
