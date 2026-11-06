// G_Damage decisions (docs/spec/weapons.md "G_Damage order").

#include "game/damagerules.h"

#include <iostream>

namespace {
int failures = 0;
#define CHECK(cond)                                                   \
    do {                                                              \
        if (!(cond)) {                                                \
            std::cerr << "FAIL line " << __LINE__ << ": " #cond "\n"; \
            ++failures;                                               \
        }                                                             \
    } while (0)
} // namespace

int main() {
    using namespace iw2::game;
    CHECK(riderIgnoresDamage(0x20000, true, 1));   // tank
    CHECK(!riderIgnoresDamage(0x20000, true, 4));  // artillery
    CHECK(!riderIgnoresDamage(0xa0000, true, 1));  // leaving: 0x80000
    CHECK(!riderIgnoresDamage(0, true, 1));
    CHECK(!riderIgnoresDamage(0x20000, false, 1));
    CHECK(damageFloor(0) == 1);
    CHECK(damageFloor(-5) == 1);
    CHECK(damageFloor(7) == 7);
    CHECK(actorDamageReduction(150, 100, false, true) == 51); // leaves health 1
    CHECK(actorDamageReduction(100, 100, false, true) == 1);
    CHECK(actorDamageReduction(99, 100, false, true) == 0);
    CHECK(actorDamageReduction(150, 100, true, true) == 0);
    CHECK(actorDamageReduction(150, 100, false, false) == 0);
    CHECK(actorDamageReduction(5, 1, false, true) == 5);      // nothing is taken at 1 health
    return failures ? 1 : 0;
}
