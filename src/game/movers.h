// Script movers (moveto, rotateto, …) and entity linking; docs/spec/movers.md.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace iw2::game {

using MVec = std::array<float, 3>;

// One trajectory, as in the original entity state: {type, start, duration, base, delta}.
struct Trajectory {
    int type = 0; // 0 stationary, 2 linear, 3 linear-stop, 4 sine, 5 gravity, 7 accelerate, 8 decelerate
    int64_t startMs = 0;
    int durationMs = 0;
    MVec base{}, delta{};
    MVec evaluate(int64_t nowMs) const;
};

// A move or rotation in progress with its acceleration phases (0x532ad0 / 0x5327d0).
struct MoverChannel {
    bool active = false;
    bool angles = false;
    Trajectory tr;
    MVec target{};
    int linearMs = 0, decelMs = 0;
    float speed = 0;
    MVec velocity{}, phase1End{}, phase2End{};
};

struct MoverState {
    MoverChannel pos, apos;
};

struct Link {
    int parent = -1; // entity number
    std::string tag;
    MVec originOffset{}, anglesOffset{};
    // Script links (linkto, tagInfo +0x10) keep the offset in the full frame of the parent's tag (0x4d6ad0); the
    // player's vehicle link keeps the older tag-position + parent-yaw placement.
    bool fullFrame = false;
    // tagInfo +0x40: the parent frame when animscripted started (0x4f60ad); scripted poses move with the parent
    // from there (0x53bcf5).
    bool hasStartFrame = false;
    MVec startOrigin{};
    std::array<MVec, 3> startAxis{};
};

// Shortest signed difference a − b in degrees, wrapped to [−180, 180] (0x423f50).
float angleSubtract(float a, float b);

// Starts a move from `current` to `target` over totalMs with accel/decel phases (0x532ad0).
void startMove(MoverChannel& ch, const MVec& current, const MVec& target, int totalMs, int accelMs, int decelMs, int64_t nowMs);
// Advances finished phases (0x5327d0); returns true when the whole move is done.
void startVelocityMove(MoverChannel& ch, const MVec& current, const MVec& velocity, int totalMs, int accelMs, int decelMs, int64_t nowMs);
bool stepMove(MoverChannel& ch, int64_t nowMs);

} // namespace iw2::game
