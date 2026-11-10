// Per-client player state in multiplayer (docs/spec/mp.md "Per-client player state" and "Client spawn"): the player
// methods called on a bot or another client leave the local player alone (shell shock, view kick, teleports, view
// angles, frozen controls, stance and lean permissions, reverb, player links, blur, input queries); the shell shock of a
// network client travels in its snapshot to its own view; a client spawn restarts the movement state (a player who
// died prone or crouched stands again), also in the network client's prediction. Needs the game files (IW2_MAIN).

#include "game/mpbots.h"
#include "game/world.h"
#include "net/playerstate.h"
#include "netgame/netmove.h"
#include "netgame/remoteview.h"
#include "netgame/worldgame.h"

#include <algorithm>
#include <set>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

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
using script::Vec3;

Value call(World& w, const std::string& fn, std::vector<Value> args, Value self = Value()) {
    script::Thread t;
    script::CallContext c{w.vm(), t, self, std::span<Value>(args)};
    return w.vm().builtin(fn)(c);
}

int getI(const std::vector<uint8_t>& b, int off) {
    int32_t v = 0;
    std::memcpy(&v, b.data() + off, 4);
    return v;
}

// Everything of the local player a player method may change.
struct LocalState {
    World::ShellShockState shock;
    int kickCount = 0;
    Vec3 kickDir{};
    bool frozen = false;
    uint32_t pmFlags = 0;
    bool linked = false;
    size_t commands = 0;
    Vec3 origin{}, angles{};
};
LocalState localState(World& w) {
    LocalState s;
    s.shock = w.shellShock();
    s.kickCount = w.debugDamageBlood();
    s.kickDir = w.debugDamageFrom();
    s.frozen = w.playerControlsFrozen();
    s.pmFlags = w.playerPmFlags();
    s.linked = w.playerLinked();
    s.origin = w.player()->obj->get("origin").asVector();
    s.angles = w.player()->obj->get("angles").asVector();
    return s;
}
bool same(const LocalState& a, const LocalState& b) {
    return a.shock.index == b.shock.index && a.shock.startMs == b.shock.startMs && a.shock.durationMs == b.shock.durationMs &&
           a.kickCount == b.kickCount && a.kickDir == b.kickDir && a.frozen == b.frozen && a.pmFlags == b.pmFlags &&
           a.linked == b.linked && a.origin == b.origin && a.angles == b.angles;
}

void prediction() {
    // A client that died prone (pm_type 6) and is playing again (pm_type 0) starts its prediction from a fresh state.
    game::CollisionWorld none;
    game::PlayerState carry;
    carry.pmType = 6;
    carry.stance = game::Stance::Prone;
    carry.viewHeightTarget = 11;
    carry.viewHeight = 11;
    carry.lean = 0.25f;
    netgame::PsFields server;
    server.pmType = 0;
    server.origin[0] = 100;
    game::PlayerState ps = netgame::predictPlayer(none, server, {}, &carry);
    CHECK(ps.stance == game::Stance::Stand && ps.viewHeightTarget == 60 && ps.lean == 0);
    CHECK(ps.origin[0] == 100);
    // Alive on both sides: the carried movement state stays.
    carry.pmType = 0;
    ps = netgame::predictPlayer(none, server, {}, &carry);
    CHECK(ps.stance == game::Stance::Prone);
    // The bob cycle starts from the server's (ps+8), not from the carried prediction: replaying the same unacknowledged
    // commands on two frames gives the same cycle (no drift), and the snapshot field round-trips.
    carry.bobCycle = 200;
    server.bobCycle = 17;
    ps = netgame::predictPlayer(none, server, {}, &carry);
    CHECK(ps.bobCycle == 17);
    std::vector<uint8_t> raw;
    netgame::writePs(raw, server);
    CHECK(netgame::readPs(raw).bobCycle == 17);
}

