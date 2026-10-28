#pragma once
// Angular trajectory re-base of a bouncing grenade (exe 0x525cb0, called from the bounce 0x525f30). Pure math; the caller
// supplies the rand() draw so the shared generator state stays in World.

#include <array>
#include <cmath>
#include <functional>

namespace iw2::game {

using GrenadeVec3 = std::array<float, 3>;

// 0x424010 / 0x424040 style 16-bit angle quantisation: truncate to 1/65536 of a turn, wrap into [0, 360).
inline float grenadeAngleMod16(float a) { return float(int(a * 182.04444885253906f) & 0xffff) * 0.0054931640625f; }
// 0x424040: the same wrapped into (-180, 180].
inline float grenadeAngleNorm180(float a) {
    float r = grenadeAngleMod16(a);
    if (!(r <= 180.0f)) r -= 360.0f;
    return r;
}

// Pitch (0x421990, vectoangles' pitch) of the horizontal heading `yawDeg` projected into the plane with normal `n`
// (0x4261a0): w = c - (c.n) n with c = (cos yaw, sin yaw, 0).
inline float grenadeSlopePitch(const GrenadeVec3& n, float yawDeg) {
    const float rad = yawDeg * 0.01745329238474369f;
    const float cy = std::cos(rad), sy = std::sin(rad);
    const float k = -(cy * n[0] + sy * n[1]);
    const float w[3] = {k * n[0] + cy, k * n[1] + sy, k * n[2]};
    float p;
    if (w[0] == 0.0f && w[1] == 0.0f) {
        p = w[2] > 0.0f ? 270.0f : 90.0f; // 0x4219c6 loads 0x43870000 (270); the other constant branch is 90
    } else {
        p = std::atan2(w[2], std::sqrt(w[0] * w[0] + w[1] * w[1])) * -180.0f / 3.14159265358979f;
        if (p < 0.0f) p += 360.0f;
    }
    return p;
}

struct GrenadeRebase {
    GrenadeVec3 angles;  // the angles the caller stores as the new angular base (and, when resting, as currentAngles)
    float pitchSpin;     // the new angular delta[0]
};

// `hit` = angles at the hit time, `spin0` = the current angular delta[0], `n` = the surface normal, `resting` = the call from the
// rest branch (0x5260d2), else the flying branch (0x52615e). `rnd` returns the next rand() >> 17 & 0x7fff (0x5a56f4); it is
// invoked exactly where the exe draws.
inline GrenadeRebase grenadeRebaseAngles(const GrenadeVec3& hit, float spin0, const GrenadeVec3& n, bool resting,
                                         const std::function<int()>& rnd) {
    GrenadeRebase r{hit, spin0};
    if (n[2] <= 0.1f) { // wall: only a flying grenade gets its pitch spin perturbed by (rand & 0x7f) - 63 degrees/s
        if (!resting) r.pitchSpin = grenadeAngleMod16(float((rnd() & 0x7f) - 63) + spin0);
        return r;
    }
    const float slope = grenadeSlopePitch(n, hit[1]);
    float diff = hit[0] - slope; // 0x423f50: reduced into [-180, 180]
    while (diff > 180.0f) diff -= 360.0f;
    while (diff < -180.0f) diff += 360.0f;
    const float d = std::fabs(diff);
    // rescale the pitch spin by rand/32768 * 0.3 + 0.85 (0x525d71..0x525da6); reversed when the grenade is within 80 degrees
    // of lying along the slope
    const float f = float(rnd()) / 32768.0f * 0.3f + 0.85f;
    r.pitchSpin = d < 80.0f ? spin0 * f * -1.0f : spin0 * f;
    const float p = grenadeAngleNorm180(hit[0]);
    if (!resting && d >= 45.0f) {
        r.angles[0] = grenadeAngleMod16(d < 80.0f ? d * 0.25f + p : p);
    } else if (std::fabs(p) <= 90.0f) { // lie flat on the slope, face up or face down by the current pitch
        r.angles[0] = grenadeAngleMod16(slope);
    } else {
        r.angles[0] = grenadeAngleMod16(slope + 180.0f);
    }
    return r;
}

} // namespace iw2::game
