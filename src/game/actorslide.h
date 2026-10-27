// Actor slide move (exe 0x5475e0, the actor's PM_SlideMove; clip velocity 0x547540; normalize 0x4210d0 / 0x421270).
// The exe keeps the move block in a global (0x1cbacac, the actor's +0x224 block) and per-frame values in 0x1cbac58..0x1cbac88;
// here they are fields of one struct. The trace is G_LocationalTrace 0x44bf00 (box mins/maxs +0x4c / +0x58 of the block,
// pass entity +0x3c, mask 0x1cbaca4), supplied by the caller.
#pragma once

#include <cstdint>
#include <functional>

namespace iw2::game {

struct SlideTrace {
    float fraction = 1.0f;
    float normal[3] = {0, 0, 0};
    uint32_t surface = 0;    // trace +0x14 (bit 0x2000000 asks for a retrace without that content bit)
    uint16_t entity = 0x7ff; // trace +0x1c: 0x7fe world, 0x7ff none
    bool startSolid = false; // trace +0x22
    bool allSolid = false;   // trace +0x23
};
using SlideTraceFn = std::function<SlideTrace(const float start[3], const float end[3])>;

struct ActorSlide {
    float origin[3] = {0, 0, 0};   // block +0x0
    float velocity[3] = {0, 0, 0}; // block +0xc
    float gravity = 0;             // block +0x44
    float dt = 0;                  // 0x1cbac58 (the frame time in seconds)
    bool groundPlane = false;      // 0x1cbac60
    float groundNormal[3] = {0, 0, 1}; // 0x1cbac68
    float impactSpeed = 0;         // 0x1cbac88 (max of -into)
    bool flag87 = false;           // 0x1cbac87
    uint32_t mask = 0;             // 0x1cbaca4 (trace content mask)
    uint16_t groundEnt = 0x7ff;    // block +0x18 (0x7ff none)
    int mode = 0;                  // block +0x40 (5 = the special case of the ground snap)
    float groundTrace[9] = {};     // 0x1cbac64..: the last ground trace (fraction, normal, ..., entity, allsolid byte)
    bool groundEntValid = false;   // 0x1cbac5c (set with the ground plane by the ground probe)
    float stepHeight = 18.0f;      // 0x1cbaca8: 10, or 18 when the block byte +0x64 is 0 (0x548888)
    // Hit bookkeeping of the block: the first wall hit.
    uint16_t hitEnt = 0x7ff; // +0xf4
    float hitPos[2] = {0, 0};    // +0xf8
    float hitNormal[2] = {0, 0}; // +0x100
    bool hitIsAllSolid = false;  // byte +0x108 (set only by the allsolid exit)
    bool blocked = false;        // byte +0x109
    int touchCount = 0;          // +0x70
    uint16_t touch[0x20] = {};   // +0x74
};

// PM_ClipVelocity 0x547540: slope-aware clip (normal.z >= 0.7 and |v.z|² <= |v.xy|²: slide along the ramp scaled by n.z).
void clipVelocity(const float in[3], const float normal[3], float out[3], float overbounce);

// 0x5475e0. Returns 0 when the move was not blocked, 1 when it bumped (or got stuck), 2 for an all-solid start on the
// first iteration with flag87 set.
int slideMove(ActorSlide& pm, bool gravity, const SlideTraceFn& trace);

// 0x548290: the ground probe (PM_GroundTrace). Traces from origin + 0.25 down 0.25 (the step height when moving down on a ground
// entity) with the pm mask (the trace function reads pm.mask), handles all-solid starts (drops content bit 0x2000000 and retraces,
// then retraces from the origin), and sets groundEnt, groundPlane, groundEntValid, flag87 and the hit entity for steep ground.
void groundProbe(ActorSlide& pm, const SlideTraceFn& trace);
// 0x547500: adds an entity number to the touched list (not the world, at most 0x20, no duplicates).
void addTouch(ActorSlide& pm, uint16_t entity);

// 0x547db0 (the actor StepSlideMove): slide move; when it bumped, retry from a step height up and keep whichever works.
// Returns 1 normally, 0 when the move is stuck (all-solid slide, or the step-up start is solid at fraction 0).
int stepSlideMove(ActorSlide& pm, bool gravity, const SlideTraceFn& trace);

} // namespace iw2::game
