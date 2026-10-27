// Idle view animation is deferred until the running action animation has finished (cgame 0x4b299d).

#include "game/playerweapon.h"

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
using iw2::game::PlayerWeapon;
} // namespace

int main() {
    const PlayerWeapon::Def d{{"firetime", "0.1"}, {"clipsize", "5"}, {"startammo", "10"}, {"raisetime", "0.1"},
                              {"idleanim", "idle"}, {"fireanim", "fire"}};
    PlayerWeapon w;
    w.animLength = [](const std::string& n) { return n == "fire" ? 1.0f : 0.0f; };
    w.equip("g", d);
    PlayerWeapon::Input in;
    in.weapon = "g";
    for (int i = 0; i < 20; ++i) w.update(0.01f, in);
    CHECK(w.anim() == "idle");
    in.fire = true;
    w.update(0.01f, in);
    in.fire = false;
    CHECK(w.anim() == "fire");
    for (int i = 0; i < 30; ++i) w.update(0.01f, in); // state timer (0.1 s) is over, anim is not
    CHECK(w.stateNumber() == 0 && w.anim() == "fire" && w.animTime() > 0.25f);
    for (int i = 0; i < 40; ++i) w.update(0.01f, in);
    CHECK(w.anim() == "fire");
    for (int i = 0; i < 40; ++i) w.update(0.01f, in);
    CHECK(w.anim() == "idle" && w.animTime() < 0.3f);
    if (failures == 0) std::cout << "idledefer ok\n";
    return failures ? 1 : 0;
}
