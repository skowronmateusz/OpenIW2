// Tests for mover trajectories (accel/linear/decel phases) and the angle difference helper.

#include "game/movers.h"
#include "game/turrets.h"

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

    // rotatevelocity (0x532e20): 90 deg/s for 2 s stops at 180 and finishes.
    MoverChannel rv;
    startVelocityMove(rv, {0, 0, 0}, {90, 0, 0}, 2000, 0, 0, 0);
    CHECK(rv.tr.type == 3 && near(rv.target[0], 180));
    CHECK(near(run(rv, 0, 3000, done), 180));
    CHECK(done == 2000);
    // With 0.5 s accel and 0.5 s decel: 0.5 s ramp to 90, 1 s at 90, 0.5 s ramp down = 22.5 + 90 + 22.5.
    MoverChannel rva;
    startVelocityMove(rva, {0, 0, 0}, {90, 0, 0}, 2000, 500, 500, 0);
    CHECK(near(rva.phase1End[0], 22.5f) && near(rva.phase2End[0], 112.5f) && near(rva.target[0], 135));
    CHECK(near(run(rva, 0, 3000, done), 135));
    CHECK(done == 2000);

    // vibrate (0x533b90): sine around the angles, zero crossings at 0, half and full duration; yaw 0 axis: forward = +x, left = +y.
    MoverChannel vb;
    startVibrate(vb, {0, 0, 0}, {2, 0, 0}, 5, 2.0f, 0.5f, 10000);
    CHECK(vb.tr.type == 4 && vb.tr.durationMs == 2000 && vb.tr.startMs == 10500);
    CHECK(near(vb.tr.delta[0], 5) && near(vb.tr.delta[1], 0) && near(vb.tr.delta[2], 0));
    CHECK(near(vb.tr.evaluate(10500)[0], 0) && near(vb.tr.evaluate(11000)[0], 5) && near(vb.tr.evaluate(11500)[0], 0, 0.1f));
    CHECK(near(vb.tr.evaluate(12000)[0], -5));
    CHECK(!stepMove(vb, 12499) && stepMove(vb, 12500) && vb.tr.type == 0 && near(vb.tr.base[0], 0));
    MoverChannel vl; // along the left axis (+y at yaw 0) the roll swings
    startVibrate(vl, {0, 0, 0}, {0, 3, 0}, 1, 1.0f, 0.0f, 0);
    CHECK(near(vl.tr.delta[0], 0) && near(vl.tr.delta[2], 1));

    // node_turret cover arcs (0x5159f0): a right arc of −45 (stored negated) wraps to ~315, a left arc of 45 stays.
    CHECK(near(iw2::game::turretArcWrapped(-45.0f), 315.0f, 0.02f));
    CHECK(near(iw2::game::turretArcWrapped(45.0f), 45.0f, 0.02f));
    CHECK(near(iw2::game::turretArcWrapped(0.0f), 0.0f));

    if (failures) return 1;
    std::cout << "movers tests passed\n";
    return 0;
}
