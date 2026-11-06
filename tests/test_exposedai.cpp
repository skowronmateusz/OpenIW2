// Exposed state details (needs IW2_MAIN): the goal edge stop 0x548be0, the reacquire shortcut of 0x548d50 for a followed or
// blocked actor, the "grenade danger" notify of 0x5200e0 and the path test 0x51fe90.
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "game/world.h"

using namespace iw2::game;

static int failures = 0;
#define CHECK(c)                                                                         \
    do {                                                                                 \
        if (!(c)) { std::cout << "FAIL line " << __LINE__ << ": " #c "\n"; ++failures; } \
    } while (0)

int main() {
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "exposedai: IW2_MAIN not set, skipped\n";
        return 0;
    }
    namespace fsys = std::filesystem;
    const fsys::path dir = fsys::temp_directory_path() / ("iw2_test_exposedai_" + std::to_string(::getpid()));
    fsys::remove_all(dir);
    fsys::create_directories(dir / "maps");
    std::ofstream(dir / "maps" / "iw2test_exposedai.gsc") << R"(
watch()
{
	self waittill("grenade danger", g);
	level.t_danger = g;
}

maymove()
{
	level.t_may = self maymovetopoint(self.origin + (1, 0, 0));
}
)";
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    vfs.mountDirectory(dir);
    World world(vfs, "toujane");
    world.start();
    for (int i = 0; i < 5; ++i) world.frame();
    Entity* actor = nullptr;
    for (const auto& e : world.entities())
        if (e->isActor && e->obj->get("health").isNumber() && e->obj->get("health").asInt() > 0) {
            actor = e.get();
            break;
        }
    CHECK(actor != nullptr);
    if (!actor) return 1;
    actor->obj->set("team", iw2::script::Value(std::string("axis")));
    if (world.player()) world.player()->obj->set("team", iw2::script::Value(std::string("allies")));
    const Vec3 o = actor->obj->get("origin").asVector();

    // 0x548be0
    auto edge = [&](float radius) {
        actor->workGoal.set = true;
        actor->workGoal.pos = o;
        actor->workGoal.radius = radius;
        actor->workGoal.height = 512;
        actor->ai.animMode = 4;
        actor->ai.movemode = 2;
        world.debugStopNearGoalEdge(*actor);
        return actor->ai.animMode;
    };
    actor->ai.pathToEnemy = false;
    actor->ai.mayMoveMs = -1000;
    CHECK(edge(100.0f) == 4);                     // well inside
    CHECK(edge(20.0f) == 1);                      // a radius up to 30 always stops
    CHECK(actor->ai.movemode == 0);
    actor->ai.mayMoveMs = world.debugTimeMs();    // a maymovetopoint that just passed
    CHECK(edge(20.0f) == 4);
    actor->ai.mayMoveMs = -1000;
    actor->ai.pathToEnemy = true;
    CHECK(edge(20.0f) == 4);
    actor->ai.pathToEnemy = false;

    // 0x53df8d: a passing maymovetopoint with its end in the goal stamps +0x744
    actor->workGoal.radius = 1000.0f;
    actor->ai.mayMoveMs = -1000;
    world.vm().start("maps/iw2test_exposedai.gsc", "maymove", {}, iw2::script::Value(actor->obj));
    CHECK(!world.vm().level()->get("t_may").truthy() || actor->ai.mayMoveMs == world.debugTimeMs());
    if (world.vm().level()->get("t_may").truthy()) std::cout << "maymove stamped\n";

    // 0x53dc50: a move across a team mate's line to its enemy is refused
    Entity* mate = nullptr;
    Entity* walker = nullptr;
    for (const auto& e : world.entities())
        if (e->isActor && e->obj->get("health").isNumber() && e->obj->get("health").asInt() > 0) {
            if (!world.debugLinked(*e) && !walker) walker = e.get();
            else if (e.get() != actor && !mate) mate = e.get();
        }
    CHECK(mate != nullptr && walker != nullptr);
    if (world.debugLinked(*actor)) CHECK(!world.debugMayMoveToPoint(*actor, o)); // 0x53dd2c: a linked actor
    if (mate && walker && world.player()) {
        const iw2::script::Value mateOrigin = mate->obj->get("origin"), mateTeam = mate->obj->get("team"),
                                 walkerTeam = walker->obj->get("team"), playerOrigin = world.player()->obj->get("origin");
        const Vec3 w = walker->obj->get("origin").asVector();
        const Vec3 step{w[0] + 1, w[1], w[2]};
        walker->obj->set("team", iw2::script::Value(std::string("axis")));
        walker->workGoal.set = true;
        walker->workGoal.scriptVolume = false;
        walker->workGoal.pos = w;
        walker->workGoal.radius = 1000.0f;
        walker->workGoal.height = 512.0f;
        const bool clear = world.debugMayMoveToPoint(*walker, step);
        mate->obj->set("team", iw2::script::Value(std::string("axis")));
        mate->obj->set("origin", iw2::script::Value(Vec3{w[0] + 0.5f, w[1] + 50, w[2]}));
        world.player()->obj->set("origin", iw2::script::Value(Vec3{w[0] + 0.5f, w[1] - 50, w[2]}));
        world.debugSetEnemy(*mate, world.player());
        CHECK(clear);
        CHECK(!world.debugMayMoveToPoint(*walker, step));
        world.debugSetEnemy(*mate, nullptr);
        CHECK(world.debugMayMoveToPoint(*walker, step) == clear);
        mate->obj->set("origin", mateOrigin);
        mate->obj->set("team", mateTeam);
        walker->obj->set("team", walkerTeam);
        world.player()->obj->set("origin", playerOrigin);
    }

    // 0x53df50: an end outside the working goal is refused and does not stamp, even where the move itself is clear
    if (walker) {
        const Vec3 w = walker->obj->get("origin").asVector();
        walker->workGoal.pos = {w[0] + 5000, w[1], w[2]};
        walker->workGoal.radius = 16.0f;
        walker->ai.mayMoveMs = -1000;
        CHECK(!world.debugMayMoveToPoint(*walker, {w[0] + 1, w[1], w[2]}));
        CHECK(walker->ai.mayMoveMs == -1000);
        walker->workGoal.pos = w;
        walker->workGoal.radius = 1000.0f;
        CHECK(world.debugMayMoveToPoint(*walker, {w[0] + 1, w[1], w[2]}));
        CHECK(walker->ai.mayMoveMs == world.debugTimeMs());
    }

    // 0x5437f0: the three kinds of stretch, with the test as observed
    {
        auto& p = actor->path;
        const auto saved = p;
        p.count = 3;
        p.lookIndex = 1;
        p.lookDist = 100;
        p.lookDir = {1, 0, 0};
        p.lookSegLen = 0;
        p.pts[0].pos = {o[0] + 1000, o[1], o[2]};
        p.pts[0].dir = {1, 0};
        p.pts[0].len = 200;
        p.pts[1].pos = {o[0] + 100, o[1], o[2]};
        p.pts[1].dir = {1, 0};
        p.pts[1].len = 900;
        const float r2 = 16 * 16;
        auto at = [&](float x, float y) { return Vec3{o[0] + x, o[1] + y, o[2]}; };
        CHECK(world.debugPathNearPoint(*actor, at(150, 10), r2));   // lookahead stretch, inside
        CHECK(!world.debugPathNearPoint(*actor, at(150, 40), r2));  // off the line
        CHECK(world.debugPathNearPoint(*actor, at(1100, 0), r2));   // point 0 stretch
        CHECK(!world.debugPathNearPoint(*actor, at(600, 0), r2));   // lookIndex's own stretch needs lookSegLen
        p.lookSegLen = 900;
        CHECK(world.debugPathNearPoint(*actor, at(600, 0), r2));
        p = saved;
    }

    // 0x519050: a take-over evicts the actor claiming a linked node (its path to it goes, the node is blocked for its team)
    if (mate) {
        const int a = 0, b = 1;
        auto node = [&](int i) -> iw2::game::PathNode& { return const_cast<iw2::game::PathNode&>(world.nodes()[size_t(i)]); };
        world.debugClaimNode(*mate, -1, false);
        world.debugClaimNode(*actor, -1, false);
        node(a).link = {-1, -1};
        node(b).link = {-1, -1};
        node(a).owner = node(b).owner = nullptr;
        node(a).ownerUntilMs = node(b).ownerUntilMs = 0;
        world.debugClaimNode(*mate, b, false);
        CHECK(mate->claimedNode == b);
        node(a).link = {b, -1};
        world.debugClaimNode(*actor, a, true);
        CHECK(actor->claimedNode == a);
        CHECK(mate->claimedNode == -1);
        world.debugClaimNode(*actor, -1, false);
        node(a).link = {-1, -1};
    }

    // 0x515150: the team is told about a new enemy 1000 ms on, by a notifying call of the setter, or at once when it changes
    if (world.player()) {
        world.debugSetEnemy(*actor, nullptr);
        CHECK(actor->enemyEventMs == 0);
        world.debugSetEnemy(*actor, world.player());
        CHECK(actor->enemyEventMs == world.debugTimeMs() + 1000);
        world.debugSetEnemy(*actor, world.player()); // same enemy, not due: still pending
        CHECK(actor->enemyEventMs == world.debugTimeMs() + 1000);
        actor->enemyEventMs = world.debugTimeMs();   // due
        world.debugSetEnemy(*actor, world.player());
        CHECK(actor->enemyEventMs == 0);
        world.debugSetEnemy(*actor, nullptr);
    }

    // 0x549520 without a path, holding the position (keepclaimednode, inside the goal): a flagged node gives 6, also with an enemy
    actor->obj->set("keepclaimednode", iw2::script::Value(int32_t(1)));
    actor->path.count = 0;
    if (world.player()) world.debugSetEnemy(*actor, world.player());
    const int flaggedFacing = world.debugNoncombatFacing(*actor, true);
    const int plainFacing = world.debugNoncombatFacing(*actor, false);
    std::cout << "noncombat facing " << flaggedFacing << " " << plainFacing << " path " << actor->path.count << "\n";
    CHECK(flaggedFacing == 6);
    CHECK(plainFacing == 3);

    // 0x549520 with a path to a point in the goal, 0x5495b7..0x549634
    const Vec3 near{o[0] + 40, o[1], o[2]};
    auto setGoal = [&](const Vec3& at, float radius) {
        actor->obj->set("$goalpos", iw2::script::Value(at));
        actor->obj->set("goalradius", iw2::script::Value(radius));
        actor->workGoal.set = false;
    };
    auto pathed = [&](bool flagged, int64_t age) {
        actor->obj->set("origin", iw2::script::Value(o));
        setGoal(near, flagged ? 8.0f : 200.0f); // flagged: the actor is still outside the goal
        if (!world.debugPathToPoint(*actor, near)) return -1;
        actor->path.planMs = world.debugTimeMs() - age;
        const int f = world.debugNoncombatFacing(*actor, flagged);
        return f;
    };
    CHECK(pathed(false, 0) == 4);
    CHECK(pathed(true, 0) == 6);
    CHECK(pathed(true, 5000) == 2);

    // 0x5497ec: with a path and the enemy seen, a followed, blocked or holding actor drops it; otherwise it keeps moving
    if (Entity* p = world.player()) {
        auto combat = [&](Entity* followed, Entity* blocker, bool hold) {
            actor->obj->set("keepclaimednode", iw2::script::Value(int32_t(hold ? 1 : 0)));
            actor->claimedNode = hold ? 0 : -1;
            actor->obj->set("origin", iw2::script::Value(o));
            const Vec3 far{o[0] + 400, o[1], o[2]};
            setGoal(hold ? o : far, hold ? 1000.0f : 64.0f); // outside the goal the position does not hold
            if (!world.debugPathToPoint(*actor, hold ? near : far)) return -1;
            actor->ai.followed = followed;
            actor->ai.blocker = blocker;
            auto& info = actor->sentientInfo[p->obj.get()];
            info.visible = true;
            info.lastCheckMs = world.debugTimeMs();
            world.debugExposedCombat(*actor);
            actor->ai.followed = actor->ai.blocker = nullptr;
            return actor->path.count;
        };
        CHECK(combat(p, nullptr, false) == 0);
        CHECK(combat(nullptr, actor, false) == 0);
        CHECK(combat(nullptr, nullptr, true) == 0);
        CHECK(combat(nullptr, nullptr, false) > 0);
    }
    actor->obj->set("origin", iw2::script::Value(o));
    setGoal(o, 1000.0f);
    actor->obj->set("keepclaimednode", iw2::script::Value(int32_t(0)));
    actor->claimedNode = -1;

    // 0x5240dc: fleeing, a followed or blocked actor drops the path
    auto flee = [&](Entity* followed) {
        actor->obj->set("origin", iw2::script::Value(o));
        if (!world.debugPathToPoint(*actor, {o[0] + 400, o[1], o[2]})) return -1;
        actor->ai.badPlace = true;
        actor->ai.followed = followed;
        world.debugBadPlaceThink(*actor);
        actor->ai.badPlace = false;
        actor->ai.followed = nullptr;
        return actor->path.count;
    };
    CHECK(flee(world.player()) == 0);
    CHECK(flee(nullptr) > 0);
    actor->path.count = 0;

    // 0x548d50: reacquire with a path, the enemy not in sight: combat only when followed or blocked
    Entity* player = world.player();
    CHECK(player != nullptr);
    if (player) {
        world.debugSetEnemy(*actor, player);
        actor->path.count = 2;
        actor->ai.substate = 0x66;
        actor->sentientInfo.clear();
        actor->ai.followed = player;
        world.debugExposedSubstate(*actor);
        CHECK(actor->ai.substate == 100);
        actor->ai.followed = nullptr;
        actor->ai.substate = 0x66;
        actor->ai.blocker = actor;
        world.debugExposedSubstate(*actor);
        CHECK(actor->ai.substate == 100);
        actor->ai.blocker = nullptr;
        actor->path.count = 0;
    }

    actor->obj->set("origin", iw2::script::Value(o));
    // 0x51fe90
    Entity& g = world.debugSpawnGrenade("frag_grenade_american_mp", {o[0] + 100, o[1], o[2] + 40}, {0, 0, 0}, 3000);
    actor->path.count = 2;
    actor->path.lookDir = {1, 0, 0};
    actor->obj->set("$goalpos", iw2::script::Value(Vec3{o[0] + 5000, o[1], o[2]}));
    g.grenade->landing = {o[0] - 100, o[1], o[2]};
    CHECK(world.debugGrenadePathLeadsAway(*actor, g));   // behind the path direction
    g.grenade->landing = {o[0] + 100, o[1], o[2]};
    CHECK(world.debugGrenadePathLeadsAway(*actor, g));   // ahead, from the enemy team, over 4 units
    g.grenade->landing = {o[0] + 1, o[1], o[2]};
    CHECK(!world.debugGrenadePathLeadsAway(*actor, g));  // too close
    g.grenade->landing = {o[0], o[1] + 100, o[2]};
    CHECK(!world.debugGrenadePathLeadsAway(*actor, g));  // sideways
    actor->path.count = 0;
    CHECK(!world.debugGrenadePathLeadsAway(*actor, g));  // no path

    // 0x5201c3: an enemy grenade near the actor notifies it
    actor->ai.grenade = nullptr;
    actor->obj->set("grenadeawareness", iw2::script::Value(1.0f));
    world.vm().start("maps/iw2test_exposedai.gsc", "watch", {}, iw2::script::Value(actor->obj));
    for (int i = 0; i < 20 && !world.vm().level()->get("t_danger").isObject(); ++i) world.frame();
    CHECK(world.vm().level()->get("t_danger").isObject());

    // 0x548a34: dying with a grenade in the hand lets it go
    Entity& held = world.debugSpawnGrenade("frag_grenade_american_mp", {o[0], o[1], o[2] + 40}, {0, 0, 0}, 3000);
    actor->ai.grenade = &held;
    held.grenade->holder = actor;
    held.grenade->inHand = true;
    actor->ai.substate = 200;
    actor->ai.baseStateMs = world.debugTimeMs() - 1000;
    world.debugDeathThink(*actor);
    CHECK(held.grenade->holder == nullptr);
    CHECK(!held.grenade->inHand);

    fsys::remove_all(dir);
    std::cout << (failures ? "exposedai FAILED\n" : "exposedai ok\n");
    return failures ? 1 : 0;
}
