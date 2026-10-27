// Script vehicles driven along info_vehicle_node paths (docs/spec/vehicles.md).
#pragma once
#include <cmath>

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
    // texureScroll (sic, +0x48) / texureScrollScale (+0x64): tread texture scrolling (0x510870).
    int texScroll = 0;
    float texScrollScale = 0;
    std::string turretWeapon;
    // lowIdleSnd / highIdleSnd / lowEngineSnd / highEngineSnd (info slots +0x260..+0x266), turretSpinSnd / turretStopSnd.
    std::string lowIdleSnd, highIdleSnd, lowEngineSnd, highEngineSnd;
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
    // Tread scroll accumulator (entity +0x5c, 0x510870): -1 = no scroll; the cgame reads it (0x4ac839).
    int texScroll = 0;
    // Engine sound (0x50dbb7, 0x510be0): engineOn (+0x1ec) is set at spawn when both aliases of a pair exist and by
    // startenginesound / stopenginesound (0x513e80 / 0x513ee0); idleFrac / engFrac (+0x1f8 / +0x1fc); live voices.
    // Body jolt (joltbody / fireturret recoil 0x50dcf0, step 0x510770): direction factors (+0x1d4 forward, +0x1d8 right),
    // amplitude (+0x1dc), phase in degrees (+0x1e0); joltPitch / joltRoll are the entity fields +0x64 / +0x6c that the
    // controller 0x5127a0 applies to tag_body as angles (pitch, 0, roll).
    float joltDirF = 0, joltDirR = 0, joltAmp = 0, joltPhase = 0, joltPitch = 0, joltRoll = 0;
    bool engineOn = true;
    float idleFrac = 0, engFrac = 0;
    int idleHandle = 0, engHandle = 0;
    int owner = -1;            // entity number of the player using it (entity +0xf4, 0x7ff = none)
};

// 0x510770: while amp > 0, es+0x64 / +0x6c = sin(phase deg) * amp * dirF / dirR, then amp -= 0.05, phase += 36.
// Returns false when the jolt is idle (the entity fields keep their last values).
inline bool vehicleJoltStep(float& amp, float& phase, float dirF, float dirR, float& pitch, float& roll) {
    if (!(amp > 0.0f)) return false;
    const float s = std::sin(phase * 0.01745329238474369f) * amp;
    pitch = s * dirF;
    roll = s * dirR;
    amp -= 0.05f;
    phase += 36.0f;
    return true;
}

// One think of 0x510870: flag 0 -> -1; else acc += (int)(speed * (1/176) * (dvar > 0 ? dvar : scale) * 0.05 * 1000).
// speed is vehicle +0x1b0 (|forward speed|, inches per second). dvar = g_vehicleTexScrollScale (default 0).
inline int vehicleTexScrollStep(int flag, int acc, float speed, float dvar, float scale) {
    if (flag == 0) return -1;
    const float s = speed * 0.0056818f * (dvar > 0.0f ? dvar : scale) * 0.05f * 1000.0f;
    return acc + int(s);
}

// The V offset the renderer subtracts from tread texture coordinates (cgame 0x4ac839): accumulator * 0.001 (0x591e30);
// the cgame skips a negative accumulator (offset 0). TODO(spec): the gfx DLL step that turns the entity's
// (cg.time - value) * 0.001 into gameTime.z (assumed to cancel the time part), and the per-frame interpolation
// ((next - cur) * frameInterp) of the accumulator.
inline float vehicleTexScrollOffset(int acc) { return acc < 0 ? 0.0f : float(acc) * 0.001f; }

} // namespace iw2::game