// The S&D plant / defuse (sd.gsc:1697 and :1911): the client is linked to the zone trigger and its weapon disabled; a
// finished plant deletes the zones (sd.gsc:1727) without an unlink, which G_FreeEntity (0x4d7c30) does for every child;
// a released use button unlinks it (sd.gsc:1789). Afterwards the client moves, fires and changes stance again.
void plantAndRelease(World& w, Entity& p, int client) {
    const Value at = p.obj->get("origin");
    for (bool finish : {true, false}) {
        Value zone = call(w, "spawn", {Value("script_origin"), at});
        call(w, "linkto", {zone}, Value(p.obj));
        call(w, "disableweapon", {}, Value(p.obj));
        CHECK(w.entityLinked(p.number));
        if (&p == w.player()) CHECK(w.playerHasLinkParent());
        CHECK(w.mpState().clients[size_t(client)].weaponDisabled);
        if (finish) {
            call(w, "delete", {}, zone);
        } else {
            call(w, "unlink", {}, Value(p.obj));
            call(w, "delete", {}, zone);
        }
        call(w, "enableweapon", {}, Value(p.obj));
        w.frame();
        CHECK(!w.entityLinked(p.number));
        CHECK(!w.mpState().clients[size_t(client)].weaponDisabled && !w.mpState().clients[size_t(client)].controlsFrozen);
        if (&p == w.player()) CHECK(!w.playerHasLinkParent() && !w.playerControlsFrozen());
    }
}

