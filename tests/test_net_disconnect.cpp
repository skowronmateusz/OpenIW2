// Disconnect reasons end to end over loopback (docs/spec/net.md section 14): the server's `w "<reason>"` command (kick,
// server-side timeout, server quit), a connection the client loses on its own, and the refusals at connect (full
// server, wrong game password, protocol mismatch). For each: the client state, the reason, the error dialog's title and
// message, and that a new connection works afterwards. Also the `password` userinfo key and the game password check.
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>

#include "net/gameclient.h"
#include "net/netfields.h"
#include "net/oob.h"
#include "net/server.h"
#include "net/udp.h"
#include "netgame/clienterror.h"

using namespace iw2;
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

struct StubGame : ServerGame {
    int time = 0;
    std::string lastUserinfo;
    std::map<int, std::string> configstrings() override {
        return {{0, "\\mapname\\stub\\g_gametype\\dm\\protocol\\118"}, {1, "\\sv_serverid\\5"}};
    }
    std::map<int, std::vector<uint8_t>> baselines() override { return {}; }
    bool clientConnect(int, const std::string& userinfo) override { lastUserinfo = userinfo; return true; }
    void clientBegin(int) override {}
    void clientCommand(int, const std::string&) override {}
    void clientThink(int, const UserCmd&) override {}
    void clientDisconnect(int) override {}
    void runFrame(int ms) override { time += ms; }
    int serverTime() const override { return time; }
    SnapshotView snapshot(int slot) override {
        SnapshotView v;
        v.ps.assign(kPlayerStateSize, 0);
        std::memcpy(v.ps.data(), &time, 4);
        std::memcpy(v.ps.data() + 204, &slot, 4);
        return v;
    }
};

// Polls both ends (one thread) until `done` or `ms` of simulated time passed; the clock advances 10 ms per step.
template <class F>
void pump(Server* server, GameClient& client, int64_t& now, int ms, F done) {
    for (int t = 0; t < ms && !done(); t += 10) {
        now += 10;
        if (server) server->poll(now, 1);
        client.poll(now, 1);
    }
}

NetAddress addressOf(const Server& s) {
    NetAddress a;
    NetAddress::parse("127.0.0.1:" + std::to_string(s.port()), a, kDefaultPort);
    return a;
}

bool connectTo(Server& server, GameClient& c, int64_t& now) {
    if (!c.connect(addressOf(server), now)) return false;
    pump(&server, c, now, 3000, [&] { return c.state() == ClientState::Disconnected || c.state() >= ClientState::Primed; });
    pump(&server, c, now, 200, [] { return false; }); // a few snapshots
    return c.state() >= ClientState::Primed;
}

// The client was dropped by the server with `reason`: a notice dialog showing the localized reason.
void expectServerDrop(GameClient& c, const std::string& reason) {
    CHECK(c.state() == ClientState::Disconnected);
    CHECK(c.dropFromServer);
    CHECK(c.dropReason == reason);
    const auto d = netgame::errorDialog(c.dropFromServer, c.dropReason);
    CHECK(d.titleKey == "MENU_NOTICE");
    CHECK(d.message == "\x14" + reason);
}

