// Tests for the camera impulses: landing dip and damage kick (CG_OffsetFirstPersonView 0x4ae290).

#include "game/viewweapon.h"

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
    CHECK(ViewImpulses::landDip(12.0f) == 0 && ViewImpulses::landDip(5) == 0);
    CHECK(ViewImpulses::landDip(38.0f) == 8);   // ((26/26) + 1) * 4
    CHECK(ViewImpulses::landDip(500.0f) == 24); // capped
    CHECK(ViewImpulses::dipFromDamage(100) == 24 && ViewImpulses::dipFromDamage(1) == 24);

    ViewImpulses v;
    v.reset();
    CHECK(v.landOffset(1000) == 0); // no landing
    v.land(38.0f, 0, 1000);
    CHECK(near(v.landOffset(1000), 0.0f));
    CHECK(near(v.landOffset(1075), -4.0f));   // half way in
    CHECK(near(v.landOffset(1150), -8.0f));   // deepest
    CHECK(near(v.landOffset(1300), -4.0f));   // half way out
    CHECK(near(v.landOffset(1450), 0.0f) && v.landOffset(2000) == 0);

    // World damage: pitch back by max(count * 0.6, 5).
    ViewImpulses d;
    const V3 f{1, 0, 0}, l{0, 1, 0};
    CHECK(d.damageKick(100, 0, false)[0] == 0);
    d.damage(255, 255, 50, f, l, 100);
    CHECK(near(d.damageKick(200, 0, false)[0], -30.0f) && near(d.damageKick(200, 0, false)[1], 0.0f)); // 100 ms: full
    CHECK(near(d.damageKick(100, 0, false)[0], 0.0f));
    CHECK(near(d.damageKick(400, 0, false)[0], -7.5f)); // 1 - x(2 - x), x = (300 - 100) / 400
    CHECK(d.damageKick(700, 0, false)[0] == 0);
    CHECK(near(d.damageKick(200, 1.0f, false)[0], -15.0f)); // aiming halves it
    // A hit from the front-left of a view looking along +x: byte yaw 0 = the shot travels along +x, so the kick direction
    // v = -(1, 0, 0) is behind the view: pitch kick = -(f . v) * kick = +kick.
    ViewImpulses e;
    e.damage(0, 0, 10, f, l, 0 + 1);
    CHECK(near(e.damageKick(101, 0, false)[0], 6.0f));
    // Shell-shock envelope of the gun sway (0x4b4e01).
    CHECK(shellShockRatio(0, 1000) == 0 && shellShockRatio(-5, 1000) == 0);
    CHECK(shellShockRatio(2000, 1000) == 1.0f && near(shellShockRatio(500, 1000), 0.5f));
    CHECK(near(shellShockRatio(250, 1000), 0.15625f)); // r*r*(3 - 2r) at r = .25
    if (failures == 0) std::cout << "viewimpulses tests passed\n";
    return failures ? 1 : 0;
}
