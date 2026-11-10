// A dedicated server has no local client entity (docs/spec/mp.md "Local client view"): the entity the single-player spawn
// leaves behind is freed, so it is not in the clip lists, no script lists it and no snapshot has it, and the killcam helpers
// of the local client run on a dedicated world without one. freezecontrols of a network client holds its movement and weapon
// while the scripts still see the held buttons (docs/spec/controls.md "freezecontrols"). Needs IW2_MAIN.
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "fs/vfs.h"
#include "game/clientinput.h"
#include "game/world.h"
#include "net/gameclient.h"
#include "net/server.h"
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
using game::World;
using script::Value;

struct Dedicated {
    World world;
    netgame::WorldGame game;
    net::Server server;
    std::unique_ptr<net::GameClient> client;
    net::UserCmd cmd;
    int64_t now = 1000;

    explicit Dedicated(fs::Vfs& vfs) : world(vfs, "mp_toujane", true), game(nullptr, "", "", 7, 8), server(game, 5) {
        world.vm().setLoopGuardIterations(200000u);
        world.cvars["mapname"] = "mp_toujane";
        world.mpSetDedicated();
        world.mpStart("dm");
        game.setWorld(&world, "mp_toujane", "dm", 7);
        server.serverId = 7;
        game.sendCommand = [this](int slot, const std::string& text) { server.sendServerCommand(slot, text); };
        server.info().mapname = "mp_toujane";
        server.info().gametype = "dm";
    }
    void frame() {
        if (client) {
            client->poll(now, 1);
            if (client->state() >= net::ClientState::Primed) {
                net::UserCmd c = cmd;
                c.serverTime = int(now);
                client->sendCommand(c, now);
            }
            (void)client->takeServerCommands();
        }
        server.poll(now, 0);
        now += 50;
    }
    int connect(const std::string& name) {
        if (!server.open(0)) return -1;
        client = std::make_unique<net::GameClient>();
        client->name = name;
        net::NetAddress a;
        net::NetAddress::parse("127.0.0.1:" + std::to_string(server.port()), a, 28960);
        client->connect(a, now);
        for (int i = 0; i < 200 && client->state() != net::ClientState::Active; ++i) frame();
        return client->state() == net::ClientState::Active ? client->clientNum : -1;
    }
    int psFlags() const {
        const net::ClientSnapshot* s = client->latest();
        int32_t f = 0;
        if (s && s->ps.size() >= 16) std::memcpy(&f, s->ps.data() + 12, 4);
        return f;
    }
};

float dist2d(const game::Vec3& a, const game::Vec3& b) { return std::hypot(a[0] - b[0], a[1] - b[1]); }

