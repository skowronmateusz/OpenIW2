// G_Damage's push on the player and the explosion events without marks (docs/spec/weapons.md, "Damage knockback").

#include "game/pmove.h"

#include <cmath>
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
using namespace iw2::game;
bool near(float a, float b) { return std::fabs(a - b) < 1e-3f; }
} // namespace

int main() {
    { // standing: trunc(100 * 0.05) = 5, 5 * 1000 * 0.004 = 20 along the unit direction
        PlayerState ps;
        applyKnockback(ps, {100, {3, 0, 4}, false});
        CHECK(near(ps.velocity[0], 12) && near(ps.velocity[2], 16));
        CHECK(ps.pmTimeMs == 50 && ps.spawnSlide); // 2 * 5 raised to 50, pm_flags 0x400
    }
    { // crouched 0.025, prone 0.01
        PlayerState ps;
        ps.stance = Stance::Crouch;
        applyKnockback(ps, {100, {1, 0, 0}, false});
        CHECK(near(ps.velocity[0], 8));
        ps = {};
        ps.stance = Stance::Prone;
        applyKnockback(ps, {100, {1, 0, 0}, false});
        CHECK(near(ps.velocity[0], 4));
        ps = {};
        ps.stance = Stance::Prone;
        applyKnockback(ps, {99, {1, 0, 0}, false}); // truncated to 0: nothing happens
        CHECK(ps.velocity[0] == 0 && ps.pmTimeMs == 0 && !ps.spawnSlide);
    }
    { // the cap of 60 and the pm_time cap of 200
        PlayerState ps;
        applyKnockback(ps, {100000, {0, 1, 0}, false});
        CHECK(near(ps.velocity[1], 240) && ps.pmTimeMs == 120);
        ps = {};
        ps.stance = Stance::Stand;
        applyKnockback(ps, {1300, {0, 1, 0}, false}); // 65 capped to 60
        CHECK(ps.pmTimeMs == 120);
    }
    { // a running pm_time stays and sets no flag
        PlayerState ps;
        ps.pmTimeMs = 700;
        applyKnockback(ps, {100, {1, 0, 0}, false});
        CHECK(ps.pmTimeMs == 700 && !ps.spawnSlide && near(ps.velocity[0], 20));
    }
    { // own grenade or rocket: the whole z velocity is quartered, including what the player had
        PlayerState ps;
        ps.velocity = {0, 0, 80};
        applyKnockback(ps, {100, {0, 0, 1}, true});
        CHECK(near(ps.velocity[2], 25));
        CHECK(selfBlastMod(3) && selfBlastMod(6) && !selfBlastMod(2) && !selfBlastMod(7));
    }
    { // no direction: no push
        PlayerState ps;
        applyKnockback(ps, {100, {0, 0, 0}, false});
        CHECK(ps.pmTimeMs == 0);
    }
    // The dead view: towards the killer, 0..360 truncated; a self kill keeps the entity yaw (LookAtKiller).
    CHECK(lookAtKillerYaw({0, 0, 0}, {0, -10, 0}, false, 5) == 270.0f);
    CHECK(lookAtKillerYaw({10, 10, 0}, {0, 0, 0}, false, 5) == 225.0f); // the world entity sits at the origin
    CHECK(lookAtKillerYaw({0, 0, 0}, {0, 0, 0}, true, 37.9f) == 37.0f);
    if (failures) return 1;
    std::cout << "ok\n";
    return 0;
}
