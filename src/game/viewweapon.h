// First-person weapon placement offsets that depend on stance, movement and aiming (docs/spec/weapons.md).
#pragma once

#include <array>
#include <cstdint>
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
    // `shock` is the shell-shock envelope 0..1 (shellShockRatio), 0 without shell shock.
    void sway(const Field& weapon, int ms, float viewPitch, float viewYaw, float adsFrac, int stance, bool reticle,
              float shock = 0.0f);

    // Scope sway of the view angles (CG_... 0x4ae060, reticle weapons only; called from the view offset 0x4ae290): returns
    // {pitch, yaw} degrees to add to the view angles. Shares the idle phase (0xf79774) and the stance factor (0xf79068)
    // with sway(); breathScale is ps+0x590 (hold-breath damping). Zero for a weapon without a reticle or at aim fraction 0.
    // stance: 0 stand, 1 crouch, 2 prone.
    std::array<float, 2> scopeSway(const Field& weapon, int ms, float adsFrac, int stance, float breathScale, bool reticle);

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

// Weapon recoil. fire(): the impulse of the cgame fire event (0x4dfea0); update(): the view-kick
// spring (0x4ade00; pos is added to the view angles) and the gun-kick spring (0x4b5d10 / integrator 0x4b5bc0; pos is added to
// the view-model angles). Angles in degrees, pitch/yaw/roll order.
class RecoilKick {
public:
    using Field = ViewWeaponMotion::Field;
    // adsFrac: the player's aim fraction. reducedKick: ps+0x40 > 0 (TODO(spec): meaning).
    void fire(const Field& w, float adsFrac, bool reducedKick);
    // hasWeapon: the current weapon index is non-zero (otherwise the centring speed is the constant 2400).
    void update(const Field& w, int ms, bool hasWeapon, float adsFrac);
    const V3& viewKick() const { return viewPos_; }   // 0xf798c8
    V3 gunKick() const { return {gunPos_[0], gunPos_[1], 0}; } // 0xf79928..30
    void reset() { *this = RecoilKick{}; }

private:
    float rand01();
    V3 viewVel_{}, viewPos_{};
    float gunVel_[2] = {0, 0}, gunPos_[2] = {0, 0};
    unsigned seed_ = 1;
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

// Shell-shock envelope of the gun sway (0x4b4e01-0x4b4e8d): remaining = start + duration - time; 0 when it is over, 1 while
// it is at least the shock's first parameter `fadeMs` (the int at +0 of its 0x84-byte record, 0xf7e888), else
// smoothstep(remaining / fadeMs) = r*r*(3 - 2r). The sway scales are multiplied by (swayShellShockScale - 1) * ratio + 1.
float shellShockRatio(int remainingMs, int fadeMs);

// Field of view while aiming (0x4ae7d9): horizontal cg_fov towards min(adsZoomFov, cg_fov). `zoomingIn` is true
// while the aim fraction rises (it becomes true at fraction 0 and false at 1).
float adsFov(float cgFov, float adsZoomFov, float zoomInFrac, float zoomOutFrac, float adsFrac, bool zoomingIn);


// Camera impulses of CG_OffsetFirstPersonView (0x4ae290): the damage kick, the landing dip and the minimum eye height.
class ViewImpulses {
public:
    // cgame's reset on a new level / respawn (0x4ba570): no landing yet.
    void reset() { landTime_ = -1; landChange_ = 0; damageTime_ = 0; stepTime_ = 0; stepChange_ = 0; }
    // A damage event (cgame 0x4ba2a0 on a new ps damageEvent): kick = clamp(count * cg_viewKickScale (0.6), 5, cg_viewKickMax
    // (90)); `fwd` and `left` are the view axes rows 0 and 1 (left = +y for yaw 0). A world hit (both bytes 255) kicks
    // the pitch back by the full amount. `timeMs` is the snapshot time (0x4ba53d).
    void damage(int yawByte, int pitchByte, int count, const V3& fwd, const V3& left, int64_t timeMs);
    // The landing report of the player state (pmove crashLand): a heavy landing (fall height above 12, no damage) or the
    // fall damage dips the view; both end as a negative z change at `timeMs` (0x4babe2 / 0x4babe8).
    void land(float fallHeight, int fallDamage, int64_t timeMs);
    // Eye z added by the landing dip at `timeMs` (0x4ae6c7-0x4ae748): up to 150 ms in, then back out over 300 ms.
    float landOffset(int64_t timeMs);
    // {pitch, roll} added to the view at `timeMs` (0x4ae371-0x4ae4a9): 100 ms in, 400 ms out; aiming scales it by
    // 1 - 0.5 * adsFrac (times 1 + 0.5 * adsFrac for a weapon with an ADS overlay reticle).
    std::array<float, 2> damageKick(int64_t timeMs, float adsFrac, bool reticle) const;
    // The own client's step event 0x8f (cgame 0x4bac84): the view step change = the unfinished part of the previous one
    // ((100 - elapsed) * change * 0.01 * 0.9 within 100 ms) + (parm - 0x80), clamped to -16..24, stamped `timeMs`.
    void step(int parm, int64_t timeMs);
    // Eye z subtracted at `timeMs` (0x4addb0): (100 - elapsed) * change * 0.01 within 100 ms of the step.
    float stepOffset(int64_t timeMs);
    // Eye z is at least the origin z + 8 (0x4ae775-0x4ae799).
    static float minEyeZ(float originZ) { return originZ + 8.0f; }
    // The view dip of a landing: 0 up to fall height 12, else min(24, trunc(((h - 12) / 26 + 1) * 4)) (0x509107, 0x4bab9f).
    static int landDip(float fallHeight);
    static int dipFromDamage(int damage); // the cgame rebuilds h from the damage (0x4bab6e) with the fall damage dvars

private:
    int64_t landTime_ = -1, damageTime_ = 0, stepTime_ = 0;
    float stepChange_ = 0;
    float landChange_ = 0, damagePitch_ = 0, damageRoll_ = 0;
};


// The view model's two aim layers (adsUpAnim at time = aim fraction, adsDownAnim at time = 1 - aim fraction) are both in
// the anim tree; every frame 0x4b28e0 calls 0x4b25e0, which sets the wanted one's goal weight to 1 over 0.5 s (0x478f10) and
// clears the other over 0.5 s, so the layers cross-fade linearly instead of switching. Weights below follow the same
// goal-weight rules as AnimState::setWeight / clearNode.
class AimBlend {
public:
    void reset() { *this = AimBlend(); } // a new weapon's tree starts at weight 0 (TODO(spec): 0x4b2c90 reset timing)
    void update(bool aiming, float dt);
    float up() const { return up_.w; }
    float down() const { return down_.w; }

private:
    struct W { float w = 0, goal = 0, left = 0; };
    static void set(W& s, float goal, float blend);
    static void clear(W& s, float blend);
    static void step(W& s, float dt);
    W up_, down_;
};

} // namespace iw2::game
