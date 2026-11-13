// Another player on a ladder or mantling, as the others see him (docs/spec/mp-players.md "Ladder and mantle move types"):
// the move types climbup / climbdown / mantle_* from the movement, the server's animation of a client whose recorded view
// state climbs or mantles, its node in the entity state's legsAnim (live and in a killcam's archived frame), and the
// network client's puppet that plays the same move type from legsAnim. Needs the game files (IW2_MAIN) for the world part.
#include "fs/vfs.h"
#include "game/mpbots.h"
#include "game/mpplayers.h"
#include "game/playeranimscript.h"
#include "game/world.h"
#include "netgame/remoteview.h"
#include "netgame/worldgame.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

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

int32_t getI(const std::vector<uint8_t>& b, size_t off) {
    int32_t v = 0;
    if (off + 4 <= b.size()) std::memcpy(&v, b.data() + off, 4);
    return v;
}

void moveTypeTests() {
    game::PaMovement m;
    m.climb = 1;
    CHECK(game::paMoveType(m) == "climbup");
    m.xySpeed = 200; // sideways along the ladder: still climbing
    CHECK(game::paMoveType(m) == "climbup");
    m.climb = -1;
    CHECK(game::paMoveType(m) == "climbdown");
    m.climb = 0;
    m.mantleAnim = 1;
    CHECK(game::paMoveType(m) == "mantle_up_57");
    m.climb = 1; // the mantle's own type wins
    CHECK(game::paMoveType(m) == "mantle_up_57");
    m.mantleAnim = 7;
    CHECK(game::paMoveType(m) == "mantle_up_21");
    m.mantleAnim = 8;
    CHECK(game::paMoveType(m) == "mantle_over_high");
    m.mantleAnim = 10;
    CHECK(game::paMoveType(m) == "mantle_over_low");
    CHECK(game::paMantleMoveType(0).empty() && game::paMantleMoveType(11).empty());
    CHECK(game::paServerOnlyMoveType("climbup") && game::paServerOnlyMoveType("climbdown") && game::paServerOnlyMoveType("mantle_over_mid"));
    CHECK(!game::paServerOnlyMoveType("run") && !game::paServerOnlyMoveType("idle"));
}

