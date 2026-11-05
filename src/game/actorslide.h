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
    uint32_t surfFlags = 0;  // trace +0x10 (surface type in bits 20..24, read at 0x548941)
    uint16_t entity = 0x7ff; // trace +0x1c: 0x7fe world, 0x7ff none
    bool allSolid = false;   // trace +0x22 (set only when the end is inside too; read by the step-up test 0x547ef6)
    bool startSolid = false; // trace +0x23 (set whenever the start is inside; 0x5477d0, 0x548011, 0x54831c)
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
    float groundTrace[9] = {};     // 0x1cbac64..: the last ground trace (fraction, normal, ..., entity, start-solid byte at +0x23 = 0x1cbac87)
    uint32_t groundSurfFlags = 0;  // 0x1cbac74
    float moveDelta[3] = {0, 0, 0}; // block +0x2c (the desired displacement of this frame)
    int timeMs = 0;                // block +0x48
    bool lowStep = false;          // block byte +0x64: step height 10 instead of 18
    bool contact = false;          // block +0x20 (set when the ground normal.z >= 0.3)
    float contactNormalZ = 0;      // block +0x24
    int surfaceType = 0;           // block +0x28
    bool groundEntValid = false;   // 0x1cbac5c (set with the ground plane by the ground probe)
    float stepHeight = 18.0f;      // 0x1cbaca8: 10, or 18 when the block byte +0x64 is 0 (0x548888)
    // Hit bookkeeping of the block: the first wall hit.
    uint16_t hitEnt = 0x7ff; // +0xf4
    float hitPos[2] = {0, 0};    // +0xf8
    float hitNormal[2] = {0, 0}; // +0x100
    bool hitIsStartSolid = false; // byte +0x108 (set only by the start-solid exit at fraction 0, 0x5477d0)
    bool blocked = false;        // byte +0x109
    int touchCount = 0;          // +0x70
    uint16_t touch[0x20] = {};   // +0x74
};

// gethityaw (0x540110): the yaw (0x421890: 0 for a zero vector, else atan2 in degrees, 0..360) of the reversed hit normal (-x, -y).
float actorHitYaw(float nx, float ny);

// PM_ClipVelocity 0x547540: slope-aware clip (normal.z >= 0.7 and |v.z|² <= |v.xy|²: slide along the ramp scaled by n.z).
void clipVelocity(const float in[3], const float normal[3], float out[3], float overbounce);

// 0x5475e0. Returns 0 when the move was not blocked, 1 when it bumped (or got stuck), 2 for a start-solid trace at fraction 0 on the
// first iteration with flag87 set.
int slideMove(ActorSlide& pm, bool gravity, const SlideTraceFn& trace);

// 0x548290: the ground probe (PM_GroundTrace). Traces from origin + 0.25 down 0.25 (the step height when moving down on a ground
// entity) with the pm mask (the trace function reads pm.mask), handles start-solid traces (drops content bit 0x2000000 and retraces,
// then retraces from the origin), and sets groundEnt, groundPlane, groundEntValid, flag87 and the hit entity for steep ground.
void groundProbe(ActorSlide& pm, const SlideTraceFn& trace);
// 0x547500: adds an entity number to the touched list (not the world, at most 0x20, no duplicates).
void addTouch(ActorSlide& pm, uint16_t entity);

// 0x547db0 (the actor StepSlideMove): slide move; when it bumped, retry from a step height up and keep whichever works.
// Returns 1 normally, 0 when the move is stuck (start-solid slide, or the step-up trace is all-solid at fraction 0).
int stepSlideMove(ActorSlide& pm, bool gravity, const SlideTraceFn& trace);

// 0x5487c0 (without the footstep timer 0x5485a0 and the sound call 0x535770, see TODO(spec) in docs/spec/ai.md): one actor move.
// mode 3: origin += moveDelta; mode 4 (0x548260): velocity = moveDelta / dt, slide move without gravity; otherwise the ground
// probe and then mode 5 (0x548200): velocity xy = moveDelta / dt, z = 0, step slide with gravity; ground entity (0x548110): the
// same, clipped to the ground plane, keeping the speed, step slide without gravity (stays put when standing still); else
// (0x5480d0): clip to the ground plane when there is one, step slide with gravity. A ground entity (0x7fe) with no desired motion
// just zeroes the velocity. Returns the step slide result (1 for the early outs).
int moveBlock(ActorSlide& pm, const SlideTraceFn& trace);

