// Client full-screen blur state: the commands scr_blur / clear_blur (exe 0x4beb80,
// 0x4bebf0), the per-frame lerp (0x4beea0) and the renderer's cross-fade factor (gfx dll 0x10030ed0 / 0x10030b50).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace iw2::game {

// F of the alpha ramp alpha = min(R / F, 1): gfx dll 0x10030b50 calls import 0x101d4c4c(3.0f) = exe 0x440cd0 (ri slot 157 filled at
// 0x40d9bd..), which returns 3.0 * [0x793950]; 0x793950 = scale * (1 / 640) stored at 0x440c2a in the viewport setup 0x440b70
// (called with the window size 0x617e1c / 0x617e20 at 0x4a333f), i.e. the horizontal 640-virtual-pixel scale.
// TODO(spec): the exact argument of 0x440c20 (assumed the window width, scale = width / 640).
inline float blurFullRadius(float widthPx) { return 3.0f * widthPx / 640.0f; }

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
    // 0x4af3d0: sqrt(cur^2 + [0xf48b88]^2 + x^2). TODO(spec): the other two terms (meaning unknown) are taken as 0.
    float composed() const { return cur; }
};

// Renderer side: R = sqrt(r_blur^2 + radius^2) with r_blur = 0; the blurred copy is blended with alpha min(R / F, 1).
inline float blurAlpha(float radius, float widthPx) {
    return radius <= 0 ? 0.0f : std::min(radius / blurFullRadius(widthPx), 1.0f);
}

} // namespace iw2::game
