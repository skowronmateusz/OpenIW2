// Vehicle movement without a path (docs/spec/vehicles.md "Free physics and player driving"): drive input to
// accelerations (0x50de40), integration and the vehicle's own ground trace, slide and step moves (0x510d30,
// 0x50e4f0, 0x50e630, 0x50ebe0, 0x50ee50).
#pragma once

#include "game/collision.h"

namespace iw2::game {

// The owner's command as the vehicle reads it (0x510d30): usercmd forward/right bytes and 127 while the jump
// button (0x400) is held; zero without a driver.
struct DriveCommand {
    int forward = 0, right = 0, jump = 0; // −127..127
    float viewYaw = 0;                    // the driver's view yaw (client +0xc8)
};

// Vehicle file values used by the drive code, inches per second and degrees per second.
struct DriveParams {
    float maxSpeed = 0, accel = 0, rotRate = 0, rotAccel = 0;
};

// The physics part of a vehicle (vehicle +0xb8 …): origin, angles, the previous frame's angles, the world
// velocity, the velocity in the vehicle's frame (forward, left, up) and the angular velocity (pitch, yaw, roll).
struct DriveBody {
    Vec origin{}, angles{}, prevAngles{};
    Vec velocity{}, localVelocity{}, angularVelocity{};
};

// Linear (vehicle frame) and angular accelerations for one frame (0x50de40).
void driveAccelerations(const DriveParams& p, const DriveBody& b, const DriveCommand& cmd, Vec& accel, Vec& angularAccel);

// Velocity clipped against a plane (0x50e2f0): on walkable planes (normal z ≥ 0.7) a mostly horizontal velocity
// keeps its horizontal part and gets the vertical speed of the plane; otherwise v − n·(v·n)·(1.01 into the plane,
// 0.99 away from it).
Vec vehicleClipVelocity(const Vec& v, const Vec& n);

// Collision settings of the move: the vehicle traces a point (its mins/maxs are zero, 0x50d970) with its clip mask
// 0x810211 (0x50d900), skipping itself.
struct DriveTrace {
    uint32_t mask = 0x810211;
    int passEntity = -1;
};

// One frame of free movement (0x510d30 without the ground alignment): applies the accelerations, turns the yaw
// (pitch and roll become 0), accelerates along the yaw axes and, when moving, runs the ground trace and the
// slide/step moves. Updates localVelocity from the new velocity and the yaw axes.
void driveMove(const CollisionWorld& w, DriveBody& b, const DriveParams& p, const DriveCommand& cmd, const DriveTrace& t);

} // namespace iw2::game
