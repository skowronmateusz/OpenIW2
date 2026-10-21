// View weapon offsets, following CoD2SP_s.exe 1.3 (docs/spec/weapons.md): 0x4b5380 (position), 0x4b5730
// (rotation), 0x4ae7d9 (aiming field of view). The cg_gun_* dvars default to 0 and are left out.
#include "game/viewweapon.h"

#include <algorithm>

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

void ViewWeaponMotion::update(const Field& w, int ms, float xySpeed, float maxSpeed, float viewHeight, float adsFrac) {
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

} // namespace iw2::game
