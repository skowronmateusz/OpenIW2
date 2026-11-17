// Another player's jumps and landings, the animation rate from the movement and a player on a turret, as the others see them
// (docs/spec/mp-players.md "Jumps and landings", "Animation rate from the movement", "Animation numbers on the wire"):
// the movement's legs events (pmove), their transfer through the recorded view state, the takeoff held in the air,
// legsAnim with the toggle bit, the network client's puppet playing it, and a climb on the spot standing still.
// Needs the game files (IW2_MAIN) for all but the rate clamps.
#include "assets/d3dbsp.h"
#include "fs/vfs.h"
#include "game/clientinput.h"
#include "game/collision.h"
#include "game/mpplayers.h"
#include "game/playeranimscript.h"
#include "game/pmove.h"
#include "game/world.h"
#include "netgame/remoteview.h"
#include "netgame/worldgame.h"

#include <cmath>
#include <cstdio>
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
using game::World;

int32_t getI(const std::vector<uint8_t>& b, size_t off) {
    int32_t v = 0;
    if (off + 4 <= b.size()) std::memcpy(&v, b.data() + off, 4);
    return v;
}

bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

// The clamps of MP 0x4f9de5..0x4f9f80.
void rateTests() {
    using game::paMoveRate;
    CHECK(near(paMoveRate(1.5f, 100, false), 1.5f));
    CHECK(near(paMoveRate(0.05f, 100, false), 0.1f));   // too slow: 0.1
    CHECK(near(paMoveRate(0.05f, 100, true), 0.1f));    // a ladder animation too, down to 0.01
    CHECK(near(paMoveRate(0.005f, 100, false), 0.1f));
    CHECK(near(paMoveRate(0.0f, 100, true), 0.0f));     // a ladder animation stops below 0.01
    CHECK(near(paMoveRate(5.0f, 100, true), 4.0f));     // ladder: at most 4
    CHECK(near(paMoveRate(3.5f, 100, true), 3.5f));
    CHECK(near(paMoveRate(5.0f, 200, false), 2.0f));    // a fast animation: 2
    CHECK(near(paMoveRate(5.0f, 85, false), 2.5f));     // 3 - (85 - 20) / 130
    CHECK(near(paMoveRate(2.4f, 85, false), 2.4f));
    CHECK(near(paMoveRate(5.0f, 20, false), 3.0f));
    CHECK(near(paMoveRate(5.0f, 10, false), 3.0f));     // a slow animation: at most 3
    CHECK(near(paMoveRate(2.5f, 10, false), 2.5f));
}

