// Script vehicles driven along info_vehicle_node paths (docs/spec/vehicles.md).
#pragma once

#include "script/value.h"

#include <string>
#include <vector>

namespace iw2::game {

using script::ObjectPtr;
using script::Vec3;

// One info_vehicle_node / info_vehicle_node_rotate after load-time linking.
struct VehicleNode {
    ObjectPtr obj;
    std::string name, target;
    bool rotate = false;       // info_vehicle_node_rotate: the vehicle takes the node's angles
    float speed = -1.0f;       // inches per second (the map key is miles per hour)
    float lookahead = -1.0f;   // seconds
    Vec3 origin{};
    Vec3 dir{};                // unit vector to the next node
    Vec3 angles{3.14159274f, 3.14159274f, 3.14159274f}; // "unset" marker until resolved
    float length = 0;          // distance to the next node
    int next = -1, prev = -1;
};

// Position on a path; the engine keeps one for the vehicle and a copy one step ahead.
struct VehiclePathPos {
    int node = -1;
    bool end = false;
    float frac = 0;      // along the current node's segment
    float speed = 0;     // path speed at this point (inches per second)
    float lookahead = 0; // path lookahead at this point (seconds)
    float rotateFrac = 0;
    Vec3 origin{}, angles{}, lookPoint{};
    // setswitchnode: while stepping, node switchFrom.name is replaced by `switched`, then restored from `original`.
    VehicleNode switched, original;
    bool hasSwitch = false;
};

// vehicles/<name> ("VEHICLEFILE\\key\\value…", field table 0x5a91e8). Speeds are inches per second.
struct VehicleInfo {
    std::string name;
    int type = 0; // 0 "4 wheel", 1 "tank", 2 "plane", 3 "boat", 4 "artillery"
    float maxSpeed = 0, accel = 0, collisionSpeed = 0, engineSndSpeed = 0;
    float rotRate = 0, rotAccel = 0; // degrees per second (and per second²), not scaled at load
    float suspensionTravel = 0, maxBodyPitch = 0, maxBodyRoll = 0;
    float turretHorizSpanLeft = 0, turretHorizSpanRight = 0, turretVertSpanUp = 0, turretVertSpanDown = 0, turretRotRate = 0;
    bool quadBarrel = false;
    std::string turretWeapon;
};

struct VehicleState {
    const VehicleInfo* info = nullptr;
    // Per wheel (front_left, front_right, back_left, back_right, middle_left, middle_right): settling height and
    // vertical speed, and the surface type under it (vehicle +0x124/+0x13c/+0x154).
    float wheelVel[6]{}, wheelHeight[6]{};
    int wheelSurface[6]{};
    VehiclePathPos path;
    bool started = false;      // startpath
    int waitNode = -1;
    float waitSpeed = -1.0f;   // inches per second
    int speedMode = 0;         // 0 path speed, 1 setspeed, 2 resumespeed
    float targetSpeed = 0, accel = 0, speed = 0;
    float stepFrac = 0;
    Vec3 velocity{};           // world velocity (vehicle +0x100)
    // Free physics (0x510d30): velocity in the vehicle's frame (forward, left, up, +0x10c), angular velocity
    // (pitch, yaw, roll, +0x118) and |forward speed| after the move (+0x1b0).
    Vec3 localVelocity{}, angularVelocity{};
    float forwardSpeed = 0;
    // Player aim (0x5108e0): fire timer in ms (+0x184, set by fireturret), the aim point (+0x1bc) and whether the
    // barrel-to-flash sight line was blocked this frame (+0x19c).
    int fireTimerMs = 0;
    Vec3 aimPoint{};
    bool sightBlocked = false;
    // Turret (vehicle +0x1b4…+0x1d0; turret pitch/yaw relative to the body are entity +0x70/+0x74).
    bool hasTarget = false;
    ObjectPtr targetEnt;
    Vec3 targetVec{}, targetOffset{};
    float turretPitch = 0, turretYaw = 0;
    bool altBarrel = false;
    int team = 0;              // 0 none, 1 axis, 2 allies
    bool usable = false;       // contents 0x200000; initially spawnflags & 1 (0x50dcd5)
    int owner = -1;            // entity number of the player using it (entity +0xf4, 0x7ff = none)
};

} // namespace iw2::game
