// Dedicated-server console (docs/spec/dedicated.md): map rotation, console commands, status, rcon, and the server
// hooks for rcon, live getstatus, private slots and the game password over UDP loopback. Headless, no game data.

#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

#include "net/netfields.h"
#include "net/oob.h"
#include "net/server.h"
#include "net/svconsole.h"
#include "net/udp.h"

using namespace iw2::net;

namespace {

int failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                             \
        }                                                                           \
    } while (0)

bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

struct FakeHost : ConsoleHost {
    bool run = true;
    std::string map = "mp_toujane";
    std::vector<std::string> loads, commands, kicks;
    int restarts = 0, fastRestarts = 0, bots = 0;
    bool quitCalled = false;
    std::vector<ConsoleClient> list;
    std::map<std::string, std::string> files;
    ServerConsole* console = nullptr;

    bool running() const override { return run; }
    std::string mapName() const override { return map; }
    bool loadMap(const std::string& m) override {
        if (m == "missing") return false;
        loads.push_back(m + "/" + console->cvar("g_gametype"));
        map = m;
        return true;
    }
    void restartMap(bool fast) override { (fast ? fastRestarts : restarts)++; }
    std::vector<ConsoleClient> clients() override { return list; }
    void kick(int num, const std::string& reason) override { kicks.push_back(std::to_string(num) + " " + reason); }
    void serverCommand(int num, const std::string& text) override {
        commands.push_back(std::to_string(num) + ":" + text);
    }
    bool readFile(const std::string& name, std::string& out) override {
        auto it = files.find(name);
        if (it == files.end()) return false;
        out = it->second;
        return true;
    }
    void quit() override { quitCalled = true; }
    bool addTestClient() override { return ++bots <= 2; }
};

void testRotation() {
    const std::string rot = "gametype tdm map mp_carentan map mp_toujane gametype dm  map mp_dawnville";
    std::string cur;
    auto r = rotateMap(cur, rot);
    CHECK(r.map && *r.map == "mp_carentan");
    CHECK(r.gametype && *r.gametype == "tdm");
    CHECK(has(r.log, "map_rotate...") && has(r.log, "Setting g_gametype: tdm.") && has(r.log, "Setting map: mp_carentan."));
    CHECK(cur == "map mp_toujane gametype dm  map mp_dawnville");
    r = rotateMap(cur, rot);
    CHECK(r.map && *r.map == "mp_toujane" && !r.gametype);
    r = rotateMap(cur, rot);
    CHECK(r.map && *r.map == "mp_dawnville" && r.gametype && *r.gametype == "dm");
    CHECK(cur.empty());
    r = rotateMap(cur, rot);  // wraps around
    CHECK(r.map && *r.map == "mp_carentan");

    std::string c2;
    r = rotateMap(c2, "");
    CHECK(!r.map && has(r.log, "No map specified in sv_mapRotation - forcing map_restart."));
    c2.clear();
    r = rotateMap(c2, "gametype");
    CHECK(!r.map && has(r.log, "No gametype specified after 'gametype' keyword"));
    c2.clear();
    r = rotateMap(c2, "map");
    CHECK(!r.map && has(r.log, "No map specified after 'map' keyword"));
    c2.clear();
    r = rotateMap(c2, "bogus map mp_a");
    CHECK(r.map && *r.map == "mp_a" && has(r.log, "Unknown keyword 'bogus' in sv_mapRotation."));
    c2.clear();
    r = rotateMap(c2, "MAP mp_b");  // keywords are case-insensitive
    CHECK(r.map && *r.map == "mp_b");
}

