// Turrets (misc_turret, spawnturret) and their script methods (docs/spec/turrets.md).
#pragma once

#include "script/value.h"

#include <string>

namespace iw2::game {

// Turret info of a turret entity (entity +0x10c). Arcs are in degrees; right and top arcs are stored negated.
struct TurretState {
    std::string weapon;          // weaponinfo
    unsigned flags = 0;          // +0x08: 1 AI may use, 2 automatic, 4 firing, 0x40 has target entity,
                                 // 0x1000 usable by the player, 0x2000 ignore goals
    script::ObjectPtr target;    // +0x14 settargetentity
    script::Vec3 targetOffset{};
    float topArc = 0, rightArc = 0, bottomArc = 0, leftArc = 0; // +0x34/+0x38/+0x3c/+0x40
    float dropPitch = 0;         // +0x4c
    float yawConvergenceTime = 0, pitchConvergenceTime = 0;
    float suppressionTime = 0;   // seconds
    float aiSpread = 0, playerSpread = 0;
    int team = 0;                // +0x8c: 1 axis, 2 allies
    bool inUse = false;          // entity +0x112
    script::ObjectPtr owner;     // entity +0xf4 while in use
};

} // namespace iw2::game
