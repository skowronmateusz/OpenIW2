#include "game/screen_blur.h"
#include <cstdio>
using namespace iw2::game;
static int fails = 0;
static void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL %s\n", what);
        ++fails;
    }
}
int main() {
    ScreenBlur b;
    b.command(1000, 32.0f, false, 2, 100, b.composed());
    b.update(600, 0); // t = 0.5
    check(std::abs(b.cur - 16.0f) < 1e-4f, "half way");
    b.command(1000, 0.0f, false, 1, 600, b.composed()); // lower priority ignored
    check(b.target == 32.0f, "priority");
    b.update(1100, 0);
    check(b.cur == 32.0f && b.start == 0 && b.priority == 0 && b.target == 32.0f, "end keeps target");
    b.command(0, 5.0f, false, 1, 2000, b.composed()); // ms == 0
    b.update(2000, 0);
    check(b.cur == 5.0f, "instant");
    b.command(100, 1.0f, true, 1, 50, b.composed()); // realtime ignores level time
    b.update(9999, 100);
    check(std::abs(b.cur - 3.0f) < 1e-4f, "realtime lerp from 5 to 1 at t=.5");
    b.clear();
    check(b.cur == 0 && b.target == 0 && b.priority == 0, "clear");
    check(blurAlpha(0, 640) == 0 && std::abs(blurAlpha(1.5f, 640) - 0.5f) < 1e-6f && blurAlpha(32, 640) == 1.0f && std::abs(blurAlpha(3.75f, 1600) - 0.5f) < 1e-6f, "alpha");
    return fails;
}
