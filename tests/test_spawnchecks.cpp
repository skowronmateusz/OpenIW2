// Actor spawn checks, docs/spec/ai.md "Spawn checks": the telefrag test (0x4e8500: spawn box against the entity
// absolute boxes widened by 1, the player's height from its stance box), and the force-spawn eviction (0x4fcaf0: the
// farthest actor the player cannot see first, undeletable actors (spawnflags 4) never, none left -> no eviction).
// Needs the game files (IW2_MAIN); uses cityhall's spawner at (4386 -4304).

#include "fs/vfs.h"
#include "game/world.h"

#include <cstdlib>
#include <iostream>

namespace {

int failures = 0;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                             \
        }                                                                           \
    } while (0)

using iw2::script::Value;
using iw2::script::Vec3;

int liveActors(iw2::game::World& w) {
    int n = 0;
    for (const auto& e : w.entities())
        if (e->isActor && !e->obj->removed && !e->ai.corpse) ++n;
    return n;
}

} // namespace

int main() {
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "IW2_MAIN not set, skipped\n";
        return 0;
    }
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    iw2::game::World w(vfs, "cityhall");
    w.prepareStart();
    iw2::game::Entity* spawner = nullptr;
    for (const auto& e : w.entities()) {
        if (!e->isSpawner || e->obj->removed) continue;
        const Value o = e->obj->get("origin");
        if (o.isVector() && o.asVector()[0] == 4386.0f && o.asVector()[1] == -4304.0f) spawner = e.get();
    }
    iw2::game::Entity* player = w.player();
    CHECK(spawner != nullptr);
    CHECK(player != nullptr);
    if (!spawner || !player) return 1;
    player->obj->set("health", Value(int32_t(100)));
    const Vec3 s = spawner->obj->get("origin").asVector();

    // Telefrag: player box (-15,-15,0)..(15,15,h) widened by 1 against the spawn box (-15,-15,0)..(15,15,70).
    auto placePlayer = [&](float dx, float dz) { player->obj->set("origin", Value(Vec3{s[0] + dx, s[1], s[2] + dz})); };
    placePlayer(0, 0);
    CHECK(w.debugWouldTelefrag(*spawner));
    placePlayer(31.0f, 0);
    CHECK(w.debugWouldTelefrag(*spawner)); // 15 + 1 + 15 = 31: touching counts
    placePlayer(31.5f, 0);
    CHECK(!w.debugWouldTelefrag(*spawner));
    placePlayer(0, -71.0f); // standing: box top at -71 + 70 + 1 = 0
    CHECK(w.debugWouldTelefrag(*spawner));
    placePlayer(0, -72.0f);
    CHECK(!w.debugWouldTelefrag(*spawner));
    iw2::game::World::PlayerCommand pc;
    pc.stance = 2; // prone: r.maxs[2] 30
    w.setPlayerCommand(pc);
    placePlayer(0, -31.0f);
    CHECK(w.debugWouldTelefrag(*spawner));
    placePlayer(0, -40.0f);
    CHECK(!w.debugWouldTelefrag(*spawner));
    pc.stance = 0;
    w.setPlayerCommand(pc);
    placePlayer(0, 0);
    player->obj->set("health", Value(int32_t(0)));
    CHECK(!w.debugWouldTelefrag(*spawner)); // pm_type >= 4 (dead) does not block
    player->obj->set("health", Value(int32_t(100)));
    CHECK(w.debugSpawnActor(*spawner, false) == nullptr); // dospawn refuses
    placePlayer(4000.0f, 0);

    // Fill the 32 actor slots (stalingradspawn skips the telefrag test).
    spawner->obj->set("count", Value(int32_t(1000)));
    std::vector<iw2::game::Entity*> actors;
    while (liveActors(w) < 32) {
        iw2::game::Entity* a = w.debugSpawnActor(*spawner, true);
        CHECK(a != nullptr);
        if (!a) break;
        actors.push_back(a);
    }
    CHECK(liveActors(w) == 32);
    CHECK(w.debugSpawnActor(*spawner, true) == nullptr); // no free actors, no spawnflags 2

    // While the level is being set up the eviction is the exe's Com_Error: nothing is freed.
    CHECK(!w.debugEvictForForceSpawn());
    CHECK(liveActors(w) == 32);
    w.debugSetLevelLoaded(true);

    // Every actor at the spawner except one far below the world (farthest, and the player cannot see it).
    if (actors.size() >= 2) {
        iw2::game::Entity* far = actors[1];
        far->obj->set("origin", Value(Vec3{s[0], s[1], s[2] - 20000.0f}));
        CHECK(w.debugEvictForForceSpawn());
        CHECK(far->obj->removed);
        CHECK(liveActors(w) == 31);
        // A spawner with spawnflags 2 (forcespawn) evicts when the pool is full again.
        CHECK(w.debugSpawnActor(*spawner, true) != nullptr);
        CHECK(liveActors(w) == 32);
        const Value sf = spawner->obj->get("spawnflags");
        spawner->obj->set("spawnflags", Value(int32_t((sf.isInt() ? sf.asInt() : 0) | 2)));
        CHECK(w.debugSpawnActor(*spawner, true) != nullptr);
        CHECK(liveActors(w) == 32);
        // Undeletable actors (spawnflags 4) are never evicted.
        for (const auto& e : w.entities())
            if (e->isActor && !e->obj->removed) e->obj->set("$spawnflags", Value(int32_t(4)));
        CHECK(!w.debugEvictForForceSpawn());
        CHECK(w.debugSpawnActor(*spawner, true) == nullptr);
        CHECK(liveActors(w) == 32);
    }
    if (failures) std::cerr << failures << " failure(s)\n";
    else std::cout << "spawnchecks ok\n";
    return failures ? 1 : 0;
}
