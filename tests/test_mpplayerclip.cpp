// Headless (docs/spec/mp-pmove.md "Players against each other"): on a dedicated server two network clients that walk
// into each other, several commands per server frame, are blocked, and so is the prediction of one of them against
// the other drawn from the snapshots (no correction); a crouching player's sent box is 50 high. A bot walking into a
// network client is blocked. On a listen server a network client walking into the moving host is blocked.
// Needs IW2_MAIN.
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "fs/vfs.h"
#include "game/clientinput.h"
#include "game/world.h"
#include "net/gameclient.h"
#include "net/server.h"
#include "netgame/netcombat.h"
#include "netgame/netmove.h"
#include "netgame/remoteview.h"
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
using game::Entity;
using game::Vec3;
using game::World;
using script::Value;

Vec3 origin(Entity* e) { return e ? e->obj->get("origin").asVector() : Vec3{0, 0, 0}; }
int32_t angleShort(float deg) { return int32_t(std::lround(deg * 65536.0f / 360.0f)) & 0xffff; }
Vec3 add(const Vec3& a, const Vec3& b, float k = 1) { return {a[0] + b[0] * k, a[1] + b[1] * k, a[2] + b[2] * k}; }

// A listen server (host player 0) with network clients; client i's commands carry the server time minus lag[i].
struct Game {
    World host;
    netgame::WorldGame game;
    net::Server server;
    std::vector<std::unique_ptr<net::GameClient>> clients;
    std::vector<net::UserCmd> cmds;
    std::vector<int> lag;
    int64_t now = 1000;
    std::function<void()> beforeFrame;

    Game(fs::Vfs& vfs, const char* gt, bool dedicated) : host(vfs, "mp_toujane", true), game(nullptr, "", "", 7, 8), server(game, 5) {
        host.vm().setLoopGuardIterations(200000u);
        host.cvars["mapname"] = "mp_toujane";
        if (dedicated) host.mpSetDedicated();
        host.mpStart(gt);
        if (!dedicated) host.mpBegin(host.mpConnect("host"));
        host.frame();
        game.setWorld(&host, "mp_toujane", gt, 7);
        game.worldOwnedByHost = true;
        server.serverId = 7;
        game.sendCommand = [this](int slot, const std::string& text) { server.sendServerCommand(slot, text); };
        server.info().mapname = "mp_toujane";
        server.info().gametype = gt;
    }
    int connect(const std::string& name) {
        auto c = std::make_unique<net::GameClient>();
        c->name = name;
        net::NetAddress a;
        net::NetAddress::parse("127.0.0.1:" + std::to_string(server.port()), a, 28960);
        c->connect(a, now);
        clients.push_back(std::move(c));
        cmds.emplace_back();
        lag.push_back(0);
        for (int i = 0; i < 200 && clients.back()->state() != net::ClientState::Active; ++i) frame();
        return clients.back()->state() == net::ClientState::Active ? clients.back()->clientNum : -1;
    }
    void frame() {
        for (size_t i = 0; i < clients.size(); ++i) {
            // the command first (the snapshot time the client shows: lag ms back), so poll sends no idle command
            // three commands per server frame, as a client drawing at 60 Hz sends them
            if (clients[i]->state() >= net::ClientState::Primed)
                for (int k = 2; k >= 0; --k) {
                    net::UserCmd c = cmds[i];
                    c.serverTime = int(host.timeMs()) - lag[i] - 16 * k;
                    clients[i]->sendCommand(c, now);
                }
            clients[i]->poll(now, 1);
            (void)clients[i]->takeServerCommands();
        }
        server.poll(now, 0);
        if (beforeFrame) beforeFrame();
        host.frame();
        server.poll(now, 0);
        now += 50;
    }
    int slotOf(int client) const {
        for (const auto& [s, n] : game.slots())
            if (n == client) return s;
        return -1;
    }
    bool clear(const Vec3& a, const Vec3& b, int skip) {
        const auto tr = host.collision().trace(a, b, {0, 0, 0}, {0, 0, 0}, game::Contents::PlayerClipMask, skip);
        return tr.fraction >= 1 && !tr.startSolid;
    }
};


