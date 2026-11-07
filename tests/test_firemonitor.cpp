// Fire-rate monitor counters (src/game/firemonitor.h): team shots, combat seconds, shots per combat minute, and the silent-shooter
// rule (an enemy in sight for 10 s without a shot; short gaps of sight do not end the stretch, a longer one does).
#include "game/firemonitor.h"

#include <cmath>
#include <iostream>

using iw2::game::FireMonitor;

namespace {
int failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                             \
        }                                                                           \
    } while (0)

FireMonitor::Sample actor(int n, const char* team, bool enemy, bool visible, int starts = 0) {
    FireMonitor::Sample s;
    s.number = n;
    s.classname = "actor_test";
    s.team = team;
    s.hasEnemy = enemy;
    s.enemyVisible = visible;
    s.fireAnimStarts = starts;
    return s;
}
} // namespace

int main() {
    {
        // Counters: 20 s of combat for an axis actor, 3 s for an allied one; 5 axis shots.
        FireMonitor m;
        for (int i = 0; i < 400; ++i) m.sample(actor(1, "axis", true, false), 0.05);
        for (int i = 0; i < 60; ++i) m.sample(actor(2, "allies", true, false), 0.05);
        for (int i = 0; i < 100; ++i) m.sample(actor(3, "axis", false, false), 0.05); // no enemy: no combat time
        for (int i = 0; i < 5; ++i) m.shot(1, "axis");
        const auto& t = m.teams();
        CHECK(t.at("axis").shots == 5);
        CHECK(std::fabs(t.at("axis").combatSecs - 20.0) < 0.01);
        CHECK(std::fabs(t.at("axis").shotsPerMinute() - 15.0) < 0.01);
        CHECK(t.at("allies").shots == 0 && std::fabs(t.at("allies").combatSecs - 3.0) < 0.01);
        CHECK(t.at("allies").shotsPerMinute() == 0.0);
        CHECK(m.silent().empty());
    }
    {
        // 12 s of sight without a shot: silent; its animation starts are counted.
        FireMonitor m;
        for (int i = 0; i < 240; ++i) {
            m.tick(0.05);
            m.sample(actor(7, "allies", true, true, i / 60), 0.05);
        }
        CHECK(m.silent().size() == 1);
        CHECK(m.silent()[0].number == 7 && m.silent()[0].visibleSecs >= 11.9 && m.silent()[0].animStarts == 3);
    }
    {
        // A shot inside the stretch, 9 s of sight only, or a gap of 1 s or more all keep an actor from being flagged.
        FireMonitor m;
        for (int i = 0; i < 240; ++i) {
            if (i == 100) m.shot(1, "axis");
            m.sample(actor(1, "axis", true, true), 0.05);
        }
        for (int i = 0; i < 180; ++i) m.sample(actor(2, "axis", true, true), 0.05);
        for (int rep = 0; rep < 3; ++rep) { // 7 s of sight, 1 s without: never 10 s in a row
            for (int i = 0; i < 140; ++i) m.sample(actor(3, "axis", true, true), 0.05);
            for (int i = 0; i < 20; ++i) m.sample(actor(3, "axis", true, false), 0.05);
        }
        CHECK(m.silent().empty());
    }
    {
        // A gap under a second does not end the stretch: 6 s, 0.5 s hidden, 6 s.
        FireMonitor m;
        for (int i = 0; i < 120; ++i) m.sample(actor(4, "axis", true, true), 0.05);
        for (int i = 0; i < 10; ++i) m.sample(actor(4, "axis", true, false), 0.05);
        for (int i = 0; i < 120; ++i) m.sample(actor(4, "axis", true, true), 0.05);
        CHECK(m.silent().size() == 1);
        // A death closes the stretch; the next one starts from zero.
        m.closeActor(4);
        for (int i = 0; i < 100; ++i) m.sample(actor(4, "axis", true, true), 0.05);
        CHECK(m.silent().size() == 1);
    }
    if (failures) return 1;
    std::cout << "firemonitor OK\n";
    return 0;
}