void serverCommandReasons() {
    StubGame game;
    Server server(game, 3);
    server.zombieMs = 0;  // the slots are reused at once here
    server.maxClients = 4;
    server.serverId = 5;
    CHECK(server.open(0));
    int64_t now = 1000;
    // Kick: kick / clientkick drop with EXE_PLAYERKICKED.
    {
        GameClient c;
        CHECK(connectTo(server, c, now));
        server.dropClient(0, "EXE_PLAYERKICKED");
        CHECK(server.client(0).state == ServerClientState::Free);
        pump(&server, c, now, 500, [&] { return c.state() == ClientState::Disconnected; });
        expectServerDrop(c, "EXE_PLAYERKICKED");
        c.poll(now + 100000, 0); // no further timeout once dropped
        CHECK(c.dropReason == "EXE_PLAYERKICKED");
    }
    // Reconnect after the drop: a fresh client reaches the game again.
    {
        GameClient c;
        CHECK(connectTo(server, c, now));
        CHECK(c.dropReason.empty() && c.error.empty());
        // Server-side timeout: the client goes silent, the server drops it with EXE_TIMEDOUT; the client reads the
        // command when it polls again.
        server.timeoutMs = 300;
        for (int i = 0; i < 70; ++i) server.poll(now += 10, 1);  // the limit, then more than five failing frames
        CHECK(server.client(0).state == ServerClientState::Free);
        pump(nullptr, c, now, 200, [&] { return c.state() == ClientState::Disconnected; });
        expectServerDrop(c, "EXE_TIMEDOUT");
        server.timeoutMs = 40000;
    }
    // Server quit: every client gets EXE_SERVERQUIT.
    {
        GameClient a, b;
        CHECK(connectTo(server, a, now));
        CHECK(connectTo(server, b, now));
        server.shutdown("EXE_SERVERQUIT");
        for (int i = 0; i < 4; ++i) CHECK(server.client(i).state == ServerClientState::Free);
        pump(nullptr, a, now, 200, [&] { return a.state() == ClientState::Disconnected; });
        pump(nullptr, b, now, 200, [&] { return b.state() == ClientState::Disconnected; });
        expectServerDrop(a, "EXE_SERVERQUIT");
        expectServerDrop(b, "EXE_SERVERQUIT");
    }
    // `w` without a reason, and the PB form.
    {
        GameClient c;
        CHECK(connectTo(server, c, now));
        server.sendServerCommand(0, "w");
        pump(&server, c, now, 500, [&] { return c.state() == ClientState::Disconnected; });
        expectServerDrop(c, "EXE_SERVER_DISCONNECTED");
        server.dropClient(0, "x");
        GameClient d;
        CHECK(connectTo(server, d, now));
        server.sendServerCommand(0, "w \"Player^7 cheating\" PB");
        pump(&server, d, now, 500, [&] { return d.state() == ClientState::Disconnected; });
        CHECK(d.dropReason == "EXE_SERVERDISCONNECTREASON\x15Player^7 cheating");
        CHECK(netgame::errorDialog(d.dropFromServer, d.dropReason).titleKey == "MENU_NOTICE");
    }
}

// The connection is lost without a word from the server (it vanished): the client's own timeout.
void lostConnection() {
    StubGame game;
    auto server = std::make_unique<Server>(game, 4);
    server->serverId = 5;
    CHECK(server->open(0));
    int64_t now = 1000;
    GameClient c;
    c.connectTimeoutMs = 100;
    CHECK(connectTo(*server, c, now));
    server.reset();
    c.poll(now + 1000, 0);
    CHECK(c.state() == ClientState::Disconnected);
    CHECK(!c.dropFromServer);
    CHECK(c.dropReason == "EXE_DISCONNECTED_FROM_SERVER");
    const auto d = netgame::errorDialog(c.dropFromServer, c.dropReason);
    CHECK(d.titleKey == "MENU_ERROR");
    // No server at all: the handshake times out with the same reason.
    GameClient h;
    h.connectTimeoutMs = 100;
    NetAddress nowhere;
    NetAddress::parse("127.0.0.1:9", nowhere, kDefaultPort);
    CHECK(h.connect(nowhere, now));
    h.poll(now + 500, 0);
    CHECK(h.state() == ClientState::Disconnected && h.dropReason == "EXE_DISCONNECTED_FROM_SERVER");
}

// A dead or stalling server never hangs the waiting caller: no answer at all, and a server that answers the handshake and
// keeps sending datagrams without ever a gamestate. Both end disconnected with the reason of the error dialog.
void handshakeBounded() {
    const auto clock = [] {
        return int64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    };
    const int64_t t0 = clock();
    NetAddress nowhere;
    NetAddress::parse("127.0.0.1:9", nowhere, kDefaultPort);
    GameClient dead;
    dead.connectTimeoutMs = 100;
    CHECK(dead.connect(nowhere, clock()));
    dead.waitForHandshake(clock, 5);
    CHECK(dead.state() == ClientState::Disconnected);
    CHECK(clock() - t0 < 2000);
    CHECK(!netgame::errorDialog(dead.dropFromServer, dead.dropReason).message.empty());

    UdpSocket stall;
    CHECK(stall.open(0x7f000001, 0));
    NetAddress at;
    NetAddress::parse("127.0.0.1:" + std::to_string(stall.localPort()), at, kDefaultPort);
    GameClient c;
    c.connectTimeoutMs = 100;
    CHECK(c.connect(at, clock()));
    const int64_t t1 = clock();
    int answers = 0;
    c.waitForHandshake(clock, 1, [&] {
        uint8_t buf[2048];
        NetAddress from;
        while (stall.receive(buf, sizeof buf, from, 0) > 0) {
            for (const char* text : {"challengeResponse 1", "connectResponse"}) {
                const auto p = makeOutOfBand(text);
                stall.sendTo(from, p.data(), int(p.size()));
            }
            ++answers;
        }
    });
    CHECK(answers > 0);
    CHECK(c.state() == ClientState::Disconnected);
    CHECK(clock() - t1 < 2000);
}

