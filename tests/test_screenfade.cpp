#include "game/screenfade.h"

#include <cmath>
#include <iostream>

int main() {
    using iw2::game::ScreenFade;
    int failures = 0;
    auto check = [&](bool c, int line) {
        if (!c) {
            std::cerr << "line " << line << " failed\n";
            ++failures;
        }
    };
    ScreenFade f;
    f.update(0, 0);
    check(f.alpha() == 0, __LINE__);
    f.command(1.0f, 250, 750, 0); // fade to black over 750 ms
    f.update(0, 16);              // 16 ms of 750
    check(std::fabs(f.alpha() - 16.0f / 750.0f) < 1e-6f, __LINE__);
    f.update(100, 116); // +100 ms
    check(std::fabs(f.alpha() - 116.0f / 750.0f) < 1e-6f, __LINE__);
    f.update(200, 1000); // an 884 ms jump is skipped (>= 500)
    check(std::fabs(f.alpha() - 116.0f / 750.0f) < 1e-6f, __LINE__);
    f.update(2000, 1016); // past start + duration: the target
    check(f.alpha() == 1.0f, __LINE__);
    f.command(0.0f, 3000, 500, 2000); // back to clear
    f.update(2100, 1100);
    check(f.alpha() < 1.0f && f.alpha() > 0.0f, __LINE__);
    f.command(0.0f, 0, 10, 5000); // already over: immediate
    check(f.alpha() == 0.0f, __LINE__);
    return failures ? 1 : 0;
}