// The recorded view state of the footsteps step's ladder branch (mpViewStateOf): in the air on a ladder, not linked.
void viewStateTests() {
    game::PlayerWeapon w;
    game::PlayerState ps;
    ps.onLadder = true;
    ps.onGround = false;
    ps.velocity = {0, 0, 50};
    ps.timeMs = 10000;
    CHECK(game::mpViewStateOf(w, ps).climb == 1 && !game::mpViewStateOf(w, ps).climbHold);
    ps.velocity[2] = 0; // on the spot: up
    CHECK(game::mpViewStateOf(w, ps).climb == 1);
    ps.velocity[2] = -30;
    CHECK(game::mpViewStateOf(w, ps).climb == -1);
    ps.lastJumpMs = 9750; // 250 ms after a jump: nothing new is chosen
    CHECK(game::mpViewStateOf(w, ps).climbHold);
    ps.lastJumpMs = 9700;
    CHECK(!game::mpViewStateOf(w, ps).climbHold);
    ps.onGround = true; // standing at the ladder's foot: the ground types
    CHECK(game::mpViewStateOf(w, ps).climb == 0);
    ps.onGround = false;
    ps.pmType = 1; // linked
    CHECK(game::mpViewStateOf(w, ps).climb == 0);
    ps = game::PlayerState{};
    ps.mantling = true;
    ps.mantleTrans = 3;
    ps.mantleTimeMs = 120;
    const game::MpViewState v = game::mpViewStateOf(w, ps);
    CHECK(v.mantling && v.mantleTrans == 3 && v.mantleTimeMs == 120 && v.climb == 0);
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
    // The climber: a client nothing else moves, whose recorded view state the test sets as its mover would.
    const int c = host.mpConnect("climber");
    host.frame();
    host.mpBegin(c);
    host.frame();
    host.mpAutoJoin(c);
    for (int i = 0; i < 20; ++i) host.frame();
    const game::MpClient& cl = host.mpState().clients[size_t(c)];
    CHECK(cl.ent && cl.ent->obj->get("sessionstate").toString() == "playing" && cl.ent->anim);
    if (!cl.ent || !cl.ent->anim) return;
    const game::AnimTree& tree = cl.ent->anim->tree();
    const game::MantleAnims* mantle = host.mpMantleAnims();
    CHECK(mantle && mantle->loaded());
    auto setView = [&](const game::MpViewState& v, int frames) {
        for (int i = 0; i < frames; ++i) {
            host.mpSetViewState(c, v);
            host.frame();
        }
    };
    auto weight = [&](const char* name) {
        const int n = tree.find(name);
        return n < 0 ? -1.0f : cl.ent->anim->nodeStates()[size_t(n)].goalWeight;
    };
    auto wireLegs = [&]() {
        const net::SnapshotView v = game.snapshot(1);
        const auto it = v.entities.find(c);
        return it == v.entities.end() ? -1 : getI(it->second, 204) & 1023;
    };
    CHECK(cl.anim.moveType == "idle" && wireLegs() == 0);
    // Up the ladder (and on the spot): climbup with pb_climbup on the legs, its node in legsAnim.
    game::MpViewState vs;
    vs.climb = 1;
    setView(vs, 3);
    CHECK(cl.anim.moveType == "climbup");
    CHECK(tree.find("pb_climbup") > 0 && weight("pb_climbup") > 0.9f);
    CHECK(wireLegs() == tree.find("pb_climbup"));
    // Down: climbdown.
    vs.climb = -1;
    setView(vs, 3);
    CHECK(cl.anim.moveType == "climbdown" && weight("pb_climbdown") > 0.9f && wireLegs() == tree.find("pb_climbdown"));
    // A jump on the ladder: for 300 ms the footsteps step chooses nothing, the climb goes on.
    vs.climb = 1;
    vs.climbHold = true;
    setView(vs, 2);
    CHECK(cl.anim.moveType == "climbdown");
    vs.climbHold = false;
    setView(vs, 1);
    CHECK(cl.anim.moveType == "climbup");
    // Off the ladder: the ground types again, nothing in legsAnim.
    vs.climb = 0;
    setView(vs, 3);
    CHECK(cl.anim.moveType == "idle" && wireLegs() == 0);
    // A mantle of transition 0 (57 units): mantle_up_57 while the time is within the up animation, then mantle_over_high.
    vs.mantling = true;
    vs.mantleTrans = 0;
    vs.mantleTimeMs = 0;
    setView(vs, 2);
    CHECK(cl.anim.moveType == "mantle_up_57" && weight("mp_mantle_up_57") > 0.9f && wireLegs() == tree.find("mp_mantle_up_57"));
    game::PlayerState mps;
    mps.mantling = true;
    mps.mantleTrans = 0;
    mps.mantleTimeMs = 0;
    CHECK(game::mantleAnimIndex(mps, *mantle) == 1);
    for (int t = 0; t < 4000 && game::mantleAnimIndex(mps, *mantle) == 1; t += 10) mps.mantleTimeMs = t;
    CHECK(game::mantleAnimIndex(mps, *mantle) == 8);
    vs.mantleTimeMs = mps.mantleTimeMs;
    setView(vs, 2);
    CHECK(cl.anim.moveType == "mantle_over_high" && wireLegs() == tree.find("mp_mantle_over_high"));
    // The killcam's archived players carry their recorded state's climb (or mantle).
    game::MpViewState arch;
    arch.climb = -1;
    CHECK(host.mpLegsAnimWire(c, &arch) == tree.find("pb_climbdown"));
    arch.climb = 0;
    CHECK(host.mpLegsAnimWire(c, &arch) == 0);

    // The watcher's front end: the puppet plays the move type of legsAnim.
    vs = game::MpViewState{};
    vs.climb = 1;
    setView(vs, 3);
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
    std::cout << "puppet move type on the ladder: " << pc.anim.moveType << "\n";
    CHECK(pc.ent && pc.anim.moveType == "climbup");
    vs.climb = -1;
    setView(vs, 2);
    show(3);
    CHECK(pc.anim.moveType == "climbdown");
    vs = game::MpViewState{};
    vs.mantling = true;
    setView(vs, 2);
    show(3);
    std::cout << "puppet move type mantling: " << pc.anim.moveType << "\n";
    CHECK(pc.anim.moveType == "mantle_up_57");
    vs = game::MpViewState{};
    setView(vs, 2);
    show(3);
    CHECK(pc.anim.moveType == "idle");
    CHECK(host.vm().errors().empty() && view.vm().errors().empty());
}

