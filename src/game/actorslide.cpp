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

void addTouch(ActorSlide& pm, uint16_t entity) {
    if (entity == 0x7fe || pm.touchCount == 0x20) return;
    for (int i = 0; i < pm.touchCount; ++i)
        if (pm.touch[i] == entity) return;
    pm.touch[pm.touchCount++] = entity;
}

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
        addTouch(pm, tr.entity);
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

int stepSlideMove(ActorSlide& pm, bool gravity, const SlideTraceFn& trace) {
    const float o0[3] = {pm.origin[0], pm.origin[1], pm.origin[2]};
    const float v0[3] = {pm.velocity[0], pm.velocity[1], pm.velocity[2]};
    const int r = slideMove(pm, gravity, trace);
    if (r == 0) return 1;
    if (r == 2) return 0;
    const float s = pm.stepHeight;
    if (!(pm.velocity[2] <= 0.0f) && !pm.groundEntValid) {
        const float down[3] = {o0[0], o0[1], o0[2] - s};
        const SlideTrace d = trace(o0, down);
        if (d.fraction == 1.0f) return 1;
        if (d.normal[2] < kMinNormalZ) return 1;
    }
    const float up[3] = {o0[0], o0[1], o0[2] + s};
    const SlideTrace u = trace(o0, up);
    if (u.startSolid) return u.fraction == 0.0f ? 0 : 1;
    const ActorSlide saved = pm;
    pm.hitEnt = 0x7ff;
    pm.groundPlane = false;
    const float upZ = (up[2] - o0[2]) * u.fraction + o0[2];
    pm.origin[0] = o0[0] + 0.0f * u.fraction; // the exe multiplies the fraction by 0.0 for x and y (0x591e00)
    pm.origin[1] = o0[1] + 0.0f * u.fraction;
    pm.origin[2] = upZ;
    for (int a = 0; a < 3; ++a) pm.velocity[a] = v0[a];
    if (slideMove(pm, gravity, trace) == 2) {
        pm = saved;
        return 1;
    }
    const float e2[3] = {pm.origin[0], pm.origin[1], (o0[2] - upZ) + pm.origin[2]};
    const SlideTrace d2 = trace(pm.origin, e2);
    if (!d2.allSolid)
        for (int a = 0; a < 3; ++a) pm.origin[a] = (e2[a] - pm.origin[a]) * d2.fraction + pm.origin[a];
    if (!(d2.fraction < 1.0f)) return 1;
    if (d2.normal[2] >= 0.3f) {
        clipVelocity(pm.velocity, d2.normal, pm.velocity, kOverclip);
        const float hi = o0[2] - saved.origin[2] >= 0.0f ? o0[2] : saved.origin[2]; // 0x408ad0: max
        if (hi + 0.1f < pm.origin[2]) pm.hitEnt = 0x7ff;
        return 1;
    }
    pm = saved;
    return 1;
}

void groundProbe(ActorSlide& pm, const SlideTraceFn& trace) {
    const float start[3] = {pm.origin[0], pm.origin[1], pm.origin[2] + 0.25f};
    const float drop = (!(pm.velocity[2] <= 0.0f) || pm.groundEnt == 0x7ff) ? 0.25f : pm.stepHeight;
    const float end[3] = {pm.origin[0], pm.origin[1], pm.origin[2] - drop};
    auto store = [&](const SlideTrace& t) {
        pm.groundTrace[0] = t.fraction;
        pm.groundTrace[1] = t.normal[0];
        pm.groundTrace[2] = t.normal[1];
        pm.groundTrace[3] = t.normal[2];
        pm.groundSurfFlags = t.surfFlags;
        for (int a = 0; a < 3; ++a) pm.groundNormal[a] = t.normal[a]; // 0x1cbac68 is the same memory
        pm.flag87 = t.allSolid;
    };
    SlideTrace r = trace(start, end);
    store(r);
    if (r.allSolid) {
        if (r.surface & 0x2000000u) {
            addTouch(pm, r.entity);
            pm.mask &= ~0x2000000u;
            r = trace(start, end);
            store(r);
        }
        if (r.allSolid) {
            const float o[3] = {pm.origin[0], pm.origin[1], pm.origin[2]};
            r = trace(o, end);
            store(r);
            if (r.allSolid) {
                pm.groundEnt = r.entity;
                pm.groundPlane = false;
                pm.groundEntValid = true;
                return;
            }
        }
    }
    if (r.fraction == 1.0f) {
        pm.groundEnt = 0x7ff;
        pm.groundPlane = false;
        pm.groundEntValid = false;
        return;
    }
    if (pm.velocity[2] > 0.0f) {
        const float d = r.normal[1] * pm.velocity[1] + r.normal[2] * pm.velocity[2] + r.normal[0] * pm.velocity[0];
        if (d > 10.0f) { // 0x591e8c
            pm.groundEnt = 0x7ff;
            pm.groundPlane = false;
            pm.groundEntValid = false;
            return;
        }
    }
    if (r.normal[2] < kMinNormalZ) {
        pm.groundEnt = 0x7ff;
        pm.groundPlane = true;
        pm.groundEntValid = false;
        if (pm.hitEnt == 0x7ff && (r.normal[0] != 0.0f || r.normal[1] != 0.0f)) {
            pm.hitEnt = r.entity;
            pm.hitPos[0] = pm.origin[0];
            pm.hitPos[1] = pm.origin[1];
            pm.hitNormal[0] = r.normal[0];
            pm.hitNormal[1] = r.normal[0]; // the exe stores normal.x for both (0x548521 / 0x54852c)
            pm.hitIsAllSolid = false;
        }
        return;
    }
    const float newZ = (end[2] - start[2]) * r.fraction + start[2];
    if (newZ < pm.origin[2] || pm.mode == 5) pm.origin[2] = newZ;
    pm.groundEnt = r.entity;
    pm.groundPlane = true;
    pm.groundEntValid = true;
    addTouch(pm, r.entity);
}

