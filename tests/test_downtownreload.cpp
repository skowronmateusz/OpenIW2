// Actors touching triggers (docs/spec/entities.md "Trigger touch"), on the ammo depot objective of downtown_assault: the depot goes
// up when an allied actor enters a trigger that takes allied actors only. The test reloads the autosave of that objective and
// proves that the level moves on from it: the wrong team and the player do not fire the trigger, the allied attackers do, and
// the objective after it becomes current.
#include "fs/vfs.h"
#include "game/savestate.h"
#include "game/world.h"

#include <cstdlib>
#include <iostream>

using namespace iw2::game;
using namespace iw2::script;

namespace {
int failures = 0;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                                \
        }                                                                              \
    } while (0)

Entity* byName(World& w, const std::string& name) {
    for (const auto& e : w.entities())
        if (!e->obj->removed && e->obj->get("targetname").isString() && e->obj->get("targetname").asString() == name) return e.get();
    return nullptr;
}

bool levelFlag(World& w, const std::string& name) {
    const Value flags = w.vm().level()->get("flag");
    if (!flags.isArray()) return false;
    ArrayKey k;
    k.isString = true;
    k.s = name;
    const Value* v = std::get<ArrayPtr>(flags.v)->get(k);
    return v && v->truthy();
}

bool isAxis(World& w, const Entity& e) {
    return e.isActor && !e.obj->removed && w.isAlive(e) && e.obj->get("team").isString() && e.obj->get("team").asString() == "axis";
}

// One frame with the player at `at`, unhurt.
void step(World& w, const Vec3& at) {
    w.setPlayerOrigin(at, {0, 0, 0});
    w.player()->obj->set("$godmode", Value(1));
    w.frame();
    w.takeSaveFiles();
}
} // namespace

int main() {
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "skipped (IW2_MAIN not set)\n";
        return 0;
    }
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);

    // The level's own "mid" start, up to the autosave of the depot objective.
    World a(vfs, "downtown_assault");
    a.cvars["start"] = "mid";
    a.start();
    for (int i = 0; i < 60; ++i) a.frame();
    a.takeSaveFiles();
    Entity* defend = byName(a, "defend_friendlies");
    CHECK(defend);
    if (!defend) return 1;
    Vec3 enter{};
    CHECK(a.debugTriggerEntryPoint(*defend, enter));
    std::vector<uint8_t> save;
    for (int i = 0; i < 2000 && save.empty(); ++i) {
        a.setPlayerOrigin(enter, {0, 0, 0});
        a.player()->obj->set("$godmode", Value(1));
        a.frame();
        for (auto& f : a.takeSaveFiles())
            if (f.name.find("downtown_assault-3") != std::string::npos) save = f.bytes;
    }
    CHECK(!save.empty());
    if (save.empty()) return 1;

    LoadedSave ls;
    CHECK(parseSaveFile(save, ls).empty());
    World b(vfs, ls.info.mapName);
    b.cvars["start"] = "mid";
    b.prepareStart();
    restoreSave(b, ls);
    Entity* window = byName(b, "player_moveup_detection");
    Entity* depot = byName(b, "destroy_depot");
    CHECK(window && depot);
    if (!window || !depot) return 1;
    Vec3 atWindow{}, inDepot{};
    CHECK(b.debugTriggerEntryPoint(*window, atWindow));
    CHECK(b.debugTriggerEntryPoint(*depot, inDepot));

    // The player (spawnflags 8) and an axis actor (spawnflags 1 clear) stand in the trigger: nothing happens.
    Entity* axis = nullptr;
    for (int i = 0; i < 200 && !axis; ++i) {
        step(b, atWindow);
        for (const auto& e : b.entities())
            if (isAxis(b, *e)) axis = e.get();
    }
    CHECK(axis);
    for (int i = 0; i < 60 && axis; ++i) {
        if (isAxis(b, *axis)) axis->obj->set("origin", Value(inDepot));
        step(b, inDepot);
    }
    CHECK(!levelFlag(b, "infiltrated_depot"));

    // The player holds the window and clears the axis on the way; the depot defenders stay. The allied attackers do the rest.
    for (int i = 0; i < 5000 && b.objectives()[7].state == b.objectives()[0].state; ++i) {
        step(b, atWindow);
        if (i % 40) continue;
        for (const auto& e : b.entities())
            if (isAxis(b, *e) && e->obj->get("origin").asVector()[0] < 1500.0f) b.debugDamage(*e, 100000);
    }
    CHECK(levelFlag(b, "infiltrated_depot"));
    CHECK(levelFlag(b, "defend_complete"));
    CHECK(b.objectives()[6].state != a.objectives()[6].state); // done, it was current in the save
    CHECK(b.objectives()[7].state == a.objectives()[6].state); // the next objective is current
    if (failures) std::cerr << failures << " failure(s)\n";
    return failures ? 1 : 0;
}
