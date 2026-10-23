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
    void update(const Field& weapon, int ms, float xySpeed, float maxSpeed, float viewHeight, float adsFrac, float lean = 0);

    // Gun sway, after update(): idle sway (0x4b51a0) and the lag behind view turns (0x4b4dc0, applied by 0x4b61d0 /
    // 0x4b6420). viewPitch/viewYaw: the view angles this frame; stance: 0 stand, 1 crouch, 2 prone.
    void sway(const Field& weapon, int ms, float viewPitch, float viewYaw, float adsFrac, int stance, bool reticle);

    // Offsets to add: forward/right/up and pitch/yaw/roll, already faded for aiming down the sights.
    const V3& move() const { return outMove_; }
    const V3& rot() const { return outRot_; }

private:
    V3 curMove_{}, curRot_{}, outMove_{}, outRot_{};
    float idleFactor_ = 0;   // 0xf79068
    int idleTime_ = 0;       // 0xf79774
    float lagSide_ = 0, lagUp_ = 0, lagPitch_ = 0, lagYaw_ = 0; // 0xf791b8 / bc / a8 / ac
    float prevPitch_ = 0, prevYaw_ = 0;                        // 0xf7919c / a0
    bool havePrev_ = false;
};

// Gun bob angles (cgame 0x4b4a90, added to the weapon angles by 0x4b4d10): pitch, yaw, roll in degrees from the
// player state's bob cycle (0..255), the horizontal speed and the view height target (11 prone, 40 crouched).
// adsBobFactor: the weapon's; reticle: the weapon has an adsOverlayReticle (then also × (1 − adsFrac)).
V3 gunBob(int bobCycle, float xySpeed, int viewHeightTarget, float adsFrac, float adsBobFactor, bool reticle);

// Camera bob (CG_OffsetFirstPersonView 0x4ae290): the eye goes up by the bob wave (0x4adf90, cap bg_bobMax 8) and
// sideways along the view's right vector by sin(cycle) · min(speed · stance amplitude, bg_bobMax) (0x4ae010).
// Aiming a weapon with adsViewBobMult turns the view: pitch −= adsFrac · mult · wave (cap 45), yaw −= adsFrac ·
// mult · side (cap 45). Returns {up, side, pitch, yaw}.
std::array<float, 4> viewBob(int bobCycle, float xySpeed, int viewHeightTarget, float adsFrac, float adsViewBobMult);

// Field of view while aiming (0x4ae7d9): horizontal cg_fov towards min(adsZoomFov, cg_fov). `zoomingIn` is true
// while the aim fraction rises (it becomes true at fraction 0 and false at 1).
float adsFov(float cgFov, float adsZoomFov, float zoomInFrac, float zoomOutFrac, float adsFrac, bool zoomingIn);

} // namespace iw2::game