// Real movers: a bot sent along the nav graph's ladder and mantle links of mp_toujane climbs and mantles with its own
// movement (the recorded view state its controller fills); while it does, its animation is the climb / mantle one and
// the watcher's snapshot carries that node in legsAnim.
void botTests(const fs::Vfs& vfs) {
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
    const int bot = host.mpAddBot("climber");
    host.mpBotSetCombat(bot, false);
    for (int i = 0; i < 40; ++i) host.frame();
    Entity* be = host.mpClientEntity(bot);
    CHECK(be && be->obj->get("sessionstate").toString() == "playing");
    if (!be) return;
    const game::MpClient& cl = host.mpState().clients[size_t(bot)];
    const game::NavGraph& g = host.mpNav();
    namespace F = game::NavLinkFlag;
    struct Kind { const char* name; uint8_t flags; int up; const char* prefix; };
    const Kind kinds[] = {{"ladder up", F::Ladder, 1, "climb"}, {"ladder down", F::Ladder, -1, "climb"}, {"mantle", F::Mantle, 0, "mantle_"}};
    for (const Kind& k : kinds) {
        int tried = 0, seen = 0, wired = 0;
        for (size_t i = 0; i < g.size() && tried < 3; ++i)
            for (const auto& l : g.links[i]) {
                if ((l.flags & F::Special) != k.flags) continue;
                const auto a = g.nodes[i], b = g.nodes[l.target];
                if (k.up && (b[2] - a[2]) * float(k.up) < 0) continue;
                if (host.paths().findPath(int(i), int(l.target)).size() != 2) continue;
                ++tried;
                be->obj->set("origin", script::Value(script::Vec3{a[0], a[1], a[2]}));
                host.mpBotSetGoal(bot, {b[0], b[1], b[2]}, 8.0f);
                bool sawType = false, sawWire = false;
                for (int f = 0; f < 20 * 15; ++f) {
                    host.frame();
                    if (cl.anim.moveType.rfind(k.prefix, 0) == 0) {
                        sawType = true;
                        const net::SnapshotView v = game.snapshot(1);
                        const auto it = v.entities.find(bot);
                        const int la = it == v.entities.end() ? -1 : getI(it->second, 204) & 1023;
                        // The node on the wire is the climb / mantle animation itself (not, say, a jump the legs still play).
                        const auto& nodes = be->anim ? be->anim->tree().nodes() : std::vector<game::AnimNode>{};
                        const std::string name = la > 0 && size_t(la) < nodes.size() ? nodes[size_t(la)].name : std::string();
                        sawWire |= la == host.mpLegsAnimWire(bot) && la == cl.anim.legsNode &&
                                   (name.rfind(std::string(k.up ? "pb_climb" : "mp_mantle_"), 0) == 0);
                    }
                    const auto o = be->obj->get("origin").asVector();
                    if (std::hypot(o[0] - b[0], o[1] - b[1]) < 24 && std::fabs(o[2] - b[2]) < 24) break;
                }
                seen += sawType;
                wired += sawWire;
                std::cout << k.name << " (" << a[0] << " " << a[1] << " " << a[2] << "): move type seen " << sawType << ", in legsAnim " << sawWire << "\n";
                i += 40;
                break;
            }
        CHECK(tried > 0);
        CHECK(seen == tried && wired == tried);
    }
    CHECK(host.vm().errors().empty());
}

} // namespace

int main() {
    moveTypeTests();
    viewStateTests();
    if (const char* main = std::getenv("IW2_MAIN")) {
        fs::Vfs vfs;
        vfs.mountDirectory(main);
        worldTests(vfs);
        botTests(vfs);
    } else {
        std::cout << "IW2_MAIN not set, world part skipped\n";
    }
    if (failures) std::cerr << failures << " failure(s)\n";
    return failures ? 1 : 0;
}
