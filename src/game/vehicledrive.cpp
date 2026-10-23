// Vehicle movement without a path. Rules follow CoD2SP_s.exe 1.3 (docs/spec/vehicles.md).
#include "game/vehicledrive.h"

#include <algorithm>
#include <cmath>

namespace iw2::game {

namespace {

constexpr float kFrameTime = 0.05f;   // 0x591e54
constexpr float kInvFrameTime = 20.0f; // 0x591e18
constexpr float kInvCmd = 1.0f / 127.0f; // 0x5920d8
constexpr float kMinWalkNormal = 0.7f; // 0x591f30
constexpr float kStepSize = 18.0f;     // 0x591f44
constexpr float kGravityStep = 40.0f;  // 0x591f78: vertical speed lost per frame

float dot(const Vec& a, const Vec& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec scale(const Vec& a, float s) { return {a[0] * s, a[1] * s, a[2] * s}; }
Vec cross(const Vec& a, const Vec& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
Vec lerpTo(const Vec& a, const Vec& b, float f) { return {(b[0] - a[0]) * f + a[0], (b[1] - a[1]) * f + a[1], (b[2] - a[2]) * f + a[2]}; }

// Normalizes in place and returns the original length (0x4210d0).
float normalize(Vec& v) {
    const float len = std::sqrt(dot(v, v));
    if (len != 0.0f) v = scale(v, 1.0f / len);
    return len;
}

// 16-bit angle quantization into (-180, 180] (0x424040).
float angleNormalize180(float a) {
    const int32_t i = int32_t(a * 182.044449f) & 0xffff;
    float r = float(i) * 0.0054931640625f;
    if (r > 180.0f) r -= 360.0f;
    return r;
}

// a − b wrapped into [-180, 180] (0x423f50).
float angleDelta(float a, float b) {
    float d = a - b;
    while (d > 180.0f) d -= 360.0f;
    while (d < -180.0f) d += 360.0f;
    return d;
}

// Ground state of one move (globals 0x1c7f894 trace, 0x1c7f8b8 ground plane, 0x1c7f8bc walking).
struct Ground {
    TraceResult trace;
    bool plane = false, walking = false;
};

TraceResult traceMove(const CollisionWorld& w, const DriveTrace& t, const Vec& start, const Vec& end) {
    return w.trace(start, end, {0, 0, 0}, {0, 0, 0}, t.mask, t.passEntity);
}

// 0x50e3a0: the first of the 26 unit offsets (table 0x56fb90, the same as the player's) that is not inside a solid
// becomes the origin, dropped by up to one unit; that trace replaces the ground trace.
bool unstick(const CollisionWorld& w, const DriveTrace& t, DriveBody& b, TraceResult& tr) {
    static constexpr float kJitter[26][3] = {
        {0, 0, 1}, {-1, 0, 1}, {0, -1, 1}, {1, 0, 1}, {0, 1, 1}, {-1, 0, 0}, {0, -1, 0}, {1, 0, 0}, {0, 1, 0},
        {0, 0, -1}, {-1, 0, -1}, {0, -1, -1}, {1, 0, -1}, {0, 1, -1}, {-1, -1, 1}, {1, -1, 1}, {1, 1, 1},
        {-1, 1, 1}, {-1, -1, 0}, {1, -1, 0}, {1, 1, 0}, {-1, 1, 0}, {-1, -1, -1}, {1, -1, -1}, {1, 1, -1},
        {-1, 1, -1}};
    for (const auto& j : kJitter) {
        const Vec p{b.origin[0] + j[0], b.origin[1] + j[1], b.origin[2] + j[2]};
        if (traceMove(w, t, p, p).startSolid) continue;
        b.origin = p;
        const Vec down{p[0], p[1], p[2] - 1.0f};
        tr = traceMove(w, t, b.origin, down);
        b.origin = lerpTo(b.origin, down, tr.fraction);
        return true;
    }
    return false;
}

// Ground trace (0x50e4f0): 0.25 units above to below the origin. A hit is ground unless the vehicle moves up and
// away from the plane (vertical speed > 0 and velocity·normal > 10); walkable from normal z 0.7.
Ground groundTrace(const CollisionWorld& w, const DriveTrace& t, DriveBody& b) {
    Ground g;
    const Vec start{b.origin[0], b.origin[1], b.origin[2] + 0.25f}, end{b.origin[0], b.origin[1], b.origin[2] - 0.25f};
    g.trace = traceMove(w, t, start, end);
    if (g.trace.allSolid && !unstick(w, t, b, g.trace)) return g;
    if (g.trace.fraction != 1.0f && (b.velocity[2] <= 0.0f || dot(g.trace.normal, b.velocity) <= 10.0f)) {
        g.plane = true;
        g.walking = g.trace.normal[2] >= kMinWalkNormal;
    }
    return g;
}

// Slide move (0x50e630): up to 4 traces against at most 5 planes (the ground plane and the velocity direction
// first). With gravity the vertical speed drops by 40 over the frame and the move uses the average. Returns
// whether anything was hit.
bool slideMove(const CollisionWorld& w, const DriveTrace& t, DriveBody& b, const Ground& g, bool gravity) {
    Vec endVelocity = b.velocity;
    float timeLeft = kFrameTime;
    if (gravity) {
        endVelocity[2] -= kGravityStep;
        b.velocity[2] = (endVelocity[2] + b.velocity[2]) * 0.5f;
        if (g.plane) b.velocity = vehicleClipVelocity(b.velocity, g.trace.normal);
    }
    Vec planes[5];
    int numPlanes = 0;
    if (g.plane) planes[numPlanes++] = g.trace.normal;
    planes[numPlanes] = b.velocity;
    normalize(planes[numPlanes]);
    ++numPlanes;
    int bump = 0;
    for (; bump < 4; ++bump) {
        const Vec end{b.origin[0] + b.velocity[0] * timeLeft, b.origin[1] + b.velocity[1] * timeLeft,
                      b.origin[2] + b.velocity[2] * timeLeft};
        const TraceResult tr = traceMove(w, t, b.origin, end);
        if (tr.startSolid) {
            b.velocity[2] = 0.0f;
            return true;
        }
        if (tr.fraction > 0.0f) b.origin = lerpTo(b.origin, end, tr.fraction);
        if (tr.fraction == 1.0f) break;
        timeLeft -= timeLeft * tr.fraction;
        if (numPlanes >= 5) {
            b.velocity = {0, 0, 0};
            return true;
        }
        // The same plane again: nudge the velocity out along its normal.
        int i = 0;
        for (; i < numPlanes; ++i)
            if (dot(tr.normal, planes[i]) > 0.99f) break;
        if (i < numPlanes) {
            for (int k = 0; k < 3; ++k) b.velocity[size_t(k)] += tr.normal[size_t(k)];
            continue;
        }
        planes[numPlanes++] = tr.normal;
        for (i = 0; i < numPlanes; ++i) {
            if (dot(b.velocity, planes[i]) >= 0.1f) continue;
            Vec clip = vehicleClipVelocity(b.velocity, planes[i]);
            Vec endClip = vehicleClipVelocity(endVelocity, planes[i]);
            for (int j = 0; j < numPlanes; ++j) {
                if (j == i || dot(clip, planes[j]) >= 0.1f) continue;
                clip = vehicleClipVelocity(clip, planes[j]);
                endClip = vehicleClipVelocity(endClip, planes[j]);
                if (dot(clip, planes[i]) >= 0.0f) continue;
                Vec dir = cross(planes[i], planes[j]);
                normalize(dir);
                clip = scale(dir, dot(dir, b.velocity));
                endClip = scale(dir, dot(dir, endVelocity));
                for (int k = 0; k < numPlanes; ++k) {
                    if (k == i || k == j) continue;
                    if (dot(clip, planes[k]) < 0.1f) {
                        b.velocity = {0, 0, 0};
                        return true;
                    }
                }
            }
            b.velocity = clip;
            endVelocity = endClip;
            break;
        }
    }
    if (gravity) b.velocity = endVelocity;
    return bump != 0;
}

// Step slide move (0x50ebe0): after a blocked slide, the same move from up to 18 units higher, dropped back down;
// not while moving up off non-ground below the start.
void stepSlideMove(const CollisionWorld& w, const DriveTrace& t, DriveBody& b, const Ground& g, bool gravity) {
    const Vec startOrigin = b.origin, startVelocity = b.velocity;
    if (!slideMove(w, t, b, g, gravity)) return;
    const Vec down{startOrigin[0], startOrigin[1], startOrigin[2] - kStepSize};
    TraceResult tr = traceMove(w, t, startOrigin, down);
    if (b.velocity[2] > 0.0f && (tr.fraction == 1.0f || tr.normal[2] < kMinWalkNormal)) return;
    const Vec up{startOrigin[0], startOrigin[1], startOrigin[2] + kStepSize};
    tr = traceMove(w, t, startOrigin, up);
    if (tr.startSolid) return;
    const float stepSize = (up[2] - startOrigin[2]) * tr.fraction;
    b.origin = lerpTo(startOrigin, up, tr.fraction);
    b.velocity = startVelocity;
    slideMove(w, t, b, g, gravity);
    const Vec drop{b.origin[0], b.origin[1], b.origin[2] - stepSize};
    tr = traceMove(w, t, b.origin, drop);
    if (!tr.startSolid) b.origin = lerpTo(b.origin, drop, tr.fraction);
    if (tr.fraction < 1.0f) b.velocity = vehicleClipVelocity(b.velocity, tr.normal);
}

// Move along walkable ground (0x50ee50): the velocity is clipped to the ground plane keeping its speed, then a
// step slide without gravity when there is horizontal movement left.
void groundMove(const CollisionWorld& w, const DriveTrace& t, DriveBody& b, const Ground& g) {
    const Vec old = b.velocity;
    const float speed = std::sqrt(dot(old, old));
    b.velocity = vehicleClipVelocity(b.velocity, g.trace.normal);
    if (dot(old, b.velocity) > 0.0f) {
        normalize(b.velocity);
        b.velocity = scale(b.velocity, speed);
    }
    if (b.velocity[0] != 0.0f || b.velocity[1] != 0.0f) stepSlideMove(w, t, b, g, false);
}

} // namespace

Vec vehicleClipVelocity(const Vec& v, const Vec& n) {
    if (n[2] >= kMinWalkNormal && v[2] * v[2] <= v[0] * v[0] + v[1] * v[1])
        return {v[0] * n[2], v[1] * n[2], -(v[0] * n[0]) - n[1] * v[1]};
    float d = dot(v, n);
    d *= d < 0.0f ? 1.01f : 0.99f;
    return {v[0] - d * n[0], v[1] - d * n[1], v[2] - d * n[2]};
}

void driveAccelerations(const DriveParams& p, const DriveBody& b, const DriveCommand& cmd, Vec& accel, Vec& angularAccel) {
    const Vec& lv = b.localVelocity;
    const Vec& av = b.angularVelocity;
    // Forward: the target speed grows by one frame of acceleration in the pressed direction, scaled by the command.
    float target = 0.0f;
    const float fwdScale = std::fabs(float(cmd.forward)) * kInvCmd;
    if (cmd.forward > 0) target = (p.accel * kFrameTime + lv[0]) * fwdScale;
    else if (cmd.forward < 0) target = (lv[0] - p.accel * kFrameTime) * fwdScale;
    target = std::clamp(target, -p.maxSpeed, p.maxSpeed);
    accel[0] = std::clamp((target - lv[0]) * kInvFrameTime, -p.accel, p.accel);
    // Sideways the target is 0; vertically the current speed limited to maxSpeed.
    accel[1] = std::clamp((0.0f - lv[1]) * kInvFrameTime, -p.accel, p.accel);
    accel[2] = std::clamp((std::clamp(lv[2], -p.maxSpeed, p.maxSpeed) - lv[2]) * kInvFrameTime, -p.accel, p.accel);
    // Yaw: left/right turn the vehicle; with the jump button held it turns towards the driver's view yaw instead
    // (that branch is not limited by rotAccel).
    if (cmd.jump <= 0) {
        const float rightScale = std::fabs(float(cmd.right)) * kInvCmd;
        float t = 0.0f;
        if (cmd.right < 0) t = (p.rotAccel * kFrameTime + av[1]) * rightScale;
        else if (cmd.right > 0) t = (av[1] - p.rotAccel * kFrameTime) * rightScale;
        t = std::clamp(t, -p.rotRate, p.rotRate);
        angularAccel[1] = std::clamp((t - av[1]) * kInvFrameTime, -p.rotAccel, p.rotAccel);
    } else {
        const float t = std::clamp(angleDelta(cmd.viewYaw, b.prevAngles[1]) * kInvFrameTime, -p.rotRate, p.rotRate);
        angularAccel[1] = (t - av[1]) * kInvFrameTime;
    }
    // Pitch and roll speeds are brought back to 0.
    angularAccel[0] = std::clamp(-av[0] * kInvFrameTime, -p.rotAccel, p.rotAccel);
    angularAccel[2] = std::clamp(-av[2] * kInvFrameTime, -p.rotAccel, p.rotAccel);
}

void driveMove(const CollisionWorld& w, DriveBody& b, const DriveParams& p, const DriveCommand& cmd, const DriveTrace& t) {
    Vec accel{}, angularAccel{};
    driveAccelerations(p, b, cmd, accel, angularAccel);
    for (int i = 0; i < 3; ++i) b.angularVelocity[size_t(i)] += angularAccel[size_t(i)] * kFrameTime;
    b.angles = {0.0f, angleNormalize180(b.angularVelocity[1] * kFrameTime + b.prevAngles[1]), 0.0f};
    const float yaw = b.angles[1] * 0.0174532924f;
    const Vec fwd{std::cos(yaw), std::sin(yaw), 0.0f}, left{-std::sin(yaw), std::cos(yaw), 0.0f}, up{0, 0, 1};
    for (int i = 0; i < 3; ++i)
        b.velocity[size_t(i)] += (up[size_t(i)] * accel[2] + left[size_t(i)] * accel[1] + fwd[size_t(i)] * accel[0]) * kFrameTime;
    if (b.velocity[0] != 0.0f || b.velocity[1] != 0.0f || b.velocity[2] != 0.0f) {
        const Ground g = groundTrace(w, t, b);
        if (g.walking) {
            groundMove(w, t, b, g);
        } else {
            if (g.plane) b.velocity = vehicleClipVelocity(b.velocity, g.trace.normal);
            stepSlideMove(w, t, b, g, true);
        }
    }
    b.localVelocity = {dot(b.velocity, fwd), dot(b.velocity, left), dot(b.velocity, up)};
}

} // namespace iw2::game
