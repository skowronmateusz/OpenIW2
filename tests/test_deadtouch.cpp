// Actor trigger touch (docs/spec/entities.md "Trigger touch", 0x4ff260; needs IW2_MAIN): only an actor whose alive flag (+0x25c) is
// set touches triggers. A dying actor (the die callback 0x4ff530 cleared the flag, the death animscript still runs) touches nothing,
// so a script waiting on the trigger never gets a dead `other`. Also the script form of spawn("trigger_radius", ...) (0x528ee0).
#include "fs/vfs.h"
#include "game/world.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>

using namespace iw2::game;

static int failures = 0;
#define CHECK(c)                                                                        \
    do {                                                                                \
        if (!(c)) { std::cout << "FAIL line " << __LINE__ << ": " #c "\n"; ++failures; } \
    } while (0)

int main() {
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "deadtouch test skipped (IW2_MAIN not set)\n";
        return 0;
    }
    const auto dir = std::filesystem::temp_directory_path() / ("iw2_test_deadtouch_" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir / "maps");
    // A trigger_radius for the allies (spawnflags 2) around the actor; the waiter records who touched it.
    std::ofstream(dir / "maps/deadtouch.gsc") << "main()\n{\n\tt = spawn(\"trigger_radius\", self.origin, 2, 8, 128);\n"
                                                 "\tlevel.touchedby = undefined;\n\tt waittill(\"trigger\", other);\n\tlevel.touchedby = other;\n}\n";
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    vfs.mountDirectory(dir.string());
    World w(vfs, "trainyard");
    w.start();
    w.spawnAllActors();
    for (int i = 0; i < 20; ++i) w.frame();
    std::vector<Entity*> allies;
    for (const auto& e : w.entities())
        if (e->isActor && !e->obj->removed && !e->ai.corpse && e->ai.alive && e->obj->get("team").toString() == "allies") allies.push_back(e.get());
    CHECK(allies.size() >= 2);
    if (allies.size() < 2) return 1;
    auto touched = [&](Entity& a, bool alive) {
        a.ai.alive = alive;
        w.vm().start("maps/deadtouch", "main", {}, iw2::script::Value(a.obj));
        for (int i = 0; i < 4; ++i) w.frame(); // the touch is recorded in one frame and notified in the next one's stage 3
        const iw2::script::Value v = w.vm().level()->get("touchedby");
        return v.isObject() && v.asObject() == a.obj;
    };
    CHECK(touched(*allies[0], true));   // control: a living actor inside the radius touches it
    CHECK(!touched(*allies[1], false)); // a dying actor does not
    // The script form of the trigger_radius spawn (0x528ee0): radius and height from the 4th / 5th argument, fewer is an error.
    std::ofstream(dir / "maps/deadtouch2.gsc") << "main()\n{\n\tt = spawn(\"trigger_radius\", (0, 0, 0), 0, 300, 40);\n\tlevel.r = t.radius;\n\tlevel.h = t.height;\n}\n"
                                                  "short()\n{\n\tt = spawn(\"trigger_radius\", (0, 0, 0), 0);\n}\n";
    w.vm().start("maps/deadtouch2", "main", {}, iw2::script::Value(w.vm().level()));
    CHECK(w.vm().level()->get("r").isNumber() && w.vm().level()->get("r").asFloat() == 300.0f);
    CHECK(w.vm().level()->get("h").isNumber() && w.vm().level()->get("h").asFloat() == 40.0f);
    const size_t errs = w.vm().errors().size();
    w.vm().start("maps/deadtouch2", "short", {}, iw2::script::Value(w.vm().level()));
    CHECK(w.vm().errors().size() == errs + 1 && w.vm().errors().back().find("USAGE: spawn( \"trigger_radius\"") != std::string::npos);
    std::filesystem::remove_all(dir);
    if (failures) return 1;
    std::cout << "deadtouch test passed\n";
    return 0;
}
