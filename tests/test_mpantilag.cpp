// End to end, headless (docs/spec/mp-hud.md "Antilag"): a network client whose commands carry a server time 100 or
// 200 ms behind the server (the snapshot it was looking at) aims at a moving network player where its screen shows it
// and fires one aimed shot (the weapon's spread set to 0). With g_antilag 1 the server moves the players back to their archived origins at that time
// for the bullet and the target is hit; with g_antilag 0 the bullet goes against the current position and misses.
// Also checks the archived origin interpolation between two server frames. Needs IW2_MAIN.
#include <cmath>
#include <cstdlib>
#include <iostream>
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

float health(Entity* e) {
    const Value v = e ? e->obj->get("health") : Value();
    return v.isNumber() ? v.asFloat() : -1;
}
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

    Game(fs::Vfs& vfs, const char* gt) : host(vfs, "mp_toujane", true), game(nullptr, "", "", 7, 8), server(game, 5) {
        host.vm().setLoopGuardIterations(200000u);
        host.cvars["mapname"] = "mp_toujane";
        host.mpStart(gt);
        const int local = host.mpConnect("host");
        host.mpBegin(local);
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
            if (clients[i]->state() >= net::ClientState::Primed) {
                const net::ClientSnapshot* ls = clients[i]->latest();
                net::UserCmd c = ls ? netgame::viewCommand(cmds[i], ls->ps) : cmds[i]; // the angles of cmds are the client's view; the command carries the view minus delta_angles (docs/spec/net.md section 10)
                c.serverTime = int(host.timeMs()) - lag[i] + (lag[i] ? 1 : 0);
                clients[i]->sendCommand(c, now);
            }
            clients[i]->poll(now, 1);
            (void)clients[i]->takeServerCommands();
        }
        server.poll(now, 0);
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

constexpr float kHalfPath = 80.0f;   // the target runs 160 units across the shooter's view
constexpr float kSpeed = 400.0f;     // units per second: 40 / 80 units in 100 / 200 ms

struct Lane {
    Vec3 shooter{}, center{}, side{};
};

// A place where the shooter at 200 units sees the whole path of the target and the path is free.
bool findLane(Game& g, const Vec3& c, int skip, Lane& out) {
    for (int k = 0; k < 16; ++k) {
        const float a = float(k) * float(M_PI) / 8.0f;
        const Vec3 d{std::cos(a), std::sin(a), 0}, side{-std::sin(a), std::cos(a), 0};
        const Vec3 s = add(c, d, 200.0f);
        bool ok = g.clear(add(c, {0, 0, 40}), add(s, {0, 0, 40}), skip);
        for (float t = -kHalfPath - 20; ok && t <= kHalfPath + 20; t += 20) {
            const Vec3 p = add(c, side, t);
            ok = g.clear(add(s, {0, 0, 60}), add(p, {0, 0, 40}), skip) && g.clear(add(c, {0, 0, 40}), add(p, {0, 0, 40}), skip) &&
                 g.clear(add(p, {0, 0, 40}), add(p, {0, 0, -8}), skip) == false;  // ground under the path
        }
        if (ok) {
            out = {s, c, side};
            return true;
        }
    }
    return false;
}

// One aimed shot of client 0 (lag ms behind) at client 1 running across; returns whether the bullet hit the target.
bool shot(Game& g, int a, int b, const Lane& lane, int lagMs, bool antilag, float speed = kSpeed) {
    g.host.cvars["g_antilag"] = antilag ? "1" : "0";
    Entity* ea = g.host.mpClientEntity(a);
    Entity* eb = g.host.mpClientEntity(b);
    ea->obj->set("origin", Value(lane.shooter));
    // The target's health comes back through the gametype's regeneration (5 s after the last hit), not by a field set:
    // the regeneration loop would miss a hit that drops health to the value it last saw (docs/spec/mp.md).
    for (int i = 0; i < 140 && health(g.host.mpClientEntity(b)) < 100; ++i) g.frame();
    eb = g.host.mpClientEntity(b);
    CHECK(health(eb) == 100);
    g.lag[0] = lagMs;
    g.cmds[0].buttons = int32_t(game::Button::Ads);
    // aim at the path centre while the aim comes up (the lag change drops commands until the server time catches up)
    const auto aim = [&](const Vec3& at) {
        const Vec3 o = origin(g.host.mpClientEntity(a));
        const float dx = at[0] - o[0], dy = at[1] - o[1], dz = at[2] - (o[2] + 60.0f);
        g.cmds[0].angles[1] = angleShort(std::atan2(dy, dx) * 180.0f / float(M_PI));
        g.cmds[0].angles[0] = angleShort(-std::atan2(dz, std::sqrt(dx * dx + dy * dy)) * 180.0f / float(M_PI));
    };
    for (int i = 0; i < 30; ++i) {
        eb->obj->set("origin", Value(add(lane.center, lane.side, -kHalfPath)));
        aim(add(lane.center, {0, 0, 40}));
        g.frame();
    }
    // the run: the archived origins by server time are what the shooter's screen shows `lag` ms later
    std::map<int64_t, Vec3> seen;
    // fire when the shooter's screen shows the target at the path centre (the live one is up to 100 units further)
    const int fireAt = int(kHalfPath / kSpeed * 20.0f) + lagMs / 50;
    const int frames = fireAt + 3;
    const float before = health(eb);
    const netgame::NetCombat* nc = g.game.combat(g.slotOf(a));
    const int shotsBefore = nc ? nc->shots() : 0;
    const int hitsBefore = g.host.stats.hits;
    bool fired = false;
    for (int i = 0; i <= frames; ++i) {
        eb = g.host.mpClientEntity(b);
        eb->obj->set("origin", Value(add(lane.center, lane.side, -kHalfPath + speed * 0.05f * float(i))));
        const int64_t screen = g.host.timeMs() - lagMs;  // the command carries screen + 1 (between frames, see the spec)
        g.cmds[0].buttons = int32_t(game::Button::Ads);
        if (!fired && i >= fireAt && seen.count(screen)) {
            aim(add(seen[screen], {0, 0, 40}));  // the chest where the shooter sees the target
            g.cmds[0].buttons |= int32_t(game::Button::Attack);
            fired = true;
        }
        g.frame();
        seen[g.host.timeMs()] = origin(g.host.mpClientEntity(b));
    }
    g.cmds[0].buttons = 0;
    for (int i = 0; i < 3; ++i) g.frame();
    const float after = health(g.host.mpClientEntity(b));
    const int shotsMade = nc ? nc->shots() - shotsBefore : 0;
    const int hits = g.host.stats.hits - hitsBefore;  // bullets that hit an entity (only the target can be hit)
    std::cout << "hits " << hits << " shots " << shotsMade << " weapon " << (nc ? nc->weapon().name() : "") << " clip " << (nc ? nc->weapon().clip() : -1) << "\n";
    std::cout << "lag " << lagMs << " g_antilag " << antilag << " fired " << fired << " health " << before << " -> " << after
              << "\n";
    CHECK(fired);
    CHECK(shotsMade == 1);
    // Every hit takes health off (no damage debounce, docs/spec/mp.md).
    if (g.host.stats.hits > hitsBefore) CHECK(after < before);
    return hits > 0;
}

