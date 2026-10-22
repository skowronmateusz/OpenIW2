// Player movement. Rules marked with an address follow CoD2SP_s.exe 1.3 (docs/spec/pmove.md).
#include "game/pmove.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace iw2::game {
namespace {

float dot(const Vec& a, const Vec& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec add(const Vec& a, const Vec& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec scale(const Vec& a, float s) { return {a[0] * s, a[1] * s, a[2] * s}; }

Vec cross(const Vec& a, const Vec& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
float normalize(Vec& v) {
    const float l = std::sqrt(dot(v, v));
    if (l != 0) v = scale(v, 1.0f / l);
    return l;
}

constexpr float kMinWalkNormal = 0.7f;     // DAT_00591f30
constexpr float kGroundTraceOffset = 0.25f; // DAT_00591fe0
constexpr float kStepSize = 18.0f, kProneStepSize = 10.0f; // 0x54f2e0
constexpr float kStickToGround = 9.0f;      // DAT_00591e88
constexpr float kMinStepNormal = 0.3f;      // DAT_00592018
constexpr uint32_t kBodyContents = 0x200c000; // 0x507b00: bodies a trace may start in
constexpr int kMaxClipPlanes = 8;           // 0x54eac0

// Per-move locals (the original's pml): frame time, ground state and the trace mask that 0x507b00 may narrow.
struct Pml {
    float frametime = 0;
    bool walking = false, groundPlane = false;
    TraceResult groundTrace;
    uint32_t mask = 0;
    int passEntity = -1;
    Vec mins{}, maxs{};
};

// 0x507c20 and the inline copies: v − n·(d − |d|·0.001) with d = v·n.
Vec clipVelocity(const Vec& v, const Vec& n) {
    float d = dot(v, n);
    d = -(d - std::fabs(d) * 0.001f);
    return {d * n[0] + v[0], d * n[1] + v[1], d * n[2] + v[2]};
}

// 0x507b00: the player trace; a trace that starts inside a body (contents 0x200c000) drops those contents from the
// mask for the rest of the move and is repeated.
TraceResult pmTrace(const CollisionWorld& w, Pml& pml, const Vec& start, const Vec& end) {
    TraceResult tr = w.trace(start, end, pml.mins, pml.maxs, pml.mask, pml.passEntity);
    if (tr.startSolid && (tr.contents & kBodyContents) != 0) {
        pml.mask &= ~kBodyContents;
        tr = w.trace(start, end, pml.mins, pml.maxs, pml.mask, pml.passEntity);
    }
    return tr;
}

Vec lerpTo(const Vec& a, const Vec& b, float f) { return {(b[0] - a[0]) * f + a[0], (b[1] - a[1]) * f + a[1], (b[2] - a[2]) * f + a[2]}; }

void noGround(PlayerState& ps, Pml& pml) {
    ps.onGround = false;
    pml.groundPlane = pml.walking = false;
}

// 0x5092f0: tries the 26 unit offsets of the table at 0x56f920; the first free position is taken and dropped by up
// to one unit, and that trace becomes the ground trace.
bool correctAllSolid(const CollisionWorld& w, PlayerState& ps, Pml& pml, TraceResult& tr) {
    static constexpr float kJitter[26][3] = {
        {0, 0, 1}, {-1, 0, 1}, {0, -1, 1}, {1, 0, 1}, {0, 1, 1}, {-1, 0, 0}, {0, -1, 0}, {1, 0, 0}, {0, 1, 0},
        {0, 0, -1}, {-1, 0, -1}, {0, -1, -1}, {1, 0, -1}, {0, 1, -1}, {-1, -1, 1}, {1, -1, 1}, {1, 1, 1},
        {-1, 1, 1}, {-1, -1, 0}, {1, -1, 0}, {1, 1, 0}, {-1, 1, 0}, {-1, -1, -1}, {1, -1, -1}, {1, 1, -1},
        {-1, 1, -1}};
    for (const auto& j : kJitter) {
        const Vec p{ps.origin[0] + j[0], ps.origin[1] + j[1], ps.origin[2] + j[2]};
        if (pmTrace(w, pml, p, p).startSolid) continue;
        ps.origin = p;
        const Vec down{p[0], p[1], p[2] - 1.0f};
        tr = pmTrace(w, pml, ps.origin, down);
        pml.groundTrace = tr;
        ps.origin = lerpTo(ps.origin, down, tr.fraction);
        return true;
    }
    noGround(ps, pml);
    ps.jumping = false;
    ps.jumpOriginZ = 0;
    return false;
}

// Ground trace (0x509480): a 0.25-unit trace above to below the origin. TODO(spec): the landing (fall damage and
// slowdown, 0x508fe0) when the player lands, ladders (pm_flags 0x20).
void groundTrace(const CollisionWorld& w, PlayerState& ps, Pml& pml) {
    Vec start{ps.origin[0], ps.origin[1], ps.origin[2] + kGroundTraceOffset};
    const Vec end{ps.origin[0], ps.origin[1], ps.origin[2] - kGroundTraceOffset};
    TraceResult tr = pmTrace(w, pml, start, end);
    pml.groundTrace = tr;
    if (tr.allSolid && !correctAllSolid(w, ps, pml, tr)) return;
    if (tr.startSolid) {
        // Retried from just below the (possibly corrected) origin, at the first trace's x and y.
        start[2] = ps.origin[2] - 0.001f;
        tr = pmTrace(w, pml, start, end);
        if (tr.startSolid) {
            noGround(ps, pml);
            return;
        }
        pml.groundTrace = tr;
    }
    if (tr.fraction == 1.0f) {
        noGround(ps, pml);
        return;
    }
    // Moving up and away from the plane: a jump, not ground.
    if (ps.velocity[2] > 0 && dot(ps.velocity, tr.normal) > 10.0f) {
        noGround(ps, pml);
        return;
    }
    if (tr.normal[2] < kMinWalkNormal) {
        ps.onGround = false;
        pml.groundPlane = true;
        pml.walking = false;
        ps.jumping = false;
        ps.jumpOriginZ = 0;
        return;
    }
    pml.groundPlane = pml.walking = true;
    ps.onGround = true;
}

// 0x54ea10: plane indices ordered by velocity·normal, ascending; returns the smallest.
float sortPlanes(const Vec& v, const Vec* planes, int count, int* order) {
    float d[kMaxClipPlanes + 1];
    for (int i = 0; i < count; ++i) {
        d[i] = dot(planes[i], v);
        int k = i;
        for (; k > 0 && d[i] <= d[order[k - 1]]; --k) order[k] = order[k - 1];
        order[k] = i;
    }
    return d[order[0]];
}

// Slide move (0x54eac0): up to 4 traces, clipping the velocity against up to 8 planes (the ground plane and the
// velocity direction first). With gravity the move uses the average of the start and end vertical speed.
// Returns true when something was hit.
bool slideMove(const CollisionWorld& w, PlayerState& ps, Pml& pml, bool gravity, const MoveParams& mp) {
    Vec endVel = ps.velocity;
    if (gravity) {
        endVel[2] = ps.velocity[2] - mp.gravity * pml.frametime;
        ps.velocity[2] = (endVel[2] + ps.velocity[2]) * 0.5f;
        if (pml.groundPlane) ps.velocity = clipVelocity(ps.velocity, pml.groundTrace.normal);
    }
    const Vec primal{endVel[0], endVel[1], endVel[2]};
    Vec planes[kMaxClipPlanes + 1];
    int numPlanes = 0;
    if (pml.groundPlane) planes[numPlanes++] = pml.groundTrace.normal;
    planes[numPlanes] = ps.velocity;
    normalize(planes[numPlanes++]);

    float timeLeft = pml.frametime;
    int bump = 0;
    for (; bump < 4; ++bump) {
        const Vec end = add(ps.origin, scale(ps.velocity, timeLeft));
        const TraceResult tr = pmTrace(w, pml, ps.origin, end);
        if (tr.allSolid) {
            ps.velocity[2] = 0;
            return true;
        }
        if (tr.fraction > 0) ps.origin = lerpTo(ps.origin, end, tr.fraction);
        if (tr.fraction == 1.0f) break;
        timeLeft -= tr.fraction * timeLeft;
        if (numPlanes >= kMaxClipPlanes) {
            ps.velocity = {0, 0, 0};
            return true;
        }
        // The same plane again: nudge the velocity off it.
        bool same = false;
        for (int i = 0; i < numPlanes && !same; ++i)
            if (dot(tr.normal, planes[i]) > 0.999f) {
                ps.velocity = add(clipVelocity(ps.velocity, tr.normal), tr.normal);
                same = true;
            }
        if (same) continue;
        planes[numPlanes++] = tr.normal;
        int order[kMaxClipPlanes + 1];
        if (sortPlanes(ps.velocity, planes, numPlanes, order) >= 0.1f) continue;
        const Vec& pi = planes[order[0]];
        Vec clip = clipVelocity(ps.velocity, pi), endClip = clipVelocity(endVel, pi);
        for (int k = 1; k < numPlanes; ++k) {
            const Vec& pj = planes[order[k]];
            if (dot(clip, pj) >= 0.1f) continue;
            clip = clipVelocity(clip, pj);
            endClip = clipVelocity(endClip, pj);
            if (dot(clip, pi) >= 0) continue;
            // Along the crease of both planes; stop when a third plane blocks that too.
            Vec dir = cross(pi, pj);
            normalize(dir);
            clip = scale(dir, dot(dir, ps.velocity));
            endClip = scale(dir, dot(dir, endVel));
            for (int m = 1; m < numPlanes; ++m)
                if (m != k && dot(clip, planes[order[m]]) < 0.1f) {
                    ps.velocity = {0, 0, 0};
                    return true;
                }
        }
        ps.velocity = clip;
        endVel = endClip;
    }
    if (gravity) ps.velocity = endVel;
    if (ps.pmTimeMs != 0) ps.velocity = primal;
    return bump != 0;
}

// 0x541270: a jump may step up to jump_stepSize, but not above the jump's apex (origin z + jump_height).
bool jumpStep(const PlayerState& ps, float startZ, float& stepSize, const MoveParams& mp) {
    const float apex = ps.jumpOriginZ + mp.jumpHeight;
    if (!(apex > startZ)) return false;
    stepSize = mp.jumpStepSize;
    if (mp.jumpStepSize + startZ > apex) stepSize = apex - startZ;
    return true;
}

// 0x541430: after a jump step the vertical speed may not carry the player above the apex.
void capJumpStep(PlayerState& ps, float startZ, const MoveParams& mp) {
    if (!(ps.origin[2] - startZ > 0)) return;
    const float left = mp.jumpHeight + ps.jumpOriginZ - ps.origin[2];
    if (left >= 0.1f) {
        const float v = std::sqrt((left + left) * mp.gravity);
        if (v < ps.velocity[2]) ps.velocity[2] = v;
    } else {
        ps.velocity[2] = 0;
    }
}

// Step slide move (0x54f2e0): the slide, then on a hit the same move from up to 18 units higher (10 prone), dropped
// back down; on the ground the result sticks to ground up to 9 units below. The step is kept when it got further
// along the velocity. TODO(spec): ladders (pm_flags 0x20), the step event 0x8f and footsteps (0x50a830, 0x50a6d0).
void stepSlideMove(const CollisionWorld& w, PlayerState& ps, Pml& pml, bool gravity, const MoveParams& mp) {
    const bool onGroundPlane = pml.groundPlane;
    if (!onGroundPlane && ps.jumping && ps.pmTimeMs != 0) {
        ps.jumping = false;
        ps.jumpOriginZ = 0;
    }
    const Vec startO = ps.origin, startV = ps.velocity;
    const bool hit = slideMove(w, ps, pml, gravity, mp);
    float stepSize = ps.stance == Stance::Prone ? kProneStepSize : kStepSize;
    bool jumpStepped = false;
    if (!ps.onGround) {
        if (ps.jumping && ps.pmTimeMs != 0) {
            ps.jumping = false;
            ps.jumpOriginZ = 0;
        }
        if (!hit || !ps.jumping || !jumpStep(ps, startO[2], stepSize, mp)) return;
        if (stepSize < 1.0f) return;
        jumpStepped = true;
    }
    const Vec downO = ps.origin, downV = ps.velocity;
    float up = 0;
    if (hit) {
        const Vec top{startO[0], startO[1], startO[2] + stepSize + 1.0f};
        const TraceResult tr = pmTrace(w, pml, startO, top);
        up = (stepSize + 1.0f) * tr.fraction - 1.0f;
        if (up >= 1.0f) {
            ps.origin = {startO[0], startO[1], startO[2] + up};
            ps.velocity = startV;
            slideMove(w, ps, pml, gravity, mp);
        } else {
            up = 0;
        }
    }
    if (onGroundPlane || up != 0) {
        Vec down{ps.origin[0], ps.origin[1], ps.origin[2] - up};
        if (onGroundPlane) down[2] -= kStickToGround;
        const TraceResult tr = pmTrace(w, pml, ps.origin, down);
        // The trace's hit id (trace+0x1c, `cmp word [esp+0x90], 1; jnc` at 0x54f5e2) is 0: keep the plain slide.
        // G_Trace (0x44bf00) sets it to 0x7ff when nothing was hit and 0x7fe for the world; an entity clip writes its
        // entity number, so only a hit on entity 0 takes this path. A clear trace lowers the origin by `up` below.
        if (tr.entity == 0) {
            ps.origin = downO;
            ps.velocity = downV;
            return;
        }
        if (tr.fraction >= 1.0f) {
            if (up != 0) ps.origin[2] -= up;
        } else {
            if (tr.normal[2] < kMinStepNormal) {
                ps.origin = downO;
                ps.velocity = downV;
                return;
            }
            ps.origin = lerpTo(ps.origin, down, tr.fraction);
            ps.velocity = clipVelocity(ps.velocity, tr.normal);
        }
    }
    const float stepped = (ps.origin[0] - startO[0]) * ps.velocity[0] + (ps.origin[1] - startO[1]) * ps.velocity[1];
    const float slid = (downO[0] - startO[0]) * ps.velocity[0] + (downO[1] - startO[1]) * ps.velocity[1];
    if (stepped <= slid + 0.001f || (jumpStepped && ps.jumpOriginZ + mp.jumpHeight < ps.origin[2])) {
        ps.origin = downO;
        ps.velocity = downV;
        if (onGroundPlane) {
            const Vec down{ps.origin[0], ps.origin[1], ps.origin[2] - kStickToGround};
            const TraceResult tr = pmTrace(w, pml, ps.origin, down);
            if (tr.fraction < 1.0f) {
                ps.origin = lerpTo(ps.origin, down, tr.fraction);
                ps.velocity = clipVelocity(ps.velocity, tr.normal);
            }
        }
    }
    if (jumpStepped) capJumpStep(ps, startO[2], mp);
    // Stairs (0x54f846): a height change of more than half a unit against the plain slide slows the player down.
    // TODO(spec): the prone surface test 0x4fc000 (0x54e990) that undoes a prone step.
    if (onGroundPlane) {
        const float dz = ps.origin[2] - downO[2];
        if (std::fabs(dz) > 0.5f && std::lrint(double(dz) + 9.313225746154785e-10) != 0) {
            const float f = (1.0f - std::fabs(ps.origin[2] - startO[2]) / stepSize) * 0.8f + 0.2f;
            ps.velocity = scale(ps.velocity, f);
        }
    }
}

// View-height keyframes {percent of the transition, height} (tables at 0x5a9088, 0x5a8fa8, 0x5a9130, 0x5a9018).
struct Key { int percent; float height; };
constexpr Key kToProne[] = {{0, 40}, {11, 38}, {22, 33}, {34, 25}, {45, 16}, {50, 15}, {55, 16}, {70, 18}, {90, 17}, {100, 11}};
constexpr Key kToCrouchFromStand[] = {{0, 60}, {1, 59.5f}, {4, 58.5f}, {30, 56}, {80, 44}, {90, 41.5f}, {95, 40.5f}, {100, 40}};
constexpr Key kToCrouchFromProne[] = {{0, 11}, {5, 10}, {30, 21}, {50, 25}, {67, 31}, {83, 34}, {100, 40}};
constexpr Key kToStand[] = {{0, 40}, {5, 40.5f}, {10, 41.5f}, {20, 44}, {70, 56}, {96, 58.5f}, {99, 59.5f}, {100, 60}};

template <size_t N>
float sampleKeys(const Key (&k)[N], int percent) {
    for (size_t i = 1; i < N; ++i)
        if (percent <= k[i].percent) {
            const float t = float(percent - k[i - 1].percent) / float(k[i].percent - k[i - 1].percent);
            return k[i - 1].height + (k[i].height - k[i - 1].height) * t;
        }
    return k[N - 1].height;
}

// Transition length in ms (0x5096a0): to prone 400; to crouch 200 from standing, 400 from prone; to stand 200.
int transitionMs(int target, bool down) { return target == 11 ? 400 : target == 40 ? (down ? 200 : 400) : 200; }

float viewHeightAt(int target, bool down, int percent) {
    if (target == 11) return sampleKeys(kToProne, percent);
    if (target == 40) return down ? sampleKeys(kToCrouchFromStand, percent) : sampleKeys(kToCrouchFromProne, percent);
    return sampleKeys(kToStand, percent);
}

float maxsZ(Stance s, const MoveParams& mp) { return s == Stance::Prone ? 30.0f : s == Stance::Crouch ? 50.0f : mp.maxs[2]; }

// Stance change (0x509d10): standing up needs room for the taller box; going prone needs ground
// (TODO(spec): the prone surface test 0x4fc000). A change starts a view-height transition (0x509840).
void updateStance(const CollisionWorld& w, PlayerState& ps, const PlayerInput& in, const MoveParams& mp) {
    Stance want = in.stance;
    if (want == ps.stance) return;
    // Moves go one step at a time between stand, crouch and prone.
    if (int(want) > int(ps.stance) + 1) want = Stance(int(ps.stance) + 1);
    if (int(want) < int(ps.stance) - 1) want = Stance(int(ps.stance) - 1);
    if (int(want) < int(ps.stance)) {
        const Vec maxs{mp.maxs[0], mp.maxs[1], maxsZ(want, mp)};
        const TraceResult tr = w.trace(ps.origin, ps.origin, mp.mins, maxs, Contents::PlayerSolid);
        if (tr.startSolid || tr.allSolid) return;
    } else if (want == Stance::Prone && !ps.onGround) {
        return;
    }
    const int target = want == Stance::Prone ? 11 : want == Stance::Crouch ? 40 : 60;
    ps.transitionDown = target < ps.viewHeightTarget;
    ps.viewHeightTarget = target;
    ps.transitionStartMs = ps.timeMs;
    ps.stance = want;
}

void updateViewHeight(PlayerState& ps) {
    if (ps.transitionStartMs < 0) {
        ps.viewHeight = float(ps.viewHeightTarget);
        return;
    }
    const int len = transitionMs(ps.viewHeightTarget, ps.transitionDown);
    const int percent = int(std::clamp<int64_t>((ps.timeMs - ps.transitionStartMs) * 100 / len, 0, 100));
    ps.viewHeight = viewHeightAt(ps.viewHeightTarget, ps.transitionDown, percent);
    if (percent >= 100) ps.transitionStartMs = -1;
}

// Command scale (0x508140, 0x5080a0): g_speed · max(|forward| (×backSpeedScale when backwards),
// |right|·strafeSpeedScale) / (|command| · 127), then ×0.65 crouched, ×0.15 prone, × the weapon's
// moveSpeedScale. TODO(spec): the blend during crouch/prone transitions (0x509760) and the ×0.4 cases.
float cmdScale(const PlayerState& ps, float f, float r, const MoveParams& mp, bool walking) {
    const float fb = f < 0 && walking ? f * mp.backSpeedScale : f;
    const float rs = walking ? std::fabs(r * mp.strafeSpeedScale) : std::fabs(r);
    const float m = std::max(std::fabs(fb), rs);
    if (m == 0) return 0;
    float s = float(mp.speed) * m / (std::sqrt(f * f + r * r) * 127.0f);
    if (walking) {
        if (ps.stance == Stance::Prone) s *= 0.15f;
        else if (ps.stance == Stance::Crouch) s *= 0.65f;
        if (mp.weaponSpeedScale > 0) s *= mp.weaponSpeedScale;
    }
    if (ps.lean != 0) s *= 0.4f; // a nonzero leanf (ps+0x4c) slows movement (0x508140)
    return s;
}

// Friction (0x507c80): control = max(speed, stopspeed); drop = control · friction · frametime on the ground.
void applyFriction(PlayerState& ps, float dt, const MoveParams& mp) {
    Vec v = ps.velocity;
    if (ps.onGround) v[2] = 0;
    const float speed = std::sqrt(dot(v, v));
    if (speed < 1.0f) {
        ps.velocity = {0, 0, ps.onGround ? 0.0f : ps.velocity[2]};
        return;
    }
    float drop = 0;
    if (ps.onGround) drop = std::max(speed, mp.stopSpeed) * mp.friction * dt;
    const float ns = std::max(0.0f, speed - drop) / speed;
    ps.velocity = scale(ps.velocity, ns);
}

// Accelerate (0x507eb0): accelspeed = max(wishspeed, stopspeed) · frametime · accel, capped at the missing speed.
// TODO(spec): the inertiaMax limit of 0x507e50.
void accelerate(PlayerState& ps, const Vec& wishDir, float wishSpeed, float accel, float dt, const MoveParams& mp) {
    const float add = wishSpeed - dot(ps.velocity, wishDir);
    if (add <= 0) return;
    const float accelSpeed = std::min(add, std::max(wishSpeed, mp.stopSpeed) * dt * accel);
    ps.velocity = ::iw2::game::add(ps.velocity, scale(wishDir, accelSpeed));
}

// Lean (0x50ade0): the target is −1 (left), +1 (right) or 0; only on the ground. The limit is 0.5, 0.25 at
// view height 11. The value moves towards the target by ms · max/350 and back to 0 by ms · max/280. A 16-unit cube
// traced from the eye to the leaned eye limits it to 1 − sqrt(1 − fraction).
// TODO(spec): the disabling ps flags (0x8000, 0x2000000/0x4000000 for allowleanleft/right, 0x20300 in ps+0x7c).
void updateLean(const CollisionWorld& w, PlayerState& ps, const PlayerInput& in, float ms, float yaw) {
    int dir = 0;
    if (ps.onGround) {
        if (in.leanLeft) dir -= 1;
        if (in.leanRight) dir += 1;
    }
    const float maxLean = ps.viewHeightTarget == 11 && ps.viewHeight <= 11.0f ? 0.25f : 0.5f;
    float f = ps.lean;
    if (dir == 0) {
        if (f > 0) f = std::max(0.0f, f - ms * (1.0f / 280.0f) * maxLean);
        else if (f < 0) f = std::min(0.0f, f + ms * (1.0f / 280.0f) * maxLean);
    } else if (dir < 0) {
        if (f > -maxLean) f -= ms * (1.0f / 350.0f) * maxLean;
        f = std::max(f, -maxLean);
    } else {
        if (f < maxLean) f += ms * (1.0f / 350.0f) * maxLean;
        f = std::min(f, maxLean);
    }
    ps.lean = f;
    if (f != 0) {
        const Vec eye{ps.origin[0], ps.origin[1], ps.origin[2] + ps.viewHeight};
        const Vec off = leanOffset(yaw, f);
        const TraceResult tr = w.trace(eye, add(eye, off), {-8, -8, -8}, {8, 8, 8}, Contents::PlayerSolid);
        const float limit = 1.0f - std::sqrt(std::max(0.0f, 1.0f - tr.fraction));
        if (limit < std::fabs(ps.lean)) ps.lean = ps.lean < 0 ? -limit : limit;
    }
}

// Jump (0x5416b0, 0x5414d0): standing on the ground, newly pressed, 500 ms after the previous jump; vertical speed
// sqrt(2 · jump_height · gravity). Leaves the ground and remembers the jump's origin z (ps+0x74).
bool checkJump(PlayerState& ps, Pml& pml, const PlayerInput& in, const MoveParams& mp) {
    const bool jumpPressed = in.jump && !ps.jumpHeld;
    ps.jumpHeld = in.jump;
    if (!jumpPressed || ps.stance != Stance::Stand || ps.viewHeightTarget != 60 || ps.timeMs - ps.lastJumpMs <= 499) return false;
    pml.groundPlane = pml.walking = false;
    ps.onGround = false;
    ps.lastJumpMs = ps.timeMs;
    ps.jumpOriginZ = ps.origin[2];
    ps.pmTimeMs = 0;
    ps.jumping = true;
    ps.velocity[2] = std::sqrt(2.0f * mp.jumpHeight * mp.gravity);
    return true;
}

// Landing timer (0x541300, called by the walk move while pm_flags 0x80000 is set): 1200 ms after landing at least
// 18 units above the jump's origin, else 1800 ms; the flag is dropped once the timer reads above 1800. The speed
// factors (0.5, 0.65) only apply with jump_slowdownEnable, which defaults to false.
void landingTimer(PlayerState& ps) {
    if (ps.pmTimeMs > 1800) {
        ps.jumping = false;
        ps.jumpOriginZ = 0;
    } else if (ps.pmTimeMs == 0) {
        ps.pmTimeMs = ps.jumpOriginZ + 18.0f <= ps.origin[2] ? 1200 : 1800;
    }
}

Vec wishVelocity(const Vec& fwd, const Vec& right, float f, float r) { return add(scale(fwd, f), scale(right, r)); }

void airMove(const CollisionWorld& w, PlayerState& ps, Pml& pml, float f, float r, const Vec& fwd, const Vec& right,
             const MoveParams& mp) {
    // Air move (0x5085b0): horizontal wish direction, acceleration 1, clipped against a steep ground plane.
    applyFriction(ps, pml.frametime, mp);
    const float s = cmdScale(ps, f, r, mp, false);
    Vec wish = wishVelocity(fwd, right, f, r);
    const float wishSpeed = normalize(wish) * s;
    accelerate(ps, wish, wishSpeed, 1.0f, pml.frametime, mp);
    if (pml.groundPlane) ps.velocity = clipVelocity(ps.velocity, pml.groundTrace.normal);
    stepSlideMove(w, ps, pml, true, mp);
}

void walkMove(const CollisionWorld& w, PlayerState& ps, Pml& pml, const PlayerInput& in, float f, float r, Vec fwd,
              Vec right, const MoveParams& mp) {
    // Walk move (0x5086f0).
    if (ps.jumping) landingTimer(ps);
    if (checkJump(ps, pml, in, mp)) {
        airMove(w, ps, pml, f, r, fwd, right, mp);
        return;
    }
    applyFriction(ps, pml.frametime, mp);
    const float s = cmdScale(ps, f, r, mp, true);
    // The movement axes follow the ground plane.
    const Vec& n = pml.groundTrace.normal;
    fwd = clipVelocity(fwd, n);
    right = clipVelocity(right, n);
    normalize(fwd);
    normalize(right);
    Vec wish = wishVelocity(fwd, right, f, r);
    const float wishSpeed = normalize(wish) * s;
    // Acceleration 9 standing, 12 crouched, 19 prone, 1 on slick ground (surface flag 2). TODO(spec): ×0.25 with
    // pm_flags 0x200 and knockback (0x400), both set by the landing code that is not implemented.
    const bool slick = (pml.groundTrace.surfaceFlags & 2) != 0;
    const float accel = slick ? 1.0f : ps.stance == Stance::Prone ? 19.0f : ps.stance == Stance::Crouch ? 12.0f : 9.0f;
    accelerate(ps, wish, wishSpeed, accel, pml.frametime, mp);
    if (slick) ps.velocity[2] -= mp.gravity * pml.frametime;
    // Along the ground plane, keeping the speed.
    const float speed = std::sqrt(dot(ps.velocity, ps.velocity));
    const Vec old = ps.velocity;
    ps.velocity = clipVelocity(ps.velocity, n);
    if (dot(old, ps.velocity) > 0) {
        normalize(ps.velocity);
        ps.velocity = scale(ps.velocity, speed);
    }
    if (ps.velocity[0] != 0 || ps.velocity[1] != 0) stepSlideMove(w, ps, pml, false, mp);
}

// One PmoveSingle step (0x50c4a0) for a normal player: ground trace, the walk or air move, ground trace.
void moveStep(const CollisionWorld& w, PlayerState& ps, const PlayerInput& in, int ms, const MoveParams& mp) {
    Pml pml;
    pml.frametime = float(ms) * 0.001f;
    pml.mask = mp.traceMask;
    pml.passEntity = mp.passEntity;
    pml.mins = mp.mins;
    pml.maxs = {mp.maxs[0], mp.maxs[1], maxsZ(ps.stance, mp)};
    groundTrace(w, ps, pml);
    updateLean(w, ps, in, float(ms), in.yaw);
    updateStance(w, ps, in, mp);
    updateViewHeight(ps);
    pml.maxs[2] = maxsZ(ps.stance, mp);

    // pm_time runs down; when it expires pm_flags 0x80000, 0x400 and 0x200 are cleared.
    if (ps.pmTimeMs != 0) {
        if (ms < ps.pmTimeMs) {
            ps.pmTimeMs -= ms;
        } else {
            ps.pmTimeMs = 0;
            ps.jumping = false;
        }
    }

    const float yaw = in.yaw * float(M_PI) / 180.0f;
    const Vec fwd{std::cos(yaw), std::sin(yaw), 0}, right{std::sin(yaw), -std::cos(yaw), 0};
    const float f = std::clamp(in.forward, -1.0f, 1.0f) * 127.0f, r = std::clamp(in.right, -1.0f, 1.0f) * 127.0f;
    if (pml.walking) walkMove(w, ps, pml, in, f, r, fwd, right, mp);
    else airMove(w, ps, pml, f, r, fwd, right, mp);
    groundTrace(w, ps, pml);
}

} // namespace

Vec leanOffset(float yawDeg, float lean) {
    if (lean == 0) return {0, 0, 0};
    const float g = (2.0f - std::fabs(lean)) * lean;
    const float roll = g * 16.0f * float(M_PI) / 180.0f;
    const float yaw = yawDeg * float(M_PI) / 180.0f;
    const float d = g * 20.0f;
    return {std::cos(roll) * std::sin(yaw) * d, -std::cos(roll) * std::cos(yaw) * d, -std::sin(roll) * d};
}

void playerMove(const CollisionWorld& w, PlayerState& ps, const PlayerInput& in, float dt, const MoveParams& mp) {
    // Pmove (0x50cc10) runs steps of at most 66 ms; PmoveSingle (0x50c4a0) clamps a step to 1..200 ms.
    int ms = std::max(1, int(std::lround(dt * 1000.0f)));
    while (ms > 0) {
        const int step = std::min(ms, 66);
        ps.timeMs += step;
        moveStep(w, ps, in, step, mp);
        ms -= step;
    }
}

} // namespace iw2::game
