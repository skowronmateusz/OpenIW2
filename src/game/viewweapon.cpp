// View weapon offsets, following CoD2SP_s.exe 1.3 (docs/spec/weapons.md): 0x4b5380 (position), 0x4b5730
// (rotation), 0x4ae7d9 (aiming field of view). The cg_gun_* dvars default to 0 and are left out.
#include "game/viewweapon.h"

#include <algorithm>
#include <cmath>

namespace iw2::game {
namespace {

// Smooths `cur` towards `target` (shared step of 0x4b5380/0x4b5730): delta·rate·ms/1000, at least 0.0001·ms per
// call, without overshooting.
void approach(V3& cur, const V3& target, float rate, int ms) {
    for (size_t k = 0; k < 3; ++k) {
        if (cur[k] == target[k]) continue;
        float step = (target[k] - cur[k]) * rate * float(ms) * 0.001f;
        if (target[k] > cur[k]) {
            step = std::max(step, float(ms) * 0.0001f);
            cur[k] = std::min(cur[k] + step, target[k]);
        } else {
            step = std::min(step, float(ms) * -0.0001f);
            cur[k] = std::max(cur[k] + step, target[k]);
        }
    }
}

// The smoothed offset is added fully when not aiming, faded by (1 − 2·adsFrac) below half aim, and not at all above.
V3 faded(const V3& v, float adsFrac) {
    const float f = adsFrac == 0 ? 1.0f : adsFrac < 0.5f ? 1.0f - 2.0f * adsFrac : 0.0f;
    return {v[0] * f, v[1] * f, v[2] * f};
}

} // namespace

void ViewWeaponMotion::update(const Field& w, int ms, float xySpeed, float maxSpeed, float viewHeight, float adsFrac, float lean) {
    const bool prone = viewHeight <= 11.0f, crouched = !prone && viewHeight <= 40.0f;
    const char* st = prone ? "prone" : crouched ? "ducked" : "stand";
    auto f = [&](const char* suffix) { return w((std::string(st) + suffix).c_str()); };

    // Position: a movement offset that grows with speed above the stance's minimum speed, plus the static crouch
    // or prone offset.
    {
        const float minSpeed = std::max(0.0f, f("MoveMinSpeed"));
        V3 target{0, 0, 0};
        if (xySpeed > minSpeed && maxSpeed > minSpeed) {
            const float frac = std::min(1.0f, (xySpeed - minSpeed) / (maxSpeed - minSpeed));
            target = {frac * f("MoveF"), frac * f("MoveR"), frac * f("MoveU")};
        }
        // The static offsets apply at the resting view heights 40 and 11 (the original compares the integer
        // current view height).
        if (int(viewHeight) == 40) { target[0] += w("duckedOfsF"); target[1] += w("duckedOfsR"); target[2] += w("duckedOfsU"); }
        if (int(viewHeight) == 11) { target[0] += w("proneOfsF"); target[1] += w("proneOfsR"); target[2] += w("proneOfsU"); }
        approach(curMove_, target, prone ? w("posProneMoveRate") : w("posMoveRate"), ms);
        outMove_ = faded(curMove_, adsFrac);
    }
    // Rotation: like the movement offset, scaled by (1 − adsFrac) before smoothing.
    {
        const float minSpeed = f("RotMinSpeed");
        V3 target{0, 0, 0};
        if (xySpeed > minSpeed && maxSpeed > minSpeed) {
            const float frac = std::clamp((xySpeed - minSpeed) / (maxSpeed - minSpeed), 0.0f, 1.0f);
            target = {frac * f("RotP"), frac * f("RotY"), frac * f("RotR")};
        }
        if (adsFrac != 0)
            for (float& x : target) x *= 1.0f - adsFrac;
        approach(curRot_, target, prone ? w("posProneRotRate") : w("posRotRate"), ms);
        outRot_ = faded(curRot_, adsFrac);
    }
    // Lean: g = (2 − |lean|)·lean. Position (0x4b61d0) while not fully aimed: + (0, −cos r, −sin r)·(1 − ads)·g·1.6
    // with r = −2g degrees; roll (0x4b6420) += −2g.
    if (lean != 0) {
        const float g = (2.0f - std::fabs(lean)) * lean;
        if (adsFrac < 1.0f) {
            const float r = -2.0f * g * 3.14159265f / 180.0f, k = (1.0f - adsFrac) * g * 1.6f;
            outMove_[1] += -std::cos(r) * k;
            outMove_[2] += -std::sin(r) * k;
        }
        outRot_[2] += -2.0f * g;
    }
}

// Dvars: cg_bobWeaponLag 0.25, cg_bobWeaponAmplitude 0.16, cg_bobWeaponRollAmplitude 1.5, cg_bobWeaponMax 10,
// bg_bobAmplitudeProne 0.03, bg_bobAmplitudeDucked 0.0075, bg_bobAmplitudeStanding 0.007 (registered at 0x4a1xxx
// and 0x4fab..). The cycle (0x4af4a8) is bobCycle / 255 · 2π + 2π; c = lag · π + that + 2π. With
// a = min(speed · amplitude · stance amplitude, max): pitch = −(sin 2c + sin(4c + π/2) · 0.2) · a · 0.75 (0x4adf90),
// yaw = −sin c · a, roll = min(0, sin(c − 0.4712389) · min(speed · amplitude · 1.5 · stance amplitude, max)).
// Aiming scales all by 1 − (1 − adsBobFactor) · adsFrac.
V3 gunBob(int bobCycle, float xySpeed, int viewHeightTarget, float adsFrac, float adsBobFactor, bool reticle) {
    constexpr float kPi = 3.1415927410125732f, kTwoPi = 6.2831854820251465f;
    const float cyc = float(bobCycle & 0xff) / 255.0f * kTwoPi + kTwoPi;
    const float c = 0.25f * kPi + cyc + kTwoPi;
    const float amp = xySpeed * 0.16f;
    const float stance = viewHeightTarget == 11 ? 0.03f : viewHeightTarget == 40 ? 0.0075f : 0.007f;
    const float a = std::min(amp * stance, 10.0f);
    V3 b{-(std::sin(c + c) + std::sin(c * 4.0f + 1.5707963705062866f) * 0.2f) * a * 0.75f, -(std::sin(c) * a),
         std::min(0.0f, std::sin(c - 0.4712389409542084f) * std::min(amp * 1.5f * stance, 10.0f))};
    if (adsFrac != 0) {
        const float k = 1.0f - (1.0f - adsBobFactor) * adsFrac;
        for (float& x : b) x *= k;
    }
    if (reticle)
        for (float& x : b) x *= 1.0f - adsFrac;
    return b;
}

V3 scopedViewBob(const V3& rawBob, float adsFrac, bool reticle) {
    if (!reticle) return {0, 0, 0};
    return {rawBob[0] * adsFrac, rawBob[1] * adsFrac, rawBob[2] * adsFrac};
}

namespace {
// x87 float to int as the exe's _ftol (0x565994): truncation towards zero.
int ftol(float v) { return int(v); }
float breathDiffTrack(float target, float cur, float rate, float dt); // DiffTrack 0x420a20, defined below
} // namespace

bool BreathSounds::update(bool holding, int frameMs, const BreathSoundParams& p, const Play& play) {
    if (delay_ > 0) delay_ -= frameMs;
    bool heartbeat = false;
    if (holding) {
        blend_ = breathDiffTrack(1.0f, blend_, p.sndLerp, float(frameMs) * 0.001f);
        if (timer_ < 0) { // the hold just started
            timer_ = 0;
            if (delay_ > 0) heartbeatAt_ = 0;
            else {
                heartbeatAt_ = play("weap_sniper_breathin");
                delay_ = ftol(p.sndDelay * 1000.0f);
            }
        } else if (timer_ > heartbeatAt_) {
            play("weap_sniper_heartbeat");
            heartbeat = true;
        }
        timer_ += frameMs;
        return heartbeat;
    }
    if (timer_ >= 0) { // the hold just ended
        timer_ += frameMs;
        if (timer_ > ftol(p.holdTime * 1000.0f)) play("weap_sniper_breathgasp");
        else if (delay_ <= 0) {
            play("weap_sniper_breathout");
            delay_ = ftol(p.sndDelay * 1000.0f);
        }
    }
    blend_ = 0;
    timer_ = -1;
    heartbeatAt_ = 0;
    return false;
}

namespace {
float stanceBobAmplitude(int vht) { return vht == 11 ? 0.03f : vht == 40 ? 0.0075f : 0.007f; }
// 0x4adf90: (sin 2c + sin(4c + π/2) · 0.2) · min(speed · stance amplitude, max) · 0.75.
float bobWave(float c, float speed, int vht, float cap) {
    const float a = std::min(speed * stanceBobAmplitude(vht), cap);
    return (std::sin(c + c) + std::sin(c * 4.0f + 1.5707963705062866f) * 0.2f) * a * 0.75f;
}
// 0x4ae010: sin c · min(speed · stance amplitude, max).
float bobSide(float c, float speed, int vht, float cap) { return std::sin(c) * std::min(speed * stanceBobAmplitude(vht), cap); }
} // namespace

std::array<float, 4> viewBob(int bobCycle, float xySpeed, int vht, float adsFrac, float adsViewBobMult) {
    constexpr float kTwoPi = 6.2831854820251465f;
    const float c = float(bobCycle & 0xff) / 255.0f * kTwoPi + kTwoPi; // 0x4af4a8
    std::array<float, 4> r{bobWave(c, xySpeed, vht, 8.0f), bobSide(c, xySpeed, vht, 8.0f), 0, 0};
    if (adsFrac != 0 && adsViewBobMult != 0) {
        r[2] = -adsFrac * adsViewBobMult * bobWave(c, xySpeed, vht, 45.0f);
        r[3] = -adsFrac * adsViewBobMult * bobSide(c, xySpeed, vht, 45.0f);
    }
    return r;
}

void ViewImpulses::damage(int yawByte, int pitchByte, int count, const V3& fwd, const V3& left, int64_t timeMs) {
    float kick = float(count) * 0.6f; // cg_viewKickScale
    if (kick < 5.0f) kick = 5.0f;
    if (kick > 90.0f) kick = 90.0f; // cg_viewKickMax
    if (yawByte == 0xff && pitchByte == 0xff) {
        damagePitch_ = -kick;
        damageRoll_ = 0;
    } else {
        // v = -AngleVectors(pitch byte * 360/255, yaw byte * 360/255) (0x4ba32d-0x4ba41b).
        const float rad = 0.01745329238474369f;
        const float a40 = float(yawByte) / 255.0f * 360.0f * rad, a44 = float(pitchByte) / 255.0f * 360.0f * rad;
        const V3 v{-std::cos(a44) * std::cos(a40), -std::cos(a44) * std::sin(a40), std::sin(a44)};
        damageRoll_ = (left[0] * v[0] + left[1] * v[1] + left[2] * v[2]) * kick;
        damagePitch_ = -(fwd[0] * v[0] + fwd[1] * v[1] + fwd[2] * v[2]) * kick;
    }
    damageTime_ = timeMs;
}

int ViewImpulses::landDip(float h) {
    if (!(h > 12.0f)) return 0;
    return std::min(24, int(((h - 12.0f) * 0.03846153989434242f + 1.0f) * 4.0f));
}

int ViewImpulses::dipFromDamage(int damage) {
    constexpr float kMinH = 200.0f, kMaxH = 350.0f; // bg_fallDamageMinHeight / MaxHeight
    return landDip((kMaxH - kMinH) * float(damage) * 0.01f + kMinH);
}

void ViewImpulses::land(float fallHeight, int fallDamage, int64_t timeMs) {
    const int dip = fallDamage > 0 ? dipFromDamage(fallDamage) : landDip(fallHeight);
    if (dip <= 0) return;
    landChange_ = -float(dip);
    landTime_ = timeMs;
}

float ViewImpulses::landOffset(int64_t timeMs) {
    const float t = float(timeMs - landTime_);
    if (t < 0) landTime_ = timeMs - 450; // 0x4ae6e0
    if (t < 150.0f) return landChange_ * (t / 150.0f);
    if (t < 450.0f) return landChange_ * (1.0f - (t - 150.0f) / 300.0f);
    return 0;
}

void ViewImpulses::step(int parm, int64_t timeMs) {
    const int64_t dt = timeMs - stepTime_;
    const float carry = dt < 100 ? float(100 - dt) * stepChange_ * 0.009999999776482582f * 0.8999999761581421f : 0.0f;
    stepChange_ = carry + float(parm - 0x80);
    if (stepChange_ > 24.0f) stepChange_ = 24.0f;
    else if (stepChange_ < -16.0f) stepChange_ = -16.0f;
    stepTime_ = timeMs;
}

float ViewImpulses::stepOffset(int64_t timeMs) {
    int64_t dt = timeMs - stepTime_;
    if (dt < 0) stepTime_ = timeMs; // 0x4addc3 (dt itself is not recomputed)
    return dt < 100 ? float(100 - dt) * stepChange_ * 0.009999999776482582f : 0.0f;
}

std::array<float, 2> ViewImpulses::damageKick(int64_t timeMs, float adsFrac, bool reticle) const {
    if (damageTime_ == 0) return {0, 0};
    const float a = adsFrac * 0.5f;
    float b = 1.0f - a;
    if (adsFrac != 0 && reticle) b *= 1.0f + a;
    const float dt = float(timeMs - damageTime_);
    float f;
    if (dt < 100.0f) {
        const float x = dt / 100.0f;
        f = x * (2.0f - std::fabs(x));
    } else {
        const float x = (dt - 100.0f) / 400.0f;
        if (1.0f - x <= 0) return {0, 0};
        f = 1.0f - x * (2.0f - std::fabs(x));
    }
    f *= b;
    return {damagePitch_ * f, damageRoll_ * f};
}

void AimBlend::set(W& s, float goal, float blend) {
    if (s.w == 0) s.w = goal * 0.001f;
    if (goal < s.w) s.left = (s.w - goal) / s.w * blend;
    else s.left = (goal - s.w) / goal * blend;
    s.goal = goal;
}

void AimBlend::clear(W& s, float blend) {
    if (s.goal != 0 || blend < s.left) s.left = blend;
    s.goal = 0;
}

void AimBlend::step(W& s, float dt) {
    if (s.left <= dt) {
        s.w = s.goal;
        s.left = 0;
    } else {
        s.w += (s.goal - s.w) / s.left * dt;
        if (s.w < 1e-6f) s.w = s.goal * 0.001f;
        s.left -= dt;
    }
}

void AimBlend::update(bool aiming, float dt) {
    set(aiming ? up_ : down_, 1.0f, 0.5f);
    clear(aiming ? down_ : up_, 0.5f);
    step(up_, dt);
    step(down_, dt);
}

namespace {
// DiffTrack (0x420a20): moves cur towards target by (target − cur) · rate · dt unless that overshoots or the gap is
// below 0.001; then target.
float diffTrack(float target, float cur, float rate, float dt) {
    const float d = target - cur, step = d * rate * dt;
    if (std::fabs(d) > 0.001f && !(std::fabs(d) <= std::fabs(step))) return step + cur;
    return target;
}
// DiffTrackAngle (0x420a90): target wrapped to within 180 of cur, then DiffTrack.
float diffTrackAngle(float target, float cur, float rate, float dt) {
    while (target - cur > 180.0f) target -= 360.0f;
    while (target - cur < -180.0f) target += 360.0f;
    return diffTrack(target, cur, rate, dt);
}
float angleDelta(float a, float b) { return std::remainder(a - b, 360.0f); } // 0x423f50
} // namespace

namespace {
float breathDiffTrack(float target, float cur, float rate, float dt) { return diffTrack(target, cur, rate, dt); }
} // namespace

float shellShockRatio(int remainingMs, int fadeMs) {
    if (remainingMs <= 0) return 0;
    float r = 1.0f;
    if (remainingMs < fadeMs) r = float(remainingMs) / float(fadeMs);
    return r * (3.0f - r * 2.0f) * r; // 0x4b4e73-0x4b4e87
}

void ViewWeaponMotion::sway(const Field& w, int ms, float viewPitch, float viewYaw, float adsFrac, int stance, bool reticle,
                            float shock) {
    const bool adsWeapon = w("aimDownSight") != 0;
    // Idle sway (0x4b51a0): amount / speed hipIdle* (aiming weapons lerp to adsIdle* by the aim fraction; without
    // hipIdleAmount: 80 and 1), × the stance factor (idleProneFactor / idleCrouchFactor) the smoothed factor
    // approaches at 0.0005 per ms (kept while a reticle weapon aims), × (1 − adsFrac) for reticle weapons; time
    // advances by trunc(ms · speed); pitch/yaw/roll += sin(t · 0.001 / 0.0007 / 0.0005) · amount · 0.01.
    {
        float amount, speed;
        if (adsWeapon) {
            amount = w("hipIdleAmount") + (w("adsIdleAmount") - w("hipIdleAmount")) * adsFrac;
            speed = w("hipIdleSpeed") + (w("adsIdleSpeed") - w("hipIdleSpeed")) * adsFrac;
        } else if (w("hipIdleAmount") != 0) {
            amount = w("hipIdleAmount");
            speed = w("hipIdleSpeed");
        } else {
            amount = 80.0f;
            speed = 1.0f;
        }
        const float target = stance == 2 ? w("idleProneFactor") : stance == 1 ? w("idleCrouchFactor") : 1.0f;
        if (!(reticle && adsFrac != 0) && target != idleFactor_) {
            const float step = float(ms) * 0.0005f;
            idleFactor_ = target > idleFactor_ ? std::min(target, idleFactor_ + step) : std::max(target, idleFactor_ - step);
        }
        amount *= idleFactor_;
        if (reticle) amount *= 1.0f - adsFrac;
        idleTime_ += int(float(ms) * speed);
        outRot_[2] += std::sin(float(idleTime_) * 0.0005f) * amount * 0.01f;
        outRot_[1] += std::sin(float(idleTime_) * 0.0007f) * amount * 0.01f;
        outRot_[0] += std::sin(float(idleTime_) * 0.001f) * amount * 0.01f;
    }
    // View-lag sway (0x4b4dc0): the view angle change since the last frame, scaled to 60 fps and clamped to
    // swayMaxAngle, drives side/up offsets (swayHoriz/VertScale) and pitch/yaw (swayPitch/YawScale) tracked at
    // swayLerpSpeed; aiming weapons lerp to the ads* values, none while a reticle weapon aims. 0x4b61d0 subtracts the
    // side from the right offset and adds the up; 0x4b6420 subtracts pitch and yaw. The four scales are multiplied by
    // (swayShellShockScale - 1) * shock + 1 (0x4b4e95, 0x4b4fc8-0x4b5004). TODO(spec): shell shock itself (the shellshock
    // script builtin, ps+0x5bc/0x5c0/0x5c4) is not implemented, so iw2view passes 0.
    if (ms > 0 && !(adsFrac > 0 && reticle && adsWeapon)) {
        auto lerpF = [&](const char* hip, const char* ads) { return adsWeapon ? w(hip) + (w(ads) - w(hip)) * adsFrac : w(hip); };
        const float maxAng = lerpF("swayMaxAngle", "adsSwayMaxAngle"), rate = lerpF("swayLerpSpeed", "adsSwayLerpSpeed");
        const float shockScale0 = (w("swayShellShockScale") - 1.0f) * shock + 1.0f;
        const float pitchS = lerpF("swayPitchScale", "adsSwayPitchScale") * shockScale0, yawS = lerpF("swayYawScale", "adsSwayYawScale") * shockScale0;
        const float shockScale = (w("swayShellShockScale") - 1.0f) * shock + 1.0f;
        const float horizS = lerpF("swayHorizScale", "adsSwayHorizScale") * shockScale, vertS = lerpF("swayVertScale", "adsSwayVertScale") * shockScale;
        const float dt = float(ms) * 0.001f;
        if (!havePrev_) { prevPitch_ = viewPitch; prevYaw_ = viewYaw; havePrev_ = true; }
        const float k = 1.0f / (dt * 60.0f);
        const float dp = std::clamp(angleDelta(viewPitch, prevPitch_) * k, -maxAng, maxAng);
        const float dy = std::clamp(angleDelta(viewYaw, prevYaw_) * k, -maxAng, maxAng);
        lagSide_ = diffTrack(dy * horizS, lagSide_, rate, dt);
        lagUp_ = diffTrack(dp * vertS, lagUp_, rate, dt);
        lagPitch_ = diffTrackAngle(dp * pitchS, lagPitch_, rate, dt);
        lagYaw_ = diffTrackAngle(dy * yawS, lagYaw_, rate, dt);
        prevPitch_ = viewPitch;
        prevYaw_ = viewYaw;
    }
    outMove_[1] -= lagSide_;
    outMove_[2] += lagUp_;
    outRot_[0] = angleDelta(outRot_[0], lagPitch_);
    outRot_[1] = angleDelta(outRot_[1], lagYaw_);
}

std::array<float, 2> ViewWeaponMotion::scopeSway(const Field& w, int ms, float adsFrac, int stance, float breathScale, bool reticle) {
    if (!reticle) return {0, 0};
    // Amount / speed as the idle sway (0x4ae0a1-0x4ae0f6): aimDownSight weapons lerp hipIdle* (0x2cc / 0x2d4) to adsIdle*
    // (0x2c8 / 0x2d0) by the aim fraction; otherwise hip values, or 80 and 1 without hipIdleAmount.
    float amount, speed;
    if (w("aimDownSight") != 0) {
        amount = w("adsIdleAmount") * adsFrac + w("hipIdleAmount") * (1.0f - adsFrac);
        speed = w("adsIdleSpeed") * adsFrac + w("hipIdleSpeed") * (1.0f - adsFrac);
    } else if (w("hipIdleAmount") != 0) {
        amount = w("hipIdleAmount");
        speed = w("hipIdleSpeed");
    } else {
        amount = 80.0f;
        speed = 1.0f;
    }
    // The stance factor (0xf79068) approaches its target only while the aim fraction is non-zero (0x4ae125-0x4ae196).
    const float target = stance == 2 ? w("idleProneFactor") : stance == 1 ? w("idleCrouchFactor") : 1.0f;
    if (adsFrac != 0 && target != idleFactor_) {
        const float step = float(ms) * 0.0005f;
        idleFactor_ = target > idleFactor_ ? std::min(target, idleFactor_ + step) : std::max(target, idleFactor_ - step);
    }
    const float x = idleFactor_ * amount * adsFrac * breathScale;
    idleTime_ += int(std::lrint(double(float(ms) * breathScale * speed)));
    return {std::sin(float(idleTime_) * 0.001f) * x * 0.01f, std::sin(float(idleTime_) * 0.0007f) * x * 0.01f};
}

float adsFov(float cgFov, float adsZoomFov, float zoomInFrac, float zoomOutFrac, float adsFrac, bool zoomingIn) {
    const float zoom = std::min(adsZoomFov, cgFov);
    if (adsFrac == 1.0f) return zoom;
    if (adsFrac == 0.0f) return cgFov;
    const float frac = zoomingIn ? zoomInFrac : zoomOutFrac;
    if (frac <= 0) return cgFov;
    const float t = (adsFrac - (1.0f - frac)) / frac;
    if (t <= 0) return cgFov;
    return cgFov - (cgFov - zoom) * t;
}


// ---- Recoil ----
float RecoilKick::rand01() {
    // The exe uses the CRT rand() / 32768 (0x553d5d); a deterministic LCG stands in for it.
    seed_ = seed_ * 1103515245u + 12345u;
    return float((seed_ >> 16) & 0x7fff) / 32768.0f;
}

void RecoilKick::fire(const Field& w, float adsFrac, bool reducedKick) {
    auto lerp = [&](const char* mn, const char* mx) { return w(mn) + (w(mx) - w(mn)) * rand01(); };
    const bool adsView = adsFrac == 1.0f; // view kick: ADS block only at exactly 1.0
    const bool adsGun = adsFrac > 0.0f;   // gun kick: ADS block for any positive fraction
    float reduce = 1.0f;
    if (reducedKick) reduce = w(adsView ? "adsGunKickReducedKickPercent" : "hipGunKickReducedKickPercent") * 0.01f;
    const float vp = (adsView ? lerp("adsViewKickPitchMin", "adsViewKickPitchMax") : lerp("hipViewKickPitchMin", "hipViewKickPitchMax")) * reduce;
    const float vy = (adsView ? lerp("adsViewKickYawMin", "adsViewKickYawMax") : lerp("hipViewKickYawMin", "hipViewKickYawMax")) * reduce;
    viewVel_ = {-vp, vy, vy * -0.5f};
    const float gp = adsGun ? lerp("adsGunKickPitchMin", "adsGunKickPitchMax") : lerp("hipGunKickPitchMin", "hipGunKickPitchMax");
    const float gy = adsGun ? lerp("adsGunKickYawMin", "adsGunKickYawMax") : lerp("hipGunKickYawMin", "hipGunKickYawMax");
    gunVel_[0] += gp * reduce;
    gunVel_[1] += gy * reduce;
}

namespace {
// 0x4b5bc0: one step of the gun-kick integrator; returns true when at rest.
bool gunKickStep(float dt, float limit, float accel, float speedMax, float speedDecay, float staticDecay, float& pos, float& vel) {
    if (std::fabs(pos) < 0.25f && std::fabs(vel) < 1.0f) {
        pos = vel = 0;
        return true;
    }
    pos += dt * vel;
    if (pos > limit) {
        pos = limit;
        if (vel > 0) vel = 0;
    } else if (pos < -limit) {
        pos = -limit;
        if (vel < 0) vel = 0;
    }
    if (pos > 0) vel -= dt * accel;
    else if (pos < 0) vel += dt * accel;
    vel -= dt * vel * speedDecay;
    if (vel > 0) {
        vel -= dt * staticDecay;
        if (vel < 0) vel = 0;
    } else {
        vel += dt * staticDecay;
        if (vel > 0) vel = 0;
    }
    if (vel > speedMax) vel = speedMax;
    else if (vel < -speedMax) vel = -speedMax;
    return false;
}
} // namespace

void RecoilKick::update(const Field& w, int ms, bool hasWeapon, float adsFrac) {
    // View-kick spring (0x4ade00), steps of at most 5 ms.
    for (int rem = ms; rem > 0;) {
        const int step = std::min(rem, 5);
        rem -= step;
        const float dt = float(step) * 0.001f;
        for (int i = 0; i < 3; ++i) {
            float& v = viewVel_[size_t(i)];
            float& p = viewPos_[size_t(i)];
            if (v == 0 && p == 0) continue;
            if (p != 0) {
                const float dir = p > 0 ? -1.0f : 1.0f;
                const float speed = hasWeapon ? (adsFrac > 0.5f ? w("adsViewKickCenterSpeed") : w("hipViewKickCenterSpeed")) : 2400.0f;
                v += dir * speed * dt;
            }
            float x = dt * v;
            if (x * p < 0) x *= 0.06f;
            const float np = p + x;
            if (np * p < 0) {
                p = 0;
                v = 0;
            } else {
                p = np;
                if (np == 0) v = 0;
                else if (std::fabs(np) > 10.0f) {
                    p = np > 0 ? 10.0f : -10.0f;
                    v = 0;
                }
            }
        }
    }
    // Gun-kick spring (0x4b5d10): only for weapons with aimDownSight.
    if (w("aimDownSight") == 0) return;
    auto blend = [&](const char* hip, const char* ads) { return w(hip) + (w(ads) - w(hip)) * adsFrac; };
    const float accel = blend("hipGunKickAccel", "adsGunKickAccel"), speedMax = blend("hipGunKickSpeedMax", "adsGunKickSpeedMax");
    const float speedDecay = blend("hipGunKickSpeedDecay", "adsGunKickSpeedDecay");
    const float staticDecay = blend("hipGunKickStaticDecay", "adsGunKickStaticDecay");
    const float maxPitch = w("gunMaxPitch"), maxYaw = w("gunMaxYaw");
    for (int rem = ms; rem > 0;) {
        const int step = std::min(rem, 5);
        rem -= step;
        const float dt = float(step) * 0.001f;
        gunKickStep(dt, maxPitch, accel, speedMax, speedDecay, staticDecay, gunPos_[0], gunVel_[0]);
        gunKickStep(dt, maxYaw, accel, speedMax, speedDecay, staticDecay, gunPos_[1], gunVel_[1]);
    }
}

} // namespace iw2::game

