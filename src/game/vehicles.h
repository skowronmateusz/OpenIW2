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

struct VehicleState {
    VehiclePathPos path;
    bool started = false;      // startpath
    int waitNode = -1;
    float waitSpeed = -1.0f;   // inches per second
    int speedMode = 0;         // 0 path speed, 1 setspeed, 2 resumespeed
    float targetSpeed = 0, accel = 0, speed = 0;
    float stepFrac = 0;
    Vec3 velocity{};
    int team = 0;              // 0 none, 1 axis, 2 allies
    bool usable = false;
};

} // namespace iw2::game
