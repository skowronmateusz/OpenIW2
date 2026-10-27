// Tests for the weapon item pickup helpers (BG_PlayerTouchesItem 0x4facc0, BG_CanItemBeGrabbed 0x4fad50, Pickup_Weapon 0x521170).

#include "game/itempickup.h"

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
    using namespace iw2::game;
    CHECK(playerTouchesItem({0, 0, 0}, {0, 0, 0}));
    CHECK(playerTouchesItem({36, -36, 0}, {0, 0, 0}));
    CHECK(!playerTouchesItem({37, 0, 0}, {0, 0, 0}));
    CHECK(playerTouchesItem({0, 0, -52}, {0, 0, 0}));  // the broad phase (+-52) is tighter than the narrow -88
    CHECK(!playerTouchesItem({0, 0, -53}, {0, 0, 0}));
    CHECK(playerTouchesItem({0, 0, 18}, {0, 0, 0}));
    CHECK(!playerTouchesItem({0, 0, 19}, {0, 0, 0}));

    CHECK(canGrabWeaponItem(true, false, true, 4));
    CHECK(!canGrabWeaponItem(true, false, true, 0));
    CHECK(!canGrabWeaponItem(false, false, true, 30)); // a rifle is taken by use only
    CHECK(canGrabWeaponItem(false, false, false, 30));
    CHECK(canGrabWeaponItem(false, true, true, 5));

    uint32_t seed = 1;
    ItemAmmo a = resolveItemAmmo({}, 0, 0, 4, 4, seed);
    CHECK(a.clip >= 2 && a.clip <= 4 && a.stock == 0);
    seed = 1;
    a = resolveItemAmmo({0, 0}, 7, 7, 8, 32, seed);
    CHECK(a.clip == 7 && a.stock == 0);
    a = resolveItemAmmo({20, 0}, 0, 0, 8, 32, seed);
    CHECK(a.clip == 8 && a.stock == 12);
    a = resolveItemAmmo({50, 0}, 0, 0, 8, 32, seed);
    CHECK(a.clip == 8 && a.stock == 24);

    ItemAmmo r = itemAmmoAfterPartial({10, 8}, 12);
    CHECK(r.stock == 0 && r.clip == 6);
    r = itemAmmoAfterPartial({10, 8}, 4);
    CHECK(r.stock == 6 && r.clip == 8);

    if (failures) return 1;
    std::cout << "ok\n";
    return 0;
}
