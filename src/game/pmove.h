// Player movement against world collision (docs/spec/pmove.md): stances with view-height transitions,
// friction, acceleration, command scaling, jump, gravity, step and slide.
#pragma once

#include "game/collision.h"

#include <cstdint>

namespace iw2::game {

enum class Stance { Stand, Crouch, Prone };

struct PlayerInput {
    float forward = 0, right = 0; // -1..1 (the original's -127..127 command bytes)
    bool jump = false;            // held
    Stance stance = Stance::Stand; // requested stance
    bool leanLeft = false, leanRight = false; // +leanleft / +leanright (usercmd bits 0x40 / 0x80)
    float yaw = 0;                // view yaw in degrees
};

struct PlayerState {
    Vec origin{};
    Vec velocity{};
    bool onGround = false;
    Stance stance = Stance::Stand;
    int viewHeightTarget = 60;   // 60 stand, 40 crouch, 11 prone
    float viewHeight = 60;       // current eye height above the origin
    int64_t timeMs = 0;
    int64_t transitionStartMs = -1;
    bool transitionDown = false;
    int64_t lastJumpMs = -100000;
    bool jumpHeld = false;
    float lean = 0;              // leanf: −0.5..0.5 (−0.25..0.25 prone), negative = left
};

// View offset of a lean (0x440380 with roll factor 16 and distance 20): g = (2 − |lean|)·lean, roll = 16·g
// degrees; offset = 20·g·(cos(roll)·right − sin(roll)·up) for yaw in degrees.
Vec leanOffset(float yawDeg, float lean);

// Movement settings: dvar defaults and constants read from CoD2SP_s.exe (docs/spec/pmove.md).
struct MoveParams {
    int speed = 190;                  // g_speed
    float gravity = 800.0f;           // g_gravity
    float friction = 5.5f;            // friction
    float stopSpeed = 100.0f;         // stopspeed
    float jumpHeight = 39.0f;         // jump_height
    float backSpeedScale = 0.7f;      // player_backSpeedScale
    float strafeSpeedScale = 0.8f;    // player_strafeSpeedScale
    float weaponSpeedScale = 0.0f;    // current weapon's moveSpeedScale (0 = none)
    float stepHeight = 18.0f;         // jump_stepSize (TODO(spec): step logic of 0x508390 not implemented)
    Vec mins{-15, -15, 0};
    Vec maxs{15, 15, 70};             // standing; crouch 50, prone 30
};

// Advances the player by dt seconds (split into steps of at most 66 ms like the original).
void playerMove(const CollisionWorld& world, PlayerState& ps, const PlayerInput& in, float dt, const MoveParams& mp = {});

} // namespace iw2::game