// A free straight run of 200 units with ground, from a dm spawn point.
bool findRun(Game& g, Vec3& start, Vec3& dir) {
    for (const auto& up : g.host.entities()) {
        if (up->classname != "mp_dm_spawn" || !up->obj->get("origin").isVector()) continue;
        const Vec3 c = up->obj->get("origin").asVector();
        for (int k = 0; k < 8; ++k) {
            const float ang = float(k) * float(M_PI) / 4.0f;
            const Vec3 d{std::cos(ang), std::sin(ang), 0};
            const auto tr = g.host.collision().trace(add(c, {0, 0, 2}), add(add(c, d, 200.0f), {0, 0, 2}), {-16, -16, 0}, {16, 16, 70},
                                                    game::Contents::PlayerClipMask, -1);
            if (tr.fraction >= 1 && !tr.startSolid) {
                start = c;
                dir = d;
                return true;
            }
        }
    }
    return false;
}

float flat(const Vec3& a, const Vec3& b) { return std::hypot(a[0] - b[0], a[1] - b[1]); }

// The view world of a network client: the map without scripts, the other players placed from the snapshots.
std::unique_ptr<World> clientView(fs::Vfs& vfs) {
    auto v = std::make_unique<World>(vfs, "mp_toujane", true);
    v->vm().setLoopGuardIterations(200000u);
    v->cvars["mapname"] = "mp_toujane";
    v->mpStartRemote();
    iw2::netgame::setMantleAnims(v->mpMantleAnims()); // the prediction climbs like the server, as the front end does
    return v;
}

void dedicated(fs::Vfs& vfs) {
    std::cout << "dedicated: two clients walk into each other\n";
    Game g(vfs, "dm", true);
    if (!g.server.open(0)) { std::cerr << "cannot open a UDP port\n"; ++failures; return; }
    const int a = g.connect("walker");
    const int b = g.connect("other");
    CHECK(a >= 0 && b >= 0 && a != b);
    if (a < 0 || b < 0) return;
    g.host.mpAutoJoin(a);
    g.host.mpAutoJoin(b);
    for (int i = 0; i < 100; ++i) g.frame();
    Vec3 start{}, dir{};
    CHECK(findRun(g, start, dir));
    const float yaw = std::atan2(dir[1], dir[0]) * 180.0f / float(M_PI);
    const Vec3 far = add(start, dir, 150.0f);
    g.cmds[0].angles[1] = angleShort(yaw);
    g.cmds[1].angles[1] = angleShort(yaw + 180.0f);
    for (int i = 0; i < 10; ++i) {
        g.host.mpClientEntity(a)->obj->set("origin", Value(start));
        g.host.mpClientEntity(b)->obj->set("origin", Value(far));
        g.frame();
    }
    // The walker's own view: the other player from the newest snapshot, the prediction from its player state.
    auto view = clientView(vfs);
    netgame::Predictor pred;
    net::GameClient& ca = *g.clients[0];
    const auto predict = [&](int64_t t) -> bool {
        const net::ClientSnapshot* snap = ca.latest();
        if (!snap) return false;
        view->mpSetRemotePlayers(netgame::RemoteView::players(*snap, *snap, 0, ca.clientNum, ca.configstrings));
        const netgame::PsFields f = netgame::readPs(snap->ps);
        pred.predict(view->collision(), f, ca.commandsAfter(f.commandTime), nullptr, t, view->player() ? view->player()->number : -1);
        return true;
    };
    for (int i = 0; i < 6; ++i) { g.frame(); predict(g.now); }
    g.cmds[0].forwardmove = 127;
    g.cmds[1].forwardmove = 127;
    float closest = 1e9f, closestPred = 1e9f, worstMiss = 0;
    for (int i = 0; i < 40; ++i) {
        g.frame();
        const Vec3 pa = origin(g.host.mpClientEntity(a)), pb = origin(g.host.mpClientEntity(b));
        closest = std::min(closest, flat(pa, pb));
        if (predict(g.now)) {
            const game::Vec p = pred.state().origin;
            closestPred = std::min(closestPred, flat({p[0], p[1], p[2]}, pb));
            worstMiss = std::max(worstMiss, pred.lastMiss());
        }
    }
    std::cout << "server closest " << closest << ", predicted closest " << closestPred << ", worst correction " << worstMiss << "\n";
    CHECK(closest >= 29.0f);       // two boxes 15 to the side stop 30 apart
    CHECK(closestPred >= 25.0f);   // the prediction against the other player as last sent stops at its box too
    CHECK(worstMiss < 16.0f);      // and agrees with the server: no snap back through the other player
    CHECK(flat(origin(g.host.mpClientEntity(a)), start) > 30.0f);  // it did walk

    // A crouching client: the sent box (solid) and the clip on the other client are 50 high.
    g.cmds[1].forwardmove = 0;
    g.cmds[1].buttons = int32_t(game::Button::Crouch);
    for (int i = 0; i < 10; ++i) { g.frame(); predict(g.now); }
    float top = 0;
    for (const auto& e : view->entities())
        if (e->isPlayer && e.get() != view->player() && !e->obj->removed)
            for (const auto& c : view->collision().entityClips())
                if (c.id == e->number) top = c.maxs[2];
    std::cout << "crouching box top on the other client " << top << "\n";
    CHECK(top == 50.0f);
}