// The movement's legs events on the toujane spawn floor (lnxded 0x80dcca8, 0x80e36aa, 0x80e30c4).
void pmoveTests(const fs::Vfs& vfs) {
    const auto bytes = vfs.read("maps/mp/mp_toujane.d3dbsp");
    CHECK(bytes.has_value());
    if (!bytes) return;
    const assets::D3dBsp bsp(*bytes);
    const game::CollisionWorld cw(bsp);
    game::Vec start{};
    for (const auto& e : bsp.entities())
        if (const std::string* c = e.get("classname"); c && *c == "mp_dm_spawn")
            if (const std::string* o = e.get("origin")) {
                std::sscanf(o->c_str(), "%f %f %f", &start[0], &start[1], &start[2]);
                break;
            }
    game::PlayerState ps;
    ps.origin = {start[0], start[1], start[2] + 8};
    for (int i = 0; i < 40; ++i) game::playerMove(cw, ps, {}, 0.05f);
    CHECK(ps.onGround);
    const game::Vec floor = ps.origin;
    auto settle = [&](game::PlayerState& p) {
        p = game::PlayerState{};
        p.origin = floor;
        for (int i = 0; i < 20; ++i) game::playerMove(cw, p, {}, 0.05f);
    };
    // A jump: the forced jump event in the move that jumps; backwards jumpbk.
    for (const float fwd : {0.0f, -1.0f}) {
        game::PlayerState p;
        settle(p);
        CHECK(p.onGround && p.animEventSeq == 0);
        game::PlayerInput in;
        in.jump = true;
        in.forward = fwd;
        const int seq = p.animEventSeq;
        game::playerMove(cw, p, in, 0.05f);
        CHECK(!p.onGround && p.animEvent == (fwd < 0 ? 4 : 3) && p.animEventForced && p.animEventSeq == seq + 1);
        // The next move adds nothing in the air; then the landing (a jump falls about 39 units, above 12).
        game::playerMove(cw, p, {}, 0.05f);
        CHECK(p.animEventSeq == seq + 1);
        int land = 0;
        for (int i = 0; i < 40 && !p.onGround; ++i) {
            const int before = p.animEventSeq;
            game::playerMove(cw, p, {}, 0.05f);
            if (p.animEventSeq != before && p.animEvent == 5 && p.animEventForced) ++land;
        }
        CHECK(p.onGround && land == 1 && p.animEventSeq == seq + 2);
    }
    // A fall of 8 units lands without the land animation; one of 40 with it.
    for (const float h : {8.0f, 40.0f}) {
        game::PlayerState p;
        settle(p);
        p.origin[2] += h;
        p.onGround = false;
        bool land = false;
        for (int i = 0; i < 40 && !(i > 0 && p.onGround); ++i) {
            const int before = p.animEventSeq;
            game::playerMove(cw, p, {}, 0.05f);
            land |= p.animEventSeq != before && p.animEvent == 5;
        }
        CHECK(land == (h > 12.0f));
    }
    // Ground lost with nothing within 64 units below: the jump animation, forced; 30 units above the floor: none.
    for (const float h : {30.0f, 70.0f}) { // (higher, the spawn has a roof)
        game::PlayerState p;
        settle(p);
        p.origin[2] += h; // still flagged on the ground
        const int before = p.animEventSeq;
        game::playerMove(cw, p, {}, 0.05f);
        CHECK(!p.onGround);
        CHECK((p.animEventSeq == before + 1 && p.animEvent == 3 && p.animEventForced) == (h > 64.0f));
    }
    // The recorded view state carries them.
    {
        game::PlayerState p;
        settle(p);
        game::PlayerInput in;
        in.jump = true;
        game::playerMove(cw, p, in, 0.05f);
        const game::MpViewState v = game::mpViewStateOf(game::PlayerWeapon{}, p);
        CHECK(!v.onGround && v.animEvent == 3 && v.animEventForced && v.animEventSeq == p.animEventSeq);
    }
}

