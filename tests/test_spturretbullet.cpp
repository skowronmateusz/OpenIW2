// Single-player turret fire uses the turret's own weapon (docs/spec/turrets.md "Turret shot"): the shipped SP turret weapons are
// rifleBullet weapons, so a turret bullet goes on through the first actor it hits and the second one takes damage too. Needs
// the game files (IW2_MAIN).
#include "fs/vfs.h"
#include "game/world.h"
#include "script/vm.h"

#include <cstdlib>
#include <iostream>
#include <vector>

using namespace iw2::game;

static int failures = 0;
#define CHECK(c)                                                                     \
    do {                                                                             \
        if (!(c)) {                                                                  \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #c "\n"; \
            ++failures;                                                              \
        }                                                                            \
    } while (0)

int main() {
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "SP turret bullet test skipped (IW2_MAIN not set)\n";
        return 0;
    }
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    World w(vfs, "decoytrenches");
    w.start();
    for (int i = 0; i < 5; ++i) w.frame();
    std::vector<Entity*> actors;
    Entity* turret = nullptr;
    for (const auto& e : w.entities()) {
        if (e->isActor && !e->isSpawner && !e->obj->removed) actors.push_back(e.get());
        if (e->turret && !e->obj->removed && !turret) turret = e.get();
    }
    CHECK(turret != nullptr && actors.size() >= 2);
    if (!turret || actors.size() < 2) return 1;
    const auto* def = w.weapon(turret->turret->weapon);
    CHECK(def && def->count("riflebullet") && std::atoi(def->at("riflebullet").c_str()) != 0);

    // Two actors in line, shot from the turret entity: the bullet goes through the first.
    Entity &a = *actors[0], &b = *actors[1];
    const Vec3 ao = a.obj->get("origin").asVector();
    b.obj->set("origin", Value(Vec3{ao[0] + 40.0f, ao[1], ao[2]}));
    w.frame();
    int through = 0, firstHits = 0;
    for (float dz = 10.0f; dz <= 60.0f; dz += 5.0f)
        for (float dy = -8.0f; dy <= 8.0f; dy += 2.0f) {
            a.obj->set("health", Value(int32_t(100000)));
            b.obj->set("health", Value(int32_t(100000)));
            const Vec3 ac = a.obj->get("origin").asVector(), bc = b.obj->get("origin").asVector();
            const Vec3 along{(bc[0] - ac[0]) / 40.0f, (bc[1] - ac[1]) / 40.0f, 0.0f};
            const Vec3 from{ac[0] - along[0] * 60.0f - along[1] * dy, ac[1] - along[1] * 60.0f + along[0] * dy, ac[2] + dz};
            const Vec3 to{from[0] + along[0] * 400.0f, from[1] + along[1] * 400.0f, from[2]};
            if (w.debugFireBulletFrom(*turret, from, to) != &a) continue;
            ++firstHits;
            through += b.obj->get("health").asInt() < 100000;
        }
    std::cout << "turret shots hitting the first actor " << firstHits << ", through to the second " << through << "\n";
    CHECK(firstHits > 0 && through > 0);
    if (failures == 0) std::cout << "SP turret bullet ok\n";
    return failures ? 1 : 0;
}
