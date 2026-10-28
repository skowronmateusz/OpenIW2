// Compass sound pings (exe 0x4c0e20 register, 0x4c0f20 draw passes).
#include "game/compassping.h"

#include <cmath>
#include <iostream>

namespace {
int failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                             \
        }                                                                           \
    } while (0)
} // namespace

int main() {
    using iw2::game::CompassPings;
    using iw2::game::compassPingAlpha;
    // Alpha: 1 while the sound plays (end >= now), linear fade over cg_hudCompassSoundPingFadeTime, inactive afterwards.
    CHECK(compassPingAlpha(5000, 4000, 2.0f) == 1.0f);
    CHECK(compassPingAlpha(5000, 5000, 2.0f) == 1.0f);
    CHECK(std::fabs(compassPingAlpha(5000, 6000, 2.0f) - 0.5f) < 1e-6f);
    CHECK(compassPingAlpha(5000, 7000, 2.0f) == 0.0f); // end + fade == now is still active
    CHECK(compassPingAlpha(5000, 7001, 2.0f) < 0.0f);

    CompassPings p;
    // Axis shot (team 1): enemy entry with the shot position, end = now + sound length.
    p.ping(10, 1, 100.0f, -50.0f, 1000, 300);
    auto e = p.activeEnemyPings(1200, 2.0f);
    CHECK(e.size() == 1 && e[0].endMs == 1300 && e[0].pos[0] == 100.0f && e[0].pos[1] == -50.0f);
    CHECK(p.activeEnemyPings(3300, 2.0f).size() == 1);
    CHECK(p.activeEnemyPings(3301, 2.0f).empty());
    // Allied shot (team 2): only the end time, no enemy entry; neutral (3) and dead (4) leave the table alone.
    p.ping(11, 2, 5.0f, 5.0f, 1000, 200);
    CHECK(p.endMs(11) == 1200);
    p.ping(12, 3, 0, 0, 1000, 200);
    p.ping(13, 4, 0, 0, 1000, 200);
    CHECK(p.endMs(12) == 0 && p.endMs(13) == 0);
    CHECK(p.activeEnemyPings(1200, 2.0f).size() == 1);
    // Free team (0) pings set the time but are not enemies.
    p.ping(14, 0, 1, 1, 1000, 0);
    CHECK(p.endMs(14) == 1000 && p.activeEnemyPings(1200, 2.0f).size() == 1);
    // The MG42 ping of 50 ms keeps the last enemy position frozen until the next one.
    p.ping(10, 1, 300.0f, 0.0f, 4000, 50);
    e = p.activeEnemyPings(4000, 2.0f);
    CHECK(e.size() == 1 && e[0].pos[0] == 300.0f && e[0].endMs == 4050);
    // A zero-length ping (alias lookup failed: 0x4a35c0 returns 0) still fades over the fade time.
    p.ping(20, 1, 0, 0, 10000, 0);
    CHECK(std::fabs(compassPingAlpha(p.endMs(20), 11000, 2.0f) - 0.5f) < 1e-6f);
    if (failures == 0) std::cout << "compassping: ok\n";
    return failures == 0 ? 0 : 1;
}
