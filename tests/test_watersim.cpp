#include "game/watersim.h"
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
    WaterParams p; // rhine description
    p.windX = 1;
    WaterSim s(p);
    std::vector<uint8_t> a, b;
    s.update(0.0f, a);
    s.update(0.37f, b);
    check(a.size() == 64 * 64 * 4, "size");
    check(a != b, "animated");
    int lo = 255, hi = 0;
    bool alpha = true;
    for (size_t i = 0; i < a.size(); i += 4) {
        lo = std::min<int>(lo, a[i + 2]);
        hi = std::max<int>(hi, a[i + 2]);
        alpha = alpha && a[i + 3] == 255 && a[i] >= 250; // blue byte ~ dz / |n| = 255
    }
    check(alpha, "alpha and z");
    check(lo < 127 && hi > 128, "slope varies around 127.5");
    return fails;
}
