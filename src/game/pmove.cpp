#include "game/pmove.h"

#include <algorithm>
#include <cmath>

namespace iw2::game {
namespace {

float dot(const Vec& a, const Vec& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec add(const Vec& a, const Vec& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec scale(const Vec& a, float s) { return {a[0] * s, a[1] * s, a[2] * s}; }

constexpr float kMinWalkNormal = 0.7f;
constexpr float kOverclip = 1.001f;

Vec clipVelocity(const Vec& v, const Vec& n) {
    float backoff = dot(v, n);
    backoff = backoff < 0 ? backoff * kOverclip : backoff / kOverclip;
    return {v[0] - n[0] * backoff, v[1] - n[1] * backoff, v[2] - n[2] * backoff};
}

// Moves along the velocity, sliding along up to four planes. Returns true if blocked.
bool slideMove(const CollisionWorld& w, PlayerState& ps, float dt, const MoveParams& mp) {
    Vec planes[5];
    int numPlanes = 0;
    float timeLeft = dt;
    for (int bump = 0; bump < 4 && timeLeft > 0; ++bump) {
        const Vec end = add(ps.origin, scale(ps.velocity, timeLeft));
        const TraceResult tr = w.trace(ps.origin, end, mp.mins, mp.maxs, Contents::PlayerSolid);
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
                // Slide along the crease of the two planes.
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

void groundTrace(const CollisionWorld& w, PlayerState& ps, const MoveParams& mp) {
    const Vec down = add(ps.origin, {0, 0, -0.25f});
    const TraceResult tr = w.trace(ps.origin, down, mp.mins, mp.maxs, Contents::PlayerSolid);
    ps.onGround = tr.fraction < 1.0f && tr.normal[2] >= kMinWalkNormal && ps.velocity[2] <= 10.0f;
    if (ps.onGround && tr.fraction > 0) ps.origin = tr.endPos;
}

} // namespace

void playerMove(const CollisionWorld& w, PlayerState& ps, const PlayerInput& in, float dt, const MoveParams& mp) {
    groundTrace(w, ps, mp);

    const float yaw = in.yaw * float(M_PI) / 180.0f;
    const Vec fwd{std::cos(yaw), std::sin(yaw), 0}, right{std::sin(yaw), -std::cos(yaw), 0};
    Vec wish = add(scale(fwd, in.forward), scale(right, in.right));
    float wishSpeed = std::sqrt(dot(wish, wish));
    if (wishSpeed > 0) wish = scale(wish, 1.0f / wishSpeed);
    wishSpeed = std::min(wishSpeed, 1.0f) * mp.speed * (in.sprint ? mp.sprintScale : 1.0f);

    if (ps.onGround) {
        if (in.jump) {
            ps.velocity[2] = mp.jumpSpeed;
            ps.onGround = false;
        } else {
            // Friction
            const float speed = std::sqrt(ps.velocity[0] * ps.velocity[0] + ps.velocity[1] * ps.velocity[1]);
            if (speed > 0) {
                const float drop = std::max(speed, 100.0f) * mp.friction * dt;
                const float ns = std::max(0.0f, speed - drop) / speed;
                ps.velocity[0] *= ns;
                ps.velocity[1] *= ns;
            }
            ps.velocity[2] = 0;
        }
    }

    // Accelerate towards the wish direction.
    const float current = dot(ps.velocity, wish);
    const float addSpeed = wishSpeed - current;
    if (addSpeed > 0) {
        const float accel = std::min(addSpeed, (ps.onGround ? mp.accelerate : mp.airAccelerate) * dt * wishSpeed);
        ps.velocity = add(ps.velocity, scale(wish, accel));
    }

    if (!ps.onGround) ps.velocity[2] -= mp.gravity * dt;

    // Step slide: try the plain move and a move lifted by the step height; keep the one that went further.
    const PlayerState before = ps;
    const bool blocked = slideMove(w, ps, dt, mp);
    if (blocked && before.onGround) {
        PlayerState up = before;
        const TraceResult lift = w.trace(up.origin, add(up.origin, {0, 0, mp.stepHeight}), mp.mins, mp.maxs, Contents::PlayerSolid);
        if (!lift.allSolid) {
            up.origin = lift.endPos;
            slideMove(w, up, dt, mp);
            const TraceResult drop = w.trace(up.origin, add(up.origin, {0, 0, -(mp.stepHeight + 1)}), mp.mins, mp.maxs, Contents::PlayerSolid);
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
    groundTrace(w, ps, mp);
}

} // namespace iw2::game
