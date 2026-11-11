// Multiplayer radius damage to a brush-model entity (docs/spec/mp-pmove.md "Radius damage"): the skip test measures a
// brush model to its bounds and the visibility / near-cover points are the middle of its bounds, not the raw origin
// field (0, 0, 0 for an origin-less script_brushmodel, as with the SP getorigin rule in docs/spec/campaign.md).
// Needs the game files (IW2_MAIN).

#include "game/world.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

int failures = 0;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                                \
        }                                                                              \
    } while (0)

} // namespace

int main() {
    const char* main = std::getenv("IW2_MAIN");
    if (!main) return 1;
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    iw2::game::World w(vfs, "mp_carentan", true);
    w.mpStart("dm");
    const int c0 = w.mpConnect("shooter");
    w.mpBegin(c0);
    w.frame();
    w.mpAutoJoin(c0);
    for (int i = 0; i < 5; ++i) w.frame();

    // A brush submodel whose box centre lies far from the map origin, so an explosion there is far from the entity's
    // (0, 0, 0) origin field: with the origin read the skip test would drop it.
    int pick = -1;
    iw2::game::Vec3 centre{0, 0, 0}, mn{}, mx{};
    double best = 0;
    for (int i = 1; i < 64 && w.submodelBounds("*" + std::to_string(i), mn, mx); ++i) {
        const iw2::game::Vec3 c{(mn[0] + mx[0]) * 0.5f, (mn[1] + mx[1]) * 0.5f, (mn[2] + mx[2]) * 0.5f};
        const double d = std::sqrt(double(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]));
        if (d > best) { best = d; pick = i; centre = c; }
    }
    CHECK(pick > 0 && best > 300.0);
    if (pick < 0) return 1;
    CHECK(w.submodelBounds("*" + std::to_string(pick), mn, mx));

    // A damageable script_brushmodel on that submodel: no "origin" key, so its origin field stays (0, 0, 0).
    iw2::game::Entity& e = w.debugSpawn("script_brushmodel");
    e.obj->set("model", iw2::script::Value("*" + std::to_string(pick)));
    e.obj->set("health", iw2::script::Value(int32_t(1000)));
    e.obj->set("$takedamage", iw2::script::Value(true));
    CHECK(e.obj->get("origin").asVector()[0] == 0 && e.obj->get("origin").asVector()[1] == 0 && e.obj->get("origin").asVector()[2] == 0);

    // An explosion at the box centre: the distance to the box is 0, so the entity takes the damage. Reading the
    // origin instead would put it `best` units away and skip it.
    w.debugRadiusDamage(centre, nullptr, 100, 50, 96, "MOD_GRENADE_SPLASH");
    w.frame();
    const int h1 = e.obj->get("health").asInt();
    CHECK(h1 < 1000);
    std::cout << "brush health 1000 -> " << h1 << " (explosion at the box centre " << centre[0] << " " << centre[1] << " " << centre[2]
              << ", origin field 0 0 0, submodel *" << pick << ")\n";

    // Beyond the box by more than the radius: no damage (the box edge, not the origin, bounds the test).
    const iw2::game::Vec3 out{mx[0] + 200.0f, centre[1], centre[2]};
    w.debugRadiusDamage(out, nullptr, 100, 50, 96, "MOD_GRENADE_SPLASH");
    w.frame();
    CHECK(e.obj->get("health").asInt() == h1);

    std::cout << (failures ? "FAILED\n" : "ok\n");
    return failures ? 1 : 0;
}
