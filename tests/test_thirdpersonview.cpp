// MP third-person (dead) view, docs/spec/mp-spectate.md "Dead view".
#include "game/thirdpersonview.h"

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
bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) < eps; }
} // namespace

int main() {
    using namespace iw2::game;
    const auto open = [](const Vec&, const Vec&, const Vec&, const Vec&) { return TraceResult{}; };
    // Level view, dead yaw 90: 120 back along +y from 8 above the eye, looking slightly down at the focus 512 ahead.
    {
        const ThirdPersonView v = thirdPersonView({0, 0, 8}, 0, 10, 0, 90.0f, 120, 0, open);
        CHECK(near(v.origin[0], 0) && near(v.origin[1], -120) && near(v.origin[2], 16));
        CHECK(near(v.yaw, 90));
        CHECK(near(v.pitch, std::atan2(8.0f, 632.0f) * 57.29578f));
        CHECK(v.roll == 0);
    }
    // Focus pitch limited to 45, view pitch halved: pitch 80 -> camera behind and above, looking down.
    {
        const ThirdPersonView v = thirdPersonView({0, 0, 0}, 80, 0, 0, std::nullopt, 120, 0, open);
        CHECK(v.origin[0] < 0 && v.origin[2] > 8);
        CHECK(v.pitch > 0 && v.pitch < 80);
    }
    // A wall halfway: pulled in and raised by (1 - fraction) * 32, then traced again.
    {
        int calls = 0;
        const auto wall = [&](const Vec& a, const Vec& b, const Vec& mn, const Vec& mx) {
            ++calls;
            CHECK(mn[0] == -4 && mx[2] == 4);
            TraceResult r;
            if (calls == 1) r.fraction = 0.5f;
            r.endPos = {a[0] + (b[0] - a[0]) * r.fraction, a[1] + (b[1] - a[1]) * r.fraction, a[2] + (b[2] - a[2]) * r.fraction};
            return r;
        };
        const ThirdPersonView v = thirdPersonView({0, 0, 0}, 0, 0, 0, std::nullopt, 120, 0, wall);
        CHECK(calls == 2);
        CHECK(near(v.origin[0], -60) && near(v.origin[2], 4 + 16));
    }
    // cg_thirdPersonAngle 90 moves the camera to the left of the view (minus the right vector) and lowers the yaw.
    {
        const ThirdPersonView v = thirdPersonView({0, 0, 0}, 0, 0, 0, std::nullopt, 120, 90, open);
        CHECK(near(v.origin[0], 0, 1e-2f) && near(v.origin[1], 120, 1e-2f));
        CHECK(near(v.yaw, -90));
    }
    return failures ? 1 : 0;
}
