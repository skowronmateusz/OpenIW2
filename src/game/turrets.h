// Turrets (misc_turret, spawnturret) and their script methods (docs/spec/turrets.md).
#pragma once

#include "script/value.h"

#include <cstdint>
#include <string>

namespace iw2::game {

// Turret info of a turret entity (entity +0x10c). Arcs are in degrees; right and top arcs are stored negated.
struct TurretState {
    std::string weapon;          // weaponinfo
    unsigned flags = 0x1003;     // +0x08: 1 AI, 2 automatic, 4 script firing, 8 missed first, 0x40 engaged target,
                                 // 0x1000 usable by the player, 0x2000 ignore goals (setup sets 0x1003, 0x51dd80)
    script::ObjectPtr target;    // +0x10 settargetentity
    script::Vec3 targetOffset{};
    float topArc = 0, rightArc = 0, bottomArc = 0, leftArc = 0; // +0x34/+0x38/+0x3c/+0x40
    float dropPitch = 0;         // +0x4c
    float yawConvergenceTime = 0, pitchConvergenceTime = 0;
    float suppressionTime = 0;   // seconds
    float aiSpread = 0, playerSpread = 0;
    int team = 3;                // +0x8c: 1 axis, 2 allies (setup: 3)
    float maxRangeSq = 3.4028235e38f; // +0x5c maxRange² (unlimited when maxRange ≤ 0)
    // Turret think state.
    int state = 0;               // +0x04: 0 idle, 2 aiming, 1 firing
    script::ObjectPtr engaged;   // +0x14 target being engaged
    int64_t engageStartMs = 0;   // +0x24
    script::Vec3 aimPoint{};     // +0x18
    script::Vec3 aimOffset{};    // +0x28
    script::Vec3 wantAngles{};   // pitch, yaw relative to the turret's angles
    float pitch = 0, yaw = 0;    // entity +0x70 / +0x74
    int scanIndex = -1;          // +0x80
    int fireCountdownMs = 0;     // +0x6c
    bool inUse = false;          // entity +0x112
    script::ObjectPtr owner;     // entity +0xf4 while in use
};

} // namespace iw2::game
