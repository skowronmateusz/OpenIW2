// Soft-lock diagnostics (src/game/softlock.cpp, iw2game --report): the spawner classification, the group verdict, the spawner
// statistics of the spawn core, and the analysis on a real map (trainyard's AI groups): a group whose spawners are still started
// by a waiting trigger is pending; when nothing alive can spawn from them any more it is a SOFTLOCK.
// The map part needs the game files (IW2_MAIN).

#include "fs/vfs.h"
#include "game/softlock.h"
#include "game/world.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>

namespace {

int failures = 0;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                             \
        }                                                                           \
    } while (0)

using namespace iw2::game;
using iw2::script::Value;
using iw2::script::Vec3;

const AiGroupReport* findGroup(const SoftlockReport& r, const std::string& name) {
    for (const auto& g : r.groups)
        if (g.name == name) return &g;
    return nullptr;
}

} // namespace

int main() {
    // classification
    CHECK(classifySpawner({0, 0, 0}) == SpawnerLife::Dead);
    CHECK(classifySpawner({1, 0, 0}) == SpawnerLife::Driven);
    CHECK(classifySpawner({0, 1, 0}) == SpawnerLife::Pending);
    CHECK(classifySpawner({0, 0, 2}) == SpawnerLife::Pending);
    CHECK(classifySpawner({1, 1, 1}) == SpawnerLife::Driven);
    CHECK(groupIsSoftlock(1, {SpawnerLife::Dead}));
    CHECK(groupIsSoftlock(2, {SpawnerLife::Dead, SpawnerLife::Dead}));
    CHECK(!groupIsSoftlock(2, {SpawnerLife::Dead, SpawnerLife::Pending}));
    CHECK(!groupIsSoftlock(1, {SpawnerLife::Driven}));
    CHECK(!groupIsSoftlock(0, {SpawnerLife::Dead}));
    CHECK(groupIsSoftlock(1, {})); // spawnercount left over with no spawner that has a count

    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "IW2_MAIN not set, map part skipped\n";
        return failures ? 1 : 0;
    }
    const auto dir = std::filesystem::temp_directory_path() / ("iw2_test_softlock_" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir / "maps");
    std::ofstream(dir / "maps/sltest.gsc") << "waiter()\n{\n\tself waittill(\"spawned\", guy);\n}\n";
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    vfs.mountDirectory(dir.string());

    { // spawner statistics: a refused dospawn is counted by reason, a successful one counts as spawned
        World w(vfs, "cityhall");
        w.prepareStart();
        Entity* spawner = nullptr;
        for (const auto& e : w.entities()) {
            if (!e->isSpawner || e->obj->removed) continue;
            const Value o = e->obj->get("origin");
            if (o.isVector() && o.asVector()[0] == 4386.0f && o.asVector()[1] == -4304.0f) spawner = e.get();
        }
        Entity* player = w.player();
        CHECK(spawner && player);
        if (!spawner || !player) return 1;
        player->obj->set("health", Value(int32_t(100)));
        spawner->obj->set("count", Value(int32_t(3)));
        player->obj->set("origin", spawner->obj->get("origin")); // standing on the spawnpoint: telefrag
        CHECK(w.debugSpawnActor(*spawner, false) == nullptr);
        CHECK(w.debugSpawnActor(*spawner, false) == nullptr);
        const auto& st = w.spawnerStats().at(spawner->number);
        CHECK(st.spawned == 0);
        CHECK(st.initialCount == 3);
        CHECK(st.refusals.at("spawnpoint would telefrag") == 2);
        CHECK(st.lastRefusal == "spawnpoint would telefrag");
        player->obj->set("origin", Value(Vec3{4000.0f, 4000.0f, 4000.0f}));
        CHECK(w.debugSpawnActor(*spawner, true) != nullptr);
        const auto& st2 = w.spawnerStats().at(spawner->number);
        CHECK(st2.spawned == 1);
        CHECK(st2.initialCount == 3);
        CHECK(spawner->obj->get("count").asInt() == 2);
        const SoftlockReport r = analyzeSoftlocks(w);
        bool found = false;
        for (const auto& s : r.spawners)
            if (s.number == spawner->number) {
                found = true;
                CHECK(s.spawned == 1 && s.initialCount == 3 && s.count == 2);
                CHECK(s.refusals.at("spawnpoint would telefrag") == 2);
                CHECK(!s.antispawn);
            }
        CHECK(found);
        // An entity with targetname antispawn whose script_noteworthy names the spawner's targetname marks the spawner (the script
        // ends such a flood on purpose when the player passes that trigger).
        spawner->obj->set("targetname", Value("test_flood"));
        for (const auto& e : w.entities())
            if (e.get() != spawner && !e->isSpawner && !e->obj->removed && e.get() != player) {
                e->obj->set("targetname", Value("antispawn"));
                e->obj->set("script_noteworthy", Value("test_flood"));
                break;
            }
        // A map-script thread waiting for "spawned" on a spawner nothing can start is reported as a dead spawn wait.
        w.vm().start("maps/sltest.gsc", "waiter", {}, Value(spawner->obj));
        w.vm().drainReady();
        const SoftlockReport r2 = analyzeSoftlocks(w);
        CHECK(r2.deadSpawnWaitCount() == 1);
        for (const auto& d : r2.deadSpawnWaits) {
            CHECK(d.targetname == "test_flood");
            CHECK(d.site.rfind("maps/sltest.gsc:", 0) == 0);
            CHECK(d.count == 2);
        }
        bool lineFound = false;
        for (const auto& l : formatSoftlockReport(r2, false)) lineFound = lineFound || l.rfind("DEAD-SPAWN-WAIT x1 targetname=test_flood", 0) == 0;
        CHECK(lineFound);
        found = false;
        for (const auto& s : r2.spawners)
            if (s.number == spawner->number) {
                found = true;
                CHECK(s.antispawn);
            }
        CHECK(found);
    }

    { // trainyard: the hardpoint1 group is pending while its flood trigger is waited on; with the triggers gone it is a SOFTLOCK
        World w(vfs, "trainyard");
        w.start();
        for (int i = 0; i < 200; ++i) w.frame();
        SoftlockReport r = analyzeSoftlocks(w);
        const AiGroupReport* g = findGroup(r, "hardpoint1");
        CHECK(g != nullptr);
        if (!g) return 1;
        CHECK(g->spawnercount > 0);
        CHECK(!g->softlock);
        CHECK(!g->spawners.empty());
        for (size_t i : g->spawners) CHECK(r.spawners[i].life != SpawnerLife::Dead);
        // Free every entity that targets one of the group's spawners (the flood triggers): nothing can start the spawners now.
        std::vector<std::string> names;
        for (size_t i : g->spawners) names.push_back(r.spawners[i].targetname);
        for (const auto& e : w.entities()) {
            if (e->isSpawner || e->obj->removed) continue;
            const Value t = e->obj->get("target");
            if (!t.isString()) continue;
            for (const auto& n : names)
                if (t.asString() == n) {
                    e->obj->removed = true;
                    w.vm().freeObject(e->obj);
                }
        }
        w.vm().processFreedObjects();
        w.frame();
        r = analyzeSoftlocks(w);
        g = findGroup(r, "hardpoint1");
        CHECK(g != nullptr);
        if (g) {
            CHECK(g->softlock);
            CHECK(g->deadSpawners == int(g->spawners.size()));
            const auto lines = formatSoftlockReport(r, false);
            bool flagged = false;
            for (const auto& l : lines) flagged = flagged || l.rfind("SOFTLOCK hardpoint1", 0) == 0;
            CHECK(flagged);
        }
    }
    std::filesystem::remove_all(dir);
    if (failures) std::cerr << failures << " failure(s)\n";
    return failures ? 1 : 0;
}