void worldTests(const fs::Vfs& vfs) {
    World host(vfs, "mp_toujane", true);
    host.vm().setLoopGuardIterations(200000u);
    host.cvars["mapname"] = "mp_toujane";
    host.cvars["scr_dm_timelimit"] = "0";
    host.mpStart("dm");
    const int local = host.mpConnect("host");
    host.mpBegin(local);
    host.frame();
    host.mpAutoJoin(local);
    netgame::WorldGame game(&host, "mp_toujane", "dm", 7, 8);
    game.worldOwnedByHost = true;
    CHECK(game.clientConnect(1, "\\name\\watcher"));
    const int watcher = game.clientNumber(1);
    host.mpBegin(watcher);
    host.frame();
    host.mpAutoJoin(watcher);
    // The jumper: a client nothing else moves, whose recorded view state the test sets as its mover would.
    const int c = host.mpConnect("jumper");
    host.frame();
    host.mpBegin(c);
    host.frame();
    host.mpAutoJoin(c);
    for (int i = 0; i < 20; ++i) host.frame();
    const game::MpClient& cl = host.mpState().clients[size_t(c)];
    CHECK(cl.ent && cl.ent->obj->get("sessionstate").toString() == "playing" && cl.ent->anim);
    if (!cl.ent || !cl.ent->anim) return;
    const game::AnimTree& tree = cl.ent->anim->tree();
    CHECK(tree.nodes().size() < 512); // room for the toggle bit 0x200
    auto setView = [&](const game::MpViewState& v, int frames) {
        for (int i = 0; i < frames; ++i) {
            host.mpSetViewState(c, v);
            host.frame();
        }
    };
    auto weight = [&](const game::Entity& e, int n) { return n < 0 ? -1.0f : e.anim->nodeStates()[size_t(n)].goalWeight; };
    auto wireLegs = [&]() {
        const net::SnapshotView v = game.snapshot(1);
        const auto it = v.entities.find(c);
        return it == v.entities.end() ? -1 : getI(it->second, 204) & 1023;
    };
    game::MpViewState vs;
    vs.onGround = true;
    setView(vs, 3);
    CHECK(cl.anim.moveType == "idle" && wireLegs() == 0);
    const int idleNode = cl.anim.legsNode;
    // A jump: the takeoff plays on the legs (duration 5) and stays there while the client is in the air; the move type
    // is not chosen anew; legsAnim names it with a toggle bit.
    vs.onGround = false;
    vs.animEvent = 3;
    vs.animEventForced = true;
    vs.animEventSeq = 1;
    setView(vs, 1);
    const int takeoff = cl.anim.legsNode;
    const std::string takeoffName = takeoff > 0 ? tree.nodes()[size_t(takeoff)].name : std::string();
    std::cout << "takeoff animation: " << takeoffName << "\n";
    CHECK(takeoffName.find("jump_takeoff") != std::string::npos && weight(*cl.ent, takeoff) > 0.9f);
    const int wire1 = wireLegs();
    CHECK((wire1 & 0x1ff) == takeoff && (wire1 & ~0x3ff) == 0);
    vs.animEvent = 0;
    vs.animEventForced = false;
    setView(vs, 10); // 500 ms in the air
    CHECK(cl.anim.legsNode == takeoff && weight(*cl.ent, takeoff) > 0.9f && cl.anim.moveType == "idle");
    CHECK(wireLegs() == wire1);
    // The landing: the land animation, legsAnim with the toggle bit changed; after its hold the move type again.
    vs.onGround = true;
    vs.animEvent = 5;
    vs.animEventForced = true;
    vs.animEventSeq = 2;
    setView(vs, 1);
    const int land = cl.anim.legsNode;
    const std::string landName = land > 0 ? tree.nodes()[size_t(land)].name : std::string();
    std::cout << "land animation: " << landName << "\n";
    CHECK(landName.find("jump_land") != std::string::npos);
    const int wire2 = wireLegs();
    CHECK((wire2 & 0x1ff) == land && ((wire2 ^ wire1) & 0x200) != 0);
    // An unforced event (leaving the ground upwards) 50 ms later does not cut the landing, which holds the legs for 100 ms.
    vs.animEvent = 3;
    vs.animEventForced = false;
    vs.animEventSeq = 3;
    setView(vs, 1);
    CHECK(cl.anim.legsNode == land && wireLegs() == wire2);
    vs.animEvent = 0;
    setView(vs, 12);
    CHECK(cl.anim.moveType == "idle" && cl.anim.eventLegsNode == 0 && cl.anim.legsNode != land && wireLegs() == 0);
    (void)idleNode;
    // The same jump again flips the toggle bit, so the receiver starts it again.
    vs.onGround = false;
    vs.animEvent = 3;
    vs.animEventForced = true;
    vs.animEventSeq = 4;
    setView(vs, 1);
    const int wire3 = wireLegs();
    CHECK((wire3 & 0x1ff) == takeoff && ((wire3 ^ wire2) & 0x200) != 0);
    // The same event number twice in a row without a new sequence is one event.
    setView(vs, 1);
    CHECK(wireLegs() == wire3);

    // The watcher's front end: the puppet plays the server's takeoff as it is and holds it while legsAnim names it.
    vs = game::MpViewState{};
    vs.onGround = true;
    vs.animEventSeq = 4;
    setView(vs, 14);
    CHECK(wireLegs() == 0);
    World view(vfs, "mp_toujane", true);
    view.mpStartRemote();
    const auto cs = game.configstrings();
    net::ClientSnapshot snap;
    snap.valid = true;
    snap.messageNum = 1;
    auto show = [&](int frames) {
        for (int i = 0; i < frames; ++i) {
            const net::SnapshotView v = game.snapshot(1);
            snap.serverTime = v.serverTime;
            snap.ps = v.ps;
            snap.entities = v.entities;
            snap.clients = v.clients;
            ++snap.messageNum;
            view.mpSetRemotePlayers(netgame::RemoteView::players(snap, snap, 0.0f, watcher, cs));
            view.frame();
        }
    };
    show(4);
    const int slot = netgame::RemoteView::worldSlot(c, watcher);
    const game::MpClient& pc = view.mpState().clients[size_t(slot)];
    CHECK(pc.ent && pc.ent->anim && pc.anim.moveType == "idle");
    if (!pc.ent || !pc.ent->anim) return;
    vs.onGround = false;
    vs.animEvent = 3;
    vs.animEventForced = true;
    vs.animEventSeq = 5;
    setView(vs, 1);
    vs.animEvent = 0;
    show(1);
    CHECK(pc.anim.legsNode == takeoff && weight(*pc.ent, takeoff) > 0.9f);
    setView(vs, 6);
    show(6);
    CHECK(pc.anim.legsNode == takeoff && weight(*pc.ent, takeoff) > 0.9f); // no move type in the air
    vs.onGround = true;
    setView(vs, 12);
    show(4);
    CHECK(pc.anim.moveType == "idle" && pc.anim.legsNode != takeoff && pc.anim.serverLegsPlayed == 0);

    // A climb on the spot: the ladder animation stops (rate 0, its time does not move); climbing at speed plays it at
    // that speed over the animation's move speed.
    const int climbup = tree.find("pb_climbup");
    const float climbSpeed = host.mpAnimMoveSpeed(climbup);
    std::cout << "pb_climbup move speed " << climbSpeed << "\n";
    CHECK(climbup > 0 && climbSpeed > 0);
    vs = game::MpViewState{};
    vs.climb = 1;
    vs.onGround = false;
    setView(vs, 4);
    CHECK(cl.anim.moveType == "climbup" && cl.anim.legsNode == climbup);
    const float t0 = cl.ent->anim->time(climbup);
    setView(vs, 4);
    CHECK(near(cl.ent->anim->nodeStates()[size_t(climbup)].rate, 0.0f) && near(cl.ent->anim->time(climbup), t0));
    // Up at half the animation's speed: rate 0.5.
    const script::Vec3 o = cl.ent->obj->get("origin").asVector();
    for (int i = 1; i <= 4; ++i) {
        cl.ent->obj->set("origin", script::Value(script::Vec3{o[0], o[1], o[2] + float(i) * climbSpeed * 0.5f * 0.05f}));
        host.mpSetViewState(c, vs);
        host.frame();
    }
    std::cout << "climb rate at half speed " << cl.ent->anim->nodeStates()[size_t(climbup)].rate << "\n";
    CHECK(std::fabs(cl.ent->anim->nodeStates()[size_t(climbup)].rate - 0.5f) < 0.02f);
    CHECK(cl.ent->anim->time(climbup) != t0);
    cl.ent->obj->set("origin", script::Value(o));
    CHECK(host.vm().errors().empty() && view.vm().errors().empty());
}