void bot(fs::Vfs& vfs) {
    std::cout << "dedicated: a bot walks into a network client\n";
    Game g(vfs, "dm", true);
    if (!g.server.open(0)) { std::cerr << "cannot open a UDP port\n"; ++failures; return; }
    const int a = g.connect("standing");
    CHECK(a >= 0);
    if (a < 0) return;
    g.host.mpAutoJoin(a);
    const int b = g.host.mpAddBot("bot");
    CHECK(b >= 0);
    if (b < 0) return;
    for (int i = 0; i < 100; ++i) g.frame();
    g.host.mpBotSetCombat(b, false);
    Vec3 start{}, dir{};
    CHECK(findRun(g, start, dir));
    const Vec3 at = add(start, dir, 100.0f);
    for (int i = 0; i < 5; ++i) {
        g.host.mpClientEntity(a)->obj->set("origin", Value(at));
        g.host.mpClientEntity(b)->obj->set("origin", Value(start));
        g.frame();
    }
    g.host.mpBotSetGoal(b, add(start, dir, 200.0f), 8.0f);
    float closest = 1e9f;
    for (int i = 0; i < 60; ++i) {
        g.frame();
        closest = std::min(closest, flat(origin(g.host.mpClientEntity(a)), origin(g.host.mpClientEntity(b))));
    }
    std::cout << "bot closest " << closest << ", walked " << flat(origin(g.host.mpClientEntity(b)), start) << "\n";
    CHECK(closest >= 29.0f);
    CHECK(flat(origin(g.host.mpClientEntity(b)), start) > 30.0f);
}

void listen(fs::Vfs& vfs) {
    std::cout << "listen: a network client walks into the moving host\n";
    Game g(vfs, "dm", false);
    g.host.mpAutoJoin(0);
    if (!g.server.open(0)) { std::cerr << "cannot open a UDP port\n"; ++failures; return; }
    const int a = g.connect("walker");
    CHECK(a > 0);
    if (a <= 0) return;
    g.host.mpAutoJoin(a);
    for (int i = 0; i < 100; ++i) g.frame();
    Vec3 start{}, dir{};
    CHECK(findRun(g, start, dir));
    g.cmds[0].angles[1] = angleShort(std::atan2(dir[1], dir[0]) * 180.0f / float(M_PI));
    Vec3 host = add(start, dir, 150.0f);
    for (int i = 0; i < 10; ++i) {
        g.host.mpClientEntity(a)->obj->set("origin", Value(start));
        g.host.setPlayerOrigin(host, {0, 0, 0});
        g.frame();
    }
    // The host's own move (the front end's) between server frames: 10 units toward the client each frame, stopped by
    // the client's box as the front end's pmove would be.
    g.beforeFrame = [&] {
        const Vec3 to = add(host, dir, -10.0f);
        const auto tr = g.host.collision().trace(host, to, {-15, -15, 0}, {15, 15, 70}, game::Contents::PlayerClipMask,
                                                 g.host.player()->number);
        host = {tr.endPos[0], tr.endPos[1], tr.endPos[2]};
        g.host.setPlayerOrigin(host, {0, 0, 0});
    };
    g.cmds[0].forwardmove = 127;
    float closest = 1e9f;
    for (int i = 0; i < 30; ++i) {
        g.frame();
        closest = std::min(closest, flat(origin(g.host.mpClientEntity(a)), host));
    }
    std::cout << "host closest " << closest << "\n";
    CHECK(closest >= 29.0f);
    CHECK(flat(origin(g.host.mpClientEntity(a)), start) > 30.0f);
}

} // namespace

int main() {
    const char* main = std::getenv("IW2_MAIN");
    if (!main) { std::cerr << "IW2_MAIN not set\n"; return 1; }
    fs::Vfs vfs;
    vfs.mountDirectory(main);
    dedicated(vfs);
    bot(vfs);
    listen(vfs);
    if (failures) std::cerr << failures << " check(s) failed\n";
    else std::cout << "mpplayerclip: ok\n";
    return failures ? 1 : 0;
}
