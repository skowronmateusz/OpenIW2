// End to end, headless (docs/spec/mp-hud.md "Killcam"): a grenade in the killcam. The archive keeps a thrown grenade
// with its trajectory and its explosion; a killcam that shows the frames of its flight draws it, also after the live
// grenade exploded and was freed, and replays the explosion once. The victim's own shell shock is not shown in the
// killcam (the killcam copies the killer's archived player state). Both on a network client (the server sends the
// archived grenade and the explosion event) and for the local client. Needs IW2_MAIN.
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "fs/vfs.h"
#include "game/mpkillcam.h"
#include "game/world.h"
#include "net/gameclient.h"
#include "net/server.h"
#include "netgame/netmove.h"
#include "netgame/remoteview.h"
#include "netgame/worldgame.h"
#include "script/vm.h"

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
using script::Value;

constexpr const char* kMap = "mp_carentan";
constexpr const char* kGrenade = "frag_grenade_american_mp";

game::Vec3 origin(Entity* e) { return e ? e->obj->get("origin").asVector() : game::Vec3{0, 0, 0}; }
float dist(const game::Vec3& a, const game::Vec3& b) {
    return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}
std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
int getI(const std::vector<uint8_t>& b, int off) {
    int32_t v = 0;
    std::memcpy(&v, b.data() + off, 4);
    return v;
}
Value call(World& w, const std::string& fn, std::vector<Value> args, Value self = Value()) {
    script::Thread t;
    script::CallContext c{w.vm(), t, self, std::span<Value>(args)};
    return w.vm().builtin(fn)(c);
}

struct Game {
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
        client->poll(now, 1);
        if (client->state() >= net::ClientState::Primed) {
            net::UserCmd c;
            c.serverTime = client->commandTime(now);
            client->sendCommand(c, now);
        }
        (void)client->takeServerCommands();
    }
    void serverFrame() {
        server.poll(now, 0);
        host.frame();
        server.poll(now, 0);
    }
    void step() {
        clientStep();
        serverFrame();
        now += 50;
    }
    int connect(const std::string& name) {
        client = std::make_unique<net::GameClient>();
        client->name = name;
        net::NetAddress a;
        net::NetAddress::parse("127.0.0.1:" + std::to_string(server.port()), a, 28960);
        client->connect(a, now);
        for (int i = 0; i < 200 && client->state() != net::ClientState::Active; ++i) step();
        return client->state() == net::ClientState::Active ? client->clientNum : -1;
    }
};

Entity* liveGrenade(World& w) {
    for (const auto& e : w.entities())
        if (!e->obj->removed && e->grenade) return e.get();
    return nullptr;
}

// Puts `victim` into a killcam of `killer` with a fixed delay (the scripts' countdown is not run here).
void startKillcam(Entity& victim, Entity& killer, float seconds) {
    victim.obj->set("sessionstate", Value("spectator"));
    victim.obj->set("spectatorclient", Value(int32_t(killer.number)));
    victim.obj->set("archivetime", Value(seconds));
}
void endKillcam(Entity& victim) {
    victim.obj->set("spectatorclient", Value(int32_t(-1)));
    victim.obj->set("archivetime", Value(0.0f));
}