// Refusals at connect: `error\n<reason>` answers.
void refusals() {
    StubGame game;
    Server server(game, 5);
    server.zombieMs = 0;  // the slots are reused at once here
    server.maxClients = 1;
    server.serverId = 5;
    CHECK(server.open(0));
    int64_t now = 1000;
    // Full server: a notice.
    GameClient first;
    CHECK(connectTo(server, first, now));
    GameClient second;
    CHECK(!connectTo(server, second, now));
    CHECK(second.state() == ClientState::Disconnected && !second.dropFromServer);
    CHECK(second.dropReason == "EXE_SERVERISFULL");
    auto d = netgame::errorDialog(second.dropFromServer, second.dropReason);
    CHECK(d.titleKey == "MENU_NOTICE" && d.message == "\x14" "EXE_SERVERISFULL");
    first.disconnect();
    pump(&server, second, now, 300, [] { return false; });
    CHECK(server.client(0).state == ServerClientState::Free);
    // Game password: refused without / with a wrong password, accepted with the right one (sent as userinfo "password").
    server.connectFilter = [](const std::string& ui) { return gamePasswordRejection("secret", ui); };
    GameClient nopw;
    CHECK(!connectTo(server, nopw, now));
    CHECK(nopw.dropReason == "GAME_INVALIDPASSWORD");
    d = netgame::errorDialog(nopw.dropFromServer, nopw.dropReason);
    CHECK(d.titleKey == "MENU_ERROR" && d.message == "\x14" "GAME_INVALIDPASSWORD");
    GameClient wrong;
    wrong.password = "Secret";
    CHECK(!connectTo(server, wrong, now));
    CHECK(wrong.dropReason == "GAME_INVALIDPASSWORD");
    GameClient right;
    right.password = "secret";
    CHECK(connectTo(server, right, now));
    CHECK(infoValueForKey(game.lastUserinfo, "password") == "secret");
    right.disconnect();
    pump(&server, right, now, 300, [] { return false; });
    // Protocol mismatch: the connectionless layer refuses another protocol; the reason carries the version.
    ConnectionlessServer cs(9);
    server.oobHook = [&](const NetAddress& from, const std::string& line, int64_t) {
        std::string l = line;
        if (const size_t p = l.find("\\protocol\\118"); p != std::string::npos) l.replace(p, 13, "\\protocol\\1");
        if (auto reply = cs.handle({from.ip, from.port}, l)) server.sendOutOfBand(from, *reply);
        return true;
    };
    GameClient old;
    CHECK(!connectTo(server, old, now));
    CHECK(old.dropReason == std::string("EXE_SERVER_IS_DIFFERENT_VER\x15") + kProtocolVersionString);
    d = netgame::errorDialog(old.dropFromServer, old.dropReason);
    CHECK(d.titleKey == "MENU_ERROR" && d.message == "\x14" + old.dropReason);
}

void passwordRule() {
    CHECK(gamePasswordRejection("", "\\name\\a").empty());
    CHECK(gamePasswordRejection("None", "\\name\\a").empty());
    CHECK(gamePasswordRejection("pw", "\\password\\pw").empty());
    CHECK(gamePasswordRejection("pw", "\\password\\PW") == "GAME_INVALIDPASSWORD");
    CHECK(gamePasswordRejection("pw", "\\name\\a") == "GAME_INVALIDPASSWORD");
    // Notice keys are compared without case; plain text stays plain.
    CHECK(netgame::errorDialog(false, "exe_disconnected").titleKey == "MENU_NOTICE");
    CHECK(netgame::errorDialog(false, "something broke").message == "something broke");
}

} // namespace

int main() {
    serverCommandReasons();
    lostConnection();
    handshakeBounded();
    CHECK(netgame::mpMenuAfterDisconnect(false, true));   // a --connect start: to the MP menu set
    CHECK(!netgame::mpMenuAfterDisconnect(true, true));   // already there
    CHECK(!netgame::mpMenuAfterDisconnect(false, false)); // a single-player level
    refusals();
    passwordRule();
    if (failures) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "net_disconnect: all tests passed\n";
    return 0;
}