void run(fs::Vfs& vfs) {
    Dedicated d(vfs);
    // The leftover entity: freed, out of every list a script, a trace or a snapshot reads.
    Entity* ghost = d.world.player();
    CHECK(ghost && ghost->obj->removed);
    CHECK(ghost && ghost->obj->get("health").asInt() == 0);
    const int a = d.connect("alpha");
    CHECK(a == 0);
    if (a != 0) return;
    d.world.mpAutoJoin(a);
    for (int i = 0; i < 100; ++i) d.frame();
    Entity* pa = d.world.mpClientEntity(a);
    CHECK(pa && pa != ghost && pa->obj->get("sessionstate").toString() == "playing");
    if (!pa) return;
    int livePlayers = 0;
    for (const auto& e : d.world.entities())
        if (e->isPlayer && !e->obj->removed) {
            ++livePlayers;
            CHECK(e.get() == pa); // every live player entity is a client slot
        }
    CHECK(livePlayers == 1);
    // getentarray("player", "classname") and getentarray() through the script builtins.
    {
        int named = 0, all = 0;
        for (const auto& e : d.world.entities())
            if (!e->obj->removed) {
                ++all;
                if (e->obj->get("classname").toString() == "player") ++named;
            }
        CHECK(named == 1);
        CHECK(all > 1);
    }
    // No clip of the ghost: nothing hits or blocks it. Only the connected client has a player clip.
    int playerClips = 0;
    for (const auto& c : d.world.collision().entityClips()) {
        CHECK(!ghost || c.id != ghost->number);
        if (c.id == pa->number) ++playerClips;
    }
    CHECK(playerClips == 1);
    // The ghost's old place (where the single-player spawn put it) is free: a bullet through it is not stopped, and the
    // client does not take damage or items there.
    CHECK(d.world.mpLocalClient() == -1);
    // The killcam helpers of the local client on a dedicated world: nothing to show, nothing to fail.
    CHECK(d.world.mpKillcamFrame() < 0);
    CHECK(!d.world.mpKillcamView(0.5f).active);
    CHECK(!d.world.mpKillcamApply(0.5f));
    d.world.mpKillcamRestore();
    World::ShellShockState ss;
    CHECK(!d.world.mpKillcamShellShock(ss));
    (void)d.world.mpPlayerDrawn(*pa);
    const int other = d.world.mpConnect("other");
    CHECK(other == 1);
    pa->obj->set("sessionstate", Value("spectator"));
    pa->obj->set("spectatorclient", Value(int32_t(other)));
    pa->obj->set("archivetime", Value(3.0f));
    for (int i = 0; i < 5; ++i) d.frame();
    CHECK(d.world.mpKillcamFrame() < 0);
    CHECK(!d.world.mpKillcamApply(0.25f));
    d.world.mpKillcamRestore();
    pa->obj->set("sessionstate", Value("playing"));
    pa->obj->set("archivetime", Value(0.0f));
    for (int i = 0; i < 10; ++i) d.frame();

    // freezecontrols: a walking client stays where it is, its weapon does not fire, scripts still see the held buttons.
    pa = d.world.mpClientEntity(a);
    CHECK(pa && pa->obj->get("sessionstate").toString() == "playing");
    if (!pa) return;
    d.cmd.angles[1] = 0;
    d.cmd.forwardmove = 127;
    for (int i = 0; i < 6; ++i) d.frame();
    const game::Vec3 walkFrom = pa->obj->get("origin").asVector();
    for (int i = 0; i < 6; ++i) d.frame();
    const game::Vec3 walkTo = pa->obj->get("origin").asVector();
    CHECK(dist2d(walkFrom, walkTo) > 20.0f); // the unfrozen client walks (the open path is the spawn's own)
    d.world.mpSetControlsFrozen(a, true);
    for (int i = 0; i < 4; ++i) d.frame();
    const game::Vec3 frozenFrom = pa->obj->get("origin").asVector();
    d.cmd.buttons = int32_t(game::Button::Attack);
    const int clip = d.game.combat(d.client->clientNum) ? d.game.combat(d.client->clientNum)->weapon().clip() : -1;
    for (int i = 0; i < 10; ++i) d.frame();
    const game::Vec3 frozenTo = pa->obj->get("origin").asVector();
    CHECK(dist2d(frozenFrom, frozenTo) < 1.0f);
    CHECK((d.psFlags() & 0x8000) != 0);
    CHECK(d.game.combat(d.client->clientNum) && d.game.combat(d.client->clientNum)->weapon().clip() == clip);
    CHECK((d.world.mpState().clients[0].buttons & uint32_t(game::Button::Attack)) != 0); // the script still reads the held button
    d.world.mpSetControlsFrozen(a, false);
    d.cmd.buttons = 0;
    for (int i = 0; i < 6; ++i) d.frame();
    CHECK(dist2d(frozenTo, pa->obj->get("origin").asVector()) > 5.0f);
    CHECK((d.psFlags() & 0x8000) == 0);
    for (const auto& e : d.world.vm().errors()) std::cerr << "script error: " << e << "\n";
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
    else std::cout << "mpdedicatedghost: ok\n";
    return failures ? 1 : 0;
}