void run(fs::Vfs& vfs) {
    Game g(vfs, "dm");
    g.host.mpAutoJoin(0);
    if (!g.server.open(0)) {
        std::cerr << "cannot open a UDP port\n";
        ++failures;
        return;
    }
    const int a = g.connect("shooter");
    const int b = g.connect("target");
    CHECK(a > 0 && b > 0 && a != b);
    if (a <= 0 || b <= 0) return;
    g.host.mpAutoJoin(a);
    g.host.mpAutoJoin(b);
    for (int i = 0; i < 100; ++i) g.frame();
    CHECK(g.host.cvars["g_antilag"] == "1");                        // registered on by default
    CHECK(g.host.mpArchive() && g.host.mpArchive()->enabled);      // the killcam script turned the archive on
    const netgame::NetCombat* ca = g.game.combat(g.slotOf(a));
    CHECK(ca && ca->weapon().equipped());
    Entity* eb = g.host.mpClientEntity(b);
    CHECK(eb && eb->obj->get("sessionstate").toString() == "playing");
    // No aim spread for the shooter's weapon (the test's only change to the weapon data), so every shot is deterministic.
    if (ca) {
        auto* def = const_cast<std::map<std::string, std::string>*>(g.host.weapon(ca->weapon().name()));
        CHECK(def);
        if (def)
            for (auto& [k, v] : *def)
                if (k.find("spread") != std::string::npos) v = "0";
    }

    // The archived origin between two frames: the target moves 20 units per frame; 25 ms after a frame is half way.
    {
        const Vec3 p0 = origin(eb);
        g.frame();
        const int64_t t0 = g.host.timeMs();
        const Vec3 at0 = origin(g.host.mpClientEntity(b));
        g.host.mpClientEntity(b)->obj->set("origin", Value(add(at0, {20, 0, 0})));
        g.frame();
        const Vec3 at1 = origin(g.host.mpClientEntity(b));
        g.frame();
        g.frame();
        Vec3 o{};
        CHECK(g.host.mpArchivedClientOrigin(b, int(t0), o));
        std::cout << "archived " << o[0] << " live then " << at0[0] << "\n";
        CHECK(std::fabs(o[0] - at0[0]) < 0.01f);
        CHECK(g.host.mpArchivedClientOrigin(b, int(t0) + 25, o));
        CHECK(std::fabs(o[0] - (at0[0] + at1[0]) * 0.5f) < 0.01f);
        (void)p0;
    }

    Lane lane;
    bool found = false;  // around the dm spawn points
    for (const auto& up : g.host.entities()) {
        if (found) break;
        if (up->classname != "mp_dm_spawn" || !up->obj->get("origin").isVector()) continue;
        found = findLane(g, up->obj->get("origin").asVector(), -1, lane);
    }
    CHECK(found);
    if (!found) return;
    CHECK(shot(g, a, b, lane, 0, true, 0.0f));
    // Three shots each: with antilag every one hits, without it none does.
    for (int lagMs : {100, 150, 200}) {
        int on = 0, off = 0;
        for (int k = 0; k < 3; ++k) {
            on += shot(g, a, b, lane, lagMs, true) ? 1 : 0;
            off += shot(g, a, b, lane, lagMs, false) ? 1 : 0;
        }
        std::cout << "lag " << lagMs << ": hits with g_antilag 1: " << on << "/3, with 0: " << off << "/3\n";
        CHECK(on == 3);
        CHECK(off == 0);
    }
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
    run(vfs);
    if (failures) std::cerr << failures << " check(s) failed\n";
    else std::cout << "mpantilag: ok\n";
    return failures ? 1 : 0;
}
