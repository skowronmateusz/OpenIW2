// The actor drop to floor (0x4fc550) and the level-start spawner drop (0x5325f0), docs/spec/ai.md "Actor spawn drop".
// Needs the game files: with IW2_MAIN set to the game's main directory, cityhall's spawner at (4386 -4304 64) (the
// cover_right guard that stood 28.6 units above the terrain) must sit on the floor after prepareStart, a second drop must
// keep it there, and a drop from more than 128 units above the floor must stop at the end point (z - 128).

#include "fs/vfs.h"
#include "game/world.h"

#include <cmath>
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
        const auto o = e->obj->get("origin");
        if (o.isVector() && o.asVector()[0] == 4386.0f && o.asVector()[1] == -4304.0f) spawner = e.get();
    }
    CHECK(spawner != nullptr);
    if (spawner) {
        const auto o = spawner->obj->get("origin").asVector();
        std::cout << "spawner z " << o[2] << "\n";
        CHECK(o[2] > 35.0f && o[2] < 36.0f); // the map origin is z 64, the terrain under it about 35.4
        CHECK(!w.dropActorToFloor(*spawner));
        const auto o2 = spawner->obj->get("origin").asVector();
        CHECK(std::fabs(o2[2] - o[2]) < 0.05f); // raised by 1, then back down onto the same floor
        // From 300 above the floor nothing is hit within 128 units: the origin becomes the trace end, z - 128 (+1 - 129).
        spawner->obj->set("origin", iw2::script::Value(iw2::script::Vec3{o[0], o[1], o[2] + 300.0f}));
        CHECK(!w.dropActorToFloor(*spawner));
        const auto o3 = spawner->obj->get("origin").asVector();
        CHECK(std::fabs(o3[2] - (o[2] + 300.0f - 128.0f)) < 0.01f);
    }
    if (failures) std::cerr << failures << " failure(s)\n";
    else std::cout << "spawndrop ok\n";
    return failures ? 1 : 0;
}
