// Turrets (misc_turret, spawnturret) and their script methods (docs/spec/turrets.md).
#pragma once

#include "script/value.h"

#include <cstdint>
#include <string>

namespace iw2::game {

// An arc limit as the node_turret cover test reads it (0x5159f0): (int)(deg · 65536 / 360) & 0xffff, back to degrees (· 360 / 65536).
inline float turretArcWrapped(float deg) { return float(uint32_t(int(deg * 182.04444885253906f)) & 0xffffu) * 0.0054931640625f; }

// Turret info of a turret entity (entity +0x10c). Arcs are in degrees; right and top arcs are stored negated.
struct TurretState {
    std::string weapon;          // weaponinfo
    unsigned flags = 0x1003;     // +0x08: 1 AI, 2 automatic, 4 script firing, 8 missed first, 0x40 engaged target,
                                 // 0x1000 usable by the player, 0x2000 ignore goals (setup sets 0x1003, 0x51dd80)
    script::ObjectPtr target;    // +0x10 settargetentity
    script::Vec3 targetOffset{};
    float topArc = 0, rightArc = 0, bottomArc = 0, leftArc = 0; // +0x34/+0x38/+0x3c/+0x40
    float setupRightArc = 0, setupLeftArc = 0;                  // +0x44/+0x48 (restored when a gunner takes it)
    float dropPitch = -90.0f;    // +0x4c (setup 0x51e39d: -90; 0x51cf10 replaces it by the stock-on-ground pitch)
    bool dropPitchDone = false;  // the first think (0x51ced0, +50 ms) has run 0x51cf10
    float yawConvergenceTime = 0, pitchConvergenceTime = 0;
    float suppressionTime = 0;   // seconds
    float aiSpread = 0, playerSpread = 0;
    int team = 3;                // +0x8c: 1 axis, 2 allies (setup: 3)
    float maxRangeSq = 3.4028235e38f; // +0x5c maxRange² (unlimited when maxRange ≤ 0)
    script::ObjectPtr hitBy;     // +0x60: the enemy sentient that hit the gunner (flag 0x10), 0x54af30
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
    std::string loopSound;       // entity +6 (es.loopSound) as an alias, "" for none
    int loopHandle = -1;         // the started loop sound
    bool inUse = false;          // entity +0x112
    script::ObjectPtr owner;     // entity +0xf4 while in use
    // Player use (0x51db50 … 0x51b3d0).
    int stance = 0;              // +0x64: the weapon's `stance` (weapondef +0x8c: stand 0, duck 1, prone 2)
    int savedStance = -1;        // +0x68: the user's stance when it got on (prone 2, crouch 1, stand 0), −1 none
    script::Vec3 savedOrigin{};  // +0x74: the user's origin when it got on
    int fireTimerMs = 0;         // +0x0c: time until the player's next shot
};

} // namespace iw2::game
