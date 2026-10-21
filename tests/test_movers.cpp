// Tests for mover trajectories (accel/linear/decel phases) and the angle difference helper.

#include "game/movers.h"

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

bool near(float a, float b, float eps = 0.05f) { return std::fabs(a - b) <= eps; }

float run(iw2::game::MoverChannel& ch, int64_t from, int64_t to, int64_t& doneAt) {
    float x = 0;
    doneAt = -1;
    for (int64_t t = from; t <= to; t += 50) {
        if (iw2::game::stepMove(ch, t) && doneAt < 0) doneAt = t;
        x = ch.active ? ch.tr.evaluate(t)[0] : ch.tr.base[0];
    }
    return x;
}

} // namespace

int main() {
    using namespace iw2::game;
    CHECK(near(angleSubtract(10, 350), 20));
    CHECK(near(angleSubtract(350, 10), -20));
    CHECK(near(angleSubtract(180, 0), 180));

    // Linear move of 100 units in 1 s.
    MoverChannel lin;
    startMove(lin, {0, 0, 0}, {100, 0, 0}, 1000, 0, 0, 0);
    CHECK(near(lin.tr.evaluate(500)[0], 50));
    int64_t done;
    CHECK(near(run(lin, 0, 1500, done), 100));
    CHECK(done == 1000);

    // 2 s with 0.5 s accel and 0.5 s decel: speed = 2·100/(4 − 0.5 − 0.5) s = 66.67 units/s.
    MoverChannel ad;
    startMove(ad, {0, 0, 0}, {100, 0, 0}, 2000, 500, 500, 0);
    CHECK(near(ad.speed, 66.6667f, 0.01f));
    CHECK(near(ad.tr.evaluate(500)[0], 16.667f));
    CHECK(near(run(ad, 0, 2500, done), 100));
    CHECK(done == 2000);

    if (failures) return 1;
    std::cout << "movers tests passed\n";
    return 0;
}
