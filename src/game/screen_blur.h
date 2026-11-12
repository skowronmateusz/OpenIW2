// Client full-screen blur state: the commands scr_blur / clear_blur (exe 0x4beb80,
// 0x4bebf0), the per-frame lerp (0x4beea0) and the renderer's cross-fade factor (gfx dll 0x10030ed0 / 0x10030b50).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace iw2::game {

// F of the alpha ramp alpha = min(R / F, 1): gfx dll 0x10030b50 calls import 0x101d4c4c(3.0f) = exe 0x440cd0 (ri slot 157 filled at
// 0x40d9bd..), which returns 3.0 * [0x793950]. The viewport setup 0x440b70 (called with the window size 0x617e1c / 0x617e20 at 0x4a333f)
// stores there 640 / (height * 4/3 / pixelAspect), the inverse of the 640-virtual-pixel scale of the 4:3 area: F is 3 real pixels in
// virtual units, so it shrinks as the window grows. pixelAspect (0x617e30, gfx 0x101d4cbc) is 1 here: pixels are square.
inline float blurFullRadius(float heightPx) { return 3.0f * 480.0f / heightPx; }
// 0x10030b50: below F the image is still built with radius F, only the alpha ramps.
inline float blurImageRadius(float radius, float heightPx) { return std::max(radius, blurFullRadius(heightPx)); }

struct ScreenBlur {
    int64_t start = 0, end = 0; // 0x4beb80: start = now, end = now + ms (start == 0: no lerp running)
    float from = 0, target = 0, cur = 0;
    bool realtime = false;
    int priority = 0;

    // scr_blur <ms> <radius> <realtime> <priority>; nowMs is level time (or real ms when realtime).
    // Ignored when priority is lower than the current one (0x4beb80).
    void command(int ms, float radius, bool rt, int prio, int64_t nowMs, float composedFrom) {
        if (prio < priority) return;
        start = nowMs;
        end = nowMs + ms;
        from = composedFrom; // 0xf79188, the last composed blur
        target = radius;
        realtime = rt;
        priority = prio;
    }
    // clear_blur (0x4bebf0): zero everything.
    void clear() { *this = ScreenBlur{}; }
    // 0x4beea0, once per frame; ms == 0 jumps straight to the target.
    void update(int64_t levelMs, int64_t realMs) {
        if (start == 0) return;
        const int64_t now = realtime ? realMs : levelMs;
        if (now < end) {
            const float t = float(now - start) / float(end - start);
            cur = (1.0f - t) * from + t * target;
        } else {
            cur = target;
            start = end = 0;
            priority = 0;
        }
    }
    // 0x4af3d0: sqrt(cur^2 + [0xf48b88]^2 + x^2). The other two terms are 0. No instruction stores to either address
    // directly, so they are fields written through a base pointer; x is only used while the UI key catcher is up (0x617f04 bit 8).
    float composed() const { return cur; }
};

// Menu blur (docs/spec/ui.md "blurWorld"): every painted menu with a non-zero blurWorld adds its radius as
// R = sqrt(R^2 + blurWorld^2) (Menu_Paint, MP 0x5425c6..0x5425f4) to the UI context's radius, which is cleared before the
// menus are painted; the client then composes sqrt(ui^2 + scr_blur^2) into the scene blur (MP 0x4cf900..0x4cf958).
template <class Range>
float menuBlurRadius(const Range& blurWorlds) {
    float r = 0;
    for (float b : blurWorlds)
        if (b != 0.0f) r = std::sqrt(r * r + b * b);
    return r;
}
inline float composeSceneBlur(float uiRadius, float scrRadius) { return std::sqrt(uiRadius * uiRadius + scrRadius * scrRadius); }

// Renderer side: R = sqrt(r_blur^2 + radius^2) with r_blur = 0; the blurred copy is blended with alpha min(R / F, 1).
inline float blurAlpha(float radius, float heightPx) {
    return radius <= 0 ? 0.0f : std::min(radius / blurFullRadius(heightPx), 1.0f);
}

} // namespace iw2::game
