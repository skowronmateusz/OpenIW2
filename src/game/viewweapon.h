// First-person weapon placement offsets that depend on stance, movement and aiming (docs/spec/weapons.md).
#pragma once

#include <array>
#include <functional>
#include <string>

namespace iw2::game {

using V3 = std::array<float, 3>;

class ViewWeaponMotion {
public:
    // Reads a numeric field of the current weapon file (0 when absent).
    using Field = std::function<float(const char*)>;

    // Advances the smoothed offsets by ms milliseconds (0x4b5380 position, 0x4b5730 rotation).
    // xySpeed: horizontal player speed; maxSpeed: g_speed; viewHeight: current eye height (60/40/11 at rest).
    void update(const Field& weapon, int ms, float xySpeed, float maxSpeed, float viewHeight, float adsFrac);

    // Offsets to add: forward/right/up and pitch/yaw/roll, already faded for aiming down the sights.
    const V3& move() const { return outMove_; }
    const V3& rot() const { return outRot_; }

private:
    V3 curMove_{}, curRot_{}, outMove_{}, outRot_{};
};

// Field of view while aiming (0x4ae7d9): horizontal cg_fov towards min(adsZoomFov, cg_fov). `zoomingIn` is true
// while the aim fraction rises (it becomes true at fraction 0 and false at 1).
float adsFov(float cgFov, float adsZoomFov, float zoomInFrac, float zoomOutFrac, float adsFrac, bool zoomingIn);

} // namespace iw2::game
