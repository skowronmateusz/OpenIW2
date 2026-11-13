// The own map-turret fire predicted on a network client (docs/spec/net.md section 10 "Predicted turret fire"):
// the server's shot interval for a fireTime, a press that shoots at once, the shots while attack is held, no burst
// after a release, the fire loop's end, and the server's shot events matched against the predicted ones.
#include <iostream>

#include "netgame/predictedturret.h"

namespace {

int failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                             \
        }                                                                           \
    } while (0)

using iw2::netgame::PredictedTurret;

} // namespace

int main() {
    // The server's timer drops by 50 a frame and shoots below 1: fireTime 50 every frame, 70 and 100 every second one.
    CHECK(PredictedTurret::shotIntervalMs(50) == 50);
    CHECK(PredictedTurret::shotIntervalMs(51) == 100);
    CHECK(PredictedTurret::shotIntervalMs(70) == 100);
    CHECK(PredictedTurret::shotIntervalMs(100) == 100);
    CHECK(PredictedTurret::shotIntervalMs(150) == 150);
    CHECK(PredictedTurret::shotIntervalMs(0) == 50);

    // Commands every 16 ms; attack pressed at 1000 and held for one second, fireTime 50.
    {
        PredictedTurret pt;
        int shots = 0, first = -1;
        for (int t = 900; t < 2000; t += 16) {
            const int n = pt.command(7, t, t >= 1000, 50);
            if (n && first < 0) first = t;
            shots += n;
            CHECK(n <= 1);
        }
        std::cout << "held 1 s: " << shots << " shots, the first at " << first << "\n";
        CHECK(first == 1012); // the first command with attack
        CHECK(shots >= 19 && shots <= 20);
        CHECK(pt.looping()); // the last shot is within fireTime * 3
        // Released: the loop ends fireTime * 3 after the last shot.
        int endAt = -1;
        for (int t = 2000; t < 2400 && endAt < 0; t += 16) {
            CHECK(pt.command(7, t, false, 50) == 0);
            if (!pt.looping()) endAt = t;
        }
        std::cout << "loop ends at " << endAt << "\n";
        CHECK(endAt > 2000 && endAt <= 2150);
        // A press 400 ms later: one shot at once, no catch-up of the frames in between.
        CHECK(pt.command(7, 2400, true, 50) == 1);
        CHECK(pt.command(7, 2416, true, 50) == 0);
    }

    // A command not newer than the last one is skipped; a long command gets the shots of the frames it spans.
    {
        PredictedTurret pt;
        CHECK(pt.command(3, 1000, true, 50) == 1);
        CHECK(pt.command(3, 1000, true, 50) == 0);
        CHECK(pt.command(3, 990, true, 50) == 0);
        CHECK(pt.command(3, 1150, true, 50) == 3);
    }

    // The server's shot events stand for the predicted shots first; one more is played; old predictions are given up.
    {
        PredictedTurret pt;
        CHECK(pt.command(5, 1000, true, 50) == 1);
        CHECK(pt.command(5, 1050, true, 50) == 1);
        CHECK(pt.pending() == 2);
        CHECK(!pt.serverShot(6)); // another turret
        CHECK(pt.serverShot(5));
        CHECK(pt.serverShot(5));
        CHECK(!pt.serverShot(5)); // not predicted: played
        CHECK(pt.command(5, 1100, true, 50) == 1);
        CHECK(pt.command(5, 2200, false, 50) == 0);
        CHECK(pt.pending() == 0); // a second without the server's event
        // Another turret starts afresh.
        CHECK(pt.command(9, 2300, true, 50) == 1);
        CHECK(pt.turret() == 9 && pt.pending() == 1);
    }

    std::cout << (failures ? "predictedturret: FAILED\n" : "predictedturret: ok\n");
    return failures ? 1 : 0;
}
