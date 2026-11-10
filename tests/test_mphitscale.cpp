// Multiplayer location scaling of a client's damage (docs/spec/mp-pmove.md "Location scaling"): the inflictor's weapon table,
// no helmet-piercing rule, the MP default table for weapons that are not bullets. Needs the game files (IW2_MAIN).

#include "game/world.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

int failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                             \
        }                                                                           \
    } while (0)

float num(iw2::game::World& w, const std::string& weapon, const char* key) {
    const auto* d = w.weapon(weapon);
    if (!d) return -1;
    const auto it = d->find(key);
    return it == d->end() ? 1.0f : float(std::atof(it->second.c_str()));
}

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
    CHECK(w.player() != nullptr);
    // A bolt-action rifle's helmet value stays the helmet value (no piercing in multiplayer).
    CHECK(std::fabs(w.debugLocMultiplier("kar98k_mp", "helmet") - num(w, "kar98k_mp", "lochelmet")) < 1e-5f);
    CHECK(std::fabs(w.debugLocMultiplier("thompson_mp", "torso_lower") - num(w, "thompson_mp", "loctorsolower")) < 1e-5f);
    // No weapon: the MP default table (torso_lower 0.8 there, 1.0 is the initial value of the table).
    CHECK(std::fabs(w.debugHitLocationMultiplier("torso_lower") - 0.8f) < 1e-5f);
    // The teleport bit follows the spawn count.
    CHECK(iw2::game::mpTeleportBit(1) == 2 && iw2::game::mpTeleportBit(2) == 0);
    CHECK(iw2::game::mpTeleportBit(w.mpState().clients[size_t(c0)].spawnCount) == (w.mpState().clients[size_t(c0)].spawnCount % 2) * 2);
    // Radius damage (docs/spec/mp-pmove.md): an explosion in the open beside the player sees all five points.
    iw2::game::Entity* p = w.player();
    if (p) {
        const auto o = p->obj->get("origin").asVector();
        CHECK(w.debugMpRadiusVisibility(*p, {o[0] + 30, o[1], o[2] + 40}) > 0.3f);
        // Far below the map, solid ground stands between: nothing is seen.
        CHECK(w.debugMpRadiusVisibility(*p, {o[0], o[1], o[2] - 4000}) == 0.0f);
        // finishPlayerDamage takes health, keeps the hit for the host's movement.
        const int h = p->obj->get("health").asInt();
        (void)w.takePlayerKnockbacks();
        w.debugDamageAt(*p, p, 10, {1, 0, 0}, "torso_upper");
        for (int i = 0; i < 2; ++i) w.frame();
        CHECK(p->obj->get("health").asInt() < h);
        const auto knocks = w.takePlayerKnockbacks();
        CHECK(knocks.size() == 1 && knocks[0].mp);
    }
    std::cout << (failures ? "FAILED\n" : "ok\n");
    return failures ? 1 : 0;
}
