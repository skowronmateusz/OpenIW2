// Third-person view of the MP client (docs/spec/mp-spectate.md "Dead view"), implemented from the spec.
#pragma once

#include "game/collision.h"

#include <cmath>
#include <functional>
#include <optional>

namespace iw2::game {

struct ThirdPersonView {
    Vec origin{};
    float pitch = 0, yaw = 0, roll = 0;
};

// Box trace used by the view: (start, end, mins, maxs) -> result. The caller binds the mask (0x811) and the pass entity.
using ThirdPersonTraceFn = std::function<TraceResult(const Vec&, const Vec&, const Vec&, const Vec&)>;

inline constexpr uint32_t kThirdPersonMask = 0x811u;

// eye: the view origin with the view height added. pitch/yaw/roll: the view angles. deadYaw: set while the movement type
// is a dead one (it replaces the yaw of both the focus and the view). range / angleDeg: cg_thirdPersonRange (120) and
// cg_thirdPersonAngle (0).
inline ThirdPersonView thirdPersonView(const Vec& eye, float pitch, float yaw, float roll, std::optional<float> deadYaw, float range,
                                       float angleDeg, const ThirdPersonTraceFn& trace) {
    constexpr float kDeg = 3.14159265358979323846f / 180.0f;
    if (deadYaw) yaw = *deadYaw;
    float focusPitch = pitch;
    if (focusPitch > 45.0f) focusPitch = 45.0f;
    auto forwardOf = [&](float p, float y) {
        const float cp = std::cos(p * kDeg), sp = std::sin(p * kDeg), cy = std::cos(y * kDeg), sy = std::sin(y * kDeg);
        return Vec{cp * cy, cp * sy, -sp};
    };
    const Vec ff = forwardOf(focusPitch, yaw);
    const Vec focus{eye[0] + ff[0] * 512.0f, eye[1] + ff[1] * 512.0f, eye[2] + ff[2] * 512.0f};
    Vec view{eye[0], eye[1], eye[2] + 8.0f};
    const float viewPitch = pitch * 0.5f;
    const Vec f = forwardOf(viewPitch, yaw);
    const float sy = std::sin(yaw * kDeg), cy = std::cos(yaw * kDeg);
    const float sr = std::sin(roll * kDeg), cr = std::cos(roll * kDeg), sp = std::sin(viewPitch * kDeg);
    const Vec r{-sr * sp * cy + cr * sy, -sr * sp * sy - cr * cy, -sr * std::cos(viewPitch * kDeg)};
    const float fs = std::cos(angleDeg * kDeg), ss = std::sin(angleDeg * kDeg);
    for (int i = 0; i < 3; ++i) view[size_t(i)] += f[size_t(i)] * (-range * fs) + r[size_t(i)] * (-range * ss);
    const Vec mins{-4, -4, -4}, maxs{4, 4, 4};
    if (trace) {
        TraceResult tr = trace(eye, view, mins, maxs);
        if (tr.fraction != 1.0f) {
            // Pull in to the hit point and raise by (1 - fraction) * 32, then trace again from the eye.
            Vec up{eye[0] + (view[0] - eye[0]) * tr.fraction, eye[1] + (view[1] - eye[1]) * tr.fraction,
                   eye[2] + (view[2] - eye[2]) * tr.fraction + (1.0f - tr.fraction) * 32.0f};
            tr = trace(eye, up, mins, maxs);
            view = {eye[0] + (up[0] - eye[0]) * tr.fraction, eye[1] + (up[1] - eye[1]) * tr.fraction,
                    eye[2] + (up[2] - eye[2]) * tr.fraction};
        }
    }
    const float dx = focus[0] - view[0], dy = focus[1] - view[1], dz = focus[2] - view[2];
    float dist = std::sqrt(dx * dx + dy * dy);
    if (dist < 1.0f) dist = 1.0f;
    ThirdPersonView out;
    out.origin = view;
    out.pitch = -std::atan2(dz, dist) / kDeg;
    out.yaw = yaw - angleDeg;
    out.roll = roll;
    return out;
}

} // namespace iw2::game
