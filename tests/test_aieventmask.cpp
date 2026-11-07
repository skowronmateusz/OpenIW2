// Receivers of the AI events of a bullet (docs/spec/ai.md "AI events", Bullet_Fire_Extended 0x531420..0x531489) and the danger plane
// expiry 0x54b4d0 (needs IW2_MAIN; skipped without it):
// 1. An actor's shot tells only its enemy team: an enemy beside the line gets the attacker mark and a hostile danger plane
//    (issuppressed true); a team mate and a neutral actor beside the same line learn nothing.
// 2. The player's shot also reaches its own team (a short-lived plane, issuppressed false) but not a neutral actor.
// 3. Expiry clears only the time of a slot: the slot keeps its source, so the same source gets the same slot back.
// 4. maymovetopoint (0x53dce0) refuses a step that crosses, in 2D, a team mate's line to its enemy (0x53dc50).
// 5. trackscriptstate (0x540280) keeps the state, the old state and the reason.
#include "fs/vfs.h"
#include "game/world.h"

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace iw2::game;
using iw2::script::Value;

namespace {
int failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                             \
        }                                                                           \
    } while (0)

bool planeFrom(const Entity& e, int source) {
    for (const DangerPlane& p : e.dangerPlanes)
        if (p.timeMs != 0 && p.sourceEnt == source) return true;
    return false;
}
} // namespace

int main() {
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "aieventmask test skipped (IW2_MAIN not set)\n";
        return 0;
    }
    namespace fsys = std::filesystem;
    const fsys::path dir = fsys::temp_directory_path() / ("iw2_test_aieventmask_" + std::to_string(::getpid()));
    fsys::remove_all(dir);
    fsys::create_directories(dir / "maps");
    std::ofstream(dir / "maps" / "iw2test_aieventmask.gsc") << R"(