void testConsole() {
    FakeHost host;
    ServerConsole con(host);
    host.console = &con;
    con.cvars["g_gametype"] = "dm";

    con.execute("set sv_hostname My Server; seta scr_dm_scorelimit 50");
    CHECK(con.cvar("sv_hostname") == "My Server");
    CHECK(con.cvar("SCR_DM_SCORELIMIT") == "50");  // names are case-insensitive
    CHECK(has(con.capture("sv_hostname"), "\"sv_hostname\" is:\"My Server^7\""));
    con.execute("sv_hostname other");
    CHECK(con.cvar("sv_hostname") == "other");
    CHECK(has(con.capture("nosuchcommand"), "Unknown command"));

    host.files["server.cfg"] = "// comment\nset g_password pw\nset sv_mapRotation \"map mp_a map mp_b\"\nexec more\n";
    host.files["more.cfg"] = "set rcon_password secret";
    const std::string ex = con.capture("exec server.cfg");
    CHECK(has(ex, "execing server.cfg"));
    CHECK(con.cvar("g_password") == "pw");
    CHECK(con.cvar("sv_mapRotation") == "map mp_a map mp_b");
    CHECK(con.cvar("rcon_password") == "secret");
    CHECK(has(con.capture("exec nothere.cfg"), "couldn't exec nothere.cfg"));

    con.execute("map mp_dawnville");
    CHECK(host.loads.size() == 1 && host.loads[0] == "mp_dawnville/dm");
    CHECK(has(con.capture("map missing"), "missing"));
    con.execute("map_restart");
    con.execute("fast_restart");
    CHECK(host.restarts == 1 && host.fastRestarts == 1);

    con.execute("map_rotate");
    CHECK(host.loads.back() == "mp_a/dm");
    CHECK(con.cvar("sv_mapRotationCurrent") == "map mp_b");
    con.execute("map_rotate");
    con.execute("map_rotate");
    CHECK(host.loads.back() == "mp_a/dm");
    con.cvars["sv_mapRotation"] = "gametype tdm map missing";
    con.cvars["sv_mapRotationCurrent"] = "";
    con.execute("map_rotate");  // a map that cannot be loaded restarts the current one
    CHECK(host.restarts == 2 && con.cvar("g_gametype") == "tdm");

    ConsoleClient a;
    a.num = 0;
    a.name = "^1Red^7Guy";
    a.score = 12;
    a.ping = 48;
    a.lastMsgMs = 50;
    a.address = "127.0.0.1:28961";
    a.qport = 1234;
    a.rate = 25000;
    ConsoleClient b;
    b.num = 3;
    b.name = "bot0";
    b.address = "bot";
    b.bot = true;
    ConsoleClient c;
    c.num = 4;
    c.name = "joining";
    c.state = ConsoleClient::State::Connecting;
    host.list = {a, b, c};
    const std::string st = con.capture("status");
    CHECK(has(st, "map: mp_a\n"));
    CHECK(has(st, "num score ping guid   name            lastmsg address               qport rate\n"));
    CHECK(has(st, "  0    12   48      0 ^1Red^7Guy^7               50 127.0.0.1:28961        1234 25000\n"));
    CHECK(has(st, "  4     0 CNCT      0 joining^7"));

    con.execute("say \"hello all\"");
    CHECK(!host.commands.empty() && host.commands.back() == "-1:h \"console: hello all\"");
    con.execute("say hi there");
    CHECK(host.commands.back() == "-1:h \"console: hi there\"");
    con.execute("tell 0 psst");
    CHECK(host.commands.back() == "0:h \"console: psst\"");
    const size_t n = host.commands.size();
    con.execute("tell 4 not active");  // only active clients
    con.execute("tell 9 nobody");
    con.execute("say");
    CHECK(host.commands.size() == n);

    con.execute("kick redguy");  // colour codes and case are ignored
    CHECK(host.kicks.size() == 1 && host.kicks[0] == "0 EXE_PLAYERKICKED");
    con.execute("clientkick 3");
    CHECK(host.kicks.back() == "3 EXE_PLAYERKICKED");
    CHECK(has(con.capture("clientkick 7"), "not active"));
    con.execute("kick all");
    CHECK(host.kicks.size() == 5);
    CHECK(has(con.capture("clientkick 7"), "Client 7 is not active"));
    CHECK(has(con.capture("clientkick abc"), "Bad slot number: abc"));
    CHECK(has(con.capture("banUser nobody"), "Player nobody is not on the server"));
    CHECK(has(con.capture("banClient 7"), "is not active"));
    CHECK(has(con.capture("kick"), "Usage: kick <player name>"));
    CHECK(has(con.capture("onlykick"), "Usage: onlykick <player name>"));
    CHECK(has(con.capture("bind x y"), "Unknown command"));
    CHECK(has(con.capture("map missing"), "Can't find map maps/mp/missing.d3dbsp"));
    CHECK(has(con.capture("dumpuser"), "Usage: info <userid>"));

    con.execute("addtestclient 5");  // stops at the first one that fails
    CHECK(host.bots == 3);

    host.run = false;
    CHECK(con.capture("status") == "Server is not running.\n");
    CHECK(con.capture("map_restart") == "Server is not running.\n");
    con.cvars["sv_mapRotation"] = "gametype sd map mp_harbor";
    con.cvars["sv_mapRotationCurrent"] = "";
    con.execute("map_rotate");  // no running check: +exec server.cfg +map_rotate starts the first map
    CHECK(host.loads.back() == "mp_harbor/sd");
    host.loads.clear();
    con.cvars["sv_mapRotation"] = "gametype sd";
    con.cvars["sv_mapRotationCurrent"] = "";
    CHECK(has(con.capture("map_rotate"), "Server is not running."));  // the forced map_restart needs a running server
    CHECK(host.loads.empty() && host.restarts == 2);
    con.execute("quit");
    CHECK(host.quitCalled);
}

void testRcon() {
    FakeHost host;
    ServerConsole con(host);
    host.console = &con;
    auto r = con.rcon("rcon x status", "1.2.3.4:5", 1000);
    CHECK(r && r->size() == 1 && (*r)[0] == "print\nThe server must set 'rcon_password' for clients to use 'rcon'.\n");
    CHECK(!con.rcon("rcon x status", "1.2.3.4:5", 1499));  // within 500 ms: dropped
    con.cvars["rcon_password"] = "Secret";
    r = con.rcon("rcon secret status", "1.2.3.4:5", 1501);  // case-sensitive
    CHECK(r && (*r)[0] == "print\nInvalid password.\n");
    r = con.rcon("rcon", "1.2.3.4:5", 2100);
    CHECK(r && has((*r)[0], "rcon login <password>"));
    r = con.rcon("rcon Secret set sv_hostname \"two words\"", "1.2.3.4:5", 2700);
    CHECK(r && (*r)[0] == "print\n");
    CHECK(con.cvar("sv_hostname") == "two words");
    host.list = {ConsoleClient{}};
    r = con.rcon("rcon Secret status", "1.2.3.4:5", 3300);
    CHECK(r && has((*r)[0], "print\nmap: mp_toujane\n"));
}