int moveBlock(ActorSlide& pm, const SlideTraceFn& trace) {
    pm.hitEnt = 0x7ff;
    pm.touchCount = 0;
    pm.blocked = false;
    if (pm.groundEnt == 0x7fe && pm.moveDelta[0] == 0.0f && pm.moveDelta[1] == 0.0f && pm.moveDelta[2] == 0.0f) {
        pm.velocity[0] = pm.velocity[1] = pm.velocity[2] = 0.0f;
        return 1;
    }
    pm.stepHeight = pm.lowStep ? 10.0f : 18.0f;
    pm.contact = false;
    pm.contactNormalZ = 0;
    if (pm.mode == 3) {
        for (int a = 0; a < 3; ++a) pm.origin[a] += pm.moveDelta[a];
        return 1;
    }
    const float inv = pm.dt > 0.0f ? 1.0f / pm.dt : 0.0f; // TODO(spec): the exe divides by dt unguarded
    if (pm.mode == 4) {
        for (int a = 0; a < 3; ++a) pm.velocity[a] = pm.moveDelta[a] * inv;
        slideMove(pm, false, trace);
        return 1;
    }
    groundProbe(pm, trace);
    int r;
    if (pm.mode == 5) {
        pm.velocity[0] = pm.moveDelta[0] * inv;
        pm.velocity[1] = pm.moveDelta[1] * inv;
        pm.velocity[2] = 0.0f;
        r = stepSlideMove(pm, true, trace);
    } else if (pm.groundEntValid) {
        pm.velocity[0] = pm.moveDelta[0] * inv;
        pm.velocity[1] = pm.moveDelta[1] * inv;
        pm.velocity[2] = 0.0f;
        const float ox = pm.velocity[0], oy = pm.velocity[1];
        const float len = std::sqrt(ox * ox + oy * oy);
        clipVelocity(pm.velocity, &pm.groundTrace[1], pm.velocity, kOverclip);
        if (oy * pm.velocity[1] + ox * pm.velocity[0] > 0.0f) {
            const float l = std::sqrt(pm.velocity[0] * pm.velocity[0] + pm.velocity[1] * pm.velocity[1] + pm.velocity[2] * pm.velocity[2]);
            if (l > 0.0f)
                for (int a = 0; a < 3; ++a) pm.velocity[a] = pm.velocity[a] / l * len;
        }
        if (pm.velocity[0] == 0.0f && pm.velocity[1] == 0.0f && pm.groundPlane) return 1;
        r = stepSlideMove(pm, false, trace);
    } else {
        if (pm.groundPlane) clipVelocity(pm.velocity, &pm.groundTrace[1], pm.velocity, kOverclip);
        r = stepSlideMove(pm, true, trace);
    }
    if (pm.groundPlane && pm.groundTrace[3] >= 0.3f) { // 0x592018
        pm.contact = true;
        pm.contactNormalZ = pm.groundTrace[3];
        pm.surfaceType = int((pm.groundSurfFlags >> 20) & 0x1f);
    } else {
        pm.surfaceType = 0;
    }
    return r;
}

} // namespace iw2::game
