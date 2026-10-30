// Death slow motion: frame-time scale curve (0x4bee00) and its effect on a dead player's movement (0x42eb70).
#include "game/framescale.h"
#include "game/pmove.h"

#include <algorithm>
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
    using namespace iw2::game;
    CHECK(frameScaleAt(0, 0, 500, 1, 0.1f, 1.0f) == 1.0f);       // none running
    CHECK(frameScaleAt(1000, 4000, 1000, 1, 0.1f, 1.0f) == 1.0f); // not yet past the start
    CHECK(std::fabs(frameScaleAt(1000, 4000, 2500, 1, 0.1f, 1.0f) - 0.55f) < 1e-5f);
    CHECK(frameScaleAt(1000, 4000, 4000, 1, 0.1f, 0.3f) == 0.1f); // ends at 0.1, not 0
    CHECK(frameScaleAt(1000, 4000, 9000, 1, 0.1f, 0.3f) == 0.1f);

    // A dead player in free fall (pm_type >= 4, no world): with the scale at 0.1 one real second moves it the distance
    // of 0.1 s of play, not of a full second.
    CollisionWorld empty;
    auto fall = [&](float scale) {
        PlayerState ps;
        ps.origin = {0, 0, 1000};
        ps.velocity = {200, 0, 0};
        PlayerInput in;
        in.dead = true;
        float remaining = scaledFrameTime(1.0f, scale);
        while (remaining > 0) {
            const float step = std::min(remaining, 1.0f / 60.0f);
            playerMove(empty, ps, in, step);
            remaining -= step;
        }
        return ps.origin;
    };
    const auto full = fall(1.0f), slow = fall(0.1f);
    CHECK(std::fabs(slow[0] - 20.0f) < 2.0f);
    CHECK(full[0] > 150.0f);
    CHECK(slow[2] > 990.0f);
    return failures ? 1 : 0;
}
