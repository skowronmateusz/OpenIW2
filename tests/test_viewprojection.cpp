// Scene projection and weapon cycle gate.
#include "game/viewprojection.h"
#include "game/weaponcycle.h"
#include "ui/focuspulse.h"

#include <cmath>
#include <iostream>

int main() {
    using namespace iw2::game;
    int failures = 0;
    auto check = [&](bool c, int line) {
        if (!c) {
            std::cerr << "line " << line << " failed\n";
            ++failures;
        }
    };
#define CHECK(c) check((c), __LINE__)
    auto ndc = [](const std::array<float, 16>& m, float d) { return (m[10] * -d + m[14]) / (m[11] * -d); };
    const auto p = sceneProjection(1.0f, 1.25f, 4.0f);
    CHECK(std::fabs(p[0] - kProjectionScale) < 1e-6f);
    CHECK(std::fabs(p[5] - 1.25f * kProjectionScale) < 1e-6f);
    CHECK(std::fabs(ndc(p, 4.0f)) < 1e-3f * 4); // the near plane maps to about 0
    CHECK(ndc(p, 1e7f) < 1.0f);                 // no far plane: distant points stay inside the depth range
    const auto h = depthHackProjection(1.0f, 1.25f, 0.1f);
    CHECK(h[14] == -0.1f);
    CHECK(h[10] == p[10] && h[0] == p[0]);

    CHECK(weaponCycleAllowed(false, 0, 0, 100, 100, 0)); // the default delay of 0 never blocks
    CHECK(!weaponCycleAllowed(true, 0, 0, 100, 0, 0));
    CHECK(!weaponCycleAllowed(false, 0x8000, 0, 100, 0, 0));
    // A last cycle time from a previous map (its clock far ahead) blocks a new map's cycles; CG_Init (0x4a5c10) zeroes it on load.
    CHECK(!weaponCycleAllowed(false, 0, 0, 5000, 733150, 0));
    CHECK(weaponCycleAllowed(false, 0, 0, 5000, 0, 0));
    CHECK(!weaponCycleAllowed(false, 0, 0x20000, 100, 0, 0));
    CHECK(!weaponCycleAllowed(false, 0, 0, 100, 50, 51));
    CHECK(weaponCycleAllowed(false, 0, 0, 100, 50, 50));

    // sin(0) = 0 gives t = 0.5; a capturing bind leaves the focus colour for red, not a flat colour.
    const auto mid = iw2::ui::focusPulseColor({1, 1, 1, 1}, false, 0);
    CHECK(std::fabs(mid[0] - 0.9f) < 1e-5f && std::fabs(mid[3] - 0.9f) < 1e-5f);
    const auto cap = iw2::ui::focusPulseColor({1, 1, 1, 1}, true, 0);
    CHECK(std::fabs(cap[0] - 0.9f) < 1e-5f && std::fabs(cap[1] - 0.5f) < 1e-5f);
    return failures ? 1 : 0;
}
