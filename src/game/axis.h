// Rotation frames as the executable keeps them: three rows forward, left, up (docs/spec/animscripted.md "Frames").
#pragma once

#include "script/value.h"

#include <array>
#include <cmath>

namespace iw2::game {

using Axis = std::array<script::Vec3, 3>;

// AnglesToAxis (0x421da0): forward, left (= −right), up of pitch / yaw / roll in degrees.
inline Axis anglesAxis(const script::Vec3& a) {
    const float rad = 0.0174532924f; // 0x591f50
    const float sy = std::sin(a[1] * rad), cy = std::cos(a[1] * rad);
    const float sp = std::sin(a[0] * rad), cp = std::cos(a[0] * rad);
    const float sr = std::sin(a[2] * rad), cr = std::cos(a[2] * rad);
    return Axis{script::Vec3{cp * cy, cp * sy, -sp},
                script::Vec3{-(cr * sy - sr * sp * cy), cr * cy + sr * sp * sy, sr * cp},
                script::Vec3{sr * sy + cr * sp * cy, cr * sp * sy - sr * cy, cr * cp}};
}

// AxisToAngles (0x424b70): yaw and pitch of the forward row (0x421b30), roll from the left row turned back by them.
inline script::Vec3 axisAngles(const Axis& m) {
    const float deg = 180.0f / float(M_PI);
    const script::Vec3& f = m[0];
    script::Vec3 a{0, 0, 0};
    if (f[1] == 0.0f && f[0] == 0.0f) {
        a[0] = f[2] > 0.0f ? 270.0f : 90.0f;
    } else {
        a[1] = std::atan2(f[1], f[0]) * deg;
        if (a[1] < 0) a[1] += 360.0f;
        a[0] = std::atan2(f[2], std::sqrt(f[0] * f[0] + f[1] * f[1])) * -deg;
        if (a[0] < 0) a[0] += 360.0f;
    }
    const script::Vec3& l = m[1];
    const float rad = 0.0174532924f;
    const float cy = std::cos(-a[1] * rad), sy = std::sin(-a[1] * rad);
    const float x1 = l[0] * cy - l[1] * sy, y1 = l[1] * cy + l[0] * sy;
    const float cp = std::cos(-a[0] * rad), sp = std::sin(-a[0] * rad);
    const float x2 = x1 * cp + l[2] * sp, z2 = l[2] * cp - x1 * sp;
    // pitch of (x2, y1, z2) (0x421a70)
    const float p = (y1 == 0.0f && x2 == 0.0f) ? (z2 > 0.0f ? -90.0f : 90.0f)
                                                : std::atan2(z2, std::sqrt(x2 * x2 + y1 * y1)) * -deg;
    a[2] = y1 < 0.0f ? p + (p >= 0.0f ? -180.0f : 180.0f) : -p;
    return a;
}

// 0x423430: origin + v.x · forward + v.y · left + v.z · up.
inline script::Vec3 axisApply(const script::Vec3& origin, const Axis& m, const script::Vec3& v) {
    script::Vec3 r;
    for (int i = 0; i < 3; ++i) r[size_t(i)] = v[0] * m[0][size_t(i)] + v[1] * m[1][size_t(i)] + v[2] * m[2][size_t(i)] + origin[size_t(i)];
    return r;
}

// 0x422450: the rows of `local` taken in the frame `parent` (row i = Σ local[i][j] · parent[j]).
inline Axis axisMul(const Axis& local, const Axis& parent) {
    Axis r{};
    for (int i = 0; i < 3; ++i)
        for (int k = 0; k < 3; ++k)
            r[size_t(i)][size_t(k)] = local[size_t(i)][0] * parent[0][size_t(k)] + local[size_t(i)][1] * parent[1][size_t(k)] +
                                      local[size_t(i)][2] * parent[2][size_t(k)];
    return r;
}

// Inverse of an orthonormal frame.
inline Axis axisTranspose(const Axis& m) {
    Axis r{};
    for (int i = 0; i < 3; ++i)
        for (int k = 0; k < 3; ++k) r[size_t(i)][size_t(k)] = m[size_t(k)][size_t(i)];
    return r;
}

// A point in the frame (origin, m): its coordinates along the rows.
inline script::Vec3 axisLocal(const script::Vec3& origin, const Axis& m, const script::Vec3& p) {
    const script::Vec3 d{p[0] - origin[0], p[1] - origin[1], p[2] - origin[2]};
    script::Vec3 r;
    for (int i = 0; i < 3; ++i) r[size_t(i)] = d[0] * m[size_t(i)][0] + d[1] * m[size_t(i)][1] + d[2] * m[size_t(i)][2];
    return r;
}

// 0x424b20: the frame of a yaw in degrees.
inline Axis yawAxis(float yaw) { return anglesAxis({0.0f, yaw, 0.0f}); }

} // namespace iw2::game
