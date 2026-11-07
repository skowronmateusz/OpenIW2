// Bullets hit drones (docs/spec/collision.md "Fake AI"): makefakeai (0x4f82a0) clears ent+0xa2 & 6 and adds contents 0x8000,
// so the entity trace (0x44b840) tests the drone's 30x30x72 box instead of its xmodel collision surfaces. On decoytown the
// running drones (maps/_vehicledroneai.gsc) wait for "damage" and then play their death; before, the player's bullets
// passed through them. Needs the game data (IW2_MAIN=<game main dir>).
#include "fs/vfs.h"
#include "game/world.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

using namespace iw2::game;

namespace {
int failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                             \
        }                                                                           \
    } while (0)
} // namespace

int main() {
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "drone hit test skipped (IW2_MAIN not set)\n";
        return 0;
    }
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    World w(vfs, "decoytown");
    w.start();
    Entity* drone = nullptr;
    for (int i = 0; i < 20 * 120 && !drone; ++i) {
        w.frame();
        for (const auto& e : w.entities())
            if (!drone && !e->obj->removed && e->classname == "script_model" && e->obj->get("$fakeai").truthy() &&
                e->obj->get("targetname").toString() == "drone")
                drone = e.get();
    }
    CHECK(drone != nullptr);
    {
        std::cout << "shell shocks:";
        for (const auto& n : w.shellShockNames()) std::cout << " " << n;
        std::cout << "\n";
    }
    if (!drone) return 1;
    for (int i = 0; i < 10; ++i) w.frame();
    CHECK(!drone->obj->removed);
    const Vec3 o = drone->obj->get("origin").asVector();
    const int health0 = drone->obj->get("health").asInt();
    std::cout << "drone " << drone->number << " at " << o[0] << " " << o[1] << " " << o[2] << " health " << health0 << "\n";
    // A shot from 200 units off at the drone's chest (the box spans z 0..72 above the origin).
    const Vec3 eye{o[0] + 200.0f, o[1], o[2] + 40.0f};
    const int hits0 = w.stats.hits;
    w.playerFire(eye, {-1, 0, 0});
    const int health1 = drone->obj->get("health").asInt();
    std::cout << "after the shot: health " << health1 << " hits " << w.stats.hits - hits0 << "\n";
    CHECK(health1 < health0);
    return failures ? 1 : 0;
}
