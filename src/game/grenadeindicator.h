// HUD grenade indicator (cgame 0x4be210 loop, 0x4be120 visibility, 0x4bde50 pointer, 0x4bdfc0 icon, 0x4b0490 rotated quad).
// Pure geometry, no rendering.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace iw2::game {

// The dvars cg_hudGrenadeIcon* / cg_hudGrenadePointer* (registered 0x4a1607..0x4a1751) with their exe defaults.
struct GrenadeIndicatorCfg {
    float maxRange = 250.0f;   // cg_hudGrenadeIconMaxRange  0xf48d00
    float maxHeight = 104.0f;  // cg_hudGrenadeIconMaxHeight 0x1040bb0
    float iconOffset = 50.0f;  // cg_hudGrenadeIconOffset    0xf569f8
    float iconWidth = 25.0f;   // cg_hudGrenadeIconWidth     0xf56990
    float iconHeight = 25.0f;  // cg_hudGrenadeIconHeight    0x1040bf0
    float pointerWidth = 25.0f;  // cg_hudGrenadePointerWidth  0x1040bf8
    float pointerHeight = 12.0f; // cg_hudGrenadePointerHeight 0x1043080
    float pivotX = 12.0f;      // cg_hudGrenadePointerPivot  0xf7eb50 (vec2)
    float pivotY = 27.0f;
    float pulseFreq = 1.7f;    // cg_hudGrenadePointerPulseFreq 0xf56a00
    float pulseMax = 1.85f;    // cg_hudGrenadePointerPulseMax  0xf48b44
    float pulseMin = 0.3f;     // cg_hudGrenadePointerPulseMin  0x1040c20
};

// 0x4be120. `base` / `delta` are the entity state's pos.trBase (es+0x20) / pos.trDelta (es+0x2c); `origin` is the predicted player
// origin (ps+0x14, 0xf708fc) and `viewHeight` ps+0xd4 (0xf709bc). Shown only for a moving grenade (|delta|^2 > 1), within
// cg_hudGrenadeIconMaxHeight below the player, within MaxHeight above the eye height, and within MaxRange (3D).
inline bool grenadeIndicatorVisible(const GrenadeIndicatorCfg& c, const std::array<float, 3>& base, const std::array<float, 3>& delta,
                                    const std::array<float, 3>& origin, float viewHeight) {
    if (!(delta[0] * delta[0] + delta[1] * delta[1] + delta[2] * delta[2] > 1.0f)) return false;
    const float dx = base[0] - origin[0], dy = base[1] - origin[1], dz = base[2] - origin[2];
    if (dz >= 0.0f) {
        if (viewHeight + c.maxHeight < dz) return false;
    } else if (!(-c.maxHeight <= dz)) {
        return false;
    }
    return dx * dx + dy * dy + dz * dz <= c.maxRange * c.maxRange;
}

// Colour alpha of the loop (0x4be34f..0x4be3ab): clamp(min + (max - min) / 2 * (1 + sin(2 pi * freq * cg.time / 1000)), 0, 1).
inline float grenadeIndicatorAlpha(const GrenadeIndicatorCfg& c, int64_t cgTimeMs) {
    const float half = (c.pulseMax - c.pulseMin) * 0.5f;
    const float s = std::sin(float(cgTimeMs) * c.pulseFreq * 0.0062831854f); // 0x5921b4 = 2 pi / 1000
    const float v = half * s + (half + c.pulseMin);
    return v < 1.0f ? (v > 0.0f ? v : 0.0f) : 1.0f;
}

// Relative yaw of the grenade in degrees [0, 360), quantised like the exe (0x4bde50 / 0x4bdfc0): vectoyaw(delta) - view yaw,
// times 182.0444, truncated, & 0xffff, times 360 / 65536. Positive = the grenade is to the left of the view direction.
inline float grenadeIndicatorAngle(float dx, float dy, float viewYawDeg) {
    const float yaw = (dx == 0.0f && dy == 0.0f) ? 0.0f : std::atan2(dy, dx) * 57.29578f;
    const int a16 = int((yaw - viewYawDeg) * 182.04444885f) & 0xffff;
    return float(a16) * 0.0054931640625f;
}

struct GrenadeIndicatorQuad {
    std::array<float, 8> xy{}; // corners TL, TR, BR, BL (x, y pairs), screen pixels, y down
};

struct GrenadeIndicatorLayout {
    GrenadeIndicatorQuad icon;    // hud_grenadeicon
    GrenadeIndicatorQuad pointer; // hud_grenadepointer
};

// Screen layout. (cx, cy) = the screen centre in pixels, sx / sy = pixels per virtual unit (0x793940 / 0x793944), angleDeg from
// grenadeIndicatorAngle. P = (cx - r sin a, cy - r cos a) with r = sy * iconOffset. The icon is the axis-aligned box of
// sx * iconWidth by sy * iconHeight centred on P (0x4bdfc0). The pointer is the box [-pivot, size - pivot] (scaled by sx / sy)
// rotated about P by -a degrees (clockwise positive, y down) with x' = cos(-a) x - sin(-a) y + Px (0x4b0490, called with the
// negated angle by 0x4bde50).
inline GrenadeIndicatorLayout grenadeIndicatorLayout(const GrenadeIndicatorCfg& c, float cx, float cy, float sx, float sy,
                                                     float angleDeg) {
    const float rad = angleDeg * 0.017453292f;
    const float s = std::sin(rad), co = std::cos(rad);
    const float r = sy * c.iconOffset;
    const float px = cx - s * r, py = cy - co * r;
    GrenadeIndicatorLayout out;
    {
        const float hw = sx * c.iconWidth * 0.5f, hh = sy * c.iconHeight * 0.5f;
        const float x0 = px - hw, x1 = px + hw, y0 = py - hh, y1 = py + hh;
        out.icon.xy = {x0, y0, x1, y0, x1, y1, x0, y1};
    }
    {
        const float w = sx * c.pointerWidth, h = sy * c.pointerHeight, pvx = sx * c.pivotX, pvy = sy * c.pivotY;
        const float rx[4] = {-pvx, w - pvx, w - pvx, -pvx}, ry[4] = {-pvy, -pvy, h - pvy, h - pvy};
        const float ra = -angleDeg * 0.017453292f, rs = std::sin(ra), rc = std::cos(ra);
        for (int k = 0; k < 4; ++k) {
            out.pointer.xy[size_t(k) * 2] = rc * rx[k] - rs * ry[k] + px;
            out.pointer.xy[size_t(k) * 2 + 1] = rs * rx[k] + rc * ry[k] + py;
        }
    }
    return out;
}

} // namespace iw2::game
