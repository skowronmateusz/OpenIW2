// HUD grenade indicator geometry (exe 0x4be120, 0x4be210, 0x4bde50, 0x4bdfc0, 0x4b0490).
#include "game/grenadeindicator.h"

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
bool near(float a, float b, float e = 1e-3f) { return std::fabs(a - b) <= e; }
} // namespace

int main() {
    using namespace iw2::game;
    const GrenadeIndicatorCfg c;
    const std::array<float, 3> o{0, 0, 0}, vel{100, 0, 0};
    // Visibility: moving, in range.
    CHECK(grenadeIndicatorVisible(c, {100, 0, 0}, vel, o, 60));
    CHECK(grenadeIndicatorVisible(c, {250, 0, 0}, vel, o, 60));   // exactly MaxRange
    CHECK(!grenadeIndicatorVisible(c, {251, 0, 0}, vel, o, 60));
    CHECK(!grenadeIndicatorVisible(c, {100, 0, 0}, {0, 0, 0}, o, 60)); // at rest (trDelta 0)
    CHECK(!grenadeIndicatorVisible(c, {100, 0, 0}, {1, 0, 0}, o, 60)); // |delta|^2 must exceed 1
    CHECK(grenadeIndicatorVisible(c, {100, 0, 0}, {1.1f, 0, 0}, o, 60));
    // Height: above up to viewHeight + MaxHeight, below down to MaxHeight.
    CHECK(grenadeIndicatorVisible(c, {10, 0, 164}, vel, o, 60));
    CHECK(!grenadeIndicatorVisible(c, {10, 0, 165}, vel, o, 60));
    CHECK(grenadeIndicatorVisible(c, {10, 0, -104}, vel, o, 60));
    CHECK(!grenadeIndicatorVisible(c, {10, 0, -105}, vel, o, 60));
    // Alpha: min + (max-min)/2 (1 + sin), clamped to 0..1.
    CHECK(near(grenadeIndicatorAlpha(c, 0), 1.0f)); // 0.3 + 0.775 = 1.075 -> 1
    {
        GrenadeIndicatorCfg d;
        d.pulseMin = 0.2f;
        d.pulseMax = 0.6f;
        d.pulseFreq = 1.0f;
        CHECK(near(grenadeIndicatorAlpha(d, 250), 0.6f)); // sin = 1
        CHECK(near(grenadeIndicatorAlpha(d, 750), 0.2f)); // sin = -1
        d.pulseMin = -3.0f;
        d.pulseMax = -2.0f;
        CHECK(grenadeIndicatorAlpha(d, 0) == 0.0f);
    }
    // Angle: grenade straight ahead = 0, to the left (+y, view yaw 0) = 90.
    CHECK(near(grenadeIndicatorAngle(10, 0, 0), 0.0f));
    CHECK(near(grenadeIndicatorAngle(0, 10, 0), 90.0f, 0.01f));
    CHECK(near(grenadeIndicatorAngle(0, -10, 0), 270.0f, 0.01f));
    CHECK(near(grenadeIndicatorAngle(10, 0, 90), 270.0f, 0.01f));
    CHECK(near(grenadeIndicatorAngle(0, 0, 0), 0.0f));
    // Layout at 640x480 (sx = sy = 1), centre (320, 240), grenade ahead: icon box centred 50 above the centre.
    {
        const auto l = grenadeIndicatorLayout(c, 320, 240, 1, 1, 0);
        CHECK(near(l.icon.xy[0], 307.5f) && near(l.icon.xy[1], 177.5f));
        CHECK(near(l.icon.xy[4], 332.5f) && near(l.icon.xy[5], 202.5f));
        // Pointer: rect [-12,13] x [-27,-15] about P = (320, 190).
        CHECK(near(l.pointer.xy[0], 308.0f) && near(l.pointer.xy[1], 163.0f));
        CHECK(near(l.pointer.xy[4], 333.0f) && near(l.pointer.xy[5], 175.0f));
    }
    // Grenade to the left (angle 90): icon centre 50 to the left, pointer further left (rotated -90 deg).
    {
        const auto l = grenadeIndicatorLayout(c, 320, 240, 1, 1, 90);
        CHECK(near((l.icon.xy[0] + l.icon.xy[4]) * 0.5f, 270.0f) && near((l.icon.xy[1] + l.icon.xy[5]) * 0.5f, 240.0f));
        float mx = 0, my = 0;
        for (int k = 0; k < 4; ++k) {
            mx += l.pointer.xy[size_t(k) * 2] * 0.25f;
            my += l.pointer.xy[size_t(k) * 2 + 1] * 0.25f;
        }
        CHECK(mx < 270.0f - 14.0f && near(my, 240.0f, 1.0f));
    }
    return failures ? 1 : 0;
}
