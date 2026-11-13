// The actor die callback clears the entity's targetname (docs/spec/ai.md "Damage", 0x4ff618..0x4ff630), so getentarray no longer
// finds a dead actor or its corpse. matmata's courtyard waves count their living members that way (maps/matmata.gsc:805) and the flak
// gun / Stuka sequence starts only after the last wave (maps/matmata.gsc:754-795). Runs the level's `courtyard` start, kills every
// wave by script damage and requires the waves to advance and planeSequence to start. Needs IW2_MAIN.
#include "fs/vfs.h"
#include "game/world.h"

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>

using namespace iw2::game;

static int failures = 0;
#define CHECK(c)                                                                         \
    do {                                                                                 \
        if (!(c)) { std::cout << "FAIL line " << __LINE__ << ": " #c "\n"; ++failures; } \
    } while (0)

int main() {
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "dead targetname test skipped (IW2_MAIN not set)\n";
        return 0;
    }
    const auto dir = std::filesystem::temp_directory_path() / ("iw2_test_deadtn_" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir / "maps");
    std::ofstream(dir / "maps/deadtn.gsc") << "main()\n{\n\tself dodamage(self.health + 1000, self.origin);\n}\n";
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    vfs.mountDirectory(dir.string());
    World w(vfs, "matmata");
    w.cvars["start"] = "courtyard"; // the level's own shortcut: the player stands at the courtyard
    w.start();
    for (int i = 0; i < 40; ++i) w.frame();

    auto named = [&](const std::string& tn) {
        int n = 0;
        for (const auto& e : w.entities())
            if (!e->obj->removed && e->obj->get("targetname").toString() == tn) ++n;
        return n;
    };
    // Put the player inside the courtyard trigger (its brush spans (4568 4660 -36)..(5196 4780 92)).
    w.player()->obj->set("origin", Value(Vec3{4882, 4720, -26}));

    std::set<std::string> seen;
    bool planes = false; // planeSequence started
    for (int s = 0; s < 120 && !planes; ++s) {
        for (int i = 0; i < 20; ++i) w.frame();
        for (const char* g : {"spawned_flak_guys", "spawned_northGroup", "spawned_eastGroup", "spawned_southGroup"})
            if (named(g)) seen.insert(g);
        if (s % 2 == 0) {
            std::vector<ObjectPtr> axis;
            for (const auto& e : w.entities())
                if (e->isActor && !e->isSpawner && !e->obj->removed && e->ai.alive && e->obj->get("team").toString() == "axis")
                    axis.push_back(e->obj);
            for (const auto& o : axis) {
                const std::string tn = o->get("targetname").toString();
                w.vm().start("maps/deadtn", "main", {}, Value(o));
                // The die callback ran: the targetname is gone at once.
                if (tn.rfind("spawned_", 0) == 0) CHECK(o->get("targetname").isDefined() == false);
            }
        }
        // planeSequence (maps/matmata.gsc:1233) sets level.planeStarted before the first bombing run.
        if (w.vm().level()->get("planestarted").truthy()) planes = true;
    }
    for (const auto& er : w.vm().errors()) std::cout << "ERR " << er << "\n";
    std::cout << "waves seen " << seen.size() << ", plane sequence " << planes << "\n";
    CHECK(seen.size() == 4);
    CHECK(planes);
    std::filesystem::remove_all(dir);
    if (failures) return 1;
    std::cout << "dead targetname test passed\n";
    return 0;
}
