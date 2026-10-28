// The player's projectile weapon (panzerschreck) fires a rocket (FireWeapon 0x531c40, weaponType 2 -> 0x531a40 ->
// G_FireRocket 0x527810) instead of a bullet trace, and the missile's projTrailEffect is queued bolted on its tag_origin
// (CG_Missile 0x4ab3af). Needs the game files (IW2_MAIN).

#include "fs/vfs.h"
#include "game/world.h"

#include <array>
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
    iw2::game::World w(vfs, "moscow");
    w.prepareStart();
    CHECK(w.player() != nullptr);
    if (!w.player()) return 1;
    (void)w.takeFxEvents();
    w.player()->obj->set("$weapon", iw2::script::Value(std::string("panzerschreck")));
    const iw2::script::Vec3 o = w.player()->obj->get("origin").asVector();
    const int impactsBefore = w.stats.bulletImpacts;
    w.playerFire({o[0], o[1], o[2] + 60}, {1, 0, 0});
    CHECK(w.stats.rockets == 1);
    CHECK(w.stats.bulletImpacts == impactsBefore); // no bullet trace
    bool trail = false;
    for (const auto& e : w.takeFxEvents())
        if (e.path == "fx/smoke/emitter_panzerschreck.efx" && e.tag == "tag_origin" && e.entity >= 0) {
            trail = true;
            iw2::game::Vec3 p{};
            std::array<iw2::game::Vec3, 3> ax{};
            bool hasAxes = false;
            CHECK(w.entityTagOrientation(e.entity, "tag_origin", p, ax, hasAxes)); // the bolt resolves on the missile model
        }
    CHECK(trail);
    // A bullet weapon still traces.
    w.player()->obj->set("$weapon", iw2::script::Value(std::string("kar98k")));
    w.playerFire({o[0], o[1], o[2] + 60}, {1, 0, 0});
    CHECK(w.stats.rockets == 1);
    if (failures) std::cerr << failures << " failure(s)\n";
    else std::cout << "playerrocket ok\n";
    return failures ? 1 : 0;
}