// Default on; IW2_ACTOR_SLIDE=0 switches World::moveActor back to the plain ground snap.
// 0x501d00 after a failed move block: the vertical nudge towards the nearest node, from dz = node z - origin z
// (constants 0x592010 = 8, 0x591e00 = 0, 0x592390 = -18).
float stuckNudge(float dz);
// 0x501fd5..0x50219c: the nearest-node loop replaces its best node when best >= candidate (the compare skips only when best < candidate),
// so on an exact tie the later node wins.
bool nearestReplaces(float bestD2, float candidateD2);
// The path walk's direct move (0x501d00, 0x501e4f..0x501f1d; docs/spec/ai.md "Failed actor move block"): the nudge above
// is only the no-path / followed-without-a-path physics. A path walker whose move block failed is moved by its raw
// delta instead (no collision). After a successful block the same direct move applies when the lookahead slope
// (+0x6d0) is above 4 (0x591e64), the delta goes up and the block did not raise the actor (pathSteepUnlifted), or when
// the sentient has no node within 192 even after a re-query. The direct move also clears the block's vertical velocity,
// the avoided sentient (+0x6ba) and raises the lookahead budget (+0x6e0) to at least 64 (0x56f6e0, pathDirectBudget).
bool pathSteepUnlifted(float lookSlope, float deltaZ, float blockZ, float entityZ);
float pathDirectBudget(float budget);
// 0x503ec0 (stop soon unset): the negotiation state is pushed when the 2D squared distance to the begin point is within
// |block velocity|^2 * 0.005 + 1e-6 (0x503fb0).
bool negotiationInRange(float dist2, float speed2);

// 0x504113..0x504145 (stop soon, timer running): the move delta is (end - origin) * k, k = (2 - r) * r with r = 50 / remaining ms,
// divided by the contact normal z when the block has a ground contact (contactNz 0 = none).
float stopSoonScale(int remainingMs, float contactNz);

// The path mover 0x500de0, attempts part (docs/spec/ai.md "The path mover with its sidestep"): up to three move blocks from
// the entity origin, the second and third with the desired delta turned to a side of the bumped entity. Every attempt restores
// the block velocity, ground entity and contact (saved on entry) and starts at `entityOrigin`.
struct SidestepInput {
    float start[2] = {0, 0};  // path start +0x3c8 (actor +0x6f8)
    float segDir[2] = {0, 0}; // dir of point count - 2 (the first segment)
    int sideBits = 0;         // gentity flags +0x128 & 0x18 on entry
    bool pathOk = false;      // 0x5442a0 (pathSidestepOk), gates the first sidestep only
    std::function<bool(int)> avoid; // 0x500d80 for a hit entity number
};
enum class SidestepOutcome { Failed, Clear, Tail }; // 0: a block failed; 1: the first block hit nothing; else the tail 0x5011d7
struct SidestepResult {
    SidestepOutcome outcome = SidestepOutcome::Clear;
    int side = 0;        // S: 8, 0x10 or 0 (no sidestep ran)
    int firstHit = 0x7ff;
};
SidestepResult sidestepAttempts(ActorSlide& pm, const float entityOrigin[3], const SidestepInput& in, const SlideTraceFn& trace);
// 0x500f04..0x500fd9 / 0x5010a0..0x501147: turns pm.moveDelta xy for one sidestep of length s; returns the side (8 or 0x10).
int sidestepDelta(float moveDelta[3], const float d[2], const float n[2], const float segDir[2], float s, int sideBits);
// 0x5011d7..0x50121f: the side-bit update. Returns true when the side flipped; `flags` gets bits 0x18 replaced by `side`.
bool sidestepFlip(int& flags, int side);
// 0x501248..0x5012a1: the move made less than a quarter of the desired delta's 2D length along its direction.
bool sidestepShort(const float blockOrigin[3], const float entityOrigin[3], const float moveDelta[3]);
// 0x5442a0 on plain values: count - 1 > traversal, points count-1 / count-2 have nodes, no far-off flag 0x20, budget > 0.001
// (0x543bc0), and (count != planned or the start is not exactly point count-1).
bool pathSidestepOk(int count, int planned, int traversal, int nodeLast, int nodePrev, uint32_t flags, float budget, bool startAtLast);

bool actorSlideEnabled();

} // namespace iw2::game