// A network client jumps with its usercmds (WorldGame::clientThink): the server's move adds the jump event, the other
// client's snapshot carries the takeoff with the toggle bit while it is in the air, and 0 again after the landing.
void netJumpTests(const fs::Vfs& vfs) {
    World host(vfs, "mp_toujane", true);
    host.vm().setLoopGuardIterations(200000u);
    host.cvars["mapname"] = "mp_toujane";
    host.cvars["scr_dm_timelimit"] = "0";
    host.mpStart("dm");
    const int local = host.mpConnect("host");
    host.mpBegin(local);
    host.frame();
    host.mpAutoJoin(local);
    netgame::WorldGame game(&host, "mp_toujane", "dm", 7, 8);
    game.worldOwnedByHost = true;
    CHECK(game.clientConnect(1, "\\name\\watcher"));
    CHECK(game.clientConnect(2, "\\name\\jumper"));
    const int watcher = game.clientNumber(1), jumper = game.clientNumber(2);
    for (const int c : {watcher, jumper}) {
        host.mpBegin(c);
        host.frame();
        host.mpAutoJoin(c);
    }
    int t = int(host.timeMs());
    auto think = [&](bool jump) {
        net::UserCmd cmd;
        t += 50;
        cmd.serverTime = t;
        cmd.buttons = jump ? int32_t(game::Button::Jump) : 0;
        game.clientThink(2, cmd);
        host.frame();
    };
    for (int i = 0; i < 40; ++i) think(false); // spawned and settled
    const game::MpClient& cl = host.mpState().clients[size_t(jumper)];
    CHECK(cl.ent && cl.ent->anim && cl.ent->obj->get("sessionstate").toString() == "playing");
    if (!cl.ent || !cl.ent->anim) return;
    const game::AnimTree& tree = cl.ent->anim->tree();
    auto wire = [&]() {
        const net::SnapshotView v = game.snapshot(1);
        const auto it = v.entities.find(jumper);
        return it == v.entities.end() ? -1 : getI(it->second, 204) & 1023;
    };
    CHECK(wire() == 0);
    think(true);
    const int w1 = wire();
    const std::string name = (w1 & 0x1ff) > 0 && size_t(w1 & 0x1ff) < tree.nodes().size() ? tree.nodes()[size_t(w1 & 0x1ff)].name : "-";
    std::cout << "network jump: legsAnim " << w1 << " (" << name << ")\n";
    CHECK(name.find("jump_takeoff") != std::string::npos && (w1 & 0x200) != 0);
    int held = 0, last = w1;
    std::vector<int> after; // the values that follow the takeoff
    for (int i = 0; i < 40; ++i) {
        think(false);
        const int w = wire();
        if (w == w1 && after.empty()) ++held;
        else if (w != last) after.push_back(w);
        last = w;
    }
    std::cout << "network jump: takeoff held " << held << " frames, then";
    for (int w : after) std::cout << " " << w << " (" << ((w & 0x1ff) > 0 ? tree.nodes()[size_t(w & 0x1ff)].name : "-") << ")";
    std::cout << "\n";
    // In the air the takeoff stays; the landing (about 39 units, above 12) sends the land animation with the toggle bit
    // changed, then the move type is chosen again (0).
    CHECK(held >= 5 && after.size() == 2 && tree.nodes()[size_t(after[0] & 0x1ff)].name.find("jump_land") != std::string::npos &&
          ((after[0] ^ w1) & 0x200) != 0 && after[1] == 0);
    CHECK(host.vm().errors().empty());
}

