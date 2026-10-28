// Pose helpers shared by the scripted-animation pose (0x53baf0) and the turret gunner's pose step (0x549ba0):
// the delta yaw and the residual filters that let an entity slide from where it stood into an animated pose.
#pragma once

#include "script/value.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace iw2::game::pose {

using script::Vec3;

// 0x4240a0: into [0, 360).
inline float angleNormalize360(float a) {
    while (a < 0.0f) a += 360.0f;
    while (a >= 360.0f) a -= 360.0f;
    return a;
}

// 0x423900: yaw in degrees of the delta quaternion (z, w).
inline float deltaYaw(float z, float w) {
    const float n = 2.0f / (z * z + w * w);
    return std::atan2(w * z * n, 1.0f - n * z * z) * 57.2957763671875f; // 0x591fd8
}

// 0x53b9a0: the offset shrinks by 300 · dt inches (the exe's fast inverse square root with one Newton step);
// what is left is added to the origin, or the offset ends.
inline void slideOffset(Vec3& offset, Vec3& origin, float dt) {
    const float len2 = offset[2] * offset[2] + offset[1] * offset[1] + offset[0] * offset[0];
    if (len2 == 0.0f) return;
    int32_t bits;
    std::memcpy(&bits, &len2, 4);
    bits = 0x5f3759df - (bits >> 1);
    float y;
    std::memcpy(&y, &bits, 4);
    const float f = 1.0f - (1.5f - len2 * 0.5f * y * y) * y * dt * 300.0f /* 0x591f4c */;
    if (f > 0.0f) {
        for (int i = 0; i < 3; ++i) {
            offset[size_t(i)] *= f;
            origin[size_t(i)] += offset[size_t(i)];
        }
        return;
    }
    offset = {0, 0, 0};
}

// 0x53ba60: each angle offset moves towards 0 by rate · dt degrees; a non-zero rest is added to the angle.
inline void slideAngles(Vec3& offset, Vec3& angles, float dt, float rate) {
    const float step = dt * rate;
    for (int i = 0; i < 3; ++i) {
        float& o = offset[size_t(i)];
        if (o == 0.0f) continue;
        if (o > step) {
            o -= step;
            angles[size_t(i)] = angleNormalize360(o + angles[size_t(i)]);
        } else if (o < -step) {
            o += step;
            angles[size_t(i)] = angleNormalize360(o + angles[size_t(i)]);
        } else {
            o = 0.0f;
        }
    }
}

} // namespace iw2::game::pose
