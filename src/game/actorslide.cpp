#include "game/actorslide.h"

#include <cmath>

namespace iw2::game {
namespace {
constexpr float kOverclip = 1.001f; // 0x3f8020c5
constexpr float kMinNormalZ = 0.7f; // 0x591f30
constexpr double kSamePlane = 0.99; // 0x5922b8
constexpr double kInto = 0.1;       // 0x5922b0

float dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// 0x421270: dst = src / |src|; a zero-length source leaves dst untouched.
void normalize2(const float src[3], float dst[3]) {
    const float len = std::sqrt(src[0] * src[0] + src[1] * src[1] + src[2] * src[2]);
    if (len == 0.0f) return;
    const float inv = 1.0f / len;
    dst[0] = inv * src[0];
    dst[1] = inv * src[1];
    dst[2] = inv * src[2];
}
void normalize(float v[3]) { normalize2(v, v); }
void cross(const float a[3], const float b[3], float out[3]) {
    out[0] = a[1] * b[2] - b[1] * a[2];
    out[1] = a[2] * b[0] - b[2] * a[0];
    out[2] = a[0] * b[1] - b[0] * a[1];
}
void recordHit(ActorSlide& pm, uint16_t ent, float nx, float ny) {
    pm.hitEnt = ent;
    pm.hitPos[0] = pm.origin[0];
    pm.hitPos[1] = pm.origin[1];
    pm.hitNormal[0] = nx;
    pm.hitNormal[1] = ny;
    pm.hitIsAllSolid = false;
}
} // namespace

void clipVelocity(const float in[3], const float n[3], float out[3], float overbounce) {
    if (!(n[2] < kMinNormalZ)) {
        const float xy = in[0] * in[0] + in[1] * in[1];
        const float zz = in[2] * in[2];
        if (zz <= xy) {
            const float x = in[0], y = in[1];
            out[2] = -(x * n[0]) - n[1] * y;
            out[0] = x * n[2];
            out[1] = y * n[2];
            return;
        }
    }
    float backoff = in[1] * n[1] + in[0] * n[0] + in[2] * n[2];
    backoff = backoff >= 0.0f ? backoff / overbounce : backoff * overbounce;
    const float ox = in[0] - backoff * n[0];
    const float oy = in[1] - backoff * n[1];
    const float oz = in[2] - backoff * n[2];
    out[0] = ox;
    out[1] = oy;
    out[2] = oz;
}

int slideMove(ActorSlide& pm, bool gravity, const SlideTraceFn& trace) {
    float* vel = pm.velocity;
    float endVel[3] = {vel[0], vel[1], vel[2]};
    if (gravity) {
        endVel[2] = endVel[2] - pm.dt * pm.gravity;
        vel[2] = (endVel[2] + vel[2]) * 0.5f;
        if (pm.groundPlane) clipVelocity(vel, pm.groundNormal, vel, kOverclip);
    }
    float timeLeft = pm.dt;
    float planes[5][3];
    int numPlanes = 0;
    if (pm.groundPlane) {
        planes[0][0] = pm.groundNormal[0];
        planes[0][1] = pm.groundNormal[1];
        planes[0][2] = pm.groundNormal[2];
        numPlanes = 1;
    }
    normalize2(vel, planes[numPlanes]);
    ++numPlanes;

    int bump = 0;
    auto finish = [&]() {
        if (gravity) {
            vel[0] = endVel[0];
            vel[1] = endVel[1];
            vel[2] = endVel[2];
        }
        return bump != 0 ? 1 : 0;
    };

    while (bump < 4) {
        float end[3] = {timeLeft * vel[0] + pm.origin[0], timeLeft * vel[1] + pm.origin[1], timeLeft * vel[2] + pm.origin[2]};
        const SlideTrace tr = trace(pm.origin, end);
        if (tr.fraction > 0.0f) {
            for (int a = 0; a < 3; ++a) pm.origin[a] = (end[a] - pm.origin[a]) * tr.fraction + pm.origin[a];
            if (tr.fraction == 1.0f) return finish();
        } else if (tr.allSolid) {
            vel[2] = 0.0f;
            if (bump == 0 && pm.flag87) return 2;
            if (pm.hitEnt == 0x7ff) {
                pm.hitEnt = tr.entity;
                pm.hitPos[0] = pm.origin[0];
                pm.hitPos[1] = pm.origin[1];
                pm.hitIsAllSolid = true;
            }
            return 1;
        }
        pm.blocked = true;
        const float* n = tr.normal;
        const bool wallHit = n[2] < kMinNormalZ && pm.hitEnt == 0x7ff && (n[0] != 0.0f || n[1] != 0.0f);
        if (wallHit) recordHit(pm, tr.entity, n[0], n[1]);
        if (tr.entity != 0x7fe && pm.touchCount != 0x20) {
            bool found = false;
            for (int i = 0; i < pm.touchCount; ++i)
                if (pm.touch[i] == tr.entity) { found = true; break; }
            if (!found) pm.touch[pm.touchCount++] = tr.entity;
        }
        timeLeft = timeLeft - tr.fraction * timeLeft;
        if (numPlanes >= 5) {
            vel[0] = vel[1] = vel[2] = 0.0f;
            return 1;
        }
        int i = 0;
        for (; i < numPlanes; ++i)
            if (double(n[1] * planes[i][1] + n[0] * planes[i][0] + n[2] * planes[i][2]) > kSamePlane) break;
        if (i < numPlanes) {
            // Same plane again: nudge the velocity out along the normal.
            if (tr.fraction == 0.0f && pm.hitEnt == 0x7ff && (n[0] != 0.0f || n[1] != 0.0f)) recordHit(pm, tr.entity, n[0], n[1]);
            vel[0] += n[0];
            vel[1] += n[1];
            vel[2] += n[2];
            ++bump;
            continue;
        }
        planes[numPlanes][0] = n[0];
        planes[numPlanes][1] = n[1];
        planes[numPlanes][2] = n[2];
        ++numPlanes;

        float clipVel[3] = {0, 0, 0}, endClip[3] = {0, 0, 0};
        int pi = 0;
        for (; pi < numPlanes; ++pi) {
            float into = vel[2] * planes[pi][2] + vel[0] * planes[pi][0] + vel[1] * planes[pi][1];
            if (double(into) >= kInto) continue;
            into = -into;
            if (into > pm.impactSpeed) pm.impactSpeed = into;
            clipVelocity(vel, planes[pi], clipVel, kOverclip);
            clipVelocity(endVel, planes[pi], endClip, kOverclip);
            for (int j = 0; j < numPlanes; ++j) {
                if (j == pi) continue;
                if (!(double(dot(clipVel, planes[j])) < kInto)) continue;
                clipVelocity(clipVel, planes[j], clipVel, kOverclip);
                clipVelocity(endClip, planes[j], endClip, kOverclip);
                if (!(dot(clipVel, planes[pi]) < 0.0f)) continue;
                float dir[3];
                cross(planes[j], planes[pi], dir); // the exe computes j x i for both
                normalize(dir);
                float d = dot(dir, vel);
                clipVel[0] = dir[0] * d;
                clipVel[1] = dir[1] * d;
                clipVel[2] = dir[2] * d;
                float dir2[3];
                cross(planes[j], planes[pi], dir2);
                normalize(dir2);
                d = dot(dir2, endVel);
                endClip[0] = dir2[0] * d;
                endClip[1] = dir2[1] * d;
                endClip[2] = dir2[2] * d;
                for (int k = 0; k < numPlanes; ++k) {
                    if (k == pi || k == j) continue;
                    if (!(double(dot(clipVel, planes[k])) < kInto)) continue;
                    vel[0] = vel[1] = vel[2] = 0.0f;
                    return 1;
                }
            }
            break;
        }
        if (pi < numPlanes) {
            if (pm.hitEnt == 0x7ff) {
                pm.hitEnt = tr.entity;
                pm.hitPos[0] = pm.origin[0];
                pm.hitPos[1] = pm.origin[1];
                pm.hitNormal[0] = clipVel[0] - vel[0];
                pm.hitNormal[1] = clipVel[1] - vel[1];
                pm.hitIsAllSolid = false;
            }
            for (int a = 0; a < 3; ++a) {
                vel[a] = clipVel[a];
                endVel[a] = endClip[a];
            }
        }
        ++bump;
    }
    return finish();
}

} // namespace iw2::game