namespace iw2::game {
// ---- Screen shake (0x4bf540 / 0x4bf5e0 / 0x4bf700) ----
V3 turretViewJitter(float vertJitter, float horizJitter, unsigned& seed) {
    auto crandom = [&seed]() {
        seed = seed * 1103515245u + 12345u; // the exe uses the CRT rand(); a deterministic LCG stands in for it
        return float((seed >> 16) & 0x7fff) / 32768.0f * 2.0f - 1.0f;
    };
    const float p = crandom() * vertJitter; // evaluation order: the pitch draw first (0x4aeead..0x4aeeb4)
    const float y = crandom() * horizJitter;
    return {p, y, 0};
}

float ViewShake::rand01() {
    seed_ = seed_ * 1103515245u + 12345u;
    return float((seed_ >> 16) & 0x7fff) / 32768.0f;
}

bool ViewShake::eval(Slot& s, int64_t nowMs, const V3& eye) const {
    const int64_t t = nowMs - s.start;
    if (t < 0 || !(float(t) < s.duration)) return false;
    const float dx = eye[0] - s.origin[0], dy = eye[1] - s.origin[1], dz = eye[2] - s.origin[2];
    const float a = 1.0f - std::sqrt(dx * dx + dy * dy + dz * dz) / s.radius;
    const float b = (1.0f - float(t) / s.duration) * s.scale;
    if (!(b > 0.0f)) return false;
    s.out1 = a < 0.0f ? a / b : a * b;
    s.b = b;
    return true;
}

void ViewShake::add(const V3& origin, float scale, int durationMs, float radius, int64_t nowMs, const V3& eye) {
    Slot n;
    n.start = nowMs;
    n.scale = scale;
    n.duration = float(durationMs);
    n.radius = radius;
    n.origin = origin;
    eval(n, nowMs, eye);
    int idx = -1;
    for (int i = 0; i < 4; ++i) {
        const Slot& s = slots_[i];
        if (s.start > nowMs || float(s.start) + s.duration <= float(nowMs)) { idx = i; break; }
    }
    if (idx < 0) {
        float running = n.out1;
        for (int i = 0; i < 4; ++i)
            if (slots_[i].out1 < running) { idx = i; running = slots_[i].out1; }
        if (idx < 0) return;
    }
    slots_[idx] = n;
}

V3 ViewShake::update(int64_t nowMs, const V3& eye) {
    float m = 0, b = 0;
    for (Slot& s : slots_)
        if (eval(s, nowMs, eye) && s.out1 > m) { m = s.out1; b = s.b; }
    if (!(m > 0.0f)) {
        phase_ = (rand01() * 2.0f - 1.0f) * 3.14159274f;
        return {0, 0, 0};
    }
    if (m > 1.0f) m = 1.0f;
    const float T = float(nowMs) / 600.0f;
    return {std::sin(T * 25.1327419f + phase_) * m * b * 18.0f, std::sin(T * 47.1238899f + phase_) * m * b * 16.0f,
            std::sin(T * 37.6991119f + phase_) * m * b * 10.0f};
}

} // namespace iw2::game
