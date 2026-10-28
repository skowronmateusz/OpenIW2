// Renderer far plane (farPlaneDistance, gfx dll 0x1000aaa0), the effect far-plane test and the shadow-caster score (0x10028150).
#include "game/fog.h"
#include "game/fx.h"
#include <cstdio>

using namespace iw2::game;

int main() {
    int bad = 0;
    auto chk = [&](bool c, int line) {
        if (!c) {
            std::printf("line %d failed\n", line);
            ++bad;
        }
    };
    chk(farPlaneDistance(false, 0, 0, 0) == 0.0f, __LINE__);         // nothing set: no far plane
    chk(farPlaneDistance(true, 3000, 0, 0) == 3000.0f, __LINE__);    // linear fog far
    chk(farPlaneDistance(true, 3000, 0, 5000) == 5000.0f, __LINE__); // setculldist larger
    chk(farPlaneDistance(true, 3000, 0, 1000) == 3000.0f, __LINE__); // max of both
    chk(farPlaneDistance(true, 3000, 2000, 0) == 2000.0f, __LINE__); // r_zfar replaces the fog far
    chk(farPlaneDistance(false, 3000, 0, 700) == 700.0f, __LINE__);  // no fog: cull distance alone
    chk(shadowCasterScore(0x8000, 500, 10) == 0.0f, __LINE__);       // always
    chk(shadowCasterScore(0x6000, 400, 10) == 10.0f, __LINE__);      // high_priority x0.25
    chk(shadowCasterScore(0x4000, 400, 10) == 160.0f, __LINE__);     // low_priority x4
    chk(shadowCasterScore(0, 400, 10) == 40.0f, __LINE__);
    FxSystem fx;
    fx.viewValid = true;
    fx.viewOrigin = {100, 0, 0};
    fx.viewForward = {1, 0, 0};
    chk(!fx.beyondFar({9999, 0, 0}, 0), __LINE__); // farDist 0 = disabled
    fx.farDist = 1000;
    chk(!fx.beyondFar({1100, 0, 0}, 0), __LINE__); // depth exactly 1000
    chk(fx.beyondFar({1101, 0, 0}, 0), __LINE__);
    chk(!fx.beyondFar({1101, 0, 0}, 5), __LINE__);  // radius widens the limit
    chk(!fx.beyondFar({-5000, 0, 0}, 0), __LINE__); // behind the eye is never beyond the far plane
    chk(!fx.beyondFar({500, 5000, 0}, 0), __LINE__); // depth is along the view direction only
    std::printf(bad ? "FAIL\n" : "farplane: ok\n");
    return bad ? 1 : 0;
}
