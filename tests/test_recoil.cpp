// Tests for the weapon recoil springs.

#include "game/viewweapon.h"

#include <cmath>
#include <iostream>
#include <map>
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
using namespace iw2::game;
} // namespace

int main() {
    // Values of the shipped enfield weapon file.
    const std::map<std::string, float> w = {
        {"aimDownSight", 1}, {"gunMaxPitch", 20}, {"gunMaxYaw", 20},
        {"hipViewKickPitchMin", 60}, {"hipViewKickPitchMax", 75}, {"hipViewKickYawMin", -20}, {"hipViewKickYawMax", -35},
        {"hipViewKickCenterSpeed", 1400}, {"adsViewKickCenterSpeed", 1200},
        {"hipGunKickPitchMin", -30}, {"hipGunKickPitchMax", -35}, {"hipGunKickYawMin", -2}, {"hipGunKickYawMax", 18},
        {"hipGunKickAccel", 1000}, {"hipGunKickSpeedMax", 1200}, {"hipGunKickSpeedDecay", 100}, {"hipGunKickStaticDecay", 100},
        {"adsGunKickAccel", 400}, {"adsGunKickSpeedMax", 300}, {"adsGunKickSpeedDecay", 100}, {"adsGunKickStaticDecay", 100}};
    RecoilKick::Field f = [&](const char* k) { auto it = w.find(k); return it == w.end() ? 0.0f : it->second; };

    RecoilKick r;
    CHECK(r.viewKick()[0] == 0 && r.gunKick()[0] == 0);
    r.fire(f, 0.0f, false);
    float maxUp = 0;
    for (int i = 0; i < 40; ++i) { // 40 frames of 16 ms
        r.update(f, 16, true, 0.0f);
        maxUp = std::max(maxUp, -r.viewKick()[0]);
        CHECK(std::fabs(r.viewKick()[0]) <= 10.0f); // clamp at 10 degrees
    }
    CHECK(maxUp > 0.5f); // pitch velocity -60..-75 raises the view (negative pitch = up)
    for (int i = 0; i < 200; ++i) r.update(f, 16, true, 0.0f);
    CHECK(std::fabs(r.viewKick()[0]) < 1e-3f && std::fabs(r.viewKick()[1]) < 1e-3f); // springs settle at zero
    CHECK(std::fabs(r.gunKick()[0]) < 0.25f);
    if (failures) return 1;
    std::cout << "recoil ok\n";
    return 0;
}
