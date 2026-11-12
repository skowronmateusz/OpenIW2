// Loopback test of the server core (src/net/server.cpp) with the protocol test client (src/net/client.cpp):
// connect, gamestate with configstrings and a baseline, reliable commands both ways, user commands driving a stub
// game, delta snapshots with entities appearing / changing / disappearing, disconnect and timeout.
// Single-threaded on an in-memory network with an injected clock: the server and the clients advance one millisecond at
// a time, so every time limit below is exact and the result does not depend on how loaded the machine is.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <utility>
#include <vector>

#include "net/client.h"
#include "net/netfields.h"
#include "net/server.h"

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

void putF(std::vector<uint8_t>& v, int off, float f) { std::memcpy(v.data() + off, &f, 4); }
void putI(std::vector<uint8_t>& v, int off, int32_t i) { std::memcpy(v.data() + off, &i, 4); }

struct StubGame : ServerGame {
    int time = 0;
    float x[8] = {};
    bool active[8] = {};
    std::vector<std::string> commands;
    int begins = 0, disconnects = 0, thinks = 0;

    std::map<int, std::string> configstrings() override {
        return {{0, "\\mapname\\stub\\g_gametype\\dm\\protocol\\118"}, {1, "\\sv_serverid\\77\\sv_pure\\0"}};
    }
    std::map<int, std::vector<uint8_t>> baselines() override {
        std::vector<uint8_t> e(kEntityStateSize, 0);
        putI(e, 4, 5);  // eType
        return {{100, e}};
    }
    bool clientConnect(int, const std::string& userinfo) override {
        return userinfo.find("\\name\\") != std::string::npos;
    }
    void clientBegin(int slot) override {
        active[slot] = true;
        ++begins;
    }
    void clientCommand(int, const std::string& text) override {
        commands.push_back(text);
    }
    void clientThink(int slot, const UserCmd& cmd) override {
        x[slot] += cmd.forwardmove > 0 ? 10.0f : 0.0f;
        ++thinks;
    }
    void clientDisconnect(int slot) override {
        active[slot] = false;
        ++disconnects;
    }
    void runFrame(int ms) override { time += ms; }
    int serverTime() const override { return time; }
    SnapshotView snapshot(int slot) override {
        SnapshotView v;
        v.ps.assign(kPlayerStateSize, 0);
        putI(v.ps, 0, time);
        putF(v.ps, 20, x[slot]);
        putI(v.ps, 204, slot);  // clientNum
        std::vector<uint8_t> e(kEntityStateSize, 0);
        putI(e, 4, 5);
        putF(e, 24, static_cast<float>(time % 1000));  // pos.trBase[0] changes every frame
        v.entities[100] = e;
        if (time < 1500) v.entities[200] = std::vector<uint8_t>(kEntityStateSize, 0);  // disappears later
        std::vector<uint8_t> c(kClientStateSize, 0);
        putI(c, 4, 1);  // team
        v.clients[slot] = c;
        return v;
    }
};

} // namespace

// docs/spec/net.md section 10 "Commands on a bursty path": a client sends a command every 8 ms (command time = real
// time - 60); the path holds everything for darkMs out of every periodMs until `evenFromMs`, then it is even. The
// server reads its socket every 5 ms and runs a game frame every 50 ms. Per frame: how far the last command run
// moved on (what the other clients see the player move).
struct PlayoutRun {
    int minStep = 1 << 30, maxStep = 0; // per frame, from fromMs on
    int held = 0, maxHold = 0;          // frames with a hold; the largest hold
    bool holdingAtEnd = false;
    size_t waitingAtEnd = 0;
    int ran = 0, sent = 0;
};
PlayoutRun runPlayout(bool playout, int periodMs, int darkMs, int fromMs, int toMs, int evenFromMs = 1 << 30, int maxHoldMs = -1) {
    PlayoutRun r;
    CommandPlayout p;
    if (maxHoldMs >= 0) p.maxHoldMs = maxHoldMs;
    std::vector<std::pair<int, int>> path; // (arrival, command time), in order
    for (int t = 0; t < toMs; t += 8) {
        int at = t + 2;
        if (periodMs > 0 && t < evenFromMs && at % periodMs < darkMs) at = at - at % periodMs + darkMs;
        path.emplace_back(at, t - 60);
    }
    r.sent = int(path.size());
    size_t next = 0;
    int lastRun = -61, lastAtFrame = -61;
    auto think = [&](const UserCmd& c) {
        CHECK(c.serverTime > lastRun); // in order, each once
        lastRun = c.serverTime;
        ++r.ran;
    };
    for (int now = 0; now < toMs; now += 5) {
        size_t end = next;
        while (end < path.size() && path[end].first <= now) ++end;
        for (; next < end; ++next) { // one packet per command, read in order
            UserCmd c;
            c.serverTime = path[next].second;
            if (playout) p.packet(now, c.serverTime);
            if (playout && p.holding()) p.push(c);
            else think(c);
        }
        if (now % 50 == 0) {
            if (playout)
                for (const UserCmd& c : p.due(now)) think(c);
            if (now >= fromMs) {
                r.minStep = std::min(r.minStep, lastRun - lastAtFrame);
                r.maxStep = std::max(r.maxStep, lastRun - lastAtFrame);
                if (p.holdMs() > 0) ++r.held;
                r.maxHold = std::max(r.maxHold, p.holdMs());
            }
            lastAtFrame = lastRun;
        }
    }
    r.holdingAtEnd = p.holding();
    r.waitingAtEnd = p.waiting();
    for (const UserCmd& c : p.flush()) think(c);
    return r;
}

