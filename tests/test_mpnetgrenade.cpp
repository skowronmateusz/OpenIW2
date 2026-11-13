// End to end, headless (docs/spec/net.md section 11 "Trajectories"): a grenade thrown on a dm listen server flies
// smoothly in a network client's view. The server sends the grenade's gravity trajectory; the client evaluates it
// at its own drawn time (16 ms render frames between 50 ms snapshots), so the drawn position moves every frame,
// follows the server's parabola and never steps. Needs IW2_MAIN.
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "fs/vfs.h"
#include "game/world.h"
#include "net/gameclient.h"
#include "net/server.h"
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
using game::World;

constexpr const char* kMap = "mp_carentan";

game::Vec3 origin(Entity* e) { return e ? e->obj->get("origin").asVector() : game::Vec3{0, 0, 0}; }

// A listen server (host player 0 and its World) with one network client, driven by an injected clock. steps: the
// server is polled at every client step too, so it sends the snapshots between its 50 ms game frames (sv_snaps 60).
struct Game {
    bool steps = false;
    World host;
    netgame::WorldGame game;
    net::Server server;
    std::unique_ptr<net::GameClient> client;
    int64_t now = 1000;

    Game(fs::Vfs& vfs) : host(vfs, kMap, true), game(nullptr, "", "", 7, 8), server(game, 5) {
        host.vm().setLoopGuardIterations(200000u);
        host.cvars["mapname"] = kMap;
        host.mpStart("dm");
        const int local = host.mpConnect("host");
        host.mpBegin(local);
        host.frame();
        game.setWorld(&host, kMap, "dm", 7);
        game.worldOwnedByHost = true;
        server.serverId = 7;
        game.sendCommand = [this](int slot, const std::string& text) { server.sendServerCommand(slot, text); };
        server.info().mapname = kMap;
        server.info().gametype = "dm";
    }
    void clientStep() {
        if (steps) server.poll(now, 0);
        client->poll(now, 1);
        if (client->state() >= net::ClientState::Primed) {
            net::UserCmd c;
            c.serverTime = client->commandTime(now);
            client->sendCommand(c, now);
        }
        (void)client->takeServerCommands();
    }
    // One server frame (50 ms): the world steps, then the snapshots go out.
    void serverFrame() {
        server.poll(now, 0);
        host.frame();
        server.poll(now, 0);
    }
    int connect(const std::string& name) {
        client = std::make_unique<net::GameClient>();
        client->name = name;
        net::NetAddress a;
        net::NetAddress::parse("127.0.0.1:" + std::to_string(server.port()), a, 28960);
        client->connect(a, now);
        for (int i = 0; i < 200 && client->state() != net::ClientState::Active; ++i) {
            clientStep();
            serverFrame();
            now += 50;
        }
        return client->state() == net::ClientState::Active ? client->clientNum : -1;
    }
};