void run(fs::Vfs& vfs) {
    Game g(vfs);
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
    for (int i = 0; i < 60; ++i) g.step();
    call(g.host, "setarchive", {Value(true)});
    call(g.host, "precacheshellshock", {Value("default")});
    Entity* alpha = g.host.mpClientEntity(a);
    Entity* hostPlayer = g.host.player();
    CHECK(alpha && hostPlayer);
    if (!alpha || !hostPlayer) return;

    // --- The network killcam: the host throws, alpha (shell shocked) watches the host's killcam. ---
    {
        const game::Vec3 eye = origin(hostPlayer);
        g.host.playerThrowGrenade(kGrenade, {eye[0], eye[1], eye[2] + 660.0f}, {-89.0f, 0.0f, 0.0f}, {0, 0, 0});
        Entity* gren = liveGrenade(g.host);
        CHECK(gren != nullptr);
        if (!gren) return;
        const int64_t throwMs = g.host.timeMs();
        std::map<int64_t, game::Vec3> flight;  // server time -> the grenade's origin
        for (int i = 0; i < 200 && !gren->obj->removed; ++i) {
            g.step();
            if (!gren->obj->removed) flight[g.host.timeMs()] = origin(gren);
        }
        CHECK(gren->obj->removed);
        call(g.host, "shellshock", {Value("default"), Value(30.0f)}, Value(alpha->obj));
        for (int i = 0; i < 4; ++i) g.step();
        const int64_t delayMs = g.host.timeMs() - throwMs + 1000;
        startKillcam(*alpha, *hostPlayer, float(delayMs) * 0.001f);

        World view(vfs, kMap, true);
        view.mpStartRemote();
        netgame::RemoteView rv;
        int follow = 0, shocked = 0, withGrenade = 0, onPath = 0, offPath = 0;
        std::set<std::pair<int, int>> explosions;
        for (int i = 0; i < int(delayMs / 50) + 20; ++i) {
            g.step();
            const net::ClientSnapshot* s = g.client->latest();
            if (!s || !s->valid || !(getI(s->ps, 12) & netgame::kPmfFollow)) continue;
            ++follow;
            if (getI(s->ps, 1496) != 0) ++shocked;
            rv.apply(view, *s, g.client->configstrings, a, view.timeMs());
            for (const auto& [number, es] : s->entities) {
                const int type = getI(es, 4) & 0xff;
                if (type == netgame::kEtEvents + netgame::kEventGrenadeExplode) explosions.insert({number, getI(es, 84)});
                if (type != netgame::kEtGeneral) continue;
                const netgame::Trajectory tr = netgame::readTrajectory(es, netgame::kPosTrajectory);
                if (!netgame::trajectoryMoves(tr)) continue;
                ++withGrenade;
                // The drawn place at the snapshot's time is the archived flight in the shown frame: the lookup takes the
                // first archived frame from now - delay on (the frame after it, as the newest frame is the current one).
                float o[3];
                netgame::evaluateTrajectory(tr, s->serverTime, o);
                const auto it = flight.find(int64_t(s->serverTime) - delayMs + 50);
                if (it == flight.end()) continue;
                if (dist({o[0], o[1], o[2]}, it->second) < 4.0f) ++onPath;
                else ++offPath;
            }
        }
        int drawn = 0;
        for (const auto& e : view.entities())
            if (!e->obj->removed && e->obj->get("model").toString().rfind("xmodel/projectile_", 0) == 0) ++drawn;
        std::cout << "net killcam: " << follow << " follow snapshots, " << shocked << " with a shell shock, " << withGrenade
                  << " with the archived grenade (" << onPath << " on its archived path, " << offPath << " off), "
                  << explosions.size() << " explosion events\n";
        CHECK(follow > 20);
        CHECK(shocked == 0);
        CHECK(withGrenade >= 10);
        CHECK(onPath >= 10);
        CHECK(offPath == 0);
        CHECK(explosions.size() == 1);
        (void)drawn;
        endKillcam(*alpha);
        for (int i = 0; i < 4; ++i) g.step();
        // Out of the killcam alpha's own shell shock is sent again (until a spawn clears it).
        const net::ClientSnapshot* s = g.client->latest();
        CHECK(s && getI(s->ps, 1496) != 0);
        call(g.host, "stopshellshock", {}, Value(alpha->obj));
        for (int i = 0; i < 40; ++i) g.step();  // the killer's archived frames of the next killcam without its shock
    }

    // --- The local killcam: alpha throws, the host (shell shocked) watches alpha's killcam. ---
    {
        const game::Vec3 eye = origin(alpha);
        g.host.clientThrowGrenade(*alpha, kGrenade, {eye[0], eye[1], eye[2] + 660.0f}, {-89.0f, 0.0f, 0.0f}, {0, 0, 0});
        Entity* gren = liveGrenade(g.host);
        CHECK(gren != nullptr);
        if (!gren) return;
        const int grenNumber = gren->number;
        const int64_t throwMs = g.host.timeMs();
        for (int i = 0; i < 200 && !gren->obj->removed; ++i) g.step();
        CHECK(gren->obj->removed);
        call(g.host, "shellshock", {Value("default"), Value(30.0f)}, Value(hostPlayer->obj));
        CHECK(g.host.shellShock().index != 0);
        for (int i = 0; i < 4; ++i) g.step();
        (void)g.host.takeFxEvents();
        const int64_t delayMs = g.host.timeMs() - throwMs + 1000;
        startKillcam(*hostPlayer, *alpha, float(delayMs) * 0.001f);
        int inKillcam = 0, shocked = 0, drawn = 0, explosionFx = 0;
        for (int i = 0; i < int(delayMs / 50) + 20; ++i) {
            g.step();
            if (!g.host.mpLocalInKillcam()) continue;
            ++inKillcam;
            if (g.host.shellShock().index != 0) ++shocked;
            if (g.host.mpKillcamApply(0.5f)) {
                for (const auto& e : g.host.entities())
                    if (e->number == grenNumber && !e->obj->removed && !e->obj->get("$hidden").truthy() &&
                        e->obj->get("model").toString().rfind("xmodel/projectile_", 0) == 0)
                        ++drawn;
                g.host.mpKillcamRestore();
            }
            for (const auto& fx : g.host.takeFxEvents())
                if (lower(fx.path).find("exp") != std::string::npos) ++explosionFx;
        }
        CHECK(gren->obj->removed);  // the drawing restored the freed entity's state
        std::cout << "local killcam: " << inKillcam << " frames, " << shocked << " with a shell shock, grenade drawn in " << drawn
                  << ", explosion effects " << explosionFx << "\n";
        CHECK(inKillcam > 20);
        CHECK(shocked == 0);
        CHECK(drawn >= 10);
        CHECK(explosionFx >= 1);
        endKillcam(*hostPlayer);
        CHECK(g.host.shellShock().index != 0);  // the victim's own shock again after the killcam
    }
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
    run(vfs);
    std::cout << (failures ? "mpkillcam_grenade_e2e: FAILED\n" : "mpkillcam_grenade_e2e: ok\n");
    return failures ? 1 : 0;
}