// ---- Server hooks over UDP ----

struct NullGame : ServerGame {
    int connects = 0;
    std::map<int, std::string> configstrings() override { return {{0, "\\mapname\\x"}}; }
    bool clientConnect(int, const std::string&) override {
        ++connects;
        return true;
    }
    void clientBegin(int) override {}
    void clientCommand(int, const std::string&) override {}
    void clientThink(int, const UserCmd&) override {}
    void clientDisconnect(int) override {}
    void runFrame(int) override {}
    int serverTime() const override { return 0; }
    SnapshotView snapshot(int) override {
        SnapshotView v;
        v.ps.assign(kPlayerStateSize, 0);
        return v;
    }
};

std::string request(UdpSocket& sock, Server& server, const NetAddress& to, const std::string& text) {
    auto pkt = makeOutOfBand(text);
    sock.sendTo(to, pkt.data(), static_cast<int>(pkt.size()));
    const auto now = [] {
        using namespace std::chrono;
        return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    };
    for (int i = 0; i < 40; ++i) {
        server.poll(now(), 5);
        uint8_t buf[4096];
        NetAddress from;
        const int n = sock.receive(buf, sizeof buf, from, 5);
        if (n > 0) {
            auto t = outOfBandText(buf, n);
            if (!t) continue;  // a sequenced message to an already connected slot
            return *t;
        }
    }
    return {};
}

std::string connect(UdpSocket& sock, Server& server, const NetAddress& to, const std::string& extra, int qport) {
    const std::string ch = request(sock, server, to, "getchallenge 0");
    const auto tok = tokenize(ch);
    if (tok.size() < 2) return "no challenge";
    return request(sock, server, to,
                   "connect \"\\protocol\\118\\challenge\\" + tok[1] + "\\qport\\" + std::to_string(qport) +
                       "\\name\\p" + extra + "\"");
}

void testServerHooks() {
    NullGame game;
    Server server(game, 3);
    server.maxClients = 2;
    server.privateClients = 1;
    server.privatePassword = "vip";
    FakeHost host;
    ServerConsole con(host);
    host.console = &con;
    con.cvars["rcon_password"] = "pw";
    std::string gPassword;
    server.oobHook = [&](const NetAddress& from, const std::string& line, int64_t now) {
        if (tokenize(line).at(0) != "rcon") return false;
        if (auto r = con.rcon(line, from.toString(), now))
            for (const auto& t : *r) server.sendOutOfBand(from, t);
        return true;
    };
    server.statusHook = [&](ServerInfo& info) {
        info.clients = 3;
        info.players = {{7, 40, "alice"}, {2, 0, "bot0"}};
    };
    server.connectFilter = [&](const std::string& userinfo) {
        return !gPassword.empty() && infoValueForKey(userinfo, "password") != gPassword ? "GAME_INVALIDPASSWORD"
                                                                                         : "";
    };
    CHECK(server.open(0));
    NetAddress to;
    NetAddress::parse("127.0.0.1:" + std::to_string(server.port()), to, 28960);
    UdpSocket sock;
    CHECK(sock.open(0, 0));

    CHECK(request(sock, server, to, "rcon bad status") == "print\nInvalid password.\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(520));
    CHECK(has(request(sock, server, to, "rcon pw say hi"), "print\n"));
    CHECK(!host.commands.empty() && host.commands.back() == "-1:h \"console: hi\"");

    const std::string st = request(sock, server, to, "getstatus");
    CHECK(has(st, "statusResponse\n") && has(st, "7 40 \"alice\"\n") && has(st, "2 0 \"bot0\"\n"));
    CHECK(has(request(sock, server, to, "getinfo abc"), "\\clients\\3"));

    gPassword = "letmein";
    CHECK(connect(sock, server, to, "", 11) == "error\nGAME_INVALIDPASSWORD");
    CHECK(game.connects == 0);
    gPassword.clear();
    // Slot 0 is private: without the private password only slot 1 is free.
    CHECK(connect(sock, server, to, "", 12) == "connectResponse");
    CHECK(connect(sock, server, to, "", 13) == "error\nEXE_SERVERISFULL");
    CHECK(connect(sock, server, to, "\\password\\vip", 14) == "connectResponse");
    CHECK(game.connects == 2);
    CHECK(server.client(0).state == ServerClientState::Connected && server.client(1).state == ServerClientState::Connected);

    server.newMap(5);
    CHECK(server.serverId == 5 && server.client(0).gamestateMessageNum == -1);
}

} // namespace

int main() {
    testRotation();
    testConsole();
    testRcon();
    testServerHooks();
    if (failures) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "svconsole: ok\n";
    return 0;
}
