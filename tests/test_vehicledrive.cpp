// Tests for vehicle free physics (docs/spec/vehicles.md "Free physics and player driving").

#include "game/vehicledrive.h"

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

bool near(float a, float b, float eps) { return std::fabs(a - b) <= eps; }

} // namespace

int main() {
    using namespace iw2::game;
    {
        // Free fall without a map: 40 units per second lost per frame, the move uses the average vertical speed.
        CollisionWorld empty;
        DriveBody b;
        b.velocity = {0, 0, -1};
        for (int i = 0; i < 20; ++i) driveMove(empty, b, {}, {}, {});
        CHECK(near(b.velocity[2], -801.0f, 0.01f));
        CHECK(near(b.origin[2], -401.0f, 0.05f));
        CHECK(near(b.localVelocity[2], -801.0f, 0.01f));
    }
    {
        // Forward acceleration: one frame of accel towards maxSpeed, limited by accel (crusader_player numbers).
        const DriveParams p{30 * 17.6f, 18 * 17.6f, 60, 100};
        DriveBody b;
        DriveCommand cmd;
        cmd.forward = 127;
        Vec a{}, aa{};
        driveAccelerations(p, b, cmd, a, aa);
        CHECK(near(a[0], 316.8f, 0.01f));
        b.localVelocity = {520, 0, 0};
        driveAccelerations(p, b, cmd, a, aa);
        CHECK(near(a[0], 160.0f, 0.05f));
        cmd.forward = -127;
        b.localVelocity = {0, 0, 0};
        driveAccelerations(p, b, cmd, a, aa);
        CHECK(near(a[0], -316.8f, 0.01f));
        // No command: sideways speed is braked, limited by accel.
        cmd.forward = 0;
        b.localVelocity = {0, 5, 0};
        driveAccelerations(p, b, cmd, a, aa);
        CHECK(near(a[1], -100.0f, 0.01f));
    }
    {
        // Turning right lowers the yaw speed by rotAccel up to rotRate; with jump held the vehicle turns towards the
        // view yaw without the rotAccel limit.
        const DriveParams p{528, 316.8f, 60, 100};
        DriveBody b;
        DriveCommand cmd;
        cmd.right = 127;
        Vec a{}, aa{};
        driveAccelerations(p, b, cmd, a, aa);
        CHECK(near(aa[1], -100.0f, 0.01f));
        b.angularVelocity = {0, -58, 0};
        driveAccelerations(p, b, cmd, a, aa);
        CHECK(near(aa[1], -40.0f, 0.01f));
        cmd = {};
        cmd.jump = 127;
        cmd.viewYaw = 90;
        b.angularVelocity = {};
        driveAccelerations(p, b, cmd, a, aa);
        CHECK(near(aa[1], 1200.0f, 0.01f));
    }
    {
        // Clip rule: walkable ground keeps a mostly horizontal velocity; other planes push 1 % into / out of it.
        Vec v = vehicleClipVelocity({100, 0, -10}, {0, 0, 1});
        CHECK(near(v[0], 100.0f, 1e-4f) && near(v[1], 0.0f, 1e-4f) && near(v[2], 0.0f, 1e-4f));
        v = vehicleClipVelocity({-100, 0, 0}, {1, 0, 0});
        CHECK(near(v[0], 1.0f, 1e-3f));
        v = vehicleClipVelocity({100, 0, 0}, {1, 0, 0});
        CHECK(near(v[0], 1.0f, 1e-3f));
    }
    {
        // One frame of driving in the air: the yaw turns by angular velocity × 0.05, pitch and roll become 0.
        CollisionWorld empty;
        const DriveParams p{528, 316.8f, 60, 100};
        DriveBody b;
        b.angles = {5, 0, 3};
        DriveCommand cmd;
        cmd.right = 127;
        driveMove(empty, b, p, cmd, {});
        CHECK(near(b.angularVelocity[1], -5.0f, 1e-3f));
        CHECK(near(b.angles[0], 0.0f, 0.0f) && near(b.angles[2], 0.0f, 0.0f));
        CHECK(near(b.angles[1], -0.25f, 0.006f));
    }
    if (failures) std::cerr << failures << " failures\n";
    return failures ? 1 : 0;
}
