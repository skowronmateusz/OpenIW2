// Tests for the grenade bounce angular re-base (exe 0x525cb0).
#include "game/grenadeangles.h"

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
bool near(float a, float b, float e = 0.02f) { return std::fabs(a - b) <= e; }
} // namespace

int main() {
    // Slope pitch: flat ground lies at pitch 0; a 45 degree slope rising toward the heading points up (315).
    const float s = std::sqrt(0.5f);
    CHECK(near(grenadeSlopePitch({0, 0, 1}, 0), 0.0f));
    CHECK(near(grenadeSlopePitch({-s, 0, s}, 0), 315.0f, 0.1f));
    CHECK(near(grenadeSlopePitch({s, 0, s}, 0), 45.0f, 0.1f));
    // Wall (normal z <= 0.1): a flying grenade draws one rand and perturbs the pitch spin; a resting one draws nothing.
    {
        int calls = 0;
        const auto r = grenadeRebaseAngles({10, 20, 30}, 100.0f, {1, 0, 0}, false, [&] { ++calls; return 0x7f; });
        CHECK(calls == 1 && near(r.pitchSpin, 164.0f, 0.1f));
        CHECK(r.angles[0] == 10 && r.angles[1] == 20 && r.angles[2] == 30);
        calls = 0;
        const auto rr = grenadeRebaseAngles({10, 20, 30}, 100.0f, {1, 0, 0}, true, [&] { ++calls; return 0; });
        CHECK(calls == 0 && rr.pitchSpin == 100.0f);
    }
    // Floor, flying, pitch far from the slope (d >= 80): spin rescaled, pitch kept.
    {
        const auto r = grenadeRebaseAngles({100, 0, 0}, 600.0f, {0, 0, 1}, false, [] { return 0; });
        CHECK(near(r.pitchSpin, 600.0f * 0.85f, 0.01f));
        CHECK(near(r.angles[0], 100.0f, 0.01f));
    }
    // d in [45, 80): spin reversed, pitch moves by d/4.
    {
        const auto r = grenadeRebaseAngles({60, 0, 0}, 600.0f, {0, 0, 1}, false, [] { return 0; });
        CHECK(near(r.pitchSpin, -600.0f * 0.85f, 0.01f));
        CHECK(near(r.angles[0], 75.0f, 0.01f));
    }
    // d < 45: spin reversed, pitch snaps to the slope.
    {
        const auto r = grenadeRebaseAngles({30, 0, 0}, 600.0f, {0, 0, 1}, false, [] { return 32767; });
        CHECK(near(r.pitchSpin, -600.0f * (32767.0f / 32768.0f * 0.3f + 0.85f), 0.1f));
        CHECK(near(r.angles[0], 0.0f, 0.01f));
    }
    // Resting: the pitch lies on the slope, face up (|pitch| <= 90) or flipped by 180 degrees; yaw and roll are kept.
    CHECK(near(grenadeRebaseAngles({80, 5, 0}, 600.0f, {0, 0, 1}, true, [] { return 0; }).angles[0], 0.0f, 0.01f));
    CHECK(near(grenadeRebaseAngles({100, 5, 0}, 600.0f, {0, 0, 1}, true, [] { return 0; }).angles[0], 180.0f, 0.01f));
    CHECK(near(grenadeRebaseAngles({30, 5, 7}, 600.0f, {0, 0, 1}, true, [] { return 0; }).angles[2], 7.0f, 0.001f));
    if (failures) return 1;
    std::cout << "grenadeangles ok\n";
    return 0;
}
