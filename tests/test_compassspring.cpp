// Compass angle spring (exe 0x4b09a0).
#include "game/compassspring.h"

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
} // namespace

int main() {
    using iw2::game::CompassSpring;
    CompassSpring s;
    CHECK(std::fabs(s.update(1000, 90.0f) - 90.0f) < 0.01f); // first call resets onto the target
    // A jump of 60 degrees: the angle trails and then settles on the target.
    float a = s.update(1016, 150.0f);
    CHECK(a < 150.0f && a > 85.0f);
    const float first = a;
    for (int t = 1032; t < 6000; t += 16) a = s.update(t, 150.0f);
    CHECK(std::fabs(a - 150.0f) < 0.3f);
    CHECK(first != a);
    // Across the 0/360 seam the spring never takes the long way.
    CompassSpring w;
    w.update(0, 359.0f);
    const float b = w.update(16, 2.0f);
    CHECK(b > 330.0f || b < 10.0f);
    // A gap over 500 ms resets onto the target at once.
    CHECK(std::fabs(w.update(2000, 200.0f) - 200.0f) < 0.01f && w.velocity() == 0.0f);
    return failures ? 1 : 0;
}
