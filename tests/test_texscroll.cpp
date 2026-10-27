// Tread texture scroll accumulator (server 0x510870).

#include "game/vehicles.h"

#include <iostream>

int main() {
    using namespace iw2::game;
    int failures = 0;
    auto check = [&](bool c, const char* what) {
        if (!c) {
            std::cerr << "FAILED: " << what << "\n";
            ++failures;
        }
    };
    check(vehicleTexScrollStep(0, 123, 500, 0, 5.5f) == -1, "flag 0 -> -1");
    // 176 in/s * (1/176) * 5.5 * 0.05 * 1000 = 275 per think.
    int acc = 0;
    for (int i = 0; i < 4; ++i) acc = vehicleTexScrollStep(1, acc, 176.0f, 0.0f, 5.5f);
    check(acc >= 1096 && acc <= 1100, "four thinks at 176 in/s, scale 5.5");
    const int d = vehicleTexScrollStep(1, 0, 176.0f, 2.0f, 5.5f);
    check(d >= 99 && d <= 100, "dvar overrides the vehicle scale");
    check(vehicleTexScrollStep(1, 40, 0.0f, 0.0f, 5.5f) == 40, "stationary keeps the value");
    check(vehicleTexScrollOffset(-1) == 0.0f && vehicleTexScrollOffset(1500) > 1.4999f && vehicleTexScrollOffset(1500) < 1.5001f, "offset = acc / 1000, negative skipped");
    return failures;
}