void run(fs::Vfs& vfs, bool steps) {
    std::cout << (steps ? "-- snapshots between the game frames\n" : "-- one snapshot per game frame\n");
    Game g(vfs);
    g.steps = steps;
    g.host.mpAutoJoin(0);
    if (!g.server.open(0)) {
        std::cerr << "cannot open a UDP port\n";
        ++failures;
        return;
    }
    const int a = g.connect("alpha");
    CHECK(a > 0);
    if (a <= 0) return;
    g.host.mpAutoJoin(a);
    for (int i = 0; i < 60; ++i) {
        g.clientStep();
        g.serverFrame();
        g.now += 50;
    }
    Entity* alpha = g.host.mpClientEntity(a);
    CHECK(alpha != nullptr);
    if (!alpha) return;

    // Alpha's front end: a remote World fed with its snapshots.
    World view(vfs, kMap, true);
    view.mpStartRemote();
    netgame::RemoteView rv;

    // The host player throws a grenade; samples are taken on the stretches between bounces.
    Entity* hostPlayer = g.host.player();
    CHECK(hostPlayer != nullptr);
    if (!hostPlayer) return;
    const game::Vec3 eye = origin(hostPlayer);
    // Launched 600 units above the player's head, straight up: seconds of free flight in the open air above the map.
    g.host.playerThrowGrenade("frag_grenade_american_mp", {eye[0], eye[1], eye[2] + 660.0f}, {-89.0f, 0.0f, 0.0f}, {0, 0, 0});
    Entity* grenPtr = nullptr;
    for (const auto& e : g.host.entities())
        if (!e->obj->removed && e->grenade) grenPtr = e.get();
    CHECK(grenPtr != nullptr);
    if (!grenPtr) return;
    Entity& gren = *grenPtr;

    // 16 ms render frames; a server frame every 50 ms of the same clock.
    struct Sample { int t; game::Vec3 drawn; game::Vec3 server; float speed; int64_t trTime; };
    std::vector<Sample> samples;
    int acc = 0, frames = 0;
    for (int step = 0; step < 230; ++step) {
        g.now += 16;
        acc += 16;
        while (acc >= 50) {
            g.serverFrame();
            acc -= 50;
        }
        g.clientStep();
        const net::ClientSnapshot *s0 = nullptr, *s1 = nullptr;
        float frac = 0;
        if (!g.client->interpolation(g.client->renderTime(g.now), s0, s1, frac)) continue;
        // The time the models with a trajectory are drawn at (docs/spec/net.md section 11 "Trajectories").
        const int t = g.client->renderTime(g.now) - netgame::RemoteView::trajectoryDelayMs(g.client->snapshotIntervalMs());
        rv.apply(view, *g.client->latest(), g.client->configstrings, a, view.timeMs());
        netgame::RemoteView::placeTrajectories(view, g.client->snapshotsFrom(t), t);
        ++frames;
        if (!gren.grenade || gren.grenade->trType != 5) continue;  // airborne on the server only
        for (const auto& e : view.entities()) {
            if (e->obj->removed || e->obj->get("model").toString().rfind("xmodel/projectile_", 0) != 0) continue;
            const game::GrenadeState& gs = *gren.grenade;
            const float tt = float(t - gs.trTime) * 0.001f;
            if (tt < 0) break;
            Sample s;
            s.t = t;
            s.drawn = origin(e.get());
            s.server = {gs.trBase[0] + gs.trDelta[0] * tt, gs.trBase[1] + gs.trDelta[1] * tt, gs.trBase[2] + gs.trDelta[2] * tt - 400.0f * tt * tt};
            const float vz = gs.trDelta[2] - 800.0f * tt;
            s.speed = std::sqrt(gs.trDelta[0] * gs.trDelta[0] + gs.trDelta[1] * gs.trDelta[1] + vz * vz);
            s.trTime = gs.trTime;
            samples.push_back(s);
            break;
        }
    }
    int moving = 0, good = 0, zero = 0, big = 0, far = 0;
    for (size_t i = 1; i < samples.size(); ++i) {
        const Sample& s = samples[i];
        const game::Vec3 d{s.drawn[0] - samples[i - 1].drawn[0], s.drawn[1] - samples[i - 1].drawn[1], s.drawn[2] - samples[i - 1].drawn[2]};
        const float moved = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        const float expect = s.speed * float(s.t - samples[i - 1].t) * 0.001f;
        // Only a stretch of one parabola, past the snapshot that carries it (the client draws the older snapshot's
        // trajectory: a bounce re-bases it on the server up to two snapshots before the client follows).
        if (s.trTime != samples[i - 1].trTime || s.t - s.trTime < 150) continue;
        if (expect < 1.0f) continue;  // near the top of the arc
        ++moving;
        if (moved < 0.05f) ++zero;
        if (moved > 2.5f * expect) ++big;
        if (moved >= 0.5f * expect && moved <= 1.5f * expect) ++good;
        const float err = std::sqrt((s.drawn[0] - s.server[0]) * (s.drawn[0] - s.server[0]) + (s.drawn[1] - s.server[1]) * (s.drawn[1] - s.server[1]) +
                                    (s.drawn[2] - s.server[2]) * (s.drawn[2] - s.server[2]));
        if (err > 2.0f) ++far;
    }
    std::cout << "grenade: " << frames << " render frames, " << samples.size() << " airborne samples, " << moving << " moving, " << good
              << " within 50% of the server speed, " << zero << " zero-move, " << big << " over 2.5x, " << far << " off the server parabola\n";
    CHECK(samples.size() >= 30);
    CHECK(moving >= 20);
    CHECK(zero == 0);
    CHECK(big == 0);
    CHECK(good >= moving * 9 / 10);
    CHECK(far == 0);

    // A lobbed grenade that bounces off the walls and the floor: the drawn path never reverses into a wall and back
    // (the model stops where the server's bounce re-based it and waits there for at most the rest of that server
    // frame), and the holds are short. Each bounce event of the server (EV_GRENADE_BOUNCE in the missile's event ring)
    // plays the surface's bounce sound once at the drawn grenade, and the drawn angles follow the server's angular
    // trajectory after every re-basing (docs/spec/net.md section 11).
    struct Heard { std::string alias; game::Vec3 at; };
    std::vector<Heard> heard;
    view.playSound = [&](const std::string& alias, const game::Vec3* at, float*, int) {
        if (alias.rfind("grenade_bounce_", 0) == 0) heard.push_back({alias, at ? *at : game::Vec3{0, 0, 0}});
        return 0;
    };
    g.host.playerThrowGrenade("frag_grenade_american_mp", {eye[0], eye[1], eye[2] + 60.0f}, {-45.0f, 0.0f, 0.0f}, {0, 0, 0});
    Entity* lob = nullptr;
    for (const auto& e : g.host.entities())
        if (!e->obj->removed && e->grenade && e.get() != &gren) lob = e.get();
    CHECK(lob != nullptr);
    if (!lob) return;
    std::vector<std::pair<int, game::Vec3>> path;
    int rebases = 0, spinChecks = 0, spinOff = 0, spinMoves = 0;
    size_t serverBounces = 0;
    game::Vec3 lastDrawnAngles{};
    bool haveAngles = false;
    int64_t lastTrTime = lob->grenade->trTime;
    for (int step = 0; step < 150; ++step) {
        g.now += 16;
        acc += 16;
        while (acc >= 50) {
            g.serverFrame();
            acc -= 50;
        }
        g.clientStep();
        const net::ClientSnapshot *s0 = nullptr, *s1 = nullptr;
        float frac = 0;
        if (!g.client->interpolation(g.client->renderTime(g.now), s0, s1, frac)) continue;
        // The time the models with a trajectory are drawn at (docs/spec/net.md section 11 "Trajectories").
        const int t = g.client->renderTime(g.now) - netgame::RemoteView::trajectoryDelayMs(g.client->snapshotIntervalMs());
        rv.apply(view, *g.client->latest(), g.client->configstrings, a, view.timeMs());
        const auto snaps = g.client->snapshotsFrom(t);
        netgame::RemoteView::placeTrajectories(view, snaps, t);
        rv.missileEvents(view, snaps);
        if (lob->obj->removed || !lob->grenade) break;
        serverBounces = lob->grenade->bounceEvents.size();
        if (lob->grenade->trType != 5) continue;
        if (lob->grenade->trTime != lastTrTime) ++rebases, lastTrTime = lob->grenade->trTime;
        for (const auto& e : view.entities()) {
            if (e->obj->removed || e->obj->get("model").toString().rfind("xmodel/projectile_", 0) != 0) continue;
            const game::Vec3 o = origin(e.get());
            if (std::fabs(o[0] - eye[0]) < 600.0f && std::fabs(o[1] - eye[1]) < 600.0f && o[2] < eye[2] + 400.0f) path.emplace_back(t, o);
            const game::Vec3 an = e->obj->get("angles").asVector();
            const game::GrenadeState& gs = *lob->grenade;
            if (t - gs.aTime >= 100) { // the drawn snapshot carries the server's current angular path
                ++spinChecks;
                for (int k = 0; k < 3; ++k) {
                    const float want = gs.aBase[size_t(k)] + gs.aDelta[size_t(k)] * float(t - gs.aTime) * 0.001f;
                    if (std::fabs(std::remainder(an[size_t(k)] - want, 360.0f)) > 1.0f) ++spinOff;
                }
                if (haveAngles && std::fabs(std::remainder(an[2] - lastDrawnAngles[2], 360.0f)) > 0.5f) ++spinMoves;
            }
            lastDrawnAngles = an;
            haveAngles = true;
        }
    }
    int reversals = 0, holds = 0, longestHold = 0, hold = 0;
    for (size_t i = 2; i < path.size(); ++i) {
        game::Vec3 d1{}, d2{};
        for (int k = 0; k < 3; ++k) {
            d1[size_t(k)] = path[i - 1].second[size_t(k)] - path[i - 2].second[size_t(k)];
            d2[size_t(k)] = path[i].second[size_t(k)] - path[i - 1].second[size_t(k)];
        }
        const float l1 = std::sqrt(d1[0] * d1[0] + d1[1] * d1[1] + d1[2] * d1[2]), l2 = std::sqrt(d2[0] * d2[0] + d2[1] * d2[1] + d2[2] * d2[2]);
        const float dot = d1[0] * d2[0] + d1[1] * d2[1] + d1[2] * d2[2];
        if (l1 > 2.0f && l2 > 2.0f && dot < -0.5f * l1 * l2) ++reversals;  // straight back along the path: an overshoot undone
        if (l2 < 0.05f) {
            ++hold;
            longestHold = std::max(longestHold, hold);
        } else {
            if (hold) ++holds;
            hold = 0;
        }
    }
    std::cout << "lob: " << path.size() << " drawn frames, " << rebases << " server bounces, " << reversals << " reversals, " << holds
              << " holds (longest " << longestHold << " frames)\n";
    CHECK(path.size() >= 40);
    CHECK(rebases >= 1);
    CHECK(reversals == 0);
    CHECK(longestHold <= 4);  // at most one server frame (50 ms) at a bounce
    std::cout << "lob: " << serverBounces << " bounce events on the server, " << heard.size() << " bounce sounds on the client;";
    for (const Heard& h : heard) std::cout << " " << h.alias;
    std::cout << "; angles checked in " << spinChecks << " frames, " << spinOff << " off the server's, the roll turning in " << spinMoves << "\n";
    CHECK(serverBounces >= 1);
    CHECK(heard.size() == serverBounces);
    for (const Heard& h : heard) {  // at the drawn grenade, near the walls it hit
        bool nearPath = false;
        for (const auto& [pt, po] : path)
            nearPath = nearPath || std::fabs(po[0] - h.at[0]) + std::fabs(po[1] - h.at[1]) + std::fabs(po[2] - h.at[2]) < 1.0f;
        CHECK(nearPath);
    }
    CHECK(spinChecks >= 10);
    CHECK(spinOff == 0);
    CHECK(spinMoves * 10 >= spinChecks * 9);
    g.client->disconnect();
}

} // namespace

int main() {
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cerr << "IW2_MAIN not set\n";
        return 1;
    }
    fs::Vfs vfs;
    vfs.mountDirectory(main);
    run(vfs, false);
    run(vfs, true);
    std::cout << (failures ? "mpnetgrenade_e2e: FAILED\n" : "mpnetgrenade_e2e: ok\n");
    return failures ? 1 : 0;
}
