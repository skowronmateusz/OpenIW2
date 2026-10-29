// Near / far plane box cull of the gfx renderer (viewcull.h) and the shadow-cookie budget controller (shadowcookie.h).
#include "game/shadowcookie.h"
#include "game/viewcull.h"
#include <cmath>
#include <cstdio>
#include <utility>

using namespace iw2::game;

int main() {
    int bad = 0;
    auto chk = [&](bool c, int line) {
        if (!c) {
            std::printf("line %d failed\n", line);
            ++bad;
        }
    };
    const float eye[3] = {100, 0, 0}, fwd[3] = {1, 0, 0};
    auto box = [&](float x0, float x1) {
        const float mn[3] = {x0, -10, -10}, mx[3] = {x1, 10, 10};
        return std::pair<bool, bool>{boxCulledByDepthPlanes(mn, mx, eye, fwd, 0.0f), boxCulledByDepthPlanes(mn, mx, eye, fwd, 1000.0f)};
    };
    chk(!box(150, 160).first && !box(150, 160).second, __LINE__);     // in front
    chk(!box(5000, 6000).first, __LINE__);                            // no far plane: never culled by depth
    chk(box(5000, 6000).second, __LINE__);                            // beyond the far plane (nearest corner depth 4900 >= 1000)
    chk(!box(900, 5000).second, __LINE__);                            // straddles depth 800..: nearest corner inside
    chk(box(1100, 1200).second, __LINE__);                            // nearest corner depth exactly 1000 is culled (<= 0 test)
    chk(!box(1099, 1200).second, __LINE__);
    chk(box(0, 99.8f).first, __LINE__);                               // wholly behind the eye (furthest corner depth -0.2 <= -0.1)
    chk(!box(0, 100.0f).first, __LINE__);                             // touches the eye plane: kept
    chk(!box(0, 99.95f).first, __LINE__);                             // furthest corner depth -0.05 is above the -0.1 limit
    // A diagonal view: positive vertex selection per axis.
    const float eye2[3] = {0, 0, 0}, f2[3] = {0.6f, 0.8f, 0.0f};
    const float mn[3] = {1000, 1000, -5}, mx[3] = {1010, 1010, 5};
    chk(boxCulledByDepthPlanes(mn, mx, eye2, f2, 500.0f), __LINE__);  // depth >= 1400
    chk(!boxCulledByDepthPlanes(mn, mx, eye2, f2, 5000.0f), __LINE__);

    // Shadow cookie controller.
    ShadowCookieParams p;
    chk(shadowCookieFade(1.0f, 10.0f, p) == 1.0f, __LINE__);   // below (1 - 0.25) * 10
    chk(shadowCookieFade(7.5f, 10.0f, p) == 1.0f, __LINE__);   // exactly at the ramp start
    chk(shadowCookieFade(10.0f, 10.0f, p) == 0.0f, __LINE__);  // at T
    chk(std::abs(shadowCookieFade(8.75f, 10.0f, p) - 0.5f) < 1e-6f, __LINE__);
    chk(shadowCookieFade(0.0f, 10.0f, p) == 1.0f, __LINE__);
    {
        ShadowCookieState st;
        ShadowCookieCaster c[24];
        for (int i = 0; i < 24; ++i) c[i] = {i, float(i) * 0.1f};
        ShadowCookieResult r = stepShadowCookies(st, c, 24, 1000, p); // all 24 pass, first call has no time delta
        chk(r.count == 24 && st.threshold == 10.0f && st.lastMs == 1000, __LINE__);
        r = stepShadowCookies(st, c, 24, 1500, p); // 24 used: T falls by 0.5 s * 2
        chk(std::abs(st.threshold - 9.0f) < 1e-5f, __LINE__);
        ShadowCookieCaster few[3] = {{1, 1.0f}, {2, 2.0f}, {3, 20.0f}}; // 2 pass, 3rd above T: 2 < 12 - 1
        r = stepShadowCookies(st, few, 3, 2500, p);
        chk(r.count == 2 && r.entity[1] == 2, __LINE__);
        chk(std::abs(st.threshold - 14.0f) < 1e-5f, __LINE__); // + 1 s * 5
        ShadowCookieCaster term[3] = {{1, 1.0f}, {-1, 0.0f}, {3, 2.0f}}; // list ends early: T unchanged
        const float before = st.threshold;
        r = stepShadowCookies(st, term, 3, 3500, p);
        chk(r.count == 1 && st.threshold == before, __LINE__);
        st.threshold = 0.00001f; // clamp
        const ShadowCookieCaster none[1] = {{-1, 0.0f}};
        stepShadowCookies(st, none, 1, 3600, p);
        chk(st.threshold == 1.0e-4f, __LINE__);
    }
    {
        ShadowCookieState st;
        ShadowCookieCaster c[14];
        for (int i = 0; i < 14; ++i) c[i] = {i, i < 13 ? 1.0f : 50.0f};
        stepShadowCookies(st, c, 14, 0, p); // 13 pass: 13 <= 12 + 1, inside the band
        stepShadowCookies(st, c, 14, 1000, p);
        chk(st.threshold == 10.0f, __LINE__);
        ShadowCookieCaster d[15];
        for (int i = 0; i < 15; ++i) d[i] = {i, i < 14 ? 1.0f : 50.0f};
        stepShadowCookies(st, d, 15, 2000, p); // 14 pass: 14 > 13, shrink
        chk(std::abs(st.threshold - 8.0f) < 1e-5f, __LINE__);
    }
    std::printf(bad ? "FAIL\n" : "viewcull: ok\n");
    return bad ? 1 : 0;
}
