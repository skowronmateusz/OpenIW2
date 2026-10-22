// Player movement. Rules marked with an address follow CoD2SP_s.exe 1.3 (docs/spec/pmove.md); the ground trace,
// slide and step code is still the id Tech 3 style approximation (TODO(spec): 0x54f2e0, 0x50bd90, 0x508390).
#include "game/pmove.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace iw2::game {
namespace {

float dot(const Vec& a, const Vec& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec add(const Vec& a, const Vec& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec scale(const Vec& a, float s) { return {a[0] * s, a[1] * s, a[2] * s}; }

constexpr float kMinWalkNormal = 0.7f;
constexpr float kOverclip = 1.001f;

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

Vec clipVelocity(const Vec& v, const Vec& n) {
    float backoff = dot(v, n);
    backoff = backoff < 0 ? backoff * kOverclip : backoff / kOverclip;
    return {v[0] - n[0] * backoff, v[1] - n[1] * backoff, v[2] - n[2] * backoff};
}

bool slideMove(const CollisionWorld& w, PlayerState& ps, float dt, const Vec& mins, const Vec& maxs) {
    Vec planes[5];
    int numPlanes = 0;
    float timeLeft = dt;
    for (int bump = 0; bump < 4 && timeLeft > 0; ++bump) {
        const Vec end = add(ps.origin, scale(ps.velocity, timeLeft));
        const TraceResult tr = w.trace(ps.origin, end, mins, maxs, Contents::PlayerSolid);
        if (tr.allSolid) {
            ps.velocity[2] = 0;
            return true;
        }
        if (tr.fraction > 0) ps.origin = tr.endPos;
        if (tr.fraction == 1.0f) return bump > 0;
        timeLeft -= timeLeft * tr.fraction;
        if (numPlanes >= 5) {
            ps.velocity = {0, 0, 0};
            return true;
        }
        planes[numPlanes++] = tr.normal;
        for (int i = 0; i < numPlanes; ++i) {
            if (dot(ps.velocity, planes[i]) >= 0.1f) continue;
            Vec clipped = clipVelocity(ps.velocity, planes[i]);
            for (int j = 0; j < numPlanes; ++j) {
                if (j == i || dot(clipped, planes[j]) >= 0.1f) continue;
                clipped = clipVelocity(clipped, planes[j]);
                if (dot(clipped, planes[i]) >= 0) continue;
                const Vec dir{planes[i][1] * planes[j][2] - planes[i][2] * planes[j][1],
                              planes[i][2] * planes[j][0] - planes[i][0] * planes[j][2],
                              planes[i][0] * planes[j][1] - planes[i][1] * planes[j][0]};
                clipped = scale(dir, dot(dir, ps.velocity));
            }
            ps.velocity = clipped;
            break;
        }
    }
    return true;
}

void groundTrace(const CollisionWorld& w, PlayerState& ps, const Vec& mins, const Vec& maxs) {
    const Vec down = add(ps.origin, {0, 0, -0.25f});
    const TraceResult tr = w.trace(ps.origin, down, mins, maxs, Contents::PlayerSolid);
    ps.onGround = tr.fraction < 1.0f && tr.normal[2] >= kMinWalkNormal && ps.velocity[2] <= 10.0f;
    if (ps.onGround && tr.fraction > 0) ps.origin = tr.endPos;
}

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

void moveStep(const CollisionWorld& w, PlayerState& ps, const PlayerInput& in, float dt, const MoveParams& mp) {
    const Vec maxs{mp.maxs[0], mp.maxs[1], maxsZ(ps.stance, mp)};
    groundTrace(w, ps, mp.mins, maxs);
    updateLean(w, ps, in, dt * 1000.0f, in.yaw);
    updateStance(w, ps, in, mp);
    updateViewHeight(ps);

    const float yaw = in.yaw * float(M_PI) / 180.0f;
    const Vec fwd{std::cos(yaw), std::sin(yaw), 0}, right{std::sin(yaw), -std::cos(yaw), 0};
    const float f = std::clamp(in.forward, -1.0f, 1.0f) * 127.0f, r = std::clamp(in.right, -1.0f, 1.0f) * 127.0f;

    // Jump (0x5416b0, 0x5414d0): standing on the ground, newly pressed, 500 ms after the previous jump;
    // vertical speed sqrt(2 · jump_height · gravity).
    const bool jumpPressed = in.jump && !ps.jumpHeld;
    ps.jumpHeld = in.jump;
    if (ps.onGround && jumpPressed && ps.stance == Stance::Stand && ps.viewHeightTarget == 60 &&
        ps.timeMs - ps.lastJumpMs > 499) {
        ps.velocity[2] = std::sqrt(2.0f * mp.jumpHeight * mp.gravity);
        ps.onGround = false;
        ps.lastJumpMs = ps.timeMs;
    }

    applyFriction(ps, dt, mp);
    const bool walking = ps.onGround;
    const float s = cmdScale(ps, f, r, mp, walking);
    Vec wish = add(scale(fwd, f), scale(right, r));
    float wishSpeed = std::sqrt(dot(wish, wish));
    if (wishSpeed > 0) wish = scale(wish, 1.0f / wishSpeed);
    wishSpeed *= s;
    // Walk acceleration 9 standing, 12 crouched, 19 prone (0x5086f0); air acceleration 1 (0x5085b0).
    const float accel = walking ? (ps.stance == Stance::Prone ? 19.0f : ps.stance == Stance::Crouch ? 12.0f : 9.0f) : 1.0f;
    accelerate(ps, wish, wishSpeed, accel, dt, mp);
    if (walking) ps.velocity[2] = 0;
    else ps.velocity[2] -= mp.gravity * dt;

    const PlayerState before = ps;
    const bool blocked = slideMove(w, ps, dt, mp.mins, maxs);
    if (blocked && before.onGround) {
        PlayerState up = before;
        const TraceResult lift = w.trace(up.origin, add(up.origin, {0, 0, mp.stepHeight}), mp.mins, maxs, Contents::PlayerSolid);
        if (!lift.allSolid) {
            up.origin = lift.endPos;
            slideMove(w, up, dt, mp.mins, maxs);
            const TraceResult drop = w.trace(up.origin, add(up.origin, {0, 0, -(mp.stepHeight + 1)}), mp.mins, maxs, Contents::PlayerSolid);
            if (!drop.allSolid) up.origin = drop.endPos;
            const float d1 = (ps.origin[0] - before.origin[0]) * (ps.origin[0] - before.origin[0]) + (ps.origin[1] - before.origin[1]) * (ps.origin[1] - before.origin[1]);
            const float d2 = (up.origin[0] - before.origin[0]) * (up.origin[0] - before.origin[0]) + (up.origin[1] - before.origin[1]) * (up.origin[1] - before.origin[1]);
            if (d2 > d1 && drop.normal[2] >= kMinWalkNormal) {
                ps.origin = up.origin;
                ps.velocity = up.velocity;
                ps.velocity[2] = 0;
            }
        }
    }
    groundTrace(w, ps, mp.mins, maxs);
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
        moveStep(w, ps, in, float(step) * 0.001f, mp);
        ms -= step;
    }
}

} // namespace iw2::game
