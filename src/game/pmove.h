// Player movement against world collision (walk, gravity, jump, step, slide).
#pragma once

#include "game/collision.h"

namespace iw2::game {

struct PlayerInput {
    float forward = 0, right = 0; // -1..1
    bool jump = false;
    bool sprint = false;
    float yaw = 0;                // view yaw in degrees
};

struct PlayerState {
    Vec origin{};
    Vec velocity{};
    bool onGround = false;
};

struct MoveParams {
    float speed = 190.0f;       // g_speed default (inferred from the id Tech 3 lineage; to verify)
    float sprintScale = 1.5f;
    float gravity = 800.0f;
    float jumpSpeed = 270.0f;
    float stepHeight = 18.0f;
    float friction = 6.0f;
    float accelerate = 10.0f;
    float airAccelerate = 1.0f;
    Vec mins{-15, -15, 0};
    Vec maxs{15, 15, 70};
    float eyeHeight = 60.0f;
};

void playerMove(const CollisionWorld& world, PlayerState& ps, const PlayerInput& in, float dt, const MoveParams& mp = {});

} // namespace iw2::game