void world(const fs::Vfs& vfs) {
    World w(vfs, "mp_toujane", true);
    w.vm().setLoopGuardIterations(200000u);
    w.cvars["mapname"] = "mp_toujane";
    w.mpStart("dm");
    const int local = w.mpConnect("host");
    w.mpBegin(local);
    w.frame();
    w.mpAutoJoin(local);
    const int bot = w.mpAddBot("bot");
    for (int i = 0; i < 20; ++i) w.frame();
    Entity* p0 = w.mpClientEntity(local);
    Entity* p1 = w.mpClientEntity(bot);
    CHECK(p0 && p1 && p0 == w.player() && p1 != p0);
    if (!p0 || !p1) return;
    plantAndRelease(w, *p0, local);
    plantAndRelease(w, *p1, bot);
    call(w, "precacheshellshock", {Value("default")});
    call(w, "precacheshellshock", {Value("pain")});
    w.takeClientCommands();
    w.takePlayerTeleport();
    w.takePlayerViewAngles();
    w.takePlayerSpawn();

    // Every player method with a local-player effect, called on the bot: the local player does not change.
    struct Call {
        const char* fn;
        std::vector<Value> args;
    };
    const std::vector<Call> calls = {
        {"shellshock", {Value("default"), Value(5.0f)}},
        {"viewkick", {Value(int32_t(50)), Value(Vec3{0, 0, 0})}},
        {"setorigin", {Value(Vec3{10, 20, 30})}},
        {"setplayerangles", {Value(Vec3{10, 70, 0})}},
        {"freezecontrols", {Value(int32_t(1))}},
        {"allowstand", {Value(int32_t(0))}},
        {"allowcrouch", {Value(int32_t(0))}},
        {"allowprone", {Value(int32_t(0))}},
        {"allowleanleft", {Value(int32_t(0))}},
        {"allowleanright", {Value(int32_t(0))}},
        {"setreverb", {Value("snd_enveffectsprio_level"), Value("stoneroom")}},
        {"deactivatereverb", {Value("snd_enveffectsprio_level")}},
        {"playerlinktodelta", {Value(p0->obj)}},
        {"unlink", {}},
        {"setblur", {Value(2.0f), Value(0.5f)}},
        {"stopshellshock", {}},
    };
    for (const Call& c : calls) {
        const LocalState before = localState(w);
        try {
            call(w, c.fn, c.args, Value(p1->obj));
        } catch (const std::exception& e) {
            std::cerr << c.fn << " on the bot: " << e.what() << "\n";
            CHECK(false);
        }
        const bool commands = w.takeClientCommands().empty();
        const bool teleport = !w.takePlayerTeleport(), angles = !w.takePlayerViewAngles(), spawn = !w.takePlayerSpawn();
        const bool unchanged = same(before, localState(w)) && commands && teleport && angles && spawn;
        if (!unchanged) std::cerr << c.fn << " on the bot changed the local player\n";
        CHECK(unchanged);
    }
    // The bot's own state took them.
    call(w, "shellshock", {Value("pain"), Value(3.0f)}, Value(p1->obj));
    CHECK(w.mpState().clients[size_t(bot)].shockIndex == 2 && w.mpState().clients[size_t(bot)].shockDurationMs == 3000);
    CHECK(w.shellShock().index == 0 && !w.shellShockRecord());
    CHECK(w.mpState().clients[size_t(bot)].controlsFrozen);
    CHECK(p1->obj->get("origin").asVector()[2] == 31.0f); // setorigin still places the bot (raised by 1)
    // Queries of the bot do not read the local player's input.
    CHECK(call(w, "getplayerangles", {}, Value(p1->obj)).isVector());

    // The same on the local player reaches the view.
    call(w, "shellshock", {Value("default"), Value(4.0f)}, Value(p0->obj));
    CHECK(w.shellShock().index == 1 && w.shellShock().durationMs == 4000 && w.shellShockRecord());
    call(w, "stopshellshock", {}, Value(p1->obj));
    CHECK(w.shellShock().index == 1);  // the bot's stop leaves the local shock running
    CHECK(w.mpState().clients[size_t(bot)].shockIndex == 0);
    call(w, "viewkick", {Value(int32_t(50)), Value(Vec3{0, 0, 0})}, Value(p0->obj));
    CHECK(w.debugDamageBlood() > 0);
    call(w, "freezecontrols", {Value(int32_t(1))}, Value(p0->obj));
    CHECK(w.playerControlsFrozen());

    // A client spawn clears the shell shock and frozen controls and restarts the front end's movement state.
    const int spawns = w.mpState().clients[size_t(local)].spawnCount;
    call(w, "spawn", {Value(Vec3{0, 0, 100}), Value(Vec3{0, 90, 0})}, Value(p0->obj));
    CHECK(w.shellShock().index == 0 && !w.playerControlsFrozen());
    CHECK(w.mpState().clients[size_t(local)].spawnCount == spawns + 1);
    const auto sp = w.takePlayerSpawn();
    CHECK(sp && (*sp)[2] == 100.0f);

    // A bot that dies prone or crouched stands after its respawn.
    game::MpBot* b = w.mpBot(bot);
    CHECK(b != nullptr);
    if (b) {
        b->ps.stance = game::Stance::Prone;
        b->ps.viewHeightTarget = 11;
        b->psValid = true;
        b->ps.origin = p1->obj->get("origin").asVector();
        CHECK(call(w, "getstance", {}, Value(p1->obj)).toString() == "prone");  // the bot's own stance, not the local one
        call(w, "spawn", {p1->obj->get("origin"), Value(Vec3{0, 0, 0})}, Value(p1->obj));
        for (int i = 0; i < 3; ++i) w.frame();
        CHECK(b->ps.stance == game::Stance::Stand && b->ps.viewHeightTarget == 60);
        CHECK(call(w, "getstance", {}, Value(p1->obj)).toString() == "stand");
    }
}