// A client on a map turret (docs/spec/mp-players.md "On a turret"): the mounted items of the move type (aim, fire while
// the attack is held), the turret pose of their blend node, no weapon in the hands; the watcher's puppet the same from
// the entity state (eFlags 0x300, otherEntityNum).
void turretTests(const fs::Vfs& vfs) {
    World host(vfs, "mp_carentan", true);
    host.vm().setLoopGuardIterations(200000u);
    host.cvars["mapname"] = "mp_carentan";
    host.cvars["scr_dm_timelimit"] = "0";
    host.mpStart("dm");
    const int local = host.mpConnect("host");
    host.mpBegin(local);
    host.frame();
    host.mpAutoJoin(local);
    netgame::WorldGame game(&host, "mp_carentan", "dm", 7, 8);
    game.worldOwnedByHost = true;
    CHECK(game.clientConnect(1, "\\name\\watcher"));
    const int watcher = game.clientNumber(1);
    host.mpBegin(watcher);
    host.frame();
    host.mpAutoJoin(watcher);
    const int c = host.mpConnect("gunner");
    host.mpBegin(c);
    host.frame();
    host.mpAutoJoin(c);
    for (int i = 0; i < 10; ++i) host.frame();
    game::Entity* p = host.mpClientEntity(c);
    game::Entity* t = nullptr;
    for (const auto& e : host.entities())
        if (e->turret && !e->obj->removed && !t) t = e.get();
    CHECK(p && t && p->anim);
    if (!p || !t || !p->anim) return;
    const script::Vec3 o = t->obj->get("origin").asVector(), a = t->obj->get("angles").asVector();
    const float yaw = a[1] * 3.14159265f / 180.0f;
    p->obj->set("origin", script::Value(script::Vec3{o[0] - std::cos(yaw) * 34.0f, o[1] - std::sin(yaw) * 34.0f, o[2] - 20.0f}));
    host.mpSetView(c, script::Vec3{0, a[1], 0}, 60.0f);
    host.mpSetButtons(c, 0);
    host.frame();
    host.mpSetButtons(c, 8);
    host.frame();
    host.mpSetButtons(c, 0);
    for (int i = 0; i < 3; ++i) host.frame();
    {
        const script::Vec3 po = p->obj->get("origin").asVector();
        std::cout << "turret " << t->turret->weapon << " at " << o[0] << " " << o[1] << " " << o[2] << ", gunner at " << po[0] << " " << po[1] << " " << po[2]
                  << " state " << p->obj->get("sessionstate").toString() << " on " << host.mpClientOnTurret(c) << "\n";
    }
    CHECK(host.mpClientOnTurret(c) && host.mpTurretOf(c) == t);
    const game::MpClient& cl = host.mpState().clients[size_t(c)];
    const game::AnimTree& tree = p->anim->tree();
    auto leafWeight = [&](const game::Entity& e, int top) { // the summed goal weight of the leaves under `top`
        float w = 0;
        std::vector<int> stack{top};
        while (!stack.empty()) {
            const int n = stack.back();
            stack.pop_back();
            const auto& ch = tree.nodes()[size_t(n)].children;
            if (ch.empty()) w += e.anim->nodeStates()[size_t(n)].goalWeight;
            stack.insert(stack.end(), ch.begin(), ch.end());
        }
        return w;
    };
    const int aim = tree.find("standmg42_aim"), fire = tree.find("standmg42_fire");
    CHECK(aim > 0 && fire > 0);
    std::cout << "gunner: move type " << cl.anim.moveType << ", legs " << (cl.anim.legsNode > 0 ? tree.nodes()[size_t(cl.anim.legsNode)].name : "-")
              << ", weapon model '" << host.mpPlayerWeaponModel(*p) << "', leaves " << leafWeight(*p, aim) << "\n";
    CHECK(cl.anim.moveType == "idle" && cl.anim.legsNode == aim && p->anim->nodeStates()[size_t(aim)].goalWeight > 0.9f);
    CHECK(leafWeight(*p, aim) > 0.9f); // posed, not the reference pose
    CHECK(host.mpPlayerWeaponModel(*p).empty());
    host.mpSetButtons(c, 1); // firing the turret
    for (int i = 0; i < 2; ++i) host.frame();
    CHECK(cl.anim.legsNode == fire && leafWeight(*p, fire) > 0.9f);

    // The watcher's puppet.
    World view(vfs, "mp_carentan", true);
    view.mpStartRemote();
    const auto cs = game.configstrings();
    net::ClientSnapshot snap;
    snap.valid = true;
    for (int i = 0; i < 4; ++i) {
        host.frame();
        const net::SnapshotView v = game.snapshot(1);
        snap.serverTime = v.serverTime;
        snap.ps = v.ps;
        snap.entities = v.entities;
        snap.clients = v.clients;
        ++snap.messageNum;
        view.mpSetRemotePlayers(netgame::RemoteView::players(snap, snap, 0.0f, watcher, cs));
        view.frame();
    }
    const int slot = netgame::RemoteView::worldSlot(c, watcher);
    const game::MpClient& pc = view.mpState().clients[size_t(slot)];
    CHECK(pc.ent && pc.ent->anim && view.mpTurretOf(slot) != nullptr);
    if (!pc.ent || !pc.ent->anim) return;
    std::cout << "puppet gunner: legs " << (pc.anim.legsNode > 0 ? tree.nodes()[size_t(pc.anim.legsNode)].name : "-") << ", weapon model '"
              << view.mpPlayerWeaponModel(*pc.ent) << "', leaves " << leafWeight(*pc.ent, fire) << "\n";
    CHECK(pc.anim.legsNode == fire && leafWeight(*pc.ent, fire) > 0.9f);
    CHECK(view.mpPlayerWeaponModel(*pc.ent).empty());
    CHECK(host.vm().errors().empty() && view.vm().errors().empty());
}

} // namespace

int main() {
    rateTests();
    if (const char* main = std::getenv("IW2_MAIN")) {
        fs::Vfs vfs;
        vfs.mountDirectory(main);
        pmoveTests(vfs);
        worldTests(vfs);
        netJumpTests(vfs);
        turretTests(vfs);
    } else {
        std::cout << "IW2_MAIN not set, world part skipped\n";
    }
    std::cout << (failures ? "FAILED" : "ok") << "\n";
    return failures ? 1 : 0;
}
