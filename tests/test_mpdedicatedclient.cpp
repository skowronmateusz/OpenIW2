// A dedicated server has no local player: the first network client takes client 0 and is served like every other
// client (docs/spec/mp.md "Local client view"). Headless on mp_toujane (dm): its weapon pickup hint comes from the
// server's use search, a use press swaps the weapon, and while it watches a killcam the server still logs and sends
// the fire events and explosions of the others. Needs IW2_MAIN.
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
    std::pair<int, std::string> hint() const {
        const net::ClientSnapshot* s = client->latest();
        if (!s || s->ps.size() < 1440) return {-1, ""};
        int32_t type = 0, idx = 0;
        std::memcpy(&type, s->ps.data() + 1432, 4);
        std::memcpy(&idx, s->ps.data() + 1436, 4);
        std::string text;
        if (const auto it = client->configstrings.find(netgame::kHintConfigstrings + idx); idx >= 0 && it != client->configstrings.end()) text = it->second;
        return {type & 0xff, text};
    }
};

void run(fs::Vfs& vfs) {
    Dedicated d(vfs);
    const int a = d.connect("alpha");
    CHECK(a == 0);
    if (a != 0) return;
    CHECK(d.world.mpLocalClient() == -1);
    Entity* pa = d.world.mpClientEntity(a);
    CHECK(pa && pa != d.world.player());
    d.world.mpAutoJoin(a);
    for (int i = 0; i < 100; ++i) d.frame();
    pa = d.world.mpClientEntity(a);
    CHECK(pa && pa->obj->get("sessionstate").toString() == "playing");
    if (!pa) return;
    d.world.takeClientCommands(); // a dedicated server queues nothing for a local front end
    std::string item;
    for (const char* w : {"kar98k_mp", "m1garand_mp", "mosin_nagant_mp", "enfield_mp", "springfield_mp"})
        if (d.world.weapon(w) && !d.world.inventory(*pa).owned.count(w)) { item = w; break; }
    CHECK(!item.empty());
    const game::Vec3 o = pa->obj->get("origin").asVector();
    const std::string before = pa->obj->get("$weapon").toString();
    d.world.debugDropItem(item, {o[0] + 30, o[1], o[2] + 8}, {0, 0, 0});
    bool seen = false, taken = false;
    for (int i = 0; i < 40 && !taken; ++i) {
        // Looks down at the item: the movement keeps the client in place.
        d.cmd.angles[0] = int32_t(30.0f * 65536.0f / 360.0f) & 0xffff;
        d.cmd.angles[1] = 0;
        d.cmd.buttons = (seen && i % 6 < 3) ? int32_t(game::Button::Activate) : 0;
        d.frame();
        const auto h = d.hint();
        if (h.first > 4) seen = true;
        taken = d.world.inventory(*pa).owned.count(item) != 0;
    }
    std::cout << "client 0: hint seen " << seen << " taken " << taken << ", weapon " << before << " -> " << pa->obj->get("$weapon").toString() << "\n";
    CHECK(seen);
    CHECK(taken);
    // The old weapon lies in view: the use search names it, except in the binocular weapon states.
    d.cmd.buttons = 0;
    for (int i = 0; i < 6; ++i) d.frame();
    CHECK(!d.world.mpState().clients[0].useHintWeapon.empty());
    d.world.mpSetClientGround(a, true, 0, 0x12);
    d.world.frame();
    CHECK(d.world.mpState().clients[0].useHintWeapon.empty());
    // Client 0 follows another client in a killcam: on this process nobody watches it, the live effects stay on.
    const int other = d.world.mpConnect("other");
    CHECK(other == 1);
    pa->obj->set("sessionstate", Value("spectator"));
    pa->obj->set("spectatorclient", Value(int32_t(other)));
    pa->obj->set("archivetime", Value(3.0f));
    d.frame();
    CHECK(!d.world.mpLocalInKillcam());
    CHECK(!d.world.mpLiveEffectsHidden());
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
    else std::cout << "mpdedicatedclient: ok\n";
    return failures ? 1 : 0;
}
