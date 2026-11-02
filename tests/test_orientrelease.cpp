// The animscript switch releases the orientmode override (exe 0x53ac90 -> 0x5063f0 "face default"): an actor that left a cover script with
// "face enemy" / "face angle" set must not keep facing that way while the move script walks it along a path (it would strafe or
// back-pedal). The same script keeps its override. Needs the game data (IW2_MAIN), otherwise skipped.
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
}  // namespace

int main() {
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "orientrelease: IW2_MAIN not set, skipped\n";
        return 0;
    }
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    iw2::game::World world(vfs, "crossroads");
    world.start();
    world.spawnAllActors();
    for (int i = 0; i < 20; ++i) world.frame();

    iw2::game::Entity* a = nullptr;
    for (const auto& ep : world.entities())
        if (ep->isActor && !ep->obj->removed && ep->obj->get("health").asInt() > 0) {
            a = ep.get();
            break;
        }
    CHECK(a);
    if (!a) return 1;

    world.debugSetActorAnimScript(*a, "stop", 0, 1);
    const std::string script = a->animScript;
    // An override (orientmode "face enemy", mode 3, with a locked target) set by the old script.
    a->ai.orient = 3;
    a->ai.overrideYaw = 123.0f;
    a->ai.orientTarget = 123.0f;
    a->ai.facing = 2;
    // The same script again: the override stays.
    world.debugSetActorAnimScript(*a, "stop", 0, 1);
    CHECK(a->animScript == script);
    CHECK(a->ai.orient == 3);
    // Another script: the override is released and the base block took it over.
    world.debugSetActorAnimScript(*a, "move", 3, 1);
    CHECK(a->ai.orient == 0);
    CHECK(a->ai.facing == 3);
    CHECK(a->ai.baseYaw == 123.0f && a->ai.baseTarget == 123.0f);
    // Without an override the switch leaves the base block alone.
    a->ai.facing = 2;
    world.debugSetActorAnimScript(*a, "stop", 0, 1);
    CHECK(a->ai.orient == 0 && a->ai.facing == 2 && a->ai.baseTarget == 123.0f);
    std::cout << "orientrelease: " << (failures == 0 ? "ok" : "FAILED") << "\n";
    return failures == 0 ? 0 : 1;
}