void network(const fs::Vfs& vfs) {
    World host(vfs, "mp_toujane", true);
    host.vm().setLoopGuardIterations(200000u);
    host.cvars["mapname"] = "mp_toujane";
    host.mpStart("dm");
    const int local = host.mpConnect("host");
    host.mpBegin(local);
    host.frame();
    host.mpAutoJoin(local);
    call(host, "precacheshellshock", {Value("default")});
    netgame::WorldGame game(&host, "mp_toujane", "dm", 7, 8);
    game.worldOwnedByHost = true;
    CHECK(game.clientConnect(1, "\\name\\remote"));
    const int n = game.clientNumber(1);
    host.mpBegin(n);
    host.frame();
    host.mpAutoJoin(n);
    for (int i = 0; i < 10; ++i) host.frame();
    Entity* pn = host.mpClientEntity(n);
    CHECK(pn && pn != host.player());
    if (!pn) return;
    plantAndRelease(host, *pn, n);
    const auto cs = game.configstrings();
    CHECK(cs.count(netgame::kShockConfigstrings + 1) && cs.at(netgame::kShockConfigstrings + 1) == "default");

    // The 1.3 gamestate layout (docs/spec/net.md section 11): the weapon list in configstring 7 with the default
    // weapon first and the map's turret weapon second, model 1 the default weapon model, then the map's models.
    CHECK(cs.count(netgame::kWeaponListConfigstring) &&
          cs.at(netgame::kWeaponListConfigstring).rfind("defaultweapon_mp mg42_bipod_stand_mp ", 0) == 0);
    CHECK(netgame::weaponNameFromList(cs, 1) == "defaultweapon_mp" && netgame::weaponNameFromList(cs, 0).empty());
    CHECK(netgame::weaponList(cs).size() > 10 && netgame::weaponList(cs)[1] == "mg42_bipod_stand_mp");
    CHECK(cs.count(netgame::kModelConfigstrings) && cs.at(netgame::kModelConfigstrings) == "xmodel/defaultweapon");
    CHECK(cs.count(netgame::kModelConfigstrings + 1) && cs.at(netgame::kModelConfigstrings + 1) == "xmodel/prop_doorknob1");
    CHECK(cs.at(0).find("\\sv_maxclients\\8") != std::string::npos && cs.at(1).find("\\sv_serverid\\7") != std::string::npos);
    // Ammo slots: distinct ammo names in weapon-list order.
    CHECK(netgame::weaponAmmoSlot(host, netgame::weaponList(cs), "defaultweapon_mp", false) == 0);
    // Map entities keep the 1.3 numbers and are in the baselines: the first turret (the second mg42 of the first
    // group in the map's entity list) is 308, eType 9 with weapon 2.
    const auto bl = game.baselines();
    CHECK(bl.count(308) && getI(bl.at(308), 4) == netgame::kEtTurret && getI(bl.at(308), 200) == 2);
    CHECK(bl.count(368) && getI(bl.at(368), 4) == netgame::kEtGeneral);
    // setClientCvar reaches the client as "v <name> \"<value>\"", a localized value with its parameters behind 0x15.
    std::vector<std::string> sent;
    game.sendCommand = [&](int slot, const std::string& t) { if (slot == 1) sent.push_back(t); };
    call(host, "setclientcvar", {Value("cg_test"), Value(script::LocalizedString{"MP_KEY"}), Value(int32_t(5))},
         Value(pn->obj));
    game.runFrame(0);
    CHECK(std::find(sent.begin(), sent.end(), "v cg_test \"MP_KEY\x15" "5\"") != sent.end());
    game.sendCommand = nullptr;

    // Shell shock on the network client: not on the host's own view, in the client's snapshot.
    call(host, "shellshock", {Value("default"), Value(2.0f)}, Value(pn->obj));
    CHECK(host.shellShock().index == 0);
    net::SnapshotView v = game.snapshot(1);
    CHECK((getI(v.ps, 1496) & 15) == 1 && getI(v.ps, 1504) == 2000 && getI(v.ps, 1500) == int(host.timeMs()));
    net::SnapshotView hv = game.snapshot(0);  // a slot that is no network client carries no shock
    CHECK(getI(hv.ps, 1496) == 0);

    // The client's view applies it on its own clock.
    World view(vfs, "mp_toujane", true);
    view.mpStartRemote();
    net::ClientSnapshot snap;
    snap.valid = true;
    snap.messageNum = 1;
    snap.serverTime = v.serverTime;
    snap.ps = v.ps;
    netgame::RemoteView rv;
    const int64_t clock = 50000;
    rv.apply(view, snap, cs, n, clock);
    CHECK(view.shellShock().index > 0 && view.shellShockRecord() && view.shellShock().durationMs == 2000);
    CHECK(view.shellShock().startMs == clock - (v.serverTime - getI(v.ps, 1500)));
    // Stopped on the server: gone on the client too.
    call(host, "stopshellshock", {}, Value(pn->obj));
    v = game.snapshot(1);
    snap.messageNum = 2;
    snap.ps = v.ps;
    rv.apply(view, snap, cs, n, clock + 50);
    CHECK(view.shellShock().index == 0 && !view.shellShockRecord());
    // The body queue: cloneplayer uses 8 bodies, numbers 64..71 on the wire with eType 2 and the client.
    {
        for (int i = 0; i < 9; ++i) call(host, "cloneplayer", {Value(int32_t(1000))}, Value(pn->obj)); // a duration: the flag lasts as long as the death animation
        int alive = 0;
        for (const auto& e : host.entities()) alive += e->classname == "playercorpse" && !e->obj->removed;
        CHECK(alive == 8);
        const net::SnapshotView bv = game.snapshot(1);
        int bodies = 0;
        for (int k = 64; k < 72; ++k)
            if (bv.entities.count(k) && getI(bv.entities.at(k), 4) == netgame::kEtCorpse &&
                getI(bv.entities.at(k), 144) == n && (getI(bv.entities.at(k), 8) & 0xa0000) == 0xa0000)
                ++bodies;
        CHECK(bodies == 8);
    }
    // A body killed in the air falls, and its horizontal speed was capped by g_clonePlayerMaxVelocity.
    {
        const auto o0 = pn->obj->get("origin").asVector();
        pn->obj->set("origin", Value(iw2::script::Vec3{o0[0], o0[1], o0[2] + 40.0f}));
        pn->velocity = {500, -20, 0};
        const Value bv = call(host, "cloneplayer", {Value(int32_t(1000))}, Value(pn->obj));
        pn->obj->set("origin", Value(o0));
        CHECK(bv.isObject() && std::fabs(bv.asObject()->get("$trdelta").asVector()[0] - 80.0f) < 0.01f);
        CHECK(bv.isObject() && std::fabs(bv.asObject()->get("$trdelta").asVector()[1] + 20.0f) < 0.01f);
        for (int i = 0; i < 6; ++i) host.frame();
        CHECK(bv.isObject() && bv.asObject()->get("origin").asVector()[2] < o0[2] + 35.0f);
    }
    // Hands: viewModel (ps+0xe4) is the model index of the setviewmodel model; the client's player takes it.
    {
        const std::string hands = pn->obj->get("$viewmodel").toString();
        CHECK(!hands.empty() && (getI(v.ps, 228) & 0xff) > 0);
        snap.messageNum = 3;
        rv.apply(view, snap, game.configstrings(), n, clock + 100);
        CHECK(view.player() && view.player()->obj->get("$viewmodel").toString() == hands);
    }

    // Walking on the server advances the client's bob cycle, which its snapshot carries at ps+8 (the start of the
    // client's prediction, so the gun and view bob of a network client follow the walk).
    int t = host.timeMs() > 0 ? int(host.timeMs()) : 1000;
    std::set<int> cycles;
    for (int i = 0; i < 60; ++i) {
        net::UserCmd c;
        t += 16;
        c.serverTime = t;
        c.forwardmove = 127;
        c.angles[1] = i * 400; // turning: never stuck at a wall for the whole walk
        game.clientThink(1, c);
        cycles.insert(getI(game.snapshot(1).ps, 8));
    }
    CHECK(cycles.size() > 5);
    CHECK(netgame::readPs(game.snapshot(1).ps).bobCycle == (getI(game.snapshot(1).ps, 8) & 0xff));
}

} // namespace

int main() {
    prediction();
    const char* mainDir = std::getenv("IW2_MAIN");
    if (mainDir) {
        fs::Vfs vfs;
        vfs.mountDirectory(mainDir);
        world(vfs);
        network(vfs);
    } else {
        std::cout << "IW2_MAIN not set: world tests skipped\n";
    }
    if (failures) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "ok\n";
    return 0;
}
