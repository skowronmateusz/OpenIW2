// End to end, headless (docs/spec/mp-hud.md "The recorded view state"): the killcam's recorded view state.
// 1. A bot throws its grenade through the offhand states of the shared weapon state machine: the record passes the
//    pull-back (state 13, holdFireAnim of the grenade) and the throw (state 15, fireAnim), the event ring gets the
//    prepare and throw events, and the grenade leaves with the solved velocity at the throw event.
// 2. The network killcam is the local one: a network client following the bot decodes from its snapshots the same
//    view (eye, angles, weapon, hands, ammunition, health, the whole recorded view state and event ring) that the
//    server's local killcam view gives for the same archived frame, and the followed client's number.
// Needs IW2_MAIN.
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "fs/vfs.h"
#include "game/mpbots.h"
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

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
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
    void step() {
        client->poll(now, 1);
        if (client->state() >= net::ClientState::Primed) {
            net::UserCmd c;
            c.serverTime = client->commandTime(now);
            client->sendCommand(c, now);
        }
        (void)client->takeServerCommands();
        server.poll(now, 0);
        host.frame();
        server.poll(now, 0);
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

std::string field(World& w, const std::string& weapon, const char* key) {
    const auto* d = w.weapon(weapon);
    if (!d) return {};
    const auto it = d->find(lower(key));
    return it == d->end() ? std::string() : it->second;
}

// The frag grenade the bot owns.
std::string fragOf(World& w, Entity& e) {
    for (const std::string& n : w.inventory(e).owned)
        if (lower(field(w, n, "offhandClass")) == "frag grenade") return n;
    return {};
}

Entity* grenadeOf(World& w, Entity& thrower) {
    for (const auto& e : w.entities())
        if (!e->obj->removed && e->grenade && e->grenade->parent == &thrower) return e.get();
    return nullptr;
}

void startKillcam(Entity& victim, Entity& killer, float seconds) {
    victim.obj->set("sessionstate", Value("spectator"));
    victim.obj->set("spectatorclient", Value(int32_t(killer.number)));
    victim.obj->set("archivetime", Value(seconds));
}

bool near3(const game::Vec3& a, const game::Vec3& b, float eps) {
    return std::fabs(a[0] - b[0]) < eps && std::fabs(a[1] - b[1]) < eps && std::fabs(a[2] - b[2]) < eps;
}
float angleDiff(float a, float b) { return std::fabs(std::remainder(a - b, 360.0f)); }

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
    const int bot = g.host.mpAddBot();
    CHECK(bot > 0);
    g.host.mpBotSetCombat(bot, false); // the bot walks but never shoots: the record below is its throw and its moves
    call(g.host, "setarchive", {Value(true)});
    // The bot joins (team and weapon menus) and spawns with its loadout.
    game::MpBot* b = nullptr;
    for (int i = 0; i < 400; ++i) {
        g.step();
        b = g.host.mpBot(bot);
        Entity* be = g.host.mpClientEntity(bot);
        if (b && be && be->obj->get("sessionstate").toString() == "playing" && b->weapon.equipped() && b->weapon.stateNumber() == 0 && i > 80) break;
    }
    Entity* be = g.host.mpClientEntity(bot);
    Entity* alpha = g.host.mpClientEntity(a);
    CHECK(b && be && alpha && b->weapon.equipped());
    if (!b || !be || !alpha || !b->weapon.equipped()) return;
    const std::string frag = fragOf(g.host, *be);
    std::cout << "bot weapon " << b->weapon.name() << " frag " << frag << "\n";
    CHECK(!frag.empty());
    if (frag.empty()) return;

    // --- 1. The throw through the offhand states. ---
    const int clipBefore = g.host.inventory(*be).clip[field(g.host, frag, "clipName")];
    const std::array<float, 3> vel{300.0f, 100.0f, 400.0f};
    b->throwPending = {true, false, frag, vel};
    int prepareFrames = 0, throwFrames = 0, holdFrames = 0;
    bool sawPrep = false, sawUse = false;
    std::string prepAnim, throwAnim, prepViewWeapon;
    Entity* gren = nullptr;
    int64_t throwMs = 0;
    int states[32] = {};
    for (int i = 0; i < 100; ++i) {
        g.step();
        const game::MpClient& cl = g.host.mpState().clients[size_t(bot)];
        const game::MpViewState& vs = cl.view;
        ++states[vs.weaponState & 31];
        if (vs.weaponState == 13) { ++prepareFrames; prepAnim = vs.animKey; prepViewWeapon = vs.viewWeapon; }
        if (vs.weaponState == 14) ++holdFrames;
        if (vs.weaponState == 15) { ++throwFrames; throwAnim = vs.animKey; }
        for (int k = 0; k < 4; ++k) {
            if (cl.events.events[size_t(k)] == game::mpev::kPrepOffhand && cl.events.weapons[size_t(k)] == frag) sawPrep = true;
            if (cl.events.events[size_t(k)] == game::mpev::kUseOffhand && cl.events.weapons[size_t(k)] == frag) sawUse = true;
        }
        if (!gren) {
            gren = grenadeOf(g.host, *be);
            if (gren) throwMs = g.host.timeMs();
        }
        if (gren && b->weapon.stateNumber() == 0 && i > 20) break;
    }
    std::cout << "throw: prepare frames " << prepareFrames << " (anim " << prepAnim << ", view weapon " << prepViewWeapon << "), hold " << holdFrames
              << ", throw frames " << throwFrames << " (anim " << throwAnim << "), events prep " << sawPrep << " use " << sawUse << ", grenade "
              << (gren != nullptr) << "\n";
    CHECK(prepareFrames >= 1 && prepAnim == "holdFireAnim" && prepViewWeapon == frag);
    CHECK(throwFrames >= 1 && throwAnim == "fireAnim");
    CHECK(sawPrep && sawUse);
    CHECK(gren != nullptr);
    if (gren) {
        CHECK(gren->grenade->weapon == frag);
        CHECK(near3(gren->grenade->trDelta, {vel[0], vel[1], vel[2]}, 0.01f));
    }
    CHECK(!b->throwPending.valid);
    CHECK(g.host.inventory(*be).clip[field(g.host, frag, "clipName")] == clipBefore - 1);
    CHECK(b->stats.grenades == 1);
    (void)throwMs;

    // --- 2. The network killcam equals the local killcam view of the same archived frame. ---
    // Another throw during the archived window, so the replay carries offhand states and the view weapon switch.
    for (int i = 0; i < 20; ++i) g.step();
    b->throwPending = {true, false, frag, vel};
    for (int i = 0; i < 40; ++i) g.step();
    // The host's local killcam view of alpha's follow (what the server's own front end would draw), by server time.
    std::map<int, World::MpKillcamView> local;
    startKillcam(*alpha, *be, 3.5f);
    int compared = 0, mismatched = 0, offhandFrames = 0, movingFrames = 0, followSnaps = 0;
    netgame::RemoteView rv;
    World view(vfs, kMap, true);
    view.mpStartRemote();
    for (int i = 0; i < 90; ++i) {
        g.step();
        local[int(g.host.timeMs())] = g.host.mpKillcamViewFor(a, 1.0f);
        const net::ClientSnapshot* s = g.client->latest();
        if (!s || !netgame::RemoteView::following(*s)) {
            if (followSnaps > 0) break;
            continue;
        }
        ++followSnaps;
        const auto lit = local.find(s->serverTime);
        if (lit == local.end() || !lit->second.active) continue;
        const World::MpKillcamView& lv = lit->second;
        const World::MpKillcamView nv = rv.killcamView(*s, *s, 1.0f, g.client->configstrings, view.timeMs());
        CHECK(nv.active);
        if (!nv.active) continue;
        ++compared;
        bool same = near3(nv.eye, lv.eye, 0.01f) && angleDiff(nv.angles[0], lv.angles[0]) < 0.01f && angleDiff(nv.angles[1], lv.angles[1]) < 0.01f &&
                    nv.weapon == lv.weapon && nv.hands == lv.hands && nv.clip == lv.clip && nv.stock == lv.stock && nv.health == lv.health &&
                    nv.maxHealth == lv.maxHealth && nv.client == lv.client && nv.client == bot && nv.view == lv.view && nv.ring == lv.ring;
        if (!same) {
            ++mismatched;
            if (mismatched <= 3) {
                std::cout << "mismatch at " << s->serverTime << ": eye " << nv.eye[0] << "," << nv.eye[1] << "," << nv.eye[2] << " / " << lv.eye[0] << ","
                          << lv.eye[1] << "," << lv.eye[2] << " angles " << nv.angles[0] << "," << nv.angles[1] << " / " << lv.angles[0] << "," << lv.angles[1]
                          << " weapon " << nv.weapon << "/" << lv.weapon << " hands " << nv.hands << "/" << lv.hands << " ammo " << nv.clip << "+" << nv.stock
                          << " / " << lv.clip << "+" << lv.stock << " health " << nv.health << "/" << lv.health << " client " << nv.client << "/" << lv.client
                          << " ring " << nv.ring.sequence << "/" << lv.ring.sequence << "\n  net   " << game::mpViewStateText(nv.view) << "\n  local "
                          << game::mpViewStateText(lv.view) << "\n";
                for (int k = 0; k < 4; ++k)
                    if (nv.ring.events[size_t(k)] != lv.ring.events[size_t(k)] || nv.ring.weapons[size_t(k)] != lv.ring.weapons[size_t(k)])
                        std::cout << "  ring[" << k << "] " << nv.ring.events[size_t(k)] << " '" << nv.ring.weapons[size_t(k)] << "' / " << lv.ring.events[size_t(k)]
                                  << " '" << lv.ring.weapons[size_t(k)] << "'\n";
            }
        }
        if (nv.view.weaponState >= 12 && nv.view.weaponState <= 16) ++offhandFrames;
        if (std::fabs(nv.view.velocity[0]) + std::fabs(nv.view.velocity[1]) > 1.0f) ++movingFrames;
    }
    std::cout << "net killcam: " << followSnaps << " follow snapshots, " << compared << " compared with the local view, " << mismatched
              << " mismatched, offhand frames " << offhandFrames << ", moving frames " << movingFrames << "\n";
    CHECK(followSnaps >= 40);
    CHECK(compared >= 30);
    CHECK(mismatched == 0);
    CHECK(offhandFrames >= 1); // the replay reached the second throw's offhand states
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
    std::cout << (failures ? "mpkillcam_record_e2e: FAILED\n" : "mpkillcam_record_e2e: ok\n");
    return failures ? 1 : 0;
}
