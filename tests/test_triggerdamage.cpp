// trigger_damage rules (0x5293c0, 0x529350) and the means-of-death numbering (0x5a95ac).
#include "game/triggerdamage.h"

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
    CHECK(modNumber("MOD_UNKNOWN") == 0 && modNumber("MOD_PISTOL_BULLET") == 1 && modNumber("MOD_RIFLE_BULLET") == 2);
    CHECK(modNumber("MOD_MELEE") == 7 && modNumber("MOD_TRIGGER_HURT") == 13 && modNumber("MOD_EXPLOSIVE") == 14);
    const int full = kTriggerDamageFullHealth;
    // No restrictions: any hit fires.
    CHECK(triggerDamageFires(0, 0, 0, 1, 2, full));
    // Threshold: a hit below it does nothing.
    CHECK(!triggerDamageFires(50, 0, 0, 49, 2, full) && triggerDamageFires(50, 0, 0, 50, 2, full));
    // Spawnflags exclude damage kinds.
    CHECK(!triggerDamageFires(0, 0, 0x1, 10, 1, full) && triggerDamageFires(0, 0, 0x1, 10, 2, full));
    CHECK(!triggerDamageFires(0, 0, 0x2, 10, 2, full));
    CHECK(!triggerDamageFires(0, 0, 0x4, 10, 3, full) && !triggerDamageFires(0, 0, 0x4, 10, 6, full) && triggerDamageFires(0, 0, 0x4, 10, 7, full));
    CHECK(!triggerDamageFires(0, 0, 0x8, 10, 14, full));
    CHECK(!triggerDamageFires(0, 0, 0x10, 10, 4, full) && triggerDamageFires(0, 0, 0x10, 10, 3, full));
    CHECK(!triggerDamageFires(0, 0, 0x20, 10, 7, full));
    CHECK(!triggerDamageFires(0, 0, 0x100, 10, 0, full) && !triggerDamageFires(0, 0, 0x100, 10, 13, full) &&
          triggerDamageFires(0, 0, 0x100, 10, 8, full) && triggerDamageFires(0, 0, 0x100, 10, 2, full));
    // Accumulate: the damage since the last firing (32000 - health) must reach it.
    CHECK(!triggerDamageFires(0, 100, 0, 40, 2, full - 40) && triggerDamageFires(0, 100, 0, 40, 2, full - 100));
    if (failures) return 1;
    // trigger_disk: box 30 wide (half 15) around x=100, disk at the origin with radius 50: touches from distance 65.
    CHECK(!triggerDiskContact(85, 115, -15, 15, 0, 0, 100));
    CHECK(triggerDiskContact(85, 115, -15, 15, 0, 0, 50));
    CHECK(!triggerDiskContact(85, 115, -15, 15, 0, 0, 90));
    std::cout << "triggerdamage tests passed\n";
    return 0;
}
