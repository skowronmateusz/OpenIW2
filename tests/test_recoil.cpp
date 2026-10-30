// Tests for the weapon recoil springs.

#include "game/viewweapon.h"

#include <algorithm>
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
    {   // Screen shake (0x4bf5e0): none without events, bounded by scale, gone after the duration, radius-limited.
        ViewShake sh;
        const V3 eye{0, 0, 0};
        CHECK(sh.update(1000, eye)[0] == 0.0f);
        sh.add({0, 0, 0}, 0.4f, 1000, 100.0f, 1000, eye);
        float peak = 0;
        for (int t = 1001; t < 1900; t += 10) {
            const V3 v = sh.update(t, eye);
            peak = std::max({peak, std::fabs(v[0]), std::fabs(v[1]), std::fabs(v[2])});
        }
        CHECK(peak > 0.5f && peak <= 0.4f * 0.4f * 18.0f + 1e-3f);
        {   // per rendered frame: render times inside one 50 ms server step give different offsets
            ViewShake q;
            q.add({0, 0, 0}, 0.4f, 4000, 100.0f, 1000, eye);
            const V3 a = q.update(clientRenderTimeMs(1500, 0.25f), eye), b = q.update(clientRenderTimeMs(1500, 0.75f), eye);
            CHECK(clientRenderTimeMs(1500, 1.0f) == 1500 && clientRenderTimeMs(1500, 0.0f) == 1450);
            CHECK(a[0] != b[0] && a[1] != b[1]);
        }
        const V3 gone = sh.update(2100, eye);
        CHECK(gone[0] == 0.0f && gone[1] == 0.0f && gone[2] == 0.0f);
        sh.add({0, 0, 0}, 0.4f, 1000, 100.0f, 3000, {200, 0, 0}); // outside the radius: a < 0, never selected
        const V3 far = sh.update(3100, {200, 0, 0});
        CHECK(far[0] == 0.0f && far[1] == 0.0f && far[2] == 0.0f);
    }
    if (failures) return 1;
    std::cout << "recoil ok\n";
    return 0;
}