track()
{
	self trackscriptstate("melee", "in range");
	self trackscriptstate("cover", "hide");
}
)";
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    vfs.mountDirectory(dir);
    World w(vfs, "toujane");
    w.start();
    for (int i = 0; i < 5; ++i) w.frame();
    std::vector<Entity*> actors;
    for (const auto& e : w.entities())
        if (e->isActor && !e->isSpawner && e->obj->get("health").isNumber() && e->obj->get("health").asInt() > 0) actors.push_back(e.get());
    Entity* player = w.player();
    CHECK(actors.size() >= 4 && player);
    if (actors.size() < 4 || !player) return 1;
    Entity& shooter = *actors[0];
    Entity& enemy = *actors[1];
    Entity& mate = *actors[2];
    Entity& neutral = *actors[3];
    const Vec3 enemyWas0 = enemy.obj->get("origin").asVector();
    // 6. The muzzle is computed once per level frame (0x500b57): moving the actor within the frame does not move it; the next
    // frame does.
    {
        Entity* gunman = nullptr;
        Vec3 m0{};
        for (Entity* e : actors)
            if (w.debugMuzzlePosition(*e, m0)) {
                gunman = e;
                break;
            }
        CHECK(gunman != nullptr);
        if (gunman) {
            const Vec3 was = gunman->obj->get("origin").asVector();
            gunman->obj->set("origin", Value(Vec3{was[0] + 100.0f, was[1], was[2]}));
            Vec3 m1{};
            CHECK(w.debugMuzzlePosition(*gunman, m1) && m1[0] == m0[0] && m1[1] == m0[1] && m1[2] == m0[2]);
            gunman->muzzleFrameMs = w.timeMs() - 50; // as if the frame had advanced
            CHECK(w.debugMuzzlePosition(*gunman, m1) && m1[0] != m0[0]);
            gunman->obj->set("origin", Value(was));
            gunman->muzzleFrameMs = -1;
        }
    }
    // 5. trackscriptstate.
    w.vm().start("maps/iw2test_aieventmask.gsc", "track", {}, Value(shooter.obj));
    CHECK(shooter.scriptState == "cover" && shooter.lastScriptState == "melee" && shooter.stateChangeReason == "hide");
    // 4. maymovetopoint where the actor stands, with a working goal around it: a short step passes while no team mate's fire line
    // crosses it, fails once one does, and passes again when the line lies beyond the step's end.
    {
        for (Entity* e : actors) e->obj->set("enemy", Value());
        // The walker: the first actor (not a rider: a linked actor always fails) whose short step passes.
        Entity* walker = nullptr;
        for (Entity* e : actors) {
            if (e == &mate || e == &enemy) continue;
            const Vec3 eo = e->obj->get("origin").asVector();
            e->workGoal.radius = 1000.0f;
            if (w.debugMayMoveToPoint(*e, Vec3{eo[0] + 16.0f, eo[1], eo[2]})) {
                walker = e;
                break;
            }
        }
        CHECK(walker != nullptr);
        if (!walker) return 1;
        const Vec3 o = walker->obj->get("origin").asVector();
        const Vec3 step{o[0] + 16.0f, o[1], o[2]};
        const std::string mateTeam = mate.obj->get("team").toString();
        mate.obj->set("team", Value(walker->obj->get("team").toString()));
        const Vec3 mateWas = mate.obj->get("origin").asVector(), enemyWas = enemy.obj->get("origin").asVector();
        mate.obj->set("origin", Value(Vec3{o[0] + 8.0f, o[1] - 300.0f, o[2]}));
        enemy.obj->set("origin", Value(Vec3{o[0] + 8.0f, o[1] + 300.0f, o[2]}));
        mate.obj->set("enemy", Value(enemy.obj));
        CHECK(!w.debugMayMoveToPoint(*walker, step));
        mate.obj->set("origin", Value(Vec3{o[0] + 40.0f, o[1] - 300.0f, o[2]}));
        enemy.obj->set("origin", Value(Vec3{o[0] + 40.0f, o[1] + 300.0f, o[2]}));
        CHECK(w.debugMayMoveToPoint(*walker, step));
        mate.obj->set("enemy", Value());
        mate.obj->set("team", Value(mateTeam));
        mate.obj->set("origin", Value(mateWas));
        enemy.obj->set("origin", Value(enemyWas));
    }
    shooter.obj->set("team", Value(std::string("axis")));
    enemy.obj->set("team", Value(std::string("allies")));
    mate.obj->set("team", Value(std::string("axis")));
    neutral.obj->set("team", Value(std::string("neutral")));
    player->obj->set("team", Value(std::string("allies")));
    // High above the shooter's spot, so that the line meets nothing: the three receivers stand 60 units beside it.
    const Vec3 base = shooter.obj->get("origin").asVector();
    const Vec3 a{base[0], base[1], base[2] + 3000.0f}, b{base[0] + 1000.0f, base[1], base[2] + 3000.0f};
    shooter.obj->set("origin", Value(Vec3{a[0] - 10.0f, a[1], a[2] - 50.0f}));
    enemy.obj->set("origin", Value(Vec3{a[0] + 300.0f, a[1] + 60.0f, a[2] - 30.0f}));
    mate.obj->set("origin", Value(Vec3{a[0] + 500.0f, a[1] + 60.0f, a[2] - 30.0f}));
    neutral.obj->set("origin", Value(Vec3{a[0] + 700.0f, a[1] + 60.0f, a[2] - 30.0f}));
    for (Entity* e : {&enemy, &mate, &neutral}) {
        e->dangerPlanes = {};
        e->sentientInfo.clear();
    }
    // 1. The actor's shot.
    w.debugFireBulletFrom(shooter, a, b);
    CHECK(planeFrom(enemy, shooter.number) && w.isSuppressed(enemy));
    CHECK(enemy.sentientInfo.count(shooter.obj.get()) && enemy.sentientInfo[shooter.obj.get()].lastAttackedMs == w.timeMs());
    CHECK(!planeFrom(mate, shooter.number) && mate.sentientInfo.count(shooter.obj.get()) == 0);
    CHECK(!planeFrom(neutral, shooter.number) && neutral.sentientInfo.count(shooter.obj.get()) == 0);
    // 2. The player's shot along the same line: its own team (allies) gets a short-lived plane, the axis team mate a hostile one,
    // the neutral actor nothing.
    for (Entity* e : {&enemy, &mate, &neutral}) e->dangerPlanes = {};
    w.debugFireBullet(a, b);
    CHECK(planeFrom(enemy, player->number) && !w.isSuppressed(enemy));
    CHECK(planeFrom(mate, player->number) && w.isSuppressed(mate));
    CHECK(!planeFrom(neutral, player->number) && neutral.sentientInfo.count(player->obj.get()) == 0);
    // 3. Expiry keeps the source: after 300 ms the short-lived slot is free but still names the player, and the player's next
    // plane goes back into it (0x54b370 matches the source first).
    int slot = -1;
    for (int i = 0; i < 4; ++i)
        if (enemy.dangerPlanes[size_t(i)].sourceEnt == player->number) slot = i;
    CHECK(slot >= 0);
    if (slot < 0) return 1;
    enemy.dangerPlanes[size_t(slot)].timeMs = w.timeMs() - 300; // 300 ms old
    enemy.dangerPlanes[size_t((slot + 1) % 4)] = {};
    w.debugExpireDangerPlanes(enemy);
    CHECK(slot >= 0 && enemy.dangerPlanes[size_t(slot)].timeMs == 0 && enemy.dangerPlanes[size_t(slot)].sourceEnt == player->number);
    w.debugAddDangerPlane(enemy, *player, a, b);
    CHECK(slot >= 0 && enemy.dangerPlanes[size_t(slot)].timeMs == w.timeMs());
    // 7. The missile ping of a rocket (event 9, every frame, 0x526bd0): an actor with grenadeawareness 1 within 512 units of a flying
    // rocket takes it as its grenade (0x5200e0) with a predicted landing, unless it is safe from it at once.
    {
        Entity& a = enemy;
        a.obj->set("team", Value(std::string("allies")));
        a.obj->set("grenadeawareness", Value(1.0f));
        a.ai.grenade = nullptr;
        a.obj->set("origin", Value(enemyWas0));
        w.frame(); // the actor settles where it stands
        const Vec3 ao = a.obj->get("origin").asVector();
        // The player (allies) fires it from 300 units away along the ground towards the actor, so its landing is near the actor.
        Entity& r = w.debugSpawnRocket("panzerfaust", Vec3{ao[0] - 300.0f, ao[1], ao[2] + 40.0f}, Vec3{1, 0, 0});
        CHECK(World::missileAi(r) != nullptr && World::missileAi(r) == &r.rocket->ai);
        bool pinged = false;
        for (int i = 0; i < 3 && !pinged && !r.obj->removed; ++i) {
            w.frame();
            const GrenadeState* g = World::missileAi(r);
            const bool landed = g && (g->landing[0] != 0.0f || g->landing[1] != 0.0f || g->landing[2] != 0.0f);
            pinged = landed;
        }
        if (!pinged)
            std::cerr << "rocket removed " << r.obj->removed << " actor alive " << a.ai.alive << " grenade " << (a.ai.grenade == &r)
                      << " actor at " << a.obj->get("origin").asVector()[0] << "," << a.obj->get("origin").asVector()[2] << " rocket at "
                      << r.obj->get("origin").asVector()[0] << "," << r.obj->get("origin").asVector()[2] << " corpse " << a.ai.corpse
                      << " removed " << a.obj->removed << "\n";
        CHECK(pinged);
        CHECK(World::missileAi(r)->trType == 2 && !World::missileAi(r)->bounce && World::missileAi(r)->weapon == "panzerfaust");
    }
    fsys::remove_all(dir);
    if (failures) return 1;
    std::cout << "aieventmask tests passed\n";
    return 0;
}
