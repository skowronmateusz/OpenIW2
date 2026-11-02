// The vehicle turret spin-sound state (turret think 0x5102e0, +0x18c): turning freely sets it, a clamped step does not.
#include "game/vehicles.h"

#include <iostream>

using namespace iw2::game;

static int failures = 0;
#define CHECK(c)                                                                        \
    do {                                                                                \
        if (!(c)) { std::cout << "FAIL line " << __LINE__ << ": " #c "\n"; ++failures; } \
    } while (0)

int main() {
    // Yawing a flak88 (spans 180 degrees, never clamped): 40 degrees to go, the clamp cuts nothing.
    CHECK(vehicleTurretTurning(0.0f, 40.0f, 0.0f, 0.0f));
    CHECK(vehicleTurretTurning(-30.0f, 0.5f, 0.0f, 0.0f));  // elevating
    CHECK(!vehicleTurretTurning(1.9f, -1.9f, 0.0f, 0.0f));   // within 2 degrees: settled
    CHECK(!vehicleTurretTurning(30.0f, 0.0f, 0.4f, 0.0f));   // the span clamp cut the pitch: no sound
    CHECK(vehicleTurretTurning(30.0f, 5.0f, 0.4f, 0.0f));    // pitch cut but yaw still turning
    // The old test compared the turned angle itself with 0, which is false for any normal heading.
    CHECK(vehicleTurretTurning(-10.0f, 0.0f, 0.0f, 0.0f));
    return failures ? 1 : 0;
}