void playoutTests() {
    // An even path: the commands never wait, with the playout or without.
    const PlayoutRun even = runPlayout(true, 0, 0, 1000, 8000);
    CHECK(even.held == 0 && !even.holdingAtEnd && even.ran == even.sent);
    CHECK(even.minStep >= 40 && even.maxStep <= 60);
    // 85 ms dark out of every 500 ms: on arrival the player gets nothing in one frame and the backlog in the next;
    // with the playout every frame moves him by a frame's worth, at the price of a hold of about the dark stretch.
    const PlayoutRun plain = runPlayout(false, 500, 85, 4000, 16000);
    const PlayoutRun ours = runPlayout(true, 500, 85, 4000, 16000);
    std::printf("commands through 85 ms spikes: per frame %d..%d ms on arrival, %d..%d ms with the playout (hold up to %d ms)\n",
                plain.minStep, plain.maxStep, ours.minStep, ours.maxStep, ours.maxHold);
    CHECK(plain.minStep <= 16 && plain.maxStep >= 100);
    CHECK(ours.minStep >= 40 && ours.maxStep <= 60);
    CHECK(ours.maxHold >= 60 && ours.maxHold <= 100);
    CHECK(ours.ran == ours.sent && plain.ran == plain.sent);
    // 220 ms dark out of every 520 ms: with a hold limit of 250 ms the same, the hold near the dark stretch. The default
    // limit (100 ms: every shot and use command waits as long as the hold) smooths only part of it, never holding more.
    const PlayoutRun radio = runPlayout(true, 520, 220, 5000, 16000, 1 << 30, 250);
    CHECK(radio.minStep >= 40 && radio.maxStep <= 60 && radio.maxHold <= 250);
    const PlayoutRun radioDefault = runPlayout(true, 520, 220, 5000, 16000);
    std::printf("commands through 220 ms dark stretches with the default limit: per frame %d..%d ms, hold up to %d ms\n",
                radioDefault.minStep, radioDefault.maxStep, radioDefault.maxHold);
    CHECK(radioDefault.maxHold <= 100 && radioDefault.ran == radioDefault.sent);
    // The path gets even again: the hold runs down (a tenth of real time: the player a tenth faster, never at once)
    // and the commands run on arrival again.
    const PlayoutRun back = runPlayout(true, 500, 85, 8000, 16000, 8000);
    CHECK(!back.holdingAtEnd && back.waitingAtEnd == 0 && back.ran == back.sent);
    CHECK(back.minStep >= 40 && back.maxStep <= 66);
    // The client's clock sliding (its command times fall behind the real time by 60 ms over 2 s, as after the first
    // snapshot of a level): not a bursty path.
    {
        CommandPlayout p;
        bool held = false;
        for (int now = 0; now < 6000; now += 5) {
            if (now % 15 == 0) p.packet(now, now - 60 - (now > 2000 ? std::min((now - 2000) * 60 / 2000, 60) : 0));
            if (now % 50 == 0) {
                (void)p.due(now);
                held = held || p.holding();
            }
        }
        CHECK(!held);
    }
    // A queue that only grows (commands far in the future) is cut at maxQueue.
    {
        CommandPlayout p;
        for (int i = 0; i < 4; ++i) p.packet(i * 300, i * 300 - (i % 2 ? 200 : 0)); // two late stretches: bursty
        CHECK(!p.holding() && p.due(900).empty()); // the window is looked at once per frame
        CHECK(p.holding());
        UserCmd c;
        for (int i = 0; i < 600; ++i) {
            c.serverTime = 100000 + i;
            p.push(c);
        }
        CHECK(p.due(950).size() == 600 - p.maxQueue);
        CHECK(p.flush().size() == p.maxQueue && p.waiting() == 0);
    }
}

int main() {
    playoutTests();
    MemoryUdp network;
    StubGame game;
    Server server(game, 7);
    server.maxClients = 4;
    server.serverId = 77;
    server.checksumFeed = 0x1234567;
    if (!server.open(0)) {
        std::cerr << "cannot open a UDP port\n";
        return 1;
    }
    bool pingSeen = false;
    int64_t now = 1000;
    // One millisecond of the virtual time; the only place the server runs.
    auto tick = [&] {
        ++now;
        server.poll(now, 0);
        if (server.client(0).ping < 999) pingSeen = true;
    };
    auto advance = [&](int ms) {
        for (int i = 0; i < ms; ++i) tick();
    };
    auto attach = [&](TestClient& c) {
        c.clock = [&now] { return now; };
        c.idle = tick;
    };
    tick();
    {
        server.sendServerCommand(-1, "nobody yet");  // no client: nothing queued
    }

    NetAddress addr;
    NetAddress::parse("127.0.0.1:" + std::to_string(server.port()), addr, 28960);
    TestClient client;
    attach(client);
    client.forwardMove = 127;
    client.addReliableCommand("say hello");
    CHECK(client.connect(addr, 2000));
    client.run(600);
    {
        server.sendServerCommand(0, "print \"welcome\"");
    }
    client.run(1800);
    std::cout << client.summary();
    CHECK(client.gamestateReceived);
    CHECK(client.configstrings[0].find("mapname") != std::string::npos);
    CHECK(client.baselines == 1);
    CHECK(client.clientNum == 0);
    CHECK(client.checksumFeed == 0x1234567);
    CHECK(client.error.empty());
    CHECK(client.snapshots >= 20);
    CHECK(client.deltaSnapshots >= 10);
    CHECK(client.entitiesInLastSnapshot == 1);  // entity 200 was removed at 1.5 s
    CHECK(client.clientsInLastSnapshot == 1);
    CHECK(client.lastOrigin[0] > 100.0f);
    CHECK(client.reliableAcknowledged == 1);
    bool welcome = false;
    for (auto& c : client.serverCommands) welcome |= c == "print \"welcome\"";
    CHECK(welcome);
    {
        CHECK(game.begins == 1);
        CHECK(game.thinks > 10);
        CHECK(game.commands.size() == 1 && game.commands[0] == "say hello");
    }
    // The last entity state the client holds matches what the game sent (delta chain intact).
    if (const auto* f = client.lastFrame()) {
        auto it = f->entities.find(100);
        CHECK(it != f->entities.end());
        if (it != f->entities.end()) {
            float x;
            std::memcpy(&x, it->second.data() + 24, 4);
            CHECK(std::fabs(x - static_cast<float>((f->serverTime) % 1000)) < 0.01f);
        }
    }
    CHECK(isLanAddress(addr));
    NetAddress wan; NetAddress::parse("8.8.8.8:1", wan, 1);
    CHECK(!isLanAddress(wan));
    NetAddress v172; NetAddress::parse("172.20.0.1:1", v172, 1);
    CHECK(isLanAddress(v172));
    {
        CHECK(server.timeoutMs == 240000);
        CHECK(server.zombieMs == 2000);
        CHECK(server.client(0).rate == 99999);  // a LAN address on a non-public server
        CHECK(server.client(0).snapshotMs == 50);
    }
    // Flood protection: the second floodable command within 800 ms is ignored; a team command never counts.
    client.addReliableCommand("say one");
    client.addReliableCommand("say two");
    client.addReliableCommand("team axis");
    client.run(400);
    {
        CHECK(pingSeen);  // the mean round trip of acknowledged snapshots; a long silence returns it to 999
    }
    {
        bool one = false, two = false, team = false;
        for (auto& c : game.commands) { one |= c == "say one"; two |= c == "say two"; team |= c == "team axis"; }
        CHECK(one); CHECK(!two); CHECK(team);
    }
    // Disconnect command.
    client.addReliableCommand("disconnect");
    client.run(300);
    {
        CHECK(server.client(0).state == ServerClientState::Free);
    }
    // Timeout: a second client connects and goes silent.
    {
        server.timeoutMs = 1500;
    }
    TestClient silent;
    attach(silent);
    CHECK(silent.connect(addr, 2000));
    silent.run(200);
    {
        CHECK(server.client(1).state != ServerClientState::Free);  // slot 0 is still a zombie of the first client
        CHECK(server.client(0).state == ServerClientState::Free);
    }
    // The last packet was at most one send interval (50 ms) ago: still connected well inside the limit.
    advance(1200);
    CHECK(server.client(1).state != ServerClientState::Free);
    // More than five failing checks after the limit.
    advance(1300);
    {
        for (int i = 0; i < 4; ++i) CHECK(server.client(i).state == ServerClientState::Free);
    }
    {
        CHECK(game.disconnects == 2);
    }
    // Server command overflow: 129 unacknowledged commands drop the client; a gap in the client's commands too.
    {
        server.timeoutMs = 60000;
    }
    TestClient over;
    attach(over);
    CHECK(over.connect(addr, 2000));
    // The zombie of the first client has expired: its slot is the first free one again.
    over.run(300);
    CHECK(over.clientNum == 0);
    {
        for (int i = 0; i < 135; ++i) server.sendServerCommand(-1, "x");
    }
    advance(200);
    {
        CHECK(game.disconnects == 3);
    }
    TestClient gap;
    attach(gap);
    CHECK(gap.connect(addr, 2000));
    gap.run(300);
    gap.reliableSequence += 5;
    gap.reliableAcknowledged = gap.reliableSequence;  // the next command leaves a gap in the sequence
    gap.addReliableCommand("say skip");
    gap.run(300);
    {
        CHECK(game.disconnects == 4);
        bool skipped = false;
        for (auto& c : game.commands) skipped |= c == "say skip";
        CHECK(!skipped);
    }
    if (failures) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "net_server: all tests passed\n";
    return 0;
}
